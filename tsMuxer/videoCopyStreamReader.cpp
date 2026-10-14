#include "videoCopyStreamReader.h"

#include <cstring>
#include <string>

#include "avCodecs.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
uint64_t readBe(const uint8_t* p, const int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | p[i];
    return v;
}

std::string proresName(const uint32_t fourcc)
{
    switch (fourcc)
    {
    case 0x61703478:  // ap4x
        return "ProRes 4444 XQ";
    case 0x61703468:  // ap4h
        return "ProRes 4444";
    case 0x61706368:  // apch
        return "ProRes 422 HQ";
    case 0x6170636E:  // apcn
        return "ProRes 422";
    case 0x61706373:  // apcs
        return "ProRes 422 LT";
    case 0x6170636F:  // apco
        return "ProRes 422 Proxy";
    default:
        return "ProRes";
    }
}
}  // namespace

VideoCopyStreamReader::VideoCopyStreamReader(const bool prores) : m_prores(prores) {}

const CodecInfo& VideoCopyStreamReader::getCodecInfo() { return m_prores ? proresCodecInfo : mpeg4AspCodecInfo; }

CheckStreamRez VideoCopyStreamReader::checkStream(const uint8_t* buffer, const int len, const bool prores)
{
    CheckStreamRez rez;
    if (len < 20 || memcmp(buffer, "VCFG", 4) != 0)
        return rez;
    const auto width = static_cast<unsigned>(readBe(buffer + 4, 2));
    const auto height = static_cast<unsigned>(readBe(buffer + 6, 2));
    const auto fourcc = static_cast<uint32_t>(readBe(buffer + 8, 4));
    rez.codecInfo = prores ? proresCodecInfo : mpeg4AspCodecInfo;
    rez.streamDescr = std::string(prores ? proresName(fourcc) : "MPEG-4 Part 2 video") +
                      ". Resolution: " + std::to_string(width) + ":" + std::to_string(height) +
                      ". Copied into a Matroska output only";
    return rez;
}

void VideoCopyStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, bool /*lastBlock*/)
{
    // What is left of the last delivery first, then the new bytes, which start getTmpBufferSize() bytes into data
    if (m_inputPos > 0)
    {
        m_input.erase(m_input.begin(), m_input.begin() + static_cast<std::ptrdiff_t>(m_inputPos));
        m_inputPos = 0;
    }
    if (data && dataLen > 0)
        m_input.insert(m_input.end(), data + MAX_AV_PACKET_SIZE, data + MAX_AV_PACKET_SIZE + dataLen);
}

int VideoCopyStreamReader::readPacket(AVPacket& avPacket)
{
    avPacket.codec = this;
    avPacket.codecID = getCodecInfo().codecID;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;

    for (;;)
    {
        const size_t left = m_input.size() - m_inputPos;
        if (left < 4)
            return NEED_MORE_DATA;
        const uint8_t* p = m_input.data() + m_inputPos;
        if (memcmp(p, "VCFG", 4) == 0)
        {
            if (left < 16)
                return NEED_MORE_DATA;
            const auto cfgLen = static_cast<size_t>(readBe(p + 12, 4));
            if (left < 16 + cfgLen)
                return NEED_MORE_DATA;
            m_width = static_cast<unsigned>(readBe(p + 4, 2));
            m_height = static_cast<unsigned>(readBe(p + 6, 2));
            m_fourcc = static_cast<uint32_t>(readBe(p + 8, 4));
            m_config.assign(p + 16, p + 16 + cfgLen);
            m_inputPos += 16 + cfgLen;
            m_processedSize += static_cast<int64_t>(16 + cfgLen);
            continue;
        }
        static constexpr size_t HEAD = 4 + 8 + 8 + 8 + 1 + 4;
        if (memcmp(p, "VCPY", 4) != 0)
            THROW(ERR_COMMON, "A copied video track lost its framing at byte " << m_processedSize << " of its data.")
        if (left < HEAD)
            return NEED_MORE_DATA;
        const auto dataSize = static_cast<size_t>(readBe(p + 29, 4));
        if (left < HEAD + dataSize)
            return NEED_MORE_DATA;
        const auto pts = static_cast<int64_t>(readBe(p + 4, 8));
        const auto dts = static_cast<int64_t>(readBe(p + 12, 8));
        const auto duration = static_cast<int64_t>(readBe(p + 20, 8));
        const bool key = p[28] != 0;

        m_packet.clear();
        // MPEG-4 Part 2 in Matroska has no CodecPrivate (the codec specifications of Matroska): its configuration goes
        // in front of the first picture, where a decoder finds it as it does in an elementary stream
        if (!m_configSent && !m_prores)
            m_packet.insert(m_packet.end(), m_config.begin(), m_config.end());
        m_configSent = true;
        m_packet.insert(m_packet.end(), p + HEAD, p + HEAD + dataSize);

        if (m_frames == 0)
            m_firstDuration = duration;
        else if (m_frames == 1 && m_fps == 0 && m_firstDuration > 0)
            m_fps = static_cast<double>(INTERNAL_PTS_FREQ) / static_cast<double>(m_firstDuration);
        avPacket.pts = pts;
        avPacket.dts = dts;
        avPacket.duration = duration > 0 ? duration : 0;
        avPacket.flags = key ? AVPacket::IS_IFRAME : 0;
        avPacket.data = m_packet.data();
        avPacket.size = static_cast<int>(m_packet.size());
        m_inputPos += HEAD + dataSize;
        m_processedSize += static_cast<int64_t>(HEAD + dataSize);
        ++m_frames;
        return 0;
    }
}

int VideoCopyStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = readPacket(avPacket);
    if (rez == NEED_MORE_DATA)
    {
        avPacket.data = nullptr;
        avPacket.size = 0;
        return 0;
    }
    return rez;
}
