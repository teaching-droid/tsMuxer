// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "oggFile.h"

#include <fs/discImage.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "assFile.h"
#include "memoryMatroska.h"
#include "opusStreamReader.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "vorbisStreamReader.h"
#include "wavpackFile.h"

namespace
{
using namespace memoryMatroska;

// tsMuxeR reads the whole file into memory; a file of sound only is far smaller than this
#if INTPTR_MAX > INT32_MAX
constexpr int64_t MAX_OGG_BYTES = INT64_C(1) << 30;
#else
constexpr int64_t MAX_OGG_BYTES = INT64_C(256) << 20;
#endif

// The CRC of an Ogg page (RFC 3533): polynomial 0x04C11DB7, not reflected, starting at 0, over the page with its CRC
// field set to 0
uint32_t oggCrc(const uint8_t* data, const size_t size, uint32_t crc)
{
    static const auto table = []
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t r = i << 24;
            for (int b = 0; b < 8; ++b) r = (r & 0x80000000U) ? (r << 1) ^ 0x04C11DB7U : r << 1;
            t[i] = r;
        }
        return t;
    }();
    for (size_t i = 0; i < size; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

// A file read in blocks, so that only what is kept is held in memory
class PageReader
{
   public:
    explicit PageReader(std::ifstream& file) : m_file(file) {}
    // n bytes from the current position on, or false at the end of the file
    bool need(const size_t n)
    {
        if (m_len - m_pos >= n)
            return true;
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_pos));
        m_offset += static_cast<int64_t>(m_pos);
        m_len -= m_pos;
        m_pos = 0;
        const size_t want = (std::max)(n, static_cast<size_t>(1 << 20));
        m_buf.resize(want);
        while (m_len < n && m_file)
        {
            m_file.read(reinterpret_cast<char*>(m_buf.data() + m_len), static_cast<std::streamsize>(want - m_len));
            m_len += static_cast<size_t>(m_file.gcount());
            if (m_file.gcount() == 0)
                break;
        }
        return m_len - m_pos >= n;
    }
    const uint8_t* at() const { return m_buf.data() + m_pos; }
    void skip(const size_t n) { m_pos += n; }
    int64_t offset() const { return m_offset + static_cast<int64_t>(m_pos); }
    // Another page begins after the current position, in the data read; after a need() that failed that is all the
    // rest of the file
    bool pageAfter() const
    {
        for (size_t i = m_pos + 1; i + 4 <= m_len; ++i)
            if (std::memcmp(m_buf.data() + i, "OggS", 4) == 0)
                return true;
        return false;
    }

   private:
    std::ifstream& m_file;
    std::vector<uint8_t> m_buf;
    size_t m_pos = 0;
    size_t m_len = 0;
    int64_t m_offset = 0;
};

enum class Kind
{
    unknown,
    opus,
    vorbis,
    skeleton,
    other
};

struct Stream
{
    uint32_t serial = 0;
    Kind kind = Kind::unknown;
    std::string otherName;  // what an unread stream holds
    int track = 0;          // its Matroska track number, from 1
    std::vector<std::vector<uint8_t>> headers;
    size_t headersWanted = 0;
    std::vector<uint8_t> partial;  // a packet that goes on in the next page
    bool partialLost = false;      // the page before was damaged: the start of the packet is lost
    bool ended = false;
    int64_t lastSequence = -1;  // the page sequence number of its last page read
    int64_t damagedSince = 0;   // its damaged pages since then
    // the audio packets, one after another, with their sizes, lengths in samples and start times
    std::vector<uint8_t> data;
    std::vector<uint32_t> sizes;
    std::vector<int32_t> lengths;
    std::vector<int64_t> starts;
    size_t timed = 0;             // the packets before this one have their start
    int64_t pos = INT64_MIN;      // the sample after the last timed packet
    bool resync = false;          // packets were lost: the next granule position places what follows
    int64_t endTrim = 0;          // samples the last page cuts off the end
    int64_t granuleMismatch = 0;  // pages whose granule position is not where the packets end
    // Opus
    int preSkip = 0;
    int channels = 0;
    int mappingFamily = 0;
    int64_t filledSamples = 0;  // the samples of lost pages filled with empty frames
    int rate = 48000;
    // Vorbis
    std::unique_ptr<VorbisStreamReader> vorbis;
    std::vector<uint8_t> codecPrivate;
    int prevBlock = 0;
    std::string language;
    std::string title;
};

// The LANGUAGE and TITLE of a Vorbis comment block (also in OpusTags): length-prefixed vendor, count, KEY=value
void readComments(Stream& s, const std::vector<uint8_t>& packet, const size_t skip)
{
    size_t p = skip;
    const auto u32 = [&](uint32_t& v)
    {
        if (p + 4 > packet.size())
            return false;
        v = le32(packet.data() + p);
        p += 4;
        return true;
    };
    uint32_t len = 0, count = 0;
    if (!u32(len) || p + len > packet.size())
        return;
    p += len;
    if (!u32(count))
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!u32(len) || p + len > packet.size())
            return;
        const std::string c(reinterpret_cast<const char*>(packet.data() + p), len);
        p += len;
        const size_t eq = c.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = strToUpperCase(c.substr(0, eq));
        const std::string value = c.substr(eq + 1);
        if (key == "LANGUAGE" && value.size() == 3 &&
            std::all_of(value.begin(), value.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
            s.language = value;
        else if (key == "TITLE" && s.title.empty())
            s.title = value;
    }
}

// The kind of a stream, from its first packet
void identify(Stream& s, const std::vector<uint8_t>& first)
{
    const auto starts = [&](const char* sig, const size_t n)
    { return first.size() >= n && std::memcmp(first.data(), sig, n) == 0; };
    if (starts("OpusHead", 8) && first.size() >= 19)
    {
        s.kind = Kind::opus;
        s.headersWanted = 2;
        s.channels = first[9];
        s.preSkip = first[10] | first[11] << 8;
        s.mappingFamily = first[18];
    }
    else if (starts("\x01vorbis", 7) && first.size() >= 30)
    {
        s.kind = Kind::vorbis;
        s.headersWanted = 3;
        s.channels = first[11];
        s.rate = static_cast<int>(le32(first.data() + 12));
    }
    else if (starts("fishead", 8) || starts("fisbone", 8))
        s.kind = Kind::skeleton;
    else
    {
        s.kind = Kind::other;
        s.otherName = starts("\x80theora", 7) ? "Theora video"
                      : starts(
                            "\x7f"
                            "FLAC",
                            5)
                          ? "FLAC sound"
                      : starts("Speex   ", 8) ? "Speex sound"
                      : starts("OVP80", 5)    ? "VP8 video"
                      : starts("\x80kate", 5) ? "Kate subtitles"
                      : starts("PCM     ", 8) ? "PCM sound"
                      : starts("\x01video", 6) ||
                              starts(
                                  "\x01"
                                  "audio",
                                  6) ||
                              starts("\x01text", 5)
                          ? "OGM"
                          : "an unknown kind of";
    }
}

void addPacket(Stream& s, std::vector<uint8_t>&& packet)
{
    if (s.kind == Kind::unknown)
        identify(s, packet);
    if (s.kind == Kind::skeleton || s.kind == Kind::other)
        return;
    if (s.headers.size() < s.headersWanted)
    {
        s.headers.push_back(std::move(packet));
        if (s.headers.size() == 2)
            readComments(s, s.headers[1], s.kind == Kind::opus ? 8 : 7);
        if (s.kind == Kind::opus && s.headers.size() == 1)
            s.codecPrivate = s.headers[0];
        if (s.kind == Kind::vorbis && s.headers.size() == 3)
        {
            // Xiph lacing: the number of packets less one, the sizes of the first two, then the three packets
            std::vector<uint8_t>& cp = s.codecPrivate;
            cp.push_back(2);
            for (int i = 0; i < 2; ++i)
            {
                size_t n = s.headers[i].size();
                for (; n >= 255; n -= 255) cp.push_back(255);
                cp.push_back(static_cast<uint8_t>(n));
            }
            for (const auto& h : s.headers) cp.insert(cp.end(), h.begin(), h.end());
            s.vorbis = std::make_unique<VorbisStreamReader>();
            if (!s.vorbis->setCodecPrivate(cp.data(), static_cast<int>(cp.size())))
            {
                s.kind = Kind::other;
                s.otherName = "Vorbis sound with damaged headers";
            }
        }
        return;
    }
    if (packet.empty())
        return;
    int length = 0;
    if (s.kind == Kind::opus)
        length = OpusStreamReader::parseFrameSamples(packet.data(), static_cast<int>(packet.size()));
    else
    {
        // a packet gives a quarter of the block before it and a quarter of its own; the first gives nothing
        const int block = s.vorbis->blockSizeOf(packet.data(), static_cast<int>(packet.size()));
        if (block == 0)
            return;  // no audio packet
        length = s.prevBlock ? s.prevBlock / 4 + block / 4 : 0;
        s.prevBlock = block;
    }
    s.data.insert(s.data.end(), packet.begin(), packet.end());
    s.sizes.push_back(static_cast<uint32_t>(packet.size()));
    s.lengths.push_back(length);
}

// The packets that ended on a page with a granule position are given their start: going on from the packets before,
// or, at the first page and after lost packets, back from the position, which is the end of the last of them. The
// last page may end the sound inside its last packet: the samples after its position are cut off (endTrim).
void placePackets(Stream& s, const int64_t granule, const bool last)
{
    if (s.kind != Kind::opus && s.kind != Kind::vorbis)
        return;
    int64_t pending = 0;
    for (size_t i = s.timed; i < s.lengths.size(); ++i) pending += s.lengths[i];
    if (s.pos == INT64_MIN || s.resync)
    {
        int64_t start = granule - pending;
        if (last && s.pos == INT64_MIN && start < 0)
            start = 0;  // a file of one page: its position is the end of the sound, short of the packets
        if (s.pos != INT64_MIN && start < s.pos)
            start = s.pos;
        // ** THE SOUND AFTER A LOST PAGE STAYS AT ITS TIME. ** Opus and Vorbis are placed by the lengths of their
        // packets, so a hole closed up and everything after it came early by what was lost. For Opus of one or two
        // channels the hole is filled with frames of no bytes (RFC 6716 3.2.1, a frame of length 0), which a decoder
        // fills in by itself; the CELT fullband modes of 20 ms down to 2.5 ms (configurations 31 to 28).
        if (s.pos != INT64_MIN && start > s.pos && s.kind == Kind::opus && s.mappingFamily == 0)
        {
            int64_t gap = (start - s.pos + 60) / 120 * 120;
            std::vector<uint32_t> sizes;
            std::vector<int32_t> lengths;
            std::vector<uint8_t> bytes;
            const uint8_t stereo = s.channels > 1 ? 0x04 : 0x00;
            for (const auto& [config, samples] : {std::pair{31, 960}, {30, 480}, {29, 240}, {28, 120}})
                for (; gap >= samples; gap -= samples)
                {
                    bytes.push_back(static_cast<uint8_t>(config << 3 | stereo));
                    sizes.push_back(1);
                    lengths.push_back(samples);
                }
            size_t at = 0;
            for (size_t i = 0; i < s.timed; ++i) at += s.sizes[i];
            s.data.insert(s.data.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
            s.sizes.insert(s.sizes.begin() + static_cast<std::ptrdiff_t>(s.timed), sizes.begin(), sizes.end());
            s.lengths.insert(s.lengths.begin() + static_cast<std::ptrdiff_t>(s.timed), lengths.begin(), lengths.end());
            for (const int32_t n : lengths) s.filledSamples += n;
            start = s.pos;  // the filler comes first, the packets after it at their time
        }
        s.pos = (std::max)(start, INT64_C(0));
        s.resync = false;
    }
    for (; s.timed < s.lengths.size(); ++s.timed)
    {
        s.starts.push_back(s.pos);
        s.pos += s.lengths[s.timed];
    }
    if (last && granule < s.pos)
        s.endTrim = s.pos - granule;
    else if (granule != s.pos)
        ++s.granuleMismatch;
}

std::mutex preparedLock;
std::set<std::string>& prepared()
{
    static std::set<std::string> names;
    return names;
}
}  // namespace

bool isOggFileExt(const std::string& upperExt) { return upperExt == "OGG" || upperExt == "OGA" || upperExt == "OPUS"; }

bool isMemorySourceExt(const std::string& upperExt)
{
    return isAssFileExt(upperExt) || isOggFileExt(upperExt) || isWavpackFileExt(upperExt);
}

bool prepareMemorySource(const std::string& fileName)
{
    const bool ass = prepareAssFile(fileName);
    const bool wavpack = prepareWavpackFile(fileName);
    return prepareOggFile(fileName) || ass || wavpack;
}

bool prepareOggFile(const std::string& fileName)
{
    const std::string name = unquoteStr(trimStr(fileName));
    if (!isOggFileExt(strToUpperCase(extractFileExt(name))))
        return false;
    {
        std::lock_guard<std::mutex> guard(preparedLock);
        if (prepared().count(name))
            return true;
    }
    const std::filesystem::path path = sourcePath(name);
    std::error_code ec;
    const auto fileSize = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    std::ifstream file(path, std::ios::binary);
    if (ec || !file)
        return false;  // said by whatever opens it, as for any file that cannot be opened
    if (fileSize > MAX_OGG_BYTES)
        THROW(ERR_COMMON, "The Ogg file " << name << " is " << fileSize / (1024 * 1024)
                                          << " MiB. tsMuxeR reads an Ogg file into memory, up to "
                                          << MAX_OGG_BYTES / (1024 * 1024) << " MiB.")

    std::vector<Stream> streams;
    PageReader in(file);
    int64_t damagedPages = 0, strayBytes = 0, missingPages = 0;
    bool afterDamage = false;
    bool endsInsidePage = false;  // the bytes after a damaged page, up to the next page, are not counted again
    bool audioBegun = false;
    for (;;)
    {
        if (!in.need(27))
        {
            endsInsidePage = in.need(1);  // bytes after the last whole page: the start of a page cut off
            break;
        }
        // the capture pattern; what is not a page is passed over to the next one
        if (std::memcmp(in.at(), "OggS", 4) != 0 || in.at()[4] != 0)
        {
            if (in.offset() == 0)
                THROW(ERR_COMMON, "The file " << name << " is no Ogg file: it does not begin with an Ogg page.")
            in.skip(1);
            if (!afterDamage)
                ++strayBytes;
            continue;
        }
        // A page longer than the rest of the file: cut off at its end, or a damaged length with pages after it
        const int segments = in.at()[26];
        size_t pageSize = 0;
        if (in.need(27 + static_cast<size_t>(segments)))
        {
            size_t bodySize = 0;
            for (int i = 0; i < segments; ++i) bodySize += in.at()[27 + i];
            pageSize = 27 + static_cast<size_t>(segments) + bodySize;
        }
        if (pageSize == 0 || !in.need(pageSize))
        {
            if (!in.pageAfter())
            {
                endsInsidePage = true;
                break;
            }
            ++damagedPages;
            in.skip(1);
            afterDamage = true;
            continue;
        }
        const uint8_t* page = in.at();
        uint8_t head[27];
        std::memcpy(head, page, 27);
        std::memset(head + 22, 0, 4);
        uint32_t crc = oggCrc(head, 27, 0);
        crc = oggCrc(page + 27, pageSize - 27, crc);
        const uint8_t type = page[5];
        int64_t granule;
        std::memcpy(&granule, page + 6, 8);  // little endian, as the machines tsMuxeR runs on
        const uint32_t serial = le32(page + 14);
        auto s = std::find_if(streams.begin(), streams.end(), [&](const Stream& x) { return x.serial == serial; });
        if (crc != le32(page + 22))
        {
            // a damaged page: its packets are lost, and the packet it goes on with too
            ++damagedPages;
            if (s != streams.end())
            {
                s->partial.clear();
                s->partialLost = true;
                s->resync = true;
                ++s->damagedSince;
            }
            in.skip(1);  // the next page may begin inside this one, where its length was damaged
            afterDamage = true;
            continue;
        }
        afterDamage = false;
        if (s == streams.end())
        {
            if ((type & 0x02) == 0)
            {
                in.skip(pageSize);  // a page of a stream whose first page is lost or cut off
                continue;
            }
            if (audioBegun && std::any_of(streams.begin(), streams.end(), [](const Stream& x) { return x.ended; }))
                THROW(ERR_COMMON, "The Ogg file "
                                      << name
                                      << " holds several streams one after the other (a chained Ogg file), which "
                                         "tsMuxeR does not read. Split it into a file for each stream; the files can "
                                         "then be joined with +.")
            streams.emplace_back();
            s = streams.end() - 1;
            s->serial = serial;
        }
        // Pages are numbered in each stream: a page lost without a trace, with its capture pattern or its stream's
        // number damaged, is a gap in the numbers. It is lost as a damaged page is.
        const uint32_t sequence = le32(page + 18);
        if (s->lastSequence >= 0 && sequence != static_cast<uint32_t>(s->lastSequence + 1))
        {
            const int64_t gap = static_cast<int64_t>(sequence - static_cast<uint32_t>(s->lastSequence + 1));
            missingPages += gap > s->damagedSince ? gap - s->damagedSince : 0;
            s->partial.clear();
            s->partialLost = true;
            s->resync = true;
        }
        s->lastSequence = sequence;
        s->damagedSince = 0;
        // the packets: a lacing value under 255 ends one, and a page that ends on 255 goes on in the next. A page
        // that does not go on with a packet ends one that was left open: it is lost.
        if ((type & 0x01) == 0)
        {
            s->partial.clear();
            s->partialLost = false;
        }
        const uint8_t* body = page + 27 + segments;
        size_t at = 0;
        bool first = true;
        int completed = 0;
        for (int i = 0; i < segments; ++i)
        {
            const uint8_t lace = page[27 + i];
            if (!(first && (type & 0x01) != 0 && s->partialLost))
                s->partial.insert(s->partial.end(), body + at, body + at + lace);
            at += lace;
            if (lace < 255)
            {
                if (first && (type & 0x01) != 0 && s->partialLost)
                    s->partial.clear();  // the end of a packet whose start is lost
                else
                {
                    addPacket(*s, std::move(s->partial));
                    ++completed;
                }
                s->partial = {};
                s->partialLost = false;
                first = false;
            }
        }
        const bool eos = (type & 0x04) != 0;
        if (s->kind == Kind::opus || s->kind == Kind::vorbis)
        {
            if (s->headers.size() >= s->headersWanted && completed > 0)
                audioBegun = true;
            if (granule != -1 && s->headers.size() >= s->headersWanted && s->lengths.size() > s->timed)
                placePackets(*s, granule, eos);
        }
        if (eos)
            s->ended = true;
        in.skip(pageSize);
    }
    // a file cut off: the packets of its last page with a position are placed, the rest go on from them
    for (Stream& s : streams)
        if ((s.kind == Kind::opus || s.kind == Kind::vorbis) && s.lengths.size() > s.timed && s.pos != INT64_MIN)
            for (; s.timed < s.lengths.size(); ++s.timed)
            {
                s.starts.push_back(s.pos);
                s.pos += s.lengths[s.timed];
            }

    int track = 0;
    for (Stream& s : streams)
    {
        if (s.kind == Kind::other)
            LTRACE(LT_WARN, 2,
                   "Warning: the Ogg file " << name << " holds a stream of " << s.otherName
                                            << ", which tsMuxeR does not read from an Ogg file. It is left out.");
        if ((s.kind == Kind::opus || s.kind == Kind::vorbis) && s.headers.size() == s.headersWanted && s.timed > 0)
            s.track = ++track;
        if (s.granuleMismatch > 0)
            LTRACE(LT_DEBUG, 0,
                   "Ogg " << name << " stream " << s.serial << ": " << s.granuleMismatch
                          << " pages with a granule position that is not where their packets end");
    }
    if (damagedPages > 0 || strayBytes > 0 || missingPages > 0)
    {
        int64_t filled = 0;
        bool closed = false;  // a hole of a track that could not be filled
        for (const Stream& s : streams)
        {
            filled = (std::max)(filled, s.filledSamples);
            closed = closed || (s.track > 0 && (s.kind == Kind::vorbis || s.mappingFamily != 0));
        }
        std::ostringstream what;
        if (damagedPages > 0)
            what << damagedPages << (damagedPages == 1 ? " damaged page" : " damaged pages");
        if (damagedPages > 0 && strayBytes > 0)
            what << " and ";
        if (strayBytes > 0)
            what << strayBytes << (strayBytes == 1 ? " byte" : " bytes") << " that are no page";
        if (missingPages > 0)
            what << (damagedPages > 0 || strayBytes > 0 ? ", and " : "") << missingPages
                 << (missingPages == 1 ? " page" : " pages") << " missing from its numbered pages";
        LTRACE(LT_WARN, 2,
               "Warning: the Ogg file "
                   << name << " has " << what.str() << ". Its sound there is lost"
                   << (filled > 0 ? ", " + doubleToStr(static_cast<double>(filled) / 48, 1) +
                                        " ms of Opus filled with frames a decoder fills in itself, so the sound after "
                                        "it stays at its time"
                                  : std::string())
                   << (closed ? (filled > 0 ? "; in a Vorbis or multichannel Opus track" : "; ") +
                                    std::string(" the sound after it comes that much earlier")
                              : std::string())
                   << ".");
    }
    if (endsInsidePage)
        LTRACE(LT_WARN, 2,
               "Warning: the Ogg file " << name
                                        << " ends inside a page: it was cut off there, and the sound of that page is "
                                           "lost.");
    if (track == 0)
        THROW(ERR_COMMON, "The Ogg file " << name << " holds no Opus or Vorbis sound that tsMuxeR can read.")

    // the Matroska file: the size of its segment is written when it is known
    std::vector<uint8_t> mkv;
    ebmlHeader(mkv);
    putId(mkv, 0x18538067);
    const size_t segmentSizeAt = mkv.size();
    putSize(mkv, 0);
    const size_t segmentStart = mkv.size();
    const auto ns = [](const Stream& s, const int64_t sample)
    { return static_cast<int64_t>(std::llround(static_cast<double>(sample) * 1e9 / s.rate)); };
    {
        std::vector<uint8_t> info;
        uintElement(info, 0x2AD7B1, 1000000);  // TimestampScale: milliseconds
        stringElement(info, 0x4D80, "");       // no application wrote it: the listing names none
        stringElement(info, 0x5741, "");
        double lastEndMs = 0;
        for (const Stream& s : streams)
            if (s.track > 0)
                lastEndMs = (std::max)(lastEndMs, static_cast<double>(ns(s, s.pos - s.endTrim)) / 1e6);
        floatElement(info, 0x4489, lastEndMs);
        element(mkv, 0x1549A966, info);
    }
    {
        std::vector<uint8_t> tracks;
        for (const Stream& s : streams)
        {
            if (s.track == 0)
                continue;
            std::vector<uint8_t> entry;
            uintElement(entry, 0xD7, static_cast<uint64_t>(s.track));
            uintElement(entry, 0x73C5, static_cast<uint64_t>(s.track));
            uintElement(entry, 0x83, 2);  // audio
            uintElement(entry, 0x9C, 0);
            stringElement(entry, 0x86, s.kind == Kind::opus ? "A_OPUS" : "A_VORBIS");
            element(entry, 0x63A2, s.codecPrivate);
            if (s.kind == Kind::opus)
            {
                // the pre-skip, which a player drops, and the 80 ms Opus needs to settle after a seek (RFC 7845 5.1)
                uintElement(entry, 0x56AA, static_cast<uint64_t>(ns(s, s.preSkip)));
                uintElement(entry, 0x56BB, 80000000);
            }
            stringElement(entry, 0x22B59C, s.language.empty() ? "und" : s.language);
            if (!s.title.empty())
                stringElement(entry, 0x536E, s.title);
            std::vector<uint8_t> audio;
            floatElement(audio, 0xB5, s.rate);
            uintElement(audio, 0x9F, static_cast<uint64_t>(s.channels));
            element(entry, 0xE1, audio);
            element(tracks, 0xAE, entry);
        }
        element(mkv, 0x1654AE6B, tracks);
    }
    // the blocks of all tracks in the order of their times, a cluster each 30 seconds so that a block's time is within
    // the 16 bits it is given
    struct Block
    {
        int64_t ms;
        size_t stream;
        size_t packet;
    };
    std::vector<Block> blocks;
    std::vector<std::vector<size_t>> offsets(streams.size());
    for (size_t i = 0; i < streams.size(); ++i)
    {
        const Stream& s = streams[i];
        if (s.track == 0)
            continue;
        size_t off = 0;
        for (size_t k = 0; k < s.starts.size(); ++k)
        {
            offsets[i].push_back(off);
            off += s.sizes[k];
            blocks.push_back({std::llround(static_cast<double>(ns(s, s.starts[k])) / 1e6), i, k});
        }
    }
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.ms < b.ms; });
    for (size_t i = 0; i < blocks.size();)
    {
        const int64_t clusterMs = blocks[i].ms;
        std::vector<uint8_t> cluster;
        uintElement(cluster, 0xE7, static_cast<uint64_t>(clusterMs));
        for (; i < blocks.size() && blocks[i].ms - clusterMs <= 30000; ++i)
        {
            const Block& b = blocks[i];
            const Stream& s = streams[b.stream];
            const bool lastWithTrim = b.packet + 1 == s.starts.size() && s.endTrim > 0;
            std::vector<uint8_t> block{static_cast<uint8_t>(0x80 | s.track)};
            const auto rel = static_cast<int16_t>(b.ms - clusterMs);
            block.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8));
            block.push_back(static_cast<uint8_t>(rel & 0xFF));
            block.push_back(lastWithTrim ? 0x00 : 0x80);  // a SimpleBlock of audio is a key frame
            const uint8_t* p = s.data.data() + offsets[b.stream][b.packet];
            block.insert(block.end(), p, p + s.sizes[b.packet]);
            if (lastWithTrim)
            {
                // the samples the last page cuts off: DiscardPadding of the last block
                std::vector<uint8_t> group;
                element(group, 0xA1, block);
                intElement(group, 0x75A2, ns(s, s.endTrim));
                element(cluster, 0xA0, group);
            }
            else
                element(cluster, 0xA3, block);
        }
        element(mkv, 0x1F43B675, cluster);
    }
    const uint64_t segmentSize = mkv.size() - segmentStart;
    for (int k = 0; k < 7; ++k) mkv[segmentSizeAt + 1 + k] = static_cast<uint8_t>(segmentSize >> (48 - 8 * k));
    discImage::addMemoryFile(name, std::move(mkv));
    std::lock_guard<std::mutex> guard(preparedLock);
    prepared().insert(name);
    return true;
}
