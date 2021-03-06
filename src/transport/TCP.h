/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file defines the CHIP Connection object that maintains TCP connections.
 *      It binds to any available local addr and port and begins listening.
 */

#ifndef __TCPTRANSPORT_H__
#define __TCPTRANSPORT_H__

#include <algorithm>
#include <utility>

#include <core/CHIPCore.h>
#include <inet/IPAddress.h>
#include <inet/IPEndPointBasis.h>
#include <inet/InetInterface.h>
#include <inet/TCPEndPoint.h>
#include <transport/Base.h>

namespace chip {
namespace Transport {

/** Defines listening parameters for setting up a TCP transport */
class TcpListenParameters
{
public:
    explicit TcpListenParameters(Inet::InetLayer * layer) : mLayer(layer) {}
    TcpListenParameters(const TcpListenParameters &) = default;
    TcpListenParameters(TcpListenParameters &&)      = default;

    Inet::InetLayer * GetInetLayer() { return mLayer; }

    Inet::IPAddressType GetAddressType() const { return mAddressType; }
    TcpListenParameters & SetAddressType(Inet::IPAddressType type)
    {
        mAddressType = type;

        return *this;
    }

    uint16_t GetListenPort() const { return mListenPort; }
    TcpListenParameters & SetListenPort(uint16_t port)
    {
        mListenPort = port;

        return *this;
    }

    InterfaceId GetInterfaceId() const { return mInterfaceId; }
    TcpListenParameters & SetInterfaceId(InterfaceId id)
    {
        mInterfaceId = id;

        return *this;
    }

private:
    Inet::InetLayer * mLayer         = nullptr;               ///< Associated inet layer
    Inet::IPAddressType mAddressType = kIPAddressType_IPv6;   ///< type of listening socket
    uint16_t mListenPort             = CHIP_PORT;             ///< TCP listen port
    InterfaceId mInterfaceId         = INET_NULL_INTERFACEID; ///< Interface to listen on
};

/**
 * Packets scheduled for sending once a connection has been established.
 */
struct PendingPacket
{
    PeerAddress peerAddress;             // where the packet is being sent to
    System::PacketBuffer * packetBuffer; // what data needs to be sent
};

/** Implements a transport using TCP. */
class DLL_EXPORT TCPBase : public Base
{
    /**
     *  The State of the TCP connection
     */
    enum class State
    {
        kNotReady    = 0, /**< State before initialization. */
        kInitialized = 1, /**< State after class is listening and ready. */
    };

public:
    TCPBase(Inet::TCPEndPoint ** activeConnectionsBuffer, size_t bufferSize, PendingPacket * packetBuffers,
            size_t packetsBuffersSize) :
        mActiveConnections(activeConnectionsBuffer),
        mActiveConnectionsSize(bufferSize), mPendingPackets(packetBuffers), mPendingPacketsSize(packetsBuffersSize)
    {
        PendingPacket emptyPending{
            .peerAddress  = PeerAddress::Uninitialized(),
            .packetBuffer = nullptr,
        };

        std::fill(mActiveConnections, mActiveConnections + mActiveConnectionsSize, nullptr);
        std::fill(mPendingPackets, mPendingPackets + mPendingPacketsSize, emptyPending);
    }
    virtual ~TCPBase();

    /**
     * Initialize a TCP transport on a given port.
     *
     * @param params        TCP configuration parameters for this transport
     *
     * @details
     *   Generally send and receive ports should be the same and equal to CHIP_PORT.
     *   The class allows separate definitions to allow local execution of several
     *   Nodes.
     */
    CHIP_ERROR Init(TcpListenParameters & params);

    CHIP_ERROR SendMessage(const MessageHeader & header, const PeerAddress & address, System::PacketBuffer * msgBuf) override;

    void Disconnect(const PeerAddress & address) override;

    bool CanSendToPeer(const PeerAddress & address) override
    {
        return (mState == State::kInitialized) && (address.GetTransportType() == Type::kTcp) &&
            (address.GetIPAddress().Type() == mEndpointType);
    }

    /**
     * Helper method to determine if IO processing is still required for a TCP transport
     * before everything is cleaned up (socket closing is async, so after calling 'Close' on
     * the transport, some time may be needed to actually be able to close.)
     */
    bool HasActiveConnections() const;

    /**
     * Close all active connections.
     */
    void CloseActiveConnections();

private:
    /**
     * Find an active connection to the given peer or return nullptr if
     * no active connection exists.
     */
    Inet::TCPEndPoint * FindActiveConnection(const PeerAddress & addr);

    /**
     * Sends the specified message once a connection has been established.
     *
     * @param addr - what peer to connect to
     * @param msg - what buffer to send once a connection has been established.
     *
     * Ownership of msg is taken over and will be freed at some unspecified time
     * in the future (once connection succeeds/fails).
     */
    CHIP_ERROR SendAfterConnect(const PeerAddress & addr, System::PacketBuffer * msg);

    /**
     * Process a single received buffer from the specified peer address.
     *
     * @param endPoint the source end point from which the data comes from
     * @param peerAddress the peer the data is coming from
     * @param buffer the actual data
     *
     * Ownership of buffer is taken over and will be freed (or re-enqueued to the endPoint receive queue)
     * as needed during processing.
     */
    CHIP_ERROR ProcessReceivedBuffer(Inet::TCPEndPoint * endPoint, const PeerAddress & peerAddress, System::PacketBuffer * buffer);

    /**
     * Process a single message of the specified size from a buffer.
     *
     * @param peerAddress the peer the data is coming from
     * @param buffer the buffer containing the message
     * @param messageSize how much of the head contains the actual message
     *
     * Does *not* free buffer.
     */
    CHIP_ERROR ProcessSingleMessageFromBufferHead(const PeerAddress & peerAddress, System::PacketBuffer * buffer,
                                                  uint16_t messageSize);

    // Callback handler for TCPEndPoint. TCP message receive handler.
    static void OnTcpReceive(Inet::TCPEndPoint * endPoint, System::PacketBuffer * buffer);

    // Callback handler for TCPEndPoint. Called when a connection has been completed.
    static void OnConnectionComplete(Inet::TCPEndPoint * endPoint, INET_ERROR err);

    // Callback handler for TCPEndPoint. Called when a connection has been closed.
    static void OnConnectionClosed(Inet::TCPEndPoint * endPoint, INET_ERROR err);

    // Callback handler for TCPEndPoint. Callend when a peer closes the connection.
    static void OnPeerClosed(Inet::TCPEndPoint * endPoint);

    // Callback handler for TCPEndPoint. Called when a connection is received on the listening port.
    static void OnConnectionReceived(Inet::TCPEndPoint * listenEndPoint, Inet::TCPEndPoint * endPoint,
                                     const IPAddress & peerAddress, uint16_t peerPort);

    // Called on accept error
    static void OnAcceptError(Inet::TCPEndPoint * endPoint, INET_ERROR err);

    Inet::TCPEndPoint * mListenSocket = nullptr;                                     ///< TCP socket used by the transport
    Inet::IPAddressType mEndpointType = Inet::IPAddressType::kIPAddressType_Unknown; ///< Socket listening type
    State mState                      = State::kNotReady;                            ///< State of the TCP transport

    // Number of active and 'pending connection' endpoints
    size_t mUsedEndPointCount = 0;

    // Currently active connections
    Inet::TCPEndPoint ** mActiveConnections;
    const size_t mActiveConnectionsSize;

    // Data to be sent when connections succeed
    PendingPacket * mPendingPackets;
    const size_t mPendingPacketsSize;
};

template <size_t kActiveConnectionsSize, size_t kPendingPacketSize>
class TCP : public TCPBase
{
public:
    TCP() : TCPBase(mConnectionsBuffer, kActiveConnectionsSize, mPendingPackets, kPendingPacketSize) {}

private:
    Inet::TCPEndPoint * mConnectionsBuffer[kActiveConnectionsSize];
    PendingPacket mPendingPackets[kPendingPacketSize];
};

} // namespace Transport
} // namespace chip

#endif // __TCPTRANSPORT_H__
