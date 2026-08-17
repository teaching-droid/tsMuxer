
#include "matroskaParser.h"

#include <fs/systemlog.h>

#include "bitStream.h"
#include "hevc.h"
#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "vvc.h"
#include "wave.h"

using namespace wave_format;

// ------------ H-264 ---------------

ParsedH264TrackData::ParsedH264TrackData(uint8_t* buff, const int size) : ParsedTrackPrivData(buff, size), m_nalSize(0)
{
    m_firstExtract = true;
    if (buff == nullptr)
        return;
    BitStreamReader bitReader{};
    try
    {
        bitReader.setBuffer(buff, buff + size);
        bitReader.skipBits(24);  // reserved 8, profile 8, reserved 8
        bitReader.skipBits(14);  // level 8, reserved 6
        m_nalSize = bitReader.getBits<uint8_t>(2) + 1;
        bitReader.skipBits(3);  // reserved
        const auto spsCnt = bitReader.getBits<uint8_t>(5);
        for (int i = 0; i < spsCnt; i++)
        {
            const auto spsLen = bitReader.getBits<uint16_t>(16);
            if (spsLen > 0)
            {
                m_spsPpsList.emplace_back();
                std::vector<uint8_t>& curData = m_spsPpsList[m_spsPpsList.size() - 1];
                for (int j = 0; j < spsLen; j++) curData.push_back(bitReader.getBits<uint8_t>(8));
            }
        }
        const auto ppsCnt = bitReader.getBits<uint8_t>(8);
        for (int i = 0; i < ppsCnt; i++)
        {
            const auto ppsLen = bitReader.getBits<uint16_t>(16);
            if (ppsLen > 0)
            {
                m_spsPpsList.emplace_back();
                std::vector<uint8_t>& curData = m_spsPpsList[m_spsPpsList.size() - 1];
                for (int j = 0; j < ppsLen; j++) curData.push_back(bitReader.getBits<uint8_t>(8));
            }
        }
    }
    catch (BitStreamException&)
    {
        THROW(ERR_MATROSKA_PARSE, "Can't parse H.264 private data")
    }
};

void ParsedH264TrackData::setStartCodeRule(const std::string& rule) { parseStartCodeRule(rule, m_startCodeByType); }

int ParsedH264TrackData::nalTypeOf(const uint8_t* nal, const int size) const { return size >= 1 ? nal[0] & 0x1F : -1; }

// Four bytes unless the source's own framing was recorded and says three for this NAL type. Both
// are legal and decode identically; the difference only matters when the point is to reproduce a
// source byte for byte, and Matroska keeps no start codes to copy.
void ParsedH264TrackData::writeNalHeader(uint8_t*& dst, const int nalType) const
{
    int len = 4;
    if (nalType >= 0)
    {
        const auto found = m_startCodeByType.find(nalType);
        if (found != m_startCodeByType.end())
            len = found->second;
    }
    for (int i = 0; i < len - 1; i++) *dst++ = 0;
    *dst++ = 1;
}

// An UPPER BOUND, deliberately. It counts four bytes for every start code, and a three byte one
// simply leaves a byte unused. Overstating it wastes a byte per NAL; understating it is a heap
// overflow, so this stays as it is and the packet is sized afterwards from what was written.
size_t ParsedH264TrackData::getSPSPPSLen() const
{
    size_t rez = 0;
    for (auto& i : m_spsPpsList) rez += i.size() + 4;
    return rez;
}

int ParsedH264TrackData::writeSPSPPS(uint8_t* dst) const
{
    const uint8_t* start = dst;
    for (auto& i : m_spsPpsList)
    {
        writeNalHeader(dst, nalTypeOf(i.data(), static_cast<int>(i.size())));
        memcpy(dst, i.data(), i.size());
        dst += i.size();
    }
    return static_cast<int>(dst - start);
}

bool ParsedH264TrackData::spsppsExists(uint8_t* buff, const int size)
{
    uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool spsFound = false;
    bool ppsFound = false;
    while (curPos < end - m_nalSize)
    {
        uint32_t elSize;
        if (m_nalSize == 4)
        {
            const auto cur32 = reinterpret_cast<uint32_t*>(curPos);
            elSize = my_ntohl(*cur32);
        }
        else
            elSize = (curPos[0] << 16l) + (curPos[1] << 8l) + curPos[2];
        const auto nalUnitType = static_cast<NALUnit::NALType>(curPos[m_nalSize] & 0x1f);
        if (nalUnitType == NALUnit::NALType::nuSPS)
            spsFound = true;
        else if (nalUnitType == NALUnit::NALType::nuPPS)
            ppsFound = true;
        curPos += elSize + m_nalSize;
    }
    return spsFound && ppsFound;
}

void ParsedH264TrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    int newBufSize = size;
    uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    int elements = 0;

    if (m_firstExtract && spsppsExists(buff, size))
        m_firstExtract = false;

    if (m_firstExtract)
        newBufSize += static_cast<int>(getSPSPPSLen());

    while (curPos <= end - m_nalSize)
    {
        uint32_t elSize = 0;
        if (m_nalSize == 4)
        {
            const auto cur32 = reinterpret_cast<uint32_t*>(curPos);
            elSize = my_ntohl(*cur32);
        }
        else if (m_nalSize == 3)
            elSize = (curPos[0] << 16l) + (curPos[1] << 8l) + curPos[2];
        else if (m_nalSize == 2)
            elSize = (curPos[0] << 8l) + curPos[1];
        else
            THROW(ERR_COMMON, "Unsupported nal unit size " << elSize)
        elements++;
        curPos += elSize + m_nalSize;
    }
    assert(curPos == end);
    if (curPos > end)
    {
        LTRACE(LT_ERROR, 2, "Matroska parse error: invalid H264 NAL unit size. NAL unit truncated.");
    }
    // Sized for a four byte start code on every NAL, which is the largest this can write. Where the
    // source's own framing was three, one byte per NAL goes unused and the packet is sized below
    // from what was actually produced.
    newBufSize += elements * (4 - m_nalSize);
    pkt->data = new uint8_t[newBufSize];

    uint8_t* dst = pkt->data;
    if (m_firstExtract)
    {
        dst += writeSPSPPS(dst);
    }
    curPos = buff;
    while (curPos <= end - m_nalSize)
    {
        uint32_t elSize = 0;
        if (m_nalSize == 4)
        {
            const auto cur32 = reinterpret_cast<uint32_t*>(curPos);
            elSize = my_ntohl(*cur32);
        }
        else if (m_nalSize == 3)
            elSize = (curPos[0] << 16l) + (curPos[1] << 8l) + curPos[2];
        else if (m_nalSize == 2)
            elSize = (curPos[0] << 8l) + curPos[1];
        else
            THROW(ERR_COMMON, "Unsupported nal unit size " << elSize)
        const auto avail = static_cast<int>(end - (curPos + m_nalSize));
        writeNalHeader(dst, nalTypeOf(curPos + m_nalSize, FFMIN(static_cast<int>(elSize), avail)));
        assert((curPos[m_nalSize] & 0x80) == 0);
        memcpy(dst, curPos + m_nalSize, FFMIN(elSize, (uint32_t)(end - curPos)));
        curPos += elSize + m_nalSize;
        dst += elSize;
    }
    pkt->size = static_cast<int>(dst - pkt->data);
    m_firstExtract = false;
}

// ----------- H.265 -----------------
ParsedH265TrackData::ParsedH265TrackData(const uint8_t* buff, const int size) : ParsedH264TrackData(nullptr, 0)
{
    m_spsPpsList = hevc_extract_priv_data(buff, size, &m_nalSize);
}

int ParsedH265TrackData::nalTypeOf(const uint8_t* nal, const int size) const
{
    return size >= 1 ? (nal[0] >> 1) & 0x3F : -1;
}

bool ParsedH265TrackData::spsppsExists(uint8_t* buff, const int size)
{
    uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool vpsFound = false;
    bool spsFound = false;
    bool ppsFound = false;
    while (curPos < end - m_nalSize)
    {
        uint32_t elSize;
        if (m_nalSize == 4)
        {
            const auto cur32 = reinterpret_cast<uint32_t*>(curPos);
            elSize = my_ntohl(*cur32);
        }
        else
            elSize = (curPos[0] << 16l) + (curPos[1] << 8l) + curPos[2];
        const auto nalUnitType = static_cast<HevcUnit::NalType>((curPos[m_nalSize] >> 1) & 0x3f);
        if (nalUnitType == HevcUnit::NalType::VPS)
            vpsFound = true;
        else if (nalUnitType == HevcUnit::NalType::SPS)
            spsFound = true;
        else if (nalUnitType == HevcUnit::NalType::PPS)
            ppsFound = true;
        curPos += elSize + m_nalSize;
    }
    return vpsFound && spsFound && ppsFound;
}

// ----------- H.266 -----------------
ParsedH266TrackData::ParsedH266TrackData(const uint8_t* buff, const int size) : ParsedH264TrackData(nullptr, 0)
{
    m_spsPpsList = vvc_extract_priv_data(buff, size, &m_nalSize);
}

// VVC puts the type in the SECOND byte: byte 0 is the forbidden zero bit, a reserved bit and the
// layer id, byte 1 is the five type bits and the temporal id.
int ParsedH266TrackData::nalTypeOf(const uint8_t* nal, const int size) const
{
    return size >= 2 ? (nal[1] >> 3) & 0x1F : -1;
}

bool ParsedH266TrackData::spsppsExists(uint8_t* buff, const int size)
{
    uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool vpsFound = false;
    bool spsFound = false;
    bool ppsFound = false;
    while (curPos < end - m_nalSize)
    {
        uint32_t elSize;
        if (m_nalSize == 4)
        {
            const auto cur32 = reinterpret_cast<uint32_t*>(curPos);
            elSize = my_ntohl(*cur32);
        }
        else
            elSize = (curPos[0] << 16l) + (curPos[1] << 8l) + curPos[2];
        const auto nalUnitType = static_cast<VvcUnit::NalType>(curPos[m_nalSize + 1] >> 3);
        if (nalUnitType == VvcUnit::NalType::VPS)
            vpsFound = true;
        else if (nalUnitType == VvcUnit::NalType::SPS)
            spsFound = true;
        else if (nalUnitType == VvcUnit::NalType::PPS)
            ppsFound = true;
        curPos += elSize + m_nalSize;
    }
    return vpsFound && spsFound && ppsFound;
}

// ------------ AV1 ---------------

#include "av1.h"

ParsedAV1TrackData::ParsedAV1TrackData(const uint8_t* buff, const int size) : m_firstExtract(true)
{
    // Parse AV1CodecConfigurationRecord from codec private data
    // and extract config OBUs (Sequence Header, etc.) as start-code-prefixed buffers
    m_configOBUs = av1_extract_priv_data(buff, size);
}

void ParsedAV1TrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    // Convert AV1 OBUs from MKV's low-overhead format (obu_has_size_field=1,
    // LEB128 sizes) to start-code-prefixed format for internal processing.
    //
    // A Temporal Delimiter OBU is inserted at the start of each block so the
    // stream reader can detect source block boundaries and preserve bundled
    // temporal units (blocks containing multiple FRAME OBUs).

    // First pass: calculate output size
    const uint8_t* src = buff;
    const uint8_t* end = buff + size;
    size_t totalSize = 0;

    // Add config OBU sizes if first extract
    if (m_firstExtract)
    {
        for (const auto& obu : m_configOBUs) totalSize += obu.size();
    }

    // Estimate size for converted OBUs (start code + header + emulation overhead)
    // +4 for the TD boundary marker
    totalSize += size * 2 + 16 + 4;

    pkt->data = new uint8_t[totalSize];
    uint8_t* dst = pkt->data;

    // Insert config OBUs before first frame
    if (m_firstExtract)
    {
        for (const auto& obu : m_configOBUs)
        {
            memcpy(dst, obu.data(), obu.size());
            dst += obu.size();
        }
        m_firstExtract = false;
    }

    // Temporal Delimiter OBU as block boundary marker (start-code + header byte 0x10)
    *dst++ = 0x00;
    *dst++ = 0x00;
    *dst++ = 0x01;
    *dst++ = 0x10;  // TD OBU header

    // Convert each OBU from low-overhead format to start-code format
    src = buff;
    while (src < end)
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(src, end);
        if (hdrLen < 0)
            break;

        if (!obuHdr.obu_has_size_field)
        {
            // Can't determine OBU boundaries without size field
            break;
        }

        int leb128Bytes = 0;
        const int64_t obuPayloadSize = decodeLeb128(src + hdrLen, end, leb128Bytes);
        if (obuPayloadSize < 0)
            break;

        const uint8_t* payload = src + hdrLen + leb128Bytes;
        if (payload + obuPayloadSize > end)
            break;

        // Skip source temporal delimiter OBUs (we've inserted our own above)
        if (obuHdr.obu_type == Av1ObuType::TEMPORAL_DELIMITER)
        {
            src = payload + obuPayloadSize;
            continue;
        }

        // Write start code
        *dst++ = 0x00;
        *dst++ = 0x00;
        *dst++ = 0x01;

        // Write OBU header byte(s) with obu_has_size_field cleared
        *dst++ = src[0] & ~0x02;
        if (obuHdr.obu_extension_flag)
            *dst++ = src[1];

        // Write payload with emulation prevention
        if (obuPayloadSize > 0)
        {
            const int encoded =
                av1_add_emulation_prevention(payload, payload + obuPayloadSize, dst, totalSize - (dst - pkt->data));
            if (encoded > 0)
                dst += encoded;
            else
            {
                // Fallback: copy raw (shouldn't happen with proper buffer sizing)
                memcpy(dst, payload, obuPayloadSize);
                dst += obuPayloadSize;
            }
        }

        src = payload + obuPayloadSize;
    }

    pkt->size = static_cast<int>(dst - pkt->data);
}

// ------------ VC-1 ---------------

static constexpr int MS_BIT_MAP_HEADER_SIZE = 40;
ParsedVC1TrackData::ParsedVC1TrackData(uint8_t* buff, const int size) : ParsedTrackPrivData(buff, size)
{
    if (size < MS_BIT_MAP_HEADER_SIZE)
        THROW(ERR_MATROSKA_PARSE, "Matroska parse error: Invalid or unsupported VC-1 stream")
    const uint8_t* curBuf = buff + MS_BIT_MAP_HEADER_SIZE;
    const uint8_t dataLen = *curBuf++;
    for (int i = 0; i < dataLen; i++) m_seqHeader.push_back(*curBuf++);
    m_firstPacket = true;
}

void ParsedVC1TrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    if (size < 0)
    {
        pkt->data = nullptr;
        pkt->size = 0;
        return;
    }

    // Allocation accounts for: size + seqHeader (if first packet) + 4-byte frame header (if needed).
    // The dst pointer advances by exactly seqHeader.size() and/or 4 before copying the remaining
    // size bytes, so the total write fits the allocation precisely.
    pkt->size = size + (m_firstPacket ? static_cast<int>(m_seqHeader.size()) : 0);
    const bool addFrameHdr = !(size >= 4 && buff[0] == 0 && buff[1] == 0 && buff[2] == 1);
    if (addFrameHdr)
        pkt->size += 4;
    pkt->data = new uint8_t[pkt->size];
    uint8_t* dst = pkt->data;
    if (m_firstPacket && !m_seqHeader.empty())
    {
        memcpy(dst, m_seqHeader.data(), m_seqHeader.size());
        dst += m_seqHeader.size();
    }
    if (addFrameHdr)
    {
        *dst++ = 0;
        *dst++ = 0;
        *dst++ = 1;
        *dst++ = 0x0d;
    }
    memcpy(dst, buff, size);
    m_firstPacket = false;
}

// ------------ AAC --------------

ParsedAACTrackData::ParsedAACTrackData(uint8_t* buff, const int size) : ParsedTrackPrivData(buff, size)
{
    m_aacRaw.m_id = 1;  // MPEG2
    m_aacRaw.m_layer = 0;
    m_aacRaw.m_rdb = 0;
    m_aacRaw.readConfig(buff, size);
}

void ParsedAACTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    pkt->size = size + AAC_HEADER_LEN;
    pkt->data = new uint8_t[pkt->size];
    m_aacRaw.buildADTSHeader(pkt->data, size + AAC_HEADER_LEN);
    memcpy(pkt->data + AAC_HEADER_LEN, buff, size);
}

// ------------ LPCM --------------
ParsedLPCMTrackData::ParsedLPCMTrackData(MatroskaTrack* track)
    : ParsedTrackPrivData(track->codec_priv, track->codec_priv_size)
{
    m_convertBytes = strEndWith(track->codec_id, "/BIG");
    const auto audiotrack = reinterpret_cast<MatroskaAudioTrack*>(track);
    m_channels = audiotrack->channels;
    m_bitdepth = audiotrack->bitdepth;

    buildWaveHeader(m_waveBuffer, audiotrack->samplerate, m_channels, m_channels >= 6, m_bitdepth);

    if (track->codec_priv_size == sizeof(WAVEFORMATPCMEX))
        memcpy(m_waveBuffer.data() + 20, track->codec_priv, track->codec_priv_size);
}

void ParsedLPCMTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    pkt->size = size + static_cast<int>(m_waveBuffer.size());
    pkt->data = new uint8_t[pkt->size];
    uint8_t* dst = pkt->data;
    if (!m_waveBuffer.isEmpty())
    {
        memcpy(dst, m_waveBuffer.data(), m_waveBuffer.size());
        dst += m_waveBuffer.size();
        m_waveBuffer.clear();
    }
    if (m_convertBytes)
        toLittleEndian(dst, buff, size, m_bitdepth);
    else
        memcpy(dst, buff, size);
}

// ------------ AC3 ---------------

ParsedAC3TrackData::ParsedAC3TrackData(uint8_t* buff, const int size) : ParsedTrackPrivData(buff, size)
{
    m_firstPacket = true;
    m_shortHeaderMode = false;
}

void ParsedAC3TrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    if (m_firstPacket && size > 2)
    {
        if (AV_RB16(buff) != 0x0b77)
            m_shortHeaderMode = true;
    }
    m_firstPacket = false;
    pkt->size = size + (m_shortHeaderMode ? 2 : 0);
    pkt->data = new uint8_t[pkt->size];
    uint8_t* dst = pkt->data;
    if (m_shortHeaderMode)
    {
        *dst++ = 0x0b;
        *dst++ = 0x77;
    }
    memcpy(dst, buff, size);
}

// ------------ SRT ---------------

ParsedSRTTrackData::ParsedSRTTrackData(uint8_t* buff, const int size) : ParsedTrackPrivData(buff, size)
{
    m_packetCnt = 0;
}

void ParsedSRTTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    std::string prefix;
    if (m_packetCnt == 0)
        prefix = "\xEF\xBB\xBF";  // UTF-8 header
    prefix += int32ToStr(++m_packetCnt);
    prefix += "\n";
    prefix += floatToTime(static_cast<double>(pkt->pts) / INTERNAL_PTS_FREQ, ',');
    prefix += " --> ";
    prefix += floatToTime(static_cast<double>(pkt->pts + pkt->duration) / INTERNAL_PTS_FREQ, ',');
    prefix += '\n';
    const std::string postfix = "\n\n";
    pkt->size = static_cast<int>(size + prefix.length() + postfix.length());
    pkt->data = new uint8_t[pkt->size];
    memcpy(pkt->data, prefix.c_str(), prefix.length());
    memcpy(pkt->data + prefix.length(), buff, size);
    memcpy(pkt->data + prefix.length() + size, postfix.c_str(), postfix.length());
}

// ------------ FLAC ---------------
ParsedFLACTrackData::ParsedFLACTrackData(uint8_t* buff, const int size)
    : ParsedTrackPrivData(buff, size), m_firstPacket(true)
{
    // Store the CodecPrivate data (fLaC marker + metadata blocks).
    if (buff && size > 0)
        m_codecPrivate.assign(buff, buff + size);
}

void ParsedFLACTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    if (m_firstPacket && !m_codecPrivate.empty())
    {
        // Prepend the CodecPrivate (fLaC + STREAMINFO) before the first FLAC frame
        // so the stream reader can parse it via findFrame().
        m_firstPacket = false;
        const int privSize = static_cast<int>(m_codecPrivate.size());
        pkt->size = privSize + size;
        pkt->data = new uint8_t[pkt->size];
        memcpy(pkt->data, m_codecPrivate.data(), privSize);
        memcpy(pkt->data + privSize, buff, size);
    }
    else
    {
        // Subsequent frames: pass through unchanged.
        pkt->size = size;
        pkt->data = new uint8_t[size];
        memcpy(pkt->data, buff, size);
    }
}

// ------------ Opus ---------------
ParsedOpusTrackData::ParsedOpusTrackData(uint8_t* buff, const int size)
    : ParsedTrackPrivData(buff, size), m_firstPacket(true)
{
    if (buff && size > 0)
        m_codecPrivate.assign(buff, buff + size);
}

void ParsedOpusTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    // Each Opus packet is prefixed with a 4-byte big-endian length so that
    // OpusStreamReader can find frame boundaries (Opus has no sync codes).
    static constexpr int PREFIX = 4;

    if (m_firstPacket && !m_codecPrivate.empty())
    {
        // First packet: prepend raw OpusHead so the stream reader can parse it.
        m_firstPacket = false;
        const int privSize = static_cast<int>(m_codecPrivate.size());
        pkt->size = privSize + PREFIX + size;
        pkt->data = new uint8_t[pkt->size];
        memcpy(pkt->data, m_codecPrivate.data(), privSize);
        // Length prefix for the Opus audio packet
        pkt->data[privSize] = static_cast<uint8_t>((size >> 24) & 0xFF);
        pkt->data[privSize + 1] = static_cast<uint8_t>((size >> 16) & 0xFF);
        pkt->data[privSize + 2] = static_cast<uint8_t>((size >> 8) & 0xFF);
        pkt->data[privSize + 3] = static_cast<uint8_t>(size & 0xFF);
        memcpy(pkt->data + privSize + PREFIX, buff, size);
    }
    else
    {
        pkt->size = PREFIX + size;
        pkt->data = new uint8_t[pkt->size];
        pkt->data[0] = static_cast<uint8_t>((size >> 24) & 0xFF);
        pkt->data[1] = static_cast<uint8_t>((size >> 16) & 0xFF);
        pkt->data[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
        pkt->data[3] = static_cast<uint8_t>(size & 0xFF);
        memcpy(pkt->data + PREFIX, buff, size);
    }
}

// ------------ PG ---------------
void ParsedPGTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    static constexpr int PG_HEADER_SIZE = 10;

    const uint8_t* curPtr = buff;
    const uint8_t* end = buff + size;
    int blocks = 0;
    while (curPtr <= end - 3)
    {
        const uint16_t blockSize = AV_RB16(curPtr + 1) + 3;
        if (blockSize == 0)
            break;
        curPtr += blockSize;
        blocks++;
    }
    // assert(curPtr == end);
    if (curPtr != end)
    {
        pkt->size = 0;
        pkt->data = nullptr;
        return;  // ignore invalid packet
    }

    pkt->size = size + PG_HEADER_SIZE * blocks;
    pkt->data = new uint8_t[pkt->size];
    curPtr = buff;
    uint8_t* dst = pkt->data;
    while (curPtr <= end - 3)
    {
        const uint16_t blockSize = AV_RB16(curPtr + 1) + 3;

        dst[0] = 'P';
        dst[1] = 'G';
        const auto ptsDts = reinterpret_cast<uint32_t*>(dst + 2);
        ptsDts[0] = my_htonl(static_cast<uint32_t>(internalClockToPts(pkt->pts)));
        ptsDts[1] = 0;
        dst += PG_HEADER_SIZE;
        memcpy(dst, curPtr, blockSize);
        curPtr += blockSize;
        dst += blockSize;
    }
    assert(dst == pkt->data + pkt->size);
}
