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
std::string framesName;
double framesFrom = -1;  // the range of --report-frames, in seconds of the output; -1 is open
double framesTo = -1;
int64_t firstFrameTs = INT64_MIN;
std::recursive_mutex lock;  // lines come from the reader threads too
bool keepPaths = false;
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

// A path in a line has its folder left out: "D:/films/a.ts" is "a.ts", and the folder alone is "<folder>". A folder is
// matched only where a path can begin, after a space, a quote or the like, so a name that merely ends like a folder
// keeps its letters.
std::string shortenPaths(const std::string& line)
{
    if (keepPaths || folders.empty())
        return line;
    std::string r;
    r.reserve(line.size());
    for (size_t i = 0; i < line.size();)
    {
        const unsigned char before = i == 0 ? ' ' : static_cast<unsigned char>(line[i - 1]);
        bool hit = false;
        if (!std::isalnum(before) && before != '_' && before != '-' && before != '.' && before < 0x80)
            for (const std::string& f : folders)
            {
                if (!folderAt(line, i, f))
                    continue;
                const size_t end = i + f.size();
                if (end < line.size() && isSep(line[end]))
                {
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
        if (!hit)
            r += line[i++];
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

void writeLine(const std::string& line)
{
    if (out == nullptr)
        return;
    const std::string s = shortenPaths(line);
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
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S (UTC%z)", &tm);
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
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        for (size_t q = line.find('"'); q != std::string::npos;)
        {
            const size_t e = line.find('"', q + 1);
            if (e == std::string::npos)
                break;
            if (const std::string s = line.substr(q + 1, e - q - 1); s.find_first_of("/\\") != std::string::npos)
                addPath(s);
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
            }
            if (rest.empty() || rest[0] != '+')
                break;
            rest = trimStr(rest.substr(1));
        }
    }
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

bool takeOptions(int& argc, char** argv)
{
    std::string fileName;
    bool frames = false;
    std::string range;
    for (int i = 1; i < argc;)
    {
        const std::string arg = argv[i];
        if (arg == "--report-full-paths")
            keepPaths = true;
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
        if (keepPaths || frames)
            std::cerr << (keepPaths ? "--report-full-paths" : "--report-frames")
                      << " goes with --report=<file>, which is not given." << std::endl;
        return !keepPaths && !frames;
    }
    if (!range.empty())
    {
        const size_t dash = range.find('-');
        const std::string from = dash == std::string::npos ? range : range.substr(0, dash);
        const std::string to = dash == std::string::npos ? std::string() : range.substr(dash + 1);
        const auto isTime = [](const std::string& t)
        { return !t.empty() && t.find_first_not_of("0123456789:.") == std::string::npos; };
        if (dash == std::string::npos || (!from.empty() && !isTime(from)) || (!to.empty() && !isTime(to)) ||
            (from.empty() && to.empty()))
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
#ifdef _WIN32
    out = _wfopen(toWide(windowsPath(fileName)).data(), L"wb");
#else
    out = std::fopen(fileName.c_str(), "wb");
#endif
    if (out == nullptr)
    {
        std::cerr << "The report " << fileName << " cannot be written: " << std::strerror(errno) << std::endl;
        return false;
    }
    if (frames)
    {
        framesName = fileName + ".frames.tsv";
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
    addPath(fileName);
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

void writeStart(const int argc, char** argv)
{
    if (!active)
        return;
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
    std::string metaText;
    if (std::any_of(argv + 1, argv + argc, [](const char* a) { return std::strcmp(a, "--bdmv-to-iso") == 0; }) &&
        positional.size() == 2)
    {
        // a disc folder made into an image: no meta file
        sources.push_back(positional[0]);
        outputName = positional[1];
    }
    else if (positional.size() == 2)
    {
        metaName = positional[0];
        outputName = positional[1];
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
        sources.push_back(positional[0]);

    writeLine("tsMuxeR diagnostic report");
    writeLine("Version: " TSMUXER_VERSION);
    writeLine("Build: " + buildText());
    writeLine("System: " + systemText());
    writeLine("Processors: " + std::to_string(std::thread::hardware_concurrency()) + "; memory: " + memoryText());
    writeLine("Started: " + localTime());
    writeLine(keepPaths ? "Paths: as given (--report-full-paths)"
                        : "Paths: folders left out, \"<folder>\" for one alone");
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
