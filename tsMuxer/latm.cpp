#include "latm.h"

#include "aac.h"
#include "bitStream.h"

namespace
{
// A bit string that begins at a bit position, moved to begin on a byte: an AudioSpecificConfig inside a
// StreamMuxConfig is not byte aligned, and the alignment of a program config element in it counts from its start.
std::vector<uint8_t> bitsFrom(const uint8_t* data, const int size, const int bitPos)
{
    std::vector<uint8_t> out;
    const int shift = bitPos % 8;
    for (int i = bitPos / 8; i < size; ++i)
        out.push_back(static_cast<uint8_t>(
            shift == 0 ? data[i] : data[i] << shift | (i + 1 < size ? data[i + 1] >> (8 - shift) : 0)));
    return out;
}
}  // namespace

void LatmToAdts::convert(const uint8_t* data, const size_t size, std::vector<uint8_t>& out)
{
    if (!m_problem.empty())
        return;
    m_pending.insert(m_pending.end(), data, data + size);
    size_t pos = 0;
    while (m_pending.size() - pos >= 3)
    {
        // AudioSyncStream: syncword 0x2B7 (11 bits), audioMuxLengthBytes (13 bits), the AudioMuxElement
        if (m_pending[pos] != 0x56 || (m_pending[pos + 1] & 0xE0) != 0xE0)
        {
            ++pos;  // not on a frame: damaged data, or the stream began inside a frame
            continue;
        }
        const size_t length = static_cast<size_t>(m_pending[pos + 1] & 0x1F) << 8 | m_pending[pos + 2];
        if (m_pending.size() - pos < 3 + length)
            break;
        frame(m_pending.data() + pos + 3, static_cast<int>(length), out);
        if (!m_problem.empty())
            return;
        pos += 3 + length;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(pos));
}

void LatmToAdts::frame(const uint8_t* data, const int size, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> buf(data, data + size);
    buf.resize(buf.size() + 8);  // the reader may look ahead of the end it is given
    BitStreamReader reader{};
    reader.setBuffer(buf.data(), buf.data() + size);
    try
    {
        if (!reader.getBit())  // useSameStreamMux 0: a StreamMuxConfig follows
        {
            int bitPos = 1;
            if (!readStreamMuxConfig(buf.data(), size, bitPos))
                return;
            reader.skipBits(static_cast<unsigned>(bitPos - 1));
        }
        if (!m_haveConfig)
            return;  // the stream began between two configs: this frame cannot be read
        // PayloadLengthInfo of frameLengthType 0: bytes that add up while they are 255
        int length = 0;
        unsigned byte;
        do
        {
            byte = reader.getBits(8);
            length += static_cast<int>(byte);
        } while (byte == 255);
        const int adtsSize = 7 + length;
        if (length <= 0 || adtsSize > 0x1FFF ||
            static_cast<int64_t>(size) * 8 - reader.getBitsCount() < static_cast<int64_t>(length) * 8)
            return;  // a damaged frame
        const uint8_t header[7] = {
            0xFF,
            0xF1,  // MPEG-4, layer 0, no CRC
            static_cast<uint8_t>(m_config.profile << 6 | m_config.rateIndex << 2 | m_config.channelCfg >> 2),
            static_cast<uint8_t>((m_config.channelCfg & 3) << 6 | adtsSize >> 11),
            static_cast<uint8_t>(adtsSize >> 3),
            static_cast<uint8_t>((adtsSize & 7) << 5 | 0x1F),  // buffer fullness 0x7FF
            0xFC};                                             // one raw data block
        out.insert(out.end(), header, header + 7);
        // the payload is not byte aligned in the frame
        for (int i = 0; i < length; ++i) out.push_back(static_cast<uint8_t>(reader.getBits(8)));
    }
    catch (BitStreamException&)
    {
        // a frame shorter than its fields say: left out
    }
}

// StreamMuxConfig (ISO/IEC 14496-3, 1.7.3.1) from bitPos, its first field audioMuxVersion; bitPos is left after it.
bool LatmToAdts::readStreamMuxConfig(const uint8_t* data, const int size, int& bitPos)
{
    BitStreamReader reader{};
    std::vector<uint8_t> buf(data, data + size);
    buf.resize(buf.size() + 8);
    reader.setBuffer(buf.data(), buf.data() + size);
    reader.skipBits(static_cast<unsigned>(bitPos));
    if (reader.getBit())
    {
        m_problem = "it uses audioMuxVersion 1, which DVB does not use and tsMuxeR does not read";
        return false;
    }
    const bool sameTimeFraming = reader.getBit();
    const unsigned subFrames = reader.getBits(6);
    const unsigned programs = reader.getBits(4);
    const unsigned layers = reader.getBits(3);
    if (!sameTimeFraming || subFrames != 0 || programs != 0 || layers != 0)
    {
        m_problem = "it carries " + std::to_string(subFrames + 1) + " access units, " + std::to_string(programs + 1) +
                    " programs and " + std::to_string(layers + 1) +
                    " layers in a frame, where DVB uses one of each, and only that form is read";
        return false;
    }

    // AudioSpecificConfig, read from where it begins
    const int ascAt = reader.getBitsCount();
    std::vector<uint8_t> asc = bitsFrom(data, size, ascAt);
    const size_t ascBytes = asc.size();
    asc.resize(ascBytes + 8);
    BitStreamReader config{};
    config.setBuffer(asc.data(), asc.data() + ascBytes);
    const auto objectType = [&config]
    {
        const unsigned type = config.getBits(5);
        return type == 31 ? 32 + config.getBits(6) : type;
    };
    unsigned type = objectType();
    const unsigned rateIndex = config.getBits(4);
    if (rateIndex == 15)
    {
        m_problem = "its sample rate is not one of the table of AAC, and an ADTS header cannot give it";
        return false;
    }
    const unsigned channelCfg = config.getBits(4);
    if (type == 5 || type == 29)  // HE-AAC, HE-AAC v2: the output rate, then the core
    {
        if (config.getBits(4) == 15)
            config.skipBits(24);
        type = objectType();
    }
    if (type < 1 || type > 4 || channelCfg > 7)
    {
        m_problem = "its audio object type is " + std::to_string(type) + " with channel configuration " +
                    std::to_string(channelCfg) + ", and only AAC Main, LC, SSR and LTP fit an ADTS header";
        return false;
    }
    // GASpecificConfig
    if (config.getBit())
    {
        m_problem = "its frames hold 960 samples, and only frames of 1024 samples are read";
        return false;
    }
    if (config.getBit())      // dependsOnCoreCoder
        config.skipBits(14);  // coreCoderDelay
    const bool extension = config.getBit();
    AacProgramConfig pce;
    if (channelCfg == 0 && !readProgramConfig(config, pce))
    {
        m_problem = "its program config element names no channel";
        return false;
    }
    if (extension)
        config.skipBit();  // extensionFlag3
    reader.skipBits(static_cast<unsigned>(config.getBitsCount()));

    const unsigned frameLengthType = reader.getBits(3);
    if (frameLengthType != 0)
    {
        m_problem = "it gives its frame lengths in another form (frameLengthType " + std::to_string(frameLengthType) +
                    "), where DVB gives them in bytes";
        return false;
    }
    reader.skipBits(8);   // latmBufferFullness
    if (reader.getBit())  // otherDataPresent: otherDataLenBits, in escaped bytes
        while (true)
        {
            const bool more = reader.getBit();
            reader.skipBits(8);
            if (!more)
                break;
        }
    if (reader.getBit())  // crcCheckPresent
        reader.skipBits(8);

    m_config.profile = static_cast<int>(type) - 1;
    m_config.rateIndex = static_cast<int>(rateIndex);
    m_config.channelCfg = static_cast<int>(channelCfg);
    m_haveConfig = true;
    bitPos = reader.getBitsCount();
    return true;
}
