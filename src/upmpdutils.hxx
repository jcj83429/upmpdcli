/* Copyright (C) 2014 J.F.Dockes
 *	 This program is free software; you can redistribute it and/or modify
 *	 it under the terms of the GNU General Public License as published by
 *	 the Free Software Foundation; either version 2 of the License, or
 *	 (at your option) any later version.
 *
 *	 This program is distributed in the hope that it will be useful,
 *	 but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	 GNU General Public License for more details.
 *
 *	 You should have received a copy of the GNU General Public License
 *	 along with this program; if not, write to the
 *	 Free Software Foundation, Inc.,
 *	 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#ifndef _UPMPDUTILS_H_X_INCLUDED_
#define _UPMPDUTILS_H_X_INCLUDED_

#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace UPnPClient {
    class UPnPDirObject;
};

// This was originally purely a translation of data from mpd. Extended
// to general purpose track/container descriptor
class UpSong {
public:
    UpSong()
	: duration_secs(0), mpdid(0),
	  iscontainer(false), searchable(false) {
    }
    void clear() {
	*this = UpSong();
    }
    std::string id;
    std::string parentid;
    std::string uri;
    std::string name; // only set for radios apparently. 
    std::string artist; // Reused as annot for containers
    std::string album;
    std::string title;
    std::string tracknum;
    std::string genre;
    std::string artUri;

    unsigned int duration_secs;
    int mpdid;
    bool iscontainer;
    bool searchable;

    std::string dump() {
        return std::string("Uri [") + uri + "] Artist [" + artist + "] Album ["
            +  album + "] Title [" + title + "] Tno [" + tracknum + "]";
    }
    // Format to DIDL fragment 
    std::string didl();

    static UpSong container(const std::string& i, const std::string& p,
			    const std::string& t, bool s = true,
			    const std::string& annot = std::string()) {
	UpSong song;
	song.iscontainer = true;
	song.id = i; song.parentid = p; song.title = t; song.searchable = s;
	song.artist = annot;
	return song;
    }
};

// Convert between db value to percent values (Get/Set Volume and VolumeDb)
extern int percentodbvalue(int value);
extern int dbvaluetopercent(int dbvalue);

extern bool sleepms(int ms);

// Return mapvalue or null strings, for maps where absent entry and
// null data are equivalent
extern const std::string& mapget(
    const std::unordered_map<std::string, std::string>& im, 
    const std::string& k);

// Format a didl fragment from MPD status data. Used by the renderer
extern std::string didlmake(const UpSong& song);

// Wrap DIDL entries in header / trailer
extern const std::string& headDIDL();
extern const std::string& tailDIDL();
extern std::string wrapDIDL(const std::string& data);

// Convert UPnP metadata to UpSong for mpdcli to use
extern bool uMetaToUpSong(const std::string&, UpSong *ups);
// Convert UPnP content directory entry object to UpSong
bool dirObjToUpSong(const UPnPClient::UPnPDirObject& dobj, UpSong *ups);

// Replace the first occurrence of regexp. cxx11 regex does not work
// that well yet...
extern std::string regsub1(const std::string& sexp, const std::string& input, 
                           const std::string& repl);

// Return map with "newer" elements which differ from "old".  Old may
// have fewer elts than "newer" (e.g. may be empty), we use the
// "newer" entries for diffing. This is used to compute state changes.
extern std::unordered_map<std::string, std::string> 
diffmaps(const std::unordered_map<std::string, std::string>& old,
         const std::unordered_map<std::string, std::string>& newer);

#define UPMPD_UNUSED(X) (void)(X)

#endif /* _UPMPDUTILS_H_X_INCLUDED_ */
