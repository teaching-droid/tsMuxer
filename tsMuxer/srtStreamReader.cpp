#include "srtStreamReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "convertUTF.h"
#include "fs/systemlog.h"
#include "matroskaParser.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

using namespace text_subtitles;

SRTStreamReader::SRTStreamReader() : m_lastBlock(false), m_short_R(0), m_short_N(0), m_long_R(0), m_long_N(0)
{
    // in future version here must be case for destination subtitle format (DVB sub, DVD sub e.t.c)
    auto* pgsRender = new PGSStreamReader();
    pgsRender->setRenderedFromText(true);
    m_dstSubCodec = pgsRender;
    m_srtRender = new TextToPGSConverter(true);
    m_processedSize = 0;
    m_state = ParseState::PARSE_FIRST_LINE;
    m_inTime = m_outTime = 0.0;
    m_srcFormat = UtfConverter::SourceFormat::sfUnknown;
    m_charSize = 1;
    m_splitterOfs = 0;
    m_linesRead = 0;
}

static std::string srtTime(int64_t ms);

SRTStreamReader::~SRTStreamReader()
{
    // save-srt: the last subtitle without an end is given an hour, as a demux of the track gives it
    if (!m_saveSrtName.empty())
    {
        try
        {
            for (const auto& [text, times] : m_saveSrtPending)
                saveSrtWrite(text, times.first, times.second < 0 ? times.first + 3600000 : times.second);
        }
        catch (...)
        {
        }
        if (m_saveSrt)
        {
            m_saveSrt->close();
            LTRACE(LT_INFO, 2,
                   "The subtitles of track " << m_streamIndex << " were also written as text to " << m_saveSrtName
                                             << " (" << m_saveSrtCount << " subtitles).");
        }
        else
            LTRACE(LT_INFO, 2, "No subtitle of track " << m_streamIndex << " was written to " << m_saveSrtName << ".");
    }
    delete m_dstSubCodec;
    delete m_srtRender;
}

// The file is made with its first subtitle. Made at once, a track that was refused afterwards left an empty .srt, and
// the folder made for it, behind.
void SRTStreamReader::setSaveSrt(const std::string& fileName) { m_saveSrtName = fileName; }

// One subtitle of save-srt as an SRT record, written the way a demux of the track writes it
void SRTStreamReader::saveSrtWrite(const std::string& text, const int64_t inMs, const int64_t outMs)
{
    if (outMs <= inMs)
        return;  // never on screen
    std::string rec;
    if (m_saveSrtCount == 0)
        rec = "\xEF\xBB\xBF";
    rec += std::to_string(++m_saveSrtCount) + "\r\n" + srtTime(inMs) + " --> " + srtTime(outMs) + "\r\n";
    for (const char c : text)
        if (c == 10)
            rec += "\r\n";
        else
            rec += c;
    rec += "\r\n\r\n";
    if (!m_saveSrt)
    {
        m_saveSrt = std::make_unique<File>();
        if (!m_saveSrt->open(m_saveSrtName.c_str(), File::ofWrite))
        {
            m_saveSrt.reset();
            THROW(ERR_COMMON, "save-srt: the file " << m_saveSrtName << " cannot be written.")
        }
    }
    m_saveSrt->write(rec.data(), static_cast<uint32_t>(rec.size()));
}

// A subtitle for save-srt: one without an end waits for the next one, whose start is its end
void SRTStreamReader::saveSrtCue(const std::string& text, const double inTime, const double outTime,
                                 const bool untilNext)
{
    const int64_t inMs = llround(inTime * 1000.0);
    while (!m_saveSrtPending.empty())
    {
        const auto& [ptext, times] = m_saveSrtPending.front();
        saveSrtWrite(ptext, times.first, times.second < 0 ? inMs : times.second);
        m_saveSrtPending.pop_front();
    }
    if (text.empty())
        return;
    const int64_t outMs = untilNext ? -1 : llround(outTime * 1000.0);
    if (untilNext)
        m_saveSrtPending.emplace_back(text, std::make_pair(inMs, outMs));
    else
        saveSrtWrite(text, inMs, outMs);
}

void SRTStreamReader::setAnimation(const TextAnimation& animation) { m_animation = animation; }

void SRTStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    m_lastBlock = lastBlock;
    uint8_t* dataBegin = data + MAX_AV_PACKET_SIZE - m_tmpBuffer.size();
    if (!m_tmpBuffer.empty())
        memmove(dataBegin, m_tmpBuffer.data(), m_tmpBuffer.size());
    const int parsedLen = parseText(dataBegin, dataLen + m_tmpBuffer.size());
    const size_t rest = dataLen + m_tmpBuffer.size() - parsedLen;
    if (rest > MAX_AV_PACKET_SIZE)
        THROW(ERR_COMMON, "Invalid SRT file or too large text message (>" << MAX_AV_PACKET_SIZE << " bytes)")
    // The unfinished line starts where parsing stopped. Its position used to be worked out AFTER the
    // buffer had been resized to the new length, which pointed at the end of the data instead, so
    // the bytes kept were whatever followed the data: a line crossing the end of a read block was
    // corrupted, and so was the last line of a file that does not end with a line end.
    const uint8_t* restStart = dataBegin + parsedLen;
    m_tmpBuffer.resize(rest);
    if (rest > 0)
        memmove(m_tmpBuffer.data(), restStart, rest);

    // A line is only taken once its line end arrives, so the last line of a file that does not end
    // with one stayed in the buffer for good, and with it the last subtitle. At the end of the file
    // that line is finished here, with a line end in the file's own encoding.
    if (m_lastBlock && !m_tmpBuffer.empty())
    {
        std::vector<uint8_t> lastLine(m_tmpBuffer.begin(), m_tmpBuffer.end());
        if (m_charSize == 2)
        {
            const auto* nl = reinterpret_cast<const uint8_t*>(&m_short_N);
            lastLine.insert(lastLine.end(), nl, nl + 2);
        }
        else if (m_charSize == 4)
        {
            const auto* nl = reinterpret_cast<const uint8_t*>(&m_long_N);
            lastLine.insert(lastLine.end(), nl, nl + 4);
        }
        else
            lastLine.push_back('\n');
        parseText(lastLine.data(), lastLine.size());
        m_tmpBuffer.clear();
    }
}

// The encoding is judged from the first block of the file, up to 2 MB. When that is plain ASCII it reads the same as
// UTF-8 and as any 8 bit code page, and UTF-8 was taken for the rest of the file as well: a code page file whose first
// letter beyond ASCII came later had every such letter drawn wrong, or replaced in a Matroska text track. The first
// line beyond ASCII decides it now: valid UTF-8 keeps UTF-8, anything else is read in the active code page from that
// line on, as a file that shows it at once already was. Only Windows has a code page to fall back to.
void SRTStreamReader::checkUtf8Guess(const uint8_t* line, const ptrdiff_t len)
{
    if (std::all_of(line, line + len, [](const uint8_t c) { return c < 0x80; }))
        return;
    m_utf8Guessed = false;
    if (convertUTF::isLegalUTF8String(line, static_cast<size_t>(len)))
        return;
#ifdef _WIN32
    m_srcFormat = UtfConverter::SourceFormat::sfANSI;
    LTRACE(LT_INFO, 2,
           "Line " << m_linesRead + 1
                   << " of the SRT file is not UTF-8, so the file is read in the active code page from that line on. "
                      "Its start is plain ASCII, which reads the same both ways.");
#endif
}

bool SRTStreamReader::detectSrcFormat(const uint8_t* dataStart, const size_t len, int& prefixLen)
{
    prefixLen = 0;
    if (len < 4)
        return false;
    // detect UTF-8/UTF-16/UTF-32 format
    // The first block read can end inside a character, and a UTF-8 file was then judged not to be UTF-8 at all, and
    // read in the code page. A character cut by the end of the block is left out of the check.
    size_t checkLen = len;
    for (size_t back = 1; back <= 3 && back <= len; ++back)
    {
        const uint8_t c = dataStart[len - back];
        if ((c & 0xC0) == 0x80)
            continue;  // a continuation byte: its lead byte is further back
        const size_t needed = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (needed > back)
            checkLen = len - back;
        break;
    }
    const bool hasUtf8Bom = dataStart[0] == 0xEF && dataStart[1] == 0xBB && dataStart[2] == 0xBF;
    if (hasUtf8Bom || convertUTF::isLegalUTF8String(dataStart, checkLen))
    {
        m_charSize = 1;
        m_srcFormat = UtfConverter::SourceFormat::sfUTF8;
        // Only skip the byte order mark when there IS one. This branch is also how a plain UTF-8
        // file with no mark is recognised, and those three bytes were being thrown away from the
        // front of it. Every other branch here already sets prefixLen only on a mark it matched.
        if (hasUtf8Bom)
            prefixLen = 3;
        m_utf8Guessed =
            !hasUtf8Bom && std::all_of(dataStart, dataStart + len, [](const uint8_t c) { return c < 0x80; });
    }
    else if (dataStart[0] == 0 && dataStart[1] == 0 && dataStart[2] == 0xFE && dataStart[3] == 0xFF)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF32be;
        m_charSize = 4;
        prefixLen = 4;
        m_splitterOfs = 3;
        m_long_N = my_htonl(0x0000000a);
        m_long_R = my_htonl(0x0000000d);
    }
    else if (dataStart[0] == 0xFF && dataStart[1] == 0xFE && dataStart[2] == 0 && dataStart[3] == 0)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF32le;
        m_charSize = 4;
        prefixLen = 4;
        m_long_N = my_htonl(0x0a000000);
        m_long_R = my_htonl(0x0d000000);
    }
    else if (dataStart[0] == 0xFE && dataStart[1] == 0xFF)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF16be;
        m_charSize = 2;
        prefixLen = 2;
        m_splitterOfs = 1;
        m_short_N = my_htons(0x000a);
        m_short_R = my_htons(0x000d);
    }
    else if (dataStart[0] == 0xFF && dataStart[1] == 0xFE)
    {
        m_srcFormat = UtfConverter::SourceFormat::sfUTF16le;
        m_charSize = 2;
        prefixLen = 2;
        m_short_N = my_htons(0x0a00);
        m_short_R = my_htons(0x0d00);
    }
    else
    {
#ifdef _WIN32
        LTRACE(LT_INFO, 2, "Failed to auto-detect SRT encoding : falling back to the active code page");
        m_srcFormat = UtfConverter::SourceFormat::sfANSI;  // default value for win32
#else
        LTRACE(LT_INFO, 2, "Failed to auto-detect SRT encoding : falling back to UTF-8");
        m_srcFormat = UtfConverter::SourceFormat::sfUTF8;
#endif
    }
    return true;
}

int SRTStreamReader::parseText(uint8_t* dataStart, const size_t len)
{
    int prefixLen = 0;
    if (m_srcFormat == UtfConverter::SourceFormat::sfUnknown)
    {
        if (!detectSrcFormat(dataStart, len, prefixLen))
            return false;
    }
    // The data ends at dataStart + len. The end used to be counted from after the byte order mark, so the first block
    // of a file that has one was scanned that many bytes past its end, through whatever the buffer held there.
    const uint8_t* cur = dataStart + prefixLen;
    const size_t roundLen = (len - prefixLen) & ~(static_cast<size_t>(m_charSize) - 1);
    const uint8_t* end = cur + roundLen;
    const uint8_t* lastProcessedLine = cur;
    const auto isChar = [this](const uint8_t* p, const bool lineFeed)
    {
        if (m_charSize == 2)
            return loadUnaligned<uint16_t>(p) == (lineFeed ? m_short_N : m_short_R);
        if (m_charSize == 4)
            return loadUnaligned<uint32_t>(p) == (lineFeed ? m_long_N : m_long_R);
        return *p == (lineFeed ? '\n' : '\r');
    };
    // A line ends with an LF, a CR LF, or a CR alone, which is how files from the classic Mac OS end their lines: such
    // a file used to be one single line, and not one subtitle was read from it. Every CR in front of an LF belongs to
    // the line end, so a file converted twice (CR CR LF) reads as it was meant. CRs that reach the end of the data wait
    // for the next block, which may start with their LF; at the end of the file, setBuffer ends the last line itself.
    for (; cur < end; cur += m_charSize)
    {
        const uint8_t* textEnd = cur;
        if (isChar(cur, true))
        {
            while (textEnd >= lastProcessedLine + m_charSize && isChar(textEnd - m_charSize, false))
                textEnd -= m_charSize;
        }
        else if (isChar(cur, false))
        {
            const uint8_t* next = cur + m_charSize;
            while (next < end && isChar(next, false)) next += m_charSize;
            if (next >= end)
                break;
            if (isChar(next, true))
            {
                cur = next - m_charSize;  // the LF ends the line, with these CRs
                continue;
            }
        }
        else
            continue;

        if (m_utf8Guessed)
            checkUtf8Guess(lastProcessedLine, textEnd - lastProcessedLine);
        m_sourceText.emplace_back(UtfConverter::toUtf8(lastProcessedLine, textEnd - lastProcessedLine, m_srcFormat));
        std::string& tmp = m_sourceText.back();
        if (strOnlySpace(tmp))
            tmp.clear();

        m_linesRead++;
        m_origSize.push(static_cast<int32_t>(cur + m_charSize - lastProcessedLine + prefixLen));
        prefixLen = 0;
        lastProcessedLine = cur + m_charSize;
    }
    return static_cast<int>(lastProcessedLine - dataStart);
}

// A line of spaces, or of tabs, is blank. A tab used to make it text, so a line holding one tab between two subtitles
// did not end the first, and every subtitle up to the next truly blank line was drawn as one.
bool SRTStreamReader::strOnlySpace(const std::string& str)
{
    return std::all_of(str.begin(), str.end(), [](const char c) { return c == ' ' || c == '\t'; });
}

// Whether a subtitle has something to show: a character outside its tags that is not a space, a tab, a
// line break or a no-break space.
bool SRTStreamReader::hasVisibleText(const std::string& text)
{
    bool inTag = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inTag)
            inTag = c != '>';
        else if (c == '<')
            inTag = true;
        else if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < text.size() &&
                 static_cast<unsigned char>(text[i + 1]) == 0xA0)
            ++i;
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return true;
    }
    return false;
}

int SRTStreamReader::flushPacket(AVPacket& avPacket)
{
    reportSkippedLines();
    if (!m_keepText)
        return m_dstSubCodec->flushPacket(avPacket);
    startTextPacket(avPacket);
    // The data has ended, so every subtitle that is complete is taken now. A demux writes them all,
    // as one piece of the file; a Matroska block holds one subtitle, so there only the first can go.
    while (readNextCue())
        if (!m_demuxMode)
            break;
    bool first = true;
    while (!m_textCues.empty())
    {
        const int64_t nextMs = m_textCues.size() > 1 ? llround(m_textCues[1].inTime * 1000.0) : -1;
        TextCue cue = std::move(m_textCues.front());
        m_textCues.pop_front();
        int64_t inMs = 0;
        int64_t outMs = 0;
        if (!prepareTextCue(cue, nextMs, inMs, outMs))
            continue;
        if (first)
        {
            avPacket.pts = avPacket.dts = inMs * INTERNAL_PTS_PER_MS;
            avPacket.duration = outMs < 0 ? 0 : (outMs - inMs) * INTERNAL_PTS_PER_MS;
            first = false;
        }
        if (m_demuxMode)
            appendSrtRecord(cue.text, inMs, outMs);
        else
        {
            m_textPacket.assign(cue.text.begin(), cue.text.end());
            break;
        }
    }
    if (!first)
    {
        avPacket.data = m_textPacket.data();
        avPacket.size = static_cast<int>(m_textPacket.size());
        avPacket.flags = AVPacket::IS_IFRAME;
    }
    // Nothing is left over here today: in Matroska output each subtitle has already gone out as its own
    // packet by the time the data ends. Should one ever remain, it is said, never lost in silence.
    if (!m_textCues.empty())
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_textCues.size()
                                 << " subtitles at the end of the track could not be written.");
    m_textCues.clear();
    if (m_lengthlessCues > 1)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_lengthlessCues
                                 << " subtitles in all have no length and were left out.");
    if (m_invalidUtf8Cues > 1)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_invalidUtf8Cues
                                 << " subtitles in all held bytes that are not valid UTF-8.");
    m_lengthlessCues = m_invalidUtf8Cues = 0;
    return 0;
}

// Valid UTF-8, which a Matroska text track must hold: a byte that does not begin or continue a valid
// sequence (RFC 3629) becomes U+FFFD. The text has already been converted from the file's encoding;
// this catches a file whose encoding was judged from its first part and changes later, and a file
// that is not UTF-8 on a system with no code page to fall back to. Returns whether a byte was replaced.
static bool makeValidUtf8(std::string& text)
{
    std::string out;
    bool replaced = false;
    size_t i = 0;
    while (i < text.size())
    {
        const auto b = static_cast<uint8_t>(text[i]);
        size_t len = 0;
        uint8_t lo = 0x80;  // the allowed range of the second byte
        uint8_t hi = 0xBF;
        if (b < 0x80)
            len = 1;
        else if (b >= 0xC2 && b <= 0xDF)
            len = 2;
        else if (b >= 0xE0 && b <= 0xEF)
        {
            len = 3;
            if (b == 0xE0)
                lo = 0xA0;  // no overlong form
            else if (b == 0xED)
                hi = 0x9F;  // no surrogate
        }
        else if (b >= 0xF0 && b <= 0xF4)
        {
            len = 4;
            if (b == 0xF0)
                lo = 0x90;
            else if (b == 0xF4)
                hi = 0x8F;  // nothing above U+10FFFF
        }
        bool ok = len > 0 && i + len <= text.size();
        for (size_t k = 1; ok && k < len; ++k)
        {
            const auto c = static_cast<uint8_t>(text[i + k]);
            ok = k == 1 ? c >= lo && c <= hi : c >= 0x80 && c <= 0xBF;
        }
        if (ok)
        {
            out.append(text, i, len);
            i += len;
            continue;
        }
        out += static_cast<char>(0xEF);
        out += static_cast<char>(0xBF);
        out += static_cast<char>(0xBD);
        replaced = true;
        ++i;
    }
    if (replaced)
        text.swap(out);
    return replaced;
}

void SRTStreamReader::startTextPacket(AVPacket& avPacket)
{
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_SRT;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;
    m_textPacket.clear();
}

// The times of a subtitle in whole milliseconds, which is what SRT holds, so they arrive exactly. The
// end is -1 for a subtitle shown until the next one. Returns false for a subtitle that is left out.
bool SRTStreamReader::prepareTextCue(TextCue& cue, const int64_t nextStartMs, int64_t& inMs, int64_t& outMs)
{
    inMs = llround(cue.inTime * 1000.0);
    outMs = cue.untilNext ? -1 : llround(cue.outTime * 1000.0);
    if (cue.untilNext && m_demuxMode)
    {
        // An SRT file has no way to say "until the next one": the end is where the next one starts.
        // The last one has no next, and is given an hour, which on screen means until the end.
        if (nextStartMs >= 0)
            outMs = nextStartMs;
        else
        {
            outMs = inMs + 3600000;
            if (!m_openEndWarned)
                LTRACE(LT_WARN, 2,
                       "Warning: track " << m_streamIndex << ": the last subtitle, at " << floatToTime(cue.inTime, ',')
                                         << ", has no end time. It is written with an end one hour later.");
            m_openEndWarned = true;
        }
    }
    // A subtitle that ends when it starts, or earlier, is never on screen. Written without a duration it
    // would mean the opposite, shown until the next one, so it is left out.
    if (outMs >= 0 && outMs <= inMs)
    {
        if (m_lengthlessCues++ == 0)
            LTRACE(LT_WARN, 2,
                   "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(cue.inTime, ',')
                                     << " ends when it starts or earlier. It is never shown, so it is left out.");
        return false;
    }
    if (makeValidUtf8(cue.text) && m_invalidUtf8Cues++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(cue.inTime, ',')
                                 << " holds bytes that are not valid UTF-8, written as the replacement "
                                    "character U+FFFD. Save the file as UTF-8 to keep every character.");
    return true;
}

// A time as SRT writes it, hh:mm:ss,mmm
static std::string srtTime(int64_t ms)
{
    ms = (std::max)(int64_t{0}, ms);
    std::string out = std::to_string(ms / 3600000);
    if (out.size() < 2)
        out.insert(0, 1, '0');
    for (const int64_t part : {ms / 60000 % 60, ms / 1000 % 60})
    {
        out += ':';
        out += static_cast<char>('0' + part / 10);
        out += static_cast<char>('0' + part % 10);
    }
    const int64_t frac = ms % 1000;
    out += ',';
    out += static_cast<char>('0' + frac / 100);
    out += static_cast<char>('0' + frac / 10 % 10);
    out += static_cast<char>('0' + frac % 10);
    return out;
}

// One subtitle as an SRT record for a demux: its number, counted from 1, its times and its lines, with
// CR LF line ends, and the UTF-8 byte order mark before the first record, so that the encoding is
// never guessed. The text is as the subtitle holds it, tags included.
void SRTStreamReader::appendSrtRecord(const std::string& text, const int64_t inMs, const int64_t outMs)
{
    const auto newLine = [this]
    {
        m_textPacket.push_back(13);
        m_textPacket.push_back(10);
    };
    const auto add = [this](const std::string& str)
    { m_textPacket.insert(m_textPacket.end(), str.begin(), str.end()); };
    if (m_srtRecords == 0)
        m_textPacket.insert(m_textPacket.end(), {0xEF, 0xBB, 0xBF});
    add(std::to_string(++m_srtRecords));
    newLine();
    add(srtTime(inMs) + " --> " + srtTime(outMs));
    newLine();
    for (const char c : text)
        if (c == 10)
            newLine();
        else
            m_textPacket.push_back(static_cast<uint8_t>(c));
    newLine();
    newLine();
}

// One subtitle as text: its UTF-8 text with the lines joined by a line feed and every tag as written,
// at its start, with its length as the duration. In a demux, the same subtitle as an SRT record.
int SRTStreamReader::readTextPacket(AVPacket& avPacket)
{
    startTextPacket(avPacket);
    for (;;)
    {
        while (m_textCues.empty())
            if (!readNextCue())
                return NEED_MORE_DATA;
        // In a demux a subtitle without an end time ends where the next one starts, so it waits for
        // the next one. At the end of the data flushPacket writes it.
        if (m_demuxMode && m_textCues.front().untilNext)
            while (m_textCues.size() < 2)
                if (!readNextCue())
                    return NEED_MORE_DATA;
        const int64_t nextMs = m_textCues.size() > 1 ? llround(m_textCues[1].inTime * 1000.0) : -1;
        TextCue cue = std::move(m_textCues.front());
        m_textCues.pop_front();
        int64_t inMs = 0;
        int64_t outMs = 0;
        if (!prepareTextCue(cue, nextMs, inMs, outMs))
            continue;
        if (m_demuxMode)
            appendSrtRecord(cue.text, inMs, outMs);
        else
            m_textPacket.assign(cue.text.begin(), cue.text.end());
        avPacket.data = m_textPacket.data();
        avPacket.size = static_cast<int>(m_textPacket.size());
        avPacket.pts = avPacket.dts = inMs * INTERNAL_PTS_PER_MS;
        // A subtitle without an end is shown until the next one: no duration
        avPacket.duration = outMs < 0 ? 0 : (outMs - inMs) * INTERNAL_PTS_PER_MS;
        avPacket.flags = AVPacket::IS_IFRAME;
        return 0;
    }
}

int SRTStreamReader::readPacket(AVPacket& avPacket)
{
    if (m_keepText)
        return readTextPacket(avPacket);
    const int rez = m_dstSubCodec->readPacket(avPacket);
    if (rez == NEED_MORE_DATA)
    {
        uint32_t renderedLen;
        uint8_t* renderedBuffer = renderNextMessage(renderedLen);
        if (renderedBuffer)
        {
            m_dstSubCodec->setBuffer(renderedBuffer - MAX_AV_PACKET_SIZE, renderedLen,
                                     m_lastBlock && m_sourceText.empty());
            return m_dstSubCodec->readPacket(avPacket);
        }
        return NEED_MORE_DATA;
    }
    return rez;
}

uint8_t* SRTStreamReader::renderNextMessage(uint32_t& renderedLen)
{
    // Keep reading cues until something can be drawn. Returning with complete cues still waiting
    // would ask for more data, and at the end of the file there is none: they would be lost.
    for (;;)
    {
        while (m_toDraw.empty())
        {
            if (!readNextCue())
            {
                // The file is finished, so whatever is held is complete
                if (m_lastBlock && m_sourceText.empty())
                    flushCueGroup();
                break;
            }
        }
        if (m_toDraw.empty())
        {
            if (m_lastBlock && m_sourceText.empty() && !m_unseenReported)
            {
                if (m_shrunkCues > 1)
                    LTRACE(LT_WARN, 2,
                           "Warning: track " << m_streamIndex << ": " << m_shrunkCues
                                             << " subtitles in all did not fit the picture and were drawn smaller.");
                if (m_unseenCues > 1)
                    LTRACE(LT_WARN, 2,
                           "Warning: track " << m_streamIndex << ": " << m_unseenCues
                                             << " subtitles in all were left out because none of their text lands "
                                                "inside the picture.");
                m_unseenReported = true;
            }
            return nullptr;
        }
        // A piece that draws nothing, such as a cue of tags only, is passed over
        if (uint8_t* rez = drawNext(renderedLen))
            return rez;
    }
}

// The line at the front is not a timing line. A blank line or a number, a counter written twice or
// standing apart from its timing line, says nothing; other text is named, the first five times.
void SRTStreamReader::skipNonCueLine()
{
    const std::string& line = m_sourceText.front();
    // A counter that carries a byte order mark (two files joined byte for byte) is still only a counter: it was named
    // as skipped text although its subtitle was kept.
    const size_t from = line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!std::all_of(line.begin() + static_cast<std::ptrdiff_t>(from), line.end(),
                     [](const char c) { return (c >= '0' && c <= '9') || c == ' '; }))
    {
        const int64_t lineNumber = m_linesRead - static_cast<int64_t>(m_sourceText.size()) + 1;
        if (m_skippedLines == 0)
        {
            m_firstSkippedLine = lineNumber;
            m_firstSkippedText = line;
        }
        if (m_skippedLines++ < 5)
        {
            std::ostringstream what;
            what << "Warning: track " << m_streamIndex;
            if (!m_streamName.empty())
                what << " (" << m_streamName << ")";
            what << ", line " << lineNumber << ": \"" << line << "\" is not part of a subtitle and is skipped.";
            LTRACE(LT_WARN, 2, what.str());
        }
    }
    m_sourceText.pop_front();
    m_processedSize += m_origSize.front();
    m_origSize.pop();
}

// At the end of the track. Lines passed over in a file that gave subtitles are a warning; in a file that
// gave none at all they are the reason, and that is an error, as it always was: an empty track that
// reports success would be worse than the stop.
void SRTStreamReader::reportSkippedLines()
{
    if (m_skippedLines == 0)
        return;
    if (m_cuesRead == 0)
    {
        std::ostringstream what;
        what << "No subtitle could be read from track " << m_streamIndex;
        if (!m_streamName.empty())
            what << " (" << m_streamName << ")";
        what << ". The first line that is not part of a subtitle is line " << m_firstSkippedLine << ", \""
             << m_firstSkippedText << "\". A subtitle starts with a timing line like 00:00:20,000 --> 00:00:24,400";
        THROW(ERR_COMMON, what.str())
    }
    if (m_skippedLines > 5)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": " << m_skippedLines
                                 << " lines in all that are not part of any subtitle were skipped.");
    m_skippedLines = 0;
}

// Parses one cue and hands it to addCue. Returns false when the next cue is not complete yet.
bool SRTStreamReader::readNextCue()
{
    if (m_sourceText.empty())
    {
        // The end of the file usually arrives as a call of its own, after the last lines. The last
        // subtitle is then complete but still waiting for the blank line that would end it, and it
        // was never drawn, because this returned before looking.
        if (m_lastBlock && m_state == ParseState::PARSE_TEXT && !m_renderedText.empty())
        {
            m_state = ParseState::PARSE_FIRST_LINE;
            addCue(m_renderedText, m_inTime, m_outTime);
            m_renderedText.clear();
            return true;
        }
        return false;
    }
    if (m_state == ParseState::PARSE_FIRST_LINE)
    {
        while (!m_sourceText.empty() && m_sourceText.front().empty())
        {
            m_sourceText.pop_front();  // delete empty lines before message
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
        if (m_sourceText.empty())
            return false;
        m_state = ParseState::PARSE_TIME;
        bool isNUmber = true;
        {
            for (const auto& c : m_sourceText.front())
                if (!(c >= '0' && c <= '9') && c != ' ')
                {
                    isNUmber = false;
                    break;
                }
        }
        if (isNUmber)
        {
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
            if (m_sourceText.empty())
                return false;
        }
    }
    if (m_state == ParseState::PARSE_TIME)
    {
        // A line where a timing line belongs that is not one used to end the whole mux. Files carry such
        // lines often enough: a counter written twice, a blank line between the counter and its timing
        // line, a title or a note above the first subtitle. A blank line or a number is passed over;
        // any other text is passed over and named in a warning, so nothing is lost in silence.
        while (!m_sourceText.empty() && !parseTime(m_sourceText.front())) skipNonCueLine();
        if (m_sourceText.empty())
            return false;
        m_state = ParseState::PARSE_TEXT;
        m_sourceText.pop_front();
        m_processedSize += m_origSize.front();
        m_origSize.pop();
        if (m_sourceText.empty())
            return false;
    }

    bool blankLineEnds = true;  // a subtitle ends at a blank line, which goes with it
    for (;;)
    {
        while (!m_sourceText.empty() && !m_sourceText.front().empty())
        {
            const int next = nextCueFollows();
            if (next < 0)
                return false;  // the next lines have not arrived yet
            if (next > 0)
            {
                blankLineEnds = false;
                break;
            }
            if (!m_renderedText.empty())
                m_renderedText += '\n';
            m_renderedText += m_sourceText.front();
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
        if (!blankLineEnds || m_sourceText.empty())
            break;
        // A blank line. SRT has no way to write an empty line inside a subtitle, and files that
        // contain one anyway are common: what follows it is then neither a counter nor a timing
        // line. That used to end the whole mux with a parse error. The blank line ends the
        // subtitle only when a new one follows it; otherwise it belongs to the text.
        const int ends = blankLineEndsCue();
        if (ends < 0)
            return false;  // the next lines have not arrived yet
        if (ends > 0)
            break;
        while (!m_sourceText.empty() && m_sourceText.front().empty())
        {
            m_renderedText += '\n';
            m_sourceText.pop_front();
            m_processedSize += m_origSize.front();
            m_origSize.pop();
        }
    }

    if (m_sourceText.empty())
    {
        if (m_lastBlock && !m_renderedText.empty())
        {
            // The last subtitle of a file with no blank line after it. Its text used to be cleared
            // before it was drawn, so it was drawn empty and never appeared.
            m_state = ParseState::PARSE_FIRST_LINE;
            addCue(m_renderedText, m_inTime, m_outTime);
            m_renderedText.clear();
            return true;
        }
        return false;
    }
    if (blankLineEnds)
    {
        m_sourceText.pop_front();  // delete empty line (messages separator)
        m_processedSize += m_origSize.front();
        m_origSize.pop();
    }
    addCue(m_renderedText, m_inTime, m_outTime);
    m_state = ParseState::PARSE_FIRST_LINE;
    m_renderedText.clear();
    return true;
}

// Closing tags for the style tags a cue leaves open, so that a cue drawn below it in the same
// picture does not inherit its italic, bold or colour. Only used when cues are stacked.
static std::string closeOpenTags(const std::string& text)
{
    static const char* const KNOWN[] = {"i", "italic", "b", "bold", "u", "underline", "strike", "f", "force", "font"};
    std::vector<std::string> open;
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos)
    {
        const size_t end = text.find('>', pos);
        if (end == std::string::npos)
            break;
        std::string tag = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(0, 1);
        tag = tag.substr(0, tag.find_first_of(" \t="));
        for (char& c : tag) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (std::find(std::begin(KNOWN), std::end(KNOWN), tag) == std::end(KNOWN))
            continue;
        if (!closing)
            open.push_back(tag);
        else if (const auto it = std::find(open.rbegin(), open.rend(), tag); it != open.rend())
            open.erase(std::next(it).base());
    }
    std::string closers;
    for (auto it = open.rbegin(); it != open.rend(); ++it) closers += "</" + *it + ">";
    return closers;
}

// Whether the text has anything to show once its tags are taken out. A cue such as "<i></i>" or one
// of spaces draws nothing; in a group it would only add an empty line that pushes the others up.
static bool hasVisibleText(const std::string& text)
{
    bool inTag = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '<')
            inTag = true;
        else if (c == '>' && inTag)
            inTag = false;
        else if (inTag || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        else if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0)
            ++i;  // a no-break space
        else
            return true;
    }
    return false;
}

void SRTStreamReader::addCue(const std::string& text, const double inTime, const double outTime)
{
    ++m_cuesRead;
    // save-srt is asked for by its file name: the file itself is made with the first subtitle written
    if (!m_saveSrtName.empty() && !m_keepText)
        saveSrtCue(text, inTime, outTime, m_untilNext);
    // Kept as text, each subtitle goes out as it was written, overlaps and all: Matroska carries
    // subtitles that overlap as they are, so nothing is grouped or drawn. A subtitle with no text
    // at all has nothing to carry.
    if (m_keepText)
    {
        if (!text.empty())
            m_textCues.push_back(TextCue{text, inTime, outTime, m_untilNext});
        return;
    }
    // Each cue used to be drawn as a picture of its own that cleared the screen at its end. Two cues
    // that overlap then wrote their pictures out of time order, and the first one's end cleared
    // the second while it was still due.
    //
    // Only a subtitle FILE is grouped. It is read ahead on its own, so holding a cue until the next
    // one shows whether they overlap costs nothing. From a container the next cue may be minutes
    // away in the source, and holding the cue would deliver it late: there each cue is drawn at
    // once, as before.
    // A timing line without an end: the subtitle stays until the next one replaces it, so it is
    // drawn without the composition that would clear it. The end given to the drawing only bounds
    // the fade in.
    if (m_untilNext)
    {
        flushCueGroup();
        if (hasVisibleText(text))
            m_toDraw.push_back(Piece{text, inTime, inTime + 3600.0, false});
        return;
    }
    const Cue cue{text, inTime, outTime, m_srtRender->alignToGrid(inTime), m_srtRender->alignToGrid(outTime)};
    // Overlaps are judged on the frame grid, where the cues are drawn: two cues that touch within
    // one frame, which converted files are full of, are not an overlap. A cue with no text or no
    // length on the grid takes no part in a group.
    const bool drawable = hasVisibleText(text) && cue.outFrame > cue.inFrame;
    const bool joins =
        m_containerType == ContainerType::ctNone && !m_cueGroup.empty() && drawable && cue.inFrame < m_cueGroupEnd;
    if (!joins)
    {
        if (!m_cueGroup.empty() && cue.inFrame < m_cueGroupEnd)
            return;  // no text or no length, inside a group: nothing to add to it
        flushCueGroup();
    }
    m_cueGroupEnd = m_cueGroup.empty() ? cue.outFrame : (std::max)(m_cueGroupEnd, cue.outFrame);
    m_cueGroup.push_back(cue);
    if (m_containerType != ContainerType::ctNone || !drawable)
        flushCueGroup();
}

void SRTStreamReader::flushCueGroup()
{
    if (m_cueGroup.size() == 1)
    {
        // A cue that overlaps nothing is drawn exactly as it always was, from its times as read
        const Cue& c = m_cueGroup.front();
        m_toDraw.push_back(Piece{c.text, c.inTime, c.outTime, true});
    }
    else if (m_cueGroup.size() > 1)
    {
        // A sweep over the starts and ends. Up to MAX_STACKED cues are shown at once, the ones that
        // started last: a cue with a mistyped end time would otherwise stay on top of everything
        // that follows it, and a damaged file could make one picture of thousands of lines.
        constexpr size_t MAX_STACKED = 4;
        const size_t firstPiece = m_toDraw.size();
        std::vector<double> bounds;
        for (const Cue& c : m_cueGroup)
        {
            bounds.push_back(c.inFrame);
            bounds.push_back(c.outFrame);
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
        std::vector<size_t> byStart(m_cueGroup.size());
        std::vector<size_t> byEnd(m_cueGroup.size());
        for (size_t i = 0; i < m_cueGroup.size(); ++i) byStart[i] = byEnd[i] = i;
        std::stable_sort(byStart.begin(), byStart.end(), [this](const size_t a, const size_t b)
                         { return m_cueGroup[a].inFrame < m_cueGroup[b].inFrame; });
        std::stable_sort(byEnd.begin(), byEnd.end(), [this](const size_t a, const size_t b)
                         { return m_cueGroup[a].outFrame < m_cueGroup[b].outFrame; });
        std::set<std::pair<double, size_t>> active;  // ordered by start, then by place in the file
        size_t nextStart = 0;
        size_t nextEnd = 0;
        bool capped = false;
        for (size_t k = 0; k + 1 < bounds.size(); ++k)
        {
            const double t = bounds[k];
            while (nextEnd < byEnd.size() && m_cueGroup[byEnd[nextEnd]].outFrame <= t)
            {
                active.erase({m_cueGroup[byEnd[nextEnd]].inFrame, byEnd[nextEnd]});
                ++nextEnd;
            }
            while (nextStart < byStart.size() && m_cueGroup[byStart[nextStart]].inFrame <= t)
            {
                if (m_cueGroup[byStart[nextStart]].outFrame > t)
                    active.insert({m_cueGroup[byStart[nextStart]].inFrame, byStart[nextStart]});
                ++nextStart;
            }
            if (active.empty())
                continue;
            if (active.size() > MAX_STACKED && !capped)
            {
                capped = true;
                LTRACE(LT_WARN, 2,
                       "Warning: more than " << MAX_STACKED << " subtitles of track " << m_streamIndex << " overlap at "
                                             << t << " s. Only the ones that started last are drawn while they do.");
            }
            auto first = active.begin();
            if (active.size() > MAX_STACKED)
                std::advance(first, active.size() - MAX_STACKED);
            std::string text;
            for (auto it = first; it != active.end(); ++it)
            {
                const std::string& part = m_cueGroup[it->second].text;
                if (!text.empty())
                    text += '\n';
                text += part + closeOpenTags(part);
            }
            // A boundary that changes nothing visible, such as the end of a cue that was not shown
            // because more recent ones filled the picture, extends the piece before it
            if (m_toDraw.size() > firstPiece && m_toDraw.back().outTime == t && m_toDraw.back().text == text)
            {
                m_toDraw.back().outTime = bounds[k + 1];
                continue;
            }
            m_toDraw.push_back(Piece{std::move(text), t, bounds[k + 1], false});
        }
        // The screen is cleared only where nothing follows a piece; elsewhere the next piece
        // replaces it at the same moment
        for (size_t i = firstPiece; i < m_toDraw.size(); ++i)
            if (!m_toDraw[i].hide && (i + 1 == m_toDraw.size() || m_toDraw[i + 1].inTime != m_toDraw[i].outTime))
                m_toDraw[i].hide = true;
    }
    m_cueGroup.clear();
}

uint8_t* SRTStreamReader::drawNext(uint32_t& renderedLen)
{
    Piece piece = std::move(m_toDraw.front());
    m_toDraw.pop_front();
    // Fades belong to the edges of what is on screen: none where a piece takes over from another
    text_subtitles::TextAnimation animation = m_animation;
    if (m_screenShown)
        animation.fadeInDuration = 0;
    if (!piece.hide)
        animation.fadeOutDuration = 0;
    // A subtitle whose text does not fit the picture, usually because of a font size far too large for it, was
    // cut off at the edges or, when none of it landed inside, left out. It is drawn at the largest size that fits
    // instead. A subtitle that fits is drawn exactly as written.
    text_subtitles::TextSubtitlesRender* render = m_srtRender->m_textRender;
    // A font size is a whole number, so most of the search's steps give sizes an earlier step already measured, and
    // each measurement sets every font of the text. A measured set of sizes is answered from memory: the search takes
    // the same steps to the same size as before, with less than half the measuring.
    if (hasVisibleText(piece.text) && !render->textFits(piece.text))
    {
        double lo = 0.0;
        double hi = 1.0;
        std::map<std::vector<int>, bool> measured;
        for (int step = 0; step < 24; ++step)
        {
            render->m_fitScale = (lo + hi) / 2;
            auto [known, isNew] = measured.try_emplace(render->fittedSizes(piece.text), false);
            if (isNew)
                known->second = render->textFits(piece.text);
            if (known->second)
                lo = render->m_fitScale;
            else
                hi = render->m_fitScale;
        }
        render->m_fitScale = lo;
        if (lo > 0.0 && m_shrunkCues++ == 0)
            LTRACE(LT_WARN, 2,
                   "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(piece.inTime, ',')
                                     << " does not fit the picture, which usually means its font size is too large "
                                        "for it. It is drawn smaller, at the largest size that fits ("
                                     << lround(lo * 100) << "% of its size).");
    }
    uint8_t* rez = m_srtRender->doConvert(piece.text, animation, piece.inTime, piece.outTime, renderedLen, piece.hide);
    render->m_fitScale = 1.0;
    if (rez)
    {
        m_screenShown = !piece.hide;
        return rez;
    }
    // A subtitle with text that draws nothing is one whose text lies wholly outside the picture, which
    // is what a font size far too large does. It used to vanish without a word.
    if (hasVisibleText(piece.text) && m_unseenCues++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: track " << m_streamIndex << ": the subtitle at " << floatToTime(piece.inTime, ',')
                                 << " is left out: none of its text lands inside the picture, which usually "
                                    "means its font size is too large for it.");
    // This piece draws nothing. If the one before it is still on screen, it has to go now, or it
    // would stay until whatever is drawn next.
    if (m_screenShown)
    {
        m_screenShown = false;
        return m_srtRender->doClear(piece.inTime, renderedLen);
    }
    return nullptr;
}

// Whether the blank line at the front ends the subtitle being read: 1 when what follows the blank
// lines is a timing line, or a counter followed by one, or the end of the file; 0 when it is more
// text of the same subtitle; -1 when the lines that decide it have not arrived yet.
// Whether the text is a time as SRT writes it: digits, up to two colons, and an optional fraction after
// a comma or a point, as in 00:01:02,500, 1:02.5 or 62. Spaces around it are allowed.
static bool isSrtTime(const std::string& text)
{
    size_t i = text.find_first_not_of(" \t");
    const size_t end = text.find_last_not_of(" \t");
    if (i == std::string::npos)
        return false;
    int colons = 0;
    bool fraction = false;
    bool digit = false;
    for (; i <= end; ++i)
    {
        const char c = text[i];
        if (c >= '0' && c <= '9')
            digit = true;
        else if (c == ':' && !fraction && digit && ++colons <= 2)
            digit = false;
        else if ((c == ',' || c == '.') && !fraction && digit)
        {
            fraction = true;
            digit = false;
        }
        else
            return false;
    }
    return digit || fraction;
}

// A timing line: a time, the arrow, and a time or nothing as the first word after it. Display
// coordinates may follow the end time. A line of dialogue that holds an arrow is no timing line.
static bool isTimingLine(const std::string& line)
{
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos || !isSrtTime(line.substr(0, arrow)))
        return false;
    const std::string rest = line.substr(arrow + 3);
    const size_t first = rest.find_first_not_of(" \t");
    if (first == std::string::npos)
        return true;  // no end: shown until the next subtitle
    return isSrtTime(rest.substr(first, rest.find_first_of(" \t", first) - first));
}

// A line meant as a timing line that is not a readable one: it starts with a time (h:mm:ss and a separator), as
// "00:00:05,000 -> 00:00:07,000" or "00:00:05:000 --> 00:00:07:000". Dialogue rarely starts that way; an arrow
// alone is not taken as a sign, because dialogue may hold one.
static bool looksLikeTimingLine(const std::string& line)
{
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos)
        return false;
    auto digits = [&](size_t minN, size_t maxN)
    {
        size_t n = 0;
        while (p < line.size() && line[p] >= '0' && line[p] <= '9' && n < maxN) ++p, ++n;
        return n >= minN;
    };
    const size_t start = p;
    if (digits(1, 2) && p < line.size() && line[p++] == ':' && digits(2, 2) && p < line.size() && line[p++] == ':' &&
        digits(2, 2) && p < line.size() && (line[p] == ',' || line[p] == '.' || line[p] == ':'))
        return true;
    // Or a time without its hours, "01:00,000 --> 01:03,000": only digits, colons, commas, dots and spaces in front of
    // an arrow. Its block was drawn inside the subtitle before it without a word.
    const size_t arrow = line.find("->", start);
    return arrow != std::string::npos && arrow > start && line.find_first_not_of("0123456789:,. \t", start) >= arrow &&
           line.find(':', start) < arrow;
}

int SRTStreamReader::blankLineEndsCue() const
{
    size_t i = 0;
    while (i < m_sourceText.size() && m_sourceText[i].empty()) ++i;
    // From a container each cue arrives whole, as one block of its own that ends with its blank line,
    // and the next cue comes only with the next block, which may be minutes away in the source.
    // Waiting for it delivered every cue late, and the last one never, because a track inside a
    // container is not always told when its data has ended. So there, a blank line with nothing
    // after it ends the cue. An empty line inside a cue still has its cue's later lines behind it.
    if (i == m_sourceText.size())
        return m_lastBlock || m_containerType != ContainerType::ctNone ? 1 : -1;
    // A counter can stand apart from its timing line, written twice or with a blank line after it, so the
    // counters and blank lines that follow are looked past: a timing line behind them ends the subtitle,
    // any other text means the blank line belongs to it.
    // A number alone that is followed by another number before the timing line is not the counter: the subtitle
    // "The year was", a blank line and "1985" lost its "1985" in silence, taken for the counter of the next one.
    std::string firstNumber;
    bool otherNumber = false;
    for (; i < m_sourceText.size(); ++i)
    {
        // A counter that carries a byte order mark, left by two files joined byte for byte, is still a counter.
        std::string next = m_sourceText[i];
        if (next.size() >= 3 && next.compare(0, 3, "\xEF\xBB\xBF") == 0)
            next.erase(0, 3);
        if (isTimingLine(next))
            return otherNumber ? 0 : 1;
        if (const std::string number = trimStr(next);
            !number.empty() && number.find_first_not_of("0123456789") == std::string::npos)
        {
            if (firstNumber.empty())
                firstNumber = number;
            else if (number != firstNumber)
                otherNumber = true;
        }
        // A timing line that cannot be read (a mistyped arrow, a colon for the comma) used to make the blank line part
        // of the subtitle, and the broken block, its counter and its text were drawn inside the subtitle before it
        // without a word. The subtitle ends here instead; the broken block is then skipped and named in a warning.
        if (looksLikeTimingLine(next))
            return 1;
        const bool counterOrBlank =
            std::all_of(next.begin(), next.end(), [](const char c) { return (c >= '0' && c <= '9') || c == ' '; });
        if (!counterOrBlank)
            return 0;
    }
    return m_lastBlock ? 1 : -1;
}

// Whether the next subtitle begins at the line in front: a timing line, or a counter (a number alone on its line) right
// before a timing line. Files leave out the blank line between two subtitles often enough, and the next subtitle's
// counter and timing line were then drawn as more text of the one before, with every subtitle after it up to the next
// blank line. 1 when it begins there, 0 when the line is text, -1 when the line that decides it has not arrived yet.
// Only the number right before the timing line is taken for the counter, so a number in the text stays text.
int SRTStreamReader::nextCueFollows() const
{
    const std::string& line = m_sourceText.front();
    if (isTimingLine(line))
        return 1;
    const bool counter = !line.empty() && std::all_of(line.begin(), line.end(),
                                                      [](const char c) { return (c >= '0' && c <= '9') || c == ' '; });
    if (!counter)
        return 0;
    if (m_sourceText.size() < 2)
        return m_lastBlock || m_containerType != ContainerType::ctNone ? 0 : -1;
    return isTimingLine(m_sourceText[1]) ? 1 : 0;
}

bool SRTStreamReader::parseTime(const string& text)
{
    if (!isTimingLine(text))
        return false;
    // length() - 2 is unsigned, so a line shorter than the arrow it is looking for wrapped the
    // bound to SIZE_MAX and the loop walked off the end of the string. A blank line where a
    // timing line belongs is exactly that case, and it is what a damaged subtitle file gives.
    for (size_t i = 0; i + 2 < text.length(); i++)
    {
        if (text[i] == '-' && text[i + 1] == '-' && text[i + 2] == '>')
        {
            // Tabs around the arrow are white space as well: trimStr keeps them, and a tab after the arrow left the
            // end time empty below, so the subtitle stayed on until the next one.
            const auto trim = [](const string& part)
            {
                const size_t b = part.find_first_not_of(" \t\r\n");
                return b == string::npos ? string() : part.substr(b, part.find_last_not_of(" \t\r\n") - b + 1);
            };
            string first = trim(text.substr(0, i));
            string second = trim(text.substr(i + 3, text.length() - i - 3));
            // Some writers put display coordinates after the end time, "X1:100 X2:600 Y1:50 Y2:100".
            // The end time is read by splitting at colons from the right, so those numbers were read
            // as the time: a subtitle due to end at 3 s ended at 600 hours. Only the first word after
            // the arrow is the time.
            if (const size_t gap = second.find_first_of(" \t"); gap != string::npos)
                second.resize(gap);
            for (char& c : first)
            {
                if (c == ',')
                    c = '.';
            }
            for (char& c : second)
            {
                if (c == ',')
                    c = '.';
            }

            m_inTime = timeToFloat(first);
            m_outTime = timeToFloat(second);
            m_untilNext = second.empty();
            return true;
        }
    }
    return false;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
CheckStreamRez SRTStreamReader::checkStream(uint8_t* buffer, const int len, const ContainerType containerType,
                                            const int containerDataType, int containerStreamIndex)
{
    CheckStreamRez rez;
    if (((containerType == ContainerType::ctMKV || containerType == ContainerType::ctMOV) &&
         containerDataType == TRACKTYPE_SRT) ||
        containerType == ContainerType::ctSRT)
    {
        rez.codecInfo = srtCodecInfo;
        rez.streamDescr = "SRT text subtitles";
        if (containerType == ContainerType::ctSRT)
            rez.lang = detectUTF8Lang(buffer, len);
    }
    return rez;
}

std::string SRTStreamReader::detectUTF8Lang(uint8_t* buffer, int len) { return ""; }
