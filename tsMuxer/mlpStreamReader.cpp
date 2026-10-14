#include "mlpStreamReader.h"

#include <fs/systemlog.h>

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
        const int sampling_frequency = (m_samplerate == 192000) ? 5 : (m_samplerate == 96000) ? 4 : 1;
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

int MLPStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        // thg packet
        // m_samplerate stays 0 until a major sync decodes; a stream that never produces one
        // must not divide by it.
        if (m_samplerate)
            avPacket.dts = avPacket.pts = samplesToInternal(m_totalTHDSamples, m_samplerate);

        m_totalTHDSamples += m_samples;
        m_demuxedTHDSamples += m_samples;
        if (m_demuxedTHDSamples >= m_samples)
        {
            m_demuxedTHDSamples -= m_samples;
        }
        return 0;
    }
}

int MLPStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA) && m_samplerate)
            avPacket.pts = avPacket.dts =
                samplesToInternal(m_totalTHDSamples, m_samplerate);  // replace time to a next HD packet
    }
    return rez;
}
