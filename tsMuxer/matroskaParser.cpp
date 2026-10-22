
#include "matroskaParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <fs/systemlog.h>

#include "bitStream.h"
#include "hevc.h"
#include "nalUnits.h"
#include "textSubtitles.h"
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

// The one rule every walk over a block's NAL units follows. A NAL unit starts at curPos only if a whole
// length field is there and the length it gives fits in what is left of the block. The comparison is made
// in 64 bits, so a length near 4 GiB can neither wrap round to a small step nor send a copy past the block.
bool ParsedH264TrackData::nextNal(const uint8_t* curPos, const uint8_t* end, uint32_t& elSize) const
{
    if (end - curPos < m_nalSize)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < m_nalSize; ++i) v = (v << 8) | curPos[i];
    elSize = v;
    return static_cast<int64_t>(elSize) <= (end - curPos) - m_nalSize;
}

bool ParsedH264TrackData::spsppsExists(uint8_t* buff, const int size)
{
    const uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool spsFound = false;
    bool ppsFound = false;
    uint32_t elSize = 0;
    while (nextNal(curPos, end, elSize))
    {
        const auto nalUnitType = static_cast<NALUnit::NALType>(nalTypeOf(curPos + m_nalSize, static_cast<int>(elSize)));
        if (nalUnitType == NALUnit::NALType::nuSPS)
            spsFound = true;
        else if (nalUnitType == NALUnit::NALType::nuPPS)
            ppsFound = true;
        curPos += m_nalSize + elSize;
    }
    return spsFound && ppsFound;
}

void ParsedH264TrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    if (m_nalSize < 2 || m_nalSize > 4)
        THROW(ERR_COMMON, "Unsupported nal unit size " << static_cast<int>(m_nalSize))

    int newBufSize = size;
    const uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    int elements = 0;

    if (m_firstExtract && spsppsExists(buff, size))
        m_firstExtract = false;

    if (m_firstExtract)
        newBufSize += static_cast<int>(getSPSPPSLen());

    // The sizing walk and the copy walk below stop at the same NAL unit, so the copy can never write more
    // than was measured here. A NAL unit whose length runs past the end of the block is left out, with
    // everything after it, as the MP4 reader does: what is there of it is not a NAL unit any more.
    uint32_t elSize = 0;
    while (nextNal(curPos, end, elSize))
    {
        elements++;
        curPos += m_nalSize + elSize;
    }
    if (curPos < end)
    {
        if (end - curPos >= m_nalSize)
        {
            uint32_t declared = 0;
            for (int i = 0; i < m_nalSize; ++i) declared = (declared << 8) | curPos[i];
            LTRACE(LT_WARN, 2,
                   "Matroska: a NAL unit declares " << declared << " bytes but its block holds "
                                                    << (end - curPos) - m_nalSize << ", so it is left out");
        }
        else
            LTRACE(LT_WARN, 2,
                   "Matroska: " << end - curPos << " stray byte(s) after the last NAL unit of a block left out");
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
    for (int n = 0; n < elements && nextNal(curPos, end, elSize); ++n)
    {
        writeNalHeader(dst, nalTypeOf(curPos + m_nalSize, static_cast<int>(elSize)));
        assert(elSize == 0 || (curPos[m_nalSize] & 0x80) == 0);
        memcpy(dst, curPos + m_nalSize, elSize);
        curPos += m_nalSize + elSize;
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
    const uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool vpsFound = false;
    bool spsFound = false;
    bool ppsFound = false;
    uint32_t elSize = 0;
    while (nextNal(curPos, end, elSize))
    {
        const auto nalUnitType =
            static_cast<HevcUnit::NalType>(nalTypeOf(curPos + m_nalSize, static_cast<int>(elSize)));
        if (nalUnitType == HevcUnit::NalType::VPS)
            vpsFound = true;
        else if (nalUnitType == HevcUnit::NalType::SPS)
            spsFound = true;
        else if (nalUnitType == HevcUnit::NalType::PPS)
            ppsFound = true;
        curPos += m_nalSize + elSize;
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
    const uint8_t* curPos = buff;
    const uint8_t* end = buff + size;
    bool vpsFound = false;
    bool spsFound = false;
    bool ppsFound = false;
    uint32_t elSize = 0;
    while (nextNal(curPos, end, elSize))
    {
        const auto nalUnitType = static_cast<VvcUnit::NalType>(nalTypeOf(curPos + m_nalSize, static_cast<int>(elSize)));
        if (nalUnitType == VvcUnit::NalType::VPS)
            vpsFound = true;
        else if (nalUnitType == VvcUnit::NalType::SPS)
            spsFound = true;
        else if (nalUnitType == VvcUnit::NalType::PPS)
            ppsFound = true;
        curPos += m_nalSize + elSize;
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

        // The last OBU of a block may leave out its size field (Matroska AV1 mapping, as the ISOBMFF one): it runs
        // to the end of the block. It was dropped, and with it the picture of every block that ended so.
        int leb128Bytes = 0;
        const int64_t obuPayloadSize =
            obuHdr.obu_has_size_field ? decodeLeb128(src + hdrLen, end, leb128Bytes) : end - (src + hdrLen);
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
ParsedVC1TrackData::ParsedVC1TrackData(uint8_t* buff, const int size, const bool decSpecStruc)
    : ParsedTrackPrivData(buff, size)
{
    m_firstPacket = true;
    if (decSpecStruc)
    {
        // V_VC1 (Matroska codec mapping, 3.3.27): a VC1DecSpecStruc (SMPTE RP 2025), whose fixed fields are followed
        // by the sequence header and the entry point, each with its start code. Taken from the sequence header's
        // start code on. The track was not read at all.
        for (int i = 0; i + 4 <= size; ++i)
            if (buff[i] == 0 && buff[i + 1] == 0 && buff[i + 2] == 1 && buff[i + 3] == 0x0F)
            {
                m_seqHeader.assign(buff + i, buff + size);
                break;
            }
        return;
    }
    if (size < MS_BIT_MAP_HEADER_SIZE)
        THROW(ERR_MATROSKA_PARSE, "Matroska parse error: Invalid or unsupported VC-1 stream")
    // After the 40 byte bitmap header: one byte with the length of the sequence header, then the header. A
    // CodecPrivate of the bitmap header alone has neither, and both used to be read from past its end; a length
    // larger than what follows it took that many bytes from past the end as well. Only what is there is taken.
    if (size > MS_BIT_MAP_HEADER_SIZE)
    {
        const uint8_t* curBuf = buff + MS_BIT_MAP_HEADER_SIZE;
        const int dataLen = (std::min)(static_cast<int>(*curBuf++), size - MS_BIT_MAP_HEADER_SIZE - 1);
        m_seqHeader.assign(curBuf, curBuf + dataLen);
    }
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
    // ID 0, MPEG-4: an A_AAC track is described by an AudioSpecificConfig, which is MPEG-4 audio. The ADTS headers
    // were written with ID 1 (MPEG-2), so AAC that came in as MPEG-4 ADTS changed its headers on a way through
    // Matroska.
    m_aacRaw.m_id = 0;
    m_aacRaw.m_layer = 0;
    m_aacRaw.m_rdb = 0;
    m_aacRaw.readConfig(buff, size);
}

void ParsedAACTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    // A layout given by a program config element goes into every frame: an ADTS header cannot name it.
    const auto& pce = m_aacRaw.inBandPce();
    const int headerLen = AAC_HEADER_LEN + static_cast<int>(pce.size());
    pkt->size = size + headerLen;
    pkt->data = new uint8_t[pkt->size];
    m_aacRaw.buildADTSHeader(pkt->data, size + headerLen);
    if (!pce.empty())
        memcpy(pkt->data + AAC_HEADER_LEN, pce.data(), pce.size());
    memcpy(pkt->data + headerLen, buff, size);
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

// The text of an ASS or SSA event as SRT text. The block holds ReadOrder, Layer, Style, Name, MarginL, MarginR,
// MarginV, Effect and Text (Matroska codec mapping, 5.3); the text is everything after the eighth comma. Override codes
// in braces turn italic, bold and underline on and off, which SRT has as <i>, <b> and <u>; the others (position,
// colour, karaoke and the like) are dropped, and so is the text of a drawing (\p1 up to \p0). \N and \n are line
// breaks and \h a hard space.
std::string assToSrtText(const std::string& event)
{
    size_t start = 0;
    for (int comma = 0; comma < 8 && start != std::string::npos; ++comma)
    {
        start = event.find(',', start);
        if (start != std::string::npos)
            ++start;
    }
    const std::string text = start == std::string::npos ? event : event.substr(start);
    std::string out;
    bool italic = false, bold = false, underline = false, drawing = false;
    const auto toggle = [&out](bool& state, const bool on, const char* tag)
    {
        if (on == state)
            return;
        state = on;
        out += on ? std::string("<") + tag + ">" : std::string("</") + tag + ">";
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '{')
        {
            const size_t close = text.find('}', i);
            if (close == std::string::npos)
                break;
            const std::string block = text.substr(i + 1, close - i - 1);
            for (size_t t = block.find('\\'); t != std::string::npos; t = block.find('\\', t + 1))
            {
                const std::string code = block.substr(t + 1, block.find('\\', t + 1) - t - 1);
                if (code.size() >= 2 && code[0] == 'i' && isdigit(static_cast<unsigned char>(code[1])))
                    toggle(italic, code[1] != '0', "i");
                else if (code.size() >= 2 && code[0] == 'b' && isdigit(static_cast<unsigned char>(code[1])))
                    toggle(bold, code[1] != '0', "b");
                else if (code.size() >= 2 && code[0] == 'u' && isdigit(static_cast<unsigned char>(code[1])))
                    toggle(underline, code[1] != '0', "u");
                else if (code.size() >= 2 && code[0] == 'p' && isdigit(static_cast<unsigned char>(code[1])))
                    drawing = code[1] != '0';
                else if (code == "r")
                {
                    toggle(italic, false, "i");
                    toggle(bold, false, "b");
                    toggle(underline, false, "u");
                }
            }
            i = close;
            continue;
        }
        if (drawing)
            continue;
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n'))
        {
            out += '\n';
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'h')
        {
            out += ' ';
            ++i;
            continue;
        }
        out += c;
    }
    toggle(underline, false, "u");
    toggle(bold, false, "b");
    toggle(italic, false, "i");
    return out;
}

// The text of a WebVTT cue as SRT text: <i>, <b> and <u> kept, the other tags (classes, voices, languages, ruby,
// timestamps) dropped with their text kept, the character references of the format turned into their characters.
std::string webVttToSrtText(const std::string& cue)
{
    std::string out;
    for (size_t i = 0; i < cue.size(); ++i)
    {
        if (cue[i] == '<')
        {
            const size_t close = cue.find('>', i);
            if (close == std::string::npos)
                break;
            std::string tag = cue.substr(i + 1, close - i - 1);
            const bool end = !tag.empty() && tag[0] == '/';
            std::string name = tag.substr(end ? 1 : 0);
            name = name.substr(0, name.find_first_of(". \t"));
            if (name == "i" || name == "b" || name == "u")
                out += std::string("<") + (end ? "/" : "") + name + ">";
            i = close;
            continue;
        }
        if (cue[i] == '&')
        {
            static const std::pair<const char*, const char*> REFS[] = {{"&amp;", "&"},
                                                                       {"&lt;", "<"},
                                                                       {"&gt;", ">"},
                                                                       {"&nbsp;", "\xC2\xA0"},
                                                                       {"&lrm;", "\xE2\x80\x8E"},
                                                                       {"&rlm;", "\xE2\x80\x8F"}};
            bool done = false;
            for (const auto& [ref, value] : REFS)
                if (cue.compare(i, strlen(ref), ref) == 0)
                {
                    out += value;
                    i += strlen(ref) - 1;
                    done = true;
                    break;
                }
            if (done)
                continue;
            // A numeric reference, &#233; or &#x263A; (WebVTT 4.5, as HTML), was kept as written
            if (i + 1 < cue.size() && cue[i + 1] == '#')
            {
                const bool hex = i + 2 < cue.size() && (cue[i + 2] == 'x' || cue[i + 2] == 'X');
                size_t j = i + (hex ? 3 : 2);
                uint32_t code = 0;
                const size_t first = j;
                for (; j < cue.size() && j - first < 8 && isxdigit(static_cast<unsigned char>(cue[j])); ++j)
                {
                    if (!hex && !isdigit(static_cast<unsigned char>(cue[j])))
                        break;
                    const auto c = static_cast<unsigned char>(cue[j]);
                    code =
                        code * (hex ? 16U : 10U) + static_cast<uint32_t>(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
                }
                if (j > first && j < cue.size() && cue[j] == ';')
                {
                    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                        code = 0xFFFD;  // as HTML takes a reference to no character
                    if (code < 0x80)
                        out += static_cast<char>(code);
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | code >> 6);
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else if (code < 0x10000)
                    {
                        out += static_cast<char>(0xE0 | code >> 12);
                        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xF0 | code >> 18);
                        out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
                        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    i = j;
                    continue;
                }
            }
        }
        out += cue[i];
    }
    return out;
}

ParsedSRTTrackData::ParsedSRTTrackData(uint8_t* buff, const int size, const Form form)
    : ParsedTrackPrivData(buff, size), m_form(form)
{
    m_packetCnt = 0;
}

void ParsedSRTTrackData::extractData(AVPacket* pkt, uint8_t* buff, int size)
{
    // ASS, SSA and WebVTT reach the SRT reader as SRT text, which tsMuxeR renders to PGS or keeps as text. They were
    // not read at all: a Matroska file with such a track listed it as a stream of unknown type.
    std::string converted;
    if (m_form != Form::Srt)
    {
        std::string text(reinterpret_cast<const char*>(buff), static_cast<size_t>(size));
        if (m_form == Form::WebmVtt)
        {
            // the cue identifier and the cue settings, a line each, come first
            for (int line = 0; line < 2; ++line)
            {
                const size_t nl = text.find('\n');
                text = nl == std::string::npos ? std::string() : text.substr(nl + 1);
            }
        }
        converted = m_form == Form::Ass ? assToSrtText(text) : webVttToSrtText(text);
        buff = reinterpret_cast<uint8_t*>(converted.data());
        size = static_cast<int>(converted.size());
    }

    std::string prefix;
    if (m_packetCnt == 0)
        prefix = "\xEF\xBB\xBF";  // UTF-8 header
    prefix += int32ToStr(++m_packetCnt);
    prefix += "\n";
    prefix += floatToTime(static_cast<double>(pkt->pts) / INTERNAL_PTS_FREQ, ',');
    prefix += " --> ";
    // A SimpleBlock carries no duration, and a track without a DefaultDuration then shows each
    // block until the next one (RFC 9559). The end used to be written from the "no timestamp"
    // value, a time hours away. No end is written instead, which the SRT reader takes as until the
    // next subtitle.
    if (pkt->duration != AV_NOPTS_VALUE_INTERNAL)
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

// ------------ VP8 and VP9 ---------------
void ParsedVpxTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    // Escaping adds at most one byte for every two.
    const size_t room = static_cast<size_t>(size) + size / 2 + 8;
    pkt->data = new uint8_t[room];
    pkt->data[0] = 0;
    pkt->data[1] = 0;
    pkt->data[2] = 1;
    int encoded = size > 0 ? NALUnit::encodeNAL(buff, buff + size, pkt->data + 3, room - 4) : 0;
    if (encoded < 0)
        encoded = 0;
    pkt->data[3 + encoded] = 0x80;
    pkt->size = encoded + 4;
}

// ------------ Vorbis and ALAC ---------------
ParsedMarkedTrackData::ParsedMarkedTrackData(const char* mark, const uint8_t* buff, const int size)
{
    memcpy(m_mark, mark, 8);
    if (buff && size > 0)
        m_codecPrivate.assign(buff, buff + size);
}

void ParsedMarkedTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    const int head = m_firstPacket && !m_codecPrivate.empty() ? 12 + static_cast<int>(m_codecPrivate.size()) : 0;
    pkt->size = head + 4 + size;
    pkt->data = new uint8_t[pkt->size];
    uint8_t* dst = pkt->data;
    const auto be32 = [&dst](const uint32_t v)
    {
        for (int i = 3; i >= 0; --i) *dst++ = static_cast<uint8_t>(v >> (8 * i));
    };
    if (head)
    {
        memcpy(dst, m_mark, 8);
        dst += 8;
        be32(static_cast<uint32_t>(m_codecPrivate.size()));
        memcpy(dst, m_codecPrivate.data(), m_codecPrivate.size());
        dst += m_codecPrivate.size();
    }
    m_firstPacket = false;
    be32(static_cast<uint32_t>(size));
    memcpy(dst, buff, size);
}

// ------------ a track copied as it is ---------------
void ParsedCopyTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    pkt->size = 4 + 8 + 8 + 1 + 4 + static_cast<int>(m_additions.size()) + 4 + size;
    pkt->data = new uint8_t[pkt->size];
    uint8_t* dst = pkt->data;
    const auto be = [&dst](const uint64_t v, const int bytes)
    {
        for (int i = bytes - 1; i >= 0; --i) *dst++ = static_cast<uint8_t>(v >> (8 * i));
    };
    memcpy(dst, "MKCP", 4);
    dst += 4;
    be(static_cast<uint64_t>(pkt->pts), 8);
    be(static_cast<uint64_t>(pkt->duration == AV_NOPTS_VALUE_INTERNAL ? -1 : pkt->duration), 8);
    *dst++ = 1;  // subtitle blocks are each a point a player can start from
    be(m_additions.size(), 4);
    if (!m_additions.empty())
        memcpy(dst, m_additions.data(), m_additions.size());
    dst += m_additions.size();
    m_additions.clear();
    be(static_cast<uint32_t>(size), 4);
    if (size > 0)
        memcpy(dst, buff, size);
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
// The display sets that one block of a bitmap subtitle track becomes, the end of the picture before among them.
static void setPacketData(AVPacket* pkt, const std::vector<uint8_t>& data)
{
    pkt->size = static_cast<int>(data.size());
    pkt->data = data.empty() ? nullptr : new uint8_t[data.size()];
    if (!data.empty())
        memcpy(pkt->data, data.data(), data.size());
}

void ParsedBitmapSubTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    std::vector<uint8_t> out;
    m_decoder->decode(buff, size, pkt->pts, pkt->duration == AV_NOPTS_VALUE_INTERNAL ? -1 : pkt->duration, out);
    setPacketData(pkt, out);
}

bool ParsedBitmapSubTrackData::finish(AVPacket* pkt)
{
    std::vector<uint8_t> out;
    m_decoder->finish(out);
    setPacketData(pkt, out);
    return !out.empty();
}

void ParsedPGTrackData::extractData(AVPacket* pkt, uint8_t* buff, const int size)
{
    static constexpr int PG_HEADER_SIZE = 10;

    // A segment is a type byte, a 16 bit length and that many bytes. The size with its 3 header bytes was kept in
    // 16 bits as well, so a segment of 65,533 bytes or more came out as 0, 1 or 2 bytes long, and the packet was
    // walked from the wrong places.
    const uint8_t* curPtr = buff;
    const uint8_t* end = buff + size;
    int blocks = 0;
    while (curPtr <= end - 3)
    {
        const int blockSize = AV_RB16(curPtr + 1) + 3;
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

    // ** A DISPLAY SET GETS THE DECODING TIMES OF A DISC. **
    // Matroska keeps one time for each display set, the time it is shown, so every segment was given that time and no
    // decoding time: a .sup demuxed from it, and a disc made from it, had the palette and the pictures decoded at the
    // moment they were to be shown. On a disc they come before it, by the time the decoder of the subtitle plane needs:
    // the plane is cleared at the start of an epoch, the windows are drawn, and each picture is decoded, at the pixel
    // rates the drawn subtitles of tsMuxeR use as well (textSubtitles.h). The times are those a disc gives for the same
    // set: PCS shown at its time and decoded at the start, WDS shown after the plane is cleared, PDS at the start, each
    // ODS shown when it is decoded, END with the last of them. A set never starts before the last one ended.
    using text_subtitles::PIXEL_COMPOSITION_RATE;
    using text_subtitles::PIXEL_DECODING_RATE;
    const auto ticks = [](const double pixels, const double rate)
    { return static_cast<int64_t>(90000.0 * pixels / rate + 0.999); };
    int64_t shown = internalClockToPts(pkt->pts);
    int64_t planeInit = 0;
    int64_t windowsTransfer = 0;
    std::vector<int64_t> objectDecode;  // per ODS segment: the time to decode its picture, 0 for a continuation
    for (const uint8_t* p = buff; p <= end - 3; p += AV_RB16(p + 1) + 3)
    {
        const int len = AV_RB16(p + 1);
        const uint8_t* s = p + 3;
        if (p[0] == 0x16 && len >= 8 && (s[7] & 0x80))  // PCS, epoch start: the whole plane is cleared
            planeInit = ticks(static_cast<double>(AV_RB16(s)) * AV_RB16(s + 2), PIXEL_COMPOSITION_RATE);
        else if (p[0] == 0x17 && len >= 1)  // WDS: id, x, y, width, height of each window
            for (int w = 0; w < s[0] && 1 + 9 * (w + 1) <= len; ++w)
                windowsTransfer += ticks(static_cast<double>(AV_RB16(s + 1 + 9 * w + 5)) * AV_RB16(s + 1 + 9 * w + 7),
                                         PIXEL_COMPOSITION_RATE);
        else if (p[0] == 0x15)  // ODS: the first part of an object gives its size
            objectDecode.push_back(
                len >= 11 && (s[3] & 0x80)
                    ? ticks(static_cast<double>(AV_RB16(s + 7)) * AV_RB16(s + 9), PIXEL_DECODING_RATE)
                    : 0);
    }
    // A Matroska time is in milliseconds, so a set shown on a frame came out up to half a millisecond beside it, and
    // after the frame where a player shows it one frame late (of 149 sets of a disc, 8 stayed on a frame). A time
    // within a millisecond of a frame of the rate the composition names is put on that frame.
    if (buff + 3 + 5 <= end && buff[0] == 0x16)
    {
        static constexpr double RATES[] = {0.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 0.0, 50.0, 60000.0 / 1001};
        const int rateId = buff[3 + 4] >> 4;
        if (rateId < 8 && RATES[rateId] > 0.0)
        {
            const double step = 90000.0 / RATES[rateId];
            const auto onFrame = static_cast<int64_t>(
                std::llround(static_cast<double>(std::llround(static_cast<double>(shown) / step)) * step));
            if (onFrame - shown <= 90 && shown - onFrame <= 90)
                shown = onFrame;
        }
    }
    // A decoding time of 0 in the header reads as "decoded when shown", so a set is decoded at 1 at the earliest; never
    // after it is shown, though: a set shown at 0 was given 1 and so was decoded after it was shown. It gets 0 instead.
    const int64_t start = (std::max)(
        (std::min)(shown, (std::max)({m_lastEnd, shown - planeInit - windowsTransfer, int64_t{1}})), int64_t{0});

    pkt->size = size + PG_HEADER_SIZE * blocks;
    pkt->data = new uint8_t[pkt->size];
    curPtr = buff;
    uint8_t* dst = pkt->data;
    int64_t decoded = start;  // where the last picture decoded so far is ready
    size_t ods = 0;
    while (curPtr <= end - 3)
    {
        const int blockSize = AV_RB16(curPtr + 1) + 3;

        int64_t pts = shown;
        int64_t dts = start;
        switch (curPtr[0])
        {
        case 0x16:  // PCS
            break;
        case 0x17:  // WDS
            pts = (std::max)(start, shown - windowsTransfer);
            break;
        case 0x14:  // PDS
            pts = start;
            break;
        case 0x15:  // ODS
            decoded = (std::min)(shown, decoded + objectDecode[ods++]);
            pts = decoded;
            break;
        default:  // END and anything else: with the last picture
            pts = dts = decoded;
            break;
        }
        m_lastEnd = (std::max)(m_lastEnd, pts);

        dst[0] = 'P';
        dst[1] = 'G';
        const auto ptsDts = UnalignedPtr<uint32_t>(dst + 2);
        ptsDts[0] = my_htonl(static_cast<uint32_t>(pts));
        ptsDts[1] = my_htonl(static_cast<uint32_t>(dts));
        dst += PG_HEADER_SIZE;
        memcpy(dst, curPtr, blockSize);
        curPtr += blockSize;
        dst += blockSize;
    }
    assert(dst == pkt->data + pkt->size);
}
