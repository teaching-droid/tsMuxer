#include "av1StreamReader.h"

#include <fs/systemlog.h>

#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "vpxStreamReader.h"

using namespace std;

AV1StreamReader::AV1StreamReader()
    : m_framing(Av1Framing::UNDECIDED),
      m_brokenObuWarned(false),
      m_demuxSeqHdrPending(false),
      m_demuxSawTd(false),
      m_seqHdrFound(false),
      m_firstFrame(true),
      m_lastIFrame(false),
      m_firstFileFrame(false),
      m_frameNum(0),
      m_hdrWcgIdc(3)  // 3 = no indication
{
}

const CodecInfo& AV1StreamReader::getCodecInfo() { return av1CodecInfo; }

unsigned AV1StreamReader::getStreamWidth() const { return m_seqHdrFound ? m_seqHdr.max_frame_width : 0; }

unsigned AV1StreamReader::getStreamHeight() const { return m_seqHdrFound ? m_seqHdr.max_frame_height : 0; }

double AV1StreamReader::getStreamFPS(void*) { return m_seqHdrFound ? m_seqHdr.getFPS() : 0.0; }

void AV1StreamReader::updateStreamFps(void*, uint8_t*, uint8_t*, int)
{
    // AV1 sequence headers are not modified in-place for FPS changes.
    // The manual FPS override is applied via setFPS() from the meta file.
}

void AV1StreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    m_curPts = m_curDts;  // AV1: PTS is always present
    m_frameNum++;
    m_firstFrame = false;
}

// ---------------------------------------------------------------------------
// Low overhead OBU framing, which is what an AV1 file on disk actually holds
// ---------------------------------------------------------------------------

bool AV1StreamReader::isLowOverhead(const uint8_t* buf, const uint8_t* end)
{
    // A start code at the very front is the framing tsMuxeR writes itself and the one the
    // container readers hand over, so that case needs no further looking.
    if (end - buf >= 4 && buf[0] == 0 && buf[1] == 0 && (buf[2] == 1 || (buf[2] == 0 && buf[3] == 1)))
        return false;

    // Otherwise walk the buffer as a chain of sized OBUs. A length that is not really a length
    // lands the next header on compressed data, where the forbidden bit, the reserved bit or
    // the type nearly always fails, so a chain of several that all check out is a strong sign.
    int obus = 0;
    const uint8_t* p = buf;
    while (p < end && obus < 8)
    {
        Av1ObuHeader hdr;
        const int hdrLen = hdr.parse(p, end);
        if (hdrLen < 0 || !hdr.obu_has_size_field)
            return false;
        const int type = static_cast<int>(hdr.obu_type);
        if (type == 0 || (type > 8 && type != 15))
            return false;
        int lebLen = 0;
        const int64_t payloadLen = decodeLeb128(p + hdrLen, end, lebLen);
        if (payloadLen < 0)
            return obus >= 2;  // the length itself runs off this buffer
        const uint8_t* next = p + hdrLen + lebLen + payloadLen;
        if (next > end || next < p)
            return obus >= 2;  // a whole OBU chain that simply continues past this buffer
        p = next;
        obus++;
    }
    return obus >= 2;
}

size_t AV1StreamReader::convertPending(const bool lastBlock)
{
    const uint8_t* src = m_pending.data();
    const uint8_t* const end = src + m_pending.size();

    // A start code and an OBU header for every OBU, plus the emulation prevention bytes. The
    // same generous estimate the container path uses, so the buffer can never run short.
    const size_t capacity = MAX_AV_PACKET_SIZE + m_pending.size() * 2 + 64;
    if (m_convBuffer.size() < capacity)
        m_convBuffer.resize(capacity);
    uint8_t* const dstStart = m_convBuffer.data() + MAX_AV_PACKET_SIZE;
    const uint8_t* const dstEnd = m_convBuffer.data() + m_convBuffer.size();
    uint8_t* dst = dstStart;

    bool broken = false;
    while (src < end)
    {
        Av1ObuHeader hdr;
        const int hdrLen = hdr.parse(src, end);
        if (hdrLen < 0 || !hdr.obu_has_size_field)
        {
            broken = true;
            break;
        }
        int lebLen = 0;
        const int64_t payloadLen = decodeLeb128(src + hdrLen, end, lebLen);
        if (payloadLen < 0)
        {
            // Eight bytes present and still no end to the length means it is not a length at
            // all; fewer than that means it is split across the block boundary.
            if (end - (src + hdrLen) >= 8)
                broken = true;
            break;
        }
        const uint8_t* payload = src + hdrLen + lebLen;
        if (payload + payloadLen > end)
            break;  // this OBU is split across blocks, carry it whole to the next one

        // Start code, then the OBU header with obu_has_size_field cleared, then the payload
        // with emulation prevention: byte for byte what matroskaParser and movDemuxer build.
        *dst++ = 0x00;
        *dst++ = 0x00;
        *dst++ = 0x01;
        *dst++ = static_cast<uint8_t>(src[0] & ~0x02);
        if (hdr.obu_extension_flag)
            *dst++ = src[1];

        if (payloadLen > 0)
        {
            const int encoded =
                av1_add_emulation_prevention(payload, payload + payloadLen, dst, static_cast<size_t>(dstEnd - dst));
            if (encoded < 0)
                THROW(ERR_COMMON_SMALL_BUFFER, "Not enough buffer to convert an AV1 OBU of " << payloadLen << " bytes")
            dst += encoded;
        }
        src = payload + payloadLen;
    }

    m_pending.erase(m_pending.begin(), m_pending.begin() + (src - m_pending.data()));

    if (broken)
    {
        // Neither carrying these bytes for ever nor writing them out as if they were video is
        // right, so they are dropped and said out loud, once.
        if (!m_brokenObuWarned)
        {
            LTRACE(LT_WARN, 2,
                   "AV1: the OBU chain breaks after " << m_totalFrameNum << " frames, " << m_pending.size()
                                                      << " bytes skipped");
            m_brokenObuWarned = true;
        }
        m_pending.clear();
    }
    else if (lastBlock && !m_pending.empty())
    {
        LTRACE(LT_WARN, 2, "AV1: the stream ends inside an OBU, " << m_pending.size() << " trailing bytes ignored");
        m_pending.clear();
    }

    return static_cast<size_t>(dst - dstStart);
}

// ** AV1 IN AN IVF FILE. ** An IVF file holds each temporal unit of low overhead OBUs behind 12 bytes of its own, its
// size and its time, after a 32 byte file header. Only VP8 and VP9 were read from IVF, so AV1 in one gave "Can't detect
// stream type". The frames are taken out of their IVF headers here and then read as a low overhead stream. A size no
// frame can have ends the reading there, as for VP8 and VP9.
static size_t takeIvfFrames(const std::vector<uint8_t>& raw, size_t at, std::vector<uint8_t>& out, bool& damaged)
{
    constexpr uint32_t maxIvfFrameSize = 256u * 1024 * 1024;
    while (at + 12 <= raw.size())
    {
        const uint32_t frameSize =
            raw[at] | raw[at + 1] << 8 | raw[at + 2] << 16 | static_cast<uint32_t>(raw[at + 3]) << 24;
        if (frameSize > maxIvfFrameSize)
        {
            damaged = true;
            return at;  // where the damage begins: the caller leaves the rest out
        }
        if (at + 12 + frameSize > raw.size())
            break;
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at + 12),
                   raw.begin() + static_cast<std::ptrdiff_t>(at + 12 + frameSize));
        at += 12 + frameSize;
    }
    return at;
}

// ** AV1 IN THE ANNEX B FORM. ** Annex B of the AV1 specification frames a stream by lengths instead of the size field
// of each OBU: a temporal unit is its size and its frame units, a frame unit its size and its OBUs, and an OBU its
// length (obu_length) and the OBU, usually without a size field. Such a file was refused as "not AV1". Its temporal
// units are taken apart here, each OBU given the size field of the low overhead form, and the result is read as a low
// overhead stream, as the frames of an IVF file are. Sizes that do not add up end the reading there, as for IVF.
static bool readLeb(const std::vector<uint8_t>& raw, const size_t at, const size_t end, uint64_t& value, size_t& len)
{
    int n = 0;
    if (at >= end)
        return false;
    const int64_t v = decodeLeb128(raw.data() + at, raw.data() + end, n);
    if (v < 0 || n <= 0)
        return false;
    value = static_cast<uint64_t>(v);
    len = static_cast<size_t>(n);
    return true;
}

static void putLeb(std::vector<uint8_t>& out, uint64_t value)
{
    do
    {
        uint8_t b = value & 0x7f;
        value >>= 7;
        if (value)
            b |= 0x80;
        out.push_back(b);
    } while (value);
}

static size_t takeAnnexBUnits(const std::vector<uint8_t>& raw, size_t at, std::vector<uint8_t>& out, bool& damaged)
{
    constexpr uint64_t maxUnitSize = 256u * 1024 * 1024;
    while (at < raw.size())
    {
        uint64_t tuSize = 0;
        size_t tuLeb = 0;
        if (!readLeb(raw, at, raw.size(), tuSize, tuLeb))
        {
            damaged = raw.size() - at >= 8;  // more than a leb128 can take, and still no end of it
            return at;
        }
        if (tuSize > maxUnitSize)
        {
            damaged = true;
            return at;
        }
        if (at + tuLeb + tuSize > raw.size())
            break;  // the rest of the unit comes with the next block
        const size_t tuEnd = at + tuLeb + static_cast<size_t>(tuSize);
        std::vector<uint8_t> unit;
        size_t p = at + tuLeb;
        while (p < tuEnd)
        {
            uint64_t fuSize = 0;
            size_t fuLeb = 0;
            if (!readLeb(raw, p, tuEnd, fuSize, fuLeb) || p + fuLeb + fuSize > tuEnd)
            {
                damaged = true;
                return at;
            }
            const size_t fuEnd = p + fuLeb + static_cast<size_t>(fuSize);
            p += fuLeb;
            while (p < fuEnd)
            {
                uint64_t obuLength = 0;
                size_t obuLeb = 0;
                if (!readLeb(raw, p, fuEnd, obuLength, obuLeb) || obuLength == 0 || p + obuLeb + obuLength > fuEnd)
                {
                    damaged = true;
                    return at;
                }
                const size_t obu = p + obuLeb;
                const size_t obuEnd = obu + static_cast<size_t>(obuLength);
                const uint8_t header = raw[obu];
                const size_t headerLen = (header & 0x04) ? 2 : 1;  // obu_extension_flag
                if ((header & 0x80) || obu + headerLen > obuEnd)   // obu_forbidden_bit
                {
                    damaged = true;
                    return at;
                }
                if (header & 0x02)  // obu_has_size_field: already in the low overhead form
                    unit.insert(unit.end(), raw.begin() + static_cast<std::ptrdiff_t>(obu),
                                raw.begin() + static_cast<std::ptrdiff_t>(obuEnd));
                else
                {
                    unit.push_back(header | 0x02);
                    if (headerLen == 2)
                        unit.push_back(raw[obu + 1]);
                    putLeb(unit, obuEnd - obu - headerLen);
                    unit.insert(unit.end(), raw.begin() + static_cast<std::ptrdiff_t>(obu + headerLen),
                                raw.begin() + static_cast<std::ptrdiff_t>(obuEnd));
                }
                p = obuEnd;
            }
        }
        out.insert(out.end(), unit.begin(), unit.end());
        at = tuEnd;
    }
    return at;
}

// Whether the data begins as an Annex B stream: two temporal units in a row (or one that fills the data), each opening
// with a frame unit whose first OBU is a temporal delimiter of the length it has, as a temporal unit begins (7.5)
bool AV1StreamReader::isAnnexB(const uint8_t* buf, const uint8_t* end)
{
    const std::vector<uint8_t> raw(buf, end);
    size_t at = 0;
    for (int units = 0; units < 2; ++units)
    {
        uint64_t tuSize = 0, fuSize = 0, obuLength = 0;
        size_t tuLeb = 0, fuLeb = 0, obuLeb = 0;
        if (!readLeb(raw, at, raw.size(), tuSize, tuLeb) || tuSize < 3)
            return false;
        const size_t fu = at + tuLeb;
        if (!readLeb(raw, fu, raw.size(), fuSize, fuLeb) || fuSize < 2 || fuSize + fuLeb > tuSize)
            return false;
        const size_t obu = fu + fuLeb;
        if (!readLeb(raw, obu, raw.size(), obuLength, obuLeb) || obu + obuLeb >= raw.size())
            return false;
        const uint8_t header = raw[obu + obuLeb];
        const bool td = (header & 0x80) == 0 && ((header >> 3) & 0x0f) == 2;      // OBU_TEMPORAL_DELIMITER
        const uint64_t tdLength = 1 + ((header >> 2) & 1) + ((header >> 1) & 1);  // header, extension, size 0
        if (!td || obuLength != tdLength)
            return false;
        at = fu + static_cast<size_t>(tuSize);
        if (at >= raw.size())
            return true;  // a unit that fills the data or runs past it: checkStream still has to find the AV1 in it
    }
    return true;
}

int64_t AV1StreamReader::getProcessedSize()
{
    if (m_framing != Av1Framing::IVF && m_framing != Av1Framing::ANNEX_B)
        return MPEGStreamReader::getProcessedSize();
    return m_ivfRead - m_ivfLeftOut;
}

void AV1StreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    // Every block arrives behind the front pad the reader protocol prefixes it with.
    const uint8_t* const block = data + MAX_AV_PACKET_SIZE;

    if (m_framing == Av1Framing::UNDECIDED && dataLen > 0)
    {
        double ivfFps = 0;
        if (VpxStreamReader::readIvfHeaderOf(block, static_cast<int>(dataLen), "AV01", ivfFps))
        {
            m_framing = Av1Framing::IVF;
            m_ivfHeaderLeft = block[6] | block[7] << 8;
            if (m_fps == 0.0 && ivfFps > 0)
                setFPS(correctFps(ivfFps));
            LTRACE(LT_INFO, 2, "AV1 in an IVF file, converting OBU framing on read");
        }
        else if (isAnnexB(block, block + dataLen))
        {
            m_framing = Av1Framing::ANNEX_B;
            LTRACE(LT_INFO, 2, "AV1 elementary stream in the Annex B format, converting OBU framing on read");
        }
        else
        {
            m_framing = isLowOverhead(block, block + dataLen) ? Av1Framing::LOW_OVERHEAD : Av1Framing::START_CODES;
            if (m_framing == Av1Framing::LOW_OVERHEAD)
                LTRACE(LT_INFO, 2, "AV1 elementary stream in the low overhead format, converting OBU framing on read");
        }
    }

    if (m_framing == Av1Framing::IVF || m_framing == Av1Framing::ANNEX_B)
    {
        const bool annexB = m_framing == Av1Framing::ANNEX_B;
        m_ivfRead += dataLen;
        if (m_ivfDamaged)
        {
            m_ivfLeftOut += dataLen;
            const size_t converted = convertPending(lastBlock);
            MPEGStreamReader::setBuffer(m_convBuffer.data(), static_cast<uint32_t>(converted), lastBlock);
            return;
        }
        m_ivfRaw.insert(m_ivfRaw.end(), block, block + dataLen);
        size_t at = 0;
        if (m_ivfHeaderLeft > 0)
        {
            at = (std::min)(static_cast<size_t>(m_ivfHeaderLeft), m_ivfRaw.size());
            m_ivfHeaderLeft -= static_cast<int>(at);
        }
        bool damaged = false;
        at = annexB ? takeAnnexBUnits(m_ivfRaw, at, m_pending, damaged)
                    : takeIvfFrames(m_ivfRaw, at, m_pending, damaged);
        if (damaged)
        {
            m_ivfDamaged = true;
            m_ivfLeftOut += static_cast<int64_t>(m_ivfRaw.size() - at);
            at = m_ivfRaw.size();
            if (annexB)
                LTRACE(LT_WARN, 2,
                       "Warning: AV1 Annex B: a temporal unit gives sizes that do not add up, so the file is damaged "
                       "there. The units before it are kept; the rest of the file is left out.");
            else
                LTRACE(LT_WARN, 2,
                       "Warning: IVF: a frame gives a size no frame can have, so the file is damaged there. The frames "
                       "before it are kept; the rest of the file is left out.");
        }
        m_ivfRaw.erase(m_ivfRaw.begin(), m_ivfRaw.begin() + static_cast<std::ptrdiff_t>(at));
        if (lastBlock && !m_ivfRaw.empty())
        {
            LTRACE(
                LT_WARN, 2,
                (annexB ? "AV1 Annex B: the file ends inside a temporal unit, " : "IVF: the file ends inside a frame, ")
                    << m_ivfRaw.size() << " bytes left out");
            m_ivfRaw.clear();
        }
        const size_t converted = convertPending(lastBlock);
        MPEGStreamReader::setBuffer(m_convBuffer.data(), static_cast<uint32_t>(converted), lastBlock);
        return;
    }

    if (m_framing != Av1Framing::LOW_OVERHEAD)
    {
        MPEGStreamReader::setBuffer(data, dataLen, lastBlock);
        return;
    }

    m_pending.insert(m_pending.end(), block, block + dataLen);
    const size_t converted = convertPending(lastBlock);
    MPEGStreamReader::setBuffer(m_convBuffer.data(), static_cast<uint32_t>(converted), lastBlock);
}

// ---------------------------------------------------------------------------
// checkStream - detect AV1 stream from elementary data
// ---------------------------------------------------------------------------

void AV1StreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
}

CheckStreamRez AV1StreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;

    // The probe buffer is the head of the file exactly as it sits on disk, so it may be in the
    // low overhead format that the scan below cannot see. Convert a copy first. Data that is
    // already start code framed, which is what getTSDescriptor passes in, is left untouched.
    std::vector<uint8_t> probe;
    uint8_t* scanBuf = buffer;
    int scanLen = len;
    // An IVF file: its frames, out of their IVF headers, are a low overhead stream
    std::vector<uint8_t> ivfFrames;
    double ivfFps = 0;
    if (len > 0 && VpxStreamReader::readIvfHeaderOf(buffer, len, "AV01", ivfFps))
    {
        const std::vector<uint8_t> raw(buffer + (std::min)(len, buffer[6] | buffer[7] << 8), buffer + len);
        bool damaged = false;
        takeIvfFrames(raw, 0, ivfFrames, damaged);
        if (ivfFrames.empty())
            return rez;
        if (m_fps == 0.0 && ivfFps > 0)
            setFPS(correctFps(ivfFps));
        scanBuf = ivfFrames.data();
        scanLen = static_cast<int>(ivfFrames.size());
    }
    else if (len > 0 && isAnnexB(buffer, buffer + len))
    {
        const std::vector<uint8_t> raw(buffer, buffer + len);
        bool damaged = false;
        takeAnnexBUnits(raw, 0, ivfFrames, damaged);
        if (ivfFrames.empty())
            return rez;
        scanBuf = ivfFrames.data();
        scanLen = static_cast<int>(ivfFrames.size());
    }
    if (scanLen > 0 && (!ivfFrames.empty() || isLowOverhead(scanBuf, scanBuf + scanLen)))
    {
        std::vector<uint8_t> savedPending;
        savedPending.swap(m_pending);
        m_pending.assign(scanBuf, scanBuf + scanLen);
        const size_t converted = convertPending(false);
        probe.assign(m_convBuffer.begin() + MAX_AV_PACKET_SIZE, m_convBuffer.begin() + MAX_AV_PACKET_SIZE + converted);
        m_pending.swap(savedPending);
        if (!probe.empty())
        {
            scanBuf = probe.data();
            scanLen = static_cast<int>(probe.size());
        }
    }
    buffer = scanBuf;
    uint8_t* end = buffer + scanLen;

    // Two-pass detection: first find a valid sequence header, then verify
    // we can also find a frame OBU (FRAME or FRAME_HEADER) to confirm this
    // is genuinely AV1 and not random data from another codec (e.g. MPEG-2
    // slice data whose start codes look like AV1 OBU headers).
    Av1SequenceHeader candidateSeqHdr;
    uint8_t* seqHdrNal = nullptr;
    uint8_t* seqHdrNextNal = nullptr;
    bool foundSeqHdr = false;
    bool foundFrame = false;

    // ** AV1 BEGINS AT THE FRONT, AND EVERY START CODE IN IT IS AN OBU. **
    // The scan used to walk every start code anywhere in the buffer, skipping those that were not followed by an OBU
    // header, and to accept a sequence header and a frame header found anywhere in it. A raw file that every earlier
    // reader had refused, such as AC-3 or DTS with a few bytes between two frames, has start codes inside its audio by
    // chance, so it was listed as AV1 with a picture size read out of audio bytes, and a mux of it said "successful".
    // An AV1 stream framed with start codes, which is what the low overhead file is converted to above and what a
    // container hands over, starts with one, and the emulation prevention bytes keep any other one out of the OBUs:
    // so the data has to begin with a start code, and the first start code that is not followed by an AV1 OBU header
    // ends the probe with no AV1 found.
    m_notAv1 = true;  // until the data is seen to begin as AV1
    int leadingZeros = 0;
    while (leadingZeros < scanLen && leadingZeros <= 16 && buffer[leadingZeros] == 0) leadingZeros++;
    if (leadingZeros < 2 || leadingZeros > 16 || leadingZeros >= scanLen || buffer[leadingZeros] != 1)
        return rez;

    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 2; nal = NALUnit::findNextNAL(nal, end))
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(nal, end);
        if (hdrLen < 0)
            return rez;

        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;

        // A type AV1 does not define
        const int typeInt = static_cast<int>(obuHdr.obu_type);
        if (typeInt <= 0 || (typeInt > 8 && typeInt != 15))
            return rez;

        if (!foundSeqHdr && obuHdr.obu_type == Av1ObuType::SEQUENCE_HEADER)
        {
            // Parse the sequence header (strip OBU header bytes first)
            const uint8_t* payload = nal + hdrLen;
            const int payloadLen = static_cast<int>(nextNal - payload);
            // Trim trailing zeros from next start code
            int trimmedLen = payloadLen;
            while (trimmedLen > 0 && payload[trimmedLen - 1] == 0) trimmedLen--;

            Av1SequenceHeader tmpHdr;
            if (tmpHdr.deserialize(payload, trimmedLen) == 0)
            {
                // Additional sanity: aspect ratio must be reasonable
                const double aspect = static_cast<double>(tmpHdr.max_frame_width) / tmpHdr.max_frame_height;
                if (aspect < 0.1 || aspect > 20.0)
                    continue;

                candidateSeqHdr = tmpHdr;
                seqHdrNal = nal;
                seqHdrNextNal = nextNal;
                foundSeqHdr = true;
            }
        }
        else if (obuHdr.obu_type == Av1ObuType::FRAME || obuHdr.obu_type == Av1ObuType::FRAME_HEADER)
        {
            foundFrame = true;
        }

        // Once we have both a sequence header and a frame, we're confident
        if (foundSeqHdr && foundFrame)
            break;
    }

    m_notAv1 = false;
    if (foundSeqHdr && foundFrame)
    {
        m_seqHdr = candidateSeqHdr;
        m_seqHdrFound = true;
        m_spsPpsFound = true;
        setHdrWcgIdc();  // getTSDescriptor writes it from here, before the first frame is read
        // Lightweight FPS extraction for probing only — the full
        // updateFPS() (with logging and 25fps fallback) runs later
        // during actual muxing in intDecodeNAL().
        {
            const double fps = correctFps(getStreamFPS(nullptr));
            if (fps > 0.0 && m_fps == 0.0)
                setFPS(fps);
        }

        rez.codecInfo = av1CodecInfo;
        ostringstream str;
        str << "Profile: " << static_cast<int>(m_seqHdr.seq_profile);
        str << "  Level: " << static_cast<int>(m_seqHdr.seq_level_idx_0);
        str << "  Resolution: " << m_seqHdr.max_frame_width << "x" << m_seqHdr.max_frame_height;
        str << "  Bit depth: " << m_seqHdr.getBitDepth() << "bit";
        // An AV1 sequence header seldom holds timing info, and the libaom and SVT-AV1 IVF files never had it, so
        // their listing said "Frame rate: not found" although the mux takes the rate from the IVF frame times (set
        // above: 23.976 for a 24000/1001 time base and also for an IVF copied through Matroska, whose time base is
        // 1/1000). The listing now gives that rate when the sequence header has none, as for VP8 and VP9 in IVF. A
        // raw OBU file has no IVF rate and still says "not found".
        str << "  Frame rate: ";
        if (m_seqHdr.getFPS() > 0)
            str << m_seqHdr.getFPS();
        else if (ivfFps > 0)
            str << correctFps(ivfFps);
        else
            str << "not found";
        rez.streamDescr = str.str();
    }

    m_totalFrameNum = m_frameNum = 0;
    m_curDts = m_curPts = 0;

    return rez;
}

// The HDR/WCG indicator of the AV1 video descriptor, from the colour properties of the sequence header. It was set only
// when the mux read the sequence header, after the PMT that carries it was written from the check of the stream, so
// every PMT said 3 (no indication), also for PQ video in BT.2020.
void AV1StreamReader::setHdrWcgIdc()
{
    if (m_seqHdr.transfer_characteristics == AV1_TC_PQ || m_seqHdr.transfer_characteristics == AV1_TC_HLG)
        m_hdrWcgIdc = 2;  // HDR (+ WCG assumed for PQ/HLG)
    else if (m_seqHdr.color_primaries == AV1_CP_BT_2020)
        m_hdrWcgIdc = 1;  // WCG only
    else if (m_seqHdr.color_primaries == AV1_CP_BT_709 || m_seqHdr.color_primaries == AV1_CP_BT_601)
        m_hdrWcgIdc = 0;  // SDR
    else
        m_hdrWcgIdc = 3;  // No indication
}

// ---------------------------------------------------------------------------
// intDecodeNAL - process OBUs between start codes
// ---------------------------------------------------------------------------

std::vector<uint8_t> AV1StreamReader::sequenceHeaderObu() const
{
    std::vector<uint8_t> onDisk;
    if (m_seqHdrBuffer.isEmpty())
        return onDisk;
    std::vector<uint8_t> framed{0, 0, 1};
    framed.insert(framed.end(), m_seqHdrBuffer.data(), m_seqHdrBuffer.data() + m_seqHdrBuffer.size());
    if (av1_start_codes_to_low_overhead(framed.data(), framed.data() + framed.size(), onDisk) <= 0)
        onDisk.clear();
    return onDisk;
}

int AV1StreamReader::intDecodeNAL(uint8_t* buff)
{
    bool frameFound = false;
    int shownLayer = -1;  // the spatial layer of the shown frame of this temporal unit, -1 before one
    m_spsPpsFound = false;
    m_lastIFrame = false;

    const uint8_t* prevPos = nullptr;
    uint8_t* curPos = buff;
    uint8_t* nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

    if (!m_eof && nextNal == m_bufEnd)
        return NOT_ENOUGH_BUFFER;

    while (curPos < m_bufEnd)
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(curPos, m_bufEnd);
        if (hdrLen < 0)
        {
            // Invalid OBU, skip to next
            prevPos = curPos;
            curPos = nextNal;
            nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);
            if (!m_eof && nextNal == m_bufEnd)
                return NOT_ENOUGH_BUFFER;
            continue;
        }

        // Calculate payload (between OBU header and next start code)
        uint8_t* nextNalWithStartCode = nextNal;
        // Back up past trailing zeros that are part of the next start code
        while (nextNalWithStartCode > curPos + hdrLen && nextNalWithStartCode[-1] == 0) nextNalWithStartCode--;

        const uint8_t* payload = curPos + hdrLen;
        const int payloadLen = static_cast<int>(nextNalWithStartCode - payload);

        // ** A STREAM WITHOUT TEMPORAL DELIMITERS. ** A temporal unit must begin with one (AV1 specification, 7.5),
        // and a unit ended only at the next one, so a stream that has none became one frame of the whole file, at
        // exit 0 and with nothing said. A temporal unit holds the frames of one time: the frames not shown, then the
        // one shown, with its tile groups. In such a stream a unit now ends after its shown frame, where the next
        // sequence header, metadata or frame of the same or a lower spatial layer begins.
        if (m_noTd == 1 && shownLayer >= 0 &&
            (obuHdr.obu_type == Av1ObuType::SEQUENCE_HEADER || obuHdr.obu_type == Av1ObuType::METADATA ||
             ((obuHdr.obu_type == Av1ObuType::FRAME || obuHdr.obu_type == Av1ObuType::FRAME_HEADER) &&
              obuHdr.spatial_id <= shownLayer)))
        {
            m_lastDecodedPos = prevPos;
            incTimings();
            return 0;
        }

        switch (obuHdr.obu_type)
        {
        case Av1ObuType::SEQUENCE_HEADER:
            if (payloadLen > 0 && m_seqHdr.deserialize(payload, payloadLen) == 0)
            {
                m_seqHdrFound = true;
                m_spsPpsFound = true;
                updateFPS(nullptr, curPos, nextNalWithStartCode, 0);

                // Store the sequence header OBU for writeAdditionData.
                // Find the next start code boundary and strip trailing zeros to
                // avoid including the next OBU's start code prefix.
                {
                    uint8_t* scPos = NALUnit::findNALWithStartCode(curPos + 1, m_bufEnd, true);
                    uint8_t* obuEnd = scPos;
                    while (obuEnd > curPos + hdrLen && obuEnd[-1] == 0) obuEnd--;
                    const int dataLen = static_cast<int>(obuEnd - curPos);
                    if (dataLen > 0)
                    {
                        m_seqHdrBuffer.resize(dataLen);
                        memcpy(m_seqHdrBuffer.data(), curPos, dataLen);
                    }
                }

                setHdrWcgIdc();
            }
            break;

        case Av1ObuType::TEMPORAL_DELIMITER:
            // TD marks the start of a new temporal unit.  We split only at TD
            // boundaries (not at FRAME OBU boundaries) to preserve the source's
            // frame bundling — some MKV blocks contain multiple FRAME OBUs that
            // must stay together as one temporal unit.
            m_tdSeen = true;
            if (frameFound)
            {
                m_lastDecodedPos = prevPos;
                incTimings();
                return 0;
            }
            break;

        case Av1ObuType::FRAME:
        case Av1ObuType::FRAME_HEADER:
        {
            if (m_noTd == -1)
            {
                m_noTd = m_tdSeen ? 0 : 1;
                if (m_noTd == 1)
                    LTRACE(LT_WARN, 2,
                           "Warning: AV1: the stream has no temporal delimiters, which each temporal unit must begin "
                           "with. Its temporal units are taken from its frame headers, and each is written with a "
                           "temporal delimiter.");
            }
            // Parse frame header for keyframe detection (no boundary split here).
            if (m_seqHdrFound && payloadLen > 0)
            {
                Av1FrameHeader frmHdr;
                if (frmHdr.deserialize(payload, payloadLen, m_seqHdr) == 0)
                {
                    if (frmHdr.show_frame || frmHdr.show_existing_frame)
                        shownLayer = (std::max)(shownLayer, static_cast<int>(obuHdr.spatial_id));
                    // Exclude show_existing_frame: it doesn't signal frame_type in the
                    // bitstream (deserializer defaults to KEY_FRAME), so including it
                    // would falsely mark bundled blocks as keyframes.
                    if (!frmHdr.show_existing_frame && (frmHdr.frame_type == Av1FrameType::KEY_FRAME ||
                                                        frmHdr.frame_type == Av1FrameType::INTRA_ONLY_FRAME))
                    {
                        m_lastIFrame = true;
                    }
                }
            }
            if (!frameFound)
                frameFound = true;
            break;
        }

        case Av1ObuType::TILE_GROUP:
            // Tile groups belong to the current frame
            break;

        default:
            break;
        }

        prevPos = curPos;
        curPos = nextNal;
        nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

        if (!m_eof && nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
    }

    if (m_eof)
    {
        if (frameFound)
            incTimings();
        m_lastDecodedPos = m_bufEnd;
        return 0;
    }
    return NEED_MORE_DATA;
}

// ---------------------------------------------------------------------------
// getTSDescriptor - AV1 registration descriptor + AV1 video descriptor
// ---------------------------------------------------------------------------

int AV1StreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, const bool hdmvDescriptors)
{
    if (m_firstFrame)
    {
        checkStream(m_buffer, static_cast<int>(m_bufEnd - m_buffer));
        // A line that names V_AV1 for data that is not AV1 was muxed all the same: start codes found by chance in audio
        // became "pictures", and the job said "Mux successful complete" over a stream nothing can play
        if (m_notAv1)
            THROW(ERR_COMMON,
                  "Track " << m_streamIndex
                           << " is not AV1: its data does not begin with an AV1 OBU, as an AV1 stream does. "
                              "The usual reason is that the codec on the meta line is not the codec the "
                              "file holds. List the file on its own to see what tsMuxeR makes of it.")
    }

    uint8_t* start = dstBuff;

    // 1. Registration descriptor (AOM spec Section 2.1)
    // descriptor_tag = 0x05, descriptor_length = 4, format_identifier = 'AV01'
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // 0x05
    *dstBuff++ = 4;                                                    // descriptor length
    *dstBuff++ = 'A';
    *dstBuff++ = 'V';
    *dstBuff++ = '0';
    *dstBuff++ = '1';

    // 2. AV1 video descriptor (AOM spec Section 2.2)
    // descriptor_tag = 0x80, descriptor_length = 4
    *dstBuff++ = 0x80;  // AV1 video descriptor tag
    *dstBuff++ = 4;     // descriptor length

    // Byte 0: marker(1) + version(7)
    *dstBuff++ = 0x81;  // marker=1, version=1

    // Byte 1: seq_profile(3) + seq_level_idx_0(5)
    *dstBuff++ = static_cast<uint8_t>((m_seqHdr.seq_profile << 5) | (m_seqHdr.seq_level_idx_0 & 0x1f));

    // Byte 2: seq_tier_0(1) + high_bitdepth(1) + twelve_bit(1) + monochrome(1) +
    //          chroma_subsampling_x(1) + chroma_subsampling_y(1) + chroma_sample_position(2)
    *dstBuff++ =
        static_cast<uint8_t>((m_seqHdr.seq_tier_0 << 7) | (m_seqHdr.high_bitdepth << 6) | (m_seqHdr.twelve_bit << 5) |
                             (m_seqHdr.mono_chrome << 4) | (m_seqHdr.chroma_subsampling_x << 3) |
                             (m_seqHdr.chroma_subsampling_y << 2) | (m_seqHdr.chroma_sample_position & 0x03));

    // Byte 3: hdr_wcg_idc(2) + reserved(1) + initial_presentation_delay_present(1) + reserved(4)
    *dstBuff++ = static_cast<uint8_t>((m_hdrWcgIdc & 0x03) << 6);

    return static_cast<int>(dstBuff - start);
}

// ---------------------------------------------------------------------------
// writeAdditionData - insert Sequence Header OBU before first frame
// ---------------------------------------------------------------------------

uint8_t* AV1StreamReader::writeNalPrefix(uint8_t* curPos) const
{
    if (!m_shortStartCodes)
        *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

uint8_t* AV1StreamReader::writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const
{
    if (srcData.isEmpty())
        return dstBuffer;
    const size_t bytesLeft = dstEnd - dstBuffer;
    const size_t requiredBytes = srcData.size() + 3 + (m_shortStartCodes ? 0 : 1);
    if (bytesLeft < requiredBytes)
        return dstBuffer;

    dstBuffer = writeNalPrefix(dstBuffer);
    memcpy(dstBuffer, srcData.data(), srcData.size());
    dstBuffer += srcData.size();
    return dstBuffer;
}

int AV1StreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                       PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;

    // A stream without temporal delimiters gets one in front of each temporal unit, as the format wants: a start code
    // and the OBU header of an empty temporal delimiter, or for a demux, which writes this as it is, the delimiter
    // as a file holds it, with its size field (0)
    if (m_noTd == 1 && dstEnd - curPos >= 5)
    {
        const auto tdHeader = static_cast<uint8_t>(static_cast<int>(Av1ObuType::TEMPORAL_DELIMITER) << 3);
        if (m_demuxMode)
        {
            *curPos++ = tdHeader | 0x02;  // obu_has_size_field
            *curPos++ = 0;
        }
        else
        {
            curPos = writeNalPrefix(curPos);
            *curPos++ = tdHeader;
        }
    }

    const bool needInsSeqHdr = m_firstFileFrame && !(avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP);
    if (needInsSeqHdr)
    {
        avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;
        // A demux writes a file, and an AV1 file is a chain of temporal units each of which BEGINS
        // with a temporal delimiter. Nothing may sit in front of the first one: writing the
        // sequence header here put an OBU there, and a reader refused the whole file for it. So in
        // demux mode the header is owed rather than written, and prepareDemuxPacket puts it just
        // after the temporal delimiter the packet starts with.
        if (m_demuxMode)
            m_demuxSeqHdrPending = true;
        else
            curPos = writeBuffer(m_seqHdrBuffer, curPos, dstEnd);
    }

    m_firstFileFrame = false;
    return static_cast<int>(curPos - dstBuffer);
}

// Step over one low overhead OBU. Returns false when there is not a whole one at `at`.
static bool obuAt(const std::vector<uint8_t>& buf, const size_t at, Av1ObuType& type, size_t& next)
{
    const uint8_t* const begin = buf.data() + at;
    const uint8_t* const end = buf.data() + buf.size();
    Av1ObuHeader hdr;
    const int hdrLen = hdr.parse(begin, end);
    if (hdrLen < 0 || !hdr.obu_has_size_field)
        return false;
    int lebLen = 0;
    const int64_t payloadLen = decodeLeb128(begin + hdrLen, end, lebLen);
    if (payloadLen < 0)
        return false;
    const size_t n = at + hdrLen + lebLen + static_cast<size_t>(payloadLen);
    if (n > buf.size())
        return false;
    type = hdr.obu_type;
    next = n;
    return true;
}

int AV1StreamReader::prepareDemuxPacket(uint8_t* data, const int size, const uint8_t** outData)
{
    // The internal framing is start codes, the way H.264 and HEVC are framed. An .obu file has none
    // of that: every OBU carries its own leb128 length instead. Without this a demux wrote a file
    // no other program will open, at exit 0 and with nothing said.
    m_demuxBuffer.clear();
    const int converted = av1_start_codes_to_low_overhead(data, data + size, m_demuxBuffer);
    if (converted <= 0 || m_demuxBuffer.empty())
    {
        *outData = data;
        return size;
    }
    // A stream without temporal delimiters: writeAdditionData has written one in front of each temporal unit, so
    // nothing is held back for one
    if (m_noTd == 1)
        m_demuxSawTd = true;

    // ** AN AV1 FILE BEGINS WITH A TEMPORAL DELIMITER AND NOTHING MAY STAND IN FRONT OF IT. **
    // A transport stream carries a sequence header ahead of the first delimiter, so writing the
    // packets out in the order they arrive put that header at the head of the file, which the
    // format does not allow, and a reader refused the whole thing. Anything ahead of the first delimiter is
    // held back and written immediately after it instead: nothing is dropped, and the file starts
    // the way the format says it must.
    if (!m_demuxSawTd)
    {
        size_t at = 0;
        size_t tdStart = m_demuxBuffer.size();
        size_t tdEnd = m_demuxBuffer.size();
        while (at < m_demuxBuffer.size())
        {
            Av1ObuType type;
            size_t next;
            if (!obuAt(m_demuxBuffer, at, type, next))
                break;
            if (type == Av1ObuType::TEMPORAL_DELIMITER)
            {
                tdStart = at;
                tdEnd = next;
                break;
            }
            at = next;
        }
        if (tdStart == m_demuxBuffer.size())
        {
            // Still no delimiter. Keep these and write nothing yet.
            m_demuxHeld.insert(m_demuxHeld.end(), m_demuxBuffer.begin(), m_demuxBuffer.end());
            *outData = data;
            return 0;
        }
        std::vector<uint8_t> ordered;
        ordered.reserve(m_demuxBuffer.size() + m_demuxHeld.size());
        ordered.insert(ordered.end(), m_demuxBuffer.begin() + tdStart, m_demuxBuffer.begin() + tdEnd);
        ordered.insert(ordered.end(), m_demuxHeld.begin(), m_demuxHeld.end());
        ordered.insert(ordered.end(), m_demuxBuffer.begin(), m_demuxBuffer.begin() + tdStart);
        ordered.insert(ordered.end(), m_demuxBuffer.begin() + tdEnd, m_demuxBuffer.end());
        m_demuxBuffer.swap(ordered);
        m_demuxHeld.clear();
        m_demuxHeld.shrink_to_fit();
        m_demuxSawTd = true;
    }

    // The sequence header a split part is owed goes INSIDE the first temporal unit, never in front
    // of it. It is only ever a copy of one the stream already carried, so this changes nothing for
    // a whole file; it is what makes each part of a split demux decodable on its own.
    if (m_demuxSeqHdrPending)
    {
        m_demuxSeqHdrPending = false;
        if (!m_seqHdrBuffer.isEmpty())
        {
            std::vector<uint8_t> framed{0, 0, 1};
            framed.insert(framed.end(), m_seqHdrBuffer.data(), m_seqHdrBuffer.data() + m_seqHdrBuffer.size());
            std::vector<uint8_t> onDisk;
            if (av1_start_codes_to_low_overhead(framed.data(), framed.data() + framed.size(), onDisk) > 0)
            {
                // Past the leading temporal delimiter, if there is one.
                size_t at = 0;
                Av1ObuHeader hdr;
                const uint8_t* const begin = m_demuxBuffer.data();
                const uint8_t* const end = begin + m_demuxBuffer.size();
                const int hdrLen = hdr.parse(begin, end);
                if (hdrLen > 0 && hdr.obu_has_size_field && hdr.obu_type == Av1ObuType::TEMPORAL_DELIMITER)
                {
                    int lebLen = 0;
                    const int64_t payloadLen = decodeLeb128(begin + hdrLen, end, lebLen);
                    if (payloadLen >= 0)
                        at = static_cast<size_t>(hdrLen + lebLen + payloadLen);
                }
                m_demuxBuffer.insert(m_demuxBuffer.begin() + at, onDisk.begin(), onDisk.end());
            }
        }
    }

    *outData = m_demuxBuffer.data();
    return static_cast<int>(m_demuxBuffer.size());
}
