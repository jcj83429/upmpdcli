/* Copyright (C) 2016 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "tidal.hxx"

#define LOGGER_LOCAL_LOGINC 3

#include <string>
#include <vector>
#include <sstream>
#include <string.h>
#include <librtmp/rtmp.h>
#include <upnp/upnp.h>

#include <microhttpd.h>

#include "cmdtalk.h"

#include "pathut.h"
#include "smallut.h"
#include "log.hxx"
#include "json.hpp"
#include "main.hxx"
#include "conftree.h"

using namespace std;
using namespace std::placeholders;
using json = nlohmann::json;
using namespace UPnPProvider;

class StreamHandle {
public:
    StreamHandle() : rtmp(nullptr), http_handle(nullptr), offset(0) {
    }
    ~StreamHandle() {
        clear();
    }
    void clear() {
        if (rtmp) {
	    RTMP_Close(rtmp);
	    RTMP_Free(rtmp);
	}
	if (http_handle) {
            LOGDEB("StreamHandle:~: closing http handle\n");
	    UpnpCloseHttpGet(http_handle);
            LOGDEB("StreamHandle:~: close done\n");
	}
        len = 0;
        offset = 0;
        media_url.clear();
        path.clear();
    }
    string path;
    string media_url;
    RTMP *rtmp;
    void *http_handle;
    int len;
    off_t offset;
    time_t opentime;
};

class Tidal::Internal {
public:
    Internal(Tidal *tidal, const vector<string>& pth, const string& hst,
             int prt, const string& pp)
	: plg(tidal), path(pth), host(hst), port(prt), pathprefix(pp), kbs(0),
          mhd(0) { }

    bool maybeStartCmd(const string&);
    string get_media_url(const std::string& path);

    // This also sets the kbs
    string get_mimetype(const std::string& path);
    
    int getinfo(const std::string&, VirtualDir::FileInfo*);
    void *open(const std::string&);
    int read(void *hdl, char* buf, size_t cnt);
    off_t seek(void *hdl, off_t offs, int whence);
    void close(void *hdl);

    Tidal *plg;
    CmdTalk cmd;
    vector<string> path;
    // Host and port for the URLs we generate when using the
    // libupnp server (through vdir)
    string host;
    int port;
    // path prefix (this is used to redirect gets to us).
    string pathprefix;
    // mimetype is a constant for a given session, depend on quality
    // choice only. Initialized once
    string mimetype;

    // kilobits/s
    int kbs;
    
    // Cached uri translation and stream: set in getinfo() and reused
    // in open() (used with miniserver/vdir)
    StreamHandle laststream;
    // When using microhttpd
    struct MHD_Daemon *mhd;
};

static int answer_to_connection(void *cls, struct MHD_Connection *connection, 
                                const char *url, 
                                const char *method, const char *version, 
                                const char *upload_data, 
                                size_t *upload_data_size, void **con_cls)
{
    LOGDEB("answer_to_connection: url " << url << " method " << method << 
           " version " << version << endl);
    Tidal::Internal *me = (Tidal::Internal*)cls;
    
    static int aptr;
    if (&aptr != *con_cls) {
        /* do not respond on first call */
        *con_cls = &aptr;
        return MHD_YES;
    }

    string path(url);

    const char* stid =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND,
                                    "trackId");
    if (!stid || !*stid) {
        LOGERR("answer_to_connection: no trackId in args\n");
    }
    path += string("?version=1&trackId=") + stid;

    string media_url = me->get_media_url(path);
    if (media_url.empty()) {
        LOGERR("answer_to_connection: media_uri not found for URL: " <<
               url << endl);
        return MHD_NO;
    }
    static char data[] = "<html><body><p>Error!</p></body></html>";
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(data), data,
                                        MHD_RESPMEM_PERSISTENT);
    if (response == NULL) {
        LOGERR("answer_to_connection: could not create response" << endl);
        return MHD_NO;
    }

    LOGDEB("Tidal: redirecting to " << media_url << endl);
    MHD_add_response_header (response, "Location", media_url.c_str());

    int ret = MHD_queue_response(connection, 302, response);
    MHD_destroy_response(response);
    return ret;
}

static int accept_policy(void *, const struct sockaddr* sa,
                         socklen_t addrlen)
{
    return MHD_YES;
}

bool Tidal::Internal::maybeStartCmd(const string& who)
{
    LOGDEB1("Tidal::maybeStartCmd for: " << who << endl);
    
    if (cmd.running()) {
        return true;
    }

    ConfSimple *conf = plg->m_services->getconfig(plg);
    string tidalquality;
    if (!conf || !conf->get("tidalquality", tidalquality)) {
        LOGERR("Tidal: can't get parameter 'tidalquality' from config\n");
        return false;
    }

    bool using_miniserver = tidalquality.compare("lossless");
    
    if (using_miniserver) {
        VirtualDir::FileOps ops;
        ops.getinfo = bind(&Tidal::Internal::getinfo, this, _1, _2);
        ops.open = bind(&Tidal::Internal::open, this, _1);
        ops.read = bind(&Tidal::Internal::read, this, _1, _2, _3);
        ops.seek = bind(&Tidal::Internal::seek, this, _1, _2, _3);
        ops.close = bind(&Tidal::Internal::close, this, _1);
        plg->m_services->setfileops(plg, plg->m_services->getpathprefix(plg),
                                    ops);
    } else {
        port = 49149;
        string sport;
        if (conf->get("tidalmicrohttpport", sport)) {
            port = atoi(sport.c_str());
        }
        mhd = MHD_start_daemon(
            MHD_USE_THREAD_PER_CONNECTION,
            //MHD_USE_SELECT_INTERNALLY, 
            port, 
            /* Accept policy callback and arg */
            accept_policy, NULL, 
            /* handler and arg */
            &answer_to_connection, this, 
            MHD_OPTION_END);
        if (nullptr == mhd) {
            LOGERR("Tidal: MHD_start_daemon failed\n");
            return false;
        }
    }
        
    string pythonpath = string("PYTHONPATH=") +
        path_cat(g_datadir, "cdplugins/pycommon");
    string configname = string("UPMPD_CONFIG=") + g_configfilename;
    stringstream ss;
    ss << host << ":" << port;
    string hostport = string("UPMPD_HTTPHOSTPORT=") + ss.str();
    string pp = string("UPMPD_PATHPREFIX=") + pathprefix;
    if (!cmd.startCmd("tidal.py", {/*args*/},
                      /* env */ {pythonpath, configname, hostport, pp},
                      /* exec path */ path)) {
        LOGERR("Tidal::maybeStartCmd: " << who << " startCmd failed\n");
        return false;
    }
    return true;
}

string Tidal::Internal::get_mimetype(const std::string& path)
{
    if (!maybeStartCmd("get_mimetype")) {
	return string();
    }
    if (mimetype.empty()) {
	unordered_map<string, string> res;
	if (!cmd.callproc("mimetype", {{"path", path}}, res)) {
	    LOGERR("Tidal::get_mimetype: slave failure\n");
	    return string();
	}

	auto it = res.find("mimetype");
	if (it == res.end()) {
	    LOGERR("Tidal::get_mimetype: no mimetype in result\n");
	    return string();
	}
	mimetype = it->second;
	it = res.find("kbs");
	if (it != res.end()) {
            kbs = atoi(it->second.c_str());
	}
	LOGDEB("Tidal: got mimetype [" << mimetype << "]\n");
    }
    return mimetype;
}

string Tidal::Internal::get_media_url(const std::string& path)
{
    if (!maybeStartCmd("get_media_url")) {
	return string();
    }
    time_t now = time(0);
    if (laststream.path.compare(path) || (now - laststream.opentime > 10)) {
	unordered_map<string, string> res;
	if (!cmd.callproc("trackuri", {{"path", path}}, res)) {
	    LOGERR("Tidal::get_media_url: slave failure\n");
	    return string();
	}

	auto it = res.find("media_url");
	if (it == res.end()) {
	    LOGERR("Tidal::get_media_url: no media url in result\n");
	    return string();
	}
        laststream.clear();
        laststream.path = path;
        laststream.media_url = it->second;
        laststream.opentime = now;
    }

    LOGDEB("Tidal: got media url [" << laststream.media_url << "]\n");
    return laststream.media_url;
}

//////////////////////////////////
// Note that the following vdir functions are currently actually not
// used when dealing with an http url (lossless flac data), because we
// now just use a redirect to the media url instead of gatewaying the
// data in this case. Code kept around just in case.

int Tidal::Internal::getinfo(const std::string& path, VirtualDir::FileInfo *inf)
{
    LOGDEB("Tidal::getinfo: " << path << endl);

    laststream.clear();
    string media_url = get_media_url(path);
    if (media_url.empty()) {
	return -1;
    }
    inf->file_length = -1;
    inf->last_modified = 0;
    inf->mime = get_mimetype(path);

    if (media_url.find("http") == 0) {
	char *content_type;
	int httpstatus;
	int code = UpnpOpenHttpGet(media_url.c_str(), &laststream.http_handle,
				     &content_type, &laststream.len,
				     &httpstatus, 30);
	LOGDEB("Tidal::getinfo: UpnpOpenHttpGet: ret " << code <<
	       " mtype " << content_type << " length " << laststream.len <<
	       " HTTP status " << httpstatus << endl);
	if (code) {
	    LOGERR("Tidal::getinfo: UpnpOpenHttpGet: ret " << code <<
		   " mtype " << content_type << " length " << laststream.len <<
		   " HTTP status " << httpstatus << endl);
	} else {
	    inf->file_length = laststream.len;
	    LOGDEB("Tidal:getinfo: got file length "<< inf->file_length <<endl);
	}
    }

    LOGDEB("Tidal::getinfo: returning\n");
    return 0;
}

void *Tidal::Internal::open(const string& path)
{
    LOGDEB("Tidal::open: " << path << endl);
    string media_url = get_media_url(path);
    if (media_url.empty()) {
	return nullptr;
    }
    if (media_url.find("http") == 0) {
        if (laststream.http_handle == nullptr) {
            char *content_type;
            int httpstatus;
            int code = UpnpOpenHttpGet(media_url.c_str(),
                                       &laststream.http_handle,
                                       &content_type, &laststream.len,
                                       &httpstatus, 30);
            LOGDEB("Tidal::open: UpnpOpenHttpGet: ret " << code <<
                   " mtype " << content_type << " length " <<
                   laststream.len <<
                   " HTTP status " << httpstatus << endl);
            if (code) {
                LOGERR("Tidal::open: UpnpOpenHttpGet: ret " << code <<
                       " mtype " << content_type << " length " <<
                       laststream.len <<
                       " HTTP status " << httpstatus << endl);
                return nullptr;
            }
        } 
        // Doc says to free content_type, but it causes malloc issues
	//free(content_type);
	StreamHandle *hdl = new StreamHandle;
	hdl->http_handle = laststream.http_handle;
	hdl->len = laststream.len;
        hdl->media_url = media_url;
        laststream.http_handle = nullptr;
        laststream.clear();
	return hdl;
    } else {
	RTMP *rtmp = RTMP_Alloc();
	RTMP_Init(rtmp);

	// Writable copy of url
	if (!RTMP_SetupURL(rtmp, strdup(media_url.c_str()))) {
	    LOGERR("Tidal::open: RTMP_SetupURL failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	if (!RTMP_Connect(rtmp, NULL)) {
	    LOGERR("Tidal::open: RTMP_Connect failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	if (!RTMP_ConnectStream(rtmp, 0)) {
	    LOGERR("Tidal::open: RTMP_ConnectStream failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	StreamHandle *hdl = new StreamHandle;
	hdl->rtmp = rtmp;
	return hdl;
    }
}

int Tidal::Internal::read(void *_hdl, char* buf, size_t cnt)
{
    LOGDEB("Tidal::read: " << cnt << endl);
    if (!_hdl)
	return -1;

    // The pupnp http code has a default 1MB buffer size which is much
    // too big for us (too slow, esp. because tidal will stall).
    const int mybsize = 200 * 1024;
    if (cnt > mybsize)
        cnt = mybsize;

    StreamHandle *hdl = (StreamHandle *)_hdl;

    if (hdl->rtmp) {
	RTMP *rtmp = hdl->rtmp;
	size_t totread = 0;
	while (totread < cnt) {
	    int didread = RTMP_Read(rtmp, buf+totread, cnt-totread);
	    //LOGDEB("Tidal::read: RTMP_Read returned: " << didread << endl);
	    if (didread <= 0)
		break;
	    totread += didread;
	}
	LOGDEB("Tidal::read: total read: " << totread << endl);
        hdl->offset += totread;
	return totread > 0 ? totread : -1;
    } else if (hdl->http_handle) {
	int code = UpnpReadHttpGet(hdl->http_handle, buf, &cnt, 30);
	if (code) {
	    LOGERR("Tidal::read: UpnpReadHttpGet returned " << code << endl);
	    return -1;
	}
        hdl->offset += cnt;
	return int(cnt);
    } else {
	LOGERR("Tidal::read: neither rtmp nor http\n");
	return -1;
    }
}

off_t Tidal::Internal::seek(void *_hdl, off_t offs, int whence)
{
    StreamHandle *hdl = (StreamHandle *)_hdl;
    LOGDEB("Tidal::seek: offs "<< offs << " whence " << whence << " current " <<
           hdl->offset << endl);
    if (whence == 0) {
        hdl->offset = offs;
    } else if (whence == 1) {
        hdl->offset += offs;
    } else if (whence == 2) {
        hdl->offset = hdl->len + offs;
    }
    if (hdl->http_handle) {
        UpnpCloseHttpGet(hdl->http_handle);
	char *content_type;
	int content_length;
	int httpstatus;
	int code = UpnpOpenHttpGetEx(hdl->media_url.c_str(), &hdl->http_handle,
				     &content_type, &content_length,
				     &httpstatus, hdl->offset, 2000000000, 30);
        LOGERR("Tidal::seek to " << hdl->offset <<
               " UpnpOpenHttpGetEx: ret " << code <<
               " mtype " << content_type << " length " << content_length <<
               " HTTP status " << httpstatus << endl);
	if (code) {
	    LOGERR("Tidal::seek to " << hdl->offset <<
                   " UpnpOpenHttpGetEx: ret " << code <<
		   " mtype " << content_type << " length " << content_length <<
		   " HTTP status " << httpstatus << endl);
            return -1;
	} 
    } else if (hdl->rtmp && (kbs > 0)) {
        // Convert offset to mS:
        long long timestamp = 1000 * (hdl->offset / (kbs/8));
        LOGDEB("Tidal::seek: seeking rtmp stream to " << timestamp/1000 <<
               "S\n");
        if (!RTMP_SendSeek(hdl->rtmp, timestamp)) {
            return -1;
        }
    }
    
    return 0;
}

void Tidal::Internal::close(void *_hdl)
{
    LOGDEB("Tidal::close\n");
    StreamHandle *hdl = (StreamHandle *)_hdl;
    delete hdl;
}



Tidal::Tidal(const std::string& name, CDPluginServices *services)
    : CDPlugin(name, services)
{
    m = new Internal(this, services->getexecpath(this),
                     services->getupnpaddr(this),
                     services->getupnpport(this),
                     services->getpathprefix(this));
}

Tidal::~Tidal()
{
    delete m;
}

static int resultToEntries(const string& encoded, int stidx, int cnt,
			   std::vector<UpSong>& entries)
{
    auto decoded = json::parse(encoded);
    LOGDEB("Tidal::results: got " << decoded.size() << " entries\n");
    LOGDEB1("Tidal::results: undecoded json: " << decoded.dump() << endl);

    for (unsigned int i = stidx; i < decoded.size(); i++) {
	if (--cnt < 0) {
	    break;
	}
	UpSong song;
	// tp is container ("ct") or item ("it")
	auto it1 = decoded[i].find("tp");
	if (it1 == decoded[i].end()) {
	    LOGERR("Tidal::browse: no type in entry\n");
	    continue;
	}
	string stp = it1.value();
	
#define JSONTOUPS(fld, nm)						\
	it1 = decoded[i].find(#nm);					\
	if (it1 != decoded[i].end()) {					\
	    /*LOGDEB("song." #fld " = " << it1.value() << endl);*/	\
	    song.fld = it1.value();					\
	}
	
	if (!stp.compare("ct")) {
	    song.iscontainer = true;
	} else	if (!stp.compare("it")) {
	    song.iscontainer = false;
	    JSONTOUPS(uri, uri);
	    JSONTOUPS(artist, dc:creator);
	    JSONTOUPS(artist, upnp:artist);
	    JSONTOUPS(genre, upnp:genre);
	    JSONTOUPS(tracknum, upnp:originalTrackNumber);
	    JSONTOUPS(artUri, upnp:albumArtURI);
	    JSONTOUPS(duration_secs, duration);
	} else {
	    LOGERR("Tidal::browse: bad type in entry: " << it1.value() << endl);
	    continue;
	}
	JSONTOUPS(id, id);
	JSONTOUPS(parentid, pid);
	JSONTOUPS(title, tt);
	entries.push_back(song);
    }
    // We return the total match size, the count of actually returned
    // entries can be obtained from the vector
    return decoded.size();
}

int Tidal::browse(const std::string& objid, int stidx, int cnt,
		  std::vector<UpSong>& entries,
		  const std::vector<std::string>& sortcrits,
		  BrowseFlag flg)
{
    LOGDEB("Tidal::browse\n");
    if (!m->maybeStartCmd("browse")) {
	return -1;
    }
    unordered_map<string, string> res;
    if (!m->cmd.callproc("browse", {{"objid", objid}}, res)) {
	LOGERR("Tidal::browse: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("Tidal::browse: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}


int Tidal::search(const string& ctid, int stidx, int cnt,
		  const string& searchstr,
		  vector<UpSong>& entries,
		  const vector<string>& sortcrits)
{
    LOGDEB("Tidal::search\n");
    if (!m->maybeStartCmd("search")) {
	return -1;
    }

    // We only accept field xx value as search criteria
    vector<string> vs;
    stringToStrings(searchstr, vs);
    LOGDEB("Tidal::search:search string split->" << vs.size() << " pieces\n");
    if (vs.size() != 3) {
	LOGERR("Tidal::search: bad search string: [" << searchstr << "]\n");
	return -1;
    }
    const string& upnpproperty = vs[0];
    string tidalfield;
    if (!upnpproperty.compare("upnp:artist") ||
	!upnpproperty.compare("dc:author")) {
	tidalfield = "artist";
    } else if (!upnpproperty.compare("upnp:album")) {
	tidalfield = "album";
    } else if (!upnpproperty.compare("dc:title")) {
	tidalfield = "track";
    } else {
	LOGERR("Tidal::search: bad property: [" << upnpproperty << "]\n");
	return -1;
    }
	
    unordered_map<string, string> res;
    if (!m->cmd.callproc("search", {
		{"objid", ctid},
		{"field", tidalfield},
		{"value", vs[2]} },  res)) {
	LOGERR("Tidal::search: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("Tidal::search: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}
