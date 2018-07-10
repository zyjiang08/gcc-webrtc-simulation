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

#include "gcc-sender.h"
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

NS_LOG_COMPONENT_DEFINE ("GccSender");

namespace ns3 {

GccSender::GccSender ()
: m_destIP{}
, m_destPort{0}
, m_initBw{0}
, m_minBw{0}
, m_maxBw{0}
, m_paused{false}
, m_ssrc{0}
, m_sequence{0}
, m_first_seq{0}	// First sequence number.
, m_gid{0}		// Group id
, m_prev_seq{0}		// Seq number of previous feedback packet
, m_prev_time{0}
, m_curr_group_time{0}
, m_prev_group_atime{0}	// Arrival Time of previous group.
, m_prev_group_seq{0}   // End Sequence number of previous feedback pkt.
, m_curr_group_start_seq{0}
, m_rtpTsOffset{0}
, m_prev_feedback_time{0.}
, m_groupchanged{false}
, m_socket{NULL}
, m_enqueueEvent{}
, m_sendEvent{}
, m_sendOversleepEvent{}
, m_rVin{0.}
, m_rSend{0.}
, m_rBitrate{0.}
, m_rateShapingBytes{0}
, m_PacingQBytes{0}
, m_nextSendTstmpUs{0}
, m_firstFeedback{true}
, m_group_size_inter{0}
, m_group_size{0}
{}

GccSender::~GccSender () {}

void GccSender::PauseResume (bool pause)
{
    NS_ASSERT (pause != m_paused);
    if (pause) {
        Simulator::Cancel (m_enqueueEvent);
        Simulator::Cancel (m_sendEvent);
        Simulator::Cancel (m_sendOversleepEvent);
        m_rateShapingBuf.clear ();
        m_rateShapingBytes = 0;

        m_PacingQ.clear();
        m_PacingQBytes = 0;
    } else {
        m_rBitrate = m_initBw;    
 
        m_rVin = m_initBw;
        m_rSend = m_initBw;
        m_enqueueEvent = Simulator::ScheduleNow (&GccSender::EnqueuePacket, this);
        m_nextSendTstmpUs = 0;
    }
    m_paused = pause;
}

void GccSender::SetCodec (std::shared_ptr<syncodecs::Codec> codec)
{
    m_codec = codec;
}

// TODO (deferred): allow flexible input of video traffic trace path via config file, etc.
void GccSender::SetCodecType (SyncodecType codecType)
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

void GccSender::SetController (std::shared_ptr<rmcat::SenderBasedController> controller)
{
    m_controller = controller;
}

void GccSender::Setup (Ipv4Address destIP,
                         uint16_t destPort)
{
    if (!m_codec) {
        m_codec = std::make_shared<syncodecs::PerfectCodec> (DEFAULT_PACKET_SIZE);
    }

    if (!m_controller) {
        // m_controller = std::make_shared<rmcat::GccController> ();
        m_controller = std::make_shared<rmcat::DummyController> ();  // TODO Controller.
    } else {
        m_controller->reset ();
    }

    m_destIP = destIP;
    m_destPort = destPort;
}

// Set Functions
void GccSender::SetRinit (float r)
{
    m_initBw = r;
    if (m_controller) m_controller->setInitBw (m_initBw);
}

void GccSender::SetRmin (float r)
{
    m_minBw = r;
    if (m_controller) m_controller->setMinBw (m_minBw);
}

void GccSender::SetRmax (float r)
{
    m_maxBw = r;
    if (m_controller) m_controller->setMaxBw (m_maxBw);
}

void GccSender::StartApplication ()
{
    m_ssrc = rand ();
    // RTP initial values for sequence number and timestamp SHOULD be random (RFC 3550)
    m_sequence = rand ();
    m_first_seq = m_sequence;
    m_rtpTsOffset = rand ();

    NS_ASSERT (m_minBw <= m_initBw);
    NS_ASSERT (m_initBw <= m_maxBw);
    
    m_rBitrate = m_initBw;

    m_rVin = m_initBw;
    m_rSend = m_initBw;

    if (m_socket == NULL) {
        m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
        auto res = m_socket->Bind ();
        NS_ASSERT (res == 0);
    }
    m_socket->SetRecvCallback (MakeCallback (&GccSender::RecvPacket, this));

    m_enqueueEvent = Simulator::Schedule (Seconds (0.0), &GccSender::EnqueuePacket, this);
    m_nextSendTstmpUs = 0;
}

void GccSender::StopApplication ()
{
    Simulator::Cancel (m_enqueueEvent);
    Simulator::Cancel (m_sendEvent);
    Simulator::Cancel (m_sendOversleepEvent);
    
    m_PacingQ.clear();
    m_PacingQBytes = 0;

    m_rateShapingBuf.clear ();
    m_rateShapingBytes = 0;
}

void GccSender::EnqueuePacket ()
{
    syncodecs::Codec& codec = *m_codec;
    codec.setTargetRate (m_rBitrate);	// Media rate.
    ++codec; // Advance codec/packetizer to next frame/packet
    const auto bytesToSend = codec->first.size ();
    NS_ASSERT (bytesToSend > 0);
    NS_ASSERT (bytesToSend <= DEFAULT_PACKET_SIZE);

    // Push into Pacing Queue Buffer.
    m_PacingQ.push_back (bytesToSend);
    m_PacingQBytes += bytesToSend;

//    m_rateShapingBuf.push_back (bytesToSend);
//    m_rateShapingBytes += bytesToSend;

    NS_LOG_INFO ("GccSender::EnqueuePacket, packet enqueued, packet length: " << bytesToSend
                 << ", buffer size: " << m_PacingQ.size ()
                 << ", buffer bytes: " << m_PacingQBytes);

    double secsToNextEnqPacket = codec->second;
    // std::cout << "secToNextEnqPacket:: " << secsToNextEnqPacket << "\n";

    Time tNext{Seconds (secsToNextEnqPacket)};
    m_enqueueEvent = Simulator::Schedule (tNext, &GccSender::EnqueuePacket, this);

    if (!USE_BUFFER) {
        m_sendEvent = Simulator::ScheduleNow (&GccSender::SendPacket, this,
                                              secsToNextEnqPacket * 1000. * 1000.);
        return;
    }

    if (m_PacingQ.size () == 1) {
        // Buffer was empty
        const uint64_t nowUs = Simulator::Now ().GetMicroSeconds ();
        const uint64_t usToNextSentPacket = nowUs < m_nextSendTstmpUs ?
                                                    m_nextSendTstmpUs - nowUs : 0;
        NS_LOG_INFO ("(Re-)starting the send timer: nowUs " << nowUs
                     << ", bytesToSend " << bytesToSend
                     << ", usToNextSentPacket " << usToNextSentPacket
                     << ", m_rBitrate " << m_rBitrate
                     << ", secsToNextEnqPacket " << secsToNextEnqPacket);
        
       //  std::cout << "usToNextSentPacket:: " << usToNextSentPacket << "\n";
        Time tNext{MicroSeconds (usToNextSentPacket)};
        m_sendEvent = Simulator::Schedule (tNext, &GccSender::SendPacket, this, usToNextSentPacket);
    }
}

void GccSender::SendPacket (uint64_t usSlept)
{
    NS_ASSERT (m_PacingQ.size () > 0);
    NS_ASSERT (m_PacingQBytes < MAX_QUEUE_SIZE_SANITY);

    const auto bytesToSend = m_PacingQ.front ();
    NS_ASSERT (bytesToSend > 0);
    NS_ASSERT (bytesToSend <= DEFAULT_PACKET_SIZE);
    m_PacingQ.pop_front ();
    NS_ASSERT (m_PacingQBytes >= bytesToSend);
    m_PacingQBytes -= bytesToSend;

    NS_LOG_INFO ("GccSender::SendPacket, packet dequeued, packet length: " << bytesToSend
                 << ", buffer size: " << m_PacingQ.size ()
                 << ", buffer bytes: " << m_PacingQBytes);
    
//    std::cout << "SendPacket::usSlept: " << usSlept << "\n";
    // Synthetic oversleep: random uniform [0% .. 1%]
    // TODO WHY RAND USED?
//    uint64_t oversleepUs = usSlept * (rand () % 100) / 10000;
    
    uint64_t oversleepUs = 0;
    Time tOver{MicroSeconds (oversleepUs)};
    m_sendOversleepEvent = Simulator::Schedule (tOver, &GccSender::SendOverSleep,
                                                this, bytesToSend);

    // usToNextSentPacketD = Time to send current data frame.
    // schedule next sendData
    const double usToNextSentPacketD = double (bytesToSend) * 8. / m_rBitrate;
    const uint64_t usToNextSentPacket = uint64_t (usToNextSentPacketD);

    if (!USE_BUFFER || m_PacingQ.size () == 0) {
        // Buffer became empty
        const auto nowUs = Simulator::Now ().GetMicroSeconds ();
        m_nextSendTstmpUs = nowUs + usToNextSentPacket;
        return;
    }

    Time tNext{MicroSeconds (usToNextSentPacket)};
    m_sendEvent = Simulator::Schedule (tNext, &GccSender::SendPacket, this, usToNextSentPacket);
}

void GccSender::SendOverSleep (uint32_t bytesToSend) {
    const auto nowUs = Simulator::Now ().GetMicroSeconds ();
    
    m_controller->processSendPacket (nowUs, m_sequence, bytesToSend);

    ns3::RtpHeader header{96}; // 96: dynamic payload type, according to RFC 3551
    header.SetSequence (m_sequence++);
    NS_ASSERT (nowUs >= 0);
    
    header.SetTimestamp (Simulator::Now ().GetMicroSeconds());
    header.SetSsrc (m_ssrc);

    auto packet = Create<Packet> (bytesToSend);
    packet->AddHeader (header);

    NS_LOG_INFO ("GccSender::SendOverSleep, " << packet->ToString ());
    m_socket->SendTo (packet, 0, InetSocketAddress{m_destIP, m_destPort});
}

void GccSender::RecvPacket (Ptr<Socket> socket)
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
    NS_LOG_INFO ("GccSender::RecvPacket, " << Packet->ToString ());
    Packet->RemoveHeader (header);
    std::set<uint32_t> ssrcList{};
    header.GetSsrcList (ssrcList);
    if (ssrcList.count (m_ssrc) == 0) {
        NS_LOG_INFO ("GccSender::Received Feedback packet with no data for SSRC " << m_ssrc);
        CalcBufferParams (nowUs);
        return;
    }
    std::vector<std::pair<uint16_t,
                          CCFeedbackHeader::MetricBlock> > feedback{};
    const bool res = header.GetMetricList (m_ssrc, feedback);
    auto l_inter_arrival = 0;
    auto l_inter_departure = 0;
    int64_t l_inter_delay_var = 0;

    NS_ASSERT (res);
    for (auto& item : feedback) {
        const auto sequence = item.first;
        std::cout << m_ssrc << "\trecv seq. :: " << sequence << "\n";
        const auto timestampUs = item.second.m_timestampUs;
        const auto curr_pkt_send_time = m_controller->GetPacketTxTimestamp(sequence);

        if(m_firstFeedback){
            std::cout << m_ssrc << "\tFirst Feedback\n";
            m_gid = 0;
            m_curr_group_start_seq = sequence;		// Sequence of Group's first packet
            m_curr_group_time = m_controller->GetPacketTxTimestamp(sequence);        // Departure time of Group's first packet
            m_firstFeedback = false;

            m_prev_time = timestampUs;
            m_prev_seq = sequence;
            continue;
        }
        /* 
        std::cout << "#. " << curr_pkt_send_time << "\n";
        std::cout << "##. " << m_curr_group_time << "\n";
        std::cout << "###. " << curr_pkt_send_time - m_curr_group_time << "\n";
*/
        // 5000micro seconds = BURST_TIME
        if((curr_pkt_send_time - m_curr_group_time) >= 5000){
            std::cout << m_ssrc << "\tGroup end Detect with c1.\n";
            // Group changed. Calculate Inter-Arrival Time and Inter-Departure Time.
            if(m_gid == 0){
                // First Group
	        m_prev_group_seq = m_prev_seq;		// Sequence of Previous Group's last packet.
                m_group_size = m_prev_group_seq - m_curr_group_start_seq;	// Group size
                m_curr_group_start_seq = sequence;

		m_prev_group_atime = m_prev_time;	// Arrival time of Previous Group's last packet.
		m_curr_group_time = curr_pkt_send_time;
                 
                std::cout << m_ssrc << "\tFirst Group:: \n";
                std::cout << "1. " << m_prev_group_seq << "\n";
                std::cout << "2. " << m_prev_group_atime << "\n";
                std::cout << "3. " << m_curr_group_time << "\n";

         	m_gid += 1;
                m_controller->PrunTransitHistory(m_prev_group_seq);
	    }
	    else{
		// Else
		// Calculate inter variables
		l_inter_arrival = m_prev_time - m_prev_group_atime;
	 	l_inter_departure = m_controller->UpdateDepartureTime(m_prev_group_seq, m_prev_seq);
        	l_inter_delay_var = l_inter_arrival - l_inter_departure;

		m_curr_group_time = curr_pkt_send_time;

		m_prev_group_atime = m_prev_time;
		m_prev_group_seq = m_prev_seq;                        
                        
  //              std::cout << "*. " << m_group_size << "\n";
                m_group_size_inter = (m_prev_group_seq - m_curr_group_start_seq) - m_group_size;
                m_group_size = m_prev_group_seq - m_curr_group_start_seq;

//	        std::cout << "**. " << m_group_size << "\n";
                m_curr_group_start_seq = sequence;
	        std::cout << m_ssrc << "\t" << m_gid << " Group Changed\n";
                std::cout << "1. " << m_prev_group_seq << "\n";
                std::cout << "2. " << m_prev_group_atime << "\n";
                std::cout << "3. " << l_inter_arrival << "\n";
                std::cout << "4. " << l_inter_departure << "\n";
                std::cout << "5. " << l_inter_delay_var << "\n";
                std::cout << "6. " << m_group_size_inter << "\n";
		m_gid += 1;
                
                m_controller->PrunTransitHistory(m_prev_group_seq);
            }

            const auto ecn = item.second.m_ecn;
            NS_ASSERT (timestampUs <= nowUs);
        
            m_controller->processFeedback (nowUs, sequence, timestampUs, l_inter_arrival, l_inter_departure, l_inter_delay_var, m_group_size_inter, m_prev_time, ecn);

            // Increment...
            m_prev_time = timestampUs;
            m_prev_seq = sequence;

            continue;
        }
        
	auto t_inter_arrival = m_prev_time - m_prev_group_atime;
	// auto t_inter_departure = m_controller->UpdateDepartureTime(m_prev_group_seq, m_prev_seq);
        auto t_inter_delay_var = l_inter_arrival - l_inter_departure;
        if(t_inter_arrival < 5000 && t_inter_delay_var < 0){
            std::cout << m_ssrc << "\tNo Group change by c2.\n";

            const auto ecn = item.second.m_ecn;
           
            NS_ASSERT (timestampUs <= nowUs);
        
            m_controller->processFeedback (nowUs, sequence, timestampUs, l_inter_arrival, l_inter_departure, l_inter_delay_var, m_group_size_inter, m_prev_time, ecn);

            // Increment...
            m_prev_time = timestampUs;
            m_prev_seq = sequence;

            continue;
        } else {
            std::cout << "Group change by c2.\n";
            // Group changed. Calculate Inter-Arrival Time and Inter-Departure Time.
            if(m_gid == 0){
                // First Group
	        m_prev_group_seq = m_prev_seq;		// Sequence of Previous Group's last packet.
                m_group_size = m_prev_group_seq - m_curr_group_start_seq;	// Group size
                m_curr_group_start_seq = sequence;

		m_prev_group_atime = m_prev_time;	// Arrival time of Previous Group's last packet.
		m_curr_group_time = curr_pkt_send_time;
                
                std::cout << m_ssrc << "\tFirst Group:: \n";
                std::cout << "1. " << m_prev_group_seq << "\n";
                std::cout << "2. " << m_prev_group_atime << "\n";
                std::cout << "3. " << m_curr_group_time << "\n";

         	m_gid += 1;
                m_controller->PrunTransitHistory(m_prev_group_seq);
	    }
	    else{
		// Else
		// Calculate inter variables
		l_inter_arrival = m_prev_time - m_prev_group_atime;
	 	l_inter_departure = m_controller->UpdateDepartureTime(m_prev_group_seq, m_prev_seq);
        	l_inter_delay_var = l_inter_arrival - l_inter_departure;

		m_curr_group_time = curr_pkt_send_time;

		m_prev_group_atime = m_prev_time;
		m_prev_group_seq = m_prev_seq;                        
                        
      //          std::cout << "*. " << m_group_size << "\n";
                m_group_size_inter = (m_prev_group_seq - m_curr_group_start_seq) - m_group_size;
                m_group_size = m_prev_group_seq - m_curr_group_start_seq;

	//        std::cout << "**. " << m_group_size << "\n";
                m_curr_group_start_seq = sequence;
                std::cout << m_ssrc << "\tGroup Changed\n";
                std::cout << "1. " << m_prev_group_seq << "\n";
                std::cout << "2. " << m_prev_group_atime << "\n";
                std::cout << "3. " << l_inter_arrival << "\n";
                std::cout << "4. " << l_inter_departure << "\n";
                std::cout << "5. " << l_inter_delay_var << "\n";
                std::cout << "6. " << m_group_size_inter << "\n";
		m_gid += 1;

                m_controller->PrunTransitHistory(m_prev_group_seq);
            }
        }	
    
        const auto ecn = item.second.m_ecn;
        NS_ASSERT (timestampUs <= nowUs);
        
        m_controller->processFeedback (nowUs, sequence, timestampUs, l_inter_arrival, l_inter_departure, l_inter_delay_var, m_group_size_inter, m_prev_time, ecn);

    
        // Increment...
        m_prev_time = timestampUs;
        m_prev_seq = sequence;
    }

    // TODO MAYBE THIS PART IS NOT NEEDED.
    // CalcBufferParams (nowUs);
    const auto r_rate = m_controller->getSendBps();
    m_rBitrate = r_rate;
}

void GccSender::CalcBufferParams (uint64_t nowUs)
{
    /*
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
    */
}

}
