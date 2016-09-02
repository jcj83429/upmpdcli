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

#include "plgwithslave.hxx"

#define LOGGER_LOCAL_LOGINC 3

#include <fcntl.h>

#include <string>
#include <vector>
#include <sstream>
#include <string.h>
#include <upnp/upnp.h>
#include <microhttpd.h>
extern "C" {
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
}


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
    StreamHandle(PlgWithSlave::Internal *plg) {
    }
    ~StreamHandle() {
        clear();
    }
    void clear() {
        plg = 0;
        path.clear();
        media_url.clear();
        len = 0;
        opentime = 0;
    }
    
    PlgWithSlave::Internal *plg;
    AVIOContext *avio;
    string path;
    string media_url;
    long long len;
    time_t opentime;
};

class PlgWithSlave::Internal {
public:
    Internal(PlgWithSlave *_plg, const string& exe, const string& hst,
             int prt, const string& pp)
	: plg(_plg), exepath(exe), upnphost(hst), upnpport(prt), pathprefix(pp), 
          laststream(this) {
    }

    bool maybeStartCmd();

    PlgWithSlave *plg;
    CmdTalk cmd;
    string exepath;
    // Upnp Host and port. This would only be used to generate URLsif
    // we were using the libupnp miniserver. We currently use
    // microhttp because it can do redirects
    string upnphost;
    int upnpport;
    // path prefix (this is used by upmpdcli that gets it for us).
    string pathprefix;
    
    // Cached uri translation
    StreamHandle laststream;
};

// microhttpd daemon handle. There is only one of these, and one port, we find
// the right plugin by looking at the url path.
static struct MHD_Daemon *mhd;

// Microhttpd connection handler. We re-build the complete url + query
// string (&trackid=value), use this to retrieve a Tidal URL, and
// redirect to it (HTTP). A previous version handled rtmp streams, and
// had to read them. Look up the history if you need the code again
// (the apparition of RTMP streams was apparently linked to the use of
// a different API key).
static int answer_to_connection(void *cls, struct MHD_Connection *connection, 
                                const char *url, 
                                const char *method, const char *version, 
                                const char *upload_data, 
                                size_t *upload_data_size, void **con_cls)
{
    static int aptr;
    if (&aptr != *con_cls) {
        /* do not respond on first call */
        *con_cls = &aptr;
        return MHD_YES;
    }

    LOGDEB("answer_to_connection: url " << url << " method " << method << 
           " version " << version << endl);

    // The 'plgi' here is just whatever plugin started up the httpd task
    // We just use it to find the appropriate plugin for this path,
    // and then dispatch the request.
    PlgWithSlave::Internal *plgi = (PlgWithSlave::Internal*)cls;
    PlgWithSlave *realplg =
      dynamic_cast<PlgWithSlave*>(plgi->plg->m_services->getpluginforpath(url));
    if (nullptr == realplg) {
        LOGERR("answer_to_connection: no plugin for path [" << url << endl);
        return MHD_NO;
    }

    // We may need one day to subclass PlgWithSlave to implement a
    // plugin-specific method. For now, existing plugins have
    // compatible python code, and we can keep one c++ method.
    // get_media_url() would also need changing because it accesses Internal:
    // either make it generic or move to subclass.
    //return realplg->answer_to_connection(connection, url, method, version,
    //                               upload_data, upload_data_size, con_cls);

    // Rebuild URL + query
    string path(url);
    const char* stid =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND,
                                    "trackId");
    if (!stid || !*stid) {
        LOGERR("answer_to_connection: no trackId in args\n");
        return MHD_NO;
    }
    path += string("?version=1&trackId=") + stid;

    // Translate to Tidal/Qobuz etc real temporary URL
    string media_url = realplg->get_media_url(path);
    if (media_url.empty()) {
        LOGERR("answer_to_connection: no media_uri for: " << url << endl);
        return MHD_NO;
    }

    if (media_url.find("http") == 0) {
        LOGDEB("answer_to_connection: redirecting to " << media_url << endl);

        static char data[] = "<html><body></body></html>";
        struct MHD_Response *response =
            MHD_create_response_from_buffer(strlen(data), data,
                                            MHD_RESPMEM_PERSISTENT);
        if (response == NULL) {
            LOGERR("answer_to_connection: could not create response" << endl);
            return MHD_NO;
        }
        MHD_add_response_header (response, "Location", media_url.c_str());
        int ret = MHD_queue_response(connection, 302, response);
        MHD_destroy_response(response);
        return ret;
    } else {
        LOGERR("PlgWithSlave: got non-http URL !: " << media_url << endl);
        LOGERR("PlgWithSlave:   the code for handling these is gone !\n");
        LOGERR("    will have to fetch it from git history\n");
        return MHD_NO;
    } 
}

static int accept_policy(void *, const struct sockaddr* sa, socklen_t addrlen)
{
    return MHD_YES;
}

// Called once for starting the Python program and do other initialization.
bool PlgWithSlave::Internal::maybeStartCmd()
{
    if (cmd.running()) {
        return true;
    }

    ConfSimple *conf = plg->m_services->getconfig(plg);
    int port = 49149;
    string sport;
    if (conf->get("plgmicrohttpport", sport)) {
        port = atoi(sport.c_str());
    }
    if (nullptr == mhd) {

        // Start the microhttpd daemon. There can be only one, and it
        // is started with a context handle which points to whatever
        // plugin got there first. The callback will only use the
        // handle to get to the plugin services, and retrieve the
        // appropriate plugin based on the url path prefix.
        LOGDEB("PlgWithSlave: starting httpd on port "<< port << endl);
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
            LOGERR("PlgWithSlave: MHD_start_daemon failed\n");
            return false;
        }
    }
    
    string pythonpath = string("PYTHONPATH=") +
        path_cat(g_datadir, "cdplugins") + ":" +
        path_cat(g_datadir, "cdplugins/pycommon") + ":" +
        path_cat(g_datadir, "cdplugins/" + plg->m_name);
    string configname = string("UPMPD_CONFIG=") + g_configfilename;
    stringstream ss;
    ss << upnphost << ":" << port;
    string hostport = string("UPMPD_HTTPHOSTPORT=") + ss.str();
    string pp = string("UPMPD_PATHPREFIX=") + pathprefix;
    if (!cmd.startCmd(exepath, {/*args*/},
                      /* env */ {pythonpath, configname, hostport, pp})) {
        LOGERR("PlgWithSlave::maybeStartCmd: startCmd failed\n");
        return false;
    }
    return true;
}

// Translate the slave-generated HTTP URL (based on the trackid), to
// an actual temporary service (e.g. tidal one), which will be an HTTP
// URL pointing to either an AAC or a FLAC stream.
// Older versions of this module handled AAC FLV transported over
// RTMP, apparently because of the use of a different API key. Look up
// the git history if you need this again.
// The Python code calls the service to translate the trackid to a temp
// URL. We cache the result for a few seconds to avoid multiple calls
// to tidal.
string PlgWithSlave::get_media_url(const std::string& path)
{
    LOGDEB("PlgWithSlave::get_media_url: " << path << endl);
    if (!m->maybeStartCmd()) {
	return string();
    }
    time_t now = time(0);
    if (m->laststream.path.compare(path) ||
        (now - m->laststream.opentime > 10)) {
	unordered_map<string, string> res;
	if (!m->cmd.callproc("trackuri", {{"path", path}}, res)) {
	    LOGERR("PlgWithSlave::get_media_url: slave failure\n");
	    return string();
	}

	auto it = res.find("media_url");
	if (it == res.end()) {
	    LOGERR("PlgWithSlave::get_media_url: no media url in result\n");
	    return string();
	}
        m->laststream.clear();
        m->laststream.path = path;
        m->laststream.media_url = it->second;
        m->laststream.opentime = now;
    }

    LOGDEB("PlgWithSlave: got media url [" << m->laststream.media_url << "]\n");
    return m->laststream.media_url;
}


PlgWithSlave::PlgWithSlave(const std::string& name, CDPluginServices *services)
    : CDPlugin(name, services)
{
    m = new Internal(this, services->getexecpath(this),
                     services->getupnpaddr(this),
                     services->getupnpport(this),
                     services->getpathprefix(this));
}

PlgWithSlave::~PlgWithSlave()
{
    delete m;
}

static int resultToEntries(const string& encoded, int stidx, int cnt,
			   std::vector<UpSong>& entries)
{
    auto decoded = json::parse(encoded);
    LOGDEB("PlgWithSlave::results: got " << decoded.size() << " entries\n");
    LOGDEB1("PlgWithSlave::results: undecoded json: " << decoded.dump() << endl);

    for (unsigned int i = stidx; i < decoded.size(); i++) {
	if (--cnt < 0) {
	    break;
	}
	UpSong song;
	// tp is container ("ct") or item ("it")
	auto it1 = decoded[i].find("tp");
	if (it1 == decoded[i].end()) {
	    LOGERR("PlgWithSlave::browse: no type in entry\n");
	    continue;
	}
	string stp = it1.value();
	
#define JSONTOUPS(fld, nm)						\
	it1 = decoded[i].find(#nm);					\
	if (it1 != decoded[i].end()) {					\
            if (it1.value() != nullptr)                                 \
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
	    LOGERR("PlgWithSlave::browse: bad type in entry: " << it1.value() << endl);
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

int PlgWithSlave::browse(const std::string& objid, int stidx, int cnt,
		  std::vector<UpSong>& entries,
		  const std::vector<std::string>& sortcrits,
		  BrowseFlag flg)
{
    LOGDEB("PlgWithSlave::browse\n");
    if (!m->maybeStartCmd()) {
	return -1;
    }
    string sbflg;
    switch (flg) {
    case CDPlugin::BFMeta:
        sbflg = "meta";
        break;
    case CDPlugin::BFChildren:
    default:
        sbflg = "children";
        break;
    }

    unordered_map<string, string> res;
    if (!m->cmd.callproc("browse", {{"objid", objid}, {"flag", sbflg}}, res)) {
	LOGERR("PlgWithSlave::browse: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("PlgWithSlave::browse: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}


int PlgWithSlave::search(const string& ctid, int stidx, int cnt,
		  const string& searchstr,
		  vector<UpSong>& entries,
		  const vector<string>& sortcrits)
{
    LOGDEB("PlgWithSlave::search\n");
    if (!m->maybeStartCmd()) {
	return -1;
    }

    // We only accept field xx value as search criteria
    vector<string> vs;
    stringToStrings(searchstr, vs);
    if (vs.size() != 3) {
	LOGERR("PlgWithSlave::search: bad search string: [" << searchstr <<
               "]\n");
	return -1;
    }
    const string& upnpproperty = vs[0];
    string slavefield;
    if (!upnpproperty.compare("upnp:artist") ||
	!upnpproperty.compare("dc:author")) {
	slavefield = "artist";
    } else if (!upnpproperty.compare("upnp:album")) {
	slavefield = "album";
    } else if (!upnpproperty.compare("dc:title")) {
	slavefield = "track";
    } else {
	LOGERR("PlgWithSlave::search: bad property: [" << upnpproperty << "]\n");
	return -1;
    }
	
    unordered_map<string, string> res;
    if (!m->cmd.callproc("search", {
		{"objid", ctid},
		{"field", slavefield},
		{"value", vs[2]} },  res)) {
	LOGERR("PlgWithSlave::search: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("PlgWithSlave::search: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}