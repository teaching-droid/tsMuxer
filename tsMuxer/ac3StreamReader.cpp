#include "ac3StreamReader.h"

#include <fs/systemlog.h>

#include <cstring>
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
    decodeLeadingFrames(findFrame(m_buffer, m_bufEnd));
    findTrueHDMajorSync();

    if (m_true_hd_mode && !m_downconvertToAC3)
        LTRACE(
            LT_INFO, 2,
            "TrueHD track (track " << m_streamIndex << "): the lossless substream is carried through to the output.");
}

// The first two frames of what has been read are decoded in test mode, from frame on, for what the stream is. Test
// mode is on when this is called and off when it returns.
//
// ** A SYNC WORD THAT IS NOT A FRAME IS STEPPED OVER. ** A track that starts inside its stream, as a file cut from a
// clip, can open on the lossless units of a TrueHD track, and their bytes hold 0B 77 now and then. The search for the
// first frame landed on such a sync word, the frame did not decode and the loop gave up at once: no AC-3 frame was
// decoded at all, so the descriptor was built from the false header, as E-AC-3 with whatever "bsid" was there, and
// TrueHD was not found. Until a frame has decoded, the search goes on behind a sync word that does not decode, up to
// the end of what has been read. After that a frame that does not decode ends the loop, as before, so the fields of
// a real frame are not overwritten by a false one. A frame that decodes but is stepped over (skipBytes, the E-AC-3
// part with down-to-ac3) counts as decoded.
void AC3StreamReader::decodeLeadingFrames(uint8_t* frame)
{
    bool decoded = false;
    for (int i = 0; i < 2 && frame != nullptr && frame < m_bufEnd;)
    {
        int skipBytes = 0;
        int skipBeforeBytes = 0;
        const int len = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
        if (len < 1)
        {
            if (decoded || skipBytes > 0)
                break;
            frame = findFrame(frame + 1, m_bufEnd);
            continue;
        }
        decoded = true;
        frame += len + skipBytes;
        if (getFrameDuration() > 0)
            i++;
    }
    m_state = AC3State::stateDecodeAC3;
    AC3Codec::setTestMode(false);
}

// A track that does not begin where its stream begins, as a part cut from a clip of a playlist or a cut file, need not
// have the lossless frame that says what the stream is (the one with a major sync) right behind its first core frames:
// it comes once in 128 frames. It is looked for in what has been read.
void AC3StreamReader::findTrueHDMajorSync()
{
    for (uint8_t* p = m_buffer + 4; !m_true_hd_mode && p + 4 <= m_bufEnd; ++p)
        if (p[0] == 0xF8 && p[1] == 0x72 && p[2] == 0x6F && p[3] == 0xBA)
            m_true_hd_mode = mlp.decodeFrame(p - 4, m_bufEnd);
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
    decodeLeadingFrames(frame);
    // The stream type is chosen from isTrueHD() as soon as this returns, and the payload path finds TrueHD only later.
    // A TrueHD track that starts inside its stream need not have a major sync where the decoding above looks for one,
    // behind its first core frames, and it was declared AC-3 with the lossless units in it. The discovery phase has
    // seen TrueHD then, and the payload path's search is made here as well. Only for such a track, so no plain AC-3
    // track is touched.
    if (m_needTrueHDProbe)
        findTrueHDMajorSync();

    // A Blu-ray player has to decode AC-3 but not E-AC-3. That is why Dolby Digital Plus on a disc is
    // an AC-3 core with the E-AC-3 part beside it: a player without an E-AC-3 decoder plays the core.
    // A stream that is E-AC-3 from its first frame has no core to fall back on. Secondary audio is E-AC-3 without a
    // core by design, so it is left alone. Said once, because the descriptor is built more than once.
    if (blurayMode && !m_eac3NoCoreReported && !isTrueHD() && !isSecondary() && isEAC3() && !isAC3())
    {
        m_eac3NoCoreReported = true;
        LTRACE(LT_ERROR, 2,
               "Warning! The E-AC-3 track " << m_streamIndex
                                            << " has no AC-3 core. A Blu-ray player has to decode AC-3 but not "
                                               "E-AC-3, so a player without an E-AC-3 decoder plays no sound from "
                                               "this track.");
    }
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
    const int rez = SimplePacketizerReader::readPacket(avPacket);
    if (rez == 0)
        stampDependentFrame(avPacket);
    return rez;
}

// A dependent E-AC-3 frame belongs to the independent frame before it: together they are one
// period, a 7.1 stream carrying channels 7 and 8 in the dependent substream. The frame was stamped
// with the time the reader had moved on to, which is the NEXT independent frame's, so in Matroska
// each dependent frame was glued to the following period and a lone dependent frame closed the
// track. Only E-AC-3 without an AC-3 core: in the Blu-ray arrangement the E-AC-3 frame is the one
// that carries the period's time, and that already works.
void AC3StreamReader::stampDependentFrame(AVPacket& avPacket)
{
    if (avPacket.size < 6 || avPacket.data[0] != 0x0B || avPacket.data[1] != 0x77 || (avPacket.data[5] >> 3) <= 10)
        return;
    const bool dependent = (avPacket.data[2] >> 6) == 1;
    if (!dependent)
        m_lastIndependentPts = avPacket.pts;
    else if (!m_bit_rate && m_lastIndependentPts >= 0)
        avPacket.pts = avPacket.dts = m_lastIndependentPts;
}

// The AC-3 and E-AC-3 header gives the frame's length (frmsizecod, or frmsiz for E-AC-3), and so does the first word of
// a TrueHD access unit braided into the track, as the 12 bit length in 16 bit words that decodeFrame reads. A tail
// shorter than the six bytes up to the bsid that begins with the sync word is a cut header.
int AC3StreamReader::wholeFrameSizeAt(const uint8_t* p, const uint8_t* end)
{
    const int64_t avail = end - p;
    if (avail <= 0)
        return 0;
    const bool sync = p[0] == 0x0B && (avail < 2 || p[1] == 0x77);
    if (!sync)
    {
        if (!m_true_hd_mode)
            return 0;
        if (avail < 2)
            return NOT_ENOUGH_BUFFER;
        const int unitLen = (((p[0] & 0x0f) << 8) | p[1]) * 2;
        return unitLen >= 4 ? unitLen : 0;
    }
    if (avail < 6)
        return NOT_ENOUGH_BUFFER;
    return frameSizeAt(p, end);
}

// AC-3 and E-AC-3 as the Blu-ray and the ATSC stream types name them, the secondary audio of a disc included, and
// the TrueHD of a disc, which carries its AC-3 core braided in: its AC-3 frames are checked on their CRC as any other.
// A TrueHD stream without a core has none, and goes on to the TrueHD reader as before.
bool AC3StreamReader::declaredByContainer() const
{
    if (m_containerType != ContainerType::ctTS && m_containerType != ContainerType::ctM2TS)
        return false;
    switch (static_cast<StreamType>(m_containerDataType))
    {
    case StreamType::AUDIO_AC3:
    case StreamType::AUDIO_EAC3:
    case StreamType::AUDIO_EAC3_ATSC:
    case StreamType::AUDIO_EAC3_SECONDARY:
    case StreamType::AUDIO_TRUE_HD:
        return true;
    default:
        return false;
    }
}

// The codec part as a new reader has it: TrueHD mode, the MLP state, the E-AC-3 programs, the Atmos counters and the
// layout. Only the settings stay. checkStream runs on a reader of its own, before any frame is read for the mux.
void AC3StreamReader::resetDecodeState()
{
    const bool downconvert = m_downconvertToAC3;
    const bool testMode = m_testMode;
    static_cast<AC3Codec&>(*this) = AC3Codec();
    m_downconvertToAC3 = downconvert;
    m_testMode = testMode;
}

int AC3StreamReader::flushPacket(AVPacket& avPacket)
{
    // With down-to-ac3 the last TrueHD frame of the track is held back here, because decodeFrame waits for the header
    // behind a TrueHD frame that ends where the data ends. The output has no TrueHD frames, so a whole one is counted
    // as read and left out, as readPacket leaves out the others. What is behind it, the start of a core frame the end
    // of the file cut, goes on below and is handled as before. A cut TrueHD frame goes on below as well.
    if (m_true_hd_mode && m_downconvertToAC3 && m_tmpBufferLen >= 2 &&
        !(m_tmpBuffer[0] == 0x0B && m_tmpBuffer[1] == 0x77))
    {
        const int unitLen = wholeFrameSizeAt(m_tmpBuffer.data(), m_tmpBuffer.data() + m_tmpBufferLen);
        if (unitLen > 0 && unitLen <= m_tmpBufferLen)
        {
            m_processedBytes += unitLen;
            m_tmpBufferLen -= unitLen;
            memmove(m_tmpBuffer.data(), m_tmpBuffer.data() + unitLen, static_cast<size_t>(m_tmpBufferLen));
        }
    }
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0 && !m_true_hd_mode)
        stampDependentFrame(avPacket);
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
        else if (!(avPacket.flags & AVPacket::PRIORITY_DATA) && mlp.m_samplerate)
            avPacket.pts = avPacket.dts =
                samplesToInternal(m_totalTHDSamples, mlp.m_samplerate);  // replace time to a next HD packet
    }
    return rez;
}

bool AC3StreamReader::needMPLSCorrection() const { return !m_true_hd_mode || m_downconvertToAC3; }

// Core frames are passed on in the order the source has them. A held-back core frame, released by a count of TrueHD
// samples, was never active: the count was back at 0 after every TrueHD unit and nothing was ever held, so that code
// is gone.
int AC3StreamReader::readPacketTHD(AVPacket& avPacket)
{
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
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_nextAc3Time += m_frameDuration;
            return 0;
        }
        else
        {
            // thg packet
            // The plain TrueHD reader and the merge reader had the same overflow after two and a half hours at
            // 192 kHz; the conversion is shared now. A rate of 0 means no major sync was decoded yet.
            if (mlp.m_samplerate)
                avPacket.dts = avPacket.pts = samplesToInternal(m_totalTHDSamples, mlp.m_samplerate);

            m_totalTHDSamples += mlp.m_samples;
            return 0;
        }
    }
}
