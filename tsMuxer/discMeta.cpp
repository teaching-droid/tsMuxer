#include "discMeta.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include <fs/file.h>
#include <fs/systemlog.h>
#include <types/types.h>

#include "utf8Converter.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
// A path for the calls of std::filesystem; on Windows a long one gets the long path prefix, as every file tsMuxeR opens
// does
std::filesystem::path pathOf(const std::string& utf8)
{
#ifdef _WIN32
    const std::string p = windowsPath(utf8);
#else
    const std::string& p = utf8;
#endif
    return {std::u8string(p.begin(), p.end())};
}

std::string utf8Of(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

// A path to take apart and put together, without the prefix: it is what the log shows and what File opens
std::filesystem::path plainPath(const std::string& utf8) { return {std::u8string(utf8.begin(), utf8.end())}; }

std::string joinPath(const std::string& dir, const std::string& name)
{
    return utf8Of(plainPath(dir) / plainPath(name));
}

std::string lower(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// The files and the folders in a folder, each by name in one order on every system: by name with the letters a-z taken
// as A-Z, then byte for byte, as the files of an image are ordered.
void listFolder(const std::string& dir, std::vector<std::string>& files, std::vector<std::string>& folders)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(pathOf(dir), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = utf8Of(it->path().filename());
        if (it->is_directory(ec))
            folders.push_back(name);
        else if (it->is_regular_file(ec))
            files.push_back(name);
    }
    const auto byName = [](const std::string& a, const std::string& b)
    {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    };
    std::sort(files.begin(), files.end(), byName);
    std::sort(folders.begin(), folders.end(), byName);
}

bool isBdmtName(const std::string& name)
{
    const std::string l = lower(name);
    return l.size() > 9 && l.compare(0, 5, "bdmt_") == 0 && l.compare(l.size() - 4, 4, ".xml") == 0;
}

// bdmt_ and three letters a-z, as every disc names its files: bdmt_eng.xml, bdmt_jpn.xml
bool isPlainBdmtName(const std::string& name)
{
    if (name.size() != 12 || name.compare(0, 5, "bdmt_") != 0 || name.compare(8, 4, ".xml") != 0)
        return false;
    return std::all_of(name.begin() + 5, name.begin() + 8, [](const char c) { return c >= 'a' && c <= 'z'; });
}

std::vector<uint8_t> readWhole(const std::string& path)
{
    File file;
    int64_t size = 0;
    if (!file.open(path.c_str(), File::ofRead) || !file.size(&size) || size < 0 || size > 64 * 1024 * 1024)
        THROW(ERR_COMMON, "Can't read " << path << " of the disc's title and pictures (BDMV/META/DL).")
    std::vector<uint8_t> data(static_cast<size_t>(size));
    size_t done = 0;
    while (done < data.size())
    {
        const int n = file.read(data.data() + done, static_cast<uint32_t>(data.size() - done));
        if (n <= 0)
            THROW(ERR_COMMON, "Can't read " << path << " of the disc's title and pictures (BDMV/META/DL).")
        done += static_cast<size_t>(n);
    }
    return data;
}

void appendUtf8(std::string& out, const uint32_t c)
{
    if (c < 0x80)
        out += static_cast<char>(c);
    else if (c < 0x800)
    {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if (c < 0x110000)
    {
        out += static_cast<char>(0xF0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The text of an element or an attribute with the five named references and the numbered ones read
std::string unescapeXml(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size();)
    {
        const size_t semi = text[i] == '&' ? text.find(';', i) : std::string::npos;
        if (semi == std::string::npos || semi - i > 10)
        {
            out += text[i++];
            continue;
        }
        const std::string ref = text.substr(i + 1, semi - i - 1);
        if (ref == "amp")
            out += '&';
        else if (ref == "lt")
            out += '<';
        else if (ref == "gt")
            out += '>';
        else if (ref == "quot")
            out += '"';
        else if (ref == "apos")
            out += '\'';
        else if (ref.size() > 1 && ref[0] == '#')
        {
            const bool hex = ref[1] == 'x' || ref[1] == 'X';
            const std::string digits = ref.substr(hex ? 2 : 1);
            if (digits.empty() ||
                digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos)
            {
                out += text[i++];
                continue;
            }
            appendUtf8(out, static_cast<uint32_t>(std::stoul(digits, nullptr, hex ? 16 : 10)));
        }
        else
        {
            out += text[i++];
            continue;
        }
        i = semi + 1;
    }
    return out;
}

// What a bdmt file says, read as plainly as the files on the discs are written: the first name element is the title,
// and each thumbnail element names a picture in its href attribute. The namespace prefix (di: on every disc seen) is
// not looked at.
struct Bdmt
{
    std::string title;
    std::vector<std::string> pictures;
    bool validUtf8 = true;
};

Bdmt parseBdmt(const std::vector<uint8_t>& data)
{
    Bdmt rez;
    std::string text(data.begin(), data.end());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    std::string check = text;
    rez.validUtf8 = !UtfConverter::makeValidUtf8(check);
    for (size_t pos = text.find('<'); pos != std::string::npos; pos = text.find('<', pos + 1))
    {
        const size_t close = text.find('>', pos);
        if (close == std::string::npos)
            break;
        const std::string tag = text.substr(pos + 1, close - pos - 1);
        if (tag.empty() || tag[0] == '/' || tag[0] == '?' || tag[0] == '!')
            continue;
        std::string name = tag.substr(0, tag.find_first_of(" \t\r\n/"));
        if (const size_t colon = name.find(':'); colon != std::string::npos)
            name.erase(0, colon + 1);
        if (name == "name" && rez.title.empty())
        {
            const size_t end = text.find('<', close);
            rez.title = trimStr(unescapeXml(text.substr(close + 1, end - close - 1)));
        }
        else if (name == "thumbnail")
        {
            for (size_t h = tag.find("href"); h != std::string::npos; h = tag.find("href", h + 4))
            {
                size_t q = tag.find_first_not_of(" \t\r\n", h + 4);
                if (q == std::string::npos || tag[q] != '=')
                    continue;
                q = tag.find_first_not_of(" \t\r\n", q + 1);
                if (q == std::string::npos || (tag[q] != '"' && tag[q] != '\''))
                    continue;
                const size_t endQ = tag.find(tag[q], q + 1);
                if (endQ != std::string::npos)
                    rez.pictures.push_back(unescapeXml(tag.substr(q + 1, endQ - q - 1)));
                break;
            }
        }
    }
    return rez;
}

// The library in one folder. given: a folder the user named, whose faults are said; a source disc's folder is taken
// as the disc has it, and only what is left out is said.
DiscMeta readFolder(const std::string& dir, const bool given)
{
    DiscMeta meta;
    meta.folder = dir;
    std::vector<std::string> names, folders;
    listFolder(dir, names, folders);

    std::set<std::string> kept;
    for (const auto& name : names)
    {
        if (!isBdmtName(name))
            continue;
        kept.insert(name);
        const Bdmt bdmt = parseBdmt(readWhole(joinPath(dir, name)));
        const std::string lang = lower(name.substr(5, name.size() - 9));
        std::string shown = bdmt.title;  // a title that is not UTF-8 is shown with U+FFFD in the log, not copied raw
        UtfConverter::makeValidUtf8(shown);
        meta.titles.emplace_back(lang, shown);
        if (given)
        {
            if (!isPlainBdmtName(name))
                LTRACE(LT_WARN, 2,
                       "Warning: --meta-dir: " << name
                                               << " is not named as a disc names these files, bdmt_ and a language "
                                                  "of three small letters (bdmt_eng.xml). A player may not read it.");
            if (!bdmt.validUtf8)
                LTRACE(LT_WARN, 2,
                       "Warning: --meta-dir: " << name
                                               << " is not in UTF-8, which these files are written in. A "
                                                  "player may show its title wrong.");
            if (bdmt.title.empty())
                LTRACE(LT_WARN, 2,
                       "Warning: --meta-dir: " << name
                                               << " gives no title (no name element). A player shows the disc "
                                                  "without one.");
        }
        for (std::string picture : bdmt.pictures)
        {
            if (picture.compare(0, 2, "./") == 0)
                picture.erase(0, 2);
            if (picture.find_first_of("/\\") != std::string::npos)
            {
                if (given)
                    LTRACE(LT_WARN, 2,
                           "Warning: --meta-dir: " << name << " names the picture " << picture
                                                   << " in another folder. Only the files of the folder itself are "
                                                      "copied: put the picture beside it and name it alone.");
                continue;
            }
            // Named as the folder has it, whatever case the bdmt file writes: a disc made on Windows does not keep
            // the two the same, and a Linux folder tells them apart.
            const auto found = std::find_if(names.begin(), names.end(),
                                            [&](const std::string& n) { return lower(n) == lower(picture); });
            if (found == names.end())
            {
                if (given)
                    LTRACE(LT_WARN, 2,
                           "Warning: --meta-dir: " << name << " names the picture " << picture
                                                   << ", which is not in the folder. A player shows no picture for "
                                                      "it.");
                continue;
            }
            if (given && kept.count(*found) == 0)
            {
                const std::vector<uint8_t> head = readWhole(joinPath(dir, *found));
                if (head.size() < 3 || head[0] != 0xFF || head[1] != 0xD8 || head[2] != 0xFF)
                    LTRACE(LT_WARN, 2,
                           "Warning: --meta-dir: the picture " << *found
                                                               << " is not a JPEG picture, as the pictures of a disc "
                                                                  "are. A player may not show it.");
            }
            kept.insert(*found);
        }
    }
    if (kept.empty())
        return meta;

    std::vector<std::string> leftOut;
    for (const auto& name : names)
        if (kept.count(name))
            meta.files.push_back(name);
        else
            leftOut.push_back(name);
    for (const auto& f : folders) leftOut.push_back(f + "/");
    if (!leftOut.empty())
    {
        std::string list;
        for (const auto& name : leftOut) list += (list.empty() ? "" : ", ") + name;
        LTRACE(LT_INFO, 2,
               "The disc's title and pictures: left out of " << dir << ", as no bdmt file names them: " << list);
    }
    return meta;
}
}  // namespace

DiscMeta readGivenDiscMeta(const std::string& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(pathOf(dir), ec))
        THROW(ERR_COMMON, "--meta-dir: the folder " << dir << " is not there.")
    for (const char* sub : {"", "DL", "META/DL", "BDMV/META/DL"})
    {
        const std::filesystem::path p = *sub ? plainPath(dir) / std::filesystem::path(sub) : plainPath(dir);
        if (!std::filesystem::is_directory(pathOf(utf8Of(p)), ec))
            continue;
        std::vector<std::string> names, folders;
        listFolder(utf8Of(p), names, folders);
        if (std::any_of(names.begin(), names.end(), isBdmtName))
            return readFolder(utf8Of(p), true);
    }
    THROW(ERR_COMMON, "--meta-dir: there is no bdmt_<lang>.xml file in "
                          << dir
                          << ", nor in a DL or META/DL folder in it. A disc keeps its title in BDMV/META/DL, in "
                             "bdmt_eng.xml and the like, beside the pictures those files name.")
}

DiscMeta discMetaOfSource(const std::string& sourceFile)
{
    std::filesystem::path dir = plainPath(sourceFile).parent_path();
    const auto named = [](const std::filesystem::path& p, const char* name)
    { return strToUpperCase(utf8Of(p.filename())) == name; };
    if (named(dir, "SSIF"))
        dir = dir.parent_path();
    if (!named(dir, "STREAM") || !named(dir.parent_path(), "BDMV"))
        return {};
    // META and DL as the disc names them; a copy on a system that tells cases apart may have changed them
    std::filesystem::path dl = dir.parent_path();
    for (const char* part : {"META", "DL"})
    {
        std::error_code ec;
        std::filesystem::path next;
        for (std::filesystem::directory_iterator it(pathOf(utf8Of(dl)), ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && strToUpperCase(utf8Of(it->path().filename())) == part)
            {
                next = dl / it->path().filename();
                break;
            }
        if (next.empty())
            return {};
        dl = next;
    }
    return readFolder(utf8Of(dl), false);
}

std::string describeDiscMeta(const DiscMeta& meta)
{
    std::string titles;
    for (const auto& [lang, title] : meta.titles)
        titles += (titles.empty() ? "" : ", ") + (title.empty() ? std::string("no title") : '"' + title + '"') + " (" +
                  lang + ")";
    return titles + ", " + std::to_string(meta.files.size()) + (meta.files.size() == 1 ? " file" : " files") +
           " from " + meta.folder;
}

std::vector<uint8_t> readDiscMetaFile(const DiscMeta& meta, const std::string& name)
{
    return readWhole(joinPath(meta.folder, name));
}
