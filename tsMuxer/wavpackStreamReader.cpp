#include "wavpackStreamReader.h"

#include <fs/systemlog.h>

#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
constexpr int FRAME_PREFIX_LEN = 4;
constexpr int MAX_WAVPACK_FRAME = 1 << 24;  // the blocks of a frame together; a block is at most 1 MB
constexpr uint8_t PRIVATE_MARK[8] = {'W', 'V', 'P', 'K', 'C', 'P', 'R', 'V'};
// the flags of a block (WavPack 4 and 5 block format, 2.0)
constexpr uint32_t HYBRID_FLAG = 8;
constexpr uint32_t FLOAT_DATA = 0x80;
constexpr uint32_t DSD_FLAG = 0x80000000U;

uint32_t be32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
}

uint32_t le32(const uint8_t* p)
{
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
}  // namespace

const CodecInfo& WavpackStreamReader::getCodecInfo() { return wavpackCodecInfo; }

const std::string WavpackStreamReader::getStreamInfo()
{
    std::ostringstream str;
    str << "Sample Rate: " << m_sampleRate << "  Channels: " << static_cast<int>(m_channels);
    if (m_bitDepth > 0)
        str << "  Bits per sample: " << m_bitDepth << "bit";
    if (m_haveFlags)
        str << ((m_flags & FLOAT_DATA) ? "  floating point" : "")
            << ((m_flags & HYBRID_FLAG) ? "  hybrid" : "  lossless");
    return str.str();
}

// The CodecPrivate (2 byte version) and the sample rate (4), channels (1) and bits per sample (1) behind it
bool WavpackStreamReader::setPrivate(const uint8_t* data, const int size)
{
    if (size < 8)
        return false;
    m_codecPrivate.assign(data, data + 2);
    m_sampleRate = static_cast<int>(le32(data + 2));
    m_channels = data[6];
    m_bitDepth = data[7];
    return m_sampleRate > 0 && m_channels > 0;
}

uint8_t* WavpackStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    if (end - buff >= 12 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        const int len = static_cast<int>(be32(buff + 8));
        if (end - buff < 12 + len)
            return nullptr;
        setPrivate(buff + 12, len);
        buff += 12 + len;
    }
    if (buff + FRAME_PREFIX_LEN > end || m_sampleRate == 0)
        return nullptr;
    const uint32_t len = be32(buff);
    if (len < 12 || len > MAX_WAVPACK_FRAME)
        return nullptr;
    return buff + FRAME_PREFIX_LEN + static_cast<int>(len) <= end ? buff : nullptr;
}

int WavpackStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    // every file of a join begins with its own CodecPrivate, in front of its first frame
    int head = 0;
    if (end - buff >= 8 && memcmp(buff, PRIVATE_MARK, 8) == 0)
    {
        if (end - buff < 12)
            return NOT_ENOUGH_BUFFER;
        const int len = static_cast<int>(be32(buff + 8));
        if (end - buff < 12 + len)
            return NOT_ENOUGH_BUFFER;
        setPrivate(buff + 12, len);
        head = 12 + len;
    }
    skipBeforeBytes = head + FRAME_PREFIX_LEN;
    if (end - buff < head + FRAME_PREFIX_LEN)
        return NOT_ENOUGH_BUFFER;
    buff += head;
    const uint32_t len = be32(buff);
    if (len < 12 || len > MAX_WAVPACK_FRAME || m_sampleRate == 0)
        return 0;
    if (end - buff < FRAME_PREFIX_LEN + static_cast<int64_t>(len))
        return NOT_ENOUGH_BUFFER;
    // the frame begins with block_samples and the flags of its first block
    const uint8_t* f = buff + FRAME_PREFIX_LEN;
    const uint32_t flags = le32(f + 4);
    if (flags & DSD_FLAG)
        THROW(ERR_COMMON, "The WavPack track holds DSD audio, which tsMuxeR does not read.")
    if (!m_haveFlags)
    {
        m_flags = flags;
        m_haveFlags = true;
        if (m_demuxMode && (flags & HYBRID_FLAG))
            LTRACE(LT_WARN, 2,
                   "Warning: track " << m_streamIndex
                                     << " is hybrid WavPack. The demux writes its lossy part as a .wv file; a "
                                        "correction part the source holds is not written, so the sound is not "
                                        "lossless. A Matroska output keeps both parts.");
    }
    m_curFrameSamples = static_cast<int>(le32(f));
    return static_cast<int>(len);
}

// The frame as Matroska holds it (block_samples, then for each block flags, crc, its size where the frame has more than
// one block, and its data) back into the blocks of a .wv file: "wvpk", the size of the rest, the version of the
// CodecPrivate, the block_index counted from 0 (40 bits, its top 8 in block_index_u8), the total of samples unknown
// (0xFFFFFFFF), block_samples, flags, crc, the data.
int WavpackStreamReader::readPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::readPacket(avPacket);
    if (m_demuxMode && rez == 0 && avPacket.data != nullptr && avPacket.size > 0)
        toWvBlocks(avPacket);
    return rez;
}

// the last frame, handed on when the stream ends: as every other one
int WavpackStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (m_demuxMode && avPacket.data != nullptr && avPacket.size > 0)
        toWvBlocks(avPacket);
    return rez;
}

// The frame as Matroska holds it (block_samples, then for each block flags, crc, its size where the frame has more than
// one block, and its data) back into the blocks of a .wv file: "wvpk", the size of the rest, the version of the
// CodecPrivate, the block_index counted from 0 (40 bits, its top 8 in block_index_u8), the total of samples unknown
// (0xFFFFFFFF), block_samples, flags, crc, the data. A frame whose blocks do not fill it exactly is damaged: it is left
// out, as a .wv file of it would not be read.
void WavpackStreamReader::toWvBlocks(AVPacket& avPacket)
{
    const uint8_t* p = avPacket.data;
    const uint8_t* end = p + avPacket.size;
    m_wvBlocks.clear();
    if (end - p < 12)
    {
        avPacket.size = 0;
        return;
    }
    const uint32_t samples = le32(p);
    p += 4;
    const auto put32 = [this](const uint32_t v)
    {
        for (int i = 0; i < 4; ++i) m_wvBlocks.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    while (end - p >= 8)
    {
        const uint32_t flags = le32(p);
        const uint32_t crc = le32(p + 4);
        p += 8;
        uint32_t size = static_cast<uint32_t>(end - p);
        if ((flags & 0x1800) != 0x1800)  // not both the initial and the final block: its size follows
        {
            if (end - p < 4)
                break;
            size = le32(p);
            p += 4;
            if (size > static_cast<uint32_t>(end - p))
                break;
        }
        for (const char c : {'w', 'v', 'p', 'k'}) m_wvBlocks.push_back(static_cast<uint8_t>(c));
        put32(24 + size);
        m_wvBlocks.push_back(m_codecPrivate.size() >= 2 ? m_codecPrivate[0] : 0x10);
        m_wvBlocks.push_back(m_codecPrivate.size() >= 2 ? m_codecPrivate[1] : 0x04);
        m_wvBlocks.push_back(static_cast<uint8_t>(m_demuxIndex >> 32));
        m_wvBlocks.push_back(0);  // total_samples_u8: with total_samples 0xFFFFFFFF, the total is unknown
        put32(0xFFFFFFFFU);
        put32(static_cast<uint32_t>(m_demuxIndex));
        put32(samples);
        put32(flags);
        put32(crc);
        m_wvBlocks.insert(m_wvBlocks.end(), p, p + size);
        p += size;
    }
    if (p != end)
    {
        m_wvBlocks.clear();
        avPacket.size = 0;
        return;
    }
    m_demuxIndex += samples;
    avPacket.data = m_wvBlocks.data();
    avPacket.size = static_cast<int>(m_wvBlocks.size());
}

double WavpackStreamReader::getFrameDuration()
{
    if (m_curFrameSamples == 0 || m_sampleRate == 0)
        return 0;
    return static_cast<double>(m_curFrameSamples) / m_sampleRate * INTERNAL_PTS_FREQ;
}
