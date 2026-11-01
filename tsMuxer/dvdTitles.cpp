#include "dvdTitles.h"

#include <fs/directory.h>
#include <fs/discImage.h>
#include <fs/file.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "vod_common.h"

namespace
{
constexpr size_t SECTOR = 2048;
using Bytes = std::vector<uint8_t>;

// A file read whole, up to `limit` bytes
Bytes readFile(const std::string& path, const size_t limit)
{
    File file;
    if (!file.open(path.c_str(), File::ofRead | File::ofOpenExisting))
        throw std::runtime_error("cannot open " + path);
    int64_t size = 0;
    if (!file.size(&size) || size < 0)
        throw std::runtime_error("cannot read " + path);
    Bytes data(static_cast<size_t>((std::min)(static_cast<uint64_t>(size), static_cast<uint64_t>(limit))));
    size_t done = 0;
    while (done < data.size())
    {
        const int r = file.read(data.data() + done, static_cast<uint32_t>(data.size() - done));
        if (r <= 0)
            break;
        done += static_cast<size_t>(r);
    }
    data.resize(done);
    return data;
}

// Big endian reads that throw at the end of the file, never read past it
struct Reader
{
    const Bytes& d;
    std::string what;
    void need(const size_t p, const size_t n) const
    {
        if (p > d.size() || n > d.size() - p)
            throw std::runtime_error(what + ": the file ends early or points outside itself");
    }
    [[nodiscard]] uint8_t u8(const size_t p) const
    {
        need(p, 1);
        return d[p];
    }
    [[nodiscard]] uint16_t u16(const size_t p) const
    {
        need(p, 2);
        return static_cast<uint16_t>(d[p] << 8 | d[p + 1]);
    }
    [[nodiscard]] uint32_t u32(const size_t p) const
    {
        need(p, 4);
        return static_cast<uint32_t>(d[p]) << 24 | static_cast<uint32_t>(d[p + 1]) << 16 |
               static_cast<uint32_t>(d[p + 2]) << 8 | d[p + 3];
    }
    [[nodiscard]] std::string text(const size_t p, const size_t n) const
    {
        need(p, n);
        return {d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + n)};
    }
    // A playback time: BCD hh mm ss ff, the top two bits of ff the frame rate (01 25, 11 30000/1001 frames a second)
    [[nodiscard]] double time(const size_t p) const
    {
        const auto bcd = [](const uint8_t b) { return (b >> 4) * 10 + (b & 15); };
        const uint8_t f = u8(p + 3);
        const long long whole = bcd(u8(p)) * 3600LL + bcd(u8(p + 1)) * 60LL + bcd(u8(p + 2));
        if (f >> 6 == 1)
            return static_cast<double>(whole * 25 + bcd(f & 0x3F)) / 25.0;
        if (f >> 6 == 3)
            return static_cast<double>(whole * 30 + bcd(f & 0x3F)) * 1001.0 / 30000.0;
        return static_cast<double>(whole);
    }
};

std::string langCode(const Reader& r, const size_t p)
{
    std::string l = r.text(p, 2);
    for (const char c : l)
        if (c < 'a' || c > 'z')
            return "";
    return l;
}

struct Pgc
{
    double seconds = 0;
    std::array<int, 8> audio{};        // the stream number of each audio stream of the title set, -1 not enabled
    std::array<int, 32> subpicture{};  // the same for the sub-pictures (the 4:3 one)
    std::vector<uint8_t> programs;     // the first cell of each program, from 1
    std::vector<DvdCell> cells;
};

Pgc readPgc(const Reader& r, const size_t at)
{
    Pgc g;
    const size_t programs = r.u8(at + 2), cells = r.u8(at + 3);
    g.seconds = r.time(at + 4);
    for (size_t i = 0; i < 8; ++i)
    {
        const uint16_t a = r.u16(at + 0x0C + i * 2);
        g.audio[i] = a & 0x8000 ? (a >> 8) & 7 : -1;
    }
    for (size_t i = 0; i < 32; ++i)
    {
        const uint32_t s = r.u32(at + 0x1C + i * 4);
        g.subpicture[i] = s & 0x80000000u ? static_cast<int>(s >> 24 & 0x1F) : -1;
    }
    const size_t map = r.u16(at + 0xE6), play = r.u16(at + 0xE8), pos = r.u16(at + 0xEA);
    if (programs && !map)
        throw std::runtime_error(r.what + ": a program chain has programs but no program map");
    if (cells && (!play || !pos))
        throw std::runtime_error(r.what + ": a program chain has cells but no cell table");
    for (size_t i = 0; i < programs; ++i)
    {
        const uint8_t e = r.u8(at + map + i);
        if (e == 0 || e > cells)
            throw std::runtime_error(r.what + ": a program starts at cell " + std::to_string(e) + " of " +
                                     std::to_string(cells));
        g.programs.push_back(e);
    }
    for (size_t c = 0; c < cells; ++c)
    {
        const size_t b = at + play + c * 24, q = at + pos + c * 4;
        DvdCell cell;
        const uint8_t flags = r.u8(b);
        cell.seconds = r.time(b + 4);
        cell.firstSector = r.u32(b + 8);
        cell.lastSector = r.u32(b + 20);
        cell.vobId = r.u16(q);
        cell.cellId = r.u8(q + 3);
        cell.angle = (flags >> 4 & 3) == 1;  // block type: angle
        cell.stcBreak = flags & 0x02;
        cell.block = static_cast<uint8_t>(flags >> 6);
        g.cells.push_back(cell);
    }
    return g;
}

// A cell that plays: not an angle other than the first of its block
bool firstAngle(const DvdCell& c) { return !c.angle || c.block <= 1; }

// The time from the start of a program chain to one of its cells (from 0), an angle block counted once
double timeBefore(const Pgc& g, const size_t cell)
{
    double t = 0;
    for (size_t c = 0; c < cell && c < g.cells.size(); ++c)
        if (firstAngle(g.cells[c]))
            t += g.cells[c].seconds;
    return t;
}

struct TitleSet
{
    std::string video;
    std::vector<DvdAudio> audio;
    std::vector<std::string> subpictures;
    std::vector<Pgc> pgcs;
    std::vector<std::vector<std::pair<int, int>>> chapters;  // per title of the set: (program chain, program)
    uint64_t vobSectors = 0;                                 // of VTS_nn_1.VOB to VTS_nn_9.VOB together
    bool encrypted = false;
};

TitleSet readSet(const Bytes& data, const int n)
{
    char name[24];
    snprintf(name, sizeof name, "VTS_%02d_0.IFO", n);
    const Reader r{data, name};
    if (r.text(0, 12) != "DVDVIDEO-VTS")
        throw std::runtime_error(std::string(name) + ": not the IFO file of a title set");
    TitleSet s;
    const uint8_t v0 = r.u8(0x200), v1 = r.u8(0x201);
    const bool pal = (v0 >> 4 & 3) == 1;
    int width = 720, height = pal ? 576 : 480;
    switch (v1 >> 2 & 3)
    {
    case 1:
        width = 704;
        break;
    case 2:
        width = 352;
        break;
    case 3:
        width = 352;
        height /= 2;
        break;
    default:
        break;
    }
    s.video = std::string(v0 >> 6 == 0 ? "MPEG-1 " : "MPEG-2 ") + std::to_string(width) + "x" + std::to_string(height) +
              (pal ? " 25 fps" : " 29.97 fps") + ((v0 >> 2 & 3) == 3 ? " 16:9" : " 4:3");
    const size_t audio = (std::min)(static_cast<size_t>(r.u16(0x202)), size_t{8});
    const size_t subs = (std::min)(static_cast<size_t>(r.u16(0x254)), size_t{32});
    for (size_t i = 0; i < audio; ++i)
    {
        const size_t a = 0x204 + i * 8;
        DvdAudio au;
        const uint8_t b0 = r.u8(a), b1 = r.u8(a + 1);
        static const char* codings[8] = {"AC-3", nullptr, "MPEG-1", "MPEG-2", "LPCM", nullptr, "DTS", nullptr};
        au.coding = codings[b0 >> 5] ? codings[b0 >> 5] : "coding " + std::to_string(b0 >> 5);
        if ((b0 >> 2 & 3) == 1)
            au.lang = langCode(r, a + 2);
        au.channels = (b1 & 7) + 1;
        s.audio.push_back(au);
    }
    for (size_t i = 0; i < subs; ++i)
    {
        const size_t a = 0x256 + i * 6;
        s.subpictures.push_back((r.u8(a) & 3) == 1 ? langCode(r, a + 2) : std::string());
    }
    const size_t ptt = static_cast<size_t>(r.u32(0xC8)) * SECTOR;
    const size_t titles = r.u16(ptt);
    const size_t end = static_cast<size_t>(r.u32(ptt + 4)) + 1;
    std::vector<size_t> offsets;
    for (size_t i = 0; i < titles; ++i) offsets.push_back(r.u32(ptt + 8 + i * 4));
    for (size_t i = 0; i < titles; ++i)
    {
        // A list that runs past the table is cut at its end; the reference to it stays readable
        const size_t from = offsets[i], to = (std::min)(i + 1 < titles ? offsets[i + 1] : end, end);
        std::vector<std::pair<int, int>> list;
        for (size_t p = from; p + 4 <= to && list.size() < 999; p += 4)
            list.emplace_back(r.u16(ptt + p), r.u16(ptt + p + 2));
        s.chapters.push_back(list);
    }
    const size_t pgci = static_cast<size_t>(r.u32(0xCC)) * SECTOR;
    const size_t count = r.u16(pgci);
    for (size_t i = 0; i < count; ++i)
    {
        try
        {
            s.pgcs.push_back(readPgc(r, pgci + r.u32(pgci + 8 + i * 8 + 4)));
        }
        catch (const std::exception&)
        {
            s.pgcs.emplace_back();  // a program chain that cannot be read plays nothing
        }
    }
    return s;
}

// Whether the start of a title set's video is still encrypted: the scrambling bits of the PES packets of its packs
bool encryptedVob(const Bytes& d)
{
    for (size_t p = 0; p + SECTOR <= d.size(); p += SECTOR)
    {
        if (d[p] != 0 || d[p + 1] != 0 || d[p + 2] != 1 || d[p + 3] != 0xBA || (d[p + 4] & 0xC0) != 0x40)
            continue;  // an MPEG-2 pack header
        size_t q = p + 14 + (d[p + 13] & 7);
        while (q + 7 <= p + SECTOR && d[q] == 0 && d[q + 1] == 0 && d[q + 2] == 1)
        {
            const uint8_t id = d[q + 3];
            const size_t len = static_cast<size_t>(d[q + 4] << 8 | d[q + 5]);
            if ((id >= 0xE0 && id <= 0xEF) || (id >= 0xC0 && id <= 0xDF) || id == 0xBD)
                if (d[q + 6] >> 4 & 3)
                    return true;
            q += 6 + len;
        }
    }
    return false;
}

bool isFolder(const std::string& path)
{
    if (discImage::split(path, nullptr, nullptr))
        return discImage::isDir(path);
    std::error_code ec;
    return std::filesystem::is_directory(std::u8string(path.begin(), path.end()), ec);
}

std::string withoutEndSeparators(std::string s)
{
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// The IFO file, or its backup copy when the file is not there or not readable as one
Bytes ifo(const std::string& videoTs, const std::string& stem, const char* magic)
{
    for (const char* ext : {".IFO", ".BUP"})
    {
        const std::string path = videoTs + "/" + stem + ext;
        if (!fileExists(path))
            continue;
        Bytes d = readFile(path, 16u << 20);
        if (d.size() >= 12 && std::string(d.begin(), d.begin() + 12) == magic)
            return d;
    }
    throw std::runtime_error(stem + ".IFO cannot be read, nor its copy " + stem + ".BUP");
}

std::string hms(const double seconds)
{
    const auto s = static_cast<long long>(seconds + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

std::string minutesText(const double seconds)
{
    const auto s = static_cast<long long>(seconds + 0.5);
    if (s % 60 == 0)
        return std::to_string(s / 60) + (s == 60 ? " minute" : " minutes");
    return std::to_string(s) + " seconds";
}

using CellKey = std::pair<uint32_t, uint32_t>;  // first and last sector

std::vector<CellKey> playedCells(const DvdTitle& t)
{
    std::vector<CellKey> out;
    for (const auto& c : t.cells)
        if (firstAngle(c))
            out.emplace_back(c.firstSector, c.lastSector);
    return out;
}
}  // namespace

std::string dvdVideoTs(const std::string& path)
{
    const std::string p = withoutEndSeparators(path);
    if (p.empty())
        return {};
    const size_t sep = p.find_last_of("/\\");
    const std::string last = strToUpperCase(sep == std::string::npos ? p : p.substr(sep + 1));
    const auto holdsIfo = [](const std::string& dir)
    { return fileExists(dir + "/VIDEO_TS.IFO") || fileExists(dir + "/VIDEO_TS.BUP"); };
    if (last == "VIDEO_TS" && isFolder(p) && holdsIfo(p))
        return p;
    if (isFolder(p + "/VIDEO_TS") && holdsIfo(p + "/VIDEO_TS"))
        return p + "/VIDEO_TS";
    return {};
}

DvdTitles listDvdTitles(const std::string& videoTs, const double minSeconds, const bool all)
{
    DvdTitles result;
    result.minSeconds = minSeconds;
    const Bytes vmg = ifo(videoTs, "VIDEO_TS", "DVDVIDEO-VMG");
    const Reader r{vmg, "VIDEO_TS.IFO"};
    const int setCount = r.u16(0x3E);
    if (setCount < 1 || setCount > 99)
        throw std::runtime_error("VIDEO_TS.IFO names " + std::to_string(setCount) + " title sets");
    std::vector<TitleSet> sets;
    for (int n = 1; n <= setCount; ++n)
    {
        char stem[16];
        snprintf(stem, sizeof stem, "VTS_%02d_0", n);
        TitleSet s;
        try
        {
            s = readSet(ifo(videoTs, stem, "DVDVIDEO-VTS"), n);
        }
        catch (const std::exception&)
        {
            // a title set that cannot be read gives no titles; the others are listed
        }
        for (int k = 1; k <= 9; ++k)
        {
            char vob[16];
            snprintf(vob, sizeof vob, "VTS_%02d_%d.VOB", n, k);
            const std::string path = videoTs + "/" + vob;
            if (!fileExists(path))
                break;
            s.vobSectors += getFileSize(path) / SECTOR;
            if (k == 1)
                s.encrypted = encryptedVob(readFile(path, 2u << 20));
        }
        sets.push_back(std::move(s));
    }

    // TT_SRPT: each title names its title set and its title in that set; its chapters name the program chains
    std::vector<DvdTitle> titles;
    const size_t srpt = static_cast<size_t>(r.u32(0xC4)) * SECTOR;
    const size_t count = (std::min)(static_cast<size_t>(r.u16(srpt)), size_t{99});
    for (size_t i = 0; i < count; ++i)
    {
        const size_t e = srpt + 8 + i * 12;
        DvdTitle t;
        t.discTitle = static_cast<int>(i + 1);
        t.angles = (std::max)(1, static_cast<int>(r.u8(e + 1)));
        t.titleSet = r.u8(e + 6);
        const int setTitle = r.u8(e + 7);
        if (t.titleSet < 1 || t.titleSet > setCount)
            continue;
        const TitleSet& s = sets[static_cast<size_t>(t.titleSet - 1)];
        if (setTitle < 1 || static_cast<size_t>(setTitle) > s.chapters.size())
        {
            t.note = "its title set has no chapter list for it";
            result.leftOut.push_back(t);
            continue;
        }
        std::vector<double> chainStart;
        for (const auto& [pgcn, pgn] : s.chapters[static_cast<size_t>(setTitle - 1)])
        {
            if (pgcn < 1 || static_cast<size_t>(pgcn) > s.pgcs.size())
                continue;
            const Pgc& g = s.pgcs[static_cast<size_t>(pgcn - 1)];
            if (pgn < 1 || static_cast<size_t>(pgn) > g.programs.size())
                continue;
            size_t k = 0;
            while (k < t.pgcs.size() && t.pgcs[k] != pgcn) ++k;
            if (k == t.pgcs.size())
            {
                chainStart.push_back(t.seconds);
                t.pgcs.push_back(pgcn);
                t.seconds += g.seconds;
                t.cells.insert(t.cells.end(), g.cells.begin(), g.cells.end());
                if (t.pgcs.size() == 1)
                {
                    for (size_t a = 0; a < s.audio.size(); ++a)
                        if (g.audio[a] >= 0)
                        {
                            // the stream id the stream has in the VOB files (in private stream 1 a sub-stream id)
                            DvdAudio au = s.audio[a];
                            const int n = g.audio[a];
                            au.track = au.coding == "AC-3"   ? 0x80 + n
                                       : au.coding == "DTS"  ? 0x88 + n
                                       : au.coding == "LPCM" ? 0xA0 + n
                                                             : 0xC0 + n;
                            t.audio.push_back(au);
                        }
                    for (size_t a = 0; a < s.subpictures.size(); ++a)
                        if (g.subpicture[a] >= 0)
                            t.subpictures.push_back(s.subpictures[a]);
                }
            }
            t.chapters.push_back(chainStart[k] +
                                 timeBefore(g, static_cast<size_t>(g.programs[static_cast<size_t>(pgn - 1)] - 1)));
        }
        t.encrypted = s.encrypted;
        // footage: each cell once; the part of it past the end of the VOB files
        std::set<CellKey> seen;
        uint64_t sectors = 0, missing = 0;
        for (const auto& c : t.cells)
        {
            if (!firstAngle(c) || !seen.insert({c.firstSector, c.lastSector}).second)
                continue;
            t.footage += c.seconds;
            const uint64_t n = c.lastSector >= c.firstSector ? uint64_t{c.lastSector} - c.firstSector + 1 : 0;
            sectors += n;
            if (c.lastSector >= s.vobSectors)
                missing += c.firstSector >= s.vobSectors ? n : uint64_t{c.lastSector} - s.vobSectors + 1;
        }
        t.missingShare = sectors ? static_cast<double>(missing) / static_cast<double>(sectors) : 0;
        if (t.cells.empty())
        {
            t.note = "it plays no cell";
            result.leftOut.push_back(t);
            continue;
        }
        titles.push_back(t);
    }

    // 1. Many cells under a second: pieces, not a title
    std::vector<DvdTitle*> left;
    for (auto& t : titles)
    {
        size_t tiny = 0, played = 0;
        for (const auto& c : t.cells)
            if (firstAngle(c))
            {
                ++played;
                tiny += c.seconds < 1.0;
            }
        if (played >= 20 && tiny * 2 > played)
            t.note = std::to_string(tiny) + " of its " + std::to_string(played) + " cells are shorter than a second";
        else
            left.push_back(&t);
    }
    // 2. The same cells as another title, in the same order or in another one. Of a group the one kept plays its
    // cells forward through the VOB files most often, which cells put in another order cannot do; then the lowest
    // number.
    std::map<std::vector<CellKey>, std::vector<DvdTitle*>> bySet;
    for (DvdTitle* t : left)
    {
        std::vector<CellKey> key = playedCells(*t);
        std::sort(key.begin(), key.end());
        bySet[key].push_back(t);
    }
    const auto forwardSteps = [](const DvdTitle& t)
    {
        const auto cells = playedCells(t);
        size_t n = 0;
        for (size_t i = 1; i < cells.size(); ++i) n += cells[i].first > cells[i - 1].second;
        return n;
    };
    for (auto& [key, group] : bySet)
    {
        if (group.size() < 2)
            continue;
        DvdTitle* kept = group.front();
        for (DvdTitle* t : group)
            if (forwardSteps(*t) > forwardSteps(*kept))
                kept = t;
        for (DvdTitle* t : group)
            if (t != kept)
                t->note = playedCells(*t) == playedCells(*kept)
                              ? "the same as DVD title " + std::to_string(kept->discTitle)
                              : "the same cells as DVD title " + std::to_string(kept->discTitle) + " in another order";
    }
    // 3. The minimum length
    std::vector<DvdTitle*> kept;
    for (DvdTitle* t : left)
    {
        if (!t->note.empty())
            continue;
        if (t->seconds < minSeconds)
            result.shortOnes.push_back(t->discTitle);
        else
            kept.push_back(t);
    }
    if (kept.empty() && !result.shortOnes.empty())
    {
        result.minApplied = false;
        result.shortOnes.clear();
        for (DvdTitle* t : left)
            if (t->note.empty())
                kept.push_back(t);
    }
    // 4. The main title: the most footage that is there (a title of a partial copy loses what is missing), then the
    // most chapters, then the lowest number; versions of it share most of its cells at about its length. Lengths count
    // in whole seconds, as listed: the episodes of a series disc are of one length, but their sums of cell times
    // differ in the last bits or by a frame, and that put them out of order and made any one of them the main title.
    std::stable_sort(kept.begin(), kept.end(),
                     [](const DvdTitle* a, const DvdTitle* b)
                     {
                         const auto fa = llround(a->footage * (1 - a->missingShare));
                         const auto fb = llround(b->footage * (1 - b->missingShare));
                         if (fa != fb)
                             return fa > fb;
                         return a->chapters.size() > b->chapters.size();
                     });
    // A disc can hold more than one main title: the episodes of a series, or several films. Another title is one too
    // when it plays cells of its own (it is no version of a main title) for at least 75 % of the footage of the main
    // title and at least 15 minutes, as on a Blu-ray (discTitles.cpp): each with its versions after it, the longest
    // first and the others in the order of the disc, or all in the order of the disc where they are of one length
    // (within 3 minutes), as episodes are. title=main takes the first.
    if (!kept.empty())
    {
        const auto present = [](const DvdTitle* t) { return t->footage * (1 - t->missingShare); };
        const auto shared = [](const DvdTitle& t, const DvdTitle& m)
        {
            const auto mainCells = playedCells(m);
            const std::set<CellKey> mainSet(mainCells.begin(), mainCells.end());
            double seconds = 0;
            std::set<CellKey> once;
            for (const auto& c : t.cells)
                if (firstAngle(c) && once.insert({c.firstSector, c.lastSector}).second &&
                    mainSet.count({c.firstSector, c.lastSector}))
                    seconds += c.seconds;
            return seconds;
        };
        std::vector<DvdTitle*> mains{kept.front()}, others;
        std::map<const DvdTitle*, std::vector<DvdTitle*>> versionsOf;
        for (size_t i = 1; i < kept.size(); ++i)
        {
            DvdTitle* t = kept[i];
            const auto of = std::find_if(mains.begin(), mains.end(),
                                         [&](const DvdTitle* m) {
                                             return t->titleSet == m->titleSet && t->footage >= 0.75 * m->footage &&
                                                    shared(*t, *m) >= 0.5 * t->footage;
                                         });
            if (of != mains.end())
                versionsOf[*of].push_back(t);
            else if (present(t) >= 0.75 * present(mains.front()) && present(t) >= 900)
                mains.push_back(t);
            else
                others.push_back(t);
        }
        result.video = sets[static_cast<size_t>(mains.front()->titleSet - 1)].video;
        // mains is by footage, the longest first
        const bool episodes = present(mains.front()) - present(mains.back()) <= 180;
        std::stable_sort(mains.begin() + (episodes ? 0 : 1), mains.end(),
                         [](const DvdTitle* a, const DvdTitle* b) { return a->discTitle < b->discTitle; });
        // the others by length in whole seconds, and those of one length in the order of the disc
        std::stable_sort(others.begin(), others.end(),
                         [](const DvdTitle* a, const DvdTitle* b)
                         {
                             const auto sa = llround(a->seconds), sb = llround(b->seconds);
                             return sa != sb ? sa > sb : a->discTitle < b->discTitle;
                         });
        kept.clear();
        for (DvdTitle* m : mains)
        {
            m->note = "main title";
            m->isMain = true;
            kept.push_back(m);
            const std::string number = std::to_string(kept.size());
            for (DvdTitle* t : versionsOf[m])
            {
                t->note = "version of title " + number;
                kept.push_back(t);
            }
        }
        kept.insert(kept.end(), others.begin(), others.end());
        result.mainTitles = static_cast<int>(mains.size());
    }
    for (DvdTitle* t : kept)
    {
        if (t->missingShare > 0)
            t->note += std::string(t->note.empty() ? "" : "; ") + "missing " +
                       std::to_string(static_cast<int>(t->missingShare * 100 + 0.5)) + " % of its cells";
        result.titles.push_back(*t);
    }
    const std::set<const DvdTitle*> keptSet(kept.begin(), kept.end());
    for (const auto& t : titles)
        if (!keptSet.count(&t) && !t.note.empty())
            result.leftOut.push_back(t);
    std::sort(result.leftOut.begin(), result.leftOut.end(),
              [](const DvdTitle& a, const DvdTitle& b) { return a.discTitle < b.discTitle; });
    if (all)
    {
        for (const auto& t : result.leftOut) result.titles.push_back(t);
        for (const auto& t : titles)
            if (std::find(result.shortOnes.begin(), result.shortOnes.end(), t.discTitle) != result.shortOnes.end())
            {
                DvdTitle s = t;
                s.note = "under " + minutesText(minSeconds);
                result.titles.push_back(s);
            }
        result.leftOut.clear();
        result.shortOnes.clear();
    }
    for (size_t i = 0; i < result.titles.size(); ++i) result.titles[i].number = static_cast<int>(i + 1);
    return result;
}

// "The disc has 4 main titles: 1, 2, 3, 4. title=main takes title 1."
static std::string mainTitlesText(const DvdTitles& t)
{
    std::string numbers;
    for (const auto& title : t.titles)
        if (title.isMain)
            numbers += (numbers.empty() ? "" : ", ") + std::to_string(title.number);
    return "The disc has " + std::to_string(t.mainTitles) + " main titles: " + numbers + ". title=main takes title " +
           numbers.substr(0, numbers.find(',')) + ".";
}

std::string describeDvdTitles(const DvdTitles& t)
{
    std::ostringstream s;
    if (!t.video.empty())
        s << "DVD-Video, " << t.video << "\n";
    if (t.titles.empty())
        s << "No title of this disc is one to mux.\n";
    if (!t.minApplied)
        s << "No title of this disc is " << minutesText(t.minSeconds) << " long, so the shorter ones are listed.\n";
    const size_t width = t.titles.size() >= 10 ? 2 : 1;
    for (const auto& title : t.titles)
    {
        std::string number = std::to_string(title.number);
        number.insert(0, width - (std::min)(width, number.size()), ' ');
        char line[160];
        snprintf(line, sizeof(line), "Title %s  DVD title %2d  %9s  %3zu %-8s", number.c_str(), title.discTitle,
                 hms(title.seconds).c_str(), title.chapters.size(),
                 title.chapters.size() == 1 ? "chapter" : "chapters");
        s << line;
        if (title.angles > 1)
            s << "  " << title.angles << " angles";
        std::string audio;
        for (const auto& a : title.audio)
            audio += (audio.empty() ? "" : ", ") + (a.lang.empty() ? std::string("--") : a.lang) + " " + a.coding +
                     " " + std::to_string(a.channels) + "ch";
        if (!audio.empty())
            s << "  audio " << audio;
        std::string subs;
        for (const auto& l : title.subpictures) subs += (subs.empty() ? "" : " ") + (l.empty() ? std::string("--") : l);
        if (!subs.empty())
            s << "  subtitles " << subs;
        if (title.encrypted)
            s << "  ENCRYPTED";
        if (!title.note.empty())
            s << "  (" << title.note << ")";
        s << '\n';
    }
    if (t.mainTitles > 1)
        s << '\n' << mainTitlesText(t) << '\n';
    if (!t.leftOut.empty() || !t.shortOnes.empty())
    {
        s << "\nLeft out:\n";
        for (const auto& title : t.leftOut) s << "  DVD title " << title.discTitle << ": " << title.note << '\n';
        if (!t.shortOnes.empty())
        {
            s << "  " << t.shortOnes.size() << (t.shortOnes.size() == 1 ? " title" : " titles") << " under "
              << minutesText(t.minSeconds) << ": DVD title";
            for (const int n : t.shortOnes) s << ' ' << n;
            s << '\n';
        }
        s << "--all-playlists lists them too, and --min-title-length=<seconds> sets the length a title needs.\n";
    }
    bool encrypted = false;
    for (const auto& title : t.titles) encrypted |= title.encrypted;
    if (encrypted)
        s << "\nThe video of the titles marked ENCRYPTED is still encrypted, and tsMuxeR cannot read it.\n";
    return s.str();
}

namespace
{
// A VOB file of a title set: the first sector of it in the joined files, and how many it has
struct Vob
{
    std::string path;
    uint64_t first = 0;
    uint64_t sectors = 0;
};

std::vector<Vob> titleSetVobs(const std::string& videoTs, const int set)
{
    std::vector<Vob> vobs;
    uint64_t first = 0;
    for (int k = 1; k <= 9; ++k)
    {
        char name[16];
        snprintf(name, sizeof name, "VTS_%02d_%d.VOB", set, k);
        const std::string path = videoTs + "/" + name;
        if (!fileExists(path))
            break;
        const uint64_t sectors = getFileSize(path) / SECTOR;
        vobs.push_back({path, first, sectors});
        first += sectors;
    }
    return vobs;
}

// Sectors first to last of the joined VOB files, as parts of the files; a part that follows on from the one before in
// the same file is joined to it
void addSectors(const std::vector<Vob>& vobs, const uint64_t first, const uint64_t last,
                std::vector<discImage::Piece>& out)
{
    for (const Vob& v : vobs)
    {
        if (v.sectors == 0)
            continue;
        const uint64_t lo = (std::max)(first, v.first), hi = (std::min)(last, v.first + v.sectors - 1);
        if (lo > hi)
            continue;
        const uint64_t offset = (lo - v.first) * SECTOR, length = (hi - lo + 1) * SECTOR;
        if (!out.empty() && out.back().file == v.path && out.back().offset + out.back().length == offset)
            out.back().length += length;
        else
            out.push_back({v.path, offset, length});
    }
}

Bytes readSector(const std::vector<Vob>& vobs, const uint64_t sector)
{
    for (const Vob& v : vobs)
        if (sector >= v.first && sector < v.first + v.sectors)
        {
            File file;
            Bytes b(SECTOR);
            if (!file.open(v.path.c_str(), File::ofRead | File::ofOpenExisting) ||
                file.seek(static_cast<int64_t>((sector - v.first) * SECTOR)) < 0 ||
                file.read(b.data(), static_cast<uint32_t>(SECTOR)) != static_cast<int>(SECTOR))
                return {};
            return b;
        }
    return {};
}

// The pieces of the first angle in the first cell of an angle block. The angles are interleaved in pieces (ILVUs),
// each beginning with a NAV pack whose DSI says where the piece ends and where the next one of the same angle begins
// (ILVU_EA and ILVU_SA, relative to the start of the piece; 0xFFFFFFFF after the last, 0 and 0 when the cell is not
// interleaved). False when a NAV pack cannot be read where one has to be.
bool firstAnglePieces(const std::vector<Vob>& vobs, const DvdCell& cell,
                      std::vector<std::pair<uint64_t, uint64_t>>& ranges)
{
    uint64_t sector = cell.firstSector;
    for (int guard = 0; sector <= cell.lastSector && guard < 1000000; ++guard)
    {
        const Bytes pack = readSector(vobs, sector);
        // the DSI packet: private stream 2 at byte 1024 of the NAV pack, its sub-stream id 0x01 after the PES header
        if (pack.size() < SECTOR || pack[1024] != 0 || pack[1025] != 0 || pack[1026] != 1 || pack[1027] != 0xBF ||
            pack[1030] != 0x01)
            return false;
        const auto be32 = [&pack](const size_t p)
        {
            return static_cast<uint32_t>(pack[p]) << 24 | static_cast<uint32_t>(pack[p + 1]) << 16 |
                   static_cast<uint32_t>(pack[p + 2]) << 8 | pack[p + 3];
        };
        const uint32_t ilvuEnd = be32(1065), nextStart = be32(1069);
        if (ilvuEnd == 0 && nextStart == 0)
        {
            ranges.emplace_back(sector, cell.lastSector);
            return true;
        }
        ranges.emplace_back(sector, (std::min)(sector + ilvuEnd, static_cast<uint64_t>(cell.lastSector)));
        if (nextStart == 0xFFFFFFFF || nextStart == 0)
            return true;
        sector += nextStart;
    }
    return true;
}
}  // namespace

std::string dvdTitleSource(const std::string& videoTs, const std::string& title,
                           std::vector<std::pair<int, std::string>>* languages, std::vector<double>* chapters,
                           const bool discNumber, const double minSeconds, const bool all, int* discTitle)
{
    // By the disc's own number every title of the disc can be named, the short and the left out ones too
    const DvdTitles list = listDvdTitles(videoTs, minSeconds, all || discNumber);
    if (list.titles.empty())
        throw std::runtime_error(videoTs + " has no title to mux: every title is left out. List the disc to see why");
    const DvdTitle* t = nullptr;
    if (discNumber)
    {
        const bool digits =
            !title.empty() && title.size() <= 2 && title.find_first_not_of("0123456789") == std::string::npos;
        const int n = digits ? std::stoi(title) : 0;
        for (const auto& candidate : list.titles)
            if (candidate.discTitle == n)
                t = &candidate;
        if (!t)
            throw std::runtime_error("dvd-title=" + title + " is not a title of " + videoTs +
                                     ": give the number of the DVD title as the listing shows it");
    }
    else if (strToLowerCase(title) == "main")
    {
        t = &list.titles.front();
        if (list.mainTitles > 1)
            LTRACE(LT_INFO, 2, videoTs << ": " << mainTitlesText(list));
    }
    else
    {
        const bool digits =
            !title.empty() && title.size() <= 4 && title.find_first_not_of("0123456789") == std::string::npos;
        const int n = digits ? std::stoi(title) : 0;
        if (n < 1 || n > static_cast<int>(list.titles.size()))
            throw std::runtime_error("title=" + title + " is not a title of " + videoTs + ", which has " +
                                     std::to_string(list.titles.size()) +
                                     (list.titles.size() == 1 ? " title" : " titles") + ": give a number from 1 to " +
                                     std::to_string(list.titles.size()) + ", or main");
        t = &list.titles[static_cast<size_t>(n - 1)];
    }
    if (t->encrypted)
        throw std::runtime_error("DVD title " + std::to_string(t->discTitle) + " of " + videoTs +
                                 " is still encrypted, and tsMuxeR cannot read it");
    const std::vector<Vob> vobs = titleSetVobs(videoTs, t->titleSet);
    if (vobs.empty())
        throw std::runtime_error("the video of DVD title " + std::to_string(t->discTitle) + " of " + videoTs +
                                 " (its VOB files) is not there");
    // The cells of the first angle, in runs whose timestamps follow on: a run ends at a cell marked as a break of the
    // timestamps (STC discontinuity) and where the sectors jump
    std::vector<std::vector<discImage::Piece>> runs;
    int64_t lastSector = -2;
    for (const DvdCell& c : t->cells)
    {
        if (c.angle && c.block != 1)
            continue;  // another angle of an angle block
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        if (!c.angle || !firstAnglePieces(vobs, c, ranges))
        {
            ranges.clear();
            ranges.emplace_back(c.firstSector, c.lastSector);
        }
        if (runs.empty() || c.stcBreak || static_cast<int64_t>(ranges.front().first) != lastSector + 1)
            runs.emplace_back();
        for (const auto& [first, last] : ranges) addSectors(vobs, first, last, runs.back());
        lastSector = static_cast<int64_t>(ranges.back().second);
    }
    std::string source;
    for (size_t k = 0; k < runs.size(); ++k)
    {
        if (runs[k].empty())
            continue;
        const std::string name =
            videoTs + "/DVD title " + std::to_string(t->discTitle) + " part " + std::to_string(k + 1) + ".VOB";
        discImage::addVirtualFile(name, runs[k]);
        source += (source.empty() ? "\"" : "+\"") + name + "\"";
    }
    if (languages)
        for (const auto& a : t->audio)
            if (!a.lang.empty())
                languages->emplace_back(a.track, a.lang);
    if (chapters)
        *chapters = t->chapters;
    if (discTitle)
        *discTitle = t->discTitle;
    return source;
}
