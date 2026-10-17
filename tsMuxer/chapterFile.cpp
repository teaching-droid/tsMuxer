#include "chapterFile.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "fs/textfile.h"
#include "vodCoreException.h"
#include "vod_common.h"

std::string iso639FromBcp47(const std::string& tag);  // metaDemuxer.cpp

namespace
{
std::string trimmed(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string upperCase(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A ChapterLanguage is an ISO 639-2 code. Whatever the file said went into the output as it stood, "english" or a
// piece of markup included. A BCP 47 tag (de, pt-BR) gives the code it stands for; anything else is "und", with a
// warning.
static std::string chapterLanguage(const std::string& value, const std::string& fileName)
{
    const std::string lower = strToLowerCase(value);
    if (lower.size() == 3 && isIso639_2(lower))
        return lower;
    if (const std::string code = iso639FromBcp47(value); !code.empty())
        return code;
    LTRACE(LT_WARN, 2,
           "Warning: " << fileName << ": the chapter language \"" << value
                       << "\" is not a language code; the chapter is marked und.");
    return "und";
}

std::string unescapeXml(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '&')
        {
            out += in[i];
            continue;
        }
        const size_t end = in.find(';', i);
        if (end == std::string::npos)
        {
            out += in[i];
            continue;
        }
        const std::string entity = in.substr(i, end - i + 1);
        if (entity == "&amp;")
            out += '&';
        else if (entity == "&lt;")
            out += '<';
        else if (entity == "&gt;")
            out += '>';
        else if (entity == "&quot;")
            out += '"';
        else if (entity == "&apos;")
            out += '\'';
        else if (entity.size() > 3 && entity[1] == '#')
        {
            // A character by its number, &#233; or &#xE9;, written as UTF-8. It was kept as written, and the name of
            // the chapter showed "Caf&#233;".
            const bool hex = entity[2] == 'x' || entity[2] == 'X';
            const std::string digits = entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
            char* stop = nullptr;
            const unsigned long cp = digits.empty() ? 0 : strtoul(digits.c_str(), &stop, hex ? 16 : 10);
            if (digits.empty() || *stop != '\0' || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                out += entity;
            else if (cp < 0x80)
                out += static_cast<char>(cp);
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | cp >> 6);
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | cp >> 12);
                out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | cp >> 18);
                out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
                out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        else
        {
            out += entity;  // not one we know: leave it alone rather than mangle the name
            i = end;
            continue;
        }
        i = end;
    }
    return out;
}

std::string escapeXml(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

// The text between <tag> and </tag>, searched only between from and to. Attributes on the opening
// tag are tolerated. Returns false when the tag is not in that range.
bool tagText(const std::string& xml, const std::string& tag, const size_t from, const size_t to, std::string& out)
{
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";
    const size_t o = xml.find(open, from);
    if (o == std::string::npos || o >= to)
        return false;
    const size_t gt = xml.find('>', o);
    if (gt == std::string::npos || gt >= to)
        return false;
    const size_t c = xml.find(close, gt);
    if (c == std::string::npos || c > to)
        return false;
    out = xml.substr(gt + 1, c - gt - 1);
    return true;
}

std::string readWholeFile(const std::string& fileName)
{
    TextFile file;
    if (!file.open(fileName.c_str(), File::ofRead))
        THROW(ERR_COMMON, "Can't open the chapter file " << fileName)
    std::string raw;
    char buf[16384];
    int got;
    while ((got = file.read(buf, sizeof(buf))) > 0) raw.append(buf, static_cast<size_t>(got));
    file.close();
    // One line break per line of the file, and an empty line stays a line, so that a message can name
    // the line an editor shows (reading line by line dropped the empty ones). CR LF, a lone LF and a
    // lone CR each end one line.
    std::string text;
    text.reserve(raw.size() + 1);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r')
        {
            text += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
        }
        else
            text += raw[i];
    }
    if (!text.empty() && text.back() != '\n')
        text += '\n';
    return text;
}

// CHAPTER01=00:00:00.000 and CHAPTER01NAME=Opening, in any order, any number of digits, any case.
std::vector<AVChapter> parseOgm(const std::string& text, const std::string& fileName)
{
    // Keyed by the chapter number, which only orders the file: a list starting at 00 and one starting
    // at 01 both have to work, and the numbers need not be dense. The number used to index a vector
    // directly, so a large one asked for a list of up to two billion entries.
    std::map<uint32_t, std::string> times;
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, int> timeLine;  // where each was given, for a message about a repeat
    std::map<uint32_t, int> nameLine;
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size())
    {
        const size_t nl = text.find('\n', pos);
        const std::string line = trimmed(text.substr(pos, nl == std::string::npos ? nl : nl - pos));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineNo;
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = upperCase(trimmed(line.substr(0, eq)));
        const std::string value = trimmed(line.substr(eq + 1));
        if (key.compare(0, 7, "CHAPTER") != 0)
            continue;

        // CHAPTER<digits> or CHAPTER<digits>NAME.
        size_t i = 7;
        while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) ++i;
        if (i == 7)
            continue;
        const bool isName = key.compare(i, std::string::npos, "NAME") == 0;
        if (!isName && i != key.size())
            continue;
        // Leading zeros do not count: CHAPTER0000000001 is chapter 1.
        size_t first = 7;
        while (first < i && key[first] == '0') ++first;
        if (i - first > 9)
            THROW(ERR_COMMON, fileName << ", line " << lineNo << ": the chapter number " << key.substr(7, i - 7)
                                       << " is too large (at most 9 digits)")
        const auto number = first < i ? static_cast<uint32_t>(std::stoul(key.substr(first, i - first))) : 0u;

        std::map<uint32_t, std::string>& into = isName ? names : times;
        std::map<uint32_t, int>& lines = isName ? nameLine : timeLine;
        if (!into.emplace(number, value).second)
            THROW(ERR_COMMON, fileName << ": the " << (isName ? "name" : "time") << " of chapter " << number
                                       << " is given twice, on lines " << lines[number] << " and " << lineNo)
        lines[number] = lineNo;
    }

    std::vector<AVChapter> chapters;
    for (const auto& [number, time] : times)
    {
        if (time.empty())
            continue;
        const double seconds = chapterTimeToSeconds(time, fileName + ", line " + std::to_string(timeLine[number]));
        const int64_t start = static_cast<int64_t>(seconds * 1e9 + 0.5);
        const auto name = names.find(number);
        chapters.emplace_back(start, name != names.end() ? name->second : std::string(), std::string());
    }
    if (chapters.empty())
        THROW(ERR_COMMON, "No chapters found in " << fileName << ". A simple text chapter file has "
                                                  << "lines like CHAPTER01=00:00:00.000")
    return chapters;
}

std::vector<AVChapter> parseXml(const std::string& text, const std::string& fileName)
{
    // One edition, the one the Matroska reader takes from a file (matroskaDemuxer.cpp, readChapters): the first marked
    // EditionFlagDefault, or the first when none is, with its hidden chapters left out (RFC 9559, 20.1.2 and 20.2.5).
    // This took the first edition with its hidden chapters, so the same chapters gave other marks from a chapter file
    // than from the file they came from. A nested chapter is read as one more entry in a flat list: that is all a mux
    // can express.
    size_t scanFrom = 0;
    size_t scanTo = text.size();
    bool chosenDefault = false;
    for (size_t ed = text.find("<EditionEntry"); ed != std::string::npos; ed = text.find("<EditionEntry", ed + 1))
    {
        const size_t edEnd = text.find("</EditionEntry>", ed);
        const size_t end = edEnd == std::string::npos ? text.size() : edEnd;
        // The edition's own flag only: the one before its first chapter
        const size_t firstAtom = std::min(text.find("<ChapterAtom", ed), end);
        std::string flag;
        const bool isDefault = tagText(text, "EditionFlagDefault", ed, firstAtom, flag) && trimmed(flag) == "1";
        if (ed == text.find("<EditionEntry") || (isDefault && !chosenDefault))
        {
            scanFrom = ed;
            scanTo = end;
            chosenDefault = isDefault;
        }
        if (chosenDefault)
            break;
    }
    int hiddenLeftOut = 0;

    std::vector<AVChapter> chapters;
    size_t pos = scanFrom;
    while (true)
    {
        const size_t atom = text.find("<ChapterAtom", pos);
        if (atom == std::string::npos || atom >= scanTo)
            break;
        size_t atomEnd = text.find("<ChapterAtom", atom + 1);
        const size_t atomClose = text.find("</ChapterAtom>", atom);
        if (atomEnd == std::string::npos || (atomClose != std::string::npos && atomClose < atomEnd))
            atomEnd = atomClose == std::string::npos ? scanTo : atomClose;
        atomEnd = std::min(atomEnd, scanTo);
        pos = atom + 1;

        std::string timeStr;
        if (!tagText(text, "ChapterTimeStart", atom, atomEnd, timeStr))
            continue;
        if (std::string hidden; tagText(text, "ChapterFlagHidden", atom, atomEnd, hidden) && trimmed(hidden) == "1")
        {
            ++hiddenLeftOut;
            continue;
        }
        std::string name;
        std::string language;
        std::string display;
        if (tagText(text, "ChapterDisplay", atom, atomEnd, display))
        {
            std::string value;
            if (tagText(display, "ChapterString", 0, display.size(), value))
                name = unescapeXml(trimmed(value));
            if (tagText(display, "ChapterLanguage", 0, display.size(), value))
                language = chapterLanguage(trimmed(value), fileName);
        }
        const int64_t start = static_cast<int64_t>(chapterTimeToSeconds(timeStr, fileName) * 1e9 + 0.5);
        chapters.emplace_back(start, name, language);
    }
    if (chapters.empty() && hiddenLeftOut > 0)
        THROW(ERR_COMMON, "Every chapter of the edition used in " << fileName << " is hidden (ChapterFlagHidden), so "
                                                                  << "there is no chapter to show.")
    if (chapters.empty())
        THROW(ERR_COMMON, "No chapters found in " << fileName << ". A Matroska XML chapter file has a "
                                                  << "ChapterAtom with a ChapterTimeStart in it")
    return chapters;
}
}  // namespace

double chapterTimeToSeconds(const std::string& text, const std::string& where)
{
    // The reader below skips whatever it does not understand, so "abc" became a chapter at 0:00 and
    // "-00:00:05" one at 0:05, while a negative, endless or very large time vanished from the output
    // although it was counted among the chapters read. So every part between the colons has to start
    // with a digit, and there is no minus sign anywhere. Nor anything else but digits and one decimal point in the
    // seconds: "00:00:01,500" was read as 1 s and "00:00:05min" as 5 s, in silence.
    const std::string t = trimmed(text);
    if (t.empty())
        return 0.0;
    // The minutes and the seconds are below 60, as a cut time's are: 00:75:00 was read as 1:15:00, while
    // --cut-start refused it.
    bool ok = true;
    size_t partStart = 0;
    int partCount = 0;
    for (size_t i = 0; ok && i <= t.size(); ++i)
    {
        if (i == t.size() || t[i] == ':')
        {
            const std::string part = t.substr(partStart, i - partStart);
            const bool last = i == t.size();
            ok = !part.empty() && std::isdigit(static_cast<unsigned char>(part[0])) &&
                 std::count(part.begin(), part.end(), '.') <= (last ? 1 : 0);
            ++partCount;
            if (ok && partCount > 1 && strtod(part.c_str(), nullptr) >= 60.0)
                ok = false;
            if (partCount > 3)
                ok = false;
            partStart = i + 1;
        }
        else if (!std::isdigit(static_cast<unsigned char>(t[i])) && t[i] != '.')
            ok = false;
    }
    const double seconds = ok ? timeToFloat(t) : 0.0;
    if (!ok || !std::isfinite(seconds) || seconds < 0.0 || seconds > 100000.0 * 3600.0)
        THROW(ERR_COMMON, where << ": the chapter time \"" << t
                                << "\" is not a time from 0 to 100000 hours, written as HH:MM:SS.mmm")
    return seconds;
}

std::vector<AVChapter> readChapterFile(const std::string& fileName)
{
    std::string text = readWholeFile(fileName);

    // mkvextract writes a UTF-8 byte order mark, so the first character of a real Matroska XML file
    // is not the one it appears to be. Without this the XML goes to the text parser and comes back
    // as "no chapters found", which is a confident wrong answer about a perfectly good file.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    // UTF-16 is worth naming rather than failing to find chapters in. Several Windows tools write
    // it, and every search for text in such a file silently matches nothing.
    if (text.size() >= 2 &&
        ((static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) ||
         (static_cast<unsigned char>(text[0]) == 0xFE && static_cast<unsigned char>(text[1]) == 0xFF)))
        THROW(ERR_COMMON, "The chapter file " << fileName << " is UTF-16. Save it as UTF-8 or plain text.")

    const std::string head = trimmed(text);
    const bool looksLikeXml = !head.empty() && head[0] == '<';
    std::vector<AVChapter> chapters = looksLikeXml ? parseXml(text, fileName) : parseOgm(text, fileName);
    LTRACE(LT_INFO, 2,
           "Read " << chapters.size()
                   << (looksLikeXml ? " chapters from the Matroska XML file " : " chapters from the text file ")
                   << fileName);
    return chapters;
}

bool writeChapterXml(const std::string& fileName, const std::vector<AVChapter>& chapters)
{
    TextFile file;
    if (!file.open(fileName.c_str(), File::ofWrite))
        return false;
    bool ok = file.writeLine("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    ok = ok && file.writeLine("<Chapters>");
    ok = ok && file.writeLine("  <EditionEntry>");
    int number = 0;
    for (const auto& chapter : chapters)
    {
        ++number;
        const double seconds = static_cast<double>(chapter.start) / 1e9;
        std::string name = chapter.cTitle;
        if (name.empty())
            name = "Chapter " + int32ToStr(number);
        std::string language = chapter.language;
        if (language.empty())
            language = "und";
        ok = ok && file.writeLine("    <ChapterAtom>");
        ok = ok && file.writeLine("      <ChapterTimeStart>" + floatToTime(seconds, '.') + "000000</ChapterTimeStart>");
        ok = ok && file.writeLine("      <ChapterDisplay>");
        ok = ok && file.writeLine("        <ChapterString>" + escapeXml(name) + "</ChapterString>");
        ok = ok && file.writeLine("        <ChapterLanguage>" + language + "</ChapterLanguage>");
        ok = ok && file.writeLine("      </ChapterDisplay>");
        ok = ok && file.writeLine("    </ChapterAtom>");
        if (!ok)
            break;
    }
    ok = ok && file.writeLine("  </EditionEntry>");
    ok = ok && file.writeLine("</Chapters>");
    file.close();
    return ok;
}
