#include "assRender.h"

#include <fs/file.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "matroskaParser.h"
#include "vodCoreException.h"
#include "vod_common.h"

// The parts of the public interface of libass (ass.h, version 0.15 and later) tsMuxeR uses, declared here so that
// tsMuxeR needs neither the header nor the library to be built: the library is loaded when it is needed.
namespace
{
struct ASS_Library;
struct ASS_Renderer;
struct ASS_Track;
struct ASS_Image
{
    int w, h;
    int stride;
    unsigned char* bitmap;  // one coverage value a pixel, 0 to 255
    uint32_t color;         // 0xRRGGBBAA, the last byte the transparency (0 opaque)
    int dst_x, dst_y;
    ASS_Image* next;
    int type;
};
constexpr int ASS_FONTPROVIDER_AUTODETECT = 1;

struct Lib
{
    bool tried = false;
    bool ok = false;
    std::string why;
    ASS_Library* (*library_init)() = nullptr;
    void (*library_done)(ASS_Library*) = nullptr;
    void (*set_message_cb)(ASS_Library*, void (*)(int, const char*, va_list, void*), void*) = nullptr;
    void (*set_extract_fonts)(ASS_Library*, int) = nullptr;
    void (*set_fonts_dir)(ASS_Library*, const char*) = nullptr;
    void (*add_font)(ASS_Library*, const char*, const char*, int) = nullptr;
    ASS_Renderer* (*renderer_init)(ASS_Library*) = nullptr;
    void (*renderer_done)(ASS_Renderer*) = nullptr;
    void (*set_frame_size)(ASS_Renderer*, int, int) = nullptr;
    void (*set_storage_size)(ASS_Renderer*, int, int) = nullptr;
    void (*set_fonts)(ASS_Renderer*, const char*, const char*, int, const char*, int) = nullptr;
    ASS_Track* (*new_track)(ASS_Library*) = nullptr;
    void (*free_track)(ASS_Track*) = nullptr;
    void (*process_codec_private)(ASS_Track*, const char*, int) = nullptr;
    void (*process_chunk)(ASS_Track*, const char*, int, long long, long long) = nullptr;
    ASS_Image* (*render_frame)(ASS_Renderer*, ASS_Track*, long long, int*) = nullptr;
};

Lib& lib()
{
    static Lib l;
    if (l.tried)
        return l;
    l.tried = true;
#ifdef _WIN32
    // From the folder of tsMuxeR.exe by its full path first, so that no ass-9.dll of the current folder is taken; the
    // libraries it needs are then found beside it as well. Then by name, from the system.
    HMODULE h = nullptr;
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string dir(exe, n);
        dir.resize(dir.find_last_of("\\/") + 1);
        h = LoadLibraryA((dir + "ass-9.dll").c_str());
    }
    for (const char* name : {"ass-9.dll", "libass-9.dll", "libass.dll", "ass.dll"})
        if (h == nullptr)
            h = LoadLibraryA(name);
    const auto sym = [&h](const char* s) { return reinterpret_cast<void*>(GetProcAddress(h, s)); };
#else
    void* h = nullptr;
    // Linux: the distribution's package (libass9). macOS: Homebrew's, whose folders the loader does not search by
    // itself
    for (const char* name : {"libass.so.9", "libass.so", "libass.9.dylib", "/opt/homebrew/lib/libass.9.dylib",
                             "/usr/local/lib/libass.9.dylib"})
        if ((h = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr)
            break;
    const auto sym = [&h](const char* s) { return dlsym(h, s); };
#endif
    if (!h)
    {
#ifdef _WIN32
        l.why = "ass-9.dll was not found beside tsMuxeR or in the system";
#else
        l.why = "the libass library was not found in the system";
#endif
        return l;
    }
    bool all = true;
    const auto get = [&](auto& fn, const char* s)
    {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(s));
        all = all && fn != nullptr;
    };
    get(l.library_init, "ass_library_init");
    get(l.library_done, "ass_library_done");
    get(l.set_message_cb, "ass_set_message_cb");
    get(l.set_extract_fonts, "ass_set_extract_fonts");
    get(l.set_fonts_dir, "ass_set_fonts_dir");
    get(l.add_font, "ass_add_font");
    get(l.renderer_init, "ass_renderer_init");
    get(l.renderer_done, "ass_renderer_done");
    get(l.set_frame_size, "ass_set_frame_size");
    get(l.set_storage_size, "ass_set_storage_size");
    get(l.set_fonts, "ass_set_fonts");
    get(l.new_track, "ass_new_track");
    get(l.free_track, "ass_free_track");
    get(l.process_codec_private, "ass_process_codec_private");
    get(l.process_chunk, "ass_process_chunk");
    get(l.render_frame, "ass_render_frame");
    l.ok = all;
    if (!all)
        l.why = "the libass library found lacks functions tsMuxeR needs (an old version?)";
    return l;
}

// libass reports problems through this; only errors are passed on, each different one once
void onMessage(const int level, const char* fmt, va_list va, void*)
{
    if (level > 1)
        return;
    char text[512];
    vsnprintf(text, sizeof(text), fmt, va);
    static std::set<std::string> told;
    if (told.insert(text).second)
        LTRACE(LT_WARN, 2, "libass: " << text);
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

namespace ass_render
{
bool available(std::string* why)
{
    const Lib& l = lib();
    if (why)
        *why = l.why;
    return l.ok;
}

std::vector<Font> fontsFromAttachments(const std::vector<std::vector<uint8_t>>& attachments)
{
    // EBML: an ID with its length marker kept, then a size without it
    const auto vint = [](const uint8_t*& p, const uint8_t* end, uint64_t& v, const bool keep)
    {
        if (p >= end || *p == 0)
            return false;
        int len = 1;
        while (!(*p & (0x80 >> (len - 1)))) ++len;
        if (end - p < len)
            return false;
        v = keep ? *p : *p & (0xFF >> len);
        for (int i = 1; i < len; ++i) v = v << 8 | p[i];
        p += len;
        return true;
    };
    std::vector<Font> fonts;
    for (const auto& a : attachments)
    {
        const uint8_t* p = a.data();
        const uint8_t* end = p + a.size();
        uint64_t id = 0, size = 0;
        if (!vint(p, end, id, true) || !vint(p, end, size, false) || id != 0x61A7)  // AttachedFile
            continue;
        end = (std::min)(end, p + size);
        std::string name, mime;
        std::vector<uint8_t> data;
        while (p < end)
        {
            if (!vint(p, end, id, true) || !vint(p, end, size, false) || size > static_cast<uint64_t>(end - p))
                break;
            if (id == 0x466E)
                name.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(size));
            else if (id == 0x4660)
                mime.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(size));
            else if (id == 0x465C)
                data.assign(p, p + size);
            p += size;
        }
        const std::string m = lower(mime);
        const std::string n = lower(name);
        const bool font = m.find("font") != std::string::npos || m.find("opentype") != std::string::npos ||
                          m.find("truetype") != std::string::npos || strEndWith(n, ".ttf") || strEndWith(n, ".otf") ||
                          strEndWith(n, ".ttc") || strEndWith(n, ".otc");
        if (font && !data.empty())
            fonts.push_back(Font{name, std::move(data)});
    }
    return fonts;
}
}  // namespace ass_render

struct AssDecoder::Impl
{
    ASS_Library* library = nullptr;
    ASS_Renderer* renderer = nullptr;
    ASS_Track* track = nullptr;
    int width = 0, height = 0;
    double fps = 25;
    // The events as they come, in the order of their start times, in milliseconds and in frames of the video
    struct Event
    {
        int64_t startMs, endMs;
        int64_t startFrame, endFrame;
    };
    std::vector<Event> events;
    size_t nextEvent = 0;                             // the first event not on the screen yet
    std::multiset<std::pair<int64_t, int64_t>> ends;  // the end frames and times of the events on the screen
    int64_t frame = 0;                                // the first frame not drawn yet
    bool shown = false;
    int64_t pictures = 0;
    // save-srt: the text of every event, as an SRT file
    std::unique_ptr<File> srt;
    std::string srtName;
    int srtCount = 0;

    // The frame a time falls on, as the PGS display sets are timed (TextToPGSConverter::alignToGrid)
    [[nodiscard]] int64_t frameOf(const int64_t ms) const
    {
        return static_cast<int64_t>(floor(static_cast<double>(ms) * fps / 1000.0 + 0.5 + 1e-6));
    }
    [[nodiscard]] int64_t ptsOf(const int64_t f) const
    {
        return llround(static_cast<double>(f) * INTERNAL_PTS_FREQ / fps);
    }
};

AssDecoder::AssDecoder(const uint8_t* header, const int headerSize, const ass_render::Drawing& drawing,
                       const std::vector<ass_render::Font>& fonts)
    : m(new Impl)
{
    Lib& l = lib();
    if (!l.ok)
    {
        delete m;
        THROW(ERR_COMMON, "libass cannot be used: " << l.why)
    }
    if (!drawing.saveSrt.empty())
    {
        m->srt = std::make_unique<File>();
        if (!m->srt->open(drawing.saveSrt.c_str(), File::ofWrite))
        {
            delete m;
            THROW(ERR_COMMON, "save-srt: the file " << drawing.saveSrt << " cannot be written.")
        }
        m->srtName = drawing.saveSrt;
    }
    setUp(drawing.width, drawing.height, drawing.fps, drawing.fps);
    m->width = drawing.width;
    m->height = drawing.height;
    if (drawing.fps > 0)
        m->fps = drawing.fps;
    m->library = l.library_init();
    l.set_message_cb(m->library, onMessage, nullptr);
    l.set_extract_fonts(m->library, 1);  // fonts in the script's own [Fonts] section
    if (!drawing.fontsDir.empty())
        l.set_fonts_dir(m->library, drawing.fontsDir.c_str());
    for (const auto& f : fonts)
        l.add_font(m->library, f.name.c_str(), reinterpret_cast<const char*>(f.data.data()),
                   static_cast<int>(f.data.size()));
    m->renderer = l.renderer_init(m->library);
    l.set_frame_size(m->renderer, m->width, m->height);
    l.set_storage_size(m->renderer, m->width, m->height);
    l.set_fonts(m->renderer, nullptr, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    m->track = l.new_track(m->library);
    if (header && headerSize > 0)
        l.process_codec_private(m->track, reinterpret_cast<const char*>(header), headerSize);
}

AssDecoder::~AssDecoder()
{
    if (m->srt)
    {
        m->srt->close();
        LTRACE(
            LT_INFO, 2,
            "The ASS subtitles were also written as text to " << m->srtName << " (" << m->srtCount << " subtitles).");
    }
    const Lib& l = lib();
    if (m->track)
        l.free_track(m->track);
    if (m->renderer)
        l.renderer_done(m->renderer);
    if (m->library)
        l.library_done(m->library);
    delete m;
}

void AssDecoder::decode(const uint8_t* data, const int size, const int64_t pts, const int64_t duration,
                        std::vector<uint8_t>& out)
{
    const int64_t startMs = pts * 1000 / INTERNAL_PTS_FREQ;
    const int64_t durMs = duration > 0 ? duration * 1000 / INTERNAL_PTS_FREQ : 0;
    // Every frame before the one this event starts on is settled: no event that comes later starts earlier
    drawUpTo(m->frameOf(startMs), out);
    lib().process_chunk(m->track, reinterpret_cast<const char*>(data), size, startMs, durMs);
    m->events.push_back({startMs, startMs + durMs, m->frameOf(startMs), m->frameOf(startMs + durMs)});
    if (m->srt && durMs > 0)
    {
        const std::string text = assToSrtText(std::string(reinterpret_cast<const char*>(data), size));
        if (text.find_first_not_of(" \n") == std::string::npos)
            return;  // a drawing or an effect without text
        const auto clock = [](const int64_t ms)
        {
            const auto two = [](const int64_t v)
            { return std::string(1, static_cast<char>('0' + v / 10 % 10)) + static_cast<char>('0' + v % 10); };
            return two(ms / 3600000) + ":" + two(ms / 60000 % 60) + ":" + two(ms / 1000 % 60) + "," +
                   std::to_string(ms % 1000 / 100) + std::to_string(ms % 100 / 10) + std::to_string(ms % 10);
        };
        std::string rec = m->srtCount == 0 ? "\xEF\xBB\xBF" : "";
        rec += std::to_string(++m->srtCount) + "\r\n" + clock(startMs) + " --> " + clock(startMs + durMs) + "\r\n";
        for (const char c : text) rec += c == '\n' ? std::string("\r\n") : std::string(1, c);
        rec += "\r\n\r\n";
        m->srt->write(rec.data(), static_cast<uint32_t>(rec.size()));
    }
}

void AssDecoder::finish(std::vector<uint8_t>& out)
{
    int64_t last = 0;
    for (const auto& e : m->events) last = (std::max)(last, e.endFrame);
    drawUpTo(last + 1, out);
    m_pgs.finish(out);
    LTRACE(LT_INFO, 2,
           "ASS subtitles drawn with libass: " << m->events.size() << " events, " << m->pictures << " pictures.");
}

// Frame by frame while anything is on the screen, a picture wherever libass draws something else than on the frame
// before: an event that starts or ends, and every frame of a fade, a move or karaoke
void AssDecoder::drawUpTo(const int64_t endFrame, std::vector<uint8_t>& out)
{
    const Lib& l = lib();
    while (m->frame < endFrame)
    {
        const int64_t f = m->frame;
        // the time libass draws the frame at: its start, or later where an event starts or ends on it after that,
        // so that an event shown on the frame is drawn and an ended one is not
        int64_t ms = llround(static_cast<double>(f) * 1000.0 / m->fps);
        while (m->nextEvent < m->events.size() && m->events[m->nextEvent].startFrame <= f)
        {
            const Impl::Event& e = m->events[m->nextEvent++];
            if (e.endFrame > f)
                m->ends.emplace(e.endFrame, e.endMs);
            if (e.startFrame == f)
                ms = (std::max)(ms, e.startMs);
        }
        while (!m->ends.empty() && m->ends.begin()->first <= f)
        {
            ms = (std::max)(ms, m->ends.begin()->second);
            m->ends.erase(m->ends.begin());
        }
        if (m->ends.empty())
        {
            // nothing on the screen: what was shown goes, and the drawing goes on where the next event starts
            if (m->shown)
            {
                m_pgs.clearCanvas();
                m_pgs.addPicture(m->ptsOf(f), -1, false, out);
                m->shown = false;
            }
            const int64_t next = m->nextEvent < m->events.size() ? m->events[m->nextEvent].startFrame : endFrame;
            m->frame = (std::max)(f + 1, (std::min)(next, endFrame));
            continue;
        }
        int changed = 0;
        const ASS_Image* img = l.render_frame(m->renderer, m->track, ms, &changed);
        if (changed || !m->shown)
        {
            m_pgs.clearCanvas();
            uint32_t* canvas = m_pgs.pixels();
            for (; img; img = img->next)
            {
                const uint32_t c = img->color;
                const uint32_t r = c >> 24, g = c >> 16 & 0xFF, b = c >> 8 & 0xFF, opacity = 255 - (c & 0xFF);
                for (int y = 0; y < img->h; ++y)
                {
                    const int dy = img->dst_y + y;
                    if (dy < 0 || dy >= m->height)
                        continue;
                    for (int x = 0; x < img->w; ++x)
                    {
                        const int dx = img->dst_x + x;
                        if (dx < 0 || dx >= m->width)
                            continue;
                        const uint32_t a = img->bitmap[y * img->stride + x] * opacity / 255;
                        if (a == 0)
                            continue;
                        // this layer over what is drawn already
                        uint32_t& d = canvas[dy * m->width + dx];
                        const uint32_t da = d >> 24;
                        const uint32_t oa = a + da * (255 - a) / 255;
                        const auto mix = [&](const uint32_t src, const int shift)
                        { return (src * a + (d >> shift & 0xFF) * da * (255 - a) / 255) / oa; };
                        d = oa << 24 | mix(r, 16) << 16 | mix(g, 8) << 8 | mix(b, 0);
                    }
                }
            }
            m_pgs.addPicture(m->ptsOf(f), -1, false, out);
            m->shown = true;
            ++m->pictures;
        }
        m->frame = f + 1;
    }
}
