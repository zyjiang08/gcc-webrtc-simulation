/******************************************************************************
 * Copyright 2016-2017 Cisco Systems, Inc.                                    *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License");            *
 * you may not use this file except in compliance with the License.           *
 *                                                                            *
 * You may obtain a copy of the License at                                    *
 *                                                                            *
 *     http://www.apache.org/licenses/LICENSE-2.0                             *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 ******************************************************************************/

/**
 * @file
 * Sender application interface for rmcat ns3 module.
 *
 * @version 0.1.1
 * @author Jiantao Fu
 * @author Sergio Mena
 * @author Xiaoqing Zhu
 */

#ifndef RMCAT_SENDER_H
#define RMCAT_SENDER_H

#include "rmcat-constants.h"
#include "ns3/syncodecs.h"
#include "ns3/sender-based-controller.h"
#include "ns3/socket.h"
#include "ns3/application.h"
#include <memory>

namespace ns3 {

class RmcatSender: public Application
{
public:

    RmcatSender ();
    virtual ~RmcatSender ();

    void PauseResume (bool pause);

    void SetCodec (std::shared_ptr<syncodecs::Codec> codec);
    void SetCodecType (SyncodecType codecType);

    void SetController (std::shared_ptr<rmcat::SenderBasedController> controller);

    void SetRinit (float Rinit);
    void SetRmin (float Rmin);
    void SetRmax (float Rmax);

    void Setup (Ipv4Address dest_ip, uint16_t dest_port);

private:
    virtual void StartApplication ();
    virtual void StopApplication ();

    void EnqueuePacket ();
    void SendPacket (uint64_t usSlept);
    void SendOverSleep (uint32_t bytesToSend);
    void RecvPacket (Ptr<Socket> socket);
    void CalcBufferParams (uint64_t nowUs);

private:
    std::shared_ptr<syncodecs::Codec> m_codec;
    std::shared_ptr<rmcat::SenderBasedController> m_controller;
    Ipv4Address m_destIP;
    uint16_t m_destPort;
    float m_initBw;
    float m_minBw;
    float m_maxBw;
    bool m_paused;
    uint32_t m_ssrc;
    uint16_t m_sequence;
    uint16_t m_first_seq;
    uint32_t m_gid;
    uint32_t m_prev_seq;		// Sequence number of previous feedback pkt
    uint64_t m_prev_time;	        // Timestmp of previous feedback pkt
    uint64_t m_prev_group_time;
    uint64_t m_prev_group_atime;    // Arrival time of previous group
    uint32_t m_prev_group_seq;	// End Sequnce number of previous feedback pkt
    uint32_t m_rtpTsOffset;
    uint64_t m_prev_feedback_time;
    bool m_groupchanged;
    Ptr<Socket> m_socket;
    EventId m_enqueueEvent;
    EventId m_sendEvent;
    EventId m_sendOversleepEvent;

    double m_rVin; //bps
    double m_rSend; //bps
    double m_rBitrate;  // Target Bit Rate.
    std::deque<uint32_t> m_rateShapingBuf;
    std::deque<uint32_t> m_PacingQ;
    uint32_t m_rateShapingBytes;
    uint32_t m_PacingQBytes;
    uint64_t m_nextSendTstmpUs;
};

}

#endif /* RMCAT_SENDER_H */
