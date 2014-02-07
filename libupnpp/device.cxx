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
#include <iostream>
using namespace std;

#include "upnpplib.hxx"
#include "vdir.hxx"
#include "device.hxx"

unordered_map<std::string, UpnpDevice *> UpnpDevice::o_devices;
static string xmlquote(const string& in)
{
    string out;
    for (unsigned int i = 0; i < in.size(); i++) {
        switch(in[i]) {
        case '"': out += "&quot;";break;
        case '&': out += "&amp;";break;
        case '<': out += "&lt;";break;
        case '>': out += "&gt;";break;
        case '\'': out += "&apos;";break;
        default: out += in[i];
        }
    }
    return out;
}


static bool vectorstoargslists(const vector<string>& names, 
                               const vector<string>& values,
                               vector<string>& qvalues,
                               vector<const char *>& cnames,
                               vector<const char *>& cvalues)
{
    if (names.size() != values.size()) {
        cerr << "vectorstoargslists: bad sizes" << endl;
        return false;
    }

    cnames.reserve(names.size());
    for (unsigned int i = 0; i < names.size(); i++) {
        cnames.push_back(names[i].c_str());
    }
    qvalues.clear();
    qvalues.reserve(values.size());
    cvalues.reserve(values.size());
    for (unsigned int i = 0; i < values.size(); i++) {
        qvalues.push_back(xmlquote(values[i]));
        cvalues.push_back(qvalues[i].c_str());
    }
    return true;
}

UpnpDevice::UpnpDevice(const string& deviceId, 
                       const unordered_map<string, string>& xmlfiles)
    : m_deviceId(deviceId)
{
    cerr << "UpnpDevice::UpnpDevice(" << m_deviceId << ")" << endl;

    m_lib = LibUPnP::getLibUPnP(true);
    if (!m_lib) {
        cerr << " Can't get LibUPnP" << endl;
        return;
    }
    if (!m_lib->ok()) {
        cerr << "Lib init failed: " <<
            m_lib->errAsString("main", m_lib->getInitError()) << endl;
        m_lib = 0;
        return;
    }

    if (o_devices.empty()) {
        // First call: init callbacks
        m_lib->registerHandler(UPNP_CONTROL_ACTION_REQUEST, sCallBack, this);
	m_lib->registerHandler(UPNP_CONTROL_GET_VAR_REQUEST, sCallBack, this);
	m_lib->registerHandler(UPNP_EVENT_SUBSCRIPTION_REQUEST, sCallBack,this);
    }

    VirtualDir* theVD = VirtualDir::getVirtualDir();
    if (theVD == 0) {
        cerr << "UpnpDevice::UpnpDevice: can't get VirtualDir" << endl;
        return;
    }

    unordered_map<string,string>::const_iterator it = 
        xmlfiles.find("description.xml");
    if (it == xmlfiles.end()) {
        cerr << "UpnpDevice::UpnpDevice: no description.xml found in xmlfiles"
             << endl;
        return;
    } 

    const string& description = it->second;

    for (it = xmlfiles.begin(); it != xmlfiles.end(); it++) {
        theVD->addFile("/", it->first, it->second, "application/xml");
    }

    // Start up the web server for sending out description files
    m_lib->setupWebServer(description);

    o_devices[m_deviceId] = this;
}

// Main libupnp callback: use the device id and call the right device
int UpnpDevice::sCallBack(Upnp_EventType et, void* evp, void* tok)
{
    //cerr << "UpnpDevice::sCallBack" << endl;

    string deviceid;
    switch (et) {
    case UPNP_CONTROL_ACTION_REQUEST:
        deviceid = ((struct Upnp_Action_Request *)evp)->DevUDN;
    break;

    case UPNP_CONTROL_GET_VAR_REQUEST:
        deviceid = ((struct Upnp_State_Var_Request *)evp)->DevUDN;
    break;

    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        deviceid = ((struct  Upnp_Subscription_Request*)evp)->UDN;
    break;

    default:
        cerr << "UpnpDevice::sCallBack: unknown event " << et << endl;
        return UPNP_E_INVALID_PARAM;
    }
    // cerr << "UpnpDevice::sCallBack: deviceid[" << deviceid << "]" << endl;

    unordered_map<std::string, UpnpDevice *> *devmap = 
        (unordered_map<std::string, UpnpDevice *> *) tok;
    unordered_map<std::string, UpnpDevice *>::iterator it =
        o_devices.find(deviceid);

    if (it == o_devices.end()) {
        cerr << "UpnpDevice::sCallBack: Device not found: [" << 
            deviceid << "]" << endl;
        return UPNP_E_INVALID_PARAM;
    }
    // cerr << "UpnpDevice::sCallBack: device found: [" << it->second 
    // << "]" << endl;
    return (it->second)->callBack(et, evp);
}

static PTMutexInit cblock;
int UpnpDevice::callBack(Upnp_EventType et, void* evp)
{
    PTMutexLocker lock(cblock);
    cerr << "UpnpDevice::callBack: evt type:" << 
        LibUPnP::evTypeAsString(et).c_str() << endl;

    switch (et) {
    case UPNP_CONTROL_ACTION_REQUEST:
    {
        struct Upnp_Action_Request *act = (struct Upnp_Action_Request *)evp;
        cerr << "UPNP_CONTROL_ACTION_REQUEST: " << act->ActionName <<
            " Params: " << ixmlPrintDocument(act->ActionRequest) << endl;

        unordered_map<string, string>::const_iterator servit = 
            m_serviceTypes.find(act->ServiceID);
        if (servit == m_serviceTypes.end()) {
            cerr << "Bad serviceID" << endl;
            return UPNP_E_INVALID_PARAM;
        }
        const string& servicetype = servit->second;

        unordered_map<string, soapfun>::iterator callit = 
            m_calls.find(act->ActionName);
        if (callit == m_calls.end()) {
            cerr << "No such action: " << act->ActionName << endl;
            return UPNP_E_INVALID_PARAM;
        }

        SoapArgs sc;
        if (!decodeSoapBody(act->ActionName, act->ActionRequest, &sc)) {
            cerr << "Error decoding Action call arguments" << endl;
            return UPNP_E_INVALID_PARAM;
        }
        SoapData dt;
        dt.name = act->ActionName;
        dt.serviceType = servicetype;

        // Call the action routine
        int ret = callit->second(sc, dt);
        if (ret != UPNP_E_SUCCESS) {
            cerr << "Action failed: " << sc.name << endl;
            return ret;
        }

        // Encode result data
        act->ActionResult = buildSoapBody(dt);
        cerr << "Response data: " << 
            ixmlPrintDocument(act->ActionResult) << endl;

        return ret;

    }
    break;

    case UPNP_CONTROL_GET_VAR_REQUEST:
        // Note that the "Control: query for variable" action is
        // deprecated (upnp arch v1), and we should never get these.
    {
        struct Upnp_State_Var_Request *act = 
            (struct Upnp_State_Var_Request *)evp;
        cerr << "UPNP_CONTROL_GET_VAR__REQUEST?: " << act->StateVarName << endl;
    }
    break;

    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
    {
        struct Upnp_Subscription_Request *act = 
            (struct  Upnp_Subscription_Request*)evp;
        cerr << "UPNP_EVENT_SUBSCRIPTION_REQUEST: " << act->ServiceId << endl;

        vector<string> names, values, qvalues;
        if (!getEventData(true, act->ServiceId, names, values)) {
            break;
        }
        vector<const char *> cnames, cvalues;
        vectorstoargslists(names, values, qvalues, cnames, cvalues);
        int ret = 
            UpnpAcceptSubscription(m_lib->getdvh(), act->UDN, act->ServiceId,
                                   &cnames[0], &cvalues[0],
                                   int(cnames.size()), act->Sid);
        if (ret != UPNP_E_SUCCESS) {
            cerr << "UpnpDevice::callBack: UpnpAcceptSubscription failed: " 
                 << ret << endl;
        }

        return ret;
    }
    break;

    default:
        cerr << "UpnpDevice::callBack: unknown op" << endl;
        return UPNP_E_INVALID_PARAM;
    }
    return UPNP_E_INVALID_PARAM;
}

void UpnpDevice::addServiceType(const std::string& serviceId, 
                                const std::string& serviceType)
{
    cerr << "UpnpDevice::addServiceType: [" << 
        serviceId << "] -> [" << serviceType << endl;
    m_serviceTypes[serviceId] = serviceType;
}

void UpnpDevice::addActionMapping(const std::string& actName, soapfun fun)
{
    // cerr << "UpnpDevice::addActionMapping:" << actName << endl;
    m_calls[actName] = fun;
}

void UpnpDevice::notifyEvent(const string& serviceId,
                             const vector<string>& names, 
                             const vector<string>& values)
{
    cerr << "UpnpDevice::notifyEvent" << endl;

    vector<const char *> cnames, cvalues;
    vector<string> qvalues;
    vectorstoargslists(names, values, qvalues, cnames, cvalues);

    int ret = UpnpNotify(m_lib->getdvh(), m_deviceId.c_str(), 
                         serviceId.c_str(), &cnames[0], &cvalues[0],
                         int(cnames.size()));
    if (ret != UPNP_E_SUCCESS) {
        cerr << "UpnpDevice::notifyEvent: UpnpNotify failed: " << ret << endl;
    }
}

void UpnpDevice::eventloop()
{
    struct timespec duration;
    duration.tv_sec = 0;
    duration.tv_nsec = 500 * 1000 * 1000; // 1/2 S
    while (nanosleep(&duration, 0) == 0) {
        static int count = 0;
        //cerr << "UpnpDevice::eventloop: " << count++ << endl;
        PTMutexLocker lock(cblock);
        for (unordered_map<string, string>::const_iterator it = 
                 m_serviceTypes.begin();
             it != m_serviceTypes.end(); it++) {
            vector<string> names, values;
            if (!getEventData(false, it->first, names, values) || 
                names.empty()) {
                continue;
            }
            notifyEvent(it->first, names, values);
        }
    }
}
