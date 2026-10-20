#include "opusStreamReader.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

// ---------------------------------------------------------------------------
// Opus / OGG constants
// ---------------------------------------------------------------------------

// Length-prefix framing: each Opus packet in the byte stream from the MKV
// demuxer is preceded by a 4-byte big-endian length (added by
// ParsedOpusTrackData::extractData).
static constexpr int FRAME_PREFIX_LEN = 4;

// Maximum reasonable Opus packet size (RFC 6716 §3.4: 61440 bytes max).
static constexpr int MAX_OPUS_PACKET = 61440;

// OGG capture pattern
static constexpr uint8_t OGG_CAPTURE[4] = {'O', 'g', 'g', 'S'};

// OGG header type flags
static constexpr uint8_t OGG_FLAG_FIRST = 0x02;  // beginning of stream
static constexpr uint8_t OGG_FLAG_LAST = 0x04;   // end of stream

// OpusHead magic
static constexpr uint8_t OPUS_HEAD_MAGIC[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};

// Opus frame durations in 48 kHz samples, indexed by config number (0-31).
// Configs 0-3:   SILK-only      NB   (10, 20, 40, 60 ms)
// Configs 4-7:   SILK-only      MB   (10, 20, 40, 60 ms)
// Configs 8-11:  SILK-only      WB   (10, 20, 40, 60 ms)
// Configs 12-13: Hybrid         SWB  (10, 20 ms)
// Configs 14-15: Hybrid         FB   (10, 20 ms)
// Configs 16-19: CELT-only      NB   (2.5, 5, 10, 20 ms)
// Configs 20-23: CELT-only      WB   (2.5, 5, 10, 20 ms)
// Configs 24-27: CELT-only      SWB  (2.5, 5, 10, 20 ms)
// Configs 28-31: CELT-only      FB   (2.5, 5, 10, 20 ms)
static constexpr int kOpusFrameSamples[32] = {
    480, 960, 1920, 2880,  // SILK NB
    480, 960, 1920, 2880,  // SILK MB
    480, 960, 1920, 2880,  // SILK WB
    480, 960,              // Hybrid SWB
    480, 960,              // Hybrid FB
    120, 240, 480,  960,   // CELT NB
    120, 240, 480,  960,   // CELT WB
    120, 240, 480,  960,   // CELT SWB
    120, 240, 480,  960,   // CELT FB
};

// ---------------------------------------------------------------------------
// OGG CRC-32 (polynomial 0x04C11DB7, used by the Ogg framing spec)
// ---------------------------------------------------------------------------

static uint32_t oggCrcTable[256];
static bool oggCrcTableBuilt = false;

static void buildOggCrcTable()
{
    if (oggCrcTableBuilt)
        return;
    for (int i = 0; i < 256; i++)
    {
        uint32_t r = static_cast<uint32_t>(i) << 24;
        for (int j = 0; j < 8; j++) r = (r << 1) ^ ((r & 0x80000000U) ? 0x04C11DB7U : 0);
        oggCrcTable[i] = r;
    }
    oggCrcTableBuilt = true;
}

static uint32_t oggCrc32(const uint8_t* data, int len)
{
    buildOggCrcTable();
    uint32_t crc = 0;
    for (int i = 0; i < len; i++) crc = (crc << 8) ^ oggCrcTable[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

/// Compute OGG CRC over two disjoint buffers (for pages where header and
/// payload are stored separately).
static uint32_t oggCrc32_two(const uint8_t* a, int aLen, const uint8_t* b, int bLen)
{
    buildOggCrcTable();
    uint32_t crc = 0;
    for (int i = 0; i < aLen; i++) crc = (crc << 8) ^ oggCrcTable[((crc >> 24) ^ a[i]) & 0xFF];
    for (int i = 0; i < bLen; i++) crc = (crc << 8) ^ oggCrcTable[((crc >> 24) ^ b[i]) & 0xFF];
    return crc;
}

// ---------------------------------------------------------------------------
// OpusStreamReader
// ---------------------------------------------------------------------------

OpusStreamReader::OpusStreamReader()
    : m_testMode(false),
      m_firstFrame(true),
      m_channels(2),
      m_preSkip(0),
      m_originalSampleRate(0),
      m_curFrameSamples(0),
      m_granulePos(0),
      m_oggPageSeqNo(0),
      m_oggSerialNo(0x4F707573)  // "Opus" as a default serial number
{
}

const CodecInfo& OpusStreamReader::getCodecInfo() { return opusCodecInfo; }

const std::string OpusStreamReader::getStreamInfo()
{
    std::ostringstream str;
    str << "Sample Rate: " << m_originalSampleRate << "  Channels: " << static_cast<int>(m_channels);
    if (m_preSkip > 0)
        str << "  Pre-skip: " << m_preSkip;
    return str.str();
}

// ---------------------------------------------------------------------------
// TS descriptor for PMT (ETSI TS 101 154)
// ---------------------------------------------------------------------------

int OpusStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    uint8_t* dst = dstBuff;

    // 1. registration_descriptor  (tag=0x05, length=4, format_identifier='Opus')
    *dst++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor_tag
    *dst++ = 4;                                                    // descriptor_length
    *dst++ = 'O';
    *dst++ = 'p';
    *dst++ = 'u';
    *dst++ = 's';

    // 2. opus_audio_descriptor  (DVB extension descriptor tag=0x7F, ext=0x80)
    //    Per ETSI TS 101 154 Annex E: descriptor_tag=0x7F, descriptor_length=2,
    //    descriptor_tag_extension=0x80, channel_config_code.
    *dst++ = static_cast<uint8_t>(TSDescriptorTag::DVB_EXTENSION);  // descriptor_tag
    *dst++ = 2;                                                     // descriptor_length
    *dst++ = 0x80;                                                  // descriptor_tag_extension (opus)

    // channel_config_code: for mapping family 0 (mono/stereo), the value equals
    // the channel count.  For family 1 with standard Vorbis channel mapping and
    // <= 8 channels, the value also equals the channel count.  Otherwise 0xFF.
    uint8_t channelConfig = 0xFF;
    if (m_codecPrivate.size() >= 19)
    {
        const uint8_t mappingFamily = m_codecPrivate[18];
        if (mappingFamily == 0 && m_channels <= 2)
        {
            channelConfig = m_channels;
        }
        else if (mappingFamily == 1 && m_channels <= 8)
        {
            // Standard Vorbis mapping — use channel count directly
            channelConfig = m_channels;
        }
    }
    else if (m_channels <= 2)
    {
        // No codec private data yet; assume RTP mapping family
        channelConfig = m_channels;
    }
    *dst++ = channelConfig;

    return static_cast<int>(dst - dstBuff);  // 6 + 4 = 10 bytes
}

// ---------------------------------------------------------------------------
// OpusHead parsing
// ---------------------------------------------------------------------------

bool OpusStreamReader::parseOpusHead(const uint8_t* data, const int size)
{
    // OpusHead layout (RFC 7845 §5.1):
    //   [0..7]   "OpusHead"
    //   [8]      Version (must be 1)
    //   [9]      Channel count
    //   [10..11] Pre-skip (LE uint16)
    //   [12..15] Input sample rate (LE uint32)
    //   [16..17] Output gain (LE int16)
    //   [18]     Channel mapping family
    //   (if family > 0: stream count, coupled count, mapping table)
    if (size < 19)
        return false;
    if (memcmp(data, OPUS_HEAD_MAGIC, 8) != 0)
        return false;
    if (data[8] != 1)  // version
        return false;

    m_channels = data[9];
    m_preSkip = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    m_originalSampleRate = static_cast<int>(data[12]) | (data[13] << 8) | (data[14] << 16) | (data[15] << 24);

    if (m_channels == 0)
        return false;

    return true;
}

void OpusStreamReader::setCodecPrivate(const uint8_t* data, const int size)
{
    if (data == nullptr || size <= 0)
        return;

    m_codecPrivate.assign(data, data + size);
    parseOpusHead(data, size);
}

void OpusStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // fills sampleRate, channels
    if (!m_codecPrivate.empty())
        data.codecPrivate = m_codecPrivate;
}

void OpusStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    // Prefer codec-private blob (OpusHead) -- it contains the authoritative
    // channel count, pre-skip, and original sample rate.
    if (!data.codecPrivate.empty())
    {
        setCodecPrivate(data.codecPrivate.data(), static_cast<int>(data.codecPrivate.size()));
    }
    else if (data.channels > 0)
    {
        m_channels = static_cast<uint8_t>(data.channels);
    }
}

// ---------------------------------------------------------------------------
// Frame detection — find length-prefixed Opus packets
// ---------------------------------------------------------------------------

uint8_t* OpusStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    // If the stream starts with "OpusHead" (prepended by ParsedOpusTrackData),
    // parse it and skip past it to reach the first framed packet.
    if (end - buff >= 19 && memcmp(buff, OPUS_HEAD_MAGIC, 8) == 0)
    {
        parseOpusHead(buff, static_cast<int>(end - buff));

        // Determine OpusHead size: 19 bytes for family 0, more for others
        int headSize = 19;
        if (buff[18] > 0 && end - buff >= 21)
        {
            // stream_count(1) + coupled_count(1) + mapping(channels)
            headSize = 21 + m_channels;
        }
        if (headSize > end - buff)
            return nullptr;

        // Store the OpusHead as codec private for demux header and MKV remux
        if (m_codecPrivate.empty())
            m_codecPrivate.assign(buff, buff + headSize);

        buff += headSize;
    }

    // Look for a valid 4-byte BE length prefix followed by a reasonable
    // Opus packet.
    while (buff + FRAME_PREFIX_LEN <= end)
    {
        const uint32_t len = (static_cast<uint32_t>(buff[0]) << 24) | (buff[1] << 16) | (buff[2] << 8) | buff[3];
        if (len >= 1 && len <= MAX_OPUS_PACKET)
        {
            if (buff + FRAME_PREFIX_LEN + static_cast<int>(len) <= end)
                return buff;
            else
                return nullptr;  // need more data
        }
        buff++;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Frame decoding
// ---------------------------------------------------------------------------

int OpusStreamReader::parseFrameSamples(const uint8_t* opusData, const int size) const
{
    if (size < 1)
        return 0;

    const uint8_t toc = opusData[0];
    const int config = (toc >> 3) & 0x1F;
    const int frameCode = toc & 0x03;

    const int samplesPerFrame = kOpusFrameSamples[config];

    switch (frameCode)
    {
    case 0:
        return samplesPerFrame;  // 1 frame
    case 1:
    case 2:
        return samplesPerFrame * 2;  // 2 frames
    case 3:
        // Arbitrary number of frames — count is in the second byte
        if (size < 2)
            return samplesPerFrame;  // fallback
        return samplesPerFrame * (opusData[1] & 0x3F);
    default:
        return samplesPerFrame;
    }
}

int OpusStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;

    // Every file of a join begins with its own OpusHead, in front of its first packet. It was read only at the start
    // of the stream: in the middle its first bytes were taken for a length, the search that followed took bytes of
    // the head for packets, and the second file of a join lost its first packets and came out seconds late.
    int head = 0;
    if (end - buff >= 8 && memcmp(buff, OPUS_HEAD_MAGIC, 8) == 0)
    {
        if (end - buff < 19)
            return NOT_ENOUGH_BUFFER;
        head = buff[18] > 0 ? 21 + buff[9] : 19;
    }
    skipBeforeBytes = head + FRAME_PREFIX_LEN;  // the head, if there is one, and the 4-byte length prefix
    if (end - buff < head + FRAME_PREFIX_LEN)
        return NOT_ENOUGH_BUFFER;
    buff += head;

    const uint32_t len = (static_cast<uint32_t>(buff[0]) << 24) | (buff[1] << 16) | (buff[2] << 8) | buff[3];

    if (len == 0 || len > MAX_OPUS_PACKET)
        return 0;  // invalid

    const int frameLen = static_cast<int>(len);
    if (end - buff < FRAME_PREFIX_LEN + frameLen)
        return NOT_ENOUGH_BUFFER;

    // In a join whose paddings are known, whether the next file's OpusHead follows this packet: then it is the last
    // packet of its file, and readPacket may leave it out.
    m_nextPreSkip = -1;
    if (!m_joinEndPadNs.empty() && m_joinFile + 1 < m_joinEndPadNs.size())
    {
        const uint8_t* after = buff + FRAME_PREFIX_LEN + frameLen;
        if (end - after >= 12 && memcmp(after, OPUS_HEAD_MAGIC, 8) == 0)
            m_nextPreSkip = after[10] | after[11] << 8;
        else if (end - after < 12 && !m_lastBlock)
            return NOT_ENOUGH_BUFFER;
    }

    // The last block of the stream ends with this packet: the samples the source trims at the end are in it
    m_lastOfStream = m_lastBlock && end - buff == FRAME_PREFIX_LEN + frameLen;

    // Parse the Opus packet to determine frame duration
    m_curFrameSamples = parseFrameSamples(buff + FRAME_PREFIX_LEN, frameLen);

    return frameLen;  // the actual Opus packet size (without prefix)
}

// ** A JOIN OF OPUS FILES PLAYED WHAT EACH FILE TRIMS. ** A file ends with padding that only fills its last packet,
// and the next one begins with its pre-skip, the encoder's start-up samples; a single file says to drop both, but in a
// join they sat between the two files as sound, and the sound of every later file came that much late against its
// picture: 19.5 ms after the first joint of two files of a usual encoder. Whole packets are left out at each joint, the
// last of the file before, as long as that brings the sound nearer to its picture, so that it stays within half a
// packet of it however many files are joined. The packets of the next file follow on without a gap.
int OpusStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0 || avPacket.size <= 0 || m_nextPreSkip < 0)
            return rez;
        const int64_t pad = (m_joinEndPadNs[m_joinFile] * 48000 + 500000000) / 1000000000;
        m_joinExcess += pad + m_nextPreSkip;
        m_nextPreSkip = -1;
        ++m_joinFile;
        if (m_curFrameSamples <= 0 || m_joinExcess * 2 < m_curFrameSamples)
            return rez;
        m_joinExcess -= m_curFrameSamples;
        m_curPts -= getFrameDuration();
        if (m_frameNum > 0)
            --m_frameNum;
        LTRACE(LT_INFO, 2,
               "Opus join: the last packet of file " << m_joinFile << " is left out at the joint, as its end padding "
                                                     << "and the next file's pre-skip are not sound.");
    }
}

// ---------------------------------------------------------------------------
// Frame duration
// ---------------------------------------------------------------------------

double OpusStreamReader::getFrameDuration()
{
    if (m_curFrameSamples == 0)
        return 0;
    // Opus always operates at 48 kHz
    return static_cast<double>(m_curFrameSamples) / 48000.0 * INTERNAL_PTS_FREQ;
}

// ---------------------------------------------------------------------------
// OGG page writing helpers
// ---------------------------------------------------------------------------

/// Write a complete OGG page with payload.
int OpusStreamReader::writeOggPage(uint8_t* dst, uint8_t* dstEnd, const uint8_t* payload, const int payloadSize,
                                   const uint8_t headerType, const int64_t granule)
{
    // Segment count: payloadSize / 255 + 1 (unless payloadSize is 0)
    const int numSegments = payloadSize / 255 + 1;
    const int headerSize = 27 + numSegments;
    const int totalSize = headerSize + payloadSize;

    if (dstEnd - dst < totalSize)
        return 0;

    uint8_t* p = dst;

    // Capture pattern
    memcpy(p, OGG_CAPTURE, 4);
    p += 4;
    // Version
    *p++ = 0;
    // Header type
    *p++ = headerType;
    // Granule position (LE int64)
    for (int i = 0; i < 8; i++) *p++ = static_cast<uint8_t>((granule >> (i * 8)) & 0xFF);
    // Serial number (LE uint32)
    for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggSerialNo >> (i * 8)) & 0xFF);
    // Page sequence number (LE uint32)
    for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggPageSeqNo >> (i * 8)) & 0xFF);
    // CRC placeholder (filled below)
    uint8_t* crcPos = p;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    // Number of segments
    *p++ = static_cast<uint8_t>(numSegments);
    // Segment table
    int remaining = payloadSize;
    for (int i = 0; i < numSegments - 1; i++)
    {
        *p++ = 255;
        remaining -= 255;
    }
    *p++ = static_cast<uint8_t>(remaining);

    // Payload
    memcpy(p, payload, payloadSize);
    p += payloadSize;

    // Compute CRC over the entire page
    const uint32_t crc = oggCrc32(dst, totalSize);
    crcPos[0] = static_cast<uint8_t>(crc & 0xFF);
    crcPos[1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    crcPos[2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    crcPos[3] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    m_oggPageSeqNo++;
    return totalSize;
}

/// Write an OGG page header + segment table only (payload will be appended
/// by the muxer).  The CRC is computed over header + the given payload
/// that the caller already has.
int OpusStreamReader::writeOggPageHeader(uint8_t* dst, uint8_t* dstEnd, const int payloadSize, const uint8_t headerType,
                                         const int64_t granule)
{
    const int numSegments = payloadSize / 255 + 1;
    const int headerSize = 27 + numSegments;

    if (dstEnd - dst < headerSize)
        return 0;

    uint8_t* p = dst;

    // Capture pattern
    memcpy(p, OGG_CAPTURE, 4);
    p += 4;
    // Version
    *p++ = 0;
    // Header type
    *p++ = headerType;
    // Granule position (LE int64)
    for (int i = 0; i < 8; i++) *p++ = static_cast<uint8_t>((granule >> (i * 8)) & 0xFF);
    // Serial number (LE uint32)
    for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggSerialNo >> (i * 8)) & 0xFF);
    // Page sequence number (LE uint32)
    for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggPageSeqNo >> (i * 8)) & 0xFF);
    // CRC placeholder
    uint8_t* crcPos = p;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    // Number of segments
    *p++ = static_cast<uint8_t>(numSegments);
    // Segment table
    int remaining = payloadSize;
    for (int i = 0; i < numSegments - 1; i++)
    {
        *p++ = 255;
        remaining -= 255;
    }
    *p++ = static_cast<uint8_t>(remaining);

    // Note: CRC cannot be computed here because the payload hasn't been
    // written yet.  We store the header size so the caller can patch it later.
    // Actually — the CRC needs to be correct for valid OGG.  Since we know
    // the payload (it's in the current AVPacket), compute over both parts.
    // We leave the CRC as 0 for now and patch it in writeAdditionData()
    // where we have access to both the header and the payload.

    m_oggPageSeqNo++;
    return headerSize;
}

// ---------------------------------------------------------------------------
// TS mux / Demux: write control header or OGG pages
// ---------------------------------------------------------------------------

bool OpusStreamReader::beforeFileCloseEvent(File& file)
{
    if (!m_demuxMode || m_lastPage.size() < 27 || m_lastPageHeader < 27)
        return true;
    // The end of the stream, and a granule position short of the packet's end by the samples the source trims there,
    // which a player then leaves out (RFC 7845, 4.4). Opus counts at 48 kHz.
    const int64_t trim = m_endTrimNs > 0 ? (m_endTrimNs * 48000 + 500000000) / 1000000000 : 0;
    const int64_t granule = (std::max)(int64_t{0}, m_granulePos - trim);
    uint8_t* p = m_lastPage.data();
    p[5] |= OGG_FLAG_LAST;
    for (int i = 0; i < 8; i++) p[6 + i] = static_cast<uint8_t>(granule >> (8 * i));
    memset(p + 22, 0, 4);
    const uint32_t crc = oggCrc32(m_lastPage.data(), static_cast<int>(m_lastPage.size()));
    for (int i = 0; i < 4; i++) p[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    file.sync();
    int64_t fileSize = 0;
    file.size(&fileSize);
    if (fileSize < static_cast<int64_t>(m_lastPage.size()) ||
        file.seek(fileSize - static_cast<int64_t>(m_lastPage.size()), File::SeekMethod::smBegin) == -1)
        return false;
    return file.write(p, m_lastPageHeader) == m_lastPageHeader;
}

int OpusStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    if (!m_demuxMode)
    {
        // TS mux mode: write ETSI TS 101 154 opus_access_unit header before
        // each Opus packet.  The 16-bit control header fields are:
        //   bits [15:5]  = control_header_prefix = 0x3FF (11 bits)
        //   bit  [4]     = start_trim_flag       = 0, 1 on the first access units, as long as the pre-skip lasts
        //   bit  [3]     = end_trim_flag          = 0, 1 on the last access unit of a track that ends in padding
        //   bit  [2]     = control_extension_flag = 0
        //   bits [1:0]   = reserved               = 11
        // Packed big-endian: 0x7FE3  (byte 0 = 0x7F, byte 1 = 0xE3)
        //
        // After the 2-byte header comes a variable-length payload size field:
        //   while (next_byte == 0xFF) { size += 255; consume byte; }
        //   size += next_byte; consume byte;
        // then, with start_trim_flag, 3 reserved bits and the 13-bit start_trim, and with end_trim_flag the same
        // for end_trim (samples at 48 kHz). Then the raw Opus packet data follows.
        //
        // The pre-skip at the start of the track (its OpusHead's, as a Matroska file's CodecDelay says it) and the
        // padding at the end, the last file's in a join, were played as sound: only a Matroska output and a demux
        // left them out. They are said here as start_trim and end_trim, what they are to a player. A pre-skip longer
        // than the first access unit goes on into the next ones. The joints of a join are kept by leaving out whole
        // packets (readPacket), so that the times of the packets stay those of the sound: a trim there would leave a
        // gap in the sound against them.

        // Maximum header size: 2 (control header) + ceil(frameSize/255) + 1 (payload length)
        // For a 61440-byte maximum Opus packet: 2 + 241 + 1 = 244 bytes.
        const int frameSize = avPacket.size;
        if (frameSize <= 0)
            return 0;

        // Calculate payload length field size
        int payloadLenFieldSize = 0;
        {
            int tmp = frameSize;
            while (tmp >= 255)
            {
                payloadLenFieldSize++;
                tmp -= 255;
            }
            payloadLenFieldSize++;  // final non-0xFF byte
        }

        const int samples = (std::min)(parseFrameSamples(avPacket.data, avPacket.size), 0x1FFF);
        if (m_startTrimLeft < 0)
            m_startTrimLeft = m_preSkip;
        const int startTrim = (std::min)(m_startTrimLeft, samples);
        m_startTrimLeft -= startTrim;
        int64_t endTrim = 0;
        if (m_lastOfStream && m_endTrimNs > 0)
            endTrim =
                (std::min)(static_cast<int64_t>(samples - startTrim), (m_endTrimNs * 48000 + 500000000) / 1000000000);

        const int totalHeaderSize = 2 + payloadLenFieldSize + (startTrim > 0 ? 2 : 0) + (endTrim > 0 ? 2 : 0);
        if (dstEnd - dstBuffer < totalHeaderSize)
            return 0;

        // Write 2-byte control header
        dstBuffer[0] = 0x7F;  // prefix upper 8 bits: 0_1111111
        // prefix lower 3 bits (111) + flags + reserved (11)
        dstBuffer[1] = static_cast<uint8_t>(0xE3 | (startTrim > 0 ? 0x10 : 0) | (endTrim > 0 ? 0x08 : 0));

        // Write variable-length payload size
        uint8_t* p = dstBuffer + 2;
        int remaining = frameSize;
        while (remaining >= 255)
        {
            *p++ = 0xFF;
            remaining -= 255;
        }
        *p++ = static_cast<uint8_t>(remaining);
        if (startTrim > 0)
        {
            *p++ = static_cast<uint8_t>(0xE0 | (startTrim >> 8));
            *p++ = static_cast<uint8_t>(startTrim & 0xFF);
        }
        if (endTrim > 0)
        {
            *p++ = static_cast<uint8_t>(0xE0 | (endTrim >> 8));
            *p++ = static_cast<uint8_t>(endTrim & 0xFF);
        }

        return totalHeaderSize;
    }

    uint8_t* dst = dstBuffer;

    if (m_firstFrame)
    {
        m_firstFrame = false;

        // ---- Page 0: OpusHead ----
        if (!m_codecPrivate.empty())
        {
            const int wrote = writeOggPage(dst, dstEnd, m_codecPrivate.data(), static_cast<int>(m_codecPrivate.size()),
                                           OGG_FLAG_FIRST, 0);
            if (wrote == 0)
                return 0;
            dst += wrote;
        }

        // ---- Page 1: OpusTags (minimal) ----
        // "OpusTags" + vendor string length (LE32) + vendor + comment count (LE32)
        const char* vendor = "tsMuxeR";
        const int vendorLen = static_cast<int>(strlen(vendor));
        const int tagsSize = 8 + 4 + vendorLen + 4;
        uint8_t tagsBuf[64];
        uint8_t* tp = tagsBuf;
        memcpy(tp, "OpusTags", 8);
        tp += 8;
        tp[0] = static_cast<uint8_t>(vendorLen & 0xFF);
        tp[1] = static_cast<uint8_t>((vendorLen >> 8) & 0xFF);
        tp[2] = 0;
        tp[3] = 0;
        tp += 4;
        memcpy(tp, vendor, vendorLen);
        tp += vendorLen;
        // Comment count = 0
        tp[0] = tp[1] = tp[2] = tp[3] = 0;

        const int wrote2 = writeOggPage(dst, dstEnd, tagsBuf, tagsSize, 0, 0);
        if (wrote2 == 0)
            return static_cast<int>(dst - dstBuffer);
        dst += wrote2;
    }

    // ---- Audio page header for the current packet ----
    // The raw Opus packet data (avPacket.data / avPacket.size) will be
    // appended by the muxer right after what we write here.  Together
    // they form a complete OGG page.
    if (avPacket.data != nullptr && avPacket.size > 0)
    {
        // Determine frame samples for granule position
        const int samples = parseFrameSamples(avPacket.data, avPacket.size);
        m_granulePos += samples;

        const int payloadSize = avPacket.size;
        const int numSegments = payloadSize / 255 + 1;
        const int headerSize = 27 + numSegments;

        if (dstEnd - dst < headerSize)
            return static_cast<int>(dst - dstBuffer);

        uint8_t* pageStart = dst;
        uint8_t* p = dst;

        // Capture pattern
        memcpy(p, OGG_CAPTURE, 4);
        p += 4;
        // Version
        *p++ = 0;
        // Header type
        *p++ = 0;
        // Granule position (LE int64)
        for (int i = 0; i < 8; i++) *p++ = static_cast<uint8_t>((m_granulePos >> (i * 8)) & 0xFF);
        // Serial number (LE uint32)
        for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggSerialNo >> (i * 8)) & 0xFF);
        // Page sequence number (LE uint32)
        for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>((m_oggPageSeqNo >> (i * 8)) & 0xFF);
        // CRC placeholder
        uint8_t* crcPos = p;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        // Number of segments
        *p++ = static_cast<uint8_t>(numSegments);
        // Segment table
        int remaining = payloadSize;
        for (int i = 0; i < numSegments - 1; i++)
        {
            *p++ = 255;
            remaining -= 255;
        }
        *p++ = static_cast<uint8_t>(remaining);

        // Compute CRC over header + payload (payload is in avPacket.data)
        const uint32_t crc = oggCrc32_two(pageStart, headerSize, avPacket.data, payloadSize);
        crcPos[0] = static_cast<uint8_t>(crc & 0xFF);
        crcPos[1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        crcPos[2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
        crcPos[3] = static_cast<uint8_t>((crc >> 24) & 0xFF);

        m_oggPageSeqNo++;
        m_lastPage.assign(pageStart, pageStart + headerSize);
        m_lastPage.insert(m_lastPage.end(), avPacket.data, avPacket.data + payloadSize);
        m_lastPageHeader = headerSize;
        dst += headerSize;
    }

    return static_cast<int>(dst - dstBuffer);
}
