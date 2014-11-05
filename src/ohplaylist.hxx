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
#ifndef _OHPLAYLIST_H_X_INCLUDED_
#define _OHPLAYLIST_H_X_INCLUDED_

#include <string>                       // for string
#include <unordered_map>                // for unordered_map
#include <vector>                       // for vector

#include "libupnpp/device/device.hxx"   // for UpnpService
#include "libupnpp/soaphelp.hxx"        // for SoapArgs, SoapData

class UpMpd;
class UpMpdRenderCtl;

using namespace UPnPP;

class OHPlaylist : public UPnPProvider::UpnpService {
public:
    OHPlaylist(UpMpd *dev, UpMpdRenderCtl *ctl);

    virtual bool getEventData(bool all, std::vector<std::string>& names, 
                              std::vector<std::string>& values);
    virtual bool cacheFind(const std::string& uri, std:: string& meta);

    // This is the "normal" call version of soap "insert", used
    // internally by the receiver service
    virtual bool insertUri(int afterid, const std::string& uri, 
                           const std::string& metadata, int *newid = 0);

private:
    int play(const SoapArgs& sc, SoapData& data);
    int pause(const SoapArgs& sc, SoapData& data);
    int stop(const SoapArgs& sc, SoapData& data);
    int next(const SoapArgs& sc, SoapData& data);
    int previous(const SoapArgs& sc, SoapData& data);
    int setRepeat(const SoapArgs& sc, SoapData& data);
    int repeat(const SoapArgs& sc, SoapData& data);
    int setShuffle(const SoapArgs& sc, SoapData& data);
    int shuffle(const SoapArgs& sc, SoapData& data);
    int seekSecondAbsolute(const SoapArgs& sc, SoapData& data);
    int seekSecondRelative(const SoapArgs& sc, SoapData& data);
    int seekId(const SoapArgs& sc, SoapData& data);
    int seekIndex(const SoapArgs& sc, SoapData& data);
    int transportState(const SoapArgs& sc, SoapData& data);
    int id(const SoapArgs& sc, SoapData& data);
    int ohread(const SoapArgs& sc, SoapData& data);
    int readList(const SoapArgs& sc, SoapData& data);
    int insert(const SoapArgs& sc, SoapData& data);
    int deleteId(const SoapArgs& sc, SoapData& data);
    int deleteAll(const SoapArgs& sc, SoapData& data);
    int tracksMax(const SoapArgs& sc, SoapData& data);
    int idArray(const SoapArgs& sc, SoapData& data);
    int idArrayChanged(const SoapArgs& sc, SoapData& data);
    int protocolInfo(const SoapArgs& sc, SoapData& data);

    bool makeIdArray(std::string&);
    bool makestate(std::unordered_map<std::string, std::string> &st);
    void maybeWakeUp(bool ok);
    // State variable storage
    std::unordered_map<std::string, std::string> m_state;
    UpMpd *m_dev;

    // Storage for song metadata, indexed by URL.  This used to be
    // indexed by song id, but this does not survive MPD restarts.
    // The data is the DIDL XML string.
    std::unordered_map<std::string, std::string> m_metacache;
    bool m_cachedirty;

    // Avoid re-reading the whole MPD queue every time by using the
    // queue version.
    int m_mpdqvers;
    std::string m_idArrayCached;
};

#endif /* _OHPLAYLIST_H_X_INCLUDED_ */