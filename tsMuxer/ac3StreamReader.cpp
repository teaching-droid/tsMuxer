#include "ac3StreamReader.h"

#include <fs/systemlog.h>

#include <sstream>

#include "avCodecs.h"
#include "bitStream.h"
#include "vod_common.h"

void AC3StreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = m_bit_rate;
    data.isTrueHD = m_true_hd_mode;
}

void AC3StreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_sample_rate = data.sampleRate;
    if (data.channels > 0)
        m_channels = static_cast<uint8_t>(data.channels);
    if (data.bitrate > 0)
        m_bit_rate = data.bitrate;
    // A TrueHD track is an AC-3 core with a lossless substream behind it, and the reader only ever
    // finds that out while PROBING, because the detection at ac3Codec.cpp is gated on test mode.
    // The instance that does the muxing is a different one, and it re-ran that probe only on the
    // Blu-ray path, inside getTSDescriptor. On every other output it stayed in plain AC-3 mode:
    // each lossless frame failed to parse, was reported as a bad frame and resynced past, so the
    // result was the 448 kbps core alone, labelled AC-3, with no warning and a successful mux.
    // Carrying the flag here is what the Dolby Vision flags already do for the same reason.
    //
    // What is carried is a REQUEST TO LOOK, not the answer. Setting m_true_hd_mode directly gives a
    // reader that believes it is TrueHD while its MLP sub-codec is still empty: the stream then
    // reports "AC3 core + UNKNOWN" at 0 KHz, and the frame timing divides by a sample rate of zero.
    // The flag only says the probe found a lossless substream, so this reader should go and find it
    // too. Only ever turned ON, so a reader that has already worked it out is left alone.
    if (data.isTrueHD && !m_true_hd_mode)
        m_needTrueHDProbe = true;
}

// Establish TrueHD mode on THIS reader, the same way the Blu-ray path does inside getTSDescriptor.
//
// TrueHD detection at ac3Codec.cpp is gated on test mode, which is only ever on while probing, so a
// reader that never probes stays in plain AC-3 mode forever. Deliberately kept to tracks the
// discovery phase already identified as TrueHD, so no plain AC-3 track on any path is touched.
void AC3StreamReader::probeTrueHD()
{
    if (m_true_hd_mode || m_buffer == nullptr || m_buffer >= m_bufEnd)
        return;

    AC3Codec::setTestMode(true);
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    for (int i = 0; i < 2 && frame != nullptr && frame < m_bufEnd;)
    {
        int skipBytes = 0;
        int skipBeforeBytes = 0;
        const int len = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
        if (len < 1)
            break;
        frame += len + skipBytes;
        if (getFrameDuration() > 0)
            i++;
    }
    m_state = AC3State::stateDecodeAC3;
    AC3Codec::setTestMode(false);

    if (m_true_hd_mode)
        LTRACE(
            LT_INFO, 2,
            "TrueHD track (track " << m_streamIndex << "): the lossless substream is carried through to the output.");
}

bool AC3StreamReader::isPriorityData(AVPacket* packet)
{
    return (packet->size >= 2 && packet->data[0] == 0x0B && packet->data[1] == 0x77 && m_strmtyp != 1);
}

bool AC3StreamReader::isSecondary() { return m_secondary; };

void AC3StreamReader::writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket)
{
    if (m_useNewStyleAudioPES)
    {
        pesPacket->flagsLo |= 1;  // enable PES extension for AC3 stream
        uint8_t* data = reinterpret_cast<uint8_t*>(pesPacket) + pesPacket->getHeaderLength();
        *data++ = 0x01;
        *data++ = 0x81;
        if (!m_true_hd_mode || m_downconvertToAC3)
        {
            if (m_bsid > 10)
                *data = 0x72;  // E-AC3 subtype
            else
                *data = 0x71;  // AC3 subtype
        }
        else
        {
            if (avPacket.flags & AVPacket::IS_CORE_PACKET)
                *data = 0x76;  // AC3 at TRUE-HD
            else
                *data = 0x72;  // TRUE-HD data
        }
        pesPacket->m_pesHeaderLen += 3;
    }
}

int AC3StreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    AC3Codec::setTestMode(true);
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    for (int i = 0; i < 2 && frame < m_bufEnd;)
    {
        int skipBytes = 0;
        int skipBeforeBytes = 0;
        const int len = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
        if (len < 1)
        {
            // m_state = stateDecodeAC3;
            // AC3Codec::setTestMode(false);
            // return 0;
            break;
        }
        frame += len + skipBytes;
        if (getFrameDuration() > 0)
            i++;
    }
    m_state = AC3State::stateDecodeAC3;
    AC3Codec::setTestMode(false);
    BitStreamWriter bitWriter{};

    if (isAC3())
    {
        // ATSC A/52 Annex A Table A3.1 AC-3 Registration Descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
        *dstBuff++ = 4;                                                    // decriptor length
        *dstBuff++ = 'A';
        *dstBuff++ = 'C';
        *dstBuff++ = '-';
        *dstBuff++ = '3';

        // ATSC A/52 Annex A Table A4.1 AC-3 Audio Descriptor Syntax
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::AC3);  // AC-3_audio_stream_descriptor
        *dstBuff++ = 4;                                           // descriptor len

        bitWriter.setBuffer(dstBuff, dstBuff + 4);

        bitWriter.putBits(3, m_fscod);     // bitrate code
        bitWriter.putBits(5, m_bsidBase);  // 6 = AC3

        bitWriter.putBits(6, m_frmsizecod >> 1);  // MSB == 0. bit rate is exact
        bitWriter.putBits(2, m_dsurmod);

        bitWriter.putBits(3, m_bsmod);
        bitWriter.putBits(4, m_acmod);  // when MSB == 0 then high (4-th) bit always 0
        bitWriter.putBit(0);            // full_svc

        bitWriter.putBits(8, 0);  // langcod
        bitWriter.flushBits();

        return 12;
    }

    // Not AC3 => EAC3
    // ATSC A/52 Annex G 2.EAC3 Registration Descriptor
    *dstBuff++ = static_cast<int>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *dstBuff++ = 4;                                                // descriptor length
    *dstBuff++ = 'E';
    *dstBuff++ = 'A';
    *dstBuff++ = 'C';
    *dstBuff++ = '3';

    // ATSC A/52 Annex G Table G.1
    *dstBuff++ = static_cast<int>(TSDescriptorTag::EAC3);  // EAC3_audio_stream_descriptor
    *dstBuff++ = 4;                                        // descriptor len

    bitWriter.setBuffer(dstBuff, dstBuff + 4);

    bitWriter.putBits(4, 12);  // reserved = 1, bsid_flag = 1, mainid_flag = 0, asvc_flag = 0
    bitWriter.putBits(1, m_mixinfoexists);
    bitWriter.putBits(3, 0);  // independant substreams not supported

    bitWriter.putBits(5, 24);  // reserved = 1, full_service_flag = 1, audio_service_type = 0 (Complete Main)
    int number_of_channels = (m_acmod == 0 ? 1 : (m_acmod == 1 ? 0 : (m_acmod == 2 ? (m_dsurmod ? 3 : 2) : 4)));
    if (m_extChannelsExists)
        number_of_channels = 5;
    bitWriter.putBits(3, number_of_channels);

    bitWriter.putBits(3, 1);  // language_flag = 0, language_flag2 = 0, reserved = 1
    bitWriter.putBits(5, m_bsid);

    bitWriter.putBits(8, 0x80);  // additional_info_byte
    bitWriter.flushBits();

    return 12;
}

int AC3StreamReader::readPacket(AVPacket& avPacket)
{
    if (m_needTrueHDProbe)
    {
        m_needTrueHDProbe = false;
        probeTrueHD();
    }
    if (m_true_hd_mode && !m_downconvertToAC3)
        return readPacketTHD(avPacket);
    return SimplePacketizerReader::readPacket(avPacket);
}

int AC3StreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0 && m_true_hd_mode && !m_downconvertToAC3)
    {
        // The last frame of a braided track arrives through here, and it can be a CORE frame.
        // Such a packet reaches this point still carrying the timestamp of an earlier one, and
        // it also carries PRIORITY_DATA, so the branch below left the stale value in place and
        // wrote it to the output. Measured on a disc that groups its core: the final frame came
        // out stamped two frames behind the previous core frame on a 7.6 minute cut, and twelve
        // frames behind over a whole feature. It is the only backward step in that output that
        // the interleave does not explain. A core frame belongs on the core clock, here as much
        // as anywhere else.
        if (rez >= 2 && avPacket.data != nullptr && avPacket.data[0] == 0x0B && avPacket.data[1] == 0x77)
        {
            avPacket.pts = avPacket.dts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_nextAc3Time += m_frameDuration;
        }
        else if (!(avPacket.flags & AVPacket::PRIORITY_DATA))
            avPacket.pts = avPacket.dts =
                m_totalTHDSamples * INTERNAL_PTS_FREQ / mlp.m_samplerate;  // replace time to a next HD packet
    }
    return rez;
}

bool AC3StreamReader::needMPLSCorrection() const { return !m_true_hd_mode || m_downconvertToAC3; }

int AC3StreamReader::readPacketTHD(AVPacket& avPacket)
{
    if (m_thdDemuxWaitAc3 && !m_delayedAc3Buffer.isEmpty())
    {
        avPacket = m_delayedAc3Packet;
        m_delayedAc3Buffer.clear();
        m_thdDemuxWaitAc3 = false;
        avPacket.dts = avPacket.pts = m_nextAc3Time;
        avPacket.flags |= AVPacket::IS_CORE_PACKET;
        m_nextAc3Time += m_frameDuration;
        return 0;
    }

    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        // A core frame is recognised by WHAT IT IS, not by the state the decoder is left in after
        // sizing it. m_state only reaches stateDecodeTrueHDFirst when the NEXT frame is not another
        // core frame, so on a disc that delivers the core in groups every frame but the last of a
        // group was taken for TrueHD: it was stamped on the TrueHD clock and it advanced that clock
        // by an access unit it does not represent. Measured on such a disc, 8,476 of 14,207 core
        // frames were stamped from the wrong clock and the two clocks then walked apart, producing
        // 5,684 timestamps that went BACKWARDS, growing to about 280 seconds by the end.
        //
        // Every core frame is stamped on the core's own clock, which advances one frame duration per
        // frame whatever the grouping. A disc that delivers one core frame at a time is unaffected:
        // there, every core frame already satisfied the old test as well.
        const bool isAc3Packet = m_frameIsCore;

        if (isAc3Packet)
        {
            m_thdDemuxWaitAc3 = false;
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_nextAc3Time += m_frameDuration;
            return 0;
        }
        else
        {
            // thg packet
            avPacket.dts = avPacket.pts = m_totalTHDSamples * INTERNAL_PTS_FREQ / mlp.m_samplerate;

            m_totalTHDSamples += mlp.m_samples;
            m_demuxedTHDSamples += mlp.m_samples;
            if (m_demuxedTHDSamples >= mlp.m_samples)
            {
                m_demuxedTHDSamples -= mlp.m_samples;
                m_thdDemuxWaitAc3 = true;
            }
            return 0;
        }
    }
}
