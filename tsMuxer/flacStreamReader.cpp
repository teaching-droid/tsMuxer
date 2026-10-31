#include "flacStreamReader.h"

#include <fs/systemlog.h>

#include <array>
#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "vodCoreException.h"
#include "vod_common.h"

// ---------------------------------------------------------------------------
// FLAC format constants
// ---------------------------------------------------------------------------

// FLAC frame sync code: 14 bits = 0x3FFE, followed by a reserved 0 bit and
// a blocking-strategy bit.  The two possible first-two-byte patterns are:
//   0xFFF8  (fixed block size)
//   0xFFF9  (variable block size)
static constexpr uint16_t FLAC_SYNC_FIXED = 0xFFF8;
static constexpr uint16_t FLAC_SYNC_VARIABLE = 0xFFF9;

// STREAMINFO is always 34 bytes (not counting the metadata block header).
static constexpr int STREAMINFO_SIZE = 34;

// "fLaC" file marker
static constexpr uint8_t FLAC_MARKER[4] = {'f', 'L', 'a', 'C'};

// Sample rate table from the FLAC specification (4 bits, index 0-11 are fixed).
static constexpr int kSampleRateTable[] = {
    0,       // 0: get from STREAMINFO
    88200,   // 1
    176400,  // 2
    192000,  // 3
    8000,    // 4
    16000,   // 5
    22050,   // 6
    24000,   // 7
    32000,   // 8
    44100,   // 9
    48000,   // 10
    96000,   // 11
    0,       // 12: get 8-bit rate in kHz from end of header
    0,       // 13: get 16-bit rate in Hz from end of header
    0,       // 14: get 16-bit rate in tens of Hz from end of header
    0,       // 15: invalid
};

// Block size table (4-bit index).
static constexpr int kBlockSizeTable[] = {
    0,      // 0: reserved
    192,    // 1
    576,    // 2
    1152,   // 3
    2304,   // 4
    4608,   // 5
    0,      // 6: get 8-bit value from end of header
    0,      // 7: get 16-bit value from end of header
    256,    // 8
    512,    // 9
    1024,   // 10
    2048,   // 11
    4096,   // 12
    8192,   // 13
    16384,  // 14
    32768,  // 15
};

// Bits-per-sample table (3-bit index).
static constexpr int kBpsTable[] = {
    0,   // 0: get from STREAMINFO
    8,   // 1
    12,  // 2
    0,   // 3: reserved
    16,  // 4
    20,  // 5
    24,  // 6
    32,  // 7  (FLAC spec says reserved, but some encoders use it)
};

// ---------------------------------------------------------------------------
// CRC-8 for FLAC frame header validation (polynomial 0x07)
// ---------------------------------------------------------------------------

static const uint8_t kCRC8Table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D, 0x70, 0x77, 0x7E,
    0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D, 0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB,
    0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD, 0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8,
    0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD, 0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6,
    0xE3, 0xE4, 0xED, 0xEA, 0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D,
    0x9A, 0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A, 0x57, 0x50,
    0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A, 0x89, 0x8E, 0x87, 0x80, 0x95,
    0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4, 0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC,
    0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4, 0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F,
    0x58, 0x4D, 0x4A, 0x43, 0x44, 0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A,
    0x33, 0x34, 0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63, 0x3E,
    0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13, 0xAE, 0xA9, 0xA0, 0xA7,
    0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83, 0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC,
    0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};

static uint8_t flac_crc8(const uint8_t* data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc = kCRC8Table[crc ^ data[i]];
    return crc;
}

// CRC-16 of a whole frame (polynomial 0x8005, initial value 0), stored in the last two bytes of every FLAC frame.
static uint16_t flac_crc16(const uint8_t* data, const size_t len)
{
    static const std::array<uint16_t, 256> table = []
    {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; ++i)
        {
            auto c = static_cast<uint16_t>(i << 8);
            for (int k = 0; k < 8; ++k)
                c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x8005) : static_cast<uint16_t>(c << 1);
            t[i] = c;
        }
        return t;
    }();
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) crc = static_cast<uint16_t>((crc << 8) ^ table[(crc >> 8) ^ data[i]]);
    return crc;
}

// The largest frame FLAC allows: 65535 samples of 8 channels at 32 bits, and its headers.
static constexpr ptrdiff_t MAX_FRAME_BYTES = 65535 * 8 * 4 + 1024;

// ---------------------------------------------------------------------------
// FLACStreamReader
// ---------------------------------------------------------------------------

FLACStreamReader::FLACStreamReader()
    : m_testMode(false),
      m_firstFrame(true),
      m_sampleRate(0),
      m_channels(0),
      m_bitsPerSample(0),
      m_minBlockSize(0),
      m_maxBlockSize(0),
      m_curBlockSize(0)
{
}

const CodecInfo& FLACStreamReader::getCodecInfo() { return flacCodecInfo; }

const std::string FLACStreamReader::getStreamInfo()
{
    std::ostringstream str;
    str << "Sample Rate: " << m_sampleRate << "  Channels: " << static_cast<int>(m_channels)
        << "  Bits per sample: " << m_bitsPerSample;
    return str.str();
}

// ---------------------------------------------------------------------------
// STREAMINFO parsing
// ---------------------------------------------------------------------------

bool FLACStreamReader::parseStreamInfo(const uint8_t* data, const int size)
{
    if (size < STREAMINFO_SIZE)
        return false;

    // STREAMINFO layout (34 bytes):
    //   [0..1]   minimum block size (samples)
    //   [2..3]   maximum block size (samples)
    //   [4..6]   minimum frame size (bytes)   -- 24 bits
    //   [7..9]   maximum frame size (bytes)   -- 24 bits
    //   [10..12] sample rate (20 bits) | channels-1 (3 bits) | bps-1 (5 bits)  ...
    //   [13]     ... | total samples (upper 4 bits)
    //   [14..17] total samples (lower 32 bits)
    //   [18..33] MD5 signature

    m_minBlockSize = (data[0] << 8) | data[1];
    m_maxBlockSize = (data[2] << 8) | data[3];

    m_sampleRate = (data[10] << 12) | (data[11] << 4) | (data[12] >> 4);
    m_channels = static_cast<uint8_t>(((data[12] >> 1) & 0x07) + 1);
    m_bitsPerSample = (((data[12] & 0x01) << 4) | (data[13] >> 4)) + 1;

    if (m_sampleRate == 0 || m_channels == 0 || m_bitsPerSample == 0)
        return false;

    return true;
}

void FLACStreamReader::setCodecPrivate(const uint8_t* data, const int size)
{
    if (data == nullptr || size <= 0)
        return;

    m_codecPrivate.assign(data, data + size);

    // Try to parse STREAMINFO from the CodecPrivate.
    // Expected layout: "fLaC" (4) + block header (4) + STREAMINFO (34)
    const uint8_t* p = data;
    int remaining = size;

    // Skip the "fLaC" marker if present
    if (remaining >= 4 && memcmp(p, FLAC_MARKER, 4) == 0)
    {
        p += 4;
        remaining -= 4;
    }

    // Read metadata block header: 1 byte (last-block flag + type) + 3 bytes (length)
    if (remaining >= 4)
    {
        const int blockType = p[0] & 0x7F;
        const int blockLen = (p[1] << 16) | (p[2] << 8) | p[3];
        p += 4;
        remaining -= 4;

        if (blockType == 0 && blockLen >= STREAMINFO_SIZE && remaining >= STREAMINFO_SIZE)
        {
            parseStreamInfo(p, remaining);
        }
    }
}

void FLACStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // fills sampleRate, channels
    data.bitsPerSample = m_bitsPerSample;
    if (!m_codecPrivate.empty())
        data.codecPrivate = m_codecPrivate;
}

void FLACStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    // Prefer codec-private blob (fLaC + STREAMINFO) -- it contains the
    // authoritative sample rate, channel count, and bits per sample.
    if (!data.codecPrivate.empty())
    {
        setCodecPrivate(data.codecPrivate.data(), static_cast<int>(data.codecPrivate.size()));
    }
    else
    {
        if (data.sampleRate > 0)
            m_sampleRate = data.sampleRate;
        if (data.channels > 0)
            m_channels = static_cast<uint8_t>(data.channels);
        if (data.bitsPerSample > 0)
            m_bitsPerSample = data.bitsPerSample;
    }
}

// ---------------------------------------------------------------------------
// Frame detection
// ---------------------------------------------------------------------------

void FLACStreamReader::countHeader(const int64_t len)
{
    if (!m_headerCounted)
        m_tagBytes += len;
    m_headerCounted = true;
}

uint8_t* FLACStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    // Check for "fLaC" magic at the beginning (standalone FLAC file).
    // Skip the file header (marker + all metadata blocks) to reach audio frames.
    if (end - buff >= 4 && memcmp(buff, FLAC_MARKER, 4) == 0)
    {
        const uint8_t* p = buff + 4;
        while (p + 4 <= end)
        {
            const bool isLast = (p[0] & 0x80) != 0;
            const int blockType = p[0] & 0x7F;
            const int blockLen = (p[1] << 16) | (p[2] << 8) | p[3];
            p += 4;

            // Parse STREAMINFO if we haven't yet
            if (blockType == 0 && blockLen >= STREAMINFO_SIZE && m_sampleRate == 0)
            {
                if (p + STREAMINFO_SIZE <= end)
                    parseStreamInfo(p, static_cast<int>(end - p));

                // Build CodecPrivate if we don't have one yet
                if (m_codecPrivate.empty())
                {
                    // Store fLaC + this STREAMINFO block
                    const int privSize = 4 + 4 + blockLen;
                    m_codecPrivate.resize(privSize);
                    memcpy(m_codecPrivate.data(), FLAC_MARKER, 4);
                    // Write block header as last-metadata-block + type 0
                    m_codecPrivate[4] = 0x80;  // last-block flag set, type = 0
                    m_codecPrivate[5] = static_cast<uint8_t>((blockLen >> 16) & 0xFF);
                    m_codecPrivate[6] = static_cast<uint8_t>((blockLen >> 8) & 0xFF);
                    m_codecPrivate[7] = static_cast<uint8_t>(blockLen & 0xFF);
                    memcpy(m_codecPrivate.data() + 8, p, blockLen);
                }
            }

            if (p + blockLen > end)
            {
                countHeader(p + blockLen - buff);
                return nullptr;  // metadata extends past buffer
            }

            p += blockLen;
            if (isLast)
                break;
        }
        countHeader(p - buff);
        // p now points to the first audio frame
        if (p + 2 <= end)
        {
            const uint16_t sync = (p[0] << 8) | p[1];
            if (sync == FLAC_SYNC_FIXED || sync == FLAC_SYNC_VARIABLE)
                return const_cast<uint8_t*>(p);
        }
        return nullptr;
    }

    // Scan for the next sync code
    for (uint8_t* p = buff; p + 2 <= end; p++)
    {
        const uint16_t sync = (p[0] << 8) | p[1];
        if (sync == FLAC_SYNC_FIXED || sync == FLAC_SYNC_VARIABLE)
        {
            // Validate by trying to parse the frame header
            if (parseFrameHeader(p, end) > 0)
                return p;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Frame header parsing
// ---------------------------------------------------------------------------

// Read a UTF-8 coded number (used for frame/sample number in FLAC frame headers).
// Returns the number of bytes consumed, or 0 on failure.
static int readUtf8Value(const uint8_t* buf, const uint8_t* end, uint64_t& value)
{
    if (buf >= end)
        return 0;

    const uint8_t b0 = buf[0];
    int len;
    if ((b0 & 0x80) == 0)
    {
        value = b0;
        len = 1;
    }
    else if ((b0 & 0xE0) == 0xC0)
    {
        value = b0 & 0x1F;
        len = 2;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        value = b0 & 0x0F;
        len = 3;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        value = b0 & 0x07;
        len = 4;
    }
    else if ((b0 & 0xFC) == 0xF8)
    {
        value = b0 & 0x03;
        len = 5;
    }
    else if ((b0 & 0xFE) == 0xFC)
    {
        value = b0 & 0x01;
        len = 6;
    }
    else if (b0 == 0xFE)
    {
        value = 0;
        len = 7;
    }
    else
    {
        return 0;
    }

    if (buf + len > end)
        return 0;

    for (int i = 1; i < len; i++)
    {
        if ((buf[i] & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (buf[i] & 0x3F);
    }
    return len;
}

int FLACStreamReader::parseFrameHeader(const uint8_t* buff, const uint8_t* end, FrameFields* fields) const
{
    if (end - buff < 5)
        return 0;

    const uint16_t sync = (buff[0] << 8) | buff[1];
    if (sync != FLAC_SYNC_FIXED && sync != FLAC_SYNC_VARIABLE)
        return 0;

    // byte 2
    const int blockSizeCode = (buff[2] >> 4) & 0x0F;
    const int sampleRateCode = buff[2] & 0x0F;
    if (sampleRateCode == 15)
        return 0;  // invalid

    // byte 3
    const int channelAssign = (buff[3] >> 4) & 0x0F;
    if (channelAssign > 10)
        return 0;  // reserved
    const int bpsCode = (buff[3] >> 1) & 0x07;
    if (bpsCode == 3)
        return 0;  // reserved

    // UTF-8 coded frame or sample number
    const uint8_t* p = buff + 4;
    uint64_t frameOrSample;
    const int utf8Len = readUtf8Value(p, end, frameOrSample);
    if (utf8Len == 0)
        return 0;
    p += utf8Len;

    // Optional block size field
    int blockSize = kBlockSizeTable[blockSizeCode];
    if (blockSizeCode == 6)
    {
        if (p + 1 > end)
            return 0;
        blockSize = p[0] + 1;
        p += 1;
    }
    else if (blockSizeCode == 7)
    {
        if (p + 2 > end)
            return 0;
        blockSize = ((p[0] << 8) | p[1]) + 1;
        p += 2;
    }

    // Optional sample rate field
    if (sampleRateCode == 12)
    {
        if (p + 1 > end)
            return 0;
        p += 1;
    }
    else if (sampleRateCode == 13 || sampleRateCode == 14)
    {
        if (p + 2 > end)
            return 0;
        p += 2;
    }

    // CRC-8 of the header
    if (p + 1 > end)
        return 0;
    const int hdrLen = static_cast<int>(p - buff);
    const uint8_t expectedCrc = *p;
    const uint8_t computedCrc = flac_crc8(buff, hdrLen);
    if (expectedCrc != computedCrc)
        return 0;

    if (blockSize <= 0)
        return 0;

    if (fields)
    {
        fields->variableBlocks = sync == FLAC_SYNC_VARIABLE;
        fields->sampleRateCode = sampleRateCode;
        fields->channels = channelAssign <= 7 ? channelAssign + 1 : 2;
        fields->sampleSizeCode = bpsCode;
        fields->number = frameOrSample;
        fields->blockSize = blockSize;
    }
    return blockSize;
}

// While the type of a track is still being worked out, a frame header pattern is not enough. 0xFFF8 followed by fields
// that pass the header's CRC-8 turns up by chance in other data, about once in every 5 to 10 MB of the MPEG-2 video of
// a DVD, and with each "frame" measured up to the next such chance match, the ten frames the check asks for were found
// in a video track, which was then listed and muxed as FLAC (#27). So here a frame counts only when it is a whole FLAC
// frame: it ends where a next header begins AND its own CRC-16, the last two bytes of every frame, matches there. And
// it must follow the frame accepted before it: the same blocking strategy, sample rate, channel count and sample size,
// and the next frame or sample number. Muxing is not changed; this runs only while a track is being identified.
int FLACStreamReader::testFrameLength(uint8_t* buff, uint8_t* end, const FrameFields& fields)
{
    // checkStream measures its first frame twice, and the second time it gets the same answer.
    if (buff == m_testLastFrame)
        return m_testLastLen;

    if (m_testPrevValid)
    {
        const uint64_t next =
            m_testPrev.variableBlocks ? m_testPrev.number + m_testPrev.blockSize : m_testPrev.number + 1;
        if (fields.variableBlocks != m_testPrev.variableBlocks || fields.sampleRateCode != m_testPrev.sampleRateCode ||
            fields.channels != m_testPrev.channels || fields.sampleSizeCode != m_testPrev.sampleSizeCode ||
            fields.number != next)
        {
            m_testPrevValid = false;
            return 0;
        }
    }

    uint8_t* limit = end - buff > MAX_FRAME_BYTES ? buff + MAX_FRAME_BYTES : end;
    for (uint8_t* p = buff + 4; p + 2 <= limit; p++)
    {
        if (p[0] != 0xFF || (p[1] != 0xF8 && p[1] != 0xF9) || parseFrameHeader(p, end) == 0)
            continue;
        const auto stored = static_cast<uint16_t>((p[-2] << 8) | p[-1]);
        if (flac_crc16(buff, static_cast<size_t>(p - 2 - buff)) != stored)
            continue;  // a header pattern inside this frame, not the start of the next one
        m_testPrev = fields;
        m_testPrevValid = true;
        m_testLastFrame = buff;
        m_testLastLen = static_cast<int>(p - buff);
        return m_testLastLen;
    }
    m_testPrevValid = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Frame decoding: determine frame length by finding the next sync code
// ---------------------------------------------------------------------------

int FLACStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;

    FrameFields fields;
    const int blockSize = parseFrameHeader(buff, end, &fields);
    if (blockSize == 0)
        return 0;  // not a valid frame

    int testLen = 0;
    if (m_testMode)
    {
        testLen = testFrameLength(buff, end, fields);
        if (testLen <= 0)
            return 0;
    }

    m_curBlockSize = blockSize;

    // Extract audio parameters from the frame header if not yet set via STREAMINFO
    if (m_sampleRate == 0)
    {
        const int sampleRateCode = buff[2] & 0x0F;
        if (sampleRateCode > 0 && sampleRateCode <= 11)
            m_sampleRate = kSampleRateTable[sampleRateCode];

        const int channelAssign = (buff[3] >> 4) & 0x0F;
        if (channelAssign <= 7)
            m_channels = static_cast<uint8_t>(channelAssign + 1);
        else
            m_channels = 2;  // mid/side, left/side, right/side stereo

        const int bpsCode = (buff[3] >> 1) & 0x07;
        if (kBpsTable[bpsCode] > 0)
            m_bitsPerSample = kBpsTable[bpsCode];
    }

    if (m_testMode)
        return testLen;

    // A frame ends where the next one begins. A header pattern that passes the CRC-8 also turns up inside the audio
    // data now and then, and taking it for the next frame split one frame in two: in a 7.1, 24 bit, 96 kHz file a
    // pattern 77,801 bytes into a frame of 192,515 cut it at 5.46 s, the decoder dropped the pieces, 8,192 samples a
    // channel, and the times after it moved. So a header counts as the next frame only where the frame's own CRC-16,
    // its last two bytes, matches in front of it, as testFrameLength already asks while a track is identified. A
    // damaged frame whose CRC-16 never matches is ended at the first header after it that carries the next frame's
    // number and the same stream fields; failing that, at the first header, once a whole frame's worth of bytes has
    // been searched or the stream has ended. Waiting for a whole frame's worth alone meant a damaged frame in the last
    // 2 MB of a stream was never ended: every frame after it was lost with it.
    uint8_t* firstHeader = nullptr;
    uint8_t* firstFollowing = nullptr;
    const uint64_t nextNumber =
        fields.variableBlocks ? fields.number + static_cast<uint64_t>(fields.blockSize) : fields.number + 1;
    for (uint8_t* p = buff + 4; p + 2 <= end && p - buff <= MAX_FRAME_BYTES; p++)
    {
        const uint16_t sync = (p[0] << 8) | p[1];
        if (sync != FLAC_SYNC_FIXED && sync != FLAC_SYNC_VARIABLE)
            continue;
        FrameFields next;
        if (parseFrameHeader(p, end, &next) == 0)
            continue;
        if (!firstHeader)
            firstHeader = p;
        const auto stored = static_cast<uint16_t>((p[-2] << 8) | p[-1]);
        if (flac_crc16(buff, static_cast<size_t>(p - 2 - buff)) == stored)
            return static_cast<int>(p - buff);
        if (!firstFollowing && next.number == nextNumber && next.variableBlocks == fields.variableBlocks &&
            next.sampleRateCode == fields.sampleRateCode && next.channels == fields.channels &&
            next.sampleSizeCode == fields.sampleSizeCode)
            firstFollowing = p;
    }
    const bool searchedAll = end - buff > MAX_FRAME_BYTES || m_lastBlock;
    if (firstFollowing && searchedAll)
        return static_cast<int>(firstFollowing - buff);
    if (firstHeader && searchedAll)
        return static_cast<int>(firstHeader - buff);

    // No next frame found: need more data
    return NOT_ENOUGH_BUFFER;
}

// ---------------------------------------------------------------------------
// Frame duration
// ---------------------------------------------------------------------------

double FLACStreamReader::getFrameDuration()
{
    if (m_sampleRate == 0 || m_curBlockSize == 0)
        return 0;
    return static_cast<double>(m_curBlockSize) / m_sampleRate * INTERNAL_PTS_FREQ;
}

// ---------------------------------------------------------------------------
// Demux: write FLAC file header before the first frame
// ---------------------------------------------------------------------------

int FLACStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    if (!m_demuxMode || !m_firstFrame)
        return 0;

    m_firstFrame = false;

    // If we have stored CodecPrivate data (from MKV), write it as the file header.
    if (!m_codecPrivate.empty())
    {
        const int size = static_cast<int>(m_codecPrivate.size());
        if (dstEnd - dstBuffer < size)
        {
            LTRACE(LT_WARN, 2, "FLAC: not enough buffer for file header");
            return 0;
        }
        memcpy(dstBuffer, m_codecPrivate.data(), size);
        return size;
    }

    // Fallback: write a minimal fLaC + STREAMINFO header from parsed parameters.
    // 4 (marker) + 4 (metadata block header) + 34 (STREAMINFO) = 42 bytes
    static constexpr int MIN_HEADER_SIZE = 42;
    if (dstEnd - dstBuffer < MIN_HEADER_SIZE || m_sampleRate == 0)
        return 0;

    uint8_t* p = dstBuffer;
    // "fLaC"
    memcpy(p, FLAC_MARKER, 4);
    p += 4;

    // Metadata block header: last-block(1) + type 0 (7) + length 34 (24)
    *p++ = 0x80;  // last-metadata-block flag | type=STREAMINFO
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = STREAMINFO_SIZE;

    // STREAMINFO (34 bytes)
    const int blkSize = m_maxBlockSize > 0 ? m_maxBlockSize : 4096;
    const int minBlk = m_minBlockSize > 0 ? m_minBlockSize : blkSize;

    // min block size (16 bits)
    *p++ = static_cast<uint8_t>((minBlk >> 8) & 0xFF);
    *p++ = static_cast<uint8_t>(minBlk & 0xFF);
    // max block size (16 bits)
    *p++ = static_cast<uint8_t>((blkSize >> 8) & 0xFF);
    *p++ = static_cast<uint8_t>(blkSize & 0xFF);
    // min frame size (24 bits): unknown, set to 0
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    // max frame size (24 bits): unknown, set to 0
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    // sample rate (20 bits) | channels-1 (3 bits) | bps-1 (5 bits) | total samples (36 bits)
    *p++ = static_cast<uint8_t>((m_sampleRate >> 12) & 0xFF);
    *p++ = static_cast<uint8_t>((m_sampleRate >> 4) & 0xFF);
    const uint8_t chanBits = (m_channels - 1) & 0x07;
    const uint8_t bpsBits = (m_bitsPerSample - 1) & 0x1F;
    *p++ = static_cast<uint8_t>(((m_sampleRate & 0x0F) << 4) | (chanBits << 1) | (bpsBits >> 4));
    *p++ = static_cast<uint8_t>((bpsBits << 4));  // total samples upper 4 bits = 0
    // total samples lower 32 bits = 0
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    // MD5 signature = 0 (16 bytes)
    memset(p, 0, 16);
    p += 16;

    return static_cast<int>(p - dstBuffer);
}
