#
# Copyright (C) 2017 J.F.Dockes
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
from __future__ import print_function

import sys
import urllib
import os

audiomtypes = frozenset([
    'audio/mpeg',
    'audio/flac',
    'application/x-flac',
    'application/ogg',
    'audio/aac',
    'audio/mp4',
    'audio/x-aiff',
    'audio/x-wav',
    'inode/directory'
    ])

# Correspondance between Recoll field names (on the right), defined by
# rclaudio and the Recoll configuration 'fields' file, and what
# plgwithslave.cxx expects, which is less than consistent.
upnp2rclfields = {
    'upnp:album': 'album',
    'upnp:artist' : 'artist',
    'comment' : 'comment',
    'composer' : 'composer',
    'conductor' : 'conductor',
    'dc:date' : 'date',
    'upnp:genre' : 'genre',
    'duration' : 'duration', #should be res:
    'res:bitrate' : 'bitrate',
    'res:channels' : 'channels',
    'res:mime' : 'mtype',
    'res:samplefreq' : 'sample_rate',
    'res:size' : 'fbytes',
    'tt' : 'title',
    'upnp:originalTrackNumber' : 'tracknumber',
    }
    
def rcldoctoentry(id, pid, httphp, pathprefix, doc):
    """
    Transform a Doc objects into the format expected by the parent

    Args:
        id (str): objid for the entry
        pid (str):  objid for the browsed object (the parent container)
        httphp: the hostport part of the generated track urls
        pathprefix: is provided by our parent process (it's used to
          what plugin an url belongs too when needed for
          translating the internal into the real url (for plugins
          based on external-services)
        doc is the Doc object to be translated
        
    Returns:
        A dict representing an UPnP item, with the
        keys as expected in the plgwithslave.cxx resultToEntries() function. 

        The permanent URIs, are of the following form, based on the
        configured host:port and pathprefix arguments and track Id:
        TBD
            http://host:port/pathprefix/track?version=1&trackId=<trackid>
    
    """
    uplog("rcldoctoentry:  pid %s id %s mtype %s" %
          (pid, id, doc.mtype))
    
    li = {}
    if doc.mtype not in audiomtypes:
        return li

    li['pid'] = pid
    li['id'] = id
    if doc.mtype == 'inode/directory':
        li['tp'] = 'ct'
        li['upnp:class'] = 'object.container'
    else:
        li['tp']= 'it'
        # TBD
        li['upnp:class'] = 'object.item.audioItem.musicTrack'

    for oname,dname in upnp2rclfields.iteritems():
        val = getattr(doc, dname)
        if val:
            li[oname] = val

    if 'upnp:artist' not in li and doc.albumartist:
        li['upnp:artist'] = doc.albumartist


    # TBD Date format ?
        
    ### li['discnumber'] = str(track.disc_num)
    #comment=
    #composer=
    #conductor=
    #discnumber=
    #genre=
    #lyricist=
    #lyrics=


    try:
        val = li['upnp:originalTrackNumber']
        l = val.split('/')
        li['upnp:originalTrackNumber'] = l[0]
    except:
        pass
        
    # Compute the url. We use the URL from recoll, stripped of file://
    # and with the pathprefix prepended (the pathprefix is used by our
    # parent process to match urls to plugins)
    path = doc.getbinurl()
    path = path[7:]
    if 'tt' not in li:
        li['tt'] = os.path.basename(path.decode('UTF-8', errors = 'replace'))
    path = os.path.join(pathprefix, path)
    li['uri'] = "http://%s%s" % (httphp, urllib.quote(path))

    # The album art uri is precooked with httphp and prefix
    if doc.albumarturi:
        li['upnp:albumArtURI'] = doc.albumarturi
        #uplog("Set upnp:albumArtURI to %s" % li['upnp:albumArtURI'])

    uplog("rcldoctoentry: uri: %s" % li['uri'])
    return li

def docfolder(doc):
    path = doc.getbinurl()
    path = path[7:]
    return os.path.dirname(path)

# Find cover art for directory. We are usually called repeatedly for
# the same dir, so we cache one result
_foldercache = {}
_artnames = ('folder.jpg', 'folder.png', 'cover.jpg', 'cover.png')
def docarturi(doc, httphp, pathprefix):
    global _foldercache, _artnames

    folder = docfolder(doc)

    if folder not in _foldercache:
        _foldercache = {}
        _foldercache[folder] = None
        candidates =  [f for f in os.listdir(folder) if
                       f.lower().startswith('folder.')
                       or f.lower().startswith('cover.')]
        artnm = None
        for targ in _artnames:
            for nm in candidates:
                if nm == targ:
                    artnm = nm
                    break
                elif nm.lower() == targ:
                    artnm = nm
                    break
            if artnm:
                path = urllib.quote(os.path.join(pathprefix, folder, artnm))
                _foldercache[folder] = "http://%s%s" % (httphp, path)
                break

    return _foldercache[folder]


def cmpentries(e1, e2):
    tp1 = e1['tp']
    tp2 = e2['tp']
    isct1 = tp1 == 'ct'
    isct2 = tp2 == 'ct'

    # Containers come before items, and are sorted in alphabetic order
    if isct1 and  not isct2:
        return 1
    elif not isct1 and isct2:
        return -1
    elif isct1 and isct2:
        tt1 = e1['tt']
        tt2 = e2['tt']
        if tt1.lower() < tt2.lower():
            return -1
        elif tt1.lower() > tt2.lower():
            return 1
        else:
            return 0

    # Tracks. Sort by album then directory then track number
    k = 'upnp:album'
    a1 = e1[k] if k in e1 else ""
    a2 = e2[k] if k in e2 else ""
    if a1 < a2:
        return -1
    elif a1 > a2:
        return 1

    d1 = os.path.dirname(e1['uri'])
    d2 = os.path.dirname(e2['uri'])
    if d1 < d2:
        return -1
    elif d1 > d2:
        return 1
    
    k = 'upnp:originalTrackNumber'
    a1 = e1[k] if k in e1 else "0"
    a2 = e2[k] if k in e2 else "0"
    return int(a1) - int(a2)


def rcldirentry(id, pid, title, arturi=None, artist=None, upnpclass=None,
                searchable='1'):
    """ Create container entry in format expected by parent """
    ret = {'id':id, 'pid':pid, 'tt':title, 'tp':'ct', 'searchable':searchable}
    if arturi:
        ret['upnp:albumArtURI'] = arturi
    if artist:
        ret['upnp:artist'] = artist
    if upnpclass:
        ret['upnp:class'] = upnpclass
    else:
        ret['upnp:class'] = 'object.container'
    return ret

def uplog(s):
    print(("%s: %s" % ('uprcl', s)).encode('utf-8'), file=sys.stderr)


# Parse string into (possibly multiword) tokens
# 'a b "one phrase" c' -> [a, b, 'one phrase', c]
def stringToStrings(str):
    # States. Note that ESCAPE can only occur inside INQUOTE
    SPACE, TOKEN, INQUOTE, ESCAPE = range(4)
    
    tokens = []
    curtok = ""
    state = SPACE;

    for c in str:
        if c == '"':
            if state == SPACE:
                state = INQUOTE
            elif state == TOKEN:
                curtok += '"'
            elif state == INQUOTE:
                if curtok:
                    tokens.append(curtok);
                curtok = ""
                state = SPACE
            elif state == ESCAPE:
                curtok += '"'
                state = INQUOTE
            continue;

        elif c == '\\':
            if state == SPACE or state == TOKEN:
                curtok += '\\'
                state = TOKEN
            elif state == INQUOTE:
                state = ESCAPE
            elif state == ESCAPE:
                curtok += '\\'
                state = INQUOTE
            continue

        elif c == ' ' or c == '\t' or c == '\n' or c == '\r':
            if state == SPACE or state == TOKEN:
                if curtok:
                    tokens.append(curtok)
                curtok = ""
                state = SPACE
            elif state == INQUOTE or state == ESCAPE:
                curtok += c
            continue;

        else:
            if state == ESCAPE:
                state = INQUOTE
            elif state == SPACE:
                state = TOKEN
            elif state == TOKEN or state == INQUOTE:
                pass
            curtok += c

    if state == SPACE:
        pass
    elif state == TOKEN:
        if curtok:
            tokens.append(curtok)
    elif state == INQUOTE or state == ESCAPE:
        raise Exception("Bad string: <" + str + ">")

    return tokens
