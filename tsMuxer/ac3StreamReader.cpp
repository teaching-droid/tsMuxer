// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "ac3StreamReader.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
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
    // Test mode is left by decodeLeadingFrames only. Where the first data read holds no frame, as after a long run of
    // other bytes at the start of a file, it stayed on for the whole mux, and with it the probe's ways: the warning
    // that the layout or rate of the stream changes was not given, nor the others test mode keeps quiet.
    if (frame == nullptr)
    {
        AC3Codec::setTestMode(false);
        return 0;
    }
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
    // core by design, so it is left alone. Said once, because the descriptor is built more than once. A demux asks
    // for the disc form of the descriptor too, but makes no disc, so it is not said there.
    if (blurayMode && !m_demuxMode && !m_eac3NoCoreReported && !isTrueHD() && !isSecondary() && isEAC3() && !isAC3())
    {
        m_eac3NoCoreReported = true;
        LTRACE(LT_ERROR, 2,
               "Warning! The E-AC-3 track " << m_streamIndex
                                            << " has no AC-3 core. A Blu-ray player has to decode AC-3 but not "
                                               "E-AC-3, so a player without an E-AC-3 decoder plays no sound from "
                                               "this track.");
    }
    BitStreamWriter bitWriter{};

    // ** E-AC-3 WITH AN AC-3 CORE UNDER STREAM TYPE 0x87 IS DESCRIBED AS E-AC-3. ** Dolby Digital Plus as a disc
    // carries it, an AC-3 frame with the E-AC-3 part beside it, gets stream type 0x87 in a TS that is neither a disc
    // nor M2TS (TSMuxer::intAddStream), and ATSC A/52 Annex G 3.5 asks for the E-AC-3 audio descriptor with that stream
    // type. It was given the AC-3 descriptor of its core. Its independent substream 0 is the core, whose bsid the
    // descriptor carries. A disc and an M2TS, where the stream type is 0x84, keep the AC-3 descriptor.
    const bool eac3Type = m_atscEac3Type && isEAC3() && !isTrueHD() && !m_downconvertToAC3;
    if (isAC3() && !eac3Type)
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
    bitWriter.putBits(5, isAC3() ? m_bsidBase : m_bsid);

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
    if (!m_pendingJumps.empty())
        takePendingJumps();
    if (m_true_hd_mode && !m_downconvertToAC3)
        return readPacketTHD(avPacket);
    const int rez = SimplePacketizerReader::readPacket(avPacket);
    if (rez == 0 && m_true_hd_mode && !m_leadApplied)
    {
        // down-to-ac3: the first core frame was timed at the start of the track, and the frames after it follow it
        const int64_t before = static_cast<int64_t>(m_curPts * m_stretch);
        applyLeadUnits();
        const int64_t moved = static_cast<int64_t>(m_curPts * m_stretch) - before;
        avPacket.pts += moved;
        avPacket.dts += moved;
    }
    if (rez == 0)
        stampDependentFrame(avPacket);
    return rez;
}

// ** A TRUEHD TRACK THAT STARTS INSIDE ITS STREAM KEEPS ITS TIME. ** A file cut from a clip begins with the access
// units of the TrueHD in front of its first AC-3 core frame, and they are left out with the search for that frame: the
// TrueHD is read from a core frame on. The track was timed from its first PES, which is one of those units, so all of
// it, core and TrueHD, came the length of the units early: 21 to 33 ms in measured cuts. Each unit says its length, so
// the units in front of the first core frame are counted from the start of the track, and its time starts after them.
// Where they do not lead to a core frame, nothing is counted, as before. Counted once, on the first search outside test
// mode.
uint8_t* AC3StreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    uint8_t* frame = AC3Codec::findFrame(buff, end);
    if (m_leadCounted || getTestMode() || !m_true_hd_mode || frame == nullptr)
        return frame;
    // The units hold 0B 77 now and then, and the search can stop there first: such a sync word is stepped over by the
    // search that follows. The walk goes on to the first core frame that stands where a unit ends.
    m_leadCounted = true;
    m_leadUnits = -1;
    int units = 0;
    for (const uint8_t* p = buff; end - p >= 6; ++units)
    {
        if (frameSizeAt(p, end) > 0)
        {
            m_leadUnits = units;
            break;
        }
        const int len = MLPCodec::getFrameSize(p);
        if (len < 4)
            break;
        p += len;
    }
    return frame;
}

// The time of the units counted by findFrame is given to the core clock and to the TrueHD clock, before the first frame
// is stamped. The core frames and the access units then keep the distance they have in the source, as for a track read
// from the start of its stream.
void AC3StreamReader::applyLeadUnits()
{
    if (m_leadApplied)
        return;
    m_leadApplied = true;
    if (m_leadUnits <= 0 || mlp.m_samples <= 0 || mlp.m_samplerate <= 0)
        return;
    const int64_t samples = static_cast<int64_t>(m_leadUnits) * mlp.m_samples;
    const int64_t lead = samplesToInternal(samples, mlp.m_samplerate);
    if (m_downconvertToAC3)
        m_curPts += static_cast<double>(lead);
    else
    {
        m_totalTHDSamples += samples;
        m_nextAc3Time += lead;
    }
    LTRACE(LT_INFO, 2,
           getCodecInfo().displayName << " stream (track " << m_streamIndex << "): it begins with " << m_leadUnits
                                      << " TrueHD access units in front of its first AC-3 core frame, which are left "
                                         "out; the audio after them keeps its time ("
                                      << samples * 1000 / mlp.m_samplerate << " ms from the start of the track).");
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

// A gap a join puts in the times of the track (addTimeGap). TrueHD with its AC-3 core stamps its core frames and its
// access units from clocks of their own (readPacketTHD), which the gap of the base class did not reach: the join said
// that the sound of a later file starts after a gap, and its core and its TrueHD still followed on from the sound
// before. Each clock now takes each gap at the frame or the unit that comes at its time, within half of one, as the
// plain TrueHD reader does. Down-to-ac3 is timed by the base class, which takes it as before.
void AC3StreamReader::addTimeGap(const int64_t atNs, const int64_t gapNs)
{
    SimplePacketizerReader::addTimeGap(atNs, gapNs);
    m_coreGaps.emplace_back(atNs, gapNs);
    m_unitGaps.emplace_back(atNs, gapNs);
}

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
        applyLeadUnits();
        // A sync word in the units in front of the first core frame that is no frame is stepped over with a packet of
        // no data. It is no access unit, and it advanced the TrueHD clock by one: the TrueHD of such a track came a
        // unit late against its core.
        if (avPacket.size == 0)
            return 0;

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
            while (!m_coreGaps.empty() &&
                   (static_cast<double>(m_nextAc3Time) + m_frameDuration / 2.0) * 1e9 / INTERNAL_PTS_FREQ >=
                       static_cast<double>(m_coreGaps.front().first))
            {
                m_nextAc3Time += llround(static_cast<double>(m_coreGaps.front().second) * INTERNAL_PTS_FREQ / 1e9);
                m_coreGaps.erase(m_coreGaps.begin());
            }
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_nextAc3Time += m_frameDuration;
            return 0;
        }
        else
        {
            while (mlp.m_samplerate && !m_unitGaps.empty() &&
                   (static_cast<double>(m_totalTHDSamples) + mlp.m_samples / 2.0) * 1e9 / mlp.m_samplerate >=
                       static_cast<double>(m_unitGaps.front().first))
            {
                m_totalTHDSamples += llround(static_cast<double>(m_unitGaps.front().second) * mlp.m_samplerate / 1e9);
                m_unitGaps.erase(m_unitGaps.begin());
            }
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

AC3StreamReader::WavForm AC3StreamReader::wavForm(const uint8_t* buf, const int len, int& data, bool* more)
{
    if (more)
        *more = false;
    if (len < 64 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return WavForm::none;
    // a WAVE file: until its data is found and looked at, more of it may be needed to tell
    if (more)
        *more = true;
    data = -1;
    for (int at = 12; at + 8 <= len;)
    {
        const uint32_t size =
            buf[at + 4] | buf[at + 5] << 8 | buf[at + 6] << 16 | static_cast<uint32_t>(buf[at + 7]) << 24;
        if (memcmp(buf + at, "data", 4) == 0)
        {
            data = at + 8;
            break;
        }
        if (size > static_cast<uint32_t>(len))
            return WavForm::none;
        at += 8 + static_cast<int>(size) + static_cast<int>(size & 1);
    }
    if (data < 0 || data + 8 > len)
        return WavForm::none;
    const uint8_t* d = buf + data;
    const int left = len - data;
    // frames: a sync word at the start of the data, and again one frame on, and one more
    if (d[0] == 0x0B && d[1] == 0x77)
    {
        if (more)
            *more = false;
        for (int n = 64; n <= 3840 && 2 * n + 2 <= left; n += 2)
            if (d[n] == 0x0B && d[n + 1] == 0x77 && d[2 * n] == 0x0B && d[2 * n + 1] == 0x77)
                return WavForm::frames;
        return WavForm::none;
    }
    // all that is looked at for bursts is there: what is found is the answer
    if (more && left >= 256 * 1024 + 10)
        *more = false;
    // bursts: the preamble Pa Pb (F872 4E1F, low byte first), the data type of AC-3 (1) or E-AC-3 (21) in Pc, and the
    // sync word of the frame behind it, its bytes swapped as the words are; twice, as samples can hold these 8 bytes
    int seen = 0;
    for (int at = 0; at + 10 <= left && at < 256 * 1024; at += 2)
        if (d[at] == 0x72 && d[at + 1] == 0xF8 && d[at + 2] == 0x1F && d[at + 3] == 0x4E &&
            ((d[at + 4] & 0x1F) == 1 || (d[at + 4] & 0x1F) == 21) && d[at + 8] == 0x77 && d[at + 9] == 0x0B &&
            ++seen == 2)
            return WavForm::bursts;
    return WavForm::none;
}

// A burst is four words in front of its frame, each stored low byte first: Pa and Pb (F872, 4E1F), Pc with the kind of
// data in its low 5 bits (1 AC-3, 21 E-AC-3), and Pd, the length of what follows, in bits for AC-3 and in bytes for
// E-AC-3. The frame follows in words of the same form, so its bytes are swapped in pairs, and zeros fill the samples up
// to the next burst. A burst of another kind (a pause) and samples that hold no burst are passed over.
void AC3StreamReader::unpackBursts(std::vector<uint8_t>& in, std::vector<uint8_t>& out, const bool last,
                                   int64_t* outside)
{
    size_t pos = 0;
    const size_t size = in.size();
    while (true)
    {
        while (pos + 10 <= size &&
               !(in[pos] == 0x72 && in[pos + 1] == 0xF8 && in[pos + 2] == 0x1F && in[pos + 3] == 0x4E))
        {
            if (outside && in[pos] != 0)
                ++*outside;
            ++pos;
        }
        if (pos + 10 > size)
            break;
        const int kind = in[pos + 4] & 0x1F;
        const size_t length = in[pos + 6] | static_cast<size_t>(in[pos + 7]) << 8;
        const size_t bytes = kind == 21 ? length : (length + 7) / 8;
        if ((kind != 1 && kind != 21) || bytes < 2 || in[pos + 8] != 0x77 || in[pos + 9] != 0x0B)
        {
            pos += 4;  // a pause, or these four bytes in other data
            continue;
        }
        size_t have = (bytes + 1) / 2 * 2;  // in whole words
        if (pos + 8 + have > size)
        {
            if (!last)
                break;  // the rest of it is in the next block
            have = (size - pos - 8) / 2 * 2;
        }
        const size_t before = out.size();
        for (size_t i = 0; i < have; i += 2)
        {
            out.push_back(in[pos + 8 + i + 1]);
            out.push_back(in[pos + 8 + i]);
        }
        if (out.size() - before > bytes)
            out.resize(before + bytes);  // a frame of an odd number of bytes
        pos += 8 + have;
    }
    if (last)
        pos = size;
    in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(pos));
}

void AC3StreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    // bursts come in a WAV, a file of its own; a container's frames, each with its own time, are passed on as they come
    if (m_bursts == Bursts::unknown && m_containerType != ContainerType::ctNone &&
        m_containerType != ContainerType::ctLPCM)
        m_bursts = Bursts::no;
    if (m_bursts == Bursts::no)
    {
        SimplePacketizerReader::setBuffer(data, dataLen, lastBlock);
        return;
    }
    const bool has = data != nullptr && dataLen > 0;
    if (m_bursts == Bursts::unknown)
    {
        // Told from the start of the file: its header and the first bursts behind it. Where they do not lie in the
        // first block, as behind a large chunk of tags, the blocks are kept until they do, as far as the listing of
        // the file looks: the listing named such a file as packed sound, and the mux read its samples as frames.
        const bool held = !m_burstPending.empty();
        if (!held && !has)
        {
            SimplePacketizerReader::setBuffer(data, dataLen, lastBlock);
            return;
        }
        if (held && has)
            m_burstPending.insert(m_burstPending.end(), data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
        const uint8_t* start = held ? m_burstPending.data() : data + MAX_AV_PACKET_SIZE;
        const size_t seen = held ? m_burstPending.size() : dataLen;
        int at = 0;
        bool more = false;
        const WavForm form =
            wavForm(start, static_cast<int>((std::min)(seen, static_cast<size_t>(1) << 30)), at, &more);
        m_burstIn += dataLen;
        if (form != WavForm::bursts && more && !lastBlock && seen < DETECT_STREAM_BUFFER_SIZE)
        {
            if (!held)
                m_burstPending.assign(data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
            m_burstOut.assign(MAX_AV_PACKET_SIZE, 0);
            SimplePacketizerReader::setBuffer(m_burstOut.data(), 0, false);  // more of the file first
            return;
        }
        if (form != WavForm::bursts)
        {
            m_bursts = Bursts::no;
            if (!held)
            {
                SimplePacketizerReader::setBuffer(data, dataLen, lastBlock);
                return;
            }
            // the blocks kept, as they are
            m_burstOut.assign(MAX_AV_PACKET_SIZE, 0);
            m_burstOut.insert(m_burstOut.end(), m_burstPending.begin(), m_burstPending.end());
            m_burstPending = std::vector<uint8_t>();
            SimplePacketizerReader::setBuffer(m_burstOut.data(),
                                              static_cast<uint32_t>(m_burstOut.size() - MAX_AV_PACKET_SIZE), lastBlock);
            return;
        }
        m_bursts = Bursts::yes;
        if (!m_testMode)
            LTRACE(LT_INFO, 2,
                   "The sound of track " << m_streamIndex
                                         << " is packed for a digital audio output (IEC 61937) in the samples of a "
                                            "WAVE file. Its frames are taken out of it.");
        if (!held)
            m_burstPending.assign(data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
    }
    else
    {
        if (has)
            m_burstPending.insert(m_burstPending.end(), data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
        m_burstIn += dataLen;
    }
    m_burstOut.assign(MAX_AV_PACKET_SIZE, 0);
    unpackBursts(m_burstPending, m_burstOut, lastBlock, &m_burstOutside);
    m_burstPassed += static_cast<int64_t>(m_burstOut.size() - MAX_AV_PACKET_SIZE);
    // ** WHAT IS NOT PACKED SOUND IS SAID. ** Bytes that are no burst and no filling are passed over: the header of
    // the file, and tags. A file of plain frames joined behind such a WAV was passed over whole in the same way, and
    // the job ended as done with that file missing.
    if (lastBlock && m_burstOutside > 65536 && !m_testMode)
    {
        LTRACE(LT_WARN, 2,
               "Warning: " << m_burstOutside << " bytes of track " << m_streamIndex
                           << " are not sound packed for a digital audio output, and were left out. A file of another "
                              "form joined to it, such as plain AC-3 frames, is not read: join files of one form.");
        m_burstOutside = 0;
    }
    SimplePacketizerReader::setBuffer(m_burstOut.data(), static_cast<uint32_t>(m_burstOut.size() - MAX_AV_PACKET_SIZE),
                                      lastBlock);
}

// The bytes of the file that the frames handed on so far stand for: the progress of a mux is counted against the file
int64_t AC3StreamReader::getProcessedSize()
{
    const int64_t processed = SimplePacketizerReader::getProcessedSize();
    if (m_bursts != Bursts::yes || m_burstPassed <= 0)
        return processed;
    return static_cast<int64_t>(static_cast<double>(processed) * static_cast<double>(m_burstIn) /
                                static_cast<double>(m_burstPassed));
}
