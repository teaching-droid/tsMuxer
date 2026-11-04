// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "mkvCopyStreamReader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "matroskaParser.h"
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

// An EBML number: the length from the first byte; with the marker bit kept for an element ID, cleared for a size
bool readVint(const uint8_t*& p, const uint8_t* end, uint64_t& value, const bool keepMarker)
{
    if (p >= end || *p == 0)
        return false;
    int len = 1;
    while (!(*p & (0x80 >> (len - 1)))) ++len;
    if (end - p < len)
        return false;
    value = keepMarker ? *p : *p & (0xFF >> len);
    for (int i = 1; i < len; ++i) value = value << 8 | p[i];
    p += len;
    return true;
}

// The BlockAdditional of BlockAddID 1 (the ID an absent BlockAddID stands for) in a BlockAdditions body
std::string blockAdditional1(const uint8_t* p, const uint8_t* end)
{
    while (p < end)
    {
        uint64_t id = 0, size = 0;
        if (!readVint(p, end, id, true) || !readVint(p, end, size, false) || size > static_cast<uint64_t>(end - p))
            return {};
        const uint8_t* body = p;
        p += size;
        if (id != 0xA6)  // BlockMore
            continue;
        uint64_t addId = 1;
        std::string data;
        for (const uint8_t* q = body; q < body + size;)
        {
            uint64_t cid = 0, csize = 0;
            if (!readVint(q, body + size, cid, true) || !readVint(q, body + size, csize, false) ||
                csize > static_cast<uint64_t>(body + size - q))
                break;
            if (cid == 0xEE)  // BlockAddID
                addId = readBe(q, static_cast<int>((std::min)(csize, uint64_t{8})));
            else if (cid == 0xA5)  // BlockAdditional
                data.assign(reinterpret_cast<const char*>(q), static_cast<size_t>(csize));
            q += csize;
        }
        if (addId == 1)
            return data;
    }
    return {};
}

// h:mm:ss.cc (ASS and SSA) or hh:mm:ss.ttt (WebVTT), from a time in INTERNAL_PTS_FREQ units
std::string clockText(int64_t t, const bool vtt)
{
    if (t < 0)
        t = 0;
    const int64_t unit = vtt ? 1000 : 100;
    const int64_t n = (t * unit + INTERNAL_PTS_FREQ / 2) / INTERNAL_PTS_FREQ;
    const int64_t frac = n % unit, s = n / unit % 60, m = n / unit / 60 % 60, h = n / unit / 3600;
    std::ostringstream out;
    out << std::setfill('0');
    if (vtt)
        out << std::setw(2) << h;
    else
        out << h;
    out << ':' << std::setw(2) << m << ':' << std::setw(2) << s << '.' << std::setw(vtt ? 3 : 2) << frac;
    return out.str();
}

// The timestamps inside a WebVTT cue (<00:00:01.500>), which Matroska keeps relative to the block, made absolute again
std::string absoluteCueTimes(const std::string& text, const int64_t start)
{
    std::string out;
    size_t i = 0;
    while (i < text.size())
    {
        const size_t lt = text.find('<', i);
        if (lt == std::string::npos)
            break;
        const size_t gt = text.find('>', lt);
        if (gt == std::string::npos)
            break;
        const std::string inner = text.substr(lt + 1, gt - lt - 1);
        int parts[4] = {0, 0, 0, 0};
        int n = 0;
        bool ok = !inner.empty() && isdigit(static_cast<unsigned char>(inner[0]));
        size_t dot = inner.rfind('.');
        if (ok && dot != std::string::npos && inner.size() - dot == 4)
        {
            std::string hms = inner.substr(0, dot);
            size_t pos = 0;
            while (ok && pos <= hms.size() && n < 3)
            {
                const size_t colon = hms.find(':', pos);
                const std::string part = hms.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
                ok = !part.empty() && part.find_first_not_of("0123456789") == std::string::npos;
                if (ok)
                    parts[n++] = atoi(part.c_str());
                if (colon == std::string::npos)
                    break;
                pos = colon + 1;
            }
            ok = ok && (n == 2 || n == 3) && inner.substr(dot + 1).find_first_not_of("0123456789") == std::string::npos;
        }
        else
            ok = false;
        out.append(text, i, lt - i);
        if (ok)
        {
            const int64_t h = n == 3 ? parts[0] : 0, m = n == 3 ? parts[1] : parts[0], s = n == 3 ? parts[2] : parts[1];
            const int64_t ms = ((h * 60 + m) * 60 + s) * 1000 + atoi(inner.substr(dot + 1).c_str());
            out += '<' + clockText(start + ms * (INTERNAL_PTS_FREQ / 1000), true) + '>';
        }
        else
            out.append(text, lt, gt - lt + 1);
        i = gt + 1;
    }
    out.append(text, (std::min)(i, text.size()), std::string::npos);
    return out;
}
}  // namespace

// The text of the subtitle file for one block, its header in front of the first. ASS and SSA: the CodecPrivate (the
// [Script Info] and styles sections), an [Events] section if it has none, and a Dialogue line for each block, from
// the block's fields (ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text) and times. WebVTT: the
// CodecPrivate (the file up to its first cue), and for each block its comments, its identifier, its times with its
// settings, and its text; S_TEXT/WEBVTT keeps the first three in BlockAdditional 1, a line each for the settings and
// the identifier, then the comments; WebM's D_WEBVTT/SUBTITLES has the identifier and the settings as the first two
// lines of the block. The lines come in the order of the blocks, which is the order of their start times.
std::string MatroskaCopyStreamReader::textFor(const int64_t pts, const int64_t duration, const uint8_t* add,
                                              const size_t addSize, const uint8_t* data, const size_t dataSize)
{
    const bool vtt = m_sourceCodecId == "S_TEXT/WEBVTT" || isWebmVttText(m_sourceCodecId);
    const bool ssa = m_sourceCodecId == "S_TEXT/SSA";
    std::string head(m_codecPrivate.begin(), m_codecPrivate.end());
    while (!head.empty() && head.back() == '\0') head.pop_back();
    const std::string nl = head.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out;
    if (!m_headerWritten)
    {
        m_headerWritten = true;
        if (vtt)
        {
            while (!head.empty() && (head.back() == '\n' || head.back() == '\r' || head.back() == ' ')) head.pop_back();
            out = (head.empty() ? std::string("WEBVTT") : head) + nl + nl;
        }
        else
        {
            out = head;
            if (!out.empty() && out.back() != '\n')
                out += nl;
            if (out.find("[Events]") == std::string::npos)
                out += nl + "[Events]" + nl +
                       (ssa ? "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"
                            : "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text") +
                       nl;
        }
    }
    const int64_t end = pts + (duration > 0 ? duration : 0);
    std::string body(reinterpret_cast<const char*>(data), dataSize);
    if (vtt)
    {
        std::string settings, identifier, comments;
        if (isWebmVttText(m_sourceCodecId))
        {
            for (std::string* line : {&identifier, &settings})
            {
                const size_t lf = body.find('\n');
                *line = body.substr(0, lf);
                body = lf == std::string::npos ? std::string() : body.substr(lf + 1);
            }
        }
        else if (addSize > 0)
        {
            const std::string extra = blockAdditional1(add, add + addSize);
            const size_t a = extra.find('\n');
            settings = extra.substr(0, a);
            if (a != std::string::npos)
            {
                const size_t b = extra.find('\n', a + 1);
                identifier = extra.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
                if (b != std::string::npos)
                    comments = extra.substr(b + 1);
            }
        }
        for (std::string* s : {&settings, &identifier})
            while (!s->empty() && (s->back() == '\r' || s->back() == '\n')) s->pop_back();
        while (!comments.empty() && (comments.back() == '\r' || comments.back() == '\n')) comments.pop_back();
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) body.pop_back();
        if (!comments.empty())
            out += comments + nl + nl;
        if (!identifier.empty())
            out += identifier + nl;
        out += clockText(pts, true) + " --> " + clockText(end, true) + (settings.empty() ? "" : " " + settings) + nl;
        out += absoluteCueTimes(body, pts) + nl + nl;
        return out;
    }
    // ReadOrder, Layer, then the fields a Dialogue line has after its times
    const size_t c1 = body.find(',');
    const size_t c2 = c1 == std::string::npos ? std::string::npos : body.find(',', c1 + 1);
    std::string layer = c1 == std::string::npos ? std::string() : body.substr(c1 + 1, c2 - c1 - 1);
    const std::string rest = c2 == std::string::npos ? body : body.substr(c2 + 1);
    if (ssa)
        layer = "Marked=0";
    else if (layer.empty())
        layer = "0";
    std::string text = rest;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    out += "Dialogue: " + layer + "," + clockText(pts, false) + "," + clockText(end, false) + "," + text + nl;
    return out;
}

MatroskaCopyStreamReader::MatroskaCopyStreamReader(const std::string& codecId, std::vector<uint8_t> codecPrivate,
                                                   std::vector<std::vector<uint8_t>> addMappings,
                                                   const uint64_t maxBlockAddId)
    : m_codecInfo(CODEC_S_MKVCOPY, "Matroska " + codecId, "S_MKVCOPY"),
      m_sourceCodecId(codecId),
      m_codecPrivate(std::move(codecPrivate)),
      m_addMappings(std::move(addMappings)),
      m_maxBlockAddId(maxBlockAddId)
{
}

void MatroskaCopyStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, bool /*lastBlock*/)
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

int MatroskaCopyStreamReader::readPacket(AVPacket& avPacket)
{
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_MKVCOPY;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;

    static constexpr size_t HEAD = 4 + 8 + 8 + 1 + 4;
    const size_t left = m_input.size() - m_inputPos;
    if (left < HEAD)
        return NEED_MORE_DATA;
    const uint8_t* p = m_input.data() + m_inputPos;
    if (memcmp(p, "MKCP", 4) != 0)
        THROW(ERR_COMMON,
              "A Matroska track copied as it is lost its framing at byte " << m_processedSize << " of its data.")
    const auto pts = static_cast<int64_t>(readBe(p + 4, 8));
    const auto duration = static_cast<int64_t>(readBe(p + 12, 8));
    const bool keyFrame = p[20] != 0;
    const auto addSize = static_cast<size_t>(readBe(p + 21, 4));
    if (left < HEAD + addSize + 4)
        return NEED_MORE_DATA;
    const auto dataSize = static_cast<size_t>(readBe(p + HEAD + addSize, 4));
    const size_t record = HEAD + addSize + 4 + dataSize;
    if (left < record)
        return NEED_MORE_DATA;

    if (m_vobSubFile)
        packVobSub(pts, p + HEAD + addSize + 4, dataSize);
    else if (m_textFile)
    {
        const std::string text = textFor(pts, duration, p + HEAD, addSize, p + HEAD + addSize + 4, dataSize);
        m_packet.assign(text.begin(), text.end());
    }
    else
    {
        m_packet.resize(4 + addSize + dataSize);
        for (int i = 0; i < 4; ++i) m_packet[i] = static_cast<uint8_t>(addSize >> (8 * (3 - i)));
        if (addSize)
            memcpy(m_packet.data() + 4, p + HEAD, addSize);
        if (dataSize)
            memcpy(m_packet.data() + 4 + addSize, p + HEAD + addSize + 4, dataSize);
    }

    avPacket.pts = avPacket.dts = pts;
    avPacket.duration = duration > 0 ? duration : 0;
    avPacket.flags = keyFrame ? AVPacket::IS_IFRAME : 0;
    avPacket.data = m_packet.data();
    avPacket.size = static_cast<int>(m_packet.size());
    m_inputPos += record;
    m_processedSize += static_cast<int64_t>(record);
    ++m_frames;
    return 0;
}

// A .sub file is a program stream: packs of 2048 bytes, a unit from the start of a pack on, in private stream 1 under
// sub-stream 0x20, the time on its first pack. What a pack has left over goes into a padding stream.
void MatroskaCopyStreamReader::packVobSub(const int64_t pts, const uint8_t* data, const size_t size)
{
    constexpr size_t PACK = 2048;
    constexpr size_t PACK_HEADER = 14;
    const int64_t ticks = pts / (INTERNAL_PTS_FREQ / 90000);
    const int64_t ms = (pts + INTERNAL_PTS_FREQ / 2000) / (INTERNAL_PTS_FREQ / 1000);
    char line[64];
    snprintf(line, sizeof line, "timestamp: %02d:%02d:%02d:%03d, filepos: %09llx\n", static_cast<int>(ms / 3600000),
             static_cast<int>(ms / 60000 % 60), static_cast<int>(ms / 1000 % 60), static_cast<int>(ms % 1000),
             static_cast<unsigned long long>(m_vobSubPos));
    m_vobSubTimes += line;

    m_packet.clear();
    size_t done = 0;
    bool first = true;
    while (done < size || first)
    {
        const size_t at = m_packet.size();
        m_packet.resize(at + PACK, 0xFF);
        uint8_t* p = m_packet.data() + at;
        // the system clock of the pack is the time of the unit
        const auto scr = static_cast<uint64_t>(ticks);
        p[0] = 0;
        p[1] = 0;
        p[2] = 1;
        p[3] = 0xBA;
        p[4] = static_cast<uint8_t>(0x44 | (scr >> 27 & 0x38) | (scr >> 28 & 0x03));
        p[5] = static_cast<uint8_t>(scr >> 20);
        p[6] = static_cast<uint8_t>((scr >> 12 & 0xF8) | 0x04 | (scr >> 13 & 0x03));
        p[7] = static_cast<uint8_t>(scr >> 5);
        p[8] = static_cast<uint8_t>((scr << 3 & 0xF8) | 0x04);
        p[9] = 0x01;
        p[10] = 0x01;
        p[11] = 0x89;
        p[12] = 0xC3;
        p[13] = 0xF8;
        uint8_t* pes = p + PACK_HEADER;
        const size_t timeBytes = first ? 5 : 0;
        // start code and length (6), flags and header length (3), the time, the sub-stream
        const size_t overhead = 6 + 3 + timeBytes + 1;
        const size_t room = PACK - PACK_HEADER - overhead;
        const size_t take = (std::min)(room, size - done);
        size_t pad = room - take;
        // too little for a padding stream: stuffing bytes in the header of the PES
        const size_t stuffing = pad < 6 ? pad : 0;
        pad -= stuffing;
        const size_t pesLength = 3 + timeBytes + stuffing + 1 + take;
        pes[0] = 0;
        pes[1] = 0;
        pes[2] = 1;
        pes[3] = 0xBD;
        pes[4] = static_cast<uint8_t>(pesLength >> 8);
        pes[5] = static_cast<uint8_t>(pesLength);
        pes[6] = 0x81;
        pes[7] = first ? 0x80 : 0;
        pes[8] = static_cast<uint8_t>(timeBytes + stuffing);
        uint8_t* q = pes + 9;
        if (first)
        {
            const auto t = static_cast<uint64_t>(ticks);
            q[0] = static_cast<uint8_t>(0x21 | (t >> 29 & 0x0E));
            q[1] = static_cast<uint8_t>(t >> 22);
            q[2] = static_cast<uint8_t>((t >> 14 & 0xFE) | 1);
            q[3] = static_cast<uint8_t>(t >> 7);
            q[4] = static_cast<uint8_t>((t << 1 & 0xFE) | 1);
            q += 5;
        }
        q += stuffing;  // 0xFF already
        *q++ = 0x20;
        if (take > 0)
            memcpy(q, data + done, take);
        q += take;
        if (pad > 0)
        {
            q[0] = 0;
            q[1] = 0;
            q[2] = 1;
            q[3] = 0xBE;
            q[4] = static_cast<uint8_t>((pad - 6) >> 8);
            q[5] = static_cast<uint8_t>(pad - 6);
        }
        done += take;
        first = false;
    }
    m_vobSubPos += static_cast<int64_t>(m_packet.size());
}

std::string MatroskaCopyStreamReader::vobSubIndex() const
{
    std::string rez = "# VobSub index file, v7 (do not modify this line!)\n";
    rez.append(reinterpret_cast<const char*>(m_codecPrivate.data()), m_codecPrivate.size());
    if (!rez.empty() && rez.back() != '\n')
        rez += '\n';
    return rez + "langidx: 0\n\nid: " + (m_vobSubLang.empty() ? "und" : m_vobSubLang) + ", index: 0\n" + m_vobSubTimes;
}

int MatroskaCopyStreamReader::flushPacket(AVPacket& avPacket)
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
