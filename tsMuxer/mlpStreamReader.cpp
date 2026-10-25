#include "mlpStreamReader.h"

#include <cmath>
#include <functional>

#include <fs/systemlog.h>

#include "ac3Codec.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

void MLPStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = m_bitrate;
}

void MLPStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_samplerate = data.sampleRate;
    if (data.channels > 0)
        m_channels = static_cast<uint8_t>(data.channels);
    if (data.bitrate > 0)
        m_bitrate = data.bitrate;
}

int MLPStreamReader::getHeaderLen() { return MLP_HEADER_LEN; }

const std::string MLPStreamReader::getStreamInfo()
{
    std::ostringstream str;

    if (m_subType == MlpSubType::stTRUEHD)
        str << "TRUE-HD";
    else if (m_subType == MlpSubType::stMLP)
        str << "MLP";
    else
        str << "UNKNOWN";

    if (m_substreams == 4)
        str << " + ATMOS";
    str << ". ";
    str << "Peak bitrate: " << m_bitrate / 1000 << "Kbps ";
    str << "Sample Rate: " << m_samplerate / 1000 << "KHz ";
    str << "Channels: " << static_cast<int>(m_channels);
    return str.str();
}

// The size of the AC-3 or E-AC-3 frame whose header starts at buf (ATSC A/52, 5.4.1 and E.1.2), or 0 when none does
static int coreFrameSize(const uint8_t* buf)
{
    if (buf[0] != 0x0B || buf[1] != 0x77)
        return 0;
    const int bsid = buf[5] >> 3;
    if (bsid > 10 && bsid <= 16)
        return ((((buf[2] & 7) << 8) | buf[3]) + 1) * 2;  // E-AC-3: frmsiz, in 16-bit words less one
    if (bsid > 8)
        return 0;
    const int fscod = buf[4] >> 6;
    const int frmsizecod = buf[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
        return 0;
    static constexpr int KBPS[19] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                     192, 224, 256, 320, 384, 448, 512, 576, 640};
    const int kbps = KBPS[frmsizecod >> 1];
    if (fscod == 0)
        return kbps * 4;  // 48 kHz: 1536 samples
    if (fscod == 2)
        return kbps * 6;                               // 32 kHz
    return (kbps * 320 / 147 + (frmsizecod & 1)) * 2;  // 44.1 kHz, in 16-bit words, the odd codes one word longer
}

int MLPStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;
    // Below MLP_FULL_HEADER_LEN the codec cannot tell a broken access unit from one that is
    // merely cut off by the end of the read block, and answers false either way. Returning 0
    // here would make the caller resync to the next major sync and drop everything up to it,
    // so ask for more data instead and let the access unit be carried into the next block.
    if (end - buff < MLP_FULL_HEADER_LEN)
        return NOT_ENOUGH_BUFFER;
    // A Blu-ray TrueHD track carries an AC-3 core: an AC-3 frame before every few access units (#804). Read as plain
    // TrueHD (A_MLP), each core frame was taken for a broken access unit, the stream was searched for the next major
    // sync, and the access units on the way were lost: 84 % of a measured track, under "Mux successful". The core
    // frames are left out now, as drop-ac3-core leaves them out, and every access unit is kept. A frame counts as core
    // only when the stream goes on after it with an access unit or another core frame.
    int core = 0;
    for (int n; (n = coreFrameSize(buff + core)) > 0;)
    {
        core += n;
        if (end - (buff + core) < MLP_FULL_HEADER_LEN)
            return NOT_ENOUGH_BUFFER;
    }
    if (core > 0 && MLPCodec::decodeFrame(buff + core, end))
    {
        if (!m_coreSaid)
            LTRACE(LT_INFO, 2,
                   getCodecInfo().displayName
                       << " stream (track " << m_streamIndex
                       << "): it holds an AC-3 core besides its TrueHD, as a Blu-ray TrueHD track does. A_MLP keeps "
                          "the TrueHD only, which is what drop-ac3-core gives on an A_AC3 line; A_AC3 keeps both.");
        m_coreSaid = true;
        skipBeforeBytes = core;
        return getFrameSize(buff + core);
    }
    if (MLPCodec::decodeFrame(buff, end))
        return getFrameSize(buff);
    return 0;
}

int MLPStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    // Ensure we have decoded at least one frame so codec parameters are valid
    uint8_t* frame = MLPCodec::findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes) < 1)
        return 0;

    if (hdmvDescriptors)
    {
        // Blu-ray core specifications - HDMV TrueHD/MLP audio registration descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
        *dstBuff++ = 8;                                                    // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;                                             // stuffing_bits
        *dstBuff++ = static_cast<uint8_t>(StreamType::AUDIO_TRUE_HD);  // stream_coding_type
        const int audio_presentation_type = (m_channels > 2) ? 6 : (m_channels == 2) ? 3 : 1;
        // The codes of the clip information file: 48 kHz is 1, 96 kHz 4 and 192 kHz 5, and a rate a disc does not have,
        // such as 44.1 kHz, 0. Every rate but 96 and 192 kHz was declared 48 kHz here, so the two disagreed.
        const int sampling_frequency = m_samplerate == 192000  ? 5
                                       : m_samplerate == 96000 ? 4
                                       : m_samplerate == 48000 ? 1
                                                               : 0;
        *dstBuff++ = static_cast<uint8_t>(audio_presentation_type << 4 | sampling_frequency);
        *dstBuff++ = 0xff;  // stuffing_bits

        return 10;  // total descriptor length (2 header + 8 data)
    }

    // Non-HDMV: SMPTE-RA registered format identifier for MLP audio
    // https://smpte-ra.org/registered-mpeg-ts-ids
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *dstBuff++ = 4;                                                    // descriptor length
    *dstBuff++ = 'm';
    *dstBuff++ = 'l';
    *dstBuff++ = 'p';
    *dstBuff++ = 'a';

    return 6;  // total descriptor length
}

// A TrueHD stream that does not begin with a major sync, as after a cut: its access units up to the first one cannot be
// decoded and are left out. The time of the stream was counted from the first access unit handed on, so all the audio
// after them moved forward by their length: 81 ms in a measured cut, 98 ms in a reported one (#600). Each access unit
// says its length, so the units before the major sync are counted here, from the start of the stream, and the time
// starts after them. Where they do not lead exactly to the major sync, nothing is counted, as before.
uint8_t* MLPStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    uint8_t* frame = MLPCodec::findFrame(buff, end);
    if (m_started)
        return frame;
    if (frame == nullptr)
    {
        m_preSyncUnits = -1;  // the search goes on in the next block: the units there are not counted
        return frame;
    }
    if (m_preSyncUnits < 0)
        return frame;
    // A Blu-ray TrueHD track carries its AC-3 core frames between its access units. Read as an access unit, a core
    // frame's sync word gave a length that led anywhere but the major sync, nothing was counted, and the audio of such
    // a cut played early again: 50 ms in a measured one. Where an AC-3 sync word stands, both readings are tried, and
    // the one that leads exactly to the major sync is taken; a core frame is not counted as an access unit.
    const std::function<int(const uint8_t*, int)> walk = [&](const uint8_t* p, const int depth) -> int
    {
        int units = 0;
        while (p < frame && frame - p >= 4)
        {
            if (const int core = AC3Codec::frameSizeAt(p, frame); core > 0 && p + core <= frame && depth < 16)
            {
                const int rest = walk(p + core, depth + 1);
                if (rest >= 0)
                    return units + rest;
            }
            const int len = getFrameSize(p);
            if (len < 4)
                return -1;
            p += len;
            ++units;
        }
        return p == frame ? units : -1;
    };
    m_preSyncUnits = walk(buff, 0);
    return frame;
}

int MLPStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        if (!m_started)
        {
            m_started = true;
            if (m_preSyncUnits > 0 && m_samples > 0)
            {
                m_totalTHDSamples = static_cast<int64_t>(m_preSyncUnits) * m_samples;
                if (m_samplerate)
                    LTRACE(LT_INFO, 2,
                           getCodecInfo().displayName
                               << " stream (track " << m_streamIndex << "): it begins with " << m_preSyncUnits
                               << " access units before its first major sync, which cannot be decoded and are left "
                                  "out; the audio after them keeps its time ("
                               << m_totalTHDSamples * 1000 / m_samplerate << " ms from the start of the track).");
            }
        }
        // A gap the source asks for (addTimeGap) begins at the unit that comes at its time, within half a unit
        while (m_samplerate && !m_thdGaps.empty() &&
               (static_cast<double>(m_totalTHDSamples) + m_samples / 2.0) * 1e9 / m_samplerate >=
                   static_cast<double>(m_thdGaps.front().first))
        {
            m_totalTHDSamples += llround(static_cast<double>(m_thdGaps.front().second) * m_samplerate / 1e9);
            m_thdGaps.erase(m_thdGaps.begin());
        }
        // thg packet
        // m_samplerate stays 0 until a major sync decodes; a stream that never produces one
        // must not divide by it.
        if (m_samplerate)
            avPacket.dts = avPacket.pts = samplesToInternal(m_totalTHDSamples, m_samplerate);

        m_totalTHDSamples += m_samples;
        return 0;
    }
}

int MLPStreamReader::flushPacket(AVPacket& avPacket)
{
    // Core frames with nothing after them at the end of the stream: decodeFrame asks for the access unit that would
    // follow, and what was held was written out as a last TrueHD unit. They are left out as the others are (a core
    // frame as the last unit was kept in the A_MLP output).
    if (m_tmpBufferLen > 0)
    {
        int core = 0;
        for (int n; m_tmpBufferLen - core >= 6 && (n = coreFrameSize(m_tmpBuffer.data() + core)) > 0;) core += n;
        if (core == m_tmpBufferLen)
        {
            m_processedBytes += m_tmpBufferLen;
            m_tmpBufferLen = 0;
        }
    }
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA) && m_samplerate)
            avPacket.pts = avPacket.dts =
                samplesToInternal(m_totalTHDSamples, m_samplerate);  // replace time to a next HD packet
    }
    return rez;
}
