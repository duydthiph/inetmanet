//
// Copyright (C) 2000 Institut fuer Telematik, Universitaet Karlsruhe
// Copyright (C) 2004,2005 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


//
// Author: Jochen Reber
// Rewrite: Andras Varga 2004,2005
//

#ifndef __UDP_H__
#define __UDP_H__

#include <map>
#include <list>
#include "UDPControlInfo_m.h"

class IPControlInfo;
class IPv6ControlInfo;

const char *ERROR_IP_ADDRESS = "10.0.0.255";
const int UDP_HEADER_BYTES = 8;


/**
 * Implements the UDP protocol: encapsulates/decapsulates user data into/from UDP.
 *
 * Sends incoming packets to *all* matching sockets. No attempt is made to filter out
 * multiple binds to the same port.
 *
 * More info in the NED file.
 */
class INET_API UDP : public cSimpleModule
{
  public:
    struct SockDesc
    {
        int sockId; // supposed to be unique across apps
        int userId; // we just send it back, but don't do anything with it
        int appGateIndex;
        bool onlyLocalPortIsSet;
        IPvXAddress localAddr;
        IPvXAddress remoteAddr;
        short localPort;
        short remotePort;
        int interfaceId; // FIXME do real sockets allow filtering by input interface??
    };

    typedef std::list<SockDesc *> SockDescList;
    typedef std::map<int,SockDesc *> SocketsByIdMap;
    typedef std::map<int,SockDescList> SocketsByPortMap;

  protected:
    SocketsByIdMap socketsByIdMap;
    SocketsByPortMap socketsByPortMap;

    short nextEphemeralPort;

    int numSent;
    int numPassedUp;
    int numDroppedWrongPort;
    int numDroppedBadChecksum;

  protected:
    // utility: show current statistics above the icon
    void updateDisplayString();

    // bind socket
    void bind(int gateIndex, UDPControlInfo *ctrl);

    // unbind socket
    void unbind(int sockId);

    // ephemeral port
    short getEphemeralPort();

    bool matchesSocket(UDPPacket *udp, IPControlInfo *ctrl, SockDesc *sd);
    bool matchesSocket(UDPPacket *udp, IPv6ControlInfo *ctrl, SockDesc *sd);
    void sendUp(cMessage *payload, UDPPacket *udpHeader, IPControlInfo *ctrl, SockDesc *sd);
    void sendUp(cMessage *payload, UDPPacket *udpHeader, IPv6ControlInfo *ctrl, SockDesc *sd);

    // process packets coming from IP
    virtual void processMsgFromIP(UDPPacket *udpPacket);

    // process packets from application
    virtual void processMsgFromApp(cMessage *appData);

    // process commands from application
    virtual void processCommandFromApp(cMessage *msg);

  public:
    UDP() {}
    virtual ~UDP();

  protected:
    virtual void initialize();
    virtual void handleMessage(cMessage *msg);
};

#endif

