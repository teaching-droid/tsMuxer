// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "vorbisStreamReader.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "vod_common.h"

namespace
{
constexpr int FRAME_PREFIX_LEN = 4;
constexpr int MAX_VORBIS_PACKET = 1 << 20;
constexpr uint8_t PRIVATE_MARK[8] = {'V', 'O', 'R', 'B', 'I', 'S', 'C', 'P'};

// Vorbis packs its fields from the lowest bit of each byte up, unlike the readers elsewhere in tsMuxeR.
class LsbBitReader
{
   public:
    LsbBitReader(const uint8_t* data, const int size) : m_data(data), m_bits(static_cast<int64_t>(size) * 8), m_pos(0)
    {
    }
    uint32_t get(const int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++m_pos)
        {
            if (m_pos >= m_bits)
                throw std::exception();
            v |= static_cast<uint32_t>(m_data[m_pos >> 3] >> (m_pos & 7) & 1) << i;
        }
        return v;
    }
    void skip(const int64_t n)
    {
        m_pos += n;
        if (m_pos > m_bits)
            throw std::exception();
    }

   private:
    const uint8_t* m_data;
    int64_t m_bits;
    int64_t m_pos;
};

int ilog(uint32_t v)
{
    int n = 0;
    while (v)
    {
        ++n;
        v >>= 1;
    }
    return n;
}

// The largest r whose dimensions-th power is at most entries (Vorbis I, 9.2.3).
int64_t lookup1Values(const int64_t entries, const int dimensions)
{
    int64_t r = 0;
    while (true)
    {
        int64_t p = 1;
        for (int i = 0; i < dimensions && p <= entries; ++i) p *= r + 1;
        if (p > entries)
            return r;
        ++r;
    }
}
}  // namespace

VorbisStreamReader::VorbisStreamReader()
    : m_sampleRate(0),
      m_channels(0),
      m_nominalBitrate(0),
      m_blockSize{0, 0},
      m_modeBits(0),
      m_prevBlock(0),
      m_curFrameSamples(0),
      m_leadSamples(0)
{
}

const CodecInfo& VorbisStreamReader::getCodecInfo() { return vorbisCodecInfo; }

const std::string VorbisStreamReader::getStreamInfo()
{
    std::ostringstream str;
    if (m_nominalBitrate > 0)
        str << "Bitrate: " << m_nominalBitrate / 1000 << "Kbps (nominal)  ";
    str << "Sample Rate: " << m_sampleRate << "  Channels: " << static_cast<int>(m_channels);
    return str.str();
}

bool VorbisStreamReader::parseIdentification(const uint8_t* data, const int size)
{
    // Vorbis I, 4.2.2: packet type 1, "vorbis", version, channels, rate, three bit rates, the two block sizes as powers
    // of two in one byte, and the framing bit.
    if (size < 30 || data[0] != 1 || memcmp(data + 1, "vorbis", 6) != 0)
        return false;
    m_channels = data[11];
    m_sampleRate = static_cast<int>(data[12] | data[13] << 8 | data[14] << 16 | static_cast<uint32_t>(data[15]) << 24);
    m_nominalBitrate =
        static_cast<int>(data[20] | data[21] << 8 | data[22] << 16 | static_cast<uint32_t>(data[23]) << 24);
    m_blockSize[0] = 1 << (data[28] & 15);
    m_blockSize[1] = 1 << (data[28] >> 4);
    return m_channels > 0 && m_sampleRate > 0;
}

bool VorbisStreamReader::parseSetup(const uint8_t* data, const int size)
{
    // Vorbis I, 4.2.4. Only the modes at its end are needed, but everything before them has to be read to find them:
    // the codebooks, the time domain transforms, the floors, the residues and the mappings.
    if (size < 7 || data[0] != 5 || memcmp(data + 1, "vorbis", 6) != 0)
        return false;
    try
    {
        LsbBitReader bits(data + 7, size - 7);
        const int codebooks = static_cast<int>(bits.get(8)) + 1;
        for (int b = 0; b < codebooks; ++b)
        {
            if (bits.get(24) != 0x564342)
                return false;
            const int dimensions = static_cast<int>(bits.get(16));
            const int64_t entries = bits.get(24);
            if (bits.get(1))  // ordered
            {
                int64_t entry = 0;
                bits.skip(5);
                while (entry < entries)
                {
                    entry += bits.get(ilog(static_cast<uint32_t>(entries - entry)));
                    if (entry > entries)
                        return false;
                }
            }
            else
            {
                const bool sparse = bits.get(1);
                for (int64_t e = 0; e < entries; ++e)
                    if (!sparse || bits.get(1))
                        bits.skip(5);
            }
            const uint32_t lookup = bits.get(4);
            if (lookup == 1 || lookup == 2)
            {
                bits.skip(64);  // minimum and delta
                const int valueBits = static_cast<int>(bits.get(4)) + 1;
                bits.skip(1);  // sequence_p
                const int64_t values = lookup == 1 ? lookup1Values(entries, dimensions) : entries * dimensions;
                bits.skip(values * valueBits);
            }
            else if (lookup != 0)
                return false;
        }
        const int times = static_cast<int>(bits.get(6)) + 1;
        for (int i = 0; i < times; ++i)
            if (bits.get(16) != 0)
                return false;
        const int floors = static_cast<int>(bits.get(6)) + 1;
        for (int i = 0; i < floors; ++i)
        {
            const uint32_t type = bits.get(16);
            if (type == 0)
            {
                bits.skip(8 + 16 + 16 + 6 + 8);
                bits.skip(8 * (static_cast<int64_t>(bits.get(4)) + 1));
            }
            else if (type == 1)
            {
                const int partitions = static_cast<int>(bits.get(5));
                std::vector<int> partitionClass(partitions);
                int maxClass = -1;
                for (int& c : partitionClass)
                {
                    c = static_cast<int>(bits.get(4));
                    maxClass = (std::max)(maxClass, c);
                }
                std::vector<int> classDimensions(maxClass + 1);
                for (int c = 0; c <= maxClass; ++c)
                {
                    classDimensions[c] = static_cast<int>(bits.get(3)) + 1;
                    const int subclasses = static_cast<int>(bits.get(2));
                    if (subclasses)
                        bits.skip(8);
                    bits.skip(8 * (static_cast<int64_t>(1) << subclasses));
                }
                bits.skip(2);
                const int rangeBits = static_cast<int>(bits.get(4));
                for (const int c : partitionClass) bits.skip(static_cast<int64_t>(classDimensions[c]) * rangeBits);
            }
            else
                return false;
        }
        const int residues = static_cast<int>(bits.get(6)) + 1;
        for (int i = 0; i < residues; ++i)
        {
            if (bits.get(16) > 2)
                return false;
            bits.skip(24 + 24 + 24);
            const int classifications = static_cast<int>(bits.get(6)) + 1;
            bits.skip(8);
            std::vector<uint32_t> cascade(classifications);
            for (uint32_t& c : cascade)
            {
                const uint32_t low = bits.get(3);
                const uint32_t high = bits.get(1) ? bits.get(5) : 0;
                c = high << 3 | low;
            }
            for (const uint32_t c : cascade)
                for (int j = 0; j < 8; ++j)
                    if (c >> j & 1)
                        bits.skip(8);
        }
        const int mappings = static_cast<int>(bits.get(6)) + 1;
        const int channelBits = ilog(m_channels - 1u);
        for (int i = 0; i < mappings; ++i)
        {
            if (bits.get(16) != 0)
                return false;
            const int submaps = bits.get(1) ? static_cast<int>(bits.get(4)) + 1 : 1;
            if (bits.get(1))
                bits.skip((static_cast<int64_t>(bits.get(8)) + 1) * 2 * channelBits);
            if (bits.get(2) != 0)
                return false;
            if (submaps > 1)
                bits.skip(4 * static_cast<int64_t>(m_channels));
            bits.skip(24 * static_cast<int64_t>(submaps));
        }
        const int modes = static_cast<int>(bits.get(6)) + 1;
        m_modeLong.assign(modes, false);
        for (int i = 0; i < modes; ++i)
        {
            m_modeLong[i] = bits.get(1) != 0;
            bits.skip(16 + 16 + 8);
        }
        m_modeBits = ilog(static_cast<uint32_t>(modes - 1));
        return true;
    }
    catch (std::exception&)
    {
        return false;
    }
}

bool VorbisStreamReader::setCodecPrivate(const uint8_t* data, const int size)
{
    // Xiph lacing: the number of packets less one, the sizes of all but the last (each a run of 255s and one smaller
    // byte), then the packets.
    if (size < 3 || data[0] != 2)
        return false;
    int pos = 1;
    int sizes[2] = {0, 0};
    for (int& s : sizes)
    {
        while (pos < size && data[pos] == 255) s += data[pos++];
        if (pos >= size)
            return false;
        s += data[pos++];
    }
    if (pos + sizes[0] + sizes[1] > size)
        return false;
    const uint8_t* ident = data + pos;
    const uint8_t* setup = ident + sizes[0] + sizes[1];
    m_headers[0].assign(ident, ident + sizes[0]);
    m_headers[1].assign(ident + sizes[0], setup);
    m_headers[2].assign(setup, data + size);
    if (!parseIdentification(ident, sizes[0]) || !parseSetup(setup, static_cast<int>(size - (setup - data))))
        return false;
    // The same headers again are the next file of a join, and the stream goes on: the packet after them overlaps the
    // block before it as any other does. Other headers start a new stream.
    if (m_codecPrivate.size() != static_cast<size_t>(size) || memcmp(m_codecPrivate.data(), data, size) != 0)
        m_prevBlock = 0;
    m_codecPrivate.assign(data, data + size);
    return true;
}

uint8_t* VorbisStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    // The header packets, in front of the first packet of a file.
    if (end - buff >= 12 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        const int len =
            static_cast<int>(static_cast<uint32_t>(buff[8]) << 24 | buff[9] << 16 | buff[10] << 8 | buff[11]);
        if (end - buff < 12 + len)
            return nullptr;
        setCodecPrivate(buff + 12, len);
        buff += 12 + len;
    }
    if (buff + FRAME_PREFIX_LEN > end || m_modeLong.empty())
        return nullptr;
    const uint32_t len = static_cast<uint32_t>(buff[0]) << 24 | buff[1] << 16 | buff[2] << 8 | buff[3];
    if (len < 1 || len > MAX_VORBIS_PACKET)
        return nullptr;
    return buff + FRAME_PREFIX_LEN + static_cast<int>(len) <= end ? buff : nullptr;
}

int VorbisStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    // Every file of a join begins with its own header packets, in front of its first packet (findFrame reads them at
    // the start of the stream).
    int head = 0;
    if (end - buff >= 8 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        if (end - buff < 12)
            return NOT_ENOUGH_BUFFER;
        const int len =
            static_cast<int>(static_cast<uint32_t>(buff[8]) << 24 | buff[9] << 16 | buff[10] << 8 | buff[11]);
        if (end - buff < 12 + len)
            return NOT_ENOUGH_BUFFER;
        setCodecPrivate(buff + 12, len);
        head = 12 + len;
    }
    skipBeforeBytes = head + FRAME_PREFIX_LEN;
    if (end - buff < head + FRAME_PREFIX_LEN)
        return NOT_ENOUGH_BUFFER;
    buff += head;
    const uint32_t len = static_cast<uint32_t>(buff[0]) << 24 | buff[1] << 16 | buff[2] << 8 | buff[3];
    if (len == 0 || len > MAX_VORBIS_PACKET || m_modeLong.empty())
        return 0;
    if (end - buff < FRAME_PREFIX_LEN + static_cast<int64_t>(len))
        return NOT_ENOUGH_BUFFER;

    // An audio packet: type bit 0, then the mode.
    const uint8_t* packet = buff + FRAME_PREFIX_LEN;
    m_curFrameSamples = 0;
    if ((packet[0] & 1) == 0)
    {
        const uint32_t mode = m_modeBits ? (packet[0] >> 1) & ((1u << m_modeBits) - 1) : 0;
        if (m_modeBits > 7 || mode >= m_modeLong.size())
            return 0;
        const int block = m_blockSize[m_modeLong[mode] ? 1 : 0];
        m_curFrameSamples = (m_prevBlock ? m_prevBlock : block) / 4 + block / 4;
        if (m_prevBlock == 0 && m_leadSamples == 0)
            m_leadSamples = m_curFrameSamples;
        m_prevBlock = block;
    }
    return static_cast<int>(len);
}

int VorbisStreamReader::blockSizeOf(const uint8_t* packet, const int size) const
{
    if (size < 1 || (packet[0] & 1) != 0 || m_modeLong.empty() || m_modeBits > 7)
        return 0;
    const uint32_t mode = m_modeBits ? (packet[0] >> 1) & ((1u << m_modeBits) - 1) : 0;
    return mode < m_modeLong.size() ? m_blockSize[m_modeLong[mode] ? 1 : 0] : 0;
}

int VorbisStreamReader::oggPageHeader(uint8_t* dst, const uint8_t* dstEnd, const std::vector<int>& packets,
                                      const uint8_t headerType, const int64_t granule, const uint8_t* payload,
                                      const int payloadSize)
{
    std::vector<uint8_t> lacing;
    for (const int size : packets)
    {
        for (int left = size; left >= 255; left -= 255) lacing.push_back(255);
        lacing.push_back(static_cast<uint8_t>(size % 255));
    }
    const int headerSize = 27 + static_cast<int>(lacing.size());
    if (lacing.size() > 255 || dstEnd - dst < headerSize)
        return 0;
    uint8_t* p = dst;
    memcpy(p, "OggS", 4);
    p[4] = 0;  // version
    p[5] = headerType;
    for (int i = 0; i < 8; i++) p[6 + i] = static_cast<uint8_t>(granule >> (8 * i));
    for (int i = 0; i < 4; i++) p[14 + i] = static_cast<uint8_t>(0x56726273u >> (8 * i));  // serial "Vrbs"
    for (int i = 0; i < 4; i++) p[18 + i] = static_cast<uint8_t>(m_oggPageSeqNo >> (8 * i));
    memset(p + 22, 0, 4);  // CRC, below
    p[26] = static_cast<uint8_t>(lacing.size());
    memcpy(p + 27, lacing.data(), lacing.size());
    // CRC-32 with polynomial 0x04C11DB7, no reflection, over the header and the payload (RFC 3533, 6)
    uint32_t crc = 0;
    const auto add = [&crc](const uint8_t* b, const int n)
    {
        for (int i = 0; i < n; i++)
        {
            crc ^= static_cast<uint32_t>(b[i]) << 24;
            for (int k = 0; k < 8; k++) crc = crc & 0x80000000u ? crc << 1 ^ 0x04C11DB7u : crc << 1;
        }
    };
    add(p, headerSize);
    add(payload, payloadSize);
    for (int i = 0; i < 4; i++) p[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    m_oggPageSeqNo++;
    return headerSize;
}

int VorbisStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                          PriorityDataInfo* priorityData)
{
    if (!m_demuxMode)
        return 0;
    uint8_t* dst = dstBuffer;
    // The identification header on a page of its own, the first of the stream; the comment and setup headers on the
    // next (Vorbis I, A.2).
    if (!m_oggHeadersWritten && !m_headers[0].empty())
    {
        m_oggHeadersWritten = true;
        const int h0 = oggPageHeader(dst, dstEnd, {static_cast<int>(m_headers[0].size())}, 0x02, 0, m_headers[0].data(),
                                     static_cast<int>(m_headers[0].size()));
        if (h0 == 0 || dstEnd - (dst + h0) < static_cast<int64_t>(m_headers[0].size()))
            return 0;
        dst += h0;
        memcpy(dst, m_headers[0].data(), m_headers[0].size());
        dst += m_headers[0].size();
        std::vector<uint8_t> second(m_headers[1]);
        second.insert(second.end(), m_headers[2].begin(), m_headers[2].end());
        const int h1 =
            oggPageHeader(dst, dstEnd, {static_cast<int>(m_headers[1].size()), static_cast<int>(m_headers[2].size())},
                          0, 0, second.data(), static_cast<int>(second.size()));
        if (h1 == 0 || dstEnd - (dst + h1) < static_cast<int64_t>(second.size()))
            return static_cast<int>(dst - dstBuffer);
        dst += h1;
        memcpy(dst, second.data(), second.size());
        dst += second.size();
    }
    // The packet's page: the muxer writes the packet itself after it. Its granule position is the number of samples
    // decoded once the packet is: none for the first packet.
    if (avPacket.data != nullptr && avPacket.size > 0)
    {
        // From the packet itself, as the reader may already be further on: its mode gives its block, and it adds a
        // quarter of the block before it and a quarter of its own.
        if ((avPacket.data[0] & 1) == 0 && !m_modeLong.empty())
        {
            const uint32_t mode = m_modeBits ? (avPacket.data[0] >> 1) & ((1u << m_modeBits) - 1) : 0;
            if (mode < m_modeLong.size())
            {
                const int block = m_blockSize[m_modeLong[mode] ? 1 : 0];
                if (m_oggPrevBlock)
                    m_granulePos += m_oggPrevBlock / 4 + block / 4;
                m_oggPrevBlock = block;
            }
        }
        const int h = oggPageHeader(dst, dstEnd, {avPacket.size}, 0, m_granulePos, avPacket.data, avPacket.size);
        m_lastPage.assign(dst, dst + h);
        m_lastPage.insert(m_lastPage.end(), avPacket.data, avPacket.data + avPacket.size);
        m_lastPageHeader = h;
        dst += h;
    }
    return static_cast<int>(dst - dstBuffer);
}

bool VorbisStreamReader::beforeFileCloseEvent(File& file)
{
    if (!m_demuxMode || m_lastPage.size() < 27 || m_lastPageHeader < 27)
        return true;
    // The end of the stream (RFC 3533, 6), and a granule position short of the packet's end by the samples the source
    // trims there, which a player then leaves out (Vorbis I, A.2).
    const int64_t trim =
        m_sampleRate > 0 && m_endTrimNs > 0 ? (m_endTrimNs * m_sampleRate + 500000000) / 1000000000 : 0;
    const int64_t granule = (std::max)(int64_t{0}, m_granulePos - trim);
    uint8_t* p = m_lastPage.data();
    p[5] |= 0x04;
    for (int i = 0; i < 8; i++) p[6 + i] = static_cast<uint8_t>(granule >> (8 * i));
    memset(p + 22, 0, 4);
    uint32_t crc = 0;
    for (const uint8_t b : m_lastPage)
    {
        crc ^= static_cast<uint32_t>(b) << 24;
        for (int k = 0; k < 8; k++) crc = crc & 0x80000000u ? crc << 1 ^ 0x04C11DB7u : crc << 1;
    }
    for (int i = 0; i < 4; i++) p[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    file.sync();
    int64_t fileSize = 0;
    file.size(&fileSize);
    if (fileSize < static_cast<int64_t>(m_lastPage.size()) ||
        file.seek(fileSize - static_cast<int64_t>(m_lastPage.size()), File::SeekMethod::smBegin) == -1)
        return false;
    return file.write(p, m_lastPageHeader) == m_lastPageHeader;
}

double VorbisStreamReader::getFrameDuration()
{
    if (m_curFrameSamples == 0 || m_sampleRate == 0)
        return 0;
    return static_cast<double>(m_curFrameSamples) / m_sampleRate * INTERNAL_PTS_FREQ;
}
