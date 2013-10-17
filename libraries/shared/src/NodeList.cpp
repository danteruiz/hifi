//
//  NodeList.cpp
//  hifi
//
//  Created by Stephen Birarda on 2/15/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <QtCore/QDebug>
#include <QtNetwork/QHostInfo>

#include "Assignment.h"
#include "Logging.h"
#include "NodeList.h"
#include "NodeTypes.h"
#include "PacketHeaders.h"
#include "SharedUtil.h"

#ifdef _WIN32
#include "Syssocket.h"
#else
#include <arpa/inet.h>
#endif

const char SOLO_NODE_TYPES[2] = {
    NODE_TYPE_AVATAR_MIXER,
    NODE_TYPE_AUDIO_MIXER
};

const QString DEFAULT_DOMAIN_HOSTNAME = "root.highfidelity.io";
const unsigned short DEFAULT_DOMAIN_SERVER_PORT = 40102;

bool silentNodeThreadStopFlag = false;
bool pingUnknownNodeThreadStopFlag = false;

NodeList* NodeList::_sharedInstance = NULL;

NodeList* NodeList::createInstance(char ownerType, unsigned short int socketListenPort) {
    if (!_sharedInstance) {
        _sharedInstance = new NodeList(ownerType, socketListenPort);
    } else {
        qDebug("NodeList createInstance called with existing instance.");
    }
    
    return _sharedInstance;
}

NodeList* NodeList::getInstance() {
    if (!_sharedInstance) {
        qDebug("NodeList getInstance called before call to createInstance. Returning NULL pointer.");
    }
    
    return _sharedInstance;
}

NodeList::NodeList(char newOwnerType, unsigned short int newSocketListenPort) :
    _domainHostname(DEFAULT_DOMAIN_HOSTNAME),
    _domainIP(),
    _domainPort(DEFAULT_DOMAIN_SERVER_PORT),
    _nodeBuckets(),
    _numNodes(0),
    _nodeSocket(newSocketListenPort),
    _ownerType(newOwnerType),
    _nodeTypesOfInterest(NULL),
    _ownerID(UNKNOWN_NODE_ID),
    _lastNodeID(UNKNOWN_NODE_ID + 1),
    _numNoReplyDomainCheckIns(0),
    _assignmentServerSocket(NULL),
    _checkInPacket(NULL),
    _numBytesCheckInPacket(0),
    _publicAddress(),
    _publicPort(0)
{
    
}

NodeList::~NodeList() {
    delete _nodeTypesOfInterest;
    
    clear();
    
    // stop the spawned threads, if they were started
    stopSilentNodeRemovalThread();
}

void NodeList::setDomainHostname(const QString& domainHostname) {
    
    if (domainHostname != _domainHostname) {
        int colonIndex = domainHostname.indexOf(':');
        
        if (colonIndex > 0) {
            // the user has included a custom DS port with the hostname
            
            // the new hostname is everything up to the colon
            _domainHostname = domainHostname.left(colonIndex);
            
            // grab the port by reading the string after the colon
            _domainPort = atoi(domainHostname.mid(colonIndex + 1, domainHostname.size()).toLocal8Bit().constData());
            
            qDebug() << "Updated hostname to" << _domainHostname << "and port to" << _domainPort << "\n";
            
        } else {
            // no port included with the hostname, simply set the member variable and reset the domain server port to default
            _domainHostname = domainHostname;
            _domainPort = DEFAULT_DOMAIN_SERVER_PORT;
        }
        
        // clear the NodeList so nodes from this domain are killed
        clear();
        
        // reset our _domainIP to the null address so that a lookup happens on next check in
        _domainIP.clear();
        notifyDomainChanged();
    }
}

void NodeList::timePingReply(sockaddr *nodeAddress, unsigned char *packetData) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (socketMatch(node->getPublicSocket(), nodeAddress) || 
            socketMatch(node->getLocalSocket(), nodeAddress)) {     

            int pingTime = usecTimestampNow() - *(uint64_t*)(packetData + numBytesForPacketHeader(packetData));
            
            node->setPingMs(pingTime / 1000);
            break;
        }
    }
}

void NodeList::processNodeData(sockaddr* senderAddress, unsigned char* packetData, size_t dataBytes) {
    switch (packetData[0]) {
        case PACKET_TYPE_DOMAIN: {
            // only process the DS if this is our current domain server
            if (_domainIP == QHostAddress(senderAddress)) {
                processDomainServerList(packetData, dataBytes);
            }
            
            break;
        }
        case PACKET_TYPE_PING: {
            char pingPacket[dataBytes];
            memcpy(pingPacket, packetData, dataBytes);
            populateTypeAndVersion((unsigned char*) pingPacket, PACKET_TYPE_PING_REPLY);
            _nodeSocket.send(senderAddress, pingPacket, dataBytes);
            break;
        }
        case PACKET_TYPE_PING_REPLY: {
            timePingReply(senderAddress, packetData);
            break;
        }
        case PACKET_TYPE_STUN_RESPONSE: {
            // a STUN packet begins with 00, we've checked the second zero with packetVersionMatch
            // pass it along so it can be processed into our public address and port
            processSTUNResponse(packetData, dataBytes);
            break;
        }
    }
}

void NodeList::processBulkNodeData(sockaddr *senderAddress, unsigned char *packetData, int numTotalBytes) {
    
    // find the avatar mixer in our node list and update the lastRecvTime from it
    Node* bulkSendNode = nodeWithAddress(senderAddress);

    if (bulkSendNode) {
        bulkSendNode->setLastHeardMicrostamp(usecTimestampNow());
        bulkSendNode->recordBytesReceived(numTotalBytes);
        
        int numBytesPacketHeader = numBytesForPacketHeader(packetData);
        
        unsigned char* startPosition = packetData;
        unsigned char* currentPosition = startPosition + numBytesPacketHeader;
        unsigned char packetHolder[numTotalBytes];
        
        // we've already verified packet version for the bulk packet, so all head data in the packet is also up to date
        populateTypeAndVersion(packetHolder, PACKET_TYPE_HEAD_DATA);
        
        uint16_t nodeID = -1;
        
        while ((currentPosition - startPosition) < numTotalBytes) {
            unpackNodeId(currentPosition, &nodeID);
            memcpy(packetHolder + numBytesPacketHeader,
                   currentPosition,
                   numTotalBytes - (currentPosition - startPosition));
            
            Node* matchingNode = nodeWithID(nodeID);
            
            if (!matchingNode) {
                // we're missing this node, we need to add it to the list
                matchingNode = addOrUpdateNode(NULL, NULL, NODE_TYPE_AGENT, nodeID);
            }
            
            currentPosition += updateNodeWithData(matchingNode,
                                                  packetHolder,
                                                  numTotalBytes - (currentPosition - startPosition));
            
        }
    }    
}

int NodeList::updateNodeWithData(sockaddr *senderAddress, unsigned char *packetData, size_t dataBytes) {
    // find the node by the sockaddr
    Node* matchingNode = nodeWithAddress(senderAddress);
    
    if (matchingNode) {
        return updateNodeWithData(matchingNode, packetData, dataBytes);
    } else {
        return 0;
    }
}

int NodeList::updateNodeWithData(Node *node, unsigned char *packetData, int dataBytes) {
    node->lock();
    
    node->setLastHeardMicrostamp(usecTimestampNow());
    
    if (node->getActiveSocket()) {
        node->recordBytesReceived(dataBytes);
    }
    
    if (!node->getLinkedData() && linkedDataCreateCallback) {
        linkedDataCreateCallback(node);
    }
    
    int numParsedBytes = node->getLinkedData()->parseData(packetData, dataBytes);
    
    node->unlock();
    
    return numParsedBytes;
}

Node* NodeList::nodeWithAddress(sockaddr *senderAddress) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (node->getActiveSocket() && socketMatch(node->getActiveSocket(), senderAddress)) {
            return &(*node);
        }
    }
    
    return NULL;
}

Node* NodeList::nodeWithID(uint16_t nodeID) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (node->getNodeID() == nodeID) {
            return &(*node);
        }
    }

    return NULL;
}

int NodeList::getNumAliveNodes() const {
    int numAliveNodes = 0;
    
    for (NodeList::iterator node = begin(); node != end(); node++) {
        if (node->isAlive()) {
            ++numAliveNodes;
        }
    }
    
    return numAliveNodes;
}

void NodeList::clear() {
    qDebug() << "Clearing the NodeList. Deleting all nodes in list.\n";
    
    // delete all of the nodes in the list, set the pointers back to NULL and the number of nodes to 0
    for (int i = 0; i < _numNodes; i++) {
        Node** nodeBucket = _nodeBuckets[i / NODES_PER_BUCKET];
        Node* node = nodeBucket[i % NODES_PER_BUCKET];
        
        node->lock();
        delete node;
        
        node = NULL;
    }
    
    _numNodes = 0;
}

void NodeList::reset() {
    clear();
    _numNoReplyDomainCheckIns = 0;
    
    delete[] _checkInPacket;
    _checkInPacket = NULL;
    
    _numBytesCheckInPacket = 0;
    
    delete _nodeTypesOfInterest;
    _nodeTypesOfInterest = NULL;
}

void NodeList::setNodeTypesOfInterest(const char* nodeTypesOfInterest, int numNodeTypesOfInterest) {
    delete _nodeTypesOfInterest;
    
    _nodeTypesOfInterest = new char[numNodeTypesOfInterest + sizeof(char)];
    memcpy(_nodeTypesOfInterest, nodeTypesOfInterest, numNodeTypesOfInterest);
    _nodeTypesOfInterest[numNodeTypesOfInterest] = '\0';
}

const uint32_t RFC_5389_MAGIC_COOKIE_NETWORK_ORDER = htonl(0x2112A442);
const int NUM_BYTES_STUN_HEADER = 20;

void NodeList::sendSTUNRequest() {
    const char STUN_SERVER_HOSTNAME[] = "root.highfidelity.io";
    const unsigned short STUN_SERVER_PORT = 3478;
    
    unsigned char stunRequestPacket[NUM_BYTES_STUN_HEADER];
    
    int packetIndex = 0;

    // leading zeros + message type
    const uint16_t REQUEST_MESSAGE_TYPE = htons(0x0001);
    memcpy(stunRequestPacket + packetIndex, &REQUEST_MESSAGE_TYPE, sizeof(REQUEST_MESSAGE_TYPE));
    packetIndex += sizeof(REQUEST_MESSAGE_TYPE);
    
    // message length (no additional attributes are included)
    uint16_t messageLength = 0;
    memcpy(stunRequestPacket + packetIndex, &messageLength, sizeof(messageLength));
    packetIndex += sizeof(messageLength);
    
    memcpy(stunRequestPacket + packetIndex, &RFC_5389_MAGIC_COOKIE_NETWORK_ORDER, sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER));
    packetIndex += sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER);
    
    // transaction ID (random 12-byte unsigned integer)
    const uint NUM_TRANSACTION_ID_BYTES = 12;
    unsigned char transactionID[NUM_TRANSACTION_ID_BYTES];
    loadRandomIdentifier(transactionID, NUM_TRANSACTION_ID_BYTES);
    memcpy(stunRequestPacket + packetIndex, &transactionID, sizeof(transactionID));
    
    // lookup the IP for the STUN server
    static QHostInfo stunInfo = QHostInfo::fromName(STUN_SERVER_HOSTNAME);
    
    for (int i = 0; i < stunInfo.addresses().size(); i++) {
        if (stunInfo.addresses()[i].protocol() == QAbstractSocket::IPv4Protocol) {
            QString stunIPAddress = stunInfo.addresses()[i].toString();
            
            qDebug("Sending a stun request to %s\n", stunIPAddress.toLocal8Bit().constData());
            
            _nodeSocket.send(stunIPAddress.toLocal8Bit().constData(),
                             STUN_SERVER_PORT,
                             stunRequestPacket,
                             sizeof(stunRequestPacket));
            
            break;
        }
    }
}

void NodeList::processSTUNResponse(unsigned char* packetData, size_t dataBytes) {
    // check the cookie to make sure this is actually a STUN response
    // and read the first attribute and make sure it is a XOR_MAPPED_ADDRESS
    const int NUM_BYTES_MESSAGE_TYPE_AND_LENGTH = 4;
    const uint16_t XOR_MAPPED_ADDRESS_TYPE = htons(0x0020);
    
    if (memcmp(packetData + NUM_BYTES_MESSAGE_TYPE_AND_LENGTH,
               &RFC_5389_MAGIC_COOKIE_NETWORK_ORDER,
               sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER)) == 0
        && memcmp(packetData + NUM_BYTES_STUN_HEADER, &XOR_MAPPED_ADDRESS_TYPE, sizeof(XOR_MAPPED_ADDRESS_TYPE)) == 0) {
        
        const int NUM_BYTES_STUN_ATTR_TYPE_AND_LENGTH = 4;
        const int NUM_BYTES_FAMILY_ALIGN = 1;
        const uint8_t IPV4_FAMILY_NETWORK_ORDER = htons(0x01) >> 8;
        
        int byteIndex = NUM_BYTES_STUN_HEADER + NUM_BYTES_STUN_ATTR_TYPE_AND_LENGTH + NUM_BYTES_FAMILY_ALIGN;
        
        uint8_t addressFamily = 0;
        memcpy(&addressFamily, packetData + byteIndex, sizeof(addressFamily));
        
        byteIndex += sizeof(addressFamily);
        
        if (addressFamily == IPV4_FAMILY_NETWORK_ORDER) {
            // grab the X-Port
            uint16_t xorMappedPort = 0;
            memcpy(&xorMappedPort, packetData + byteIndex, sizeof(xorMappedPort));
            
            _publicPort = ntohs(xorMappedPort) ^ (ntohl(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER) >> 16);
            
            byteIndex += sizeof(xorMappedPort);
            
            // grab the X-Address
            uint32_t xorMappedAddress = 0;
            memcpy(&xorMappedAddress, packetData + byteIndex, sizeof(xorMappedAddress));
            
            uint32_t stunAddress = ntohl(xorMappedAddress) ^ ntohl(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER);
            _publicAddress = QHostAddress(stunAddress);
            
            qDebug("Public socket received from STUN server is %s:%hu\n",
                   _publicAddress.toString().toLocal8Bit().constData(),
                   _publicPort);
        }
    }
}

void NodeList::sendDomainServerCheckIn(const char* assignmentUUID) {
    static bool printedDomainServerIP = false;
    
    //  Lookup the IP address of the domain server if we need to
    if (_domainIP.isNull()) {
        qDebug("Looking up DS hostname %s.\n", _domainHostname.toLocal8Bit().constData());
        
        QHostInfo domainServerHostInfo = QHostInfo::fromName(_domainHostname);
        
        for (int i = 0; i < domainServerHostInfo.addresses().size(); i++) {
            if (domainServerHostInfo.addresses()[i].protocol() == QAbstractSocket::IPv4Protocol) {
                _domainIP = domainServerHostInfo.addresses()[i];
                
                qDebug("DS at %s is at %s\n", _domainHostname.toLocal8Bit().constData(),
                       _domainIP.toString().toLocal8Bit().constData());
                
                printedDomainServerIP = true;
                
                break;
            }
            
            // if we got here without a break out of the for loop then we failed to lookup the address
            if (i == domainServerHostInfo.addresses().size() - 1) {
                qDebug("Failed domain server lookup\n");
            }
        }
    } else if (!printedDomainServerIP) {
        qDebug("Domain Server IP: %s\n", _domainIP.toString().toLocal8Bit().constData());
        printedDomainServerIP = true;
    }
    
    if (_publicAddress.isNull()) {
        // we don't know our public socket and we need to send it to the domain server
        // send a STUN request to figure it out
        sendSTUNRequest();
    } else {
        // construct the DS check in packet if we need to
        if (!_checkInPacket) {
            int numBytesNodesOfInterest = _nodeTypesOfInterest ? strlen((char*) _nodeTypesOfInterest) : 0;
            
            const int IP_ADDRESS_BYTES = 4;
            
            // check in packet has header, optional UUID, node type, port, IP, node types of interest, null termination
            int numPacketBytes = sizeof(PACKET_TYPE) + sizeof(PACKET_VERSION) + sizeof(NODE_TYPE) +
                NUM_BYTES_RFC4122_UUID + (2 * (sizeof(uint16_t) + IP_ADDRESS_BYTES)) +
                numBytesNodesOfInterest + sizeof(unsigned char);
            
            _checkInPacket = new unsigned char[numPacketBytes];
            unsigned char* packetPosition = _checkInPacket;
            
            PACKET_TYPE nodePacketType = (memchr(SOLO_NODE_TYPES, _ownerType, sizeof(SOLO_NODE_TYPES)))
                ? PACKET_TYPE_DOMAIN_REPORT_FOR_DUTY
                : PACKET_TYPE_DOMAIN_LIST_REQUEST;
            
            packetPosition += populateTypeAndVersion(packetPosition, nodePacketType);
            
            *(packetPosition++) = _ownerType;
            
            if (!assignmentUUID) {
                // if we don't have an assignment UUID just send the null one
                assignmentUUID = QUuid().toRfc4122().constData();
            }
            
            // send our assignment UUID or the null one
            memcpy(packetPosition, assignmentUUID, NUM_BYTES_RFC4122_UUID);
            packetPosition += NUM_BYTES_RFC4122_UUID;
            
            // pack our public address to send to domain-server
            packetPosition += packSocket(_checkInPacket + (packetPosition - _checkInPacket),
                                         htonl(_publicAddress.toIPv4Address()), htons(_publicPort));
            
            // pack our local address to send to domain-server
            packetPosition += packSocket(_checkInPacket + (packetPosition - _checkInPacket),
                                         getLocalAddress(),
                                         htons(_nodeSocket.getListeningPort()));
            
            // add the number of bytes for node types of interest
            *(packetPosition++) = numBytesNodesOfInterest;
            
            // copy over the bytes for node types of interest, if required
            if (numBytesNodesOfInterest > 0) {
                memcpy(packetPosition,
                       _nodeTypesOfInterest,
                       numBytesNodesOfInterest);
                packetPosition += numBytesNodesOfInterest;
            }
            
            _numBytesCheckInPacket = packetPosition - _checkInPacket;
        }
        
        _nodeSocket.send(_domainIP.toString().toLocal8Bit().constData(), _domainPort, _checkInPacket, _numBytesCheckInPacket);
        
        // increment the count of un-replied check-ins
        _numNoReplyDomainCheckIns++;
    }
}

int NodeList::processDomainServerList(unsigned char* packetData, size_t dataBytes) {
    // this is a packet from the domain server, reset the count of un-replied check-ins
    _numNoReplyDomainCheckIns = 0;
    
    int readNodes = 0;

    char nodeType;
    uint16_t nodeId;
    
    // assumes only IPv4 addresses
    sockaddr_in nodePublicSocket;
    nodePublicSocket.sin_family = AF_INET;
    sockaddr_in nodeLocalSocket;
    nodeLocalSocket.sin_family = AF_INET;
    
    unsigned char* readPtr = packetData + numBytesForPacketHeader(packetData);
    unsigned char* startPtr = packetData;
    
    while((readPtr - startPtr) < dataBytes - sizeof(uint16_t)) {
        nodeType = *readPtr++;
        readPtr += unpackNodeId(readPtr, (uint16_t*) &nodeId);
        readPtr += unpackSocket(readPtr, (sockaddr*) &nodePublicSocket);
        readPtr += unpackSocket(readPtr, (sockaddr*) &nodeLocalSocket);
        
        // if the public socket address is 0 then it's reachable at the same IP
        // as the domain server
        if (nodePublicSocket.sin_addr.s_addr == 0) {
            nodePublicSocket.sin_addr.s_addr = htonl(_domainIP.toIPv4Address());
        }
        
        addOrUpdateNode((sockaddr*) &nodePublicSocket, (sockaddr*) &nodeLocalSocket, nodeType, nodeId);
    }
    
    // read out our ID from the packet
    unpackNodeId(readPtr, &_ownerID);

    return readNodes;
}

const sockaddr_in DEFAULT_LOCAL_ASSIGNMENT_SOCKET = socketForHostnameAndHostOrderPort(LOCAL_ASSIGNMENT_SERVER_HOSTNAME,
                                                                                      DEFAULT_DOMAIN_SERVER_PORT);
void NodeList::sendAssignment(Assignment& assignment) {
    unsigned char assignmentPacket[MAX_PACKET_SIZE];
    
    PACKET_TYPE assignmentPacketType = assignment.getCommand() == Assignment::CreateCommand
        ? PACKET_TYPE_CREATE_ASSIGNMENT
        : PACKET_TYPE_REQUEST_ASSIGNMENT;
    
    int numHeaderBytes = populateTypeAndVersion(assignmentPacket, assignmentPacketType);
    int numAssignmentBytes = assignment.packToBuffer(assignmentPacket + numHeaderBytes);
    
    sockaddr* assignmentServerSocket = (_assignmentServerSocket == NULL)
        ? (sockaddr*) &DEFAULT_LOCAL_ASSIGNMENT_SOCKET
        : _assignmentServerSocket;
    
    _nodeSocket.send(assignmentServerSocket, assignmentPacket, numHeaderBytes + numAssignmentBytes);
}

Node* NodeList::addOrUpdateNode(sockaddr* publicSocket, sockaddr* localSocket, char nodeType, uint16_t nodeId) {
    NodeList::iterator node = end();
    
    if (publicSocket) {
        for (node = begin(); node != end(); node++) {
            if (node->matches(publicSocket, localSocket, nodeType)) {
                // we already have this node, stop checking
                break;
            }
        }
    }
    
    if (node == end()) {
        // we didn't have this node, so add them
        Node* newNode = new Node(publicSocket, localSocket, nodeType, nodeId);
        
        if (socketMatch(publicSocket, localSocket)) {
            // likely debugging scenario with two nodes on local network
            // set the node active right away
            newNode->activatePublicSocket();
        }
   
        if (newNode->getType() == NODE_TYPE_VOXEL_SERVER ||
            newNode->getType() == NODE_TYPE_AVATAR_MIXER ||
            newNode->getType() == NODE_TYPE_AUDIO_MIXER) {
            // this is currently the cheat we use to talk directly to our test servers on EC2
            // to be removed when we have a proper identification strategy
            newNode->activatePublicSocket();
        }
        
        addNodeToList(newNode);
        
        return newNode;
    } else {
        
        if (node->getType() == NODE_TYPE_AUDIO_MIXER ||
            node->getType() == NODE_TYPE_VOXEL_SERVER) {
            // until the Audio class also uses our nodeList, we need to update
            // the lastRecvTimeUsecs for the audio mixer so it doesn't get killed and re-added continously
            node->setLastHeardMicrostamp(usecTimestampNow());
        }
        
        // we had this node already, do nothing for now
        return &*node;
    }    
}

void NodeList::addNodeToList(Node* newNode) {
    // find the correct array to add this node to
    int bucketIndex = _numNodes / NODES_PER_BUCKET;
    
    if (!_nodeBuckets[bucketIndex]) {
        _nodeBuckets[bucketIndex] = new Node*[NODES_PER_BUCKET]();
    }
    
    _nodeBuckets[bucketIndex][_numNodes % NODES_PER_BUCKET] = newNode;
    
    ++_numNodes;
    
    qDebug() << "Added" << *newNode << "\n";
    
    notifyHooksOfAddedNode(newNode);
}

unsigned NodeList::broadcastToNodes(unsigned char* broadcastData, size_t dataBytes, const char* nodeTypes, int numNodeTypes) {
    unsigned n = 0;
    for(NodeList::iterator node = begin(); node != end(); node++) {
        // only send to the NodeTypes we are asked to send to.
        if (node->getActiveSocket() != NULL && memchr(nodeTypes, node->getType(), numNodeTypes)) {
            // we know which socket is good for this node, send there
            _nodeSocket.send(node->getActiveSocket(), broadcastData, dataBytes);
            ++n;
        }
    }
    return n;
}

void NodeList::handlePingReply(sockaddr *nodeAddress) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        // check both the public and local addresses for each node to see if we find a match
        // prioritize the private address so that we prune erroneous local matches
        if (socketMatch(node->getPublicSocket(), nodeAddress)) {
            node->activatePublicSocket();
            break;
        } else if (socketMatch(node->getLocalSocket(), nodeAddress)) {
            node->activateLocalSocket();
            break;
        }
    }
}

Node* NodeList::soloNodeOfType(char nodeType) {
    if (memchr(SOLO_NODE_TYPES, nodeType, sizeof(SOLO_NODE_TYPES)) != NULL) {
        for(NodeList::iterator node = begin(); node != end(); node++) {
            if (node->getType() == nodeType) {
                return &(*node);
            }
        }
    }
    
    return NULL;
}

void* removeSilentNodes(void *args) {
    NodeList* nodeList = (NodeList*) args;
    uint64_t checkTimeUsecs = 0;
    int sleepTime = 0;
    
    while (!silentNodeThreadStopFlag) {
        
        checkTimeUsecs = usecTimestampNow();
        
        for(NodeList::iterator node = nodeList->begin(); node != nodeList->end(); ++node) {
            node->lock();
            
            if ((usecTimestampNow() - node->getLastHeardMicrostamp()) > NODE_SILENCE_THRESHOLD_USECS) {

                qDebug() << "Killed " << *node << "\n";
                
                nodeList->notifyHooksOfKilledNode(&*node);
                
                node->setAlive(false);
            }
            
            node->unlock();
        }
        
        sleepTime = NODE_SILENCE_THRESHOLD_USECS - (usecTimestampNow() - checkTimeUsecs);
        
        #ifdef _WIN32
        
        Sleep( static_cast<int>(1000.0f*sleepTime) );
        
        #else
        
        if (sleepTime > 0) {
            usleep(sleepTime);
        }
        
        #endif
    }
    
    pthread_exit(0);
    return NULL;
}

void NodeList::startSilentNodeRemovalThread() {
    pthread_create(&removeSilentNodesThread, NULL, removeSilentNodes, (void*) this);
}

void NodeList::stopSilentNodeRemovalThread() {
    silentNodeThreadStopFlag = true;
    pthread_join(removeSilentNodesThread, NULL);
    
}

const QString QSETTINGS_GROUP_NAME = "NodeList";
const QString DOMAIN_SERVER_SETTING_KEY = "domainServerHostname";

void NodeList::loadData(QSettings *settings) {
    settings->beginGroup(DOMAIN_SERVER_SETTING_KEY);
    
    QString domainServerHostname = settings->value(DOMAIN_SERVER_SETTING_KEY).toString();
    
    if (domainServerHostname.size() > 0) {
        _domainHostname = domainServerHostname;
        notifyDomainChanged();
    }
    
    settings->endGroup();
}

void NodeList::saveData(QSettings* settings) {
    settings->beginGroup(DOMAIN_SERVER_SETTING_KEY);
    
    if (_domainHostname != DEFAULT_DOMAIN_HOSTNAME) {
        // the user is using a different hostname, store it
        settings->setValue(DOMAIN_SERVER_SETTING_KEY, QVariant(_domainHostname));
    } else {
        // the user has switched back to default, remove the current setting
        settings->remove(DOMAIN_SERVER_SETTING_KEY);
    }
    
    settings->endGroup();
}

NodeList::iterator NodeList::begin() const {
    Node** nodeBucket = NULL;
    
    for (int i = 0; i < _numNodes; i++) {
        if (i % NODES_PER_BUCKET == 0) {
            nodeBucket =  _nodeBuckets[i / NODES_PER_BUCKET];
        }
        
        if (nodeBucket[i % NODES_PER_BUCKET]->isAlive()) {
            return NodeListIterator(this, i);
        }
    }
    
    // there's no alive node to start from - return the end
    return end();
}

NodeList::iterator NodeList::end() const {
    return NodeListIterator(this, _numNodes);
}

NodeListIterator::NodeListIterator(const NodeList* nodeList, int nodeIndex) :
    _nodeIndex(nodeIndex) {
    _nodeList = nodeList;
}

NodeListIterator& NodeListIterator::operator=(const NodeListIterator& otherValue) {
    _nodeList = otherValue._nodeList;
    _nodeIndex = otherValue._nodeIndex;
    return *this;
}

bool NodeListIterator::operator==(const NodeListIterator &otherValue) {
    return _nodeIndex == otherValue._nodeIndex;
}

bool NodeListIterator::operator!=(const NodeListIterator &otherValue) {
    return !(*this == otherValue);
}

Node& NodeListIterator::operator*() {
    Node** nodeBucket = _nodeList->_nodeBuckets[_nodeIndex / NODES_PER_BUCKET];
    return *nodeBucket[_nodeIndex % NODES_PER_BUCKET];
}

Node* NodeListIterator::operator->() {
    Node** nodeBucket = _nodeList->_nodeBuckets[_nodeIndex / NODES_PER_BUCKET];
    return nodeBucket[_nodeIndex % NODES_PER_BUCKET];
}

NodeListIterator& NodeListIterator::operator++() {
    skipDeadAndStopIncrement();
    return *this;
}

NodeList::iterator NodeListIterator::operator++(int) {
    NodeListIterator newIterator = NodeListIterator(*this);
    skipDeadAndStopIncrement();
    return newIterator;
}

void NodeListIterator::skipDeadAndStopIncrement() {
    while (_nodeIndex != _nodeList->_numNodes) {
        ++_nodeIndex;
        
        if (_nodeIndex == _nodeList->_numNodes) {
            break;
        } else if ((*(*this)).isAlive()) {
            // skip over the dead nodes
            break;
        }
    }
}

void NodeList::addDomainListener(DomainChangeListener* listener) {
    _domainListeners.push_back(listener);
    QString domain = _domainHostname.isEmpty() ? _domainIP.toString() : _domainHostname;
    listener->domainChanged(domain);
}

void NodeList::removeDomainListener(DomainChangeListener* listener) {
    for (int i = 0; i < _domainListeners.size(); i++) {
        if (_domainListeners[i] == listener) {
            _domainListeners.erase(_domainListeners.begin() + i);
            return;
        }
    }
}

void NodeList::addHook(NodeListHook* hook) {
    _hooks.push_back(hook);
}

void NodeList::removeHook(NodeListHook* hook) {
    for (int i = 0; i < _hooks.size(); i++) {
        if (_hooks[i] == hook) {
            _hooks.erase(_hooks.begin() + i);
            return;
        }
    }
}

void NodeList::notifyHooksOfAddedNode(Node* node) {
    for (int i = 0; i < _hooks.size(); i++) {
        //printf("NodeList::notifyHooksOfAddedNode() i=%d\n", i);
        _hooks[i]->nodeAdded(node);
    }
}

void NodeList::notifyHooksOfKilledNode(Node* node) {
    for (int i = 0; i < _hooks.size(); i++) {
        //printf("NodeList::notifyHooksOfKilledNode() i=%d\n", i);
        _hooks[i]->nodeKilled(node);
    }
}

void NodeList::notifyDomainChanged() {
    for (int i = 0; i < _domainListeners.size(); i++) {
        _domainListeners[i]->domainChanged(_domainHostname);
    }
}
