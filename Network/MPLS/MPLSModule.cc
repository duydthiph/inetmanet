#include <omnetpp.h>
#include <string.h>
#include "MPLSModule.h"


using namespace std;

Define_Module(MPLSModule);


void MPLSModule::initialize()
{
    ipdataQueue.setName("ipdataQueue");
    ldpQueue.setName("ldpQueue");
    maxFecId = 0;

    // Is this LSR an Ingress Router
    isIR = par("isIR");

    // Is this ER an Egress Router
    isER = par("isER");

    // Which FEC classification scheme is used
    classifierType = par("classifier").longValue();

    // Signalling component is ready or not
    //isSignallingReady = false;
    isSignallingReady = true; // FIXME
}


void MPLSModule::dumpFECTable()
{
    ev << "Current FEC table:\n";
    for (std::vector<FECElem>::iterator it = fecList.begin(); it != fecList.end(); it++)
    {
        ev << "FECid=" << it->fecId
           << "  dest=" << it->destAddr
           << "  src =" << it->srcAddr << "\n";
    }
}


void MPLSModule::handleMessage(cMessage * msg)
{
    if (!strcmp(msg->arrivalGate()->name(), "fromL3"))
    {
        ev << "Processing message from L3: " << msg << endl;
        processPacketFromL3(msg);
    }
    else if (!strcmp(msg->arrivalGate()->name(), "fromSignalModule"))
    {
        ev << "Processing message from signalling module: " << msg << endl;
        processPacketFromSignalling(msg);
    }
    else
    {
        ev << "Processing message from L2: " << msg << endl;
        processPacketFromL2(msg);
    }
}


void MPLSModule::processPacketFromL3(cMessage * msg)
{
    IPDatagram *ipdatagram = check_and_cast<IPDatagram *>(msg);
    int gateIndex = msg->arrivalGate()->index();

    // If the MPLS processing is not on, then simply passing the packet
    if (ipdatagram->hasPar("trans"))  // FIXME do we need this field?
    {
        send(ipdatagram, "toL2", gateIndex);
        return;
    }

    // IP data from L3 and requires MPLS processing
    MPLSPacket *outPacket = new MPLSPacket(ipdatagram->name());
    outPacket->encapsulate(ipdatagram);

    // This is native IP
    outPacket->pushLabel(-1);
    send(outPacket, "toL2", gateIndex);
}


void MPLSModule::processPacketFromSignalling(cMessage * msg)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();

    if (!isSignallingReady)
    {
        // This is message from LDP saying that it is ready.
        // FIXME some assert() to make sure it's really that?
        // FIXME why should *we* buffer messages? Why can't we send them to LDP and let *it* buffer them until it gets ready???
        ev << "LDP says it is ready, sending out buffered LDP queries to it\n";

        isSignallingReady = true;

        // Start to send out all the pending queries to signal_module (LDP or RSVP-TE)
        cModule *signallingMod = parentModule()->submodule("signal_module");  // FIXME maybe use connections instead of direct sending....
        if (!signallingMod)
            error("Cannot find signal_module");
        while (!ldpQueue.empty())
        {
            cMessage *ldpMsg = (cMessage *) ldpQueue.pop();
            sendDirect(ldpMsg, 0.0, signallingMod, "from_mpls_switch");
        }
        delete msg;
        return;
    }

    // Get the mapping from the message: "label", "fec" parameters
    int label = msg->par("label").longValue();
    int returnedFEC = (int) (msg->par("fec").longValue());
    bool isLDP = msg->hasPar("my_name");
    delete msg;

    ev << "Message from signalling: label=" << label << ", FEC=" << returnedFEC << "\n";

    // Update FEC table
    if (!isLDP)
    {
        for (int i = 0; i < fecList.size(); i++)
        {
            // FIXME!!!! pending FEC's seem to get an id (2*MAX_LSP_NO-returnedFEC)
            // until they are resolved!!! Ughhh!!! should use a bool flag "pending"
            if (fecList[i].fecId == (2 * MAX_LSP_NO - returnedFEC))
            {
                fecList[i].fecId = returnedFEC;
                break;
            }
        }
    }

    dumpFECTable();

    // try sending out buffered IP datagrams and MPLS packets which are waiting for this FEC
    trySendBufferedPackets(returnedFEC);
}


void MPLSModule::trySendBufferedPackets(int returnedFEC)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();

    for (int i = 0; i < ipdataQueue.items(); i++)
    {
        cMessage *queuedmsg = (cMessage *) ipdataQueue[i];
        if (!queuedmsg)
            continue;

        // Release packets in queue
        // FIXME only IPDatagrams can occur here, or??
        IPDatagram *data = dynamic_cast<IPDatagram *>(queuedmsg);
        MPLSPacket *mplsPck = dynamic_cast<MPLSPacket *>(queuedmsg);
        ASSERT(data || mplsPck);

        // Incoming interface
        int gateIndex;
        if (data)
        {
            gateIndex = data->par("gateIndex");
        }
        else
        {
            gateIndex = mplsPck->par("gateIndex");
            data = check_and_cast<IPDatagram *>(mplsPck->decapsulate());
        }

        InterfaceEntry *ientry = rt->interfaceByPortNo(gateIndex);
        string senderInterface = string(ientry->name.c_str());
        int fecID = classifyPacket(data, classifierType);

        // FIXME khmmm --- we already decapsulated here, can't prentend nothing happened!@!!!@!!!! Andras
        if (fecID!=returnedFEC)
            continue;

        // Remove the message
        ipdataQueue.remove(i);

        // Construct a new MPLS packet
        MPLSPacket *newPacket = NULL;
        if (mplsPck != NULL)
        {
            newPacket = mplsPck;
        }
        else
        {
            newPacket = new MPLSPacket(data->name());
            ev << "FIXME debug: " << data->fullPath();
            ev << " / " << data->owner()->fullPath() << endl;
            newPacket->encapsulate(data);
        }

        // Find label and outgoing interface
        int label;
        string outgoingInterface;
        bool found = lt->resolveFec(returnedFEC, label, outgoingInterface);
        ASSERT(found);

        newPacket->pushLabel(label);
        newPacket->setKind(fecID);

        int outgoingPort = rt->interfaceByName(outgoingInterface.c_str())->outputPort;
        send(newPacket, "toL2", outgoingPort);
    }
}

void MPLSModule::processPacketFromL2(cMessage *msg)
{
    IPDatagram *ipdatagram = dynamic_cast<IPDatagram *>(msg);
    MPLSPacket *mplsPacket = dynamic_cast<MPLSPacket *>(msg);

    if (ipdatagram)
    {
        if (ipdatagram->hasPar("trans")) // FIXME this can never happen, peer won't set "trans"...
        {
            ev << "'trans' param set on message, sending up\n";
            int gateIndex = msg->arrivalGate()->index();
            send(ipdatagram, "toL3", gateIndex);
        }
        else if (!isIR)
        {
            // if we are not an Ingress Router and still get an IP packet,
            // then just pass it through to L3
            ev << "setting 'trans' param on message and sending up\n";
            ipdatagram->addPar("trans") = 0;
            int gateIndex = msg->arrivalGate()->index();
            send(ipdatagram, "toL3", gateIndex);
        }
        else
        {
            // IP datagram arrives at Ingress router. We'll try to classify it
            // and add an MPLS header
            processIPDatagramFromL2(ipdatagram);
        }
    }
    else if (mplsPacket)
    {
        processMPLSPacketFromL2(mplsPacket);
    }
    else
    {
        error("Unknown message received");
    }
}


void MPLSModule::processMPLSPacketFromL2(MPLSPacket *mplsPacket)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();

    int gateIndex = mplsPacket->arrivalGate()->index();

    // Here we process MPLS packets
    InterfaceEntry *ientry = rt->interfaceByPortNo(gateIndex);
    string senderInterface = string(ientry->name.c_str());
    int oldLabel = mplsPacket->topLabel();

    if (oldLabel==-1)
    {
        // This is not IP native packet
        // Decapsulate the message and pass up to L3 since this is LDP packet
        //
        // FIXME this smells like hacking. Or is this an "IPv4 Explicit NULL Label"
        // (rfc 3032) or something like this? (Andras)
        IPDatagram *ipdatagram = check_and_cast<IPDatagram *>(mplsPacket->decapsulate());
        send(ipdatagram, "toL3", gateIndex);
        return;
    }

    int optCode=-1;
    int newLabel=-1;
    string outgoingInterface;
    bool found = lt->resolveLabel(oldLabel, senderInterface,
                                  optCode, newLabel, outgoingInterface);

    if (found && newLabel!=-1)  // New label found
    {
        ev << "incoming label=" << oldLabel << ": ";
        switch (optCode)
        {
            case PUSH_OPER:
                ev << "PUSH " << newLabel;
                mplsPacket->pushLabel(newLabel);
                break;
            case SWAP_OPER:
                ev << "SWAP by " << newLabel;
                mplsPacket->swapLabel(newLabel);
                break;
            case POP_OPER:
                ev << "POP";
                mplsPacket->popLabel();
                break;
            default:
                error("Unknown MPLS OptCode %d", optCode);
        }
        ev << ", outgoing interface: " << outgoingInterface << "\n";

        int outgoingPort = rt->interfaceByName(outgoingInterface.c_str())->outputPort;
        send(mplsPacket, "toL2", outgoingPort);

    }
    // FIXME (!found) case not handled...
    else if (newLabel==-1 && isER)  // ER router and the new label must be native IP
    {
        int outgoingPort = rt->interfaceByName(outgoingInterface.c_str())->outputPort;

        mplsPacket->popLabel();

        // Test if this is a tunnel ER
        if (!mplsPacket->hasLabel())
        {
            // last label popped: decapsulate and send out IP datagram
            IPDatagram *nativeIP = check_and_cast<IPDatagram *>(mplsPacket->decapsulate());
            delete mplsPacket;

            ev << "No new label & last label popped: decapsulating\n";
            ev << "Sending the packet to interface " << outgoingInterface.c_str() << "\n";

            send(nativeIP, "toL2", outgoingPort);
        }
        else  // Message is out of the tunnel
        {
            ev << "Label popped\n";
            ev << "Sending the packet to interface " << outgoingInterface.c_str() << "\n";
            send(mplsPacket, "toL2", outgoingPort);
        }
    }
    else  // Some sort of error here
    {
        error("LIB table inconsistent");
    }
}

void MPLSModule::processIPDatagramFromL2(IPDatagram *ipdatagram)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();

    int gateIndex = ipdatagram->arrivalGate()->index();

    // Incoming interface
    InterfaceEntry *ientry = rt->interfaceByPortNo(gateIndex);
    string senderInterface = string(ientry->name.c_str());
    ev << "Message from outside to Ingress node\n";

    bool makeRequest = false;
    int fecID = classifyPacket(ipdatagram, classifierType);
    if (fecID == -1)
    {
        makeRequest = true;
        fecID = addFEC(ipdatagram, classifierType);
        ev << "Registered new FEC=" << fecID << "\n";
    }
    ev << "Packet src=" << ipdatagram->srcAddress() <<
          ", dest=" << ipdatagram->destAddress() <<
          " --> FEC=" << fecID << "\n";

    int label=-1;
    string outgoingInterface;
    bool found = lt->resolveFec(fecID, label, outgoingInterface);

    if (found)  // New Label found
    {
        ev << "FEC found in LIB: outLabel=" << label << ", outInterface=" << outgoingInterface << "\n";

        // Construct a new MPLS packet
        MPLSPacket *newPacket = NULL;
        newPacket = new MPLSPacket(ipdatagram->name());
        newPacket->encapsulate(ipdatagram);

        // consistent in packet color
        if (fecID < MAX_LSP_NO)
            newPacket->setKind(fecID);
        else
            newPacket->setKind(2 * MAX_LSP_NO - fecID);

        newPacket->pushLabel(label);

        int outgoingPort = rt->interfaceByName(outgoingInterface.c_str())->outputPort;
        send(newPacket, "toL2", outgoingPort);
    }
    else  // Need to make ldp query
    {
        ev << "FEC not yet in LIB, queueing up\n";

        // queue up packet
        ipdatagram->addPar("gateIndex") = gateIndex;
        ipdataQueue.add(ipdatagram);

        if (makeRequest)
        {
            // if FEC just made it into fecList, we haven't asked signalling yet: do it now
            ev << "Sending path request to signalling module\n";
            sendPathRequestToSignalling(fecID, ipdatagram->srcAddress(), ipdatagram->destAddress(), gateIndex);
        }
    }
}


void MPLSModule::sendPathRequestToSignalling(int fecID, IPAddress src, IPAddress dest, int gateIndex)
{
    cModule *signallingMod = parentModule()->submodule("signal_module");
    if (!signallingMod)
        error("Cannot find signal_module");

    // assemble Path Request command
    cMessage *signalMessage = new cMessage("PATH_REQUEST");
    signalMessage->addPar("FEC") = fecID;
    signalMessage->addPar("src_addr") = src.getInt();
    signalMessage->addPar("dest_addr") = dest.getInt();
    signalMessage->addPar("gateIndex") = gateIndex;

    if (isSignallingReady)
    {
        // Send to directly to its input gate
        sendDirect(signalMessage, 0.0, signallingMod, "from_mpls_switch");
    }
    else  // Pending
    {
        ev << "Signalling not yet ready, queueing up request\n";
        ldpQueue.insert(signalMessage);
    }
}

int MPLSModule::classifyPacket(IPDatagram *ipdatagram, int type)
{
    IPAddress src = ipdatagram->srcAddress();
    IPAddress dest = ipdatagram->destAddress();

    // find existing FEC based on classifier type
    for (std::vector<FECElem>::iterator it=fecList.begin(); it!=fecList.end(); it++)
    {
        // FEC determined by Destination only
        if (type==DEST_CLASSIFIER && dest==it->destAddr)
            return it->fecId;

        // FEC determined by Destination and Source
        if (type==SRC_AND_DEST_CLASSIFIER && dest==it->destAddr && src==it->srcAddr)
            return it->fecId;
    }
    return -1;
}

int MPLSModule::addFEC(IPDatagram *ipdatagram, int type)
{
    FECElem newEle;
    newEle.destAddr = ipdatagram->destAddress();
    newEle.srcAddr = ipdatagram->srcAddress();
    newEle.fecId = ++maxFecId;
    fecList.push_back(newEle);
    return newEle.fecId;
}


