#!/usr/bin/env python
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

import sys
import os
import json
import posixpath
import re
import conftree
import cmdtalkplugin
import urllib
import threading
import subprocess

import uprclfolders
import uprcltags
import uprcluntagged
import uprclsearch
import uprclhttp
from uprclutils import *

from recoll import recoll
from recoll import rclconfig

# This must be consistent with what contentdirectory.cxx does
g_myprefix = '0$uprcl$'

# The recoll documents
g_rcldocs = []

# Func name to method mapper
dispatcher = cmdtalkplugin.Dispatch()
# Pipe message handler
msgproc = cmdtalkplugin.Processor(dispatcher)

def uprcl_init():
    global httphp, pathprefix, pathmap, rclconfdir, g_rcldocs
    
    if "UPMPD_PATHPREFIX" not in os.environ:
        raise Exception("No UPMPD_PATHPREFIX in environment")
    pathprefix = os.environ["UPMPD_PATHPREFIX"]
    if "UPMPD_CONFIG" not in os.environ:
        raise Exception("No UPMPD_CONFIG in environment")
    upconfig = conftree.ConfSimple(os.environ["UPMPD_CONFIG"])

    httphp = upconfig.get("uprclhostport")
    if httphp is None:
        raise Exception("uprclhostport not in config")

    pthstr = upconfig.get("uprclpaths")
    if pthstr is None:
        raise Exception("uprclpaths not in config")
    lpth = pthstr.split(',')
    pathmap = {}
    for ptt in lpth:
        l = ptt.split(':')
        pathmap[l[0]] = l[1]

    global rclconfdir
    rclconfdir = upconfig.get("uprclconfdir")
    if rclconfdir is None:
        raise Exception("uprclconfdir not in config")

    g_rcldocs = uprclfolders.inittree(rclconfdir, httphp, pathprefix)
    uprcltags.recolltosql(g_rcldocs)
    uprcluntagged.recoll2untagged(g_rcldocs)

    host,port = httphp.split(':')
    if False:
        # Running the server as a thread. We get into trouble because
        # something somewhere writes to stdout a bunch of --------.
        # Could not find where they come from, happens after a sigpipe
        # when a client closes a stream. The --- seem to happen before
        # and after the exception strack trace, e.g:
        # ----------------------------------------
        #   Exception happened during processing of request from ('192...
        #   Traceback...
        #   [...]
        # error: [Errno 32] Broken pipe
        # ----------------------------------------
        httpthread = threading.Thread(target=uprclhttp.runHttp,
                                      kwargs = {'host':host ,
                                                'port':int(port),
                                                'pthstr':pthstr,
                                                'pathprefix':pathprefix})
        httpthread.daemon = True 
        httpthread.start()
    else:
        # Running the HTTP server as a separate process
        cmdpath = os.path.join(os.path.dirname(sys.argv[0]), 'uprclhttp.py')
        cmd = subprocess.Popen((cmdpath, host, port, pthstr, pathprefix),
                               stdin = open('/dev/null'),
                               stdout = sys.stderr,
                               stderr = sys.stderr,
                               close_fds = True)
            
@dispatcher.record('trackuri')
def trackuri(a):
    # This is used for plugins which generate temporary local urls
    # pointing to the microhttpd instance. The microhttpd
    # answer_to_connection() routine in plgwithslave calls 'trackuri'
    # to get a real URI to redirect to. We generate URIs which
    # directly point to our python http server, so this method should
    # never be called.
    msgproc.log("trackuri: [%s]" % a)
    raise Exception("trackuri: should not be called for uprcl!")

# objid prefix to module map
rootmap = {}

def _rootentries():
    # Build up root directory. This is our top internal structure. We
    # let the different modules return their stuff, and we take note
    # of the objid prefixes for later dispatching
    entries = []

    nents = uprcltags.rootentries(g_myprefix)
    for e in nents:
        rootmap[e['id']] = 'tags'
    entries += nents

    nents = uprcluntagged.rootentries(g_myprefix)
    for e in nents:
        rootmap[e['id']] = 'untagged'
    entries += nents

    nents = uprclfolders.rootentries(g_myprefix)
    for e in nents:
        rootmap[e['id']] = 'folders'
    entries += nents

    uplog("Browse root: rootmap now %s" % rootmap)
    return entries

def _browsedispatch(objid, bflg, httphp, pathprefix):
    for id,mod in rootmap.iteritems():
        uplog("Testing %s against %s" % (objid, id))
        if objid.startswith(id):
            if mod == 'folders':
                return uprclfolders.browse(objid, bflg, httphp, pathprefix)
            elif mod == 'tags':
                return uprcltags.browse(objid, bflg, httphp, pathprefix)
            elif mod == 'untagged':
                return uprcluntagged.browse(objid, bflg, httphp, pathprefix)
            else:
                raise Exception("Browse: dispatch: bad mod " + mod)
    raise Exception("Browse: dispatch: bad objid not in rootmap: " + objid)

@dispatcher.record('browse')
def browse(a):
    msgproc.log("browse: %s" % a)
    if 'objid' not in a:
        raise Exception("No objid in args")
    objid = a['objid']
    bflg = a['flag'] if 'flag' in a else 'children'
    
    if not objid.startswith(g_myprefix):
        raise Exception("bad objid <%s>" % objid)

    idpath = objid.replace(g_myprefix, '', 1)
    msgproc.log("browse: idpath: <%s>" % idpath)

    entries = []

    if bflg == 'meta':
        raise Exception("uprcl-app: browse: can't browse meta for now")
    else:
        if not idpath:
            entries = _rootentries()
        else:
            entries = _browsedispatch(objid, bflg, httphp, pathprefix)

    #msgproc.log("%s" % entries)
    encoded = json.dumps(entries)
    return {"entries" : encoded}


@dispatcher.record('search')
def search(a):
    msgproc.log("search: [%s]" % a)
    objid = a['objid']
    if re.match('0\$uprcl\$', objid) is None:
        raise Exception("bad objid [%s]" % objid)

    upnps = a['origsearch']

    entries = uprclsearch.search(rclconfdir, objid, upnps, g_myprefix,
                                 httphp, pathprefix)
    
    encoded = json.dumps(entries)
    return {"entries" : encoded}

uprcl_init()

msgproc.log("Uprcl running")
msgproc.mainloop()
