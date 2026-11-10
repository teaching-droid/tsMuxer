// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "diagnosticReport.h"

#include <fs/file.h>
#include <fs/systemlog.h>
#include <types/types.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "vod_common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace diagReport
{
bool active = false;
bool framesActive = false;

namespace
{
std::FILE* out = nullptr;
std::FILE* framesOut = nullptr;
std::string reportName;
bool framesAsked = false;
std::string framesName;
double framesFrom = -1;  // the range of --report-frames, in seconds of the output; -1 is open
double framesTo = -1;
int64_t firstFrameTs = INT64_MIN;
std::recursive_mutex lock;  // lines come from the reader threads too
bool keepPaths = false;
bool keepNames = false;
bool listing = false;
// the start of a line said with LTRACE2, kept for the line that ends it, on the thread that says both
thread_local std::string pending;
int lastTenth = -1;
std::chrono::steady_clock::time_point started;
std::streambuf* consoleOut = nullptr;  // the console as it was: a line said while it is turned away was not shown
std::streambuf* consoleErr = nullptr;
std::vector<std::string> folders;  // the folders left out of paths, longest first
std::vector<std::string> sources;
std::string metaName;
std::string outputName;
bool inMessages = false;   // the last section written is that of the messages
bool hadMessages = false;  // a section of messages was written before

// A file whose name is replaced by a placeholder unless --report-keep-names is given: "Der Film.mkv" is
// "<source1>.mkv", and the name without its extension, which a demux or a split builds its names from, "<source1>"
struct NamedFile
{
    std::string name;         // without its folder
    std::string stem;         // without its extension either
    std::string placeholder;  // for the stem: "<source1>"
    std::string path;         // as given, for the section of the names
    bool source = false;      // a source: the section says what tsMuxeR takes from its name
};
std::vector<NamedFile> namedFiles;  // longest name first
// A title, a track name or a chapter name as a file holds it, and its placeholder; longest first
std::vector<std::pair<std::string, std::string>> namedTexts;
int sourceCount = 0;
int discCount = 0;
int otherCount = 0;

#ifdef _WIN32
constexpr char LINE_END[] = "\r\n";  // the Notepad of Windows 7 shows a file of bare line feeds as one line
#else
constexpr char LINE_END[] = "\n";
#endif

std::filesystem::path fsPath(const std::string& utf8)
{
#ifdef _WIN32
    const std::string p = windowsPath(utf8);
#else
    const std::string& p = utf8;
#endif
    return {std::u8string(p.begin(), p.end())};
}

std::string utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

bool isSep(const char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool sameChar(const char a, const char b)
{
#ifdef _WIN32
    // paths on Windows match whatever the case and whichever slash
    if (isSep(a) && isSep(b))
        return true;
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
#else
    return a == b;
#endif
}

bool folderAt(const std::string& line, const size_t pos, const std::string& folder)
{
    if (pos + folder.size() > line.size())
        return false;
    for (size_t i = 0; i < folder.size(); ++i)
        if (!sameChar(line[pos + i], folder[i]))
            return false;
    return true;
}

// A path can begin at this place of a line: after a space, a quote or the like, so a name that merely ends like a
// folder or a file keeps its letters
bool pathStart(const std::string& line, const size_t i)
{
    const unsigned char before = i == 0 ? ' ' : static_cast<unsigned char>(line[i - 1]);
    return !std::isalnum(before) && before != '_' && before != '-' && before != '.' && before < 0x80;
}

// A folder on a network share: \\server\share\films, or //server/share/films on Windows
bool networkPath(const std::string& line, const size_t i)
{
#ifdef _WIN32
    return i + 2 < line.size() && isSep(line[i]) && isSep(line[i + 1]) && line[i] == line[i + 1] &&
           !isSep(line[i + 2]) && (line[i] == '\\' || i == 0 || line[i - 1] != ':');  // not the // of a web address
#else
    return false;
#endif
}

// The length of a drive at the start of a path, a letter, a colon and a slash ("X:/" or "X:\"), or 0
size_t driveAt(const std::string& line, const size_t i)
{
#ifdef _WIN32
    if (i + 2 < line.size() && std::isalpha(static_cast<unsigned char>(line[i])) && line[i + 1] == ':' &&
        isSep(line[i + 2]))
        return 3;
#endif
    return 0;
}

// A path in a line has its folder left out: "D:/films/a.ts" is "a.ts", and the folder alone is "<folder>". What is left
// still tells a local drive from a network share: "D:/a.ts" is "<drive>/a.ts", "\\server\films\a.ts" is
// "<network share>/a.ts".
std::string shortenPaths(const std::string& line)
{
    if (keepPaths)
        return line;
    std::string r;
    r.reserve(line.size());
    for (size_t i = 0; i < line.size();)
    {
        bool hit = false;
        if (pathStart(line, i))
            for (const std::string& f : folders)
            {
                if (!folderAt(line, i, f))
                    continue;
                const size_t end = i + f.size();
                if (end < line.size() && isSep(line[end]))
                {
                    if (networkPath(f, 0))
                        r += "<network share>/";
                    i = end + 1;
                    hit = true;
                    break;
                }
                // the folder alone, at the end of the line or before a mark; a full stop only where the sentence ends
                // there, so that "D:/films.old" keeps its letters
                const bool fullStop = line[end] == '.' && (end + 1 == line.size() || line[end + 1] == ' ');
                if (end == line.size() || fullStop || std::strchr(" 	\"',;:)]>!?", line[end]) != nullptr)
                {
                    r += "<folder>";
                    i = end;
                    hit = true;
                    break;
                }
            }
        if (!hit && pathStart(line, i))
        {
            if (const size_t drive = driveAt(line, i); drive > 0)
            {
                r += "<drive>/";
                i += drive;
                // folders of a path tsMuxeR was not given as a file, as of an option's value: "<folder>/" for them all
                bool anyFolder = false;
                for (size_t k = i; k < line.size();)
                {
                    size_t e = k;
                    while (e < line.size() && !isSep(line[e]) && line[e] != '"' && line[e] != ',' && line[e] != ';')
                        ++e;
                    if (e >= line.size() || !isSep(line[e]) || e == k)
                        break;
                    anyFolder = true;
                    k = e + 1;
                    i = k;
                }
                if (anyFolder)
                    r += "<folder>/";
                hit = true;
            }
            else if (networkPath(line, i))
            {
                // a share no folder of the run is on: its server and share are left out as well
                size_t end = i + 2;
                for (int part = 0; part < 2 && end < line.size(); ++part)
                {
                    while (end < line.size() && !isSep(line[end]) && line[end] != '"' && line[end] != ' ') ++end;
                    if (part == 0 && end < line.size() && isSep(line[end]))
                        ++end;
                }
                r += "<network share>";
                i = end;
                hit = true;
            }
        }
        if (!hit)
            r += line[i++];
    }
    return r;
}

bool nameChar(const unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c >= 0x80; }

// The names of files and the titles and names the files hold, replaced by their placeholders unless
// --report-keep-names is given. A name is matched only as a whole: where a path or a word can begin, and up to where
// it ends; the name of a file without its extension only before a full stop and a letter or digit, as in the names a
// demux or a split builds from it ("<source1>.track_4113.ac3").
std::string replaceNames(const std::string& line)
{
    if (keepNames || (namedFiles.empty() && namedTexts.empty()))
        return line;
    std::string r;
    r.reserve(line.size());
    for (size_t i = 0; i < line.size();)
    {
        bool hit = false;
        if (pathStart(line, i))
        {
            for (const NamedFile& f : namedFiles)
            {
                if (folderAt(line, i, f.name) && (i + f.name.size() == line.size() ||
                                                  !nameChar(static_cast<unsigned char>(line[i + f.name.size()]))))
                {
                    r += f.placeholder + f.name.substr(f.stem.size());
                    i += f.name.size();
                    hit = true;
                    break;
                }
                const size_t end = i + f.stem.size();
                if (f.stem.size() < f.name.size() && folderAt(line, i, f.stem) && end + 1 < line.size() &&
                    line[end] == '.' && std::isalnum(static_cast<unsigned char>(line[end + 1])))
                {
                    r += f.placeholder;
                    i = end;
                    hit = true;
                    break;
                }
            }
            if (!hit)
                for (const auto& [text, placeholder] : namedTexts)
                {
                    const size_t end = i + text.size();
                    // Replaced where it is quoted, or where it cannot be a word of tsMuxeR's own: with a space or a
                    // letter beyond ASCII, or 10 letters or more. A track named "AC3" or "English" replaced those
                    // words in every line, "Stream type: AC3" among them; its own lines (srcname=, Stream name:)
                    // are replaced by redactFields.
                    const bool quoted = i > 0 && line[i - 1] == '"' && end < line.size() && line[end] == '"';
                    const bool ownWord = text.size() < 10 && text.find(' ') == std::string::npos &&
                                         std::all_of(text.begin(), text.end(),
                                                     [](const char c) { return static_cast<unsigned char>(c) < 0x80; });
                    if ((quoted || !ownWord) && line.compare(i, text.size(), text) == 0 &&
                        (end == line.size() || !nameChar(static_cast<unsigned char>(line[end]))))
                    {
                        r += placeholder;
                        i = end;
                        hit = true;
                        break;
                    }
                }
        }
        if (!hit)
            r += line[i++];
    }
    return r;
}

// What a placeholder keeps of a name, which problems with names depend on: its length, and whether it is plain ASCII
std::string nameInfo(const std::string& text)
{
    size_t chars = 0;
    bool ascii = true;
    for (const unsigned char c : text)
    {
        chars += (c & 0xC0) != 0x80 ? 1 : 0;
        ascii = ascii && c < 0x80;
    }
    std::string s = std::to_string(chars) + (chars == 1 ? " character" : " characters");
    if (!ascii)
        s += " (" + std::to_string(text.size()) + " bytes), non-ASCII";
    return s;
}

std::string textPlaceholder(const std::string& kind, const std::string& text)
{
    return "<" + kind + ", " + nameInfo(text) + ">";
}

// The values that name a track or a title wherever a line gives them, also one no file has said before: the "Stream
// name" of a listing, srcname= and track-name= of a track, --title= of the output
std::string redactFields(const std::string& line)
{
    if (keepNames)
        return line;
    std::string r = line;
    static const std::string streamName = "Stream name: ";
    if (const size_t p = r.find(streamName); p != std::string::npos && r.find_first_not_of(' ') == p)
    {
        const size_t v = p + streamName.size();
        if (v < r.size() && r[v] != '<')
            r = r.substr(0, v) + textPlaceholder("track name", r.substr(v));
    }
    for (const auto& [key, kind] : {std::pair<std::string, std::string>{"srcname=", "track name"},
                                    {"track-name=", "track name"},
                                    {"--title=", "title"}})
        for (size_t p = r.find(key); p != std::string::npos; p = r.find(key, p + 1))
        {
            const char before = p == 0 ? ' ' : r[p - 1];
            if (before != ' ' && before != ',' && before != '\t' && before != '"')
                continue;
            size_t v = p + key.size();
            size_t end;
            if (v < r.size() && r[v] == '"')
                end = r.find('"', ++v);  // track-name="..."
            else if (before == '"')
                end = r.find('"', v);  // "--title=a b" as the command line quotes it
            else if (key == "srcname=")
            {
                // up to the next ", key=" of the track: a name may hold a comma
                end = std::string::npos;
                for (size_t c = r.find(", ", v); c != std::string::npos && end == std::string::npos;
                     c = r.find(", ", c + 1))
                {
                    size_t k = c + 2;
                    while (k < r.size() && (std::islower(static_cast<unsigned char>(r[k])) || r[k] == '-')) ++k;
                    if (k > c + 2 && k < r.size() && r[k] == '=')
                        end = c;
                }
            }
            else
                end = r.find(key == "--title=" ? ' ' : ',', v);
            if (end == std::string::npos)
                end = r.size();
            if (end > v && r[v] != '<')
                r.replace(v, end - v, textPlaceholder(kind, r.substr(v, end - v)));
        }
    return r;
}

void addFolder(std::string f)
{
    while (f.size() > 1 && isSep(f.back())) f.pop_back();
    // A root, "/" or "C:", says nothing of the computer, and left out it would take the start of every path
    if (f.size() <= 3 || f == "." || f == "..")
        return;
    for (const std::string& known : folders)
        if (known.size() == f.size() && folderAt(known, 0, f))
            return;
    folders.push_back(f);
    std::sort(folders.begin(), folders.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
}

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> pathParts(const std::string& path)
{
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path)
        if (isSep(c))
        {
            if (!part.empty())
                parts.push_back(part);
            part.clear();
        }
        else
            part += c;
    if (!part.empty())
        parts.push_back(part);
    return parts;
}

std::vector<NamedFile> filesInOrder;  // for the section of the names

// A file of the run, whose name is replaced by a placeholder. A file of a disc, under its BDMV or VIDEO_TS folder,
// keeps its name, which says nothing of anyone, and the disc's folder is named instead; so is a disc folder or image
// given as a source.
void addFile(const std::string& path, const std::string& kind)
{
    if (keepNames || path.empty())
        return;
    const std::vector<std::string> parts = pathParts(path);
    std::string name;
    bool disc = false;
    bool discFolder = false;  // the name is that of the folder above BDMV or VIDEO_TS
    for (size_t k = 1; k < parts.size() && !disc; ++k)
        if (upper(parts[k]) == "BDMV" || upper(parts[k]) == "VIDEO_TS")
        {
            name = parts[k - 1];
            disc = discFolder = true;
        }
    std::error_code ec;
    const bool folder = std::filesystem::is_directory(fsPath(path), ec);
    if (!disc && !parts.empty())
    {
        name = parts.back();
        const std::string ext = name.size() > 4 ? upper(name.substr(name.size() - 4)) : std::string();
        disc = kind == "source" && (folder || ext == ".ISO" || ext == ".IMG" || ext == ".UDF");
    }
    if (name.empty() || name == "." || name == ".." || (name.size() == 2 && name[1] == ':'))
        return;
    for (const NamedFile& f : namedFiles)
        if (f.name.size() == name.size() && folderAt(f.name, 0, name))
            return;
    NamedFile f;
    f.name = name;
    f.path = path;
    f.source = kind == "source" && !disc;
    const size_t dot = name.rfind('.');
    // a folder has no extension: "My.Disc.2019" is one name
    f.stem = (folder && disc) || discFolder || dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    if (kind == "source")
        f.placeholder =
            disc ? "<disc" + std::to_string(++discCount) + ">" : "<source" + std::to_string(++sourceCount) + ">";
    else if (kind == "file")
        f.placeholder = "<file" + std::to_string(++otherCount) + ">";
    else
        f.placeholder = "<" + kind + ">";
    filesInOrder.push_back(f);
    namedFiles.push_back(f);
    std::sort(namedFiles.begin(), namedFiles.end(),
              [](const NamedFile& a, const NamedFile& b) { return a.name.size() > b.name.size(); });
}

// The values of track-name= and --title= of a line of a meta file, quoted or not
void addMetaNames(const std::string& line)
{
    for (const auto& [key, kind] :
         {std::pair<std::string, std::string>{"track-name=", "track name"}, {"--title=", "title"}})
        for (size_t p = line.find(key); p != std::string::npos; p = line.find(key, p + 1))
        {
            size_t v = p + key.size();
            const size_t end =
                v < line.size() && line[v] == '"' ? line.find('"', ++v) : line.find(key == "--title=" ? ' ' : ',', v);
            addName(line.substr(v, end == std::string::npos ? std::string::npos : end - v), kind);
        }
}

void writeLine(const std::string& line)
{
    if (out == nullptr)
        return;
    const std::string s = redactFields(replaceNames(shortenPaths(line)));
    std::fwrite(s.data(), 1, s.size(), out);
    std::fputs(LINE_END, out);
    std::fflush(out);  // a run that crashes keeps every line written so far
}

double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }

void record(const unsigned level, const bool onScreen, const std::string& text)
{
    if (!inMessages)
    {
        // a section of its own, the tracks or a listing, came in between
        writeLine("");
        writeLine(hadMessages ? "== Messages, continued" : "== Messages");
        inMessages = hadMessages = true;
    }
    char tag = level <= LT_ERROR ? 'E' : level == LT_WARN ? 'W' : level == LT_INFO ? 'I' : 'D';
    if (!onScreen)
        tag = static_cast<char>(std::tolower(tag));
    char head[32];
    std::snprintf(head, sizeof(head), "%9.3f %c  ", seconds(), tag);
    std::istringstream lines(text);
    std::string line;
    bool first = true;
    while (std::getline(lines, line) || first)
    {
        writeLine((first ? std::string(head) : std::string(std::strlen(head), ' ')) + line);
        first = false;
    }
}

std::string localTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[64];
    // without the offset from UTC, which would tell where in the world the report was made
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string systemText()
{
    std::ostringstream s;
#ifdef _WIN32
    // GetVersionEx gives the version the program was built for; RtlGetVersion the one it runs on
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
              : nullptr;
    if (rtlGetVersion && rtlGetVersion(&v) == 0)
        s << "Windows " << v.dwMajorVersion << '.' << v.dwMinorVersion << '.' << v.dwBuildNumber;
    else
        s << "Windows";
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64)
        s << ", a 32-bit program on 64-bit Windows";
    s << ", code page " << GetACP() << ", console code page " << GetConsoleOutputCP();
#else
    utsname u{};
    if (uname(&u) == 0)
        s << u.sysname << ' ' << u.release << ' ' << u.machine;
#ifdef __APPLE__
    char product[64] = {};
    size_t len = sizeof(product);
    if (sysctlbyname("kern.osproductversion", product, &len, nullptr, 0) == 0)
        s << ", macOS " << product;
#else
    if (std::FILE* f = std::fopen("/etc/os-release", "r"))
    {
        char line[256];
        while (std::fgets(line, sizeof(line), f))
            if (std::strncmp(line, "PRETTY_NAME=", 12) == 0)
            {
                std::string name = line + 12;
                name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '"' || c == '\n'; }),
                           name.end());
                s << ", " << name;
            }
        std::fclose(f);
    }
#endif
    for (const char* var : {"LC_ALL", "LANG"})
        if (const char* value = std::getenv(var); value && *value)
        {
            s << ", " << var << '=' << value;
            break;
        }
#endif
    return s.str();
}

std::string memoryText()
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(1);
    constexpr double GIB = 1024.0 * 1024.0 * 1024.0;
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m))
        s << static_cast<double>(m.ullTotalPhys) / GIB << " GiB, " << static_cast<double>(m.ullAvailPhys) / GIB
          << " GiB free";
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        s << static_cast<double>(pages) * static_cast<double>(pageSize) / GIB << " GiB";
#endif
    return s.str();
}

std::string buildText()
{
    std::ostringstream s;
#if defined(_MSC_VER)
    s << "MSVC " << _MSC_FULL_VER;
#elif defined(__clang__)
    s << "Clang " << __clang_version__;
#elif defined(__GNUC__)
    s << "GCC " << __VERSION__;
#endif
    s << ", " << sizeof(void*) * 8 << "-bit";
#ifdef NDEBUG
    s << ", release";
#else
    s << ", debug";
#endif
    return s.str();
}

std::string quoted(const std::string& arg)
{
    return arg.find_first_of(" \t\"") == std::string::npos && !arg.empty() ? arg : "\"" + arg + "\"";
}

// The names of the sources on the lines of a meta file: the second field, one name or several joined with "+", each
// in quotes or not. Every quoted text with a slash names a path too (a chapter file, a font) and has its folder left
// out.
void readMeta(const std::string& text)
{
    std::istringstream lines(text);
    std::string line;
    std::vector<std::string> otherFiles;  // named after the sources, which are numbered first
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        addMetaNames(line);
        // an unquoted value of a key that names a file or a folder: --chapters-file=, fonts-dir=, --meta-dir= ...
        for (size_t eq = line.find('='); eq != std::string::npos; eq = line.find('=', eq + 1))
        {
            if (eq + 1 >= line.size() || line[eq + 1] == '"')
                continue;
            const size_t end = line.find_first_of(" ,", eq + 1);
            const std::string value = line.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
            if (value.find_first_of("/\\") != std::string::npos)
            {
                addPath(value);
                otherFiles.push_back(value);
            }
        }
        for (size_t q = line.find('"'); q != std::string::npos;)
        {
            const size_t e = line.find('"', q + 1);
            if (e == std::string::npos)
                break;
            const bool nameValue =
                strEndWith(line.substr(0, q), "track-name=") || strEndWith(line.substr(0, q), "title=");
            if (const std::string s = line.substr(q + 1, e - q - 1);
                !nameValue && s.find_first_of("/\\") != std::string::npos)
            {
                addPath(s);
                otherFiles.push_back(s);
            }
            q = line.find('"', e + 1);
        }
        const std::string t = trimStr(line);
        if (t.empty() || t[0] == '#' || strStartWith(t, "MUXOPT"))
            continue;
        const size_t comma = t.find(',');
        if (comma == std::string::npos)
            continue;
        std::string rest = trimStr(t.substr(comma + 1));
        while (!rest.empty())
        {
            std::string name;
            if (rest[0] == '"')
            {
                const size_t e = rest.find('"', 1);
                if (e == std::string::npos)
                    break;
                name = rest.substr(1, e - 1);
                rest = trimStr(rest.substr(e + 1));
            }
            else
            {
                const size_t e = rest.find_first_of(",+");
                name = trimStr(rest.substr(0, e));
                rest = e == std::string::npos ? std::string() : trimStr(rest.substr(e));
            }
            if (!name.empty() && std::find(sources.begin(), sources.end(), name) == sources.end())
            {
                sources.push_back(name);
                addPath(name);
                addFile(name, "source");
            }
            if (rest.empty() || rest[0] != '+')
                break;
            rest = trimStr(rest.substr(1));
        }
    }
    for (const std::string& f : otherFiles) addFile(f, "file");
}

std::string hexBytes(const uint8_t* data, const int n)
{
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (int i = 0; i < n; ++i) s << (i ? " " : "") << std::setw(2) << static_cast<int>(data[i]);
    return s.str();
}

// What a source is: its size and its first bytes, which tell the kind of file where its name does not. A file inside
// a disc image is opened as the mux opens it.
void describeSource(const std::string& name)
{
    std::error_code ec;
    if (std::filesystem::is_directory(fsPath(name), ec))
    {
        text(name + ": a folder");
        return;
    }
    File file;
    if (!file.open(name.c_str(), File::ofRead))
    {
        text(name + ": cannot be opened");
        return;
    }
    const int64_t size = file.size();
    uint8_t head[32] = {};
    const int n = file.read(head, sizeof(head));
    file.close();
    text(name + ": " + std::to_string(size) + " bytes, starts " + (n > 0 ? hexBytes(head, n) : "with nothing"));
}

std::string exitText(const int code)
{
    switch (code)
    {
    case 0:
        return "0, the run ended well";
    case -1:
        return "-1, an error";
    case -2:
        return "-2, an error (or a stop)";
    case -3:
        return "-3, a stream that could not be read";
    case -4:
        return "-4, an error of an unknown kind";
    default:
        return std::to_string(code);
    }
}

#ifdef _WIN32
LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
{
    if (out != nullptr && info != nullptr && info->ExceptionRecord != nullptr)
    {
        std::fprintf(out, "%s== Crash%s  tsMuxeR stopped at %.3f s with exception 0x%08lX.%s", LINE_END, LINE_END,
                     seconds(), static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), LINE_END);
        std::fflush(out);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// Only what is safe in a signal handler: the lines before are written already
void writeSignalNote(const char* title, const int sig)
{
    if (out == nullptr)
        return;
    char msg[128];
    const int n = std::snprintf(msg, sizeof(msg), "%s== %s%s  tsMuxeR stopped with signal %d.%s", LINE_END, title,
                                LINE_END, sig, LINE_END);
    if (n <= 0)
        return;
#ifdef _WIN32
    std::fwrite(msg, 1, static_cast<size_t>(n), out);
    std::fflush(out);
#else
    [[maybe_unused]] const ssize_t w = ::write(fileno(out), msg, static_cast<size_t>(n));
#endif
}

void onCrashSignal(const int sig)
{
    writeSignalNote("Crash", sig);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
}  // namespace

void addPath(const std::string& path)
{
    if (path.empty())
        return;
    const auto sep = std::find_if(path.rbegin(), path.rend(), isSep);
    if (sep != path.rend())
        addFolder(path.substr(0, static_cast<size_t>(path.rend() - sep - 1)));
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(fsPath(path), ec);
    if (ec)
        return;
    // Every folder above it as well: a message may name the disc a playlist is on, or the folder of a folder output
    for (std::filesystem::path p = absolute.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
    {
        std::string s = utf8(p);
        if (strStartWith(s, "\\\\?\\"))
            s = s.substr(4);
        addFolder(s);
        if (p == p.parent_path())
            break;
    }
}

void addName(const std::string& text, const std::string& kind)
{
    // a name of one or two letters would be matched in too many other words; the lines that give it are covered by
    // their own rules (redactFields)
    if (!active || keepNames || text.size() < 3 || text[0] == '<')
        return;
    const std::lock_guard<std::recursive_mutex> guard(lock);
    for (const auto& [known, placeholder] : namedTexts)
        if (known == text)
            return;
    namedTexts.emplace_back(text, textPlaceholder(kind, text));
    std::sort(namedTexts.begin(), namedTexts.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

namespace
{
// The name is the report's file, or a file inside it: a source can be named inside a disc image. Compared as the system
// does, and by the full path for a name that is not there yet, as an output.
bool reportHolds(const std::string& name)
{
    std::error_code ec;
    if (std::filesystem::equivalent(fsPath(reportName), fsPath(name), ec) && !ec)
        return true;
    const std::filesystem::path report = std::filesystem::weakly_canonical(fsPath(reportName), ec);
    if (ec)
        return false;
    const std::filesystem::path other = std::filesystem::weakly_canonical(fsPath(name), ec);
    if (ec)
        return false;
    const auto same = [](const std::filesystem::path& a, const std::filesystem::path& b)
    {
#ifdef _WIN32
        return strToLowerCase(utf8(a)) == strToLowerCase(utf8(b));
#else
        return a == b;
#endif
    };
    return std::mismatch(report.begin(), report.end(), other.begin(), other.end(), same).first == report.end();
}

// ** A REPORT NEVER TAKES THE PLACE OF A FILE OF THE JOB. ** Opening the report empties its file. It was opened first
// of all, so a report named as a source or a meta file took that file away before the job began, and one named as the
// output ended up as neither. The files are opened here, with the sources known, as an output named as a source is
// refused.
bool openFiles(const std::vector<std::string>& names)
{
    for (const std::string& name : names)
        if (reportHolds(name))
        {
            std::cerr << "--report names a file of the job: " << name << ". Give the report another name." << std::endl;
            return false;
        }
#ifdef _WIN32
    out = _wfopen(toWide(windowsPath(reportName)).data(), L"wb");
#else
    out = std::fopen(reportName.c_str(), "wb");
#endif
    if (out == nullptr)
    {
        std::cerr << "The report " << reportName << " cannot be written: " << std::strerror(errno) << std::endl;
        return false;
    }
    if (framesAsked)
    {
        framesName = reportName + ".frames.tsv";
#ifdef _WIN32
        framesOut = _wfopen(toWide(windowsPath(framesName)).data(), L"wb");
#else
        framesOut = std::fopen(framesName.c_str(), "wb");
#endif
        if (framesOut == nullptr)
        {
            std::cerr << "The frames file " << framesName << " cannot be written: " << std::strerror(errno)
                      << std::endl;
            std::fclose(out);
            out = nullptr;
            return false;
        }
        std::fputs(
            "kind	part	packet	PID	track	key	PTS	DTS	time_s	bytes	PCR	"
            "early_ms	EB_bytes	"
            "EB_size	late",
            framesOut);
        std::fputs(LINE_END, framesOut);
        framesActive = true;
    }
    return true;
}
}  // namespace

bool takeOptions(int& argc, char** argv)
{
    std::string fileName;
    bool frames = false;
    std::string range;
    for (int i = 1; i < argc;)
    {
        const std::string arg = argv[i];
        if (arg == "--report-full-paths")
            keepPaths = keepNames = true;
        else if (arg == "--report-keep-names")
            keepNames = true;
        else if (arg == "--report-frames")
            frames = true;
        else if (arg.compare(0, 16, "--report-frames=") == 0)
        {
            frames = true;
            range = arg.substr(16);
        }
        else if (arg.compare(0, 9, "--report=") == 0)
            fileName = unquoteStr(arg.substr(9));
        else
        {
            ++i;
            continue;
        }
        for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
        --argc;
    }
    if (fileName.empty())
    {
        if (keepNames || frames)
            std::cerr << (keepPaths   ? "--report-full-paths"
                          : keepNames ? "--report-keep-names"
                                      : "--report-frames")
                      << " goes with --report=<file>, which is not given." << std::endl;
        return !keepNames && !frames;
    }
    if (!range.empty())
    {
        const size_t dash = range.find('-');
        const std::string from = dash == std::string::npos ? range : range.substr(0, dash);
        const std::string to = dash == std::string::npos ? std::string() : range.substr(dash + 1);
        const auto isTime = [](const std::string& t)
        {
            return !t.empty() && t.find_first_not_of("0123456789:.") == std::string::npos &&
                   std::count(t.begin(), t.end(), ':') <= 2;
        };
        if (dash == std::string::npos || (!from.empty() && !isTime(from)) || (!to.empty() && !isTime(to)) ||
            (from.empty() && to.empty()) || (!from.empty() && !to.empty() && timeToFloat(from) > timeToFloat(to)))
        {
            std::cerr << "Unknown --report-frames value \"" << range
                      << "\". It takes a range of the output's time, <from>-<to>, as h:mm:ss or seconds; either end "
                         "may be left out."
                      << std::endl;
            return false;
        }
        framesFrom = from.empty() ? -1 : timeToFloat(from);
        framesTo = to.empty() ? -1 : timeToFloat(to);
    }
    reportName = fileName;
    framesAsked = frames;
    addPath(fileName);
    addFile(fileName, "report");
    started = std::chrono::steady_clock::now();
    consoleOut = std::cout.rdbuf();
    consoleErr = std::cerr.rdbuf();
    active = true;
#ifdef _WIN32
    SetUnhandledExceptionFilter(onCrash);
    std::signal(SIGABRT, onCrashSignal);
#else
    for (const int sig : {SIGSEGV, SIGFPE, SIGBUS, SIGILL, SIGABRT}) std::signal(sig, onCrashSignal);
#endif
    return true;
}

bool writeStart(const int argc, char** argv)
{
    if (!active)
        return true;
    // The folders first, so every line below is written without them
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-')
            positional.emplace_back(unquoteStr(argv[i]));
    std::error_code ec;
    addFolder(utf8(std::filesystem::current_path(ec)));
    addFolder(utf8(std::filesystem::temp_directory_path(ec)));
    for (const char* home : {"HOME", "USERPROFILE"})
        if (const char* value = std::getenv(home); value && *value)
            addFolder(value);
    addPath(argv[0]);
    for (const std::string& p : positional) addPath(p);
    // the value of an option that names a file or a folder (--meta-dir=, --custom-chapters with a file, ...)
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] == '-')
            if (const char* eq = std::strchr(argv[i], '='); eq && std::strpbrk(eq + 1, "/\\"))
            {
                const std::string value = unquoteStr(eq + 1);
                addPath(value);
                addFile(value, "file");
            }
    std::string metaText;
    if (std::any_of(argv + 1, argv + argc, [](const char* a) { return std::strcmp(a, "--bdmv-to-iso") == 0; }) &&
        positional.size() == 2)
    {
        // a disc folder made into an image: no meta file
        sources.push_back(positional[0]);
        outputName = positional[1];
        addFile(positional[0], "source");
        addFile(outputName, "output");
    }
    else if (std::any_of(argv + 1, argv + argc, [](const char* a) { return std::strcmp(a, "--make-sample") == 0; }) &&
             !positional.empty())
    {
        // a sample of a source: no meta file
        sources.push_back(positional[0]);
        addFile(positional[0], "source");
        if (positional.size() > 1)
        {
            outputName = positional[1];
            addFile(outputName, "output");
        }
    }
    else if (positional.size() == 2)
    {
        metaName = positional[0];
        outputName = positional[1];
        addFile(metaName, "meta file");
        addFile(outputName, "output");
        File file;
        if (file.open(metaName.c_str(), File::ofRead))
        {
            const int64_t size = file.size();
            metaText.resize(static_cast<size_t>(std::clamp<int64_t>(size, 0, 1 << 20)));
            const int n = file.read(metaText.data(), static_cast<uint32_t>(metaText.size()));
            metaText.resize(static_cast<size_t>((std::max)(n, 0)));
            file.close();
        }
        readMeta(metaText);
    }
    else if (positional.size() == 1)
    {
        sources.push_back(positional[0]);
        addFile(positional[0], "source");
    }

    std::vector<std::string> jobFiles = positional;
    jobFiles.insert(jobFiles.end(), sources.begin(), sources.end());
    for (int i = 1; i < argc; ++i)
        if (strStartWith(argv[i], "--add-report="))
            jobFiles.push_back(unquoteStr(argv[i] + 13));
    if (!openFiles(jobFiles))
    {
        active = false;
        return false;
    }

    writeLine("tsMuxeR diagnostic report");
    writeLine("Version: " TSMUXER_VERSION);
    writeLine("Build: " + buildText());
    writeLine("System: " + systemText());
    writeLine("Processors: " + std::to_string(std::thread::hardware_concurrency()) + "; memory: " + memoryText());
    writeLine("Started: " + localTime());
    writeLine(keepPaths   ? "Paths: as given (--report-full-paths)"
              : keepNames ? "Paths: folders left out, \"<folder>\" for one alone; names as given (--report-keep-names)"
                          : "Paths: folders left out, \"<folder>\" for one alone; the names of files, titles, track "
                            "names and chapter names replaced by placeholders, described under Names");
    writeLine(
        "Lines: seconds since the start, then E error, W warning, I information, D detail; a lower-case letter "
        "marks a line that was not shown on the screen");
    if (framesActive)
    {
        std::string range = "the whole output";
        if (framesFrom >= 0 || framesTo >= 0)
            range = "from " + (framesFrom >= 0 ? doubleToStr(framesFrom, 3) + " s" : std::string("the start")) +
                    " to " + (framesTo >= 0 ? doubleToStr(framesTo, 3) + " s" : std::string("the end"));
        writeLine("Frames: " + framesName + ", " + range +
                  ". kind pes: a PES of a transport stream mux (part = output file of a split, packet = its first "
                  "packet in that file, PTS/DTS/PCR in 90 kHz, early_ms = DTS minus the clock when it was written). "
                  "kind buffer: an MPEG-2 picture taken out of the decoder's buffer at its DTS (EB_bytes = the "
                  "buffer then, early_ms = how long its first byte waited, late = not whole in time).");
    }

    if (!keepNames && !filesInOrder.empty())
    {
        // What the placeholders keep of each name: what problems with names depend on, and what tsMuxeR takes from a
        // name. Written through the same rules, each name here is its placeholder.
        section("Names");
        for (const NamedFile& f : filesInOrder)
        {
            std::string line = f.name + ": the name " + nameInfo(f.name);
            if (f.name.find(' ') != std::string::npos)
                line += ", with spaces";
            if (const std::string lang = f.source ? langFromFileName(f.name) : std::string(); !lang.empty())
                line += ", gives the language " + lang;
            if (const int64_t delay = f.source ? delayFromFileName(f.name) : 0; delay != 0)
                line += ", gives a delay of " + std::to_string(delay) + " ms";
            std::error_code ec;
            const std::string absolute = utf8(std::filesystem::absolute(fsPath(f.path), ec));
            if (!ec)
            {
                line += "; the path " + nameInfo(absolute);
                if (networkPath(absolute, 0))
                    line += ", on a network share";
            }
            text(line);
        }
    }

    section("Command line");
    std::string line = "tsMuxeR";
    for (int i = 1; i < argc; ++i) line += " " + quoted(argv[i]);
    text(line);

    if (!metaName.empty())
    {
        section("Meta file " + metaName);
        const bool bom = strStartWith(metaText, "\xEF\xBB\xBF");
        const size_t crlf = std::count(metaText.begin(), metaText.end(), '\r');
        text(std::to_string(metaText.size()) + " bytes" + (bom ? ", UTF-8 with a byte order mark" : "") +
             (crlf > 0 ? ", lines ended CR LF" : ", lines ended LF"));
        std::istringstream lines(bom ? metaText.substr(3) : metaText);
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            text(line);
        }
    }
    if (!sources.empty())
    {
        section("Sources");
        for (const std::string& s : sources) describeSource(s);
    }
    return true;
}

void trace(const unsigned level, const int errIndex, const std::string& text)
{
    // printed exactly as LTRACE prints without a report
    if ((errIndex & 2) != 0)
    {
        if (level <= LT_WARN)
            std::cerr << text << std::endl;
        else if (level == LT_INFO)
            std::cout << text << std::endl;
        if (level <= LT_INFO)
            sLastMsg = true;
    }
    const std::lock_guard<std::recursive_mutex> guard(lock);
    const bool onScreen = (errIndex & 2) != 0 && level <= LT_INFO &&
                          (level <= LT_WARN ? std::cerr.rdbuf() == consoleErr : std::cout.rdbuf() == consoleOut);
    const std::string full = pending + text;
    pending.clear();
    if (listing)
        diagReport::text(full);
    else
        record(level, onScreen, full);
}

void partial(const unsigned level, const std::string& text)
{
    if (level <= LT_WARN)
        std::cerr << text;
    else if (level == LT_INFO)
        std::cout << text;
    if (level <= LT_INFO)
        sLastMsg = true;
    const std::lock_guard<std::recursive_mutex> guard(lock);
    pending += text;
}

void progress(const double percent)
{
    if (!active)
        return;
    const int tenth = static_cast<int>(percent / 10);
    const std::lock_guard<std::recursive_mutex> guard(lock);
    if (tenth <= lastTenth)
        return;
    lastTenth = tenth;
    std::ostringstream s;
    s << std::fixed << std::setprecision(1) << percent << "% complete";
    record(LT_INFO, true, s.str());
}

void section(const std::string& title)
{
    const std::lock_guard<std::recursive_mutex> guard(lock);
    inMessages = false;
    writeLine("");
    writeLine("== " + title);
}

void text(const std::string& line)
{
    const std::lock_guard<std::recursive_mutex> guard(lock);
    // a text of several lines, as a disc's title listing, line by line
    std::istringstream lines(line);
    std::string one;
    bool any = false;
    while (std::getline(lines, one))
    {
        if (!one.empty() && one.back() == '\r')
            one.pop_back();
        writeLine("  " + one);
        any = true;
    }
    if (!any)
        writeLine("  ");
}

void setListing(const bool on) { listing = on; }

std::vector<std::string> metaSources() { return sources; }

void addFileName(const std::string& path, const std::string& kind)
{
    const std::lock_guard<std::recursive_mutex> guard(lock);
    addPath(path);
    addFile(path, kind);
}

bool keepingNames() { return keepNames; }

void setOutput(const std::string& output) { outputName = output; }

bool muxRun() { return !metaName.empty(); }

void writeOutput(const std::string& output)
{
    if (!active || output.empty())
        return;
    section("Output");
    std::error_code ec;
    const std::filesystem::path root = fsPath(output);
    if (std::filesystem::is_regular_file(root, ec))
        text(output + ": " + std::to_string(std::filesystem::file_size(root, ec)) + " bytes");
    else if (std::filesystem::is_directory(root, ec))
    {
        std::vector<std::pair<std::string, uintmax_t>> files;
        constexpr size_t MAX_WALK = 100000;  // a demux into a big folder of other files
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator() && files.size() < MAX_WALK; it.increment(ec))
            if (it->is_regular_file(ec))
                files.emplace_back(utf8(std::filesystem::relative(it->path(), root, ec)), it->file_size(ec));
        std::sort(files.begin(), files.end());
        uintmax_t total = 0;
        for (const auto& [name, size] : files) total += size;
        text(output + ": a folder of " + std::to_string(files.size()) + (files.size() == 1 ? " file, " : " files, ") +
             std::to_string(total) + " bytes");
        constexpr size_t MAX_FILES = 2000;
        for (size_t i = 0; i < files.size() && i < MAX_FILES; ++i)
            text("  " + files[i].first + ": " + std::to_string(files[i].second) + " bytes");
        if (files.size() > MAX_FILES)
            text("  and " + std::to_string(files.size() - MAX_FILES) + " more");
    }
    else
    {
        // A split writes <name>.split.1.ts and on, not the name given
        const std::string stem = utf8(root.stem()) + ".";
        const std::string ext = utf8(root.extension());
        std::vector<std::pair<std::string, uintmax_t>> parts;
        for (auto it = std::filesystem::directory_iterator(
                 root.parent_path().empty() ? std::filesystem::path(".") : root.parent_path(), ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            const std::string name = utf8(it->path().filename());
            if (it->is_regular_file(ec) && strStartWith(name, stem) && strEndWith(name, ext))
                parts.emplace_back(name, it->file_size(ec));
        }
        std::sort(parts.begin(), parts.end());
        for (const auto& [name, size] : parts) text(name + ": " + std::to_string(size) + " bytes");
        if (parts.empty())
            text(output + ": not there");
    }
    std::filesystem::path folder = std::filesystem::is_directory(root, ec) ? root : root.parent_path();
    if (folder.empty())
        folder = std::filesystem::current_path(ec);
    if (const auto space = std::filesystem::space(folder, ec); !ec)
        text("Free space there: " + std::to_string(space.available) + " bytes");
}

void close(const int exitCode)
{
    if (!active)
        return;
    const std::lock_guard<std::recursive_mutex> guard(lock);
    if (!pending.empty())
        record(LT_INFO, true, pending);
    pending.clear();
    writeOutput(outputName);
    section("End");
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << seconds();
    text("Exit code: " + exitText(exitCode));
    text("Run time: " + s.str() + " s");
    active = false;
    std::fclose(out);
    out = nullptr;
    if (framesOut != nullptr)
    {
        framesActive = false;
        std::fclose(framesOut);
        framesOut = nullptr;
    }
}

double frameTime(const int64_t ts90)
{
    const std::lock_guard<std::recursive_mutex> guard(lock);
    if (firstFrameTs == INT64_MIN)
        firstFrameTs = ts90;
    return static_cast<double>(ts90 - firstFrameTs) / 90000.0;
}

bool framesWanted(const double seconds)
{
    return framesActive && (framesFrom < 0 || seconds >= framesFrom) && (framesTo < 0 || seconds <= framesTo);
}

void frameRow(const std::string& row)
{
    const std::lock_guard<std::recursive_mutex> guard(lock);
    if (framesOut == nullptr)
        return;
    std::fwrite(row.data(), 1, row.size(), framesOut);
    std::fputs(LINE_END, framesOut);
}

void signalNote(const int signal)
{
    if (active)
        writeSignalNote("Stopped", signal);
}

void stopped(const int signal)
{
    if (!active)
        return;
    const std::lock_guard<std::recursive_mutex> guard(lock);
    record(LT_INFO, false, "Stopped by signal " + std::to_string(signal) + " (Ctrl+C, or Abort in the window)");
    close(-2);
}
}  // namespace diagReport
