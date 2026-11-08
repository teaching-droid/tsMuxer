// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "alacStreamReader.h"

#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "vod_common.h"

namespace
{
constexpr int FRAME_PREFIX_LEN = 4;
constexpr int MAX_ALAC_PACKET = 1 << 22;
constexpr uint8_t PRIVATE_MARK[8] = {'A', 'L', 'A', 'C', 'C', 'P', 'R', 'V'};

uint32_t be32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
}
}  // namespace

AlacStreamReader::AlacStreamReader()
    : m_frameLength(0), m_bitDepth(0), m_channels(0), m_sampleRate(0), m_curFrameSamples(0)
{
}

const CodecInfo& AlacStreamReader::getCodecInfo() { return alacCodecInfo; }

const std::string AlacStreamReader::getStreamInfo()
{
    std::ostringstream str;
    str << "Sample Rate: " << m_sampleRate << "  Channels: " << static_cast<int>(m_channels)
        << "  Bits per sample: " << m_bitDepth << "bit";
    return str.str();
}

bool AlacStreamReader::setCodecPrivate(const uint8_t* data, const int size)
{
    // ALACSpecificConfig, big endian: frameLength (32), compatibleVersion, bitDepth, pb, mb, kb, numChannels (8 each),
    // maxRun (16), maxFrameBytes, avgBitRate, sampleRate (32 each). Some writers put the 12 byte header of an 'alac'
    // atom in front of it.
    const uint8_t* cfg = data;
    int len = size;
    if (len >= 36 && memcmp(cfg + 4, "alac", 4) == 0)
    {
        cfg += 12;
        len -= 12;
    }
    if (len < 24 || cfg[4] != 0)
        return false;
    m_frameLength = be32(cfg);
    m_bitDepth = cfg[5];
    m_channels = cfg[9];
    m_sampleRate = static_cast<int>(be32(cfg + 20));
    if (m_frameLength == 0 || m_channels == 0 || m_sampleRate == 0)
        return false;
    m_codecPrivate.assign(data, data + size);
    return true;
}

uint8_t* AlacStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    if (end - buff >= 12 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        const int len = static_cast<int>(be32(buff + 8));
        if (end - buff < 12 + len)
            return nullptr;
        setCodecPrivate(buff + 12, len);
        buff += 12 + len;
    }
    if (buff + FRAME_PREFIX_LEN > end || m_frameLength == 0)
        return nullptr;
    const uint32_t len = be32(buff);
    if (len < 1 || len > MAX_ALAC_PACKET)
        return nullptr;
    return buff + FRAME_PREFIX_LEN + static_cast<int>(len) <= end ? buff : nullptr;
}

int AlacStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    // Every file of a join begins with its own configuration, in front of its first frame.
    int head = 0;
    if (end - buff >= 8 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        if (end - buff < 12)
            return NOT_ENOUGH_BUFFER;
        const int len = static_cast<int>(be32(buff + 8));
        if (end - buff < 12 + len)
            return NOT_ENOUGH_BUFFER;
        setCodecPrivate(buff + 12, len);
        head = 12 + len;
    }
    skipBeforeBytes = head + FRAME_PREFIX_LEN;
    if (end - buff < head + FRAME_PREFIX_LEN)
        return NOT_ENOUGH_BUFFER;
    buff += head;
    const uint32_t len = be32(buff);
    if (len == 0 || len > MAX_ALAC_PACKET || m_frameLength == 0)
        return 0;
    if (end - buff < FRAME_PREFIX_LEN + static_cast<int64_t>(len))
        return NOT_ENOUGH_BUFFER;

    // The frame's first element: its tag (3 bits), for a channel element (single, pair or LFE) the instance tag (4)
    // and 12 unused bits, then partialFrame, bytesShifted (2) and escapeFlag. A partial frame gives its sample count
    // in the 32 bits after that.
    const uint8_t* f = buff + FRAME_PREFIX_LEN;
    m_curFrameSamples = static_cast<int>(m_frameLength);
    const int tag = f[0] >> 5;
    if ((tag == 0 || tag == 1 || tag == 3) && len >= 10)
    {
        // bit 19 counted from the start of the frame is partialFrame; the count starts at bit 23
        if (f[2] >> 4 & 1)
        {
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) bits = bits << 8 | f[2 + i];  // bits 16..79
            const auto samples = static_cast<uint32_t>(bits >> (64 - 7 - 32));
            if (samples > 0 && samples <= m_frameLength)
                m_curFrameSamples = static_cast<int>(samples);
        }
    }
    return static_cast<int>(len);
}

double AlacStreamReader::getFrameDuration()
{
    if (m_curFrameSamples == 0 || m_sampleRate == 0)
        return 0;
    return static_cast<double>(m_curFrameSamples) / m_sampleRate * INTERNAL_PTS_FREQ;
}
