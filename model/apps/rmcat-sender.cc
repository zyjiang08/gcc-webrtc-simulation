/*******************************************************************************
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
 * Sender application implementation for rmcat ns3 module.
 *
 * @version 0.1.1
 * @author Jiantao Fu
 * @author Sergio Mena
 * @author Xiaoqing Zhu
 */

#include "rmcat-sender.h"
#include "rtp-header.h"
#include "ns3/dummy-controller.h"
#include "ns3/nada-controller.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/log.h"
#include "ns3/gcc-controller.h"

#include <sys/stat.h>

NS_LOG_COMPONENT_DEFINE ("RmcatSender");

namespace ns3 {

RmcatSender::RmcatSender ()
: m_destIP{}
, m_destPort{0}
, m_initBw{0}
, m_minBw{0}
, m_maxBw{0}
, m_paused{false}
, m_prev_feedback_time{0.}
, m_groupchanged{false}
, m_ssrc{0}
, m_sequence{0}
, m_first_seq{0}	// First sequence number.
, m_gid{0}		// Group id
, m_prev_seq{0}		// Seq number of previous feedback packet
, m_prev_group_seq{0}   // End Sequence number of previous feedback pkt.
, m_prev_group_time{0}
, m_prev_group_atime{0}	// Arrival Time of previous group.
, m_prev_time{0}
, m_rtpTsOffset{0}
, m_socket{NULL}
, m_enqueueEvent{}
, m_sendEvent{}
, m_sendOversleepEvent{}
, m_rVin{0.}
, m_rSend{0.}
, m_rateShapingBytes{0}
, m_nextSendTstmpUs{0}
{}

RmcatSender::~RmcatSender () {}

void RmcatSender::PauseResume (bool pause)
{
    NS_ASSERT (pause != m_paused);
    if (pause) {
        Simulator::Cancel (m_enqueueEvent);
        Simulator::Cancel (m_sendEvent);
        Simulator::Cancel (m_sendOversleepEvent);
        m_rateShapingBuf.clear ();
        m_rateShapingBytes = 0;
    } else {
        m_rVin = m_initBw;
        m_rSend = m_initBw;
        m_enqueueEvent = Simulator::ScheduleNow (&RmcatSender::EnqueuePacket, this);
        m_nextSendTstmpUs = 0;
    }
    m_paused = pause;
}

void RmcatSender::SetCodec (std::shared_ptr<syncodecs::Codec> codec)
{
    m_codec = codec;
}

// TODO (deferred): allow flexible input of video traffic trace path via config file, etc.
void RmcatSender::SetCodecType (SyncodecType codecType)
{
    syncodecs::Codec* codec = NULL;
    switch (codecType) {
        case SYNCODEC_TYPE_PERFECT:
        {
            codec = new syncodecs::PerfectCodec{DEFAULT_PACKET_SIZE};
            break;
        }
        case SYNCODEC_TYPE_FIXFPS:
        {
            const auto fps = SYNCODEC_DEFAULT_FPS;
            auto innerCodec = new syncodecs::SimpleFpsBasedCodec{fps};
            codec = new syncodecs::ShapedPacketizer{innerCodec, DEFAULT_PACKET_SIZE};
            break;
        }
        case SYNCODEC_TYPE_STATS:
        {
            const auto fps = SYNCODEC_DEFAULT_FPS;
            auto innerStCodec = new syncodecs::StatisticsCodec{fps};
            codec = new syncodecs::ShapedPacketizer{innerStCodec, DEFAULT_PACKET_SIZE};
            break;
        }
        case SYNCODEC_TYPE_TRACE:
        case SYNCODEC_TYPE_HYBRID:
        {
            const std::vector<std::string> candidatePaths = {
                ".",      // If run from top directory (e.g., with gdb), from ns-3.26/
                "../",    // If run from with test_new.py with designated directory, from ns-3.26/2017-xyz/
                "../..",  // If run with test.py, from ns-3.26/testpy-output/201...
            };

            const std::string traceSubDir{"src/ns3-rmcat/model/syncodecs/video_traces/chat_firefox_h264"};
            std::string traceDir{};

            for (auto c : candidatePaths) {
                std::ostringstream currPathOss;
                currPathOss << c << "/" << traceSubDir;
                struct stat buffer;
                if (::stat (currPathOss.str ().c_str (), &buffer) == 0) {
                    //filename exists
                    traceDir = currPathOss.str ();
                    break;
                }
            }

            NS_ASSERT_MSG (!traceDir.empty (), "Traces file not found in candidate paths");

            auto filePrefix = "chat";
            auto innerCodec = (codecType == SYNCODEC_TYPE_TRACE) ?
                                 new syncodecs::TraceBasedCodecWithScaling{
                                    traceDir,        // path to traces directory
                                    filePrefix,      // video filename
                                    SYNCODEC_DEFAULT_FPS,             // Default FPS: 30fps
                                    true} :          // fixed mode: image resolution doesn't change
                                 new syncodecs::HybridCodec{
                                    traceDir,        // path to traces directory
                                    filePrefix,      // video filename
                                    SYNCODEC_DEFAULT_FPS,             // Default FPS: 30fps
                                    true};           // fixed mode: image resolution doesn't change

            codec = new syncodecs::ShapedPacketizer{innerCodec, DEFAULT_PACKET_SIZE};
            break;
        }
        case SYNCODEC_TYPE_SHARING:
        {
            auto innerShCodec = new syncodecs::SimpleContentSharingCodec{};
            codec = new syncodecs::ShapedPacketizer{innerShCodec, DEFAULT_PACKET_SIZE};
            break;
        }
        default:  // defaults to perfect codec
            codec = new syncodecs::PerfectCodec{DEFAULT_PACKET_SIZE};
    }

    // update member variable
    m_codec = std::shared_ptr<syncodecs::Codec>{codec};
}

void RmcatSender::SetController (std::shared_ptr<rmcat::SenderBasedController> controller)
{
    m_controller = controller;
}

void RmcatSender::Setup (Ipv4Address destIP,
                         uint16_t destPort)
{
    if (!m_codec) {
        m_codec = std::make_shared<syncodecs::PerfectCodec> (DEFAULT_PACKET_SIZE);
    }

    if (!m_controller) {
        m_controller = std::make_shared<rmcat::DummyController> ();
    } else {
        m_controller->reset ();
    }

    m_destIP = destIP;
    m_destPort = destPort;
}

void RmcatSender::SetRinit (float r)
{
    m_initBw = r;
    if (m_controller) m_controller->setInitBw (m_initBw);
}

void RmcatSender::SetRmin (float r)
{
    m_minBw = r;
    if (m_controller) m_controller->setMinBw (m_minBw);
}

void RmcatSender::SetRmax (float r)
{
    m_maxBw = r;
    if (m_controller) m_controller->setMaxBw (m_maxBw);
}

void RmcatSender::StartApplication ()
{
    m_ssrc = rand ();
    // RTP initial values for sequence number and timestamp SHOULD be random (RFC 3550)
    m_sequence = rand ();
    m_first_seq = m_sequence;
    m_rtpTsOffset = rand ();

    NS_ASSERT (m_minBw <= m_initBw);
    NS_ASSERT (m_initBw <= m_maxBw);

    m_rVin = m_initBw;
    m_rSend = m_initBw;

    if (m_socket == NULL) {
        m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
        auto res = m_socket->Bind ();
        NS_ASSERT (res == 0);
    }
    m_socket->SetRecvCallback (MakeCallback (&RmcatSender::RecvPacket, this));

    m_enqueueEvent = Simulator::Schedule (Seconds (0.0), &RmcatSender::EnqueuePacket, this);
    m_nextSendTstmpUs = 0;
}

void RmcatSender::StopApplication ()
{
    Simulator::Cancel (m_enqueueEvent);
    Simulator::Cancel (m_sendEvent);
    Simulator::Cancel (m_sendOversleepEvent);
    m_rateShapingBuf.clear ();
    m_rateShapingBytes = 0;
}

void RmcatSender::EnqueuePacket ()
{
    syncodecs::Codec& codec = *m_codec;
    codec.setTargetRate (m_rVin);	// Media rate.
    ++codec; // Advance codec/packetizer to next frame/packet
    const auto bytesToSend = codec->first.size ();
    NS_ASSERT (bytesToSend > 0);
    NS_ASSERT (bytesToSend <= DEFAULT_PACKET_SIZE);

    m_rateShapingBuf.push_back (bytesToSend);
    m_rateShapingBytes += bytesToSend;

    NS_LOG_INFO ("RmcatSender::EnqueuePacket, packet enqueued, packet length: " << bytesToSend
                 << ", buffer size: " << m_rateShapingBuf.size ()
                 << ", buffer bytes: " << m_rateShapingBytes);

    double secsToNextEnqPacket = codec->second;
    Time tNext{Seconds (secsToNextEnqPacket)};
    m_enqueueEvent = Simulator::Schedule (tNext, &RmcatSender::EnqueuePacket, this);

    if (!USE_BUFFER) {
        m_sendEvent = Simulator::ScheduleNow (&RmcatSender::SendPacket, this,
                                              secsToNextEnqPacket * 1000. * 1000.);
        return;
    }

    if (m_rateShapingBuf.size () == 1) {
        // Buffer was empty
        const uint64_t nowUs = Simulator::Now ().GetMicroSeconds ();
        const uint64_t usToNextSentPacket = nowUs < m_nextSendTstmpUs ?
                                                    m_nextSendTstmpUs - nowUs : 0;
        NS_LOG_INFO ("(Re-)starting the send timer: nowUs " << nowUs
                     << ", bytesToSend " << bytesToSend
                     << ", usToNextSentPacket " << usToNextSentPacket
                     << ", m_rSend " << m_rSend
                     << ", m_rVin " << m_rVin
                     << ", secsToNextEnqPacket " << secsToNextEnqPacket);

        Time tNext{MicroSeconds (usToNextSentPacket)};
        m_sendEvent = Simulator::Schedule (tNext, &RmcatSender::SendPacket, this, usToNextSentPacket);
    }
}

void RmcatSender::SendPacket (uint64_t usSlept)
{
    NS_ASSERT (m_rateShapingBuf.size () > 0);
    NS_ASSERT (m_rateShapingBytes < MAX_QUEUE_SIZE_SANITY);

    const auto bytesToSend = m_rateShapingBuf.front ();
    NS_ASSERT (bytesToSend > 0);
    NS_ASSERT (bytesToSend <= DEFAULT_PACKET_SIZE);
    m_rateShapingBuf.pop_front ();
    NS_ASSERT (m_rateShapingBytes >= bytesToSend);
    m_rateShapingBytes -= bytesToSend;

    NS_LOG_INFO ("RmcatSender::SendPacket, packet dequeued, packet length: " << bytesToSend
                 << ", buffer size: " << m_rateShapingBuf.size ()
                 << ", buffer bytes: " << m_rateShapingBytes);

    // Synthetic oversleep: random uniform [0% .. 1%]
    uint64_t oversleepUs = usSlept * (rand () % 100) / 10000;
    Time tOver{MicroSeconds (oversleepUs)};
    m_sendOversleepEvent = Simulator::Schedule (tOver, &RmcatSender::SendOverSleep,
                                                this, bytesToSend);

    // schedule next sendData
    const double usToNextSentPacketD = double (bytesToSend) * 8. * 1000. * 1000. / m_rSend;
    const uint64_t usToNextSentPacket = uint64_t (usToNextSentPacketD);

    if (!USE_BUFFER || m_rateShapingBuf.size () == 0) {
        // Buffer became empty
        const auto nowUs = Simulator::Now ().GetMicroSeconds ();
        m_nextSendTstmpUs = nowUs + usToNextSentPacket;
        return;
    }

    Time tNext{MicroSeconds (usToNextSentPacket)};
    m_sendEvent = Simulator::Schedule (tNext, &RmcatSender::SendPacket, this, usToNextSentPacket);
}

void RmcatSender::SendOverSleep (uint32_t bytesToSend) {
    const auto nowUs = Simulator::Now ().GetMicroSeconds ();

    m_controller->processSendPacket (nowUs, m_sequence, bytesToSend);

    ns3::RtpHeader header{96}; // 96: dynamic payload type, according to RFC 3551
    header.SetSequence (m_sequence++);
    NS_ASSERT (nowUs >= 0);
    // Most video payload types in RFC 3551, Table 5, use a 90 KHz clock
    // Therefore, assuming 90 KHz clock for RTP timestamps
    header.SetTimestamp (m_rtpTsOffset + uint32_t (nowUs * 90 / 1000));;
    header.SetSsrc (m_ssrc);

    auto packet = Create<Packet> (bytesToSend);
    packet->AddHeader (header);

    NS_LOG_INFO ("RmcatSender::SendOverSleep, " << packet->ToString ());
    m_socket->SendTo (packet, 0, InetSocketAddress{m_destIP, m_destPort});
}

void RmcatSender::RecvPacket (Ptr<Socket> socket)
{
    Address remoteAddr;
    auto Packet = m_socket->RecvFrom (remoteAddr);
    NS_ASSERT (Packet);
    
    auto rIPAddress = InetSocketAddress::ConvertFrom (remoteAddr).GetIpv4 ();
    auto rport = InetSocketAddress::ConvertFrom (remoteAddr).GetPort ();
    NS_ASSERT (rIPAddress == m_destIP);
    NS_ASSERT (rport == m_destPort);

    // get the feedback header
    const uint64_t nowUs = Simulator::Now ().GetMicroSeconds ();
    CCFeedbackHeader header{};
    NS_LOG_INFO ("RmcatSender::RecvPacket, " << Packet->ToString ());
    Packet->RemoveHeader (header);
    std::set<uint32_t> ssrcList{};
    header.GetSsrcList (ssrcList);
    if (ssrcList.count (m_ssrc) == 0) {
        NS_LOG_INFO ("RmcatSender::Received Feedback packet with no data for SSRC " << m_ssrc);
        CalcBufferParams (nowUs);
        return;
    }
    std::vector<std::pair<uint16_t,
                          CCFeedbackHeader::MetricBlock> > feedback{};
    const bool res = header.GetMetricList (m_ssrc, feedback);
    NS_ASSERT (res);
    for (auto& item : feedback) {
        const auto sequence = item.first;
        const auto timestampUs = item.second.m_timestampUs;
	const bool is_group_changed = false;
        if(sequence == m_first_seq){
            gid = 0;
            m_prev_group_time = timestampUs;        
        }
        // 5000micro seconds = BURST_TIME
        if((timestampUs - m_prev_group_time) >= 5000){
            // Group changed. Calculate Inter-Arrival Time and Inter-Departure Time.
            if(gid == 0){
		// First Group
		m_prev_group_seq = m_prev_seq;
		m_prev_group_atime = m_prev_time;
		m_prev_group_time = timestampUs;
		gid += 1;
	    }
	    else{
		// Else
		// Calculate inter variables
		const auto l_inter_arrival = m_prev_time - m_prev_group_atime;
		// Prefiltering (If inter arrival time is less than BURST_TIME, it is part of current working packet group.)
		if(l_inter_arrival > 5000){
		    const auto l_inter_departure = m_controller->UpdateDepartureTime(m_prev_group_seq, m_prev_seq);  	
	            	
  	            // Calculate Inter Delay Variation
        	    const auto l_inter_delay_var = l_inter_arrival - l_inter_departure;
                    // Prefiltering (If inter delay variation is less than 0, it is part of current working packet group.)
		    if(l_inter_delay_var >= 0){
		        gid += 1;
		        m_prev_group_time = timestampUs;

		        m_prev_group_atime = m_prev_time;
		        m_prev_group_seq = m_prev_time;
			is_group_changed = true;
		    }
		    else{
		        is_group_changed = false;
		    }
		}
		else{
		    is_group_changed = false;
		}
	    }
        }
	
	// Increment...
	m_prev_time = timestampUs;
	m_prev_seq = sequence;
        

        const auto ecn = item.second.m_ecn;
        NS_ASSERT (timestampUs <= nowUs);

	// TODO SHOULD MODIFY. UNCLEAR : Does processFeedback only call when group is changed?
	if(is_group_chaged){
            m_controller->processFeedback (nowUs, sequence, timestampUs, ecn, l_inter_arrival, l_inter_departure, l_inter_delay_var);
        }
    }
    // TODO MAYBE THIS PART IS NOT NEEDED.
    CalcBufferParams (nowUs);
}

void RmcatSender::CalcBufferParams (uint64_t nowUs)
{
    //Calculate rate shaping buffer parameters
    const auto r_ref = m_controller->getBandwidth (nowUs); // bandwidth in bps
    float bufferLen;
    //Purpose: smooth out timing issues between send and receive
    // feedback for the common case: buffer oscillating between 0 and 1 packets
    if (m_rateShapingBuf.size () > 1) {
        bufferLen = static_cast<float> (m_rateShapingBytes);
    } else {
        bufferLen = 0;
    }

    syncodecs::Codec& codec = *m_codec;

    // TODO (deferred): encapsulate rate shaping buffer in a separate class
    if (USE_BUFFER && static_cast<bool> (codec)) {
        const float fps = 1. / static_cast<float>  (codec->second);
        m_rVin = std::max<float> (m_minBw, r_ref - BETA_V * 8. * bufferLen * fps);
        m_rSend = r_ref + BETA_S * 8. * bufferLen * fps;
        NS_LOG_INFO ("New rate shaping buffer parameters: r_ref " << r_ref
                     << ", m_rVin " << m_rVin
                     << ", m_rSend " << m_rSend
                     << ", fps " << fps
                     << ", buffer length " << bufferLen);
    } else {
        m_rVin = r_ref;
        m_rSend = r_ref;
    }
}

}
