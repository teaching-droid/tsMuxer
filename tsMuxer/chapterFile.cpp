#include "chapterFile.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cctype>

#include "fs/textfile.h"
#include "vodCoreException.h"
#include "vod_common.h"

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
    std::string text;
    std::string line;
    while (file.readLine(line))
    {
        text += line;
        text += '\n';
    }
    file.close();
    return text;
}

// CHAPTER01=00:00:00.000 and CHAPTER01NAME=Opening, in any order, any number of digits, any case.
std::vector<AVChapter> parseOgm(const std::string& text, const std::string& fileName)
{
    std::vector<std::string> times;
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t nl = text.find('\n', pos);
        const std::string line = trimmed(text.substr(pos, nl == std::string::npos ? nl : nl - pos));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = upperCase(trimmed(line.substr(0, eq)));
        const std::string value = trimmed(line.substr(eq + 1));
        if (key.compare(0, 7, "CHAPTER") != 0)
            continue;

        // CHAPTER<digits> or CHAPTER<digits>NAME. The number only orders the file, so it is used as
        // an index rather than trusted to be dense: a list starting at 00 and one starting at 01
        // both have to work.
        size_t i = 7;
        while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) ++i;
        if (i == 7)
            continue;
        const int index = strToInt32(key.substr(7, i - 7).c_str());
        const bool isName = key.compare(i, std::string::npos, "NAME") == 0;
        if (!isName && i != key.size())
            continue;

        std::vector<std::string>& into = isName ? names : times;
        if (static_cast<size_t>(index) >= into.size())
            into.resize(static_cast<size_t>(index) + 1);
        into[static_cast<size_t>(index)] = value;
    }

    std::vector<AVChapter> chapters;
    for (size_t i = 0; i < times.size(); ++i)
    {
        if (times[i].empty())
            continue;
        const int64_t start = static_cast<int64_t>(timeToFloat(times[i]) * 1e9 + 0.5);
        chapters.emplace_back(start, i < names.size() ? names[i] : std::string(), std::string());
    }
    if (chapters.empty())
        THROW(ERR_COMMON, "No chapters found in " << fileName << ". A simple text chapter file has "
                                                  << "lines like CHAPTER01=00:00:00.000")
    return chapters;
}

std::vector<AVChapter> parseXml(const std::string& text, const std::string& fileName)
{
    // Only the first edition, which is what the Matroska reader here does as well. A nested chapter
    // is read as one more entry in a flat list: that is all a mux can express.
    size_t scanFrom = 0;
    size_t scanTo = text.size();
    const size_t ed = text.find("<EditionEntry");
    if (ed != std::string::npos)
    {
        scanFrom = ed;
        const size_t edEnd = text.find("</EditionEntry>", ed);
        if (edEnd != std::string::npos)
            scanTo = edEnd;
    }

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
        std::string name;
        std::string language;
        std::string display;
        if (tagText(text, "ChapterDisplay", atom, atomEnd, display))
        {
            std::string value;
            if (tagText(display, "ChapterString", 0, display.size(), value))
                name = unescapeXml(trimmed(value));
            if (tagText(display, "ChapterLanguage", 0, display.size(), value))
                language = trimmed(value);
        }
        const int64_t start = static_cast<int64_t>(timeToFloat(trimmed(timeStr)) * 1e9 + 0.5);
        chapters.emplace_back(start, name, language);
    }
    if (chapters.empty())
        THROW(ERR_COMMON, "No chapters found in " << fileName << ". A Matroska XML chapter file has a "
                                                  << "ChapterAtom with a ChapterTimeStart in it")
    return chapters;
}
}  // namespace

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
