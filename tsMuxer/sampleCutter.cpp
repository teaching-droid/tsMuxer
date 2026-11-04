#include "sampleCutter.h"

#include <types/types.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <stdexcept>

#include "zlib.h"

namespace sample
{
namespace
{
using Bytes = std::vector<uint8_t>;
constexpr int64_t NO_TIME = INT64_MIN;
constexpr int64_t MB = 1000000;

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& name)
{
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string()
                                                                                   : lower(name.substr(dot));
}

std::string mb(const int64_t bytes)
{
    char s[32];
    std::snprintf(s, sizeof(s), "%.1f MB", static_cast<double>(bytes) / MB);
    return s;
}

std::string clock(const double seconds)
{
    const auto t = static_cast<int64_t>(seconds * 1000 + 0.5);
    char s[32];
    std::snprintf(s, sizeof(s), "%d:%02d:%02d.%03d", static_cast<int>(t / 3600000), static_cast<int>(t / 60000 % 60),
                  static_cast<int>(t / 1000 % 60), static_cast<int>(t % 1000));
    return s;
}

// A file on the drive, opened by the system: not through File, which reads a source tsMuxeR has turned into a Matroska
// file in memory (an Ogg file, a subtitle script) as that Matroska file once the source has been listed
std::FILE* openFile(const std::string& name, const bool write)
{
#ifdef _WIN32
    return _wfopen(toWide(windowsPath(name)).data(), write ? L"wb" : L"rb");
#else
    return std::fopen(name.c_str(), write ? "wb" : "rb");
#endif
}

bool seekFile(std::FILE* f, const int64_t pos, const int whence)
{
#ifdef _WIN32
    return _fseeki64(f, pos, whence) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), whence) == 0;
#endif
}

// The source, read at any place
class Source
{
   public:
    explicit Source(const std::string& name) : m_file(openFile(name, false))
    {
        if (m_file == nullptr || !seekFile(m_file, 0, SEEK_END))
            throw std::runtime_error("The file " + name + " cannot be opened.");
#ifdef _WIN32
        m_size = _ftelli64(m_file);
#else
        m_size = ftello(m_file);
#endif
    }
    ~Source()
    {
        if (m_file)
            std::fclose(m_file);
    }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    [[nodiscard]] int64_t size() const { return m_size; }
    // up to n bytes from pos: fewer at the end of the file
    Bytes read(const int64_t pos, const int64_t n) const
    {
        Bytes b;
        if (pos < 0 || pos >= m_size || n <= 0 || !seekFile(m_file, pos, SEEK_SET))
            return b;
        b.resize(static_cast<size_t>(std::min(n, m_size - pos)));
        b.resize(std::fread(b.data(), 1, b.size(), m_file));
        return b;
    }
    // the first place at or after from where match(bytes, at) holds, searched up to limit bytes; -1 if none
    int64_t find(int64_t from, const int64_t limit, const size_t window,
                 const std::function<bool(const uint8_t*, size_t)>& match) const
    {
        constexpr int64_t CHUNK = 1 << 20;
        const int64_t end = std::min(m_size, from + limit);
        while (from < end)
        {
            const Bytes b = read(from, std::min(CHUNK, end - from) + static_cast<int64_t>(window));
            if (b.size() < window)
                return -1;
            const size_t stop = std::min(b.size() - window + 1, static_cast<size_t>(std::min(CHUNK, end - from)));
            for (size_t i = 0; i < stop; ++i)
                if (match(b.data() + i, b.size() - i))
                    return from + static_cast<int64_t>(i);
            from += static_cast<int64_t>(stop);
        }
        return -1;
    }

    // the last place before `before` (and at most limit bytes before it) where match holds; -1 if none
    int64_t findBack(const int64_t before, const int64_t limit, const size_t window,
                     const std::function<bool(const uint8_t*, size_t)>& match) const
    {
        constexpr int64_t CHUNK = 1 << 20;
        const int64_t floor = std::max<int64_t>(0, before - limit);
        for (int64_t to = before; to > floor;)
        {
            const int64_t from = std::max(floor, to - CHUNK);
            const Bytes b = read(from, to - from + static_cast<int64_t>(window));
            for (int64_t i =
                     std::min<int64_t>(to - from, static_cast<int64_t>(b.size()) - static_cast<int64_t>(window));
                 i-- > 0;)
                if (match(b.data() + i, b.size() - static_cast<size_t>(i)))
                    return from + i;
            to = from;
        }
        return -1;
    }

   private:
    std::FILE* m_file;
    int64_t m_size = 0;
};

class Target
{
   public:
    explicit Target(const std::string& name) : m_file(openFile(name, true))
    {
        if (m_file == nullptr)
            throw std::runtime_error("The file " + name + " cannot be written.");
    }
    ~Target()
    {
        if (m_file)
            std::fclose(m_file);
    }
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
    void write(const uint8_t* data, const size_t n)
    {
        if (n > 0 && std::fwrite(data, 1, n, m_file) != n)
            throw std::runtime_error("The sample cannot be written: the disc may be full.");
        m_written += static_cast<int64_t>(n);
    }
    void write(const Bytes& b) { write(b.data(), b.size()); }
    void copy(const Source& src, int64_t from, const int64_t to)
    {
        while (from < to)
        {
            const Bytes b = src.read(from, std::min<int64_t>(to - from, 4 << 20));
            if (b.empty())
                break;
            write(b);
            from += static_cast<int64_t>(b.size());
        }
    }
    [[nodiscard]] int64_t written() const { return m_written; }
    void close()
    {
        std::FILE* const f = m_file;
        m_file = nullptr;
        if (f && std::fclose(f) != 0)
            throw std::runtime_error("The sample cannot be written: the disc may be full.");
    }

   private:
    std::FILE* m_file;
    int64_t m_written = 0;
};

// Where a part of maxBytes is taken for the request, before it is moved to a place a reader can begin at: from the
// start, from the end, or with the time's place in the middle
int64_t roughStart(const Request& r, const int64_t size, const int64_t atByte)
{
    if (r.take == Take::Start)
        return 0;
    if (r.take == Take::End)
        return std::max<int64_t>(0, size - r.maxBytes);
    return std::clamp<int64_t>(atByte - r.maxBytes / 2, 0, std::max<int64_t>(0, size - r.maxBytes));
}

// The place of a time in a file whose clock rises with its bytes: the last place whose clock is not after the time
int64_t bisect(const int64_t size, const int64_t target, const int64_t step,
               const std::function<int64_t(int64_t)>& clockAt)
{
    int64_t lo = 0;
    int64_t hi = size;
    while (hi - lo > step)
    {
        const int64_t mid = lo + (hi - lo) / 2;
        const int64_t t = clockAt(mid);
        if (t == NO_TIME || t > target)
            hi = mid;
        else
            lo = mid;
    }
    return lo;
}

// ---- MPEG-TS and M2TS (H.222.0 2.4.3) ----

struct TsLayout
{
    int packet = 0;  // 188, or 192 with the 4 byte time code of a Blu-ray clip in front
    int sync = 0;    // the place of 0x47 in a packet
    int64_t first = 0;
};

bool tsLayout(const Source& src, TsLayout& ts)
{
    const Bytes b = src.read(0, 192 * 8);
    for (const int packet : {188, 192})
        for (int64_t first = 0; first < packet; ++first)
        {
            const int sync = packet == 192 ? 4 : 0;
            int hits = 0;
            for (int k = 0; k < 5; ++k)
            {
                const size_t p = static_cast<size_t>(first + sync + k * packet);
                hits += p < b.size() && b[p] == 0x47 ? 1 : 0;
            }
            if (hits == 5)
            {
                ts = {packet, sync, first};
                return true;
            }
        }
    return false;
}

int64_t tsAlign(const TsLayout& ts, const int64_t pos)
{
    return pos <= ts.first ? ts.first : ts.first + (pos - ts.first) / ts.packet * ts.packet;
}

// The clock of the first packet at or after pos that gives one: its PCR, or when a stream has none the PTS of a PES
int64_t tsClock(const Source& src, const TsLayout& ts, const int64_t pos, const bool usePts)
{
    constexpr int64_t SCAN = 4 << 20;
    const int64_t from = tsAlign(ts, pos);
    const Bytes b = src.read(from, SCAN);
    for (size_t p = 0; p + static_cast<size_t>(ts.packet) <= b.size(); p += static_cast<size_t>(ts.packet))
    {
        const uint8_t* h = b.data() + p + ts.sync;
        if (h[0] != 0x47)
            return NO_TIME;
        const int afc = h[3] >> 4 & 3;
        if (!usePts)
        {
            if ((afc & 2) && h[4] >= 7 && (h[5] & 0x10))
                return static_cast<int64_t>(h[6]) << 25 | h[7] << 17 | h[8] << 9 | h[9] << 1 | h[10] >> 7;
            continue;
        }
        if (!(h[1] & 0x40))
            continue;
        const size_t start = 4 + ((afc & 2) ? 1 + h[4] : 0);
        if (start + 14 > 188 || !(afc & 1))
            continue;
        const uint8_t* e = h + start;
        if (e[0] == 0 && e[1] == 0 && e[2] == 1 && (e[7] & 0x80))
            return static_cast<int64_t>(e[9] >> 1 & 7) << 30 | e[10] << 22 | (e[11] >> 1) << 15 | e[12] << 7 |
                   e[13] >> 1;
    }
    return NO_TIME;
}

void cutTs(const Request& r, const Source& src, Target& out, Result& res)
{
    TsLayout ts;
    if (!tsLayout(src, ts))
        throw std::runtime_error("The source looked like a transport stream, but its packets were not found.");
    res.kind = ts.packet == 192 ? "M2TS (192 byte packets)" : "MPEG-TS (188 byte packets)";
    int64_t atByte = 0;
    if (r.take == Take::At)
    {
        bool usePts = tsClock(src, ts, 0, false) == NO_TIME;
        const int64_t t0 = tsClock(src, ts, 0, usePts);
        if (t0 == NO_TIME)
            throw std::runtime_error(
                "The transport stream gives no clock (PCR) and no PTS near its start, so a time "
                "cannot be found in it. Take its start or its end instead.");
        constexpr int64_t WRAP = int64_t{1} << 33;
        const auto target = static_cast<int64_t>(r.atSeconds * 90000);
        atByte = bisect(src.size(), target, ts.packet * 256,
                        [&](const int64_t pos)
                        {
                            const int64_t t = tsClock(src, ts, pos, usePts);
                            return t == NO_TIME ? NO_TIME : (t - t0 + WRAP) % WRAP;
                        });
        res.lines.push_back(std::string("The time was found by the ") + (usePts ? "PTS" : "PCR") +
                            " of the packets: " + clock(r.atSeconds) + " is near byte " + std::to_string(atByte) + ".");
    }
    int64_t start = tsAlign(ts, roughStart(r, src.size(), atByte));
    bool keyFound = false;
    if (start > ts.first)
    {
        // the next key picture: a packet of video that begins a PES with the random access indicator set (H.222.0
        // 2.4.3.5), and the PAT before it, so that the PAT and PMT come first and the video begins where it can be
        // decoded
        const Bytes b = src.read(start, 32 << 20);
        int64_t key = -1;
        for (size_t p = 0; p + static_cast<size_t>(ts.packet) <= b.size() && key < 0;
             p += static_cast<size_t>(ts.packet))
        {
            const uint8_t* h = b.data() + p + ts.sync;
            if (h[0] != 0x47)
                break;
            const int afc = h[3] >> 4 & 3;
            const size_t at = (afc & 2) ? 5 + h[4] : 4;
            if (!(h[1] & 0x40) || !(afc & 1) || at + 9 > 188)
                continue;
            const uint8_t* e = h + at;
            if (e[0] != 0 || e[1] != 0 || e[2] != 1 || !((e[3] >= 0xE0 && e[3] <= 0xEF) || e[3] == 0xFD))
                continue;
            const bool flagged = (afc & 2) && h[4] > 0 && (h[5] & 0x40);
            // a clip of a Blu-ray disc does not set the flag (its EP map names the key pictures): the parameters of
            // the sequence at the start of the PES then mark one, an H.264 SPS, an HEVC VPS, an MPEG-2 or VC-1
            // sequence header
            bool sequence = false;
            for (size_t i = 9 + e[8]; !flagged && !sequence && at + i + 5 <= 188; ++i)
                if (e[i] == 0 && e[i + 1] == 0 && e[i + 2] == 1)
                {
                    const uint8_t t = e[i + 3];
                    sequence = (t & 0x9F) == 0x07 && (t & 0x60) != 0;  // H.264 SPS, nal_ref_idc not 0
                    sequence = sequence || (t == 0x40 && e[i + 4] == 0x01) || t == 0xB3 || t == 0x0F;
                }
            if (flagged || sequence)
                key = start + static_cast<int64_t>(p);
        }
        if (key >= 0)
        {
            const int64_t from = tsAlign(ts, std::max(ts.first, key - (2 << 20)));
            const Bytes before = src.read(from, key - from);
            int64_t pat = -1;
            for (size_t p = 0; p + static_cast<size_t>(ts.packet) <= before.size(); p += static_cast<size_t>(ts.packet))
            {
                const uint8_t* h = before.data() + p + ts.sync;
                if (h[0] == 0x47 && (h[1] & 0x40) && (h[1] & 0x1F) == 0 && h[2] == 0)
                    pat = from + static_cast<int64_t>(p);
            }
            start = pat >= 0 ? pat : key;
            keyFound = true;
        }
    }
    // else from a PAT on: the PAT and PMT, which come again every few tenths of a second, then come first
    if (start > ts.first && !keyFound)
    {
        int64_t pat = -1;
        const Bytes b = src.read(start, 8 << 20);
        for (size_t p = 0; p + static_cast<size_t>(ts.packet) <= b.size() && pat < 0;
             p += static_cast<size_t>(ts.packet))
        {
            const uint8_t* h = b.data() + p + ts.sync;
            if (h[0] == 0x47 && (h[1] & 0x40) && (h[1] & 0x1F) == 0 && h[2] == 0)
                pat = start + static_cast<int64_t>(p);
        }
        if (pat >= 0)
            start = pat;
        else
            res.lines.push_back("No PAT was found after the place, so the sample begins at a packet without one.");
    }
    const int64_t end = std::min(src.size(), start + (r.maxBytes / ts.packet) * ts.packet);
    out.copy(src, start, end);
    res.lines.push_back("Bytes " + std::to_string(start) + " to " + std::to_string(end - 1) + " of the source (" +
                        mb(end - start) + "), from " +
                        (start == ts.first ? std::string("its start")
                         : keyFound        ? std::string("the PAT before a key picture")
                                           : std::string("a packet with the PAT")) +
                        ".");
}

// ---- Matroska (RFC 9559) ----

struct Element
{
    uint32_t id = 0;
    int64_t pos = 0;    // of the ID
    int64_t data = 0;   // of the data
    int64_t size = -1;  // of the data; -1 unknown
    [[nodiscard]] int64_t end(const int64_t limit) const { return size < 0 ? limit : data + size; }
};

bool readElement(const Bytes& b, const size_t at, Element& e, const int64_t base)
{
    if (at >= b.size() || b[at] == 0)
        return false;
    int idLen = 1;
    while (idLen <= 4 && !(b[at] & (0x80 >> (idLen - 1)))) ++idLen;
    if (idLen > 4 || at + idLen >= b.size())
        return false;
    uint32_t id = 0;
    for (int i = 0; i < idLen; ++i) id = id << 8 | b[at + i];
    const size_t s = at + idLen;
    if (b[s] == 0)
        return false;
    int sizeLen = 1;
    while (sizeLen <= 8 && !(b[s] & (0x80 >> (sizeLen - 1)))) ++sizeLen;
    if (sizeLen > 8 || s + sizeLen > b.size())
        return false;
    uint64_t size = b[s] & (0xFF >> sizeLen);
    bool allOnes = size == (0xFFu >> sizeLen);
    for (int i = 1; i < sizeLen; ++i)
    {
        size = size << 8 | b[s + i];
        allOnes = allOnes && b[s + i] == 0xFF;
    }
    e.id = id;
    e.pos = base + static_cast<int64_t>(at);
    e.data = e.pos + idLen + sizeLen;
    e.size = allOnes ? -1 : static_cast<int64_t>(size);
    return true;
}

Element elementAt(const Source& src, const int64_t pos)
{
    Element e;
    if (!readElement(src.read(pos, 12), 0, e, pos))
        e.id = 0;
    return e;
}

constexpr uint32_t MKV_SEGMENT = 0x18538067;
constexpr uint32_t MKV_CLUSTER = 0x1F43B675;
constexpr uint32_t MKV_INFO = 0x1549A966;
constexpr uint32_t MKV_TRACKS = 0x1654AE6B;
constexpr uint32_t MKV_CHAPTERS = 0x1043A770;

// The title and the track, edition and chapter names in a copy of Info, Tracks or Chapters, each byte made an "x", so
// that every size stays as it was
void blankNames(Bytes& b, size_t from, const size_t to)
{
    // Info, Tracks, Chapters, TrackEntry, EditionEntry, ChapterAtom, ChapterDisplay, and the EditionDisplay of later
    // Matroska versions
    static const uint32_t masters[] = {MKV_INFO, MKV_TRACKS, MKV_CHAPTERS, 0xAE, 0x45B9, 0xB6, 0x80, 0x4520};
    // Title, SegmentFilename, PrevFilename, NextFilename (RFC 9559 5.1.2), Name (5.1.4.1.18), ChapString, EditionString
    static const uint32_t names[] = {0x7BA9, 0x7384, 0x3C83AB, 0x3E83BB, 0x536E, 0x85, 0x4521};
    while (from < to)
    {
        Element e;
        if (!readElement(b, from, e, 0) || e.size < 0 || e.data + e.size > static_cast<int64_t>(to))
            return;
        if (std::find(std::begin(masters), std::end(masters), e.id) != std::end(masters))
            blankNames(b, static_cast<size_t>(e.data), static_cast<size_t>(e.data + e.size));
        else if (e.id == 0xBF)  // CRC-32: it would not match the blanked bytes; a Void of the same size in its place
            b[static_cast<size_t>(e.pos)] = 0xEC;
        else if (std::find(std::begin(names), std::end(names), e.id) != std::end(names))
            std::fill(b.begin() + static_cast<std::ptrdiff_t>(e.data),
                      b.begin() + static_cast<std::ptrdiff_t>(e.data + e.size), static_cast<uint8_t>('x'));
        from = static_cast<size_t>(e.data + e.size);
    }
}

void cutMatroska(const Request& r, const Source& src, Target& out, Result& res)
{
    res.kind = "Matroska";
    const Element ebml = elementAt(src, 0);
    if (ebml.id != 0x1A45DFA3 || ebml.size < 0)
        throw std::runtime_error("The Matroska file has no EBML header.");
    const Element segment = elementAt(src, ebml.end(0));
    if (segment.id != MKV_SEGMENT)
        throw std::runtime_error("The Matroska file has no Segment after its EBML header.");
    const int64_t segmentEnd = std::min(src.size(), segment.end(src.size()));

    // the elements of the segment: the clusters with their times, and the headers a reader needs
    struct Cluster
    {
        int64_t pos;
        int64_t end;
        int64_t time;
    };
    std::vector<Cluster> clusters;
    std::vector<Element> kept;
    int64_t timestampScale = 1000000;
    for (int64_t pos = segment.data; pos < segmentEnd;)
    {
        Element e = elementAt(src, pos);
        if (e.id == 0)
        {
            // damaged, or a cluster of unknown size ended: the next cluster by its ID
            const int64_t next = src.find(pos + 1, 64 << 20, 4, [](const uint8_t* p, size_t)
                                          { return p[0] == 0x1F && p[1] == 0x43 && p[2] == 0xB6 && p[3] == 0x75; });
            if (next < 0)
                break;
            if (!clusters.empty() && clusters.back().end > next)
                clusters.back().end = next;
            pos = next;
            continue;
        }
        if (e.id == MKV_CLUSTER)
        {
            int64_t end = e.size < 0 ? -1 : std::min(segmentEnd, e.end(segmentEnd));
            if (end < 0)
            {
                // a cluster of unknown size ends where the next one begins, or the index, tags or attachments
                // after the last one (RFC 9559 6.3: an element of unknown size ends at an element that is not its
                // child)
                const int64_t next =
                    src.find(e.data, 256 << 20, 4,
                             [](const uint8_t* p, size_t)
                             {
                                 static const uint8_t ids[][4] = {{0x1F, 0x43, 0xB6, 0x75}, {0x1C, 0x53, 0xBB, 0x6B},
                                                                  {0x12, 0x54, 0xC3, 0x67}, {0x19, 0x41, 0xA4, 0x69},
                                                                  {0x11, 0x4D, 0x9B, 0x74}, {0x10, 0x43, 0xA7, 0x70}};
                                 for (const auto& id : ids)
                                     if (std::memcmp(p, id, 4) == 0)
                                         return true;
                                 return false;
                             });
                end = next < 0 ? segmentEnd : next;
            }
            int64_t time = clusters.empty() ? 0 : clusters.back().time;
            const Bytes head = src.read(e.data, 32);
            for (size_t at = 0; at < head.size();)
            {
                Element c;
                if (!readElement(head, at, c, e.data) || c.size < 0)
                    break;
                if (c.id == 0xE7)
                {
                    const Bytes v = src.read(c.data, c.size);
                    time = 0;
                    for (const uint8_t x : v) time = time << 8 | x;
                    break;
                }
                at = static_cast<size_t>(c.data + c.size - e.data);
            }
            clusters.push_back({e.pos, end, time});
            pos = end;
            continue;
        }
        if (e.size < 0)
            break;
        if (e.id == MKV_INFO || e.id == MKV_TRACKS || e.id == MKV_CHAPTERS)
            kept.push_back(e);
        if (e.id == MKV_INFO)
        {
            const Bytes info = src.read(e.data, std::min<int64_t>(e.size, 1 << 20));
            for (size_t at = 0; at < info.size();)
            {
                Element c;
                if (!readElement(info, at, c, e.data) || c.size < 0)
                    break;
                // TimestampScale: 1 to 8 bytes within Info; an empty one is the default (RFC 8794: an empty element
                // of an unsigned integer is its default), and so is a damaged one
                if (c.id == 0x2AD7B1 && c.size >= 1 && c.size <= 8 &&
                    c.data - e.data + c.size <= static_cast<int64_t>(info.size()))
                {
                    int64_t scale = 0;
                    for (int64_t i = 0; i < c.size; ++i)
                        scale = scale << 8 | info[static_cast<size_t>(c.data - e.data + i)];
                    if (scale > 0)
                        timestampScale = scale;
                }
                at = static_cast<size_t>(c.data + c.size - e.data);
            }
        }
        pos = e.end(segmentEnd);
    }
    if (clusters.empty())
        throw std::runtime_error("The Matroska file has no cluster, so it holds no frames.");

    // the headers: EBML, the segment of unknown size (it ends with the file), Info, Tracks and Chapters
    Bytes head = src.read(0, ebml.end(0));
    for (const uint8_t b : {0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) head.push_back(b);
    for (const Element& e : kept)
    {
        Bytes copy = src.read(e.pos, e.data + e.size - e.pos);
        if (!r.keepNames)
            blankNames(copy, 0, copy.size());
        head.insert(head.end(), copy.begin(), copy.end());
    }
    const int64_t budget = r.maxBytes - static_cast<int64_t>(head.size());
    if (budget <= 0)
        throw std::runtime_error("The headers of the Matroska file alone are larger than the sample may be.");

    size_t first = 0;
    if (r.take == Take::At)
    {
        const auto target = static_cast<int64_t>(r.atSeconds * 1e9 / static_cast<double>(timestampScale));
        size_t at = 0;
        while (at + 1 < clusters.size() && clusters[at + 1].time <= target + clusters.front().time) ++at;
        first = at;
        for (int64_t before = 0;
             first > 0 && before + (clusters[first - 1].end - clusters[first - 1].pos) <= budget / 2; --first)
            before += clusters[first - 1].end - clusters[first - 1].pos;
        res.lines.push_back("The time was found by the timestamps of the clusters: " + clock(r.atSeconds) +
                            " is in the cluster at byte " + std::to_string(clusters[at].pos) + ".");
    }
    else if (r.take == Take::End)
    {
        first = clusters.size();
        for (int64_t total = 0; first > 0 && total + (clusters[first - 1].end - clusters[first - 1].pos) <= budget;
             --first)
            total += clusters[first - 1].end - clusters[first - 1].pos;
        first = std::min(first, clusters.size() - 1);
    }
    out.write(head);
    size_t last = first;
    int64_t total = 0;
    for (; last < clusters.size() && total + (clusters[last].end - clusters[last].pos) <= budget; ++last)
        total += clusters[last].end - clusters[last].pos;
    if (last == first)
    {
        // one cluster longer than the sample: its start
        out.copy(src, clusters[first].pos, clusters[first].pos + budget);
        res.lines.push_back("The cluster at byte " + std::to_string(clusters[first].pos) +
                            " alone is longer than the sample; its first " + mb(budget) + " are taken.");
    }
    else
    {
        out.copy(src, clusters[first].pos, clusters[last - 1].end);
        res.lines.push_back("Clusters " + std::to_string(first + 1) + " to " + std::to_string(last) + " of " +
                            std::to_string(clusters.size()) + ", bytes " + std::to_string(clusters[first].pos) +
                            " to " + std::to_string(clusters[last - 1].end - 1) + " of the source (" + mb(total) +
                            "), after the headers (EBML, Segment of unknown size, Info, Tracks, Chapters; " +
                            std::to_string(head.size()) + " bytes).");
    }
    res.lines.push_back(r.keepNames ? "The title and the names of the tracks and chapters are kept."
                                    : "The title and the names of the tracks and chapters are blanked to x, each as "
                                      "long as it was. Tags, attachments, the index (Cues) and the SeekHead are left "
                                      "out.");
}

// ---- MP4 and QuickTime (ISO/IEC 14496-12): from the start only ----

void cutMp4(const Request& r, const Source& src, Target& out, Result& res)
{
    res.kind = "MP4 / QuickTime";
    if (r.take != Take::Start)
        throw std::runtime_error(
            "An MP4 or QuickTime file keeps the place of every frame in one index (moov), so a "
            "part from its middle or end cannot be read without making that index anew. Take its "
            "start, or mux the file to MKV with tsMuxeR first and take the part from that.");
    int64_t moovEnd = -1;
    int64_t mdat = -1;
    int64_t mdatHeader = 0;
    int64_t mdatEnd = -1;
    int64_t firstMoof = -1;
    struct Box
    {
        int64_t pos, end;
        int header;
        bool mdat;
    };
    std::vector<Box> boxes;  // the top-level boxes
    for (int64_t pos = 0; pos + 8 <= src.size();)
    {
        const Bytes h = src.read(pos, 16);
        int64_t size = static_cast<int64_t>(h[0]) << 24 | h[1] << 16 | h[2] << 8 | h[3];
        int header = 8;
        if (size == 1 && h.size() >= 16)
        {
            size = 0;
            for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
            header = 16;
        }
        else if (size == 0)
            size = src.size() - pos;
        if (size < header)
            break;
        if (std::memcmp(h.data() + 4, "moov", 4) == 0)
            moovEnd = pos + size;
        if (std::memcmp(h.data() + 4, "mdat", 4) == 0 && mdat < 0)
        {
            mdat = pos;
            mdatHeader = header;
            mdatEnd = pos + size;
        }
        if (std::memcmp(h.data() + 4, "moof", 4) == 0 && firstMoof < 0)
            firstMoof = pos;
        boxes.push_back({pos, std::min(pos + size, src.size()), header, std::memcmp(h.data() + 4, "mdat", 4) == 0});
        pos += size;
    }
    if (moovEnd < 0)
        throw std::runtime_error("The MP4 file has no index (moov).");
    if (mdat >= 0 && mdat < moovEnd)
        throw std::runtime_error(
            "The index (moov) of this MP4 file comes after its frames, so its start holds frames "
            "without the index to read them by. Mux the file to MKV with tsMuxeR first and take "
            "the part from that.");
    if (moovEnd > r.maxBytes)
        throw std::runtime_error("The index (moov) of this MP4 file alone is larger than the sample may be.");
    if (firstMoof >= 0)
    {
        // A fragmented file: each fragment (moof and its mdat) says where its frames are. The boxes are taken whole
        // while they fit; a fragment that does not fit is taken with the start of its frames (mdat), whose size is
        // set to what the sample holds, as it is the last box of the sample.
        int64_t end = 0;
        size_t cut = boxes.size();
        for (size_t i = 0; i < boxes.size(); ++i)
        {
            if (boxes[i].end <= r.maxBytes)
            {
                end = boxes[i].end;
                continue;
            }
            if (boxes[i].mdat && boxes[i].pos + boxes[i].header < r.maxBytes)
            {
                cut = i;
                end = r.maxBytes;
            }
            break;
        }
        if (end <= firstMoof)
            throw std::runtime_error("The first fragment of this MP4 file does not begin within the sample.");
        Bytes b = src.read(0, end);
        if (cut < boxes.size())
        {
            const Box& m = boxes[cut];
            const int64_t size = end - m.pos;
            if (m.header == 16)
                for (int i = 0; i < 8; ++i)
                    b[static_cast<size_t>(m.pos + 15 - i)] = static_cast<uint8_t>(size >> (8 * i));
            else
                for (int i = 0; i < 4; ++i)
                    b[static_cast<size_t>(m.pos + 3 - i)] = static_cast<uint8_t>(size >> (8 * i));
        }
        out.write(b);
        res.lines.push_back("Bytes 0 to " + std::to_string(end - 1) + " of the source (" + mb(end) +
                            "): a fragmented file, its index (moov) and its first fragments (moof and mdat)" +
                            (cut < boxes.size() ? std::string(", the last of them cut, its mdat size set to what the "
                                                              "sample holds")
                                                : std::string()) +
                            ". A title or track names in the index are kept as they are.");
        return;
    }
    const int64_t end = std::min(src.size(), r.maxBytes);
    Bytes b = src.read(0, end);
    if (mdat >= 0 && mdat + mdatHeader <= end && mdatEnd > end)
    {
        // the frames box ends with the sample, not past it
        const int64_t size = end - mdat;
        if (mdatHeader == 16)
            for (int i = 0; i < 8; ++i) b[static_cast<size_t>(mdat + 15 - i)] = static_cast<uint8_t>(size >> (8 * i));
        else
            for (int i = 0; i < 4; ++i) b[static_cast<size_t>(mdat + 3 - i)] = static_cast<uint8_t>(size >> (8 * i));
    }
    out.write(b);
    res.lines.push_back("Bytes 0 to " + std::to_string(end - 1) + " of the source (" + mb(end) +
                        "): the index (moov) and the start of the frames (mdat), its size set to what the sample "
                        "holds. Frames the index names past the end are not in the sample. A title or track names "
                        "in the index are kept as they are.");
}

// ---- MPEG program streams: VOB, MPG (H.222.0 2.5.3) ----

int64_t packClock(const uint8_t* p)
{
    if ((p[4] & 0xC0) == 0x40)  // MPEG-2
        return static_cast<int64_t>(p[4] >> 3 & 7) << 30 | static_cast<int64_t>(p[4] & 3) << 28 | p[5] << 20 |
               (p[6] >> 3) << 15 | (p[6] & 3) << 13 | p[7] << 5 | p[8] >> 3;
    if ((p[4] & 0xF0) == 0x20)  // MPEG-1
        return static_cast<int64_t>(p[4] >> 1 & 7) << 30 | p[5] << 22 | (p[6] >> 1) << 15 | p[7] << 7 | p[8] >> 1;
    return NO_TIME;
}

bool isPack(const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0xBA; }

void cutProgramStream(const Request& r, const Source& src, Target& out, Result& res, const std::string& name)
{
    res.kind = "MPEG program stream (VOB, MPG)";
    int64_t atByte = 0;
    if (r.take == Take::At)
    {
        const auto clockAt = [&](const int64_t pos) -> int64_t
        {
            const int64_t p = src.find(pos, 1 << 20, 10, [](const uint8_t* b, size_t) { return isPack(b); });
            return p < 0 ? NO_TIME : packClock(src.read(p, 10).data());
        };
        const int64_t t0 = clockAt(0);
        if (t0 == NO_TIME)
            throw std::runtime_error(
                "The program stream gives no clock (SCR) at its start, so a time cannot be found "
                "in it. Take its start or its end instead.");
        atByte = bisect(src.size(), t0 + static_cast<int64_t>(r.atSeconds * 90000), 64 << 10, clockAt);
        res.lines.push_back("The time was found by the clock (SCR) of the packs: " + clock(r.atSeconds) +
                            " is near byte " + std::to_string(atByte) + ".");
    }
    int64_t start = roughStart(r, src.size(), atByte);
    if (start > 0)
    {
        // a pack whose video begins with a sequence header, where a decoder can start; else any pack
        constexpr int64_t SCAN = 16 << 20;
        int64_t found = -1;
        for (int64_t p = src.find(start, SCAN, 4, [](const uint8_t* b, size_t) { return isPack(b); });
             p >= 0 && p < start + SCAN && found < 0;)
        {
            const Bytes b = src.read(p, 4096);
            for (size_t i = 4; i + 4 <= b.size() && found < 0; ++i)
            {
                if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xBA)
                    break;
                if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xB3)
                    found = p;
            }
            p = src.find(p + 4, SCAN, 4, [](const uint8_t* x, size_t) { return isPack(x); });
        }
        if (found < 0)
        {
            found = src.find(start, SCAN, 4, [](const uint8_t* b, size_t) { return isPack(b); });
            res.lines.push_back(
                "No pack with a sequence header was found after the place; the sample begins at the "
                "next pack.");
        }
        if (found < 0)
            throw std::runtime_error("No pack of the program stream was found after the place.");
        start = found;
    }
    int64_t end = std::min(src.size(), start + r.maxBytes);
    if (end < src.size())
    {
        // ends before a pack, not inside one
        const Bytes tail = src.read(std::max(start, end - (64 << 10)), end - std::max(start, end - (64 << 10)));
        for (size_t i = tail.size() >= 4 ? tail.size() - 3 : 0; i-- > 4;)  // a pack code needs 4 bytes
            if (isPack(tail.data() + i))
            {
                end = std::max(start, end - (64 << 10)) + static_cast<int64_t>(i);
                break;
            }
    }
    out.copy(src, start, end);
    res.lines.push_back("Bytes " + std::to_string(start) + " to " + std::to_string(end - 1) + " of the source (" +
                        mb(end - start) + "), from a pack" +
                        (start == 0 ? std::string(" at its start") : std::string(" with a sequence header")) + ".");
    // the IFO file of its title set: the colours of the subtitles and the languages
    const size_t slash = name.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? std::string() : name.substr(0, slash + 1);
    const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
    std::string ifo;
    if (lower(file).compare(0, 4, "vts_") == 0 && file.size() >= 12)
        ifo = file.substr(0, 7) + "0.IFO";
    else if (lower(file) == "video_ts.vob")
        ifo = "VIDEO_TS.IFO";
    if (!ifo.empty())
    {
        if (std::FILE* f = openFile(folder + ifo, false))
        {
            std::fclose(f);
            res.extraFiles.push_back(folder + ifo);
            res.lines.push_back(ifo + " of the title set is added: the colours of the subtitles and the languages.");
        }
    }
}

// ---- Ogg (RFC 3533) ----

bool isPage(const uint8_t* p) { return p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S' && p[4] == 0; }

int64_t pageLength(const Source& src, const int64_t pos)
{
    const Bytes h = src.read(pos, 27 + 255);
    if (h.size() < 27 || !isPage(h.data()) || h.size() < static_cast<size_t>(27 + h[26]))
        return -1;
    int64_t body = 0;
    for (int i = 0; i < h[26]; ++i) body += h[static_cast<size_t>(27 + i)];
    return 27 + h[26] + body;
}

void cutOgg(const Request& r, const Source& src, Target& out, Result& res)
{
    res.kind = "Ogg";
    // The pages of the header packets, the codec's set-up, which every reader needs first: 2 of Opus (RFC 7845:
    // identification and comment header), 3 of Vorbis (identification, comment, setup). They are counted by the
    // packets that end on each page (a lacing value under 255), as a comment header with a picture spans pages that
    // end no packet, whose granule position is -1 (RFC 3533)
    int64_t headEnd = 0;
    int rate = 48000;
    int wanted = 0;  // header packets, known from the first
    int done = 0;
    while (headEnd < src.size() && (wanted == 0 || done < wanted))
    {
        const int64_t len = pageLength(src, headEnd);
        if (len < 0)
            break;
        const Bytes h = src.read(headEnd, 27 + 255 + 40);
        if (h.size() < 27 || h.size() < static_cast<size_t>(27 + h[26]))
            break;
        const size_t body = static_cast<size_t>(27 + h[26]);
        if (wanted == 0)
        {
            if (body + 8 <= h.size() && std::memcmp(h.data() + body, "OpusHead", 8) == 0)
                wanted = 2;
            else if (body + 16 <= h.size() && h[body] == 1 && std::memcmp(h.data() + body + 1, "vorbis", 6) == 0)
            {
                wanted = 3;
                rate = h[body + 12] | h[body + 13] << 8 | h[body + 14] << 16 | h[body + 15] << 24;
            }
            else
                wanted = 1;  // another codec: its first packet
        }
        for (int i = 0; i < h[26]; ++i) done += h[static_cast<size_t>(27 + i)] < 255 ? 1 : 0;
        headEnd += len;
    }
    if (headEnd == 0)
        throw std::runtime_error("The Ogg file does not begin with a page.");
    int64_t atByte = 0;
    if (r.take == Take::At)
    {
        const auto clockAt = [&](const int64_t pos) -> int64_t
        {
            const int64_t p = src.find(pos, 1 << 20, 27, [](const uint8_t* b, size_t) { return isPage(b); });
            if (p < 0)
                return NO_TIME;
            const Bytes h = src.read(p, 14);
            int64_t granule = 0;
            for (int i = 13; i >= 6; --i) granule = granule << 8 | h[static_cast<size_t>(i)];
            return granule < 0 ? NO_TIME : granule;
        };
        atByte = bisect(src.size(), static_cast<int64_t>(r.atSeconds * rate), 16 << 10, clockAt);
        res.lines.push_back("The time was found by the granule positions of the pages: " + clock(r.atSeconds) +
                            " is near byte " + std::to_string(atByte) + ".");
    }
    int64_t start = std::max(headEnd, roughStart(r, src.size(), atByte) + headEnd);
    if (start > headEnd)
    {
        const int64_t page = src.find(start, 4 << 20, 27, [](const uint8_t* b, size_t) { return isPage(b); });
        start = page < 0 ? src.size() : page;
    }
    int64_t end = start;
    for (int64_t len;
         end < src.size() && (len = pageLength(src, end)) > 0 && end + len - start + headEnd <= r.maxBytes;)
        end += len;
    out.copy(src, 0, headEnd);
    out.copy(src, start, end);
    res.lines.push_back("The header pages (bytes 0 to " + std::to_string(headEnd - 1) + "), then the pages of bytes " +
                        std::to_string(start) + " to " + std::to_string(end - 1) + " of the source (" +
                        mb(end - start) + ")." +
                        (start > headEnd ? " The page numbers jump between the two, which a reader reports as pages "
                                           "missing."
                                         : ""));
}

// ---- WAV, RF64 and BW64 ----

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

void putLe32(uint8_t* p, const uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void cutWav(const Request& r, const Source& src, Target& out, Result& res)
{
    const Bytes h = src.read(0, 1 << 16);
    const bool rf64 = std::memcmp(h.data(), "RF64", 4) == 0 || std::memcmp(h.data(), "BW64", 4) == 0;
    res.kind = rf64 ? "RF64 / BW64" : "WAV";
    size_t pos = 12;
    size_t dataAt = 0;
    size_t ds64 = 0;
    uint32_t byteRate = 0;
    uint32_t blockAlign = 1;
    while (pos + 8 <= h.size())
    {
        const uint32_t size = le32(h.data() + pos + 4);
        if (std::memcmp(h.data() + pos, "ds64", 4) == 0)
            ds64 = pos;
        if (std::memcmp(h.data() + pos, "fmt ", 4) == 0 && pos + 24 <= h.size())
        {
            byteRate = le32(h.data() + pos + 16);
            blockAlign = std::max<uint32_t>(1, h[pos + 20] | h[pos + 21] << 8);
        }
        if (std::memcmp(h.data() + pos, "data", 4) == 0)
        {
            dataAt = pos + 8;
            break;
        }
        pos += 8 + size + (size & 1);
    }
    if (dataAt == 0 || byteRate == 0)
        throw std::runtime_error("The WAV file has no format chunk or no data chunk in its first 64 KB.");
    // the data chunk as it gives its size, without what may follow it; RF64 gives it in ds64
    int64_t dataLen = src.size() - static_cast<int64_t>(dataAt);
    if (const uint32_t declared = le32(h.data() + dataAt - 4); !rf64 && declared != 0xFFFFFFFF)
        dataLen = std::min<int64_t>(dataLen, declared);
    if (rf64 && ds64 != 0 && ds64 + 24 <= h.size())  // ds64: riffSize, dataSize, sampleCount (64 bits each)
    {
        uint64_t declared = 0;
        for (int i = 7; i >= 0; --i) declared = declared << 8 | h[ds64 + 16 + i];
        if (declared > 0 && declared < static_cast<uint64_t>(dataLen))
            dataLen = static_cast<int64_t>(declared);
    }
    const int64_t budget = (r.maxBytes - static_cast<int64_t>(dataAt)) / blockAlign * blockAlign;
    int64_t from = 0;
    if (r.take == Take::End)
        from = std::max<int64_t>(0, dataLen - budget);
    else if (r.take == Take::At)
        from = std::clamp<int64_t>(static_cast<int64_t>(r.atSeconds * byteRate) - budget / 2, 0,
                                   std::max<int64_t>(0, dataLen - budget));
    from = from / blockAlign * blockAlign;
    const int64_t len = std::min(budget, dataLen - from) / blockAlign * blockAlign;
    Bytes head(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(dataAt));
    if (rf64 && ds64 != 0)
    {
        // the 64 bit sizes of ds64: the RIFF, the data and the number of samples
        const uint64_t riff = dataAt - 8 + len;
        const uint64_t samples = static_cast<uint64_t>(len) / blockAlign;
        for (int i = 0; i < 8; ++i)
        {
            head[ds64 + 8 + i] = static_cast<uint8_t>(riff >> (8 * i));
            head[ds64 + 16 + i] = static_cast<uint8_t>(static_cast<uint64_t>(len) >> (8 * i));
            if (ds64 + 32 <= head.size())
                head[ds64 + 24 + i] = static_cast<uint8_t>(samples >> (8 * i));
        }
    }
    else
    {
        putLe32(head.data() + 4, static_cast<uint32_t>(dataAt - 8 + len));
        putLe32(head.data() + dataAt - 4, static_cast<uint32_t>(len));
    }
    out.write(head);
    out.copy(src, static_cast<int64_t>(dataAt) + from, static_cast<int64_t>(dataAt) + from + len);
    res.lines.push_back("The header (" + std::to_string(dataAt) + " bytes, its sizes set to the sample), then bytes " +
                        std::to_string(from) + " to " + std::to_string(from + len - 1) + " of the sound (" + mb(len) +
                        ", from " + clock(static_cast<double>(from) / byteRate) + ").");
}

// ---- A stream on its own: video or sound without a container ----

enum class EsKind
{
    H264,
    Hevc,
    Mpeg2,
    Vc1,
    Ac3,
    Dts,
    TrueHd,
    Aac,
    MpegAudio,
    Flac,
    Other
};

EsKind esKindOf(const std::string& ext)
{
    if (ext == ".264" || ext == ".h264" || ext == ".avc" || ext == ".jsv" || ext == ".mvc")
        return EsKind::H264;
    if (ext == ".265" || ext == ".h265" || ext == ".hevc" || ext == ".hvc")
        return EsKind::Hevc;
    if (ext == ".m2v" || ext == ".mpv" || ext == ".m1v")
        return EsKind::Mpeg2;
    if (ext == ".vc1")
        return EsKind::Vc1;
    if (ext == ".ac3" || ext == ".eac3" || ext == ".ec3" || ext == ".dd+" || ext == ".ddp")
        return EsKind::Ac3;
    if (ext == ".dts" || ext == ".dtshd" || ext == ".dtsma")
        return EsKind::Dts;
    if (ext == ".thd" || ext == ".mlp" || ext == ".truehd")
        return EsKind::TrueHd;
    if (ext == ".aac" || ext == ".adts")
        return EsKind::Aac;
    if (ext == ".mp3" || ext == ".mp2" || ext == ".mpa")
        return EsKind::MpegAudio;
    if (ext == ".flac")
        return EsKind::Flac;
    return EsKind::Other;
}

// Where a reader of the stream can begin: the parameter sets of video, the frame sync of sound
bool esEntry(const EsKind kind, const uint8_t* p, const size_t n)
{
    if (n < 8)
        return false;
    const bool code = p[0] == 0 && p[1] == 0 && p[2] == 1;
    switch (kind)
    {
    case EsKind::H264:
        return code && (p[3] & 0x1F) == 7;
    case EsKind::Hevc:
        return code && (p[3] >> 1 & 0x3F) == 32;
    case EsKind::Mpeg2:
        return code && p[3] == 0xB3;
    case EsKind::Vc1:
        return code && p[3] == 0x0F;
    case EsKind::Ac3:
        return p[0] == 0x0B && p[1] == 0x77;
    case EsKind::Dts:
        return p[0] == 0x7F && p[1] == 0xFE && p[2] == 0x80 && p[3] == 0x01;
    case EsKind::TrueHd:
        return p[4] == 0xF8 && p[5] == 0x72 && p[6] == 0x6F && (p[7] == 0xBA || p[7] == 0xBB);
    case EsKind::Aac:
        return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
    case EsKind::MpegAudio:
        return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && (p[1] & 0x18) != 0x08 && (p[1] & 0x06) != 0 &&
               (p[2] & 0xF0) != 0xF0 && (p[2] & 0x0C) != 0x0C;
    case EsKind::Flac:
        return p[0] == 0xFF && (p[1] & 0xFE) == 0xF8;
    default:
        return false;
    }
}

// The bytes a second of a sound of constant rate takes, from its first frame; 0 when it has no constant rate
double constantByteRate(const EsKind kind, const Bytes& b)
{
    if (b.size() < 16)
        return 0;
    if (kind == EsKind::Ac3 && (b[5] >> 3) <= 8)  // AC-3, not E-AC-3: its frame size code gives the rate
    {
        static const int kbps[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                   192, 224, 256, 320, 384, 448, 512, 576, 640};
        const int code = b[4] & 0x3F;
        return code / 2 < 19 ? kbps[code / 2] * 1000.0 / 8 : 0;
    }
    if (kind == EsKind::Dts)
    {
        // core frame: its size and its number of samples, at the rate of its sample frequency code
        const int samples = ((b[4] & 1) << 6 | b[5] >> 2) + 1;
        const int size = ((b[5] & 3) << 12 | b[6] << 4 | b[7] >> 4) + 1;
        static const int rates[] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0};
        const int rate = rates[b[8] >> 2 & 0xF];
        return rate ? static_cast<double>(size) * rate / (samples * 32) : 0;
    }
    if (kind == EsKind::MpegAudio)
    {
        static const int l2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
        static const int l3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
        static const int l3low[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
        const int layer = 4 - (b[1] >> 1 & 3);
        const bool mpeg1 = (b[1] & 0x08) != 0;
        const int index = b[2] >> 4;
        const int kbps = !mpeg1 ? l3low[index] : layer == 2 ? l2[index] : layer == 3 ? l3[index] : 0;
        return kbps * 1000.0 / 8;
    }
    return 0;
}

void cutEs(const Request& r, const Source& src, Target& out, Result& res, const EsKind kind)
{
    const bool video = kind == EsKind::H264 || kind == EsKind::Hevc || kind == EsKind::Mpeg2 || kind == EsKind::Vc1;
    res.kind = video ? "a video stream on its own" : "a sound stream on its own";
    if (kind == EsKind::Other)
        throw std::runtime_error(
            "tsMuxeR does not know where a reader can begin in a file of this kind, so it cannot "
            "take a part of it. Send its listing, or a sample of the file it came from.");
    if (kind == EsKind::Flac && r.take != Take::Start)
        throw std::runtime_error("A FLAC file keeps its format in its first bytes only; take its start.");
    int64_t atByte = 0;
    if (r.take == Take::At)
    {
        if (video)
            throw std::runtime_error(
                "A video stream on its own holds no times, so a time cannot be found in it. Take "
                "its start or its end, or a sample of the file it came from.");
        const int64_t first =
            src.find(0, 1 << 20, 16, [kind](const uint8_t* p, size_t n) { return esEntry(kind, p, n); });
        const double rate = first < 0 ? 0 : constantByteRate(kind, src.read(first, 16));
        if (rate <= 0)
            throw std::runtime_error(
                "This sound stream has no constant rate (or its first frame does not give it), so "
                "a time cannot be found in it without reading it all. Take its start or its end.");
        atByte = static_cast<int64_t>(r.atSeconds * rate);
        res.lines.push_back("The time was found by the constant rate of the stream, " +
                            std::to_string(static_cast<int64_t>(rate * 8 / 1000)) + " kbit/s: " + clock(r.atSeconds) +
                            " is near byte " + std::to_string(atByte) + ".");
    }
    int64_t start = roughStart(r, src.size(), atByte);
    Bytes params;  // the parameter sets of the start of the file, put in front of a key picture that has none
    if (start > 0)
    {
        // Where a reader can begin: a sequence's parameters (or a frame of sound) after the place. H.264 and HEVC that
        // give their parameter sets at the start of the file only, as many encoders write them, begin at a key picture
        // after the place, with the parameter sets of the start put in front. Near the end of a file, where neither
        // comes after the place, the last one before it: parameters within a sample's length, else a key picture.
        const auto isEntry = [kind](const uint8_t* p, size_t n) { return esEntry(kind, p, n); };
        const bool keyForm = kind == EsKind::H264 || kind == EsKind::Hevc;
        const auto isKey = [kind](const uint8_t* p, size_t n)
        {
            if (n < 6 || p[0] != 0 || p[1] != 0 || p[2] != 1)
                return false;
            return kind == EsKind::H264 ? (p[3] & 0x1F) == 5 : ((p[3] >> 1 & 0x3F) >= 16 && (p[3] >> 1 & 0x3F) <= 21);
        };
        int64_t entry = src.find(start, 16 << 20, 16, isEntry);
        bool atKey = false;
        if (entry < 0 && keyForm && (entry = src.find(start, 16 << 20, 6, isKey)) >= 0)
            atKey = true;
        if (entry < 0)
            entry = src.findBack(start, r.maxBytes, 16, isEntry);
        if (entry < 0 && keyForm && (entry = src.findBack(start, 32 << 20, 6, isKey)) >= 0)
            atKey = true;
        if (entry < 0)
            throw std::runtime_error("No place a reader can begin at was found near the place.");
        if (atKey)
        {
            // the parameter sets: what comes before the first picture of the file
            const int64_t firstSlice = src.find(0, 4 << 20, 6,
                                                [kind](const uint8_t* p, size_t n)
                                                {
                                                    if (n < 6 || p[0] != 0 || p[1] != 0 || p[2] != 1)
                                                        return false;
                                                    const int t =
                                                        kind == EsKind::H264 ? (p[3] & 0x1F) : (p[3] >> 1 & 0x3F);
                                                    return kind == EsKind::H264 ? (t >= 1 && t <= 5) : t <= 21;
                                                });
            if (firstSlice <= 0 || src.find(0, firstSlice, 16, isEntry) < 0)
                throw std::runtime_error(
                    "The stream gives no parameter sets before its first picture, so a part of "
                    "it cannot be read. Take its start.");
            params = src.read(0, firstSlice);
            res.lines.push_back("The parameter sets of the start of the file (bytes 0 to " +
                                std::to_string(firstSlice - 1) + ") are put in front of a key picture.");
        }
        start = entry;
    }
    int64_t end = std::min(src.size(), start + r.maxBytes - static_cast<int64_t>(params.size()));
    if (end < src.size())
    {
        // ends before a frame or a sequence, not inside one
        const int64_t back = std::max(start + 1, end - (8 << 20));
        const Bytes tail = src.read(back, end - back);
        for (size_t i = tail.size(); i-- > 0;)
            if (esEntry(kind, tail.data() + i, tail.size() - i) ||
                (video && i + 4 <= tail.size() && tail[i] == 0 && tail[i + 1] == 0 && tail[i + 2] == 1 &&
                 ((kind == EsKind::H264 &&
                   ((tail[i + 3] & 0x1F) == 9 || (((tail[i + 3] & 0x1F) == 1 || (tail[i + 3] & 0x1F) == 5) &&
                                                  i + 4 < tail.size() && (tail[i + 4] & 0x80)))) ||
                  (kind == EsKind::Hevc &&
                   ((tail[i + 3] >> 1 & 0x3F) == 35 ||
                    ((tail[i + 3] >> 1 & 0x3F) <= 21 && i + 5 < tail.size() && (tail[i + 5] & 0x80)))) ||
                  (kind == EsKind::Mpeg2 && tail[i + 3] == 0x00))))
            {
                end = back + static_cast<int64_t>(i);
                break;
            }
    }
    if (kind == EsKind::Flac)
        start = 0;
    out.write(params);
    out.copy(src, start, end);
    res.lines.push_back(
        "Bytes " + std::to_string(start) + " to " + std::to_string(end - 1) + " of the source (" + mb(end - start) +
        ")" +
        (start > 0 ? std::string(!video           ? ", from a frame"
                                 : params.empty() ? ", from a sequence's parameters"
                                                  : ", from a key picture")
                   : "") +
        (end < src.size() ? std::string(video ? ", up to the start of a picture" : ", up to a frame") : "") + ".");
}

// ---- the zip ----

void put16(Bytes& b, const uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(Bytes& b, const uint32_t v)
{
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}
}  // namespace

Result cut(const Request& r, const std::string& target)
{
    const Source src(r.source);
    Result res;
    const std::string ext = extensionOf(r.source);
    res.extension = ext;
    // a Matroska file always through its own rule: its title and names are blanked unless kept
    const bool matroska = [&]
    {
        const Bytes h = src.read(0, 4);
        return h.size() == 4 && std::memcmp(h.data(), "\x1A\x45\xDF\xA3", 4) == 0;
    }();
    if (src.size() <= r.maxBytes && !(matroska && !r.keepNames))
    {
        Target out(target);
        out.copy(src, 0, src.size());
        out.close();
        res.kind = "a file";
        res.lines.push_back("The whole source (" + mb(src.size()) + ") fits in the sample, so all of it is taken.");
        if (r.take != Take::Start)
            res.lines.push_back("Its start, end or time does not matter then.");
        return res;
    }
    const Bytes head = src.read(0, 1024);
    if (head.size() < 16)
        throw std::runtime_error("The source is too short to be read.");
    Target out(target);
    TsLayout ts;
    if (std::memcmp(head.data(), "\x1A\x45\xDF\xA3", 4) == 0)
        cutMatroska(r, src, out, res);
    else if (tsLayout(src, ts))
        cutTs(r, src, out, res);
    else if (isPack(head.data()))
        cutProgramStream(r, src, out, res, r.source);
    else if (isPage(head.data()))
        cutOgg(r, src, out, res);
    else if ((std::memcmp(head.data(), "RIFF", 4) == 0 || std::memcmp(head.data(), "RF64", 4) == 0 ||
              std::memcmp(head.data(), "BW64", 4) == 0) &&
             std::memcmp(head.data() + 8, "WAVE", 4) == 0)
        cutWav(r, src, out, res);
    else if (std::memcmp(head.data() + 4, "ftyp", 4) == 0 || std::memcmp(head.data() + 4, "moov", 4) == 0 ||
             std::memcmp(head.data() + 4, "free", 4) == 0 || std::memcmp(head.data() + 4, "wide", 4) == 0 ||
             std::memcmp(head.data() + 4, "mdat", 4) == 0)
        cutMp4(r, src, out, res);
    else
        cutEs(r, src, out, res, esKindOf(ext));
    out.close();
    if (out.written() == 0)
        throw std::runtime_error("Nothing of the source was taken: the place is past its end.");
    return res;
}

void writeZip(const std::string& zipName, const std::vector<std::pair<std::string, std::string>>& entries)
{
    // PKWARE APPNOTE 6.3: a local header and the data of each file, then the central directory and its end
    Target zip(zipName);
    Bytes central;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const auto dosTime = static_cast<uint32_t>(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const auto dosDate = static_cast<uint32_t>((std::max(tm.tm_year - 80, 0)) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    for (const auto& [name, file] : entries)
    {
        const Source src(file);
        uLong crc = crc32(0L, Z_NULL, 0);
        for (int64_t pos = 0; pos < src.size();)
        {
            const Bytes b = src.read(pos, 4 << 20);
            if (b.empty())
                break;
            crc = crc32(crc, b.data(), static_cast<uInt>(b.size()));
            pos += static_cast<int64_t>(b.size());
        }
        const auto size = static_cast<uint32_t>(src.size());
        const auto offset = static_cast<uint32_t>(zip.written());
        Bytes local;
        put32(local, 0x04034B50);
        put16(local, 10);       // version needed: 1.0, stored
        put16(local, 1 << 11);  // the name is UTF-8
        put16(local, 0);        // stored
        put16(local, dosTime);
        put16(local, dosDate);
        put32(local, static_cast<uint32_t>(crc));
        put32(local, size);
        put32(local, size);
        put16(local, static_cast<uint32_t>(name.size()));
        put16(local, 0);
        local.insert(local.end(), name.begin(), name.end());
        zip.write(local);
        zip.copy(src, 0, src.size());

        put32(central, 0x02014B50);
        put16(central, 20);  // made by: 2.0
        put16(central, 10);
        put16(central, 1 << 11);
        put16(central, 0);
        put16(central, dosTime);
        put16(central, dosDate);
        put32(central, static_cast<uint32_t>(crc));
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<uint32_t>(name.size()));
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, 0);  // external attributes
        put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
    }
    const auto centralAt = static_cast<uint32_t>(zip.written());
    zip.write(central);
    Bytes end;
    put32(end, 0x06054B50);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<uint32_t>(entries.size()));
    put16(end, static_cast<uint32_t>(entries.size()));
    put32(end, static_cast<uint32_t>(central.size()));
    put32(end, centralAt);
    put16(end, 0);
    zip.write(end);
    zip.close();
}
}  // namespace sample
