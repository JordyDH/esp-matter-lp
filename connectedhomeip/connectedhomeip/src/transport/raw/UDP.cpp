/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2013-2017 Nest Labs, Inc.
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
 *      This file implements the CHIP Connection object that maintains a UDP connection.
 */
#include <transport/raw/UDP.h>

#include <lib/support/CHIPFaultInjection.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <transport/raw/MessageHeader.h>
#include <transport/raw/ConnectionManager.h>

#include <inttypes.h>

namespace chip {
namespace Transport {

UDP::~UDP()
{
    Close();
}

CHIP_ERROR UDP::Init(UdpListenParameters & params)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    if (mState != State::kNotReady)
    {
        Close();
    }

    err = params.GetEndPointManager()->NewEndPoint(&mUDPEndPoint);
    SuccessOrExit(err);

    mUDPEndPoint->SetNativeParams(params.GetNativeParams());

    ChipLogDetail(Inet, "UDP::Init bind&listen port=%d", params.GetListenPort());

    err = mUDPEndPoint->Bind(params.GetAddressType(), Inet::IPAddress::Any, params.GetListenPort(), params.GetInterfaceId());
    SuccessOrExit(err);

    err = mUDPEndPoint->Listen(OnUdpReceive, OnUdpError, this);
    SuccessOrExit(err);

    mUDPEndpointType = params.GetAddressType();

    mState = State::kInitialized;

    ChipLogDetail(Inet, "UDP::Init bound to port=%d", mUDPEndPoint->GetBoundPort());

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(Inet, "Failed to initialize Udp transport: %" CHIP_ERROR_FORMAT, err.Format());
        if (mUDPEndPoint)
        {
            mUDPEndPoint->Free();
            mUDPEndPoint = nullptr;
        }
    }

    return err;
}

uint16_t UDP::GetBoundPort()
{
    VerifyOrDie(mUDPEndPoint != nullptr);
    return mUDPEndPoint->GetBoundPort();
}

void UDP::Close()
{
    if (mUDPEndPoint)
    {
        // Udp endpoint is only non null if udp endpoint is initialized and listening
        mUDPEndPoint->Close();
        mUDPEndPoint->Free();
        mUDPEndPoint = nullptr;
    }
    mState = State::kNotReady;
}

CHIP_ERROR UDP::SendMessage(const Transport::PeerAddress & address, System::PacketBufferHandle && msgBuf)
{
    VerifyOrReturnError(address.GetTransportType() == Type::kUdp, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(mState == State::kInitialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mUDPEndPoint != nullptr, CHIP_ERROR_INCORRECT_STATE);

    Inet::IPPacketInfo addrInfo;
    addrInfo.Clear();

    addrInfo.DestAddress = address.GetIPAddress();
    addrInfo.DestPort    = address.GetPort();
    addrInfo.Interface   = address.GetInterface();

    // Drop the message and return. Free the buffer.
    CHIP_FAULT_INJECT(FaultInjection::kFault_DropOutgoingUDPMsg, msgBuf = nullptr; return CHIP_ERROR_CONNECTION_ABORTED;);

    return mUDPEndPoint->SendMsg(&addrInfo, std::move(msgBuf));
}

// Helper struct to pass data to the timer callback
struct UDPDelayedMessageInfo
{
    UDPDelayedMessageInfo(UDP * udp, const PeerAddress & peerAddress, System::PacketBufferHandle && buffer) :
        mUDP(udp), mPeerAddress(peerAddress), mBuffer(std::move(buffer))
    {}

    UDP * mUDP;
    PeerAddress mPeerAddress;
    System::PacketBufferHandle mBuffer;
};

// Static callback function for the timer
static void HandleDelayedMessage(System::Layer * layer, void * appState)
{
    UDPDelayedMessageInfo * info = static_cast<UDPDelayedMessageInfo *>(appState);
    UDP * udp = info->mUDP;
    
    udp->ProcessReceivedMessage(info->mPeerAddress, std::move(info->mBuffer));
    delete info;
}

void UDP::ProcessReceivedMessage(const PeerAddress & peerAddress, System::PacketBufferHandle && buffer)
{
    // This is a public method that can access the protected HandleMessageReceived method
    HandleMessageReceived(peerAddress, std::move(buffer));
}

void UDP::OnUdpReceive(Inet::UDPEndPoint * endPoint, System::PacketBufferHandle && buffer, const Inet::IPPacketInfo * pktInfo)
{
    CHIP_ERROR err          = CHIP_NO_ERROR;
    UDP * udp               = reinterpret_cast<UDP *>(endPoint->mAppState);
    PeerAddress peerAddress = PeerAddress::UDP(pktInfo->SrcAddress, pktInfo->SrcPort, pktInfo->Interface);

    CHIP_FAULT_INJECT(FaultInjection::kFault_DropIncomingUDPMsg, buffer = nullptr; return;);

    // Check if BLE connection is active
    bool bleConnectionActive = ConnectionManager::GetInstance().IsBLEConnectionActive();
    
    // Define delay times based on BLE connection status
    uint32_t kMessageProcessingDelayMs;
    
    if (bleConnectionActive)
    {
        // Longer delay when BLE is active to reduce power consumption
        kMessageProcessingDelayMs = 500; // Adjust value as needed
        ChipLogProgress(Inet, "UDP: BLE connection active - Adding delay before processing message");
    }
    else
    {
        // Shorter delay when no BLE connection
        kMessageProcessingDelayMs = 100; // Adjust value as needed
        ChipLogProgress(Inet, "UDP: No BLE connection - Using shorter processing delay");
    }

    // Create info for the delayed processing
    UDPDelayedMessageInfo * messageInfo = new UDPDelayedMessageInfo(udp, peerAddress, std::move(buffer));
    
    // Check if memory allocation failed
    if (messageInfo == nullptr)
    {
        ChipLogError(Inet, "Failed to allocate memory for delayed message processing");
        udp->ProcessReceivedMessage(peerAddress, std::move(buffer));
        return;
    }

    // Start timer with the static callback
    err = DeviceLayer::SystemLayer().StartTimer(
        System::Clock::Milliseconds32(kMessageProcessingDelayMs),
        HandleDelayedMessage,
        messageInfo);

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Inet, "Failed to start timer: %" CHIP_ERROR_FORMAT, err.Format());
        udp->ProcessReceivedMessage(messageInfo->mPeerAddress, std::move(messageInfo->mBuffer));
        delete messageInfo;
    }

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Inet, "Failed to receive UDP message: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void UDP::OnUdpError(Inet::UDPEndPoint * endPoint, CHIP_ERROR err, const Inet::IPPacketInfo * pktInfo)
{
    ChipLogError(Inet, "Failed to receive UDP message: %" CHIP_ERROR_FORMAT, err.Format());
}

CHIP_ERROR UDP::MulticastGroupJoinLeave(const Transport::PeerAddress & address, bool join)
{
    char addressStr[Transport::PeerAddress::kMaxToStringSize];
    address.ToString(addressStr, Transport::PeerAddress::kMaxToStringSize);

    if (join)
    {
        ChipLogProgress(Inet, "Joining Multicast Group with address %s", addressStr);
        return mUDPEndPoint->JoinMulticastGroup(mUDPEndPoint->GetBoundInterface(), address.GetIPAddress());
    }

    ChipLogProgress(Inet, "Leaving Multicast Group with address %s", addressStr);
    return mUDPEndPoint->LeaveMulticastGroup(mUDPEndPoint->GetBoundInterface(), address.GetIPAddress());
}

} // namespace Transport
} // namespace chip
