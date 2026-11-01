// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "metaDemuxer.h"

#include <fs/directory.h>
#include <fs/discImage.h>
#include <fs/systemlog.h>

#include <fs/textfile.h>
#include <types/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

static bool isKnownTrackParam(const std::string& name);

#include "aacStreamReader.h"
#include "ac3StreamReader.h"
#include "alacStreamReader.h"
#include "av1StreamReader.h"
#include "avCodecs.h"
#include "bufferedReaderManager.h"
#include "combinedH264Demuxer.h"
#include "discTitles.h"
#include "dtsStreamReader.h"
#include "dvbSubStreamReader.h"
#include "dvdTitles.h"
#include "flacStreamReader.h"
#include "h264StreamReader.h"
#include "hevcDolbyVisionFilter.h"
#include "hevcStreamReader.h"
#include "lpcmStreamReader.h"
#include "matroskaDemuxer.h"
#include "mkvCopyStreamReader.h"
#include "mlpStreamReader.h"
#include "movDemuxer.h"
#include "mpeg2StreamReader.h"
#include "mpegAudioStreamReader.h"
#include "mpegStreamReader.h"
#include "nalUnits.h"
#include "opusStreamReader.h"
#include "pgsStreamReader.h"
#include "programStreamDemuxer.h"
#include "srtStreamReader.h"
#include "subTrackFilter.h"
#include "textSubtitles.h"
#include "trueHDAC3MergeReader.h"
#include "tsDemuxer.h"
#include "tsMuxer.h"
#include "vc1StreamReader.h"
#include "videoCopyStreamReader.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "vorbisStreamReader.h"
#include "vpxStreamReader.h"
#include "vvcStreamReader.h"

extern "C"
{
#include "zlib.h"
}

// The names and the byte layout are fixed by the muxer that writes them, and stated in the
// manifest that travels beside them.
static const char DV_RPU_ATTACHMENT[] = "dv-original-rpu.bin";
static const char DV_RPU_PTS_ATTACHMENT[] = "dv-original-rpu-pts.bin";
// The manifest's own name is MANIFEST_ATTACHMENT_NAME, in nalUnits.h, because the demuxer reads
// the same file for the framing rule.

// One "  key   value" line out of the manifest. Defined once in nalUnits.cpp, because the demuxer
// reads the same manifest to frame its own output.
static std::string manifestValue(const std::string& text, const std::string& key) { return dvManifestValue(text, key); }

// Load the disc's own RPUs out of a profile 8.1 carrier and hand them to the splitter.
//
// A file with no manifest is an ordinary profile 7 merged track: it carries its originals inline
// and there is nothing to restore, so this does nothing at all and the split behaves as before.
//
// A file WITH a manifest is making a promise, and every part of that promise is checked here
// rather than trusted: the attachments are present, they are the size and the checksum the
// manifest states, there is one timestamp per entry, and the order is the one claimed. The
// manifest itself says a mismatch must be refused rather than guessed, because the result would be
// a disc that looks right and is wrong.
// Audio or video of a transport stream that begins after the first DETECT_STREAM_BUFFER_SIZE bytes, as the sound of a
// recording that starts a minute after its picture, was "Can't detect stream type" though its PMT names it. Such a
// track is read on alone after that window, until it has enough data to be told, the file ends, or LATE_LIMIT more
// bytes are read. Subtitles are left out: they are often far apart, and they are told by their stream type.
// The same for the audio and video tracks of a Matroska file, whose late track was "Can't detect stream type" as well,
// and had no Stream delay.
static void readLateTracks(AbstractDemuxer* demuxer, DemuxedData& demuxedData,
                           const std::map<int32_t, TrackInfo>& acceptedPidMap, int64_t& discardedSize)
{
    static constexpr int64_t LATE_LIMIT = 1024LL * 1024 * 1024;
    static constexpr size_t ENOUGH = 256 * 1024;
    const auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxer);
    if ((dynamic_cast<TSDemuxer*>(demuxer) == nullptr && mkv == nullptr) || demuxer->getFileBlockSize() == 0)
        return;
    PIDSet late;
    for (const auto& [pid, info] : acceptedPidMap)
    {
        const int t = info.m_trackType;
        const bool audioOrVideo = mkv ? mkv->isAudioOrVideo(pid)
                                      : t == 0x01 || t == 0x02 || t == 0x03 || t == 0x04 || t == 0x0f || t == 0x10 ||
                                            t == 0x1b || t == 0x20 || t == 0x24 || t == 0x33 || t == 0xea ||
                                            (t >= 0x80 && t <= 0x88) || t == 0xa1 || t == 0xa2;
        const auto data = demuxedData.find(pid);
        if (audioOrVideo && (data == demuxedData.end() || data->second.size() == 0))
            late.insert(pid);
    }
    // The demuxer keeps the tracks it was first asked for, so the others are cut back to what the window gave them
    // after every block: they would otherwise hold up to LATE_LIMIT in memory.
    std::map<int32_t, unsigned> kept;
    for (const auto& [pid, info] : acceptedPidMap)
        if (late.count(pid) == 0)
            kept[pid] = demuxedData.count(pid) ? static_cast<unsigned>(demuxedData[pid].size()) : 0;
    for (int64_t read = 0; !late.empty() && read < LATE_LIMIT; read += demuxer->getFileBlockSize())
    {
        const int rez = demuxer->simpleDemuxBlock(demuxedData, late, discardedSize);
        for (const auto& [pid, size] : kept)
            if (const auto data = demuxedData.find(pid); data != demuxedData.end())
                data->second.resize(size);
        if (rez == BufferedReader::DATA_EOF)
            break;
        for (auto pid = late.begin(); pid != late.end();)
            pid = demuxedData[*pid].size() >= ENOUGH ? late.erase(pid) : std::next(pid);
    }
}

// The first coded picture of an HEVC elementary stream is a random access picture (IDR, CRA or BLA); -1 while the data
// holds no slice
static int hevcStartsAtRandomAccess(StreamData& es)
{
    uint8_t* const end = es.data() + es.size();
    for (uint8_t* nal = NALUnit::findNextNAL(es.data(), end); nal < end; nal = NALUnit::findNextNAL(nal, end))
    {
        const int type = (*nal >> 1) & 0x3f;
        if (type < 32)  // the first slice
            return type >= 16 && type <= 23 ? 1 : 0;
    }
    return -1;
}

// The same for the HEVC stream of a transport stream file
static bool tsHevcStartsAtRandomAccess(const BufferedReaderManager& readManager, const std::string& fileName,
                                       const int pid)
{
    TSDemuxer probe(readManager, "");
    probe.openFile(fileName);
    std::map<int32_t, TrackInfo> tracks;
    probe.getTrackList(tracks);
    if (probe.getFileBlockSize() == 0)
        return false;
    DemuxedData demuxedData;
    const PIDSet video{pid};
    int64_t discardedSize = 0;
    for (int block = 0; block < 16; block++)
    {
        const bool eof = probe.simpleDemuxBlock(demuxedData, video, discardedSize) == BufferedReader::DATA_EOF;
        if (const int randomAccess = hevcStartsAtRandomAccess(demuxedData[pid]); randomAccess >= 0)
            return randomAccess == 1;
        if (eof)
            break;
    }
    return false;
}

// Where a stream of a transport stream file starts against the first picture of its video that is shown, in 90 kHz
// ticks (TSDemuxer::getTrackStart); 0 for a file without video, or where the stream or the video is not found as far
// as the listing looks for a track that starts late. Only the PES headers are read: no stream is handed on.
//
// The video readers time the pictures from the first one shown. H.264 and MPEG-2 show first the pictures that follow
// the first picture in decode order and are shown before it (an open GOP at the start of a cut), so that is the
// smallest PTS of the video. An HEVC stream that begins with a random access picture times the pictures shown before
// it before the start, as a decoder leaves them out (they refer to pictures before the cut): that is the PTS of the
// first video PES.
static int64_t tsTrackStart(const BufferedReaderManager& readManager, const std::string& fileName, const int pid)
{
    static constexpr int64_t LATE_LIMIT = 1024LL * 1024 * 1024;
    // the pictures shown before the first one follow it in decode order within the depth of the picture reordering,
    // at most 16 frames, or 32 fields
    static constexpr int64_t LEADING_PES = 34;
    TSDemuxer probe(readManager, "");
    probe.openFile(fileName);
    std::map<int32_t, TrackInfo> tracks;
    probe.getTrackList(tracks);
    if (tracks.find(pid) == tracks.end() || !probe.hasVideoStream() || probe.getFileBlockSize() == 0)
        return 0;
    DemuxedData demuxedData;
    const PIDSet none;
    int64_t discardedSize = 0;
    int64_t ticks = 0;
    for (int64_t read = 0; read < LATE_LIMIT; read += probe.getFileBlockSize())
    {
        if (probe.videoPesRead() >= LEADING_PES && probe.getTrackStart(pid, false, ticks))
            break;
        if (probe.simpleDemuxBlock(demuxedData, none, discardedSize) == BufferedReader::DATA_EOF)
            break;
    }
    const auto video = tracks.find(probe.firstVideoPid());
    const bool fromFirstPes = video != tracks.end() &&
                              video->second.m_trackType == static_cast<int>(StreamType::VIDEO_H265) &&
                              tsHevcStartsAtRandomAccess(readManager, fileName, video->first);
    return probe.getTrackStart(pid, fromFirstPes, ticks) ? ticks : 0;
}

static void loadDvOriginalRpus(AbstractDemuxer* demuxer, HevcDolbyVisionFilter* filter)
{
    std::vector<uint8_t> manifestBytes;
    if (demuxer == nullptr || !demuxer->getAttachment(MANIFEST_ATTACHMENT_NAME, manifestBytes))
        return;

    const std::string manifest(manifestBytes.begin(), manifestBytes.end());

    // How the source framed its NALs, if it was recorded. This is independent of Dolby Vision: an
    // ordinary profile 7 file carries the same line, and a file with nothing unusual to say carries
    // no manifest at all.
    const std::string scRule = manifestValue(manifest, "start-code");
    if (!scRule.empty())
        filter->setStartCodeRule(scRule);  // the demuxer reports it, once, for every path that uses it

    // The rest applies only to a profile 8.1 carrier. A manifest that carries nothing but the
    // framing rule has no RPU attachment and must not be treated as if it did.
    if (manifestValue(manifest, "rpu-attachment").empty())
        return;

    const std::string order = manifestValue(manifest, "rpu-order");
    if (order != "display")
        THROW(ERR_COMMON, "Dolby Vision: this file states that its preserved original RPUs are in '"
                              << order << "' order, which this version does not know how to put back.")

    std::vector<uint8_t> rpuBytes;
    std::vector<uint8_t> ptsBytes;
    if (!demuxer->getAttachment(DV_RPU_ATTACHMENT, rpuBytes) ||
        !demuxer->getAttachment(DV_RPU_PTS_ATTACHMENT, ptsBytes))
        THROW(ERR_COMMON,
              "Dolby Vision: this file says it preserves the original RPUs so the disc can be rebuilt, but the "
              "attachments holding them are missing. They are dropped by some tools when a file is copied.")

    const uint32_t rpuCrc =
        static_cast<uint32_t>(crc32(crc32(0, nullptr, 0), rpuBytes.data(), static_cast<uInt>(rpuBytes.size())));
    const uint32_t ptsCrc =
        static_cast<uint32_t>(crc32(crc32(0, nullptr, 0), ptsBytes.data(), static_cast<uInt>(ptsBytes.size())));
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", rpuCrc);
    if (manifestValue(manifest, "rpu-crc32") != buf)
        THROW(ERR_COMMON,
              "Dolby Vision: the preserved original RPUs do not match the checksum this file records for them, so "
              "they have been altered and the disc is not rebuilt from them.")
    snprintf(buf, sizeof(buf), "%08x", ptsCrc);
    if (manifestValue(manifest, "pts-crc32") != buf)
        THROW(ERR_COMMON,
              "Dolby Vision: the timestamps of the preserved original RPUs do not match the checksum this file "
              "records for them, so they have been altered and the disc is not rebuilt from them.")

    DvOriginalRpus originals;
    // Split on the four byte start code that separates the entries, keeping one buffer.
    for (size_t i = 0; i + 4 <= rpuBytes.size(); ++i)
    {
        if (rpuBytes[i] != 0 || rpuBytes[i + 1] != 0 || rpuBytes[i + 2] != 0 || rpuBytes[i + 3] != 1)
            continue;
        if (!originals.offsets.empty())
            originals.lengths.push_back(static_cast<uint32_t>(i - originals.offsets.back()));
        originals.offsets.push_back(static_cast<uint32_t>(i + 4));
        i += 3;
    }
    if (!originals.offsets.empty())
        originals.lengths.push_back(static_cast<uint32_t>(rpuBytes.size() - originals.offsets.back()));

    const size_t declared = static_cast<size_t>(strtoull(manifestValue(manifest, "rpu-count").c_str(), nullptr, 10));
    if (originals.offsets.size() != declared)
        THROW(ERR_COMMON, "Dolby Vision: this file records " << declared << " preserved original RPUs but holds "
                                                             << originals.offsets.size()
                                                             << ", so the disc is not rebuilt from it.")

    if (ptsBytes.size() != originals.offsets.size() * 8)
        THROW(ERR_COMMON,
              "Dolby Vision: this file preserves "
                  << originals.offsets.size() << " original RPUs but " << ptsBytes.size() / 8
                  << " timestamps for them, so they cannot be matched to pictures and the disc is not rebuilt.")

    originals.pts.reserve(ptsBytes.size() / 8);
    for (size_t i = 0; i + 8 <= ptsBytes.size(); i += 8)
    {
        int64_t value = 0;
        for (int b = 0; b < 8; ++b) value = (value << 8) | ptsBytes[i + b];
        originals.pts.push_back(value);
    }

    originals.data = std::move(rpuBytes);
    const size_t count = originals.offsets.size();
    filter->setOriginalRpus(std::move(originals));

    LTRACE(LT_INFO, 2,
           "Dolby Vision: this file was written as profile 8.1 and preserves the disc's own "
               << count
               << " RPUs. They are put back as the layers are separated, so the disc is rebuilt exactly rather "
                  "than from converted metadata.");
}

using namespace std;

// What one track of a container may hold waiting to be muxed. It was 192 MiB, below the 256 MiB a video reader takes a
// picture up to (MAX_TMP_BUFFER_SIZE): a 250 MiB picture from Matroska was refused here, with a message about
// the frame rate. The room above 256 MiB is for the data of the track that follows such a picture.
static constexpr int MAX_DEMUX_BUFFER_SIZE = 1024 * 1024 * (256 + 16);
static constexpr int MIN_READED_BLOCK = 16384;

// ** THE FILE NAME DECIDES WHICH READER OPENS A FILE, AND A NAME CAN BE WRONG. **
//
// A broadcast capture called .mpg is very often a transport stream, and it was then handed to the
// program stream reader. What came back was either an error phrased in that reader's own terms, or
// for several combinations nothing at all: a banner, one blank line and exit 0, which reads as
// "there is nothing in this file".
//
// Measured by copying one real file of each kind under every extension in turn. Every container is
// chosen from the name this way, not only .mpg.
//
// So the name is checked against the first bytes. The rule is deliberately one sided: a file is
// refused only when the content positively identifies a DIFFERENT container. When the content
// cannot be identified, nothing is said and the old path runs, so an elementary stream, a format
// without a signature and anything unusual are all left exactly as they were.
static bool syncsAt(const uint8_t* buf, const int len, const int stride, const int offset)
{
    int seen = 0;
    for (int i = offset; i < len; i += stride)
    {
        if (buf[i] != 0x47)
            return false;
        if (++seen >= 16)
            return true;
    }
    return false;
}

// The families the name can put a file into. ctUnknown means the name says nothing useful, which
// includes every elementary stream.
enum class NamedAs
{
    ctUnknown,
    ctTS,
    ctPS,
    ctMP4,
    ctMKV,
    ctElementary
};

static NamedAs familyOfExt(const std::string& ext)
{
    if (ext == "ts" || ext == "m2ts" || ext == "mts" || ext == "m2t" || ext == "ssif")
        return NamedAs::ctTS;
    if (ext == "mpg" || ext == "mpeg" || ext == "vob" || ext == "evo")
        return NamedAs::ctPS;
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a")
        return NamedAs::ctMP4;
    // WebM is Matroska with a narrower set of codecs, and .mk3d is the name used for 3D Matroska files.
    if (ext == "mkv" || ext == "mka" || ext == "mks" || ext == "mk3d" || ext == "webm")
        return NamedAs::ctMKV;
    // The only elementary extensions the dispatch acts on. Every other unknown extension already
    // reaches the reader that works the codec out from the bytes, so naming one would gain nothing.
    if (ext == "264" || ext == "h264" || ext == "mvc")
        return NamedAs::ctElementary;
    return NamedAs::ctUnknown;
}

// Within the transport family the two shapes are not interchangeable, and only one direction
// sorts itself out. A file of 192 byte packets named .ts is detected by TSDemuxer::checkForRealM2ts,
// which says so and carries on. The other way round there is no such check, because that one only
// ever turns the mode on, so a file of plain 188 byte packets named .m2ts reached the reader as
// though it carried timestamps and came back as "Invalid tsPacket->getHeaderSize".
//
// Making the reader symmetrical would change how every .m2ts file is opened, which is a large
// blast radius for a small gain, so the mismatch is named here instead.
static bool namedAsM2ts(const std::string& ext) { return ext == "m2ts" || ext == "mts" || ext == "ssif"; }

// What the help has always asked of lang=: exactly three letters, in either case. Which three is not checked, so a
// code that worked before still works.
static bool isLangCodeForMeta(const std::string& value)
{
    if (value.size() != 3)
        return false;
    for (const char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    return true;
}

// A well formed BCP 47 language tag (RFC 5646, 2.1) that starts with a language of two or three letters: the language,
// up to three extended languages, then a script, a region, variants, extensions and a private part, each in its
// place. "de", "pt-BR", "zh-Hant-TW", "sr-Latn-RS" and "de-CH-1996" are; "german", "en_US" and "x-klingon" are not.
static bool isBcp47Tag(const std::string& value)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t dash = value.find('-', start);
        parts.push_back(value.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
        if (dash == std::string::npos)
            break;
        start = dash + 1;
    }
    const auto all = [](const std::string& s, auto test)
    {
        if (s.empty())
            return false;
        for (const char c : s)
            if (!test(static_cast<unsigned char>(c)))
                return false;
        return true;
    };
    const auto alpha = [&](const std::string& s) { return all(s, [](unsigned char c) { return isalpha(c) != 0; }); };
    const auto digit = [&](const std::string& s) { return all(s, [](unsigned char c) { return isdigit(c) != 0; }); };
    const auto alnum = [&](const std::string& s) { return all(s, [](unsigned char c) { return isalnum(c) != 0; }); };

    size_t i = 0;
    if (!(alpha(parts[0]) && (parts[0].size() == 2 || parts[0].size() == 3)))
        return false;
    ++i;
    for (int ext = 0; ext < 3 && i < parts.size() && parts[i].size() == 3 && alpha(parts[i]); ++ext) ++i;  // extlang
    if (i < parts.size() && parts[i].size() == 4 && alpha(parts[i]))
        ++i;  // script
    if (i < parts.size() && ((parts[i].size() == 2 && alpha(parts[i])) || (parts[i].size() == 3 && digit(parts[i]))))
        ++i;  // region
    while (i < parts.size() && alnum(parts[i]) &&
           ((parts[i].size() >= 5 && parts[i].size() <= 8) ||
            (parts[i].size() == 4 && isdigit(static_cast<unsigned char>(parts[i][0])) != 0)))
        ++i;  // variants
    while (i < parts.size() && parts[i].size() == 1 && alnum(parts[i]) && tolower(parts[i][0]) != 'x')
    {
        // an extension: its singleton, then one or more parts of 2 to 8
        ++i;
        const size_t first = i;
        while (i < parts.size() && parts[i].size() >= 2 && parts[i].size() <= 8 && alnum(parts[i])) ++i;
        if (i == first)
            return false;
    }
    if (i < parts.size() && parts[i].size() == 1 && tolower(parts[i][0]) == 'x')
    {
        ++i;
        const size_t first = i;
        while (i < parts.size() && !parts[i].empty() && parts[i].size() <= 8 && alnum(parts[i])) ++i;
        if (i == first)
            return false;
    }
    return i == parts.size();
}

static NamedAs familyOfContent(const uint8_t* buf, const int len, const char** name, bool* isM2ts)
{
    if (len >= 192 * 17)
    {
        if (syncsAt(buf, len, 188, 0))
        {
            *name = "a transport stream of plain 188 byte packets, so rename it to .ts";
            *isM2ts = false;
            return NamedAs::ctTS;
        }
        if (syncsAt(buf, len, 192, 4))
        {
            *name = "a transport stream with arrival timestamps, so rename it to .m2ts";
            *isM2ts = true;
            return NamedAs::ctTS;
        }
    }
    if (len >= 16)
    {
        // A pack header. An H.264 stream cannot begin this way: 0xBA sets the bit that H.264
        // requires to be zero in the byte after a start code.
        if (buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0xBA)
        {
            *name = "an MPEG program stream, so rename it to .mpg";
            return NamedAs::ctPS;
        }
        if (buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3)
        {
            *name = "a Matroska file, so rename it to .mkv";
            return NamedAs::ctMKV;
        }
        if (!memcmp(buf + 4, "ftyp", 4))
        {
            *name = "an mp4 or QuickTime file, so rename it to .mp4";
            return NamedAs::ctMP4;
        }
        // A start code, so a raw stream rather than a container. This is tested last on purpose:
        // the length field of the first mp4 box is four bytes and can itself read as 00 00 01 xx,
        // so ftyp has to be ruled out first. A pack header is already handled above, and it is the
        // one start code that does begin a container.
        const bool startCode3 = buf[0] == 0 && buf[1] == 0 && buf[2] == 1;
        const bool startCode4 = buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1;
        if (startCode3 || startCode4)
        {
            *name =
                "a raw stream and not a container at all, so give it a name that is not a "
                "container extension, for example .264";
            return NamedAs::ctElementary;
        }
        // Raw AC-3 or DTS under a container's name: AC-3 named .mp4 was read as an mp4 file, "moov atom not found"
        if (buf[0] == 0x0B && buf[1] == 0x77)
        {
            *name = "a raw AC-3 stream and not a container at all, so rename it to .ac3";
            return NamedAs::ctElementary;
        }
        if (buf[0] == 0x7F && buf[1] == 0xFE && buf[2] == 0x80 && buf[3] == 0x01)
        {
            *name = "a raw DTS stream and not a container at all, so rename it to .dts";
            return NamedAs::ctElementary;
        }
    }
    return NamedAs::ctUnknown;
}

// A name without an extension, or with one tsMuxeR does not know, sends the file to the raw stream readers. A transport
// stream or a Matroska file named so was "Can't detect stream type", or read as junk frames: such a name is now told
// when the file is a container. Any other content under such a name is left to the readers, as before.
static void checkNameMatchesContent(const std::string& fileName, const std::string& ext)
{
    const NamedAs named = familyOfExt(ext);

    File file;
    if (!file.open(fileName.c_str(), File::ofRead))
        return;  // let whatever opens it next report that properly
    uint8_t buf[192 * 20];
    const int len = file.read(buf, sizeof(buf));
    file.close();
    if (len <= 0)
        return;

    const char* what = nullptr;
    bool contentIsM2ts = false;
    const NamedAs found = familyOfContent(buf, len, &what, &contentIsM2ts);
    if (found == NamedAs::ctUnknown)
        return;
    if (named == NamedAs::ctUnknown)
    {
        if (found == NamedAs::ctElementary)
            return;
        if (ext.empty())
            THROW(ERR_COMMON, "The file " << fileName
                                          << " has no extension, and the name is what decides how it is read, but it "
                                             "is "
                                          << what << " and try again.")
    }

    if (found == named)
    {
        // Same family. Only the one direction the reader cannot sort out is worth naming: plain
        // packets under a name that promises timestamps. The reverse is detected further down and
        // already works, so it is left alone.
        if (found != NamedAs::ctTS || contentIsM2ts || !namedAsM2ts(ext))
            return;
    }

    THROW(ERR_COMMON, "The file " << fileName << " is named ." << ext
                                  << ", and the name is what decides how it is read, but it is " << what
                                  << " and try again.")
}

METADemuxer::METADemuxer(const BufferedReaderManager& readManager)
    : m_containerReader(*this, readManager), m_shiftedSubReader(*this, readManager), m_readManager(readManager)
{
    m_flushDataMode = false;
    m_HevcFound = false;
    m_MvcFound = false;
    m_totalSize = 0;
    m_lastProgressY = 0;
    m_lastReadRez = 0;
}

METADemuxer::~METADemuxer()
{
    readClose();
    for (FileListIterator*& m_iterator : m_iterators) delete m_iterator;
}

int64_t METADemuxer::getDemuxedSize()
{
    int64_t rez = 0;
    for (const StreamInfo& si : m_codecInfo)
        rez += si.m_streamReader->getProcessedSize();  // m_codecInfo[i].m_dataProcessed;
    return rez + m_containerReader.getDiscardedSize();
}

int METADemuxer::readPacket(AVPacket& avPacket)

{
    avPacket.stream_index = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.addedBytes = 0;
    avPacket.codec = nullptr;
    m_lastReadRez = 0;
    while (true)
    {
        int minDtsIndex = -1;
        int64_t minDts = LLONG_MAX;
        bool allDataDelayed = true;
        while (allDataDelayed)
        {
            for (int i = 0; i < static_cast<int>(m_codecInfo.size()); i++)
            {
                StreamInfo& streamInfo = m_codecInfo[i];
                if (!m_flushDataMode)
                {
                    // A negative timeshift: its packets are due this much before their place in the file. 10 s
                    // more, as the time a file has been read to is that of its latest packet, and a transport
                    // stream carries its video well ahead of a subtitle that is due at the same time.
                    if (streamInfo.m_timeShift < 0)
                        if (auto* wrapper = dynamic_cast<ContainerToReaderWrapper*>(streamInfo.m_dataReader))
                            wrapper->setReadAhead(streamInfo.m_readerID,
                                                  m_muxPos - streamInfo.m_timeShift + 10 * INTERNAL_PTS_FREQ);
                    streamInfo.lastReadRez = streamInfo.read();
                    if (streamInfo.lastReadRez == BufferedFileReader::DATA_DELAYED)
                        continue;  // skip stream
                    allDataDelayed = false;
                    if (streamInfo.lastReadRez == BufferedFileReader::DATA_NOT_READY)
                    {
                        m_lastReadRez = BufferedFileReader::DATA_NOT_READY;
                        return BufferedFileReader::DATA_NOT_READY;
                    }
                    if (streamInfo.lastReadRez != BufferedFileReader::DATA_EOF2)
                    {
                        if (streamInfo.m_lastDTS < minDts)
                        {
                            minDtsIndex = i;
                            minDts = streamInfo.m_lastDTS;
                        }
                    }
                    else if (streamInfo.m_lastDTS < minDts && !streamInfo.m_flushed)
                    {
                        minDtsIndex = i;
                        minDts = streamInfo.m_lastDTS;
                    }
                }
                else
                {
                    allDataDelayed = false;
                    if (streamInfo.m_lastDTS < minDts && !streamInfo.m_flushed)
                    {
                        minDtsIndex = i;
                        minDts = streamInfo.m_lastDTS;
                    }
                }
            }
            if (allDataDelayed)
                for (const StreamInfo& si : m_codecInfo)
                {
                    const auto cReader = dynamic_cast<ContainerToReaderWrapper*>(si.m_dataReader);
                    if (cReader)
                        cReader->resetDelayedMark();
                }
        }
        if (minDtsIndex != -1)
        {
            m_muxPos = (std::max)(m_muxPos, minDts);
            StreamInfo& chosen = m_codecInfo[minDtsIndex];
            if (chosen.m_held)
            {
                // the packet after a gap of a join, now that the other streams have come up to it (below)
                avPacket = chosen.m_heldPacket;
                chosen.m_held = false;
                chosen.m_lastDTS =
                    avPacket.dts +
                    (avPacket.codecID == CODEC_S_SRT || avPacket.codecID == CODEC_S_MKVCOPY ? 0 : avPacket.duration);
                if (!m_quiet)
                    updateReport(true);
                return 0;
            }
            const int64_t endBefore = chosen.m_lastDTS;
            if (!m_flushDataMode)
            {
                if (m_codecInfo[minDtsIndex].lastReadRez != BufferedFileReader::DATA_EOF2)
                {
                    // ** THE PACKET STARTS AT THIS STREAM'S OWN POSITION. ** A reader that asks for more data
                    // without saying where its stream stands (the PGS, SRT, Matroska copy and video copy readers)
                    // left the packet as the stream before had it: the time of another track, or this one's with
                    // the timeshift added, and the timeshift was added again below. With timeshift=5000ms 57 of 95
                    // subtitles of a disc came up to 10 s after they were due. A reader that does say keeps its
                    // own word; one that does not leaves its stream where it stood.
                    avPacket.data = nullptr;
                    avPacket.size = 0;
                    avPacket.duration = 0;
                    avPacket.dts = avPacket.pts =
                        m_codecInfo[minDtsIndex].m_lastDTS - m_codecInfo[minDtsIndex].m_timeShift;
                    const int res = m_codecInfo[minDtsIndex].m_streamReader->readPacket(avPacket);
                    m_codecInfo[minDtsIndex].m_lastAVRez = res;
                }
                else
                {
                    // flush single stream
                    // a raw video file read to its end: whether its last picture is whole (K19)
                    StreamInfo& ended = m_codecInfo[minDtsIndex];
                    if (auto* video = dynamic_cast<MPEGStreamReader*>(ended.m_streamReader);
                        video && !m_quiet && !ended.m_sourceFiles.empty() &&
                        video->getSrcContainerType() == AbstractStreamReader::ContainerType::ctNone)
                        video->checkFileEnd(ended.m_sourceFiles.back());
                    m_codecInfo[minDtsIndex].m_streamReader->flushPacket(avPacket);
                    m_codecInfo[minDtsIndex].m_flushed = true;
                }
                // add time shift from external sync source
                // add static time shift
                avPacket.dts += m_codecInfo[minDtsIndex].m_timeShift;
                avPacket.pts += m_codecInfo[minDtsIndex].m_timeShift;
                // A text subtitle's duration is how long it stays on screen, not how long until the
                // next one, which can start sooner and even overlap it. Timed by its end, an overlapping
                // subtitle was read after the other tracks had passed its start.
                m_codecInfo[minDtsIndex].m_lastDTS =
                    avPacket.dts +
                    (avPacket.codecID == CODEC_S_SRT || avPacket.codecID == CODEC_S_MKVCOPY ? 0 : avPacket.duration);
                // ** THE SOUND AFTER THE GAP OF A JOIN WAITS FOR ITS TIME. ** A stream is chosen by where its last
                // packet ends, and where a join put a gap in its times (takeTimeGaps), its next packet comes the gap
                // later than that. It was handed on at once, ahead of the video before it, and a transport stream
                // takes its clock from each packet's time: the clock went on by the gap in PCR-only packets, and the
                // pictures of the next file arrived after their decoding times. An MPEG-2 TS joined with itself,
                // whose sound starts 0.48 s after its picture: five PCR-only packets took the clock 0.43 s on, and
                // 3 pictures came up to 71 ms late; three MP4 parts whose AAC is shorter than their picture: one
                // picture 9.8 ms late at each joint. That packet is now held back until the other streams have come
                // up to its time; the gap itself, which keeps the sound of each file against its picture, stays.
                if (!chosen.m_joinGaps.empty() && !chosen.m_flushed && avPacket.size > 0 &&
                    avPacket.dts - endBefore >= chosen.m_joinGaps.front() / 2)
                {
                    chosen.m_joinGaps.erase(chosen.m_joinGaps.begin());
                    bool othersBehind = false;
                    for (int i = 0; i < static_cast<int>(m_codecInfo.size()); i++)
                        if (i != minDtsIndex && !m_codecInfo[i].m_flushed &&
                            m_codecInfo[i].lastReadRez != BufferedFileReader::DATA_EOF2 &&
                            m_codecInfo[i].m_lastDTS < avPacket.dts)
                            othersBehind = true;
                    if (othersBehind)
                    {
                        chosen.m_heldPacket = avPacket;
                        chosen.m_held = true;
                        chosen.m_lastDTS = avPacket.dts;
                        continue;
                    }
                }
            }
            else
            {  // flush all streams
                m_codecInfo[minDtsIndex].m_streamReader->flushPacket(avPacket);
                m_codecInfo[minDtsIndex].m_flushed = true;
            }
            if (!m_quiet)
                updateReport(true);
            return 0;
        }
        if (!m_flushDataMode)
            m_flushDataMode = true;
        else
        {
            if (!m_quiet)
                updateReport(false);
            m_lastReadRez = BufferedFileReader::DATA_EOF;
            return BufferedReader::DATA_EOF;
        }
    }
}

void METADemuxer::openFile(const string& streamName)
{
    m_streamName = streamName;
    readClose();

    TextFile file(m_streamName.c_str(), File::ofRead);
    string str;

    // A track folded into a TrueHD track by merge-ac3-track must not ALSO be muxed as a stream of
    // its own. That combination opened the same track of the same file twice on one container and
    // then crashed while closing, AFTER printing "Mux successful complete", leaving a file that
    // looked finished. It is also meaningless: the compatibility core would be written twice, once
    // braided into the TrueHD track and once beside it.
    //
    // It matters because it is what the interface produces if you set the merge and leave the AC-3
    // track ticked in the track list, which is the natural thing to do.
    //
    // Checked as the lines are read, against what has been read SO FAR, in both directions, so it
    // is caught whichever order the two lines appear in. It has to refuse BEFORE the second of the
    // two streams is opened: refusing afterwards would leave the duplicate open and the close would
    // still be the one that crashes.
    std::set<std::pair<string, int>> muxedTracks;
    std::set<std::pair<string, int>> mergeSources;

    file.readLine(str);
    while (str.length() > 0)
    {
        // A tab outside quotes is a space, as on the MUXOPT line: a tab between the fields of a track line, or a
        // line holding only a tab, stopped the job
        bool quoted = false;
        for (char& c : str)
        {
            if (c == '"')
                quoted = !quoted;
            else if (c == '\t' && !quoted)
                c = ' ';
        }
        str = trimStr(str);
        if (str.length() == 0 || str[0] == '#')
        {
            file.readLine(str);
            continue;
        }
        // A MUXOPT line indented with a tab and then spaces was taken for a track line and stopped the job
        if (strStartWith(str.substr((std::min)(str.find_first_not_of(" \t"), str.size())), "MUXOPT"))
        {
            file.readLine(str);
            continue;
        }
        // A quote that is not closed, such as one inside a track-name, cut the line there: the rest was read as
        // parameters no track has, with a warning for each, and the name was lost
        if (std::count(str.begin(), str.end(), '"') % 2 != 0)
            THROW(ERR_COMMON, "A quote on this meta line is not closed, so it cannot be read: "
                                  << trimStr(str)
                                  << ". A file name or a value with a comma in it goes in quotes; a value cannot hold "
                                     "a quote itself.")
        vector<string> params = splitQuotedStr(str.c_str(), ',');
        if (params.size() < 2)
            THROW(ERR_INVALID_CODEC_FORMAT, "Invalid codec format: " << str)
        map<string, string> addParams;
        for (unsigned i = 2; i < params.size(); i++)
        {
            // params[i] = strToLowerCase ( params[i] );
            // The value is everything after the first '=': a track-name with an '=' in it was cut there.
            const size_t eq = params[i].find('=');
            const string paramName = trimStr(params[i].substr(0, eq));
            const string paramValue = trimStr(eq == string::npos ? "" : params[i].substr(eq + 1));
            // A parameter given twice, such as lang=eng and lang=ger, took the last value without a word
            if (const auto twice = addParams.find(paramName); twice != addParams.end() && !paramName.empty())
                LTRACE(LT_WARN, 2,
                       "Warning: " << paramName << " is given twice on the line of " << trimStr(params[1]) << " ("
                                   << (twice->second.empty() ? paramName : paramName + "=" + twice->second) << " and "
                                   << (paramValue.empty() ? paramName : paramName + "=" + paramValue)
                                   << "). The last one is used; remove the other.");
            addParams[paramName] = paramValue;
            // Digits alone, or digits and a unit, with no name: the part after a decimal comma, as the parameters are
            // parted at commas. fps=23,976 became fps=23, and timeshift=1,5s timeshift=1 (1 ms), with only a warning
            // about "976" or "5s".
            // Every unit a time takes, and a space before it: timeshift=1,500us and timeshift=1,5 s were read as 1 ms.
            const size_t digitsEnd = paramName.find_first_not_of("0123456789");
            const std::string unit =
                digitsEnd == string::npos ? std::string() : strToLowerCase(trimStr(paramName.substr(digitsEnd)));
            if (eq == string::npos && i > 0 && !paramName.empty() && digitsEnd != 0 &&
                (digitsEnd == string::npos || unit == "ns" || unit == "us" || unit == "ms" || unit == "s" ||
                 unit == "min" || unit == "h"))
                THROW(ERR_COMMON, "\"" << trimStr(params[i - 1]) << "," << paramName
                                       << "\" on a track line: a decimal comma parts the value in two, as the "
                                          "parameters are parted at commas. Write it with a point: "
                                       << trimStr(params[i - 1]) << "." << paramName << ".")
            // A track number is digits alone: track=1.5 was read as track 1, and track=-1 as no track, which skipped
            // the check that the track is in the file and made an empty output that said it was successful
            if (paramName == "track" &&
                (addParams[paramName].empty() || addParams[paramName].find_first_not_of("0123456789") != string::npos ||
                 addParams[paramName].size() > 9))
                THROW(ERR_COMMON, "track=" << addParams[paramName]
                                           << " is not a track number. Write the number the listing gives, digits "
                                              "alone, such as track=1 or track=4113.")
            // The Matroska flags took any value that is not a "no" word as a yes: forced=2 and original=bogus set them
            if (paramName == "forced" || paramName == "hearing-impaired" || paramName == "visual-impaired" ||
                paramName == "text-descriptions" || paramName == "original" || paramName == "commentary")
            {
                const string lower = strToLowerCase(addParams[paramName]);
                if (!lower.empty() && !defaultParamMeansNo(lower) && lower != "1" && lower != "yes" &&
                    lower != "true" && lower != "on")
                    THROW(ERR_COMMON, paramName << "=" << addParams[paramName] << " is not a value of " << paramName
                                                << ". Write " << paramName
                                                << " alone (or =1, =yes) to set the flag, or " << paramName
                                                << "=0 (or =no) to clear it.")
            }
            if (paramName == "default" && defaultParamKind(addParams[paramName]) < 0)
                THROW(ERR_COMMON, "default=" << addParams[paramName]
                                             << " is not a value of default. Write all (or yes) for the default track, "
                                                "forced for the forced subtitles only, or none (or no).")
            if (!isKnownTrackParam(paramName))
            {
                // A misspelled parameter used to be accepted in silence and simply do nothing,
                // so "langauge=deu" looked like it had worked. Warn rather than fail: a meta
                // written for a newer build must keep working on an older one, and refusing
                // would break that.
                LTRACE(LT_WARN, 2, "Warning: unknown track parameter \"" << paramName << "\" ignored.");
            }
        }
        string codec = trimStr(params[0]);
        string codecStreamName = trimStr(params[1]);
        // title= names a title of a Blu-ray or DVD disc folder or image (discTitles.h, dvdTitles.h): the line reads the
        // title's playlist, or the cells of the DVD title, whose audio takes its language from the disc. dvd-title=
        // names a DVD title by the disc's own number, as the window writes it: that stays the same whatever the
        // listing of titles makes of the disc.
        auto title = addParams.find("title");
        const bool byDiscNumber = title == addParams.end() && addParams.count("dvd-title");
        if (byDiscNumber)
            title = addParams.find("dvd-title");
        else if (title != addParams.end() && addParams.count("dvd-title"))
            THROW(ERR_COMMON, "title= and dvd-title= both name a title on one line: give one of them.")
        if (title != addParams.end())
        {
            const string disc = unquoteStr(codecStreamName);
            try
            {
                const string videoTs = blurayDiscRoot(disc).empty() ? dvdVideoTs(disc) : string();
                if (byDiscNumber && videoTs.empty())
                    throw std::runtime_error("dvd-title= names a title of a DVD, and " + disc +
                                             " is no DVD folder or image: use title= for a Blu-ray");
                if (!videoTs.empty())
                {
                    std::vector<std::pair<int, std::string>> languages;
                    std::vector<double> chapters;
                    codecStreamName = dvdTitleSource(videoTs, title->second, &languages, &chapters, byDiscNumber);
                    if (m_dvdChapters.empty())
                        for (const double start : chapters)
                            m_dvdChapters.emplace_back(llround(start * 1e9), std::string());
                    LTRACE(LT_INFO, 2,
                           title->first << "=" << title->second << " of " << disc << " is read as " << codecStreamName
                                        << ".");
                    const auto track = addParams.find("track");
                    if (!addParams.count("lang") && track != addParams.end())
                        for (const auto& [id, lang] : languages)
                            if (std::to_string(id) == track->second)
                                if (const string code = iso639FromBcp47(lang); !code.empty())
                                    addParams["lang"] = code;
                }
                else
                {
                    const string playlist = discTitlePlaylist(disc, title->second);
                    LTRACE(LT_INFO, 2,
                           "title=" << title->second << " of " << disc << " is the playlist " << playlist << ".");
                    codecStreamName = "\"" + playlist + "\"";
                }
            }
            catch (const std::exception& e)
            {
                THROW(ERR_COMMON, e.what() << ".")
            }
            addParams.erase(title);
        }
        codec = strToUpperCase(codec);
        // Only an HEVC reader takes hdr10-override; on another track, such as H.264 on a disc, it did nothing, without
        // a word.
        if (addParams.count("hdr10-override") && codec != "V_MPEGH/ISO/HEVC")
            LTRACE(
                LT_WARN, 2,
                "Warning: hdr10-override applies to HEVC video only, and is ignored for this " << codec << " track.");
        // level= is written into H.264 and HEVC streams only; on VVC, AV1, VP9 or any other track it did nothing,
        // without a word
        if (addParams.count("level") && codec != "V_MPEG4/ISO/AVC" && codec != "V_MPEG4/ISO/MVC" &&
            codec != "V_MPEGH/ISO/HEVC")
            LTRACE(
                LT_WARN, 2,
                "Warning: level= applies to H.264 and HEVC video only, and is ignored for this " << codec << " track.");
        // ** THE KEY IS THE FILE THE LINE OPENS, NOT THE TEXT THAT NAMES IT. **
        //
        // The guard below refuses a track that is both merged into a TrueHD line and muxed on its
        // own, because the two readers then share one DemuxerData and the second close frees what
        // the first still holds. It keyed on the TYPED NAME, and a JOIN is written as "a"+"b", so
        // the joined line and the plain line naming "a" produced different keys and the joined line
        // walked straight past it into the same access violation, after printing "Mux successful
        // complete". Measured on the shipped binary: four separate shapes segfault, of which the
        // smallest is one media file joined to itself.
        //
        // ContainerToReaderWrapper keys its demuxers on fileList[0], not on the typed text, so that
        // is what decides whether two readers share anything. extractFileList is the same splitter
        // addStream itself uses below and the loss report uses in main.cpp: it unquotes, splits an
        // UNQUOTED plus, and leaves a plus INSIDE a quoted path alone, so a file genuinely named
        // with a plus sign is still one file and is still caught when it really is a duplicate.
        const string streamKey = extractFileList(codecStreamName)[0];
        if (codec == "A_MLP" && (addParams.find("merge-ac3-track") != addParams.end() ||
                                 addParams.find("merge-ac3-file") != addParams.end()))
        {
            const bool haveTrack =
                addParams.find("merge-ac3-track") != addParams.end() && !addParams.at("merge-ac3-track").empty();
            const bool haveFile =
                addParams.find("merge-ac3-file") != addParams.end() && !addParams.at("merge-ac3-file").empty();
            if (haveTrack && haveFile)
                THROW(ERR_INVALID_CODEC_FORMAT, "Specify only one of merge-ac3-track or merge-ac3-file.")
            // An empty value reached the merge reader, which stopped with a message of its own inner workings
            if (!haveTrack && !haveFile)
                THROW(ERR_INVALID_CODEC_FORMAT,
                      (addParams.count("merge-ac3-file") ? "merge-ac3-file=" : "merge-ac3-track=")
                          << " has no value on the A_MLP line of " << codecStreamName
                          << ". Give the AC-3 file, such as merge-ac3-file=\"compat.ac3\", or the number of the AC-3 "
                             "track, such as merge-ac3-track=3, or leave the parameter out.")

            if (haveTrack)
            {
                const int thdPid =
                    addParams.find("track") != addParams.end() ? strToInt32(addParams.at("track").c_str()) : 0;
                const int ac3Pid = strToInt32(addParams.at("merge-ac3-track").c_str());
                if (thdPid == 0)
                    THROW(ERR_INVALID_CODEC_FORMAT,
                          "A_MLP with merge-ac3-track requires track=<TrueHD Matroska track number>.")
                if (ac3Pid <= 0)
                    THROW(ERR_INVALID_CODEC_FORMAT, "merge-ac3-track must be a positive Matroska track number.")
                if (ac3Pid == thdPid)
                    THROW(ERR_INVALID_CODEC_FORMAT,
                          "merge-ac3-track must be a different track number than the TrueHD track= parameter.")
                if (muxedTracks.find({streamKey, ac3Pid}) != muxedTracks.end())
                    THROW(ERR_INVALID_CODEC_FORMAT,
                          "Track "
                              << ac3Pid
                              << " is already muxed as a track of its own, so it cannot also be merged into the TrueHD "
                                 "track with merge-ac3-track. Remove that track from the mux and keep only the TrueHD "
                                 "line: merge-ac3-track already carries the AC-3 core inside it, which is where a "
                                 "Blu-ray expects it.")
                mergeSources.insert({streamKey, ac3Pid});
            }
        }
        else if (codec != "A_MLP")
        {
            // Only an A_MLP line builds a TrueHD track with a core from two sources; on another line the
            // parameter did nothing, without a word.
            for (const char* name : {"merge-ac3-file", "merge-ac3-track"})
                if (addParams.count(name))
                    LTRACE(LT_WARN, 2,
                           "Warning: " << name << " is used on an A_MLP line only, and is ignored on this " << codec
                                       << " line of " << codecStreamName
                                       << ". To give a TrueHD stream its AC-3 core, name the TrueHD on an A_MLP line.");
        }
        {
            // The same rule seen from the other side, for a plain track line that names a track an
            // earlier line already claimed as its merge source.
            const auto trackParam = addParams.find("track");
            if (trackParam != addParams.end() && !trackParam->second.empty())
            {
                const int pid = strToInt32(trackParam->second.c_str());
                if (mergeSources.find({streamKey, pid}) != mergeSources.end())
                    THROW(ERR_INVALID_CODEC_FORMAT,
                          "Track " << pid
                                   << " is already being merged into a TrueHD track with merge-ac3-track, so it cannot "
                                      "also be muxed as a track of its own. Remove this line: the AC-3 core is carried "
                                      "inside the TrueHD track, which is where a Blu-ray expects it.")
                muxedTracks.insert({streamKey, pid});
            }
        }
        if (!m_HevcFound)
            m_HevcFound = (codec.find("HEVC") == 12);
        if (!m_MvcFound)
            m_MvcFound = codec == "V_MPEG4/ISO/MVC";
        if (codec == "V_MPEG4/ISO/MVC" && m_mvcSource.empty())
            m_mvcSource = unquoteStr(trimStr(splitQuotedStr(codecStreamName.c_str(), '+')[0]));
        addStream(codec, codecStreamName, addParams);
        file.readLine(str);
    }

    // A default= the user wrote on a subtitle line wins over one taken from a file name: a disc has one default
    // subtitle, the first that has one, so an earlier file named "forced" took it from a later line that said
    // default=all. The forced file keeps its forced mark for a Matroska output.
    bool defaultWritten = false;
    for (const auto& si : m_codecInfo)
        if (!si.m_codec.empty() && si.m_codec[0] == 'S' && si.m_addParams.count("default") &&
            !si.m_addParams.count("defaultfromname"))
            defaultWritten = true;
    for (auto& si : m_codecInfo)
    {
        if (!si.m_addParams.count("defaultfromname"))
            continue;
        si.m_addParams.erase("defaultfromname");
        if (defaultWritten)
        {
            si.m_addParams.erase("default");
            si.m_addParams.try_emplace("forced", "");
        }
    }

    auto primarySEI = H264StreamReader::SeiMethod::SEI_NotDefined;
    for (const auto& si : m_codecInfo)
    {
        const auto reader = dynamic_cast<H264StreamReader*>(si.m_streamReader);
        if (reader && !reader->isSubStream())
        {
            primarySEI = reader->getInsertSEI();
            break;
        }
    }

    if (primarySEI != H264StreamReader::SeiMethod::SEI_NotDefined)
    {
        bool warned = false;
        for (const auto& i : m_codecInfo)
        {
            const auto reader = dynamic_cast<H264StreamReader*>(i.m_streamReader);
            if (reader && reader->isSubStream())
            {
                if (!warned && reader->getInsertSEI() != primarySEI)
                {
                    LTRACE(LT_INFO, 2,
                           "Parameter 'insertSEI' for MVC dependent view differs from same param for AVC base view. "
                           "Ignore and apply same value");
                    warned = true;
                }
                reader->setInsertSEI(primarySEI);
            }
        }
    }
    placeJoinedSubtitles();
    checkSubtitlePlanes();
}

// ** A 3D PLANE IS ONE OF THE MVC VIDEO. ** The planes of a 3D disc are the offset sequences the MVC video carries, as
// many as its listing gives as 3d-pg-planes. A 3d-plane= past the last of them named a plane no player has, and the
// disc was written without a word. Read from the start of the MVC source, as the listing reads it; a source that does
// not say (or a playlist) is not checked.
void METADemuxer::checkSubtitlePlanes() const
{
    if (m_mvcSource.empty())
        return;
    const string ext = strToLowerCase(extractFileExt(m_mvcSource));
    if (ext == "mpls" || ext == "mpl")
        return;
    bool asked = false;
    for (const StreamInfo& si : m_codecInfo)
        if (si.m_addParams.count("3d-plane") && parse3dPlane(si.m_addParams.at("3d-plane")) != 0xff)
            asked = true;
    if (!asked)
        return;
    int planes = -1;
    for (const CheckStreamRez& s : DetectStreamReader(m_readManager, m_mvcSource, false).streams)
        if (const size_t at = s.streamDescr.find("3d-pg-planes: "); at != string::npos)
        {
            planes = strToInt32(s.streamDescr.substr(at + 14).c_str());
            break;
        }
    if (planes < 0)
        return;
    for (const StreamInfo& si : m_codecInfo)
    {
        const auto it = si.m_addParams.find("3d-plane");
        if (it == si.m_addParams.end())
            continue;
        const uint8_t plane = parse3dPlane(it->second);
        if (plane != 0xff && plane >= planes)
            THROW(ERR_COMMON, "3d-plane=" << it->second << " on the line of " << unquoteStr(si.m_streamName)
                                          << " is not a plane of the MVC video, which has " << planes
                                          << (planes == 1 ? " plane, 0" : " planes, 0 to " + int32ToStr(planes - 1))
                                          << ". Give one of them, or 3d-plane=255 for none.")
    }
}

// ** THE SUBTITLE FILES OF A JOIN FOLLOW ITS VIDEO. ** A join of .srt or .sup files ("a.srt"+"b.srt") read each file at
// its own times, so the subtitles of every later file came at the start, among those of the first. Each later file
// starts where the video joined from as many files has its file of the same place in the join start: the pictures of
// the files before, as the video reader times them. MP4, MOV and Matroska files say how long their video is. A
// transport stream and an elementary video stream do not, and the subtitles of a later file came at their own times:
// the files before the last are read through for it, a transport stream by the decoding times of its video, as a join
// lays its files (TSDemuxer::fileVideoLengthNs), an elementary stream by its own reader (esVideoLengthNs). A join
// inside a container moves its subtitles itself.
int64_t METADemuxer::esVideoLengthNs(const std::string& codec, const std::map<std::string, std::string>& addParams,
                                     const std::string& fileName)
{
    std::unique_ptr<AbstractStreamReader> reader;
    try
    {
        reader.reset(createCodec(codec, addParams, fileName, std::vector<MPLSPlayItem>(), true));
    }
    catch (...)
    {
        return -1;
    }
    auto* video = dynamic_cast<MPEGStreamReader*>(reader.get());
    File file;
    if (!video || !file.open(fileName.c_str(), File::ofRead))
        return -1;
    static constexpr uint32_t BLOCK = 2 * 1024 * 1024;
    std::vector<uint8_t> buffer(MAX_AV_PACKET_SIZE + BLOCK);
    int64_t first = INT64_MAX, last = INT64_MIN;
    bool eof = false;
    AVPacket packet;
    try
    {
        for (;;)
        {
            const int rez = reader->readPacket(packet);
            if (rez == AbstractStreamReader::NEED_MORE_DATA)
            {
                if (eof)
                {
                    // the last picture, which the reader holds until the end of the stream, as the mux flushes it
                    reader->flushPacket(packet);
                    if (packet.size > 0 && packet.pts != AV_NOPTS_VALUE_INTERNAL)
                    {
                        first = (std::min)(first, packet.pts);
                        last = (std::max)(last, packet.pts);
                    }
                    break;
                }
                const int got = file.read(buffer.data() + MAX_AV_PACKET_SIZE, BLOCK);
                eof = got < static_cast<int>(BLOCK);
                reader->setBuffer(buffer.data(), static_cast<uint32_t>((std::max)(got, 0)), eof);
                continue;
            }
            if (rez != 0)
                break;
            if (packet.size > 0 && packet.pts != AV_NOPTS_VALUE_INTERNAL)
            {
                first = (std::min)(first, packet.pts);
                last = (std::max)(last, packet.pts);
            }
        }
    }
    catch (...)
    {
        return -1;
    }
    file.close();
    const double fps = video->getFPS();
    if (first > last || fps <= 0)
        return -1;
    // from the first picture shown to the end of the last, one picture after its time
    return static_cast<int64_t>(static_cast<double>(last - first) * 1e9 / INTERNAL_PTS_FREQ + 1e9 / fps + 0.5);
}

void METADemuxer::placeJoinedSubtitles()
{
    for (StreamInfo& sub : m_codecInfo)
    {
        auto* srt = dynamic_cast<SRTStreamReader*>(sub.m_streamReader);
        auto* pgs = dynamic_cast<PGSStreamReader*>(sub.m_streamReader);
        if (!srt && !pgs)
            continue;
        const std::vector<std::string> subFiles = splitQuotedStr(sub.m_fullStreamName.c_str(), '+');
        if (subFiles.size() < 2)
            continue;
        // A join of Matroska files without video (subtitles only, as save-srt or a demux leaves them) is read by a
        // demuxer of its own, which placed each file where the subtitles of the one before end: the second file's
        // subtitles came seconds early. It takes the same starts. Other containers move their subtitles themselves.
        MatroskaDemuxer* mkvJoin = nullptr;
        if (dynamic_cast<ContainerToReaderWrapper*>(sub.m_dataReader))
        {
            const auto it = m_containerReader.m_demuxers.find(sub.m_streamName);
            mkvJoin = it != m_containerReader.m_demuxers.end() ? dynamic_cast<MatroskaDemuxer*>(it->second.m_demuxer)
                                                               : nullptr;
            int64_t start = 0, end = 0;
            if (!mkvJoin)
                continue;
            MatroskaDemuxer first(m_readManager);
            first.openFile(unquoteStr(trimStr(subFiles[0])));
            if (first.getVideoSpanNano(start, end))
                continue;  // a file with video: the join follows its own video
        }
        for (const StreamInfo& video : m_codecInfo)
        {
            if (video.m_codec.empty() || video.m_codec[0] != 'V')
                continue;
            const std::vector<std::string> files = splitQuotedStr(video.m_fullStreamName.c_str(), '+');
            if (files.size() != subFiles.size())
                continue;
            std::vector<int64_t> starts{0};
            int64_t prevEnd = 0;
            for (size_t i = 0; i < files.size(); ++i)
            {
                const std::string file = unquoteStr(trimStr(files[i]));
                const std::string ext = strToUpperCase(extractFileExt(file));
                int64_t start = 0;
                int64_t end = 0;
                if (ext == "MP4" || ext == "MOV" || ext == "M4V")
                {
                    MovDemuxer part(m_readManager);
                    part.openFile(file);
                    end = part.videoLengthNs();
                }
                else if (ext == "MKV" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
                {
                    MatroskaDemuxer part(m_readManager);
                    part.openFile(file);
                    if (!part.getVideoSpanNano(start, end))
                        end = -1;
                }
                else if (i + 1 == files.size())
                    end = 1;  // the last file: where it starts is all that is needed
                else
                {
                    LTRACE(LT_INFO, 2,
                           "Subtitles joined from " << subFiles.size() << " files: reading " << file
                                                    << " for how long its video is.");
                    end = ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T"
                              ? TSDemuxer::fileVideoLengthNs(m_readManager, file)
                              : esVideoLengthNs(video.m_codec, video.m_addParams, file);
                }
                if (end <= 0)
                {
                    starts.clear();  // a length that cannot be read: the files are placed as before
                    break;
                }
                if (i > 0)
                    starts.push_back(starts.back() + prevEnd - start);
                prevEnd = end;
            }
            if (starts.size() == files.size())
            {
                if (mkvJoin)
                    mkvJoin->setJoinStarts(starts);
                else if (srt)
                    srt->setJoinStarts(starts);
                else
                    pgs->setJoinStarts(starts);
                LTRACE(LT_INFO, 2,
                       "Subtitles joined from " << subFiles.size()
                                                << " files: each file starts where the video file at its place in "
                                                   "the join starts.");
            }
            break;
        }
    }
}

// The folder above the one that holds the playlist is the disc's BDMV folder, and STREAM is in it; for a playlist of
// BACKUP, one more folder up. A bare name means the working directory holds the playlist, so BDMV is one above it.
// The listing and the meta path each had their own copy of this, and both took a playlist named with exactly one
// folder, PLAYLIST/00000.mpls from inside BDMV, for one with no BDMV at all: the listing showed no stream folder and
// the meta path gave every clip an empty name. The meta path also looked for the streams of a BACKUP playlist inside
// BACKUP, where there are none.
//
// The folders are looked at, not only their names: a disc folder whose BDMV folder is itself named BACKUP has its
// STREAM beside that BACKUP's PLAYLIST, and was sent one folder too high. A name relative to the working
// directory says nothing of the folders above it, so ./00000.mpls, a bare name in BACKUP/PLAYLIST
// and PLAYLIST/00000.mpls from inside BACKUP took STREAM from the wrong folder.
std::string METADemuxer::playlistStreamDir(const std::string& mplsFileName)
{
    const char sep = getDirSeparator();
    const string up = string("..") + sep;
    // The folder above a folder path that ends with a separator (an empty path is the working directory), as a path
    // that ends with a separator again; empty for the working directory.
    std::function<string(const string&)> parentOf = [&](const string& d) -> string
    {
        if (d.empty())
            return up;
        const string t = d.substr(0, d.size() - 1);
        const size_t pos = t.find_last_of(sep);
        const string rest = pos == string::npos ? string() : t.substr(0, pos + 1);
        const string last = pos == string::npos ? t : t.substr(pos + 1);
        if (last == ".")
            return parentOf(rest);
        if (last == "..")
            return d + up;
        return rest;
    };
    const auto toPath = [](const string& d)
    {
        const string s = d.empty() ? string(".") : d;
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
    };
    // A folder that holds something: an empty STREAM folder elsewhere is not the disc's
    const auto isFolder = [&](const string& d)
    {
        if (discImage::split(d, nullptr, nullptr))
        {
            std::vector<discImage::Entry> entries;
            return discImage::list(d, &entries) && !entries.empty();
        }
        std::error_code ec;
        return std::filesystem::is_directory(toPath(d), ec) && !std::filesystem::is_empty(toPath(d), ec) && !ec;
    };
    const auto nameOf = [&](const string& d)
    {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(toPath(d), ec).lexically_normal();
        if (!p.has_filename())
            p = p.parent_path();
        const std::u8string u = p.filename().u8string();
        return strToUpperCase(string(u.begin(), u.end()));
    };
    const string bdmv = parentOf(toNativeSeparators(extractFileDir(mplsFileName)));
    const string stream = bdmv + "STREAM" + sep;
    if (isFolder(stream))
        return stream;
    if (nameOf(bdmv) == "BACKUP")
        return parentOf(bdmv) + "STREAM" + sep;
    return stream;
}

std::string METADemuxer::mplsTrackToFullName(const std::string& mplsFileName, const std::string& mplsNum)
{
    const string path = playlistStreamDir(mplsFileName);

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string m2tsExt = (mplsExt == "mpls") ? "m2ts" : "mts";

    return clipNameAsOnDisc(path + mplsNum + string("."), m2tsExt);
}

// A camera's card names its clips in capitals, 00000.MTS beside 00000.MPL, and the listing named them so, but the mux
// opened 00000.mts, which a file system that tells case apart (Linux, macOS) does not have. The name is taken in the
// case that exists, lower case first; lower case when neither does, for the message that it cannot be opened.
std::string METADemuxer::clipNameAsOnDisc(const std::string& stem, const std::string& lowerExt)
{
    const string lower = stem + lowerExt;
    if (!fileExists(lower))
    {
        const string upper = stem + strToUpperCase(lowerExt);
        if (fileExists(upper))
            return upper;
    }
    return lower;
}

std::string METADemuxer::mplsTrackToSSIFName(const std::string& mplsFileName, const std::string& mplsNum)
{
    const string path = playlistStreamDir(mplsFileName) + string("SSIF") + getDirSeparator();

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string ssifExt = mplsExt == "mpls" ? "ssif" : "sif";

    return clipNameAsOnDisc(path + mplsNum + string("."), ssifExt);
}

int METADemuxer::addPGSubStream(const string& codec, const string& _codecStreamName,
                                const map<string, string>& addParams, const MPLSStreamInfo* subStream)
{
    map<string, string> params = addParams;
    params["track"] = int32ToStr(subStream->streamPID);
    if (subStream->type == 2)
        params["subClip"] = "1";
    // An eye of a stereo subtitle or an aligned subtitle is part of the track the line names, not a track of its own:
    // it is not the default track the line may make that one (the disc start selected the left eye, being the first
    // subtitle muxed), and its language is that track's (the caller puts it in lang=). The playlist declares none
    // for it, and it was muxed with none.
    params.erase("default");
    params["srcpgpart"] = "1";
    return addStream(codec, _codecStreamName, params);
}

std::vector<MPLSPlayItem> METADemuxer::mergePlayItems(const std::vector<MPLSParser>& mplsInfoList)
{
    std::vector<MPLSPlayItem> result;
    for (auto& i : mplsInfoList)
    {
        for (auto& j : i.m_playItems) result.push_back(j);
    }
    return result;
}

static double parseFpsValue(const std::string& text);
static void warnSubtitleCanvas(const std::string& what, uint16_t width, uint16_t height, double fps);

// level= as the codec counts it (H.264 in tenths, HEVC in thirtieths), when it is one of the codec's levels; anything
// else is refused with the levels there are.
static int checkedLevel(const std::string& text, const char* codec, const int scale, std::initializer_list<int> levels)
{
    const std::string t = trimStr(text);
    const bool number = !t.empty() && t.find_first_not_of("0123456789.") == std::string::npos &&
                        std::count(t.begin(), t.end(), '.') <= 1 && t.find_first_of("0123456789") != std::string::npos;
    // The value must BE a level: it was rounded first, so 4.15 was taken as 4.2 and 3.95 as 4.0 without a word
    const double scaled = number ? strToDouble(t.c_str()) * scale : -1.0;
    const long idc = number && std::fabs(scaled - std::round(scaled)) < 1e-6 ? lround(scaled) : -1;
    if (std::find(levels.begin(), levels.end(), idc) == levels.end())
    {
        std::string list;
        for (const int l : levels)
            list += (list.empty() ? "" : ", ") +
                    (l % scale ? doubleToStr(static_cast<double>(l) / scale, 1) : std::to_string(l / scale));
        THROW(ERR_COMMON, "level=" << text << " is not a " << codec << " level. The levels are " << list << ".")
    }
    return static_cast<int>(idc);
}

VideoAspectRatio arNameToCode(const string& arName);  // below, with createCodec

int METADemuxer::addStream(const string& codecName, const string& codecStreamName, const map<string, string>& addParams)
{
    // the reader's codec: the one asked for, or PGS for an ASS or SSA track drawn with libass
    string codec = codecName;
    int32_t pid = 0;
    auto tmpitr = addParams.find("track");
    if (tmpitr != addParams.end())
        pid = strToInt32(tmpitr->second.c_str());

    // A track has two parts, 1 and 2. subTrack=0 or 3 was taken as asked, the track then gave nothing, and the job was
    // refused as if the codec on the line were wrong. Without track= the line names an elementary stream, which is
    // read whole: subTrack= on it was dropped without a word, where the same stream in an MP4 file is refused.
    tmpitr = addParams.find("subTrack");
    if (tmpitr != addParams.end())
    {
        const std::string part = trimStr(tmpitr->second);
        if (part != "1" && part != "2")
            THROW(ERR_COMMON, "subTrack=" << tmpitr->second << " on the " << codec << " line of "
                                          << unquoteStr(codecStreamName)
                                          << " is not a part of a track: subTrack is 1 or 2. For a dual layer Dolby "
                                             "Vision track 1 is the base layer and 2 the enhancement layer; for a "
                                             "combined AVC/MVC track 1 is the MVC part and 2 the AVC part.")
        if (pid == 0)
            THROW(ERR_COMMON, "subTrack= on the " << codec << " line of " << unquoteStr(codecStreamName)
                                                  << " picks a part of the track that track= names in a container "
                                                     "file (MKV, MP4, TS), and this line names no track. A stream "
                                                     "file of its own is read whole: leave subTrack= out, or add "
                                                     "track= for a container.")
        pid = SubTrackFilter::pidToSubPid(pid, strToInt32(part));
    }

    vector<string> fileList;
    string unquotedStreamName = unquoteStr(codecStreamName);
    string fileExt = strToLowerCase(extractFileExt(codecStreamName));
    std::vector<MPLSParser> mplsInfoList;
    std::vector<ClipPart> clipParts;  // the part of each clip that is muxed (cutPlayItems); empty for whole clips
    bool anyPart = false;

    // filds for SS PG
    int leftEyeSubStreamIdx = -1;
    int rightEyeSubStreamIdx = -1;
    bool isSSPG = false;
    int ssPGOffset = 0xff;
    int alignedSubStreamIdx[2] = {-1, -1};
    uint8_t alignedOffset[2] = {0xff, 0xff};
    ////

    bool isSubStream = (codec == h264DepCodecInfo.programName) || addParams.find("subClip") != addParams.end();
    if (fileExt == "mpls" || fileExt == "mpl")
    {
        mplsInfoList = getMplsInfo(codecStreamName);
        vector<string> mplsNames = splitQuotedStr(codecStreamName.c_str(), '+');
        for (size_t k = 0; k < mplsInfoList.size(); ++k)
        {
            MPLSParser& mplsInfo = mplsInfoList[k];
            unquotedStreamName = unquoteStr(mplsNames[k]);
            for (size_t i = 0; i < mplsInfo.m_playItems.size(); ++i)
            {
                string playItemName;
                if (isSubStream)
                {
                    if (mplsInfo.m_mvcFiles.empty())
                    {
                        THROW(ERR_INVALID_CODEC_FORMAT,
                              "Current playlist file doesn't has MVC track info. Please, remove MVC track from the "
                              "track list")
                    }
                    // An empty entry is a play item that no sub play item of the 3D sub path names.
                    if (mplsInfo.m_mvcFiles.size() <= i || mplsInfo.m_mvcFiles[i].empty())
                        THROW(ERR_INVALID_CODEC_FORMAT,
                              "Bad playlist file: number of CLPI files for AVC and VMC parts do not match")
                    playItemName = mplsInfo.m_mvcFiles[i];
                }
                else
                {
                    playItemName = mplsInfo.m_playItems[i].fileName;
                }
                string fileName = mplsTrackToFullName(unquotedStreamName, playItemName);
                if (mplsInfo.isDependStreamExist && !fileExists(fileName))
                    fileName = mplsTrackToSSIFName(unquotedStreamName, mplsInfo.m_playItems[i].fileName);
                fileList.push_back(fileName);
                clipParts.push_back(isSubStream ? ClipPart() : mplsInfo.m_playItems[i].part);
                anyPart = anyPart || clipParts.back().isCut();
            }

            MPLSStreamInfo streamInfo = mplsInfo.getStreamByPID(pid);
            // The playlist places its picture-in-picture video itself, with moves and a size of its own over time
            // (ExtensionData 1/1). That is not read: a disc places it by the pip options, and the playlist's own
            // placing was left out without a word.
            if (!m_quiet && (V3_flags & BLURAY_OUT) && !codec.empty() && codec[0] == 'V' &&
                addParams.find("secondary") != addParams.end() && mplsInfo.hasPipMetadata &&
                m_pipPlacingReported.insert(mplsNames[k]).second)
                LTRACE(LT_WARN, 2,
                       "Warning: the picture-in-picture video (track "
                           << pid << ") of playlist " << extractFileName2(unquotedStreamName)
                           << " is placed by the pip options (pipCorner, pipHOffset, pipVOffset, pipScale, pipLumma, "
                              "or their defaults). The position, size, moves and transparency the playlist gives it "
                              "are left out. The rest is muxed.");
            std::map<string, string> partParams = addParams;
            const std::string partLang = applyLangCodeStyle(streamInfo.language_code);
            if (partParams.find("lang") == partParams.end() && isLanguageCode(partLang) && partLang != "und")
                partParams["lang"] = partLang;
            if (streamInfo.stream_coding_type == StreamType::SUB_PGS && streamInfo.isSSPG)
            {
                // add refs to addition tracks for stereo subtitles
                leftEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, partParams, streamInfo.leftEye);
                rightEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, partParams, streamInfo.rightEye);

                isSSPG = streamInfo.isSSPG;
                ssPGOffset = streamInfo.SS_PG_offset_sequence_id;
            }
            // The aligned subtitles of the track (3D table), muxed with it as the eyes of a stereo one are. They were
            // offered as unrelated subtitles of the clip and written as ordinary ones without a language.
            for (int a = 0; a < 2 && streamInfo.stream_coding_type == StreamType::SUB_PGS; ++a)
            {
                const MPLSStreamInfo::AlignedRef& ref = a == 0 ? streamInfo.topAS : streamInfo.bottomAS;
                if (ref.pid == 0 || alignedSubStreamIdx[a] != -1)
                    continue;
                MPLSStreamInfo aligned;
                aligned.type = ref.type;
                aligned.streamPID = ref.pid;
                alignedSubStreamIdx[a] = addPGSubStream(codec, unquotedStreamName, partParams, &aligned);
                alignedOffset[a] = ref.offsetId;
            }
        }
    }
    else
    {
        fileList = extractFileList(codecStreamName);
    }

    if (m_keepEditions && fileList.size() > 1)
        THROW(ERR_COMMON, "--keep-editions keeps one Matroska file as it is stored, and cannot be used with a join.")

    // A Matroska file is played as a player plays it: by its ordered edition, or with the files linked after it.
    // Not with --keep-editions, which keeps the file as it is stored, editions and all.
    std::vector<JoinRange> timelineRanges;
    if (fileList.size() == 1 && !m_keepEditions)
    {
        const std::string ext = strToLowerCase(extractFileExt(fileList[0]));
        if (ext == "mkv" || ext == "mka" || ext == "mks" || ext == "mk3d" || ext == "webm")
        {
            std::string how;
            const std::vector<MatroskaPlayPart> parts = MatroskaDemuxer::playTimeline(fileList[0], how);
            if (!parts.empty())
            {
                if (m_timelinesReported.insert(fileList[0]).second)
                    LTRACE(LT_INFO, 2, "Reading " << fileList[0] << " as a player plays it: " << how << ".");
                fileList.clear();
                for (const MatroskaPlayPart& part : parts)
                {
                    fileList.push_back(part.file);
                    timelineRanges.push_back(part.range);
                }
            }
        }
    }

    // ** THE KEY IS THE FILE AND THE TRACK, NOT WHERE THE LINE NAMES IT OR HOW. ** It held the file's place in the line
    // and the name as written, so a file the second line names at another place (p2.ts alone, then "p1.ts"+"p2.ts"),
    // or another way (./p1.ts), was muxed a second time, and the same track came out twice. A file named twice in ONE
    // line, a join of a file with itself, is checked only against the lines before it.
    const auto fileKey = [](const string& name)
    {
        std::error_code ec;
        const std::filesystem::path p(std::u8string(name.begin(), name.end()));
        std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
        if (ec)
            c = p;
        const std::u8string u = c.generic_u8string();
        const string key(u.begin(), u.end());
#ifdef _WIN32
        return strToLowerCase(key);
#else
        return key;
#endif
    };
    const string trackPart =
        string(addParams.find("subClip") == addParams.end() ? "_track_" : "_subtrack_") + int32ToStr(pid);
    string lineKey;  // every file of this line, for telling a line that repeats an earlier one
    std::vector<string> keys;
    for (const auto& name : fileList)
    {
        keys.push_back(fileKey(name) + trackPart);
        lineKey += keys.back() + "|";
    }
    // A track of a file named by an earlier line with subClip= and here without it, or the other way round, is the
    // same track: two readers read it, and closing the first freed what the second still used. It is refused.
    const string otherTrackPart =
        string(addParams.find("subClip") == addParams.end() ? "_subtrack_" : "_track_") + int32ToStr(pid);
    for (const auto& name : fileList)
    {
        if (m_processedTracks.count(fileKey(name) + otherTrackPart) == 0)
            continue;
        std::ostringstream which;
        which << codec << " from " << unquotedStreamName;
        if (pid)
            which << ", track " << pid;
        THROW(ERR_COMMON,
              which.str() << " is muxed by an earlier line of the meta file "
                          << (addParams.find("subClip") == addParams.end() ? "as the second view of 3D video (subClip=)"
                                                                           : "without subClip=")
                          << ", and the same track of a file can be muxed only once.")
    }
    std::vector<string> taken;  // files of this line whose track an earlier line already reads
    for (int i = static_cast<int>(fileList.size()) - 1; i >= 0; --i)
    {
        if (m_processedTracks.find(keys[i]) != m_processedTracks.end())
        {
            taken.insert(taken.begin(), fileList[i]);
            fileList.erase(fileList.begin() + i);
            if (static_cast<size_t>(i) < timelineRanges.size())
                timelineRanges.erase(timelineRanges.begin() + i);
            if (static_cast<size_t>(i) < clipParts.size())
                clipParts.erase(clipParts.begin() + i);
        }
    }
    for (const auto& key : keys) m_processedTracks.insert(key);
    // A line that names only files an earlier line already muxes for this track, but not the same files as any
    // earlier line, would lose its part as well: the first file of a join alone after the join gave nothing. It is
    // refused like the line above. A line that repeats an earlier one exactly is still left out with a warning.
    const bool repeatsALine = m_processedLines.count(lineKey) != 0;
    m_processedLines.insert(lineKey);
    if (!taken.empty() && fileList.empty() && !repeatsALine)
    {
        std::ostringstream which;
        which << codec << " from " << unquotedStreamName;
        if (pid)
            which << ", track " << pid;
        THROW(ERR_COMMON, which.str() << " uses " << taken.front() << (taken.size() > 1 ? " and other files" : "")
                                      << " for a track an earlier line of the meta file already muxes, and the same "
                                         "track of a file can be muxed only once. Give each file to one line only.")
    }
    // A track of a file is read once, so a line that names some of the files of an earlier line for the same track
    // cannot have them. They were left out without a word, and the track started where the first remaining file does:
    // a join of two files of 10 s, after a line with the first one, gave the second file only, 10 s late.
    if (!taken.empty() && !fileList.empty())
    {
        std::ostringstream which;
        which << codec << " from " << unquotedStreamName;
        if (pid)
            which << ", track " << pid;
        THROW(ERR_COMMON, which.str() << " uses " << taken.front() << (taken.size() > 1 ? " and other files" : "")
                                      << " for a track an earlier line of the meta file already muxes, and the same "
                                         "track of a file can be muxed only once. The line would lose that part. "
                                         "Give each file to one line only.")
    }
    if (fileList.empty())
    {
        // Every file this line names has already been taken by an earlier line asking for the
        // same track number, so this line adds nothing. It used to add nothing SILENTLY: a meta
        // naming the same subtitle file twice produced one track and said so nowhere, and the
        // second line simply was not there in the output.
        std::ostringstream which;
        which << codec << " from " << unquotedStreamName;
        if (pid)
            which << ", track " << pid;
        LTRACE(LT_WARN, 2,
               "Warning: " << which.str()
                           << " is already muxed by an earlier line of the meta file, so this line is ignored. "
                              "The same track cannot be muxed twice.");
        return -1;
    }

    FileListIterator* listIterator = nullptr;
    if (fileList.size() > 1 || !timelineRanges.empty() || anyPart)
    {
        listIterator = new FileListIterator();
        m_iterators.push_back(listIterator);
        for (size_t i = 0; i < fileList.size(); ++i)
            if (i < timelineRanges.size())
                listIterator->addFile(fileList[i], timelineRanges[i]);
            else if (anyPart && i < clipParts.size())
                listIterator->addFile(fileList[i], clipParts[i]);
            else
                listIterator->addFile(fileList[i]);
    }

    int64_t fileSize = 0;

    if (m_containerReader.m_demuxers.find(fileList[0]) == m_containerReader.m_demuxers.end())
    {
        File tmpFile;
        for (size_t i = 0; i < fileList.size(); ++i)
        {
            const string& fileName = fileList[i];
            if (const std::string hint = discSourceHint(fileName); !hint.empty())
                THROW(ERR_INVALID_CODEC_FORMAT, "Can't use " << fileName << " as a source: " << hint << ".")
            if (!tmpFile.open(fileName.c_str(), File::ofRead))
                THROW(ERR_INVALID_CODEC_FORMAT, "Can't open file: " << fileName << openFailureReason(fileName))
            int64_t tmpSize = 0;
            tmpFile.size(&tmpSize);
            // of a clip cut to a part, the bytes that are read
            if (anyPart && i < clipParts.size() && clipParts[i].isCut())
                tmpSize = (clipParts[i].endByte >= 0 ? (std::min)(clipParts[i].endByte, tmpSize) : tmpSize) -
                          (std::min)(clipParts[i].firstByte, tmpSize);
            fileSize += tmpSize;
            tmpFile.close();
        }
    }

    // delPulldown promises that the frame rate goes from 30 to 24 when pulldown is present.
    // Only the window kept that promise, by writing fps=24000/1001 into the meta itself. A meta
    // written by hand, or the command line, cleared the repeat flags and kept 29.97, so film played
    // a quarter too fast. Ask the stream, as the listing does, and supply the rate when it is
    // pulldown and nobody chose one. The same holds for H.264 and VC-1, whose readers remove the
    // pulldown as well: 144 pictures of H.264 film played in 4.8 s instead of 6.0.
    // A video track of a transport stream named on the line as another video codec than its data is was read as that
    // codec: the MPEG-2 video of a disc clip named V_MPEG4/ISO/AVC came out as the odd pictures the H.264 reader finds
    // in MPEG-2 data, and the mux said it was successful. Where the stream type the PMT gives is not the codec of the
    // line, the track is detected as the listing detects it, by its data (a PMT can name the wrong codec, and the
    // listing and the window then give the right one), and a line naming another video codec stops here.
    const auto videoCodecId = [](const std::string& name)
    {
        return name == "V_MPEG-2"                                       ? CODEC_V_MPEG2
               : name == "V_MPEG4/ISO/AVC" || name == "V_MPEG4/ISO/MVC" ? CODEC_V_MPEG4_H264
               : name == "V_MPEGH/ISO/HEVC"                             ? CODEC_V_MPEG4_H265
               : name == "V_MPEGI/ISO/VVC"                              ? CODEC_V_MPEG4_H266
               : name == "V_MS/VFW/WVC1"                                ? CODEC_V_VC1
                                                                        : 0;
    };
    const auto isVideoCodecId = [](const int id)
    {
        return id == CODEC_V_MPEG2 || id == CODEC_V_MPEG4_H264 || id == CODEC_V_MPEG4_H265 ||
               id == CODEC_V_MPEG4_H266 || id == CODEC_V_VC1;
    };
    if (pid > 0 && !SubTrackFilter::isSubTrack(pid) && fileList.size() == 1)
    {
        const std::string ext = strToUpperCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        const int named = videoCodecId(codec);
        int pmtCodec = 0;
        if (named != 0 && (ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T"))
        {
            TSDemuxer pmt(m_readManager, "");
            pmt.openFile(unquoteStr(trimStr(fileList[0])));
            std::map<int32_t, TrackInfo> tracks;
            pmt.getTrackList(tracks);
            if (const auto t = tracks.find(pid); t != tracks.end())
                switch (static_cast<StreamType>(t->second.m_trackType))
                {
                case StreamType::VIDEO_MPEG2:
                    pmtCodec = CODEC_V_MPEG2;
                    break;
                case StreamType::VIDEO_H264:
                case StreamType::VIDEO_MVC:
                    pmtCodec = CODEC_V_MPEG4_H264;
                    break;
                case StreamType::VIDEO_H265:
                    pmtCodec = CODEC_V_MPEG4_H265;
                    break;
                case StreamType::VIDEO_H266:
                    pmtCodec = CODEC_V_MPEG4_H266;
                    break;
                case StreamType::VIDEO_VC1:
                    pmtCodec = CODEC_V_VC1;
                    break;
                default:
                    pmtCodec = -1;  // another stream type: the data says what it is
                }
        }
        if (named != 0 && pmtCodec != 0 && pmtCodec != named)
        {
            const DetectStreamRez probe = DetectStreamReader(m_readManager, unquoteStr(trimStr(fileList[0])), false);
            for (const CheckStreamRez& s : probe.streams)
            {
                if (s.trackID != pid)
                    continue;
                const int found =
                    s.codecInfo.codecID == CODEC_V_MPEG4_H264_DEP ? CODEC_V_MPEG4_H264 : s.codecInfo.codecID;
                if (found != named && isVideoCodecId(found))
                    THROW(ERR_COMMON, "Track " << pid << " of " << unquoteStr(fileList[0]) << " is "
                                               << s.codecInfo.programName << ", but the line names " << codec
                                               << ". Name the track as the listing gives it. Nothing was written.")
                break;
            }
        }
    }

    // ** EVERY FILE OF A JOIN HOLDS THE CODEC OF ITS LINE. ** A join of elementary files is one stream to the reader,
    // which read the files after the first as the codec of the line too: an HEVC file joined to an H.264 one on an
    // H.264 line gave pictures of HEVC data, and "Mux successful complete", as the pictures of the first file were
    // enough for every check of the track. Joined the other way, the H.264 file was refused as multi-layer HEVC. Each
    // file of such a join is detected as the listing detects it, and a file of another video codec stops here.
    // The probe says nothing: what its reader has to say about the stream, the mux says again, and a file it cannot
    // read is left to the rules that follow.
    if (const int named = videoCodecId(codec); pid == 0 && named != 0 && fileList.size() > 1)
        for (size_t i = 0; i < fileList.size(); ++i)
        {
            const std::string file = unquoteStr(trimStr(fileList[i]));
            struct Silence
            {
                struct NullBuffer final : std::streambuf
                {
                    int overflow(const int c) override { return traits_type::not_eof(c); }
                } null;
                std::streambuf* out = std::cout.rdbuf(&null);
                std::streambuf* err = std::cerr.rdbuf(&null);
                const bool lastMsg = sLastMsg;
                Silence() = default;
                Silence(const Silence&) = delete;
                Silence& operator=(const Silence&) = delete;
                ~Silence()
                {
                    std::cout.rdbuf(out);
                    std::cerr.rdbuf(err);
                    sLastMsg = lastMsg;
                }
            };
            DetectStreamRez probe;
            try
            {
                Silence silence;
                probe = DetectStreamReader(m_readManager, file, false);
            }
            catch (...)
            {
                continue;
            }
            if (probe.streams.size() != 1)
                continue;  // a container: its tracks are named by track=
            const CodecInfo& found = probe.streams[0].codecInfo;
            const int foundId = found.codecID == CODEC_V_MPEG4_H264_DEP ? CODEC_V_MPEG4_H264 : found.codecID;
            if (foundId != named && isVideoCodecId(foundId))
                THROW(ERR_COMMON, "File " << i + 1 << " of the join on the " << codec << " line, " << file << ", is "
                                          << found.displayName << ", not the codec of the line. Every file of a "
                                          << "join has to hold the codec of its line. Nothing was written.")
        }

    map<string, string> readerParams = addParams;
    bool fpsFromPulldown = false;
    const int pulldownCodec = codec == "V_MPEG-2"          ? CODEC_V_MPEG2
                              : codec == "V_MPEG4/ISO/AVC" ? CODEC_V_MPEG4_H264
                              : codec == "V_MS/VFW/WVC1"   ? CODEC_V_VC1
                                                           : 0;
    if (pulldownCodec != 0 && addParams.count("delPulldown") && !addParams.count("fps"))
    {
        const DetectStreamRez probe = DetectStreamReader(m_readManager, fileList[0], false);
        for (const CheckStreamRez& s : probe.streams)
        {
            if (s.codecInfo.codecID != pulldownCodec || (pid && s.trackID != pid))
                continue;
            const size_t at = s.streamDescr.find("Frame rate: ");
            if (s.streamDescr.find("(pulldown)") == string::npos || at == string::npos)
                break;
            const double streamFps = strToDouble(s.streamDescr.substr(at + 12).c_str());
            const char* filmFps = fabs(streamFps - 29.97) < 0.01  ? "24000/1001"
                                  : fabs(streamFps - 30.0) < 0.01 ? "24"
                                                                  : nullptr;
            // H.264 says how long each picture is shown: the rate follows from that, at 59.94 and 60 as well (720p
            // film, its pictures doubled and tripled). A cadence that gives no standard rate keeps the rule above.
            double nearest = 0.003;  // of the rate: a probe of a few dozen pictures may end inside a cadence
            if (s.filmFps > 0.0)
                for (const auto& [rate, name] : {std::pair<double, const char*>{24000.0 / 1001, "24000/1001"},
                                                 {24.0, "24"},
                                                 {25.0, "25"},
                                                 {30000.0 / 1001, "30000/1001"},
                                                 {30.0, "30"}})
                    if (fabs(s.filmFps - rate) / rate < nearest)
                    {
                        nearest = fabs(s.filmFps - rate) / rate;
                        filmFps = name;
                    }
            if (filmFps)
            {
                readerParams["fps"] = filmFps;
                fpsFromPulldown = true;
                LTRACE(LT_INFO, 2,
                       "delPulldown: the " << s.codecInfo.displayName << " stream carries pulldown at " << streamFps
                                           << " fps, so it is muxed at " << filmFps << " fps.");
            }
            break;
        }
    }

    // ** A MATROSKA OUTPUT KEEPS A SUBTITLE IN THE FORM ITS MATROSKA SOURCE HAS IT. ** The listing gives ASS, SSA and
    // WebVTT as SRT and VobSub and DVB as PGS, because every other output needs them that way, and the meta line says
    // so. Into a Matroska output they used to go converted as well: ASS lost its styles, colours and positions,
    // WebVTT its cue settings, and VobSub and DVB were drawn again as PGS. Copied, every block stays as it was, with
    // its CodecPrivate, its BlockAdditions and its times. Text is copied only when it is kept as text: without
    // keep-text it is drawn into pictures, as asked. WebM holds subtitles only as WebVTT in its own form.
    AbstractStreamReader* codecReader = nullptr;
    // A demux does the same for the text formats kept as text: they are written as .ass, .ssa or .vtt files instead
    // of SRT, which lost the styles and positions of ASS and the cue settings of WebVTT.
    if ((m_matroskaOutput || m_demuxOutput) && pid > 0 &&
        ((m_matroskaOutput && codec == "S_HDMV/PGS") || (codec == "S_TEXT/UTF8" && addParams.count("keep-text"))))
    {
        const std::string srcExt = strToUpperCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        if (srcExt == "MKV" || srcExt == "MKA" || srcExt == "MKS" || srcExt == "MK3D" || srcExt == "WEBM")
        {
            MatroskaDemuxer probe(m_readManager);
            probe.openFile(fileList[0]);
            MatroskaDemuxer::CopySource src;
            if (probe.getCopySource(pid, src))
            {
                const std::string& id = src.codecId;
                const bool text =
                    id == "S_TEXT/ASS" || id == "S_TEXT/SSA" || id == "S_TEXT/WEBVTT" || isWebmVttText(id);
                const bool bitmap = id == "S_VOBSUB" || id == "S_DVBSUB";
                const bool copy = !m_matroskaOutput ? text && codec == "S_TEXT/UTF8"
                                  : m_matroskaOutput == 2
                                      ? isWebmVttText(id)
                                      : (text && codec == "S_TEXT/UTF8") || (bitmap && codec == "S_HDMV/PGS");
                if (copy)
                {
                    LTRACE(LT_INFO, 2,
                           "Track " << pid << " of " << unquoteStr(fileList[0]) << " is " << id
                                    << (m_matroskaOutput ? ", and is copied into the output as it is."
                                                         : ", and is written in that form."));
                    auto* copyReader = new MatroskaCopyStreamReader(id, std::move(src.codecPrivate),
                                                                    std::move(src.addMappings), src.maxBlockAddId);
                    copyReader->setTextFileOutput(!m_matroskaOutput);
                    codecReader = copyReader;
                }
            }
        }
    }
    // ** ASS AND SSA ARE DRAWN WITH LIBASS. ** Drawn as SRT text, a track lost its styles, colours, positions and
    // animations, and its own fonts. libass draws it as the script says, with the fonts attached to the file, and its
    // pictures become PGS as DVD and DVB subtitles do. Without libass, or with ass-as-srt, it is drawn as before.
    if (codecReader == nullptr && codec == "S_TEXT/UTF8" && pid > 0 && !addParams.count("keep-text") &&
        !addParams.count("ass-as-srt") && !SubTrackFilter::isSubTrack(pid))
    {
        const std::string srcExt = strToUpperCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        // A Matroska file of any name: an ASS track in a .mka was drawn as plain text without a word
        if (srcExt == "MKV" || srcExt == "MKA" || srcExt == "MKS" || srcExt == "MK3D")
        {
            MatroskaDemuxer probe(m_readManager);
            probe.openFile(fileList[0]);
            MatroskaDemuxer::CopySource src;
            if (probe.getCopySource(pid, src) && (src.codecId == "S_TEXT/ASS" || src.codecId == "S_TEXT/SSA"))
            {
                ass_render::Drawing drawing;
                if (const auto it = readerParams.find("video-width"); it != readerParams.end())
                    drawing.width = strToInt32(it->second.c_str());
                if (const auto it = readerParams.find("video-height"); it != readerParams.end())
                    drawing.height = strToInt32(it->second.c_str());
                if (const auto it = readerParams.find("fps"); it != readerParams.end())
                    drawing.fps = parseFpsValue(it->second);
                if (const auto it = readerParams.find("fonts-dir"); it != readerParams.end())
                    drawing.fontsDir = unquoteStr(it->second);
                // drawn at the standard size a subtitle of that video has, as an SRT track is: 704x576 is drawn at
                // 720x576, which a disc carries
                const int videoWidth = drawing.width;
                const int videoHeight = drawing.height;
                if (drawing.width > 0 && drawing.height > 0 && drawing.fps > 0)
                {
                    uint16_t w = 0, h = 0;
                    text_subtitles::TextToPGSConverter::enlargeCrop(static_cast<uint16_t>(drawing.width),
                                                                    static_cast<uint16_t>(drawing.height), drawing.fps,
                                                                    &w, &h);
                    drawing.width = w;
                    drawing.height = h;
                }
                std::string why;
                if (drawing.width <= 0 || drawing.height <= 0 || drawing.fps <= 0)
                    ;  // the SRT reader says what is missing
                else if (!ass_render::available(&why))
                    LTRACE(LT_WARN, 2,
                           "Warning: track " << pid << " of " << unquoteStr(fileList[0]) << " is " << src.codecId
                                             << " and is drawn as plain text, without its styles: " << why
                                             << ". libass draws it as the script says (see the documentation).");
                else
                {
                    if (const auto it = readerParams.find("save-srt"); it != readerParams.end())
                    {
                        drawing.saveSrt = unquoteStr(it->second);
                        if (drawing.saveSrt.empty())
                            THROW(ERR_COMMON,
                                  "save-srt needs the name of the SRT file to write, as save-srt=\"name.srt\"")
                        drawing.srtFile = std::make_shared<ass_render::SaveSrtFile>(drawing.saveSrt);
                    }
                    LTRACE(LT_INFO, 2,
                           "Track " << pid << " of " << unquoteStr(fileList[0]) << " is " << src.codecId
                                    << ", and is drawn with libass.");
                    if (videoWidth <= 65535 && videoHeight <= 65535)
                        warnSubtitleCanvas(
                            "the subtitles of track " + std::to_string(pid) + " of " + unquoteStr(fileList[0]),
                            static_cast<uint16_t>(videoWidth), static_cast<uint16_t>(videoHeight), drawing.fps);
                    // A fonts-dir that is not there, as a mistyped name, was passed over without a word
                    if (std::error_code ec; !drawing.fontsDir.empty() &&
                                            !std::filesystem::is_directory(
                                                std::u8string(drawing.fontsDir.begin(), drawing.fontsDir.end()), ec))
                        LTRACE(LT_WARN, 2,
                               "Warning: fonts-dir names " << drawing.fontsDir
                                                           << ", which is not a folder. The track is drawn with the "
                                                              "fonts attached to the file and those of the system.");
                    codec = "S_HDMV/PGS";
                    m_containerReader.m_assDrawing[{fileList[0], pid}] = drawing;
                    m_shiftedSubReader.m_assDrawing[{fileList[0], pid}] = drawing;
                }
            }
        }
    }
    if (codecReader == nullptr)
        codecReader = createCodec(codec, readerParams, fileList[0], mergePlayItems(mplsInfoList), m_quiet);
    // The reader belongs to the stream once it is added (m_codecInfo). A line refused before that, as a track of a
    // container file without track=, left it behind.
    std::unique_ptr<AbstractStreamReader> readerGuard(codecReader);
    // ar= on H.264, HEVC, VVC and VC-1, which was read for MPEG-2 only (createCodec) and ignored for the others
    // without a word: a Matroska output takes it as the display size, which a player shows over the stream's own
    // shape. MPEG-2 has the shape in its sequence header, which is rewritten; the other codecs have it in headers
    // that are not, so any other output keeps the stream's own shape, and says so.
    if (const auto mpegVideo = dynamic_cast<MPEGStreamReader*>(codecReader);
        mpegVideo && codec != "V_MPEG-2" && addParams.find("ar") != addParams.end())
    {
        const VideoAspectRatio ar = arNameToCode(addParams.find("ar")->second);  // a value it does not know is refused
        if (m_matroskaOutput)
            mpegVideo->setAspectRatio(ar);
        else
            LTRACE(LT_WARN, 2,
                   "Warning: ar= on the line of " << codec
                                                  << " is applied to a Matroska output only. This stream gives its "
                                                     "shape in its own headers, which are kept as they are, so this "
                                                     "output keeps the stream's shape.");
    }
    if (fpsFromPulldown)
        if (auto* mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader))
            mpegReader->setFpsAnnounced();
    // A playlist whose clips are cut to the parts played: the video reader asks where a part begins
    if (anyPart)
        if (auto* mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader))
            mpegReader->setPartStartSource(
                [this, file = fileList[0], pid](const int64_t picture)
                {
                    const auto it = m_containerReader.m_demuxers.find(file);
                    return it != m_containerReader.m_demuxers.end() && it->second.m_demuxer &&
                           it->second.m_demuxer->isPartStartPicture(pid, picture);
                });
    codecReader->setStreamIndex(static_cast<int>(m_codecInfo.size() + 1));
    codecReader->setStreamName(fileList[0]);
    codecReader->setTimeOffset(m_timeOffset);

    if (m_codecInfo.empty())
        codecReader->m_flags |= AVPacket::PCR_STREAM;

    AbstractReader* dataReader;
    string tmpname = strToLowerCase(fileList[0]);
    tmpname = unquoteStr(trimStr(tmpname));

    checkNameMatchesContent(unquoteStr(trimStr(fileList[0])), strToLowerCase(extractFileExt(tmpname)));

    // ** A SUBTITLE TRACK WITH A NEGATIVE TIMESHIFT IS READ BY A DEMUXER OF ITS OWN. ** Its packets are due that much
    // before their place in the file, so the file was read ahead for them, holding the video and the sound read on the
    // way in memory, which stops at a quarter of the buffer limit: at 135 Mbit/s that is about 4 s, and with
    // timeshift=-10s the first subtitles came about 5 s after they were due. A demuxer of its own reads the same file
    // for the subtitle track alone and holds nothing else. Only for a single Matroska file or transport stream that
    // another line already reads (whose demuxer gives the title, chapters, attachments and tags of the file).
    int64_t lineShift = 0;
    if (const auto ts = addParams.find("timeshift"); ts != addParams.end())
        parseTimeToTicks(ts->second, "ms", &lineShift);
    const bool ownReader = pid > 0 && !SubTrackFilter::isSubTrack(pid) && lineShift < 0 && !codec.empty() &&
                           codec[0] == 'S' && fileList.size() == 1 && listIterator == nullptr && mplsInfoList.empty() &&
                           m_containerReader.m_demuxers.find(fileList[0]) != m_containerReader.m_demuxers.end() &&
                           (strEndWith(tmpname, ".mkv") || strEndWith(tmpname, ".mks") ||
                            strEndWith(tmpname, ".webm") || strEndWith(tmpname, ".ts") ||
                            strEndWith(tmpname, ".m2ts") || strEndWith(tmpname, ".mts") || strEndWith(tmpname, ".m2t"));

    if (strEndWith(tmpname, ".h264") || strEndWith(tmpname, ".264") || strEndWith(tmpname, ".mvc"))
    {
        if (pid)
        {
            dataReader = &m_containerReader;
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMultiH264);
            if (listIterator)
                dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        }
        else
        {
            dataReader = m_readManager.getReader(fileList[0].c_str());
        }
    }
    // .m2t and .mpeg belong to their families here as they do in familyOfExt and ContainerToReaderWrapper::openStream:
    // this table and the two in discoverStreams and DetectStreamReader left them out, so such a file was read as a
    // bare stream, or not read at all.
    else if (strEndWith(tmpname, ".ts") || strEndWith(tmpname, ".m2ts") || strEndWith(tmpname, ".mts") ||
             strEndWith(tmpname, ".m2t") || strEndWith(tmpname, ".ssif"))
    {
        if (pid)
            dataReader = ownReader ? &m_shiftedSubReader : &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside TS/M2TS container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        if (strEndWith(tmpname, ".ts") || strEndWith(tmpname, ".m2t"))
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctTS);
        else
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctM2TS);
    }
    else if (strEndWith(tmpname, ".vob") || strEndWith(tmpname, ".evo") || strEndWith(tmpname, ".mpg") ||
             strEndWith(tmpname, ".mpeg"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MPG/VOB/EVO container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        if (strEndWith(tmpname, ".evo") || strEndWith(tmpname, ".evo\""))
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctEVOB);
        else
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctVOB);
    }
    else if (strEndWith(tmpname, ".mkv") || strEndWith(tmpname, ".mka") || strEndWith(tmpname, ".mks") ||
             strEndWith(tmpname, ".mk3d") || strEndWith(tmpname, ".webm"))
    {
        if (pid)
            dataReader = ownReader ? &m_shiftedSubReader : &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT,
                  (addParams.count("track") ? std::string("track=0 is not a track: the tracks of a Matroska file are "
                                                          "numbered from 1. List the file to see them.")
                                            : std::string("A track of a Matroska file needs track= on its line, "
                                                          "such as track=2. List the file to see its tracks.")))
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMKV);
    }
    else if (strEndWith(tmpname, ".mov") || strEndWith(tmpname, ".mp4") || strEndWith(tmpname, ".m4v") ||
             strEndWith(tmpname, ".m4a"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MOV/MP4 container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMOV);
    }
    else
    {
        // Opus, Vorbis, ALAC, MPEG-4 Part 2 and ProRes are read from a container only: their readers take the packets
        // a container gives them. A file of its own, as an Ogg Opus file, was read all the same: any four bytes were
        // taken for the length of a packet, and the mux of the noise that made was said to be successful.
        if (codec == "A_OPUS" || codec == "A_VORBIS" || codec == "A_ALAC" || codec == "V_MPEG4/ISO/ASP" ||
            codec == "V_PRORES")
        {
            THROW(ERR_INVALID_CODEC_FORMAT, codec << " is read from a Matroska, WebM, MP4 or MOV file or a transport "
                                                     "stream only, and "
                                                  << unquoteStr(trimStr(fileList[0]))
                                                  << " is none of them. An Ogg file is not read.")
        }
        dataReader = m_readManager.getReader(fileList[0].c_str());
    }
    if (dataReader == nullptr)
        THROW(ERR_INVALID_CODEC_FORMAT, "This version do not support multicast or other network steams for muxing")

    m_codecInfo.emplace_back(dataReader, codecReader, fileList[0], codecStreamName, pid, isSubStream);
    readerGuard.release();
    m_codecInfo.back().m_sourceFiles = fileList;
    if (listIterator)
    {
        auto fileReader = dynamic_cast<BufferedFileReader*>(dataReader);
        if (fileReader)
            fileReader->setFileIterator(listIterator, m_codecInfo.rbegin()->m_readerID);
    }

    StreamInfo& streamInfo = *m_codecInfo.rbegin();
    // The track's own UID in its source, for the tags that source has about it. Its header has been read by now, and
    // asking reads nothing else. A sub-track is half of a track the output writes as one, or two of: it gets none.
    if ((dataReader == &m_containerReader || dataReader == &m_shiftedSubReader) && pid > 0 &&
        !SubTrackFilter::isSubTrack(pid))
        if (const auto it = m_containerReader.m_demuxers.find(fileList[0]);
            it != m_containerReader.m_demuxers.end() && it->second.m_demuxer)
            streamInfo.m_srcTrackUid = it->second.m_demuxer->getTrackUid(pid);
    // A video or audio track of a Matroska output keeps the BlockAdditions of its frames: asked for before anything of
    // the file is read, so that none is missed and none is kept for a track nobody muxes
    // Of a join, the header of each file is asked in turn until one declares them: a join of a VP9 file without alpha
    // and then one with alpha lost the alpha of the second, its BlockAdditions and its AlphaMode, as the track's header
    // is written from the first file. The track gets the declaration and the AlphaMode of the first file that has them;
    // the blocks of a file without them have none, as RFC 9559 allows (AlphaMode, BlockAdditions).
    std::string joinAlphaMode;
    if (m_matroskaOutput && dataReader == &m_containerReader && pid > 0 && !SubTrackFilter::isSubTrack(pid) &&
        (codec[0] == 'V' || codec[0] == 'A'))
        if (const auto it = m_containerReader.m_demuxers.find(fileList[0]);
            it != m_containerReader.m_demuxers.end() && it->second.m_demuxer)
        {
            uint64_t maxId = 0;
            std::vector<std::vector<uint8_t>> mappings;
            // the first file's header declares them
            bool declared = it->second.m_demuxer->getBlockAdditionInfo(pid, maxId, mappings);
            auto* const mkvJoin = dynamic_cast<MatroskaDemuxer*>(it->second.m_demuxer);
            for (size_t i = 1; i < fileList.size() && !declared && mkvJoin; ++i)
            {
                const std::string file = unquoteStr(trimStr(fileList[i]));
                const std::string ext = strToUpperCase(extractFileExt(file));
                if (ext != "MKV" && ext != "MKA" && ext != "MKS" && ext != "MK3D" && ext != "WEBM")
                    continue;
                MatroskaDemuxer probe(m_readManager);
                probe.openFile(file);
                if (!probe.getBlockAdditionInfo(pid, maxId, mappings))
                    continue;
                mkvJoin->declareBlockAdditions(pid, maxId, mappings);
                declared = true;
                std::map<int32_t, TrackInfo> trackList;
                probe.getTrackList(trackList);
                if (const auto t = trackList.find(pid); t != trackList.end())
                    if (const auto mode = t->second.m_srcParams.find("srcalphamode");
                        mode != t->second.m_srcParams.end())
                        joinAlphaMode = mode->second;
            }
            if (declared)
                it->second.m_demuxer->keepBlockAdditions(pid);
        }
    streamInfo.m_codec = codec;
    streamInfo.m_addParams = addParams;
    if (!joinAlphaMode.empty() && addParams.find("srcalphamode") == addParams.end())
        streamInfo.m_addParams["srcalphamode"] = joinAlphaMode;
    int streamIndex = codecReader->getStreamIndex();

    // If the codec is a video stream reader with no manually specified FPS,
    // try to get the FPS from the container (e.g. MKV default_duration, MOV timescale)
    if (addParams.find("fps") == addParams.end())
    {
        auto mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
        if (mpegReader && mpegReader->getFPS() == 0.0 && dataReader == &m_containerReader)
        {
            auto demuxerIt = m_containerReader.m_demuxers.find(fileList[0]);
            if (demuxerIt != m_containerReader.m_demuxers.end() && demuxerIt->second.m_demuxer)
            {
                const double containerFps = correctFps(demuxerIt->second.m_demuxer->getTrackFps(pid));
                if (containerFps > 0.0)
                    mpegReader->setContainerFPS(containerFps);
            }
            // For a stream that turns out to name no rate either: the times of its packets, asked from the demuxer of
            // the file when it is needed, not before
            const int packetPid = SubTrackFilter::isSubTrack(pid) ? pid >> 16 : pid;
            mpegReader->setPacketFpsSource(
                [this, file = fileList[0], packetPid](const bool fieldCoded)
                {
                    const auto it = m_containerReader.m_demuxers.find(file);
                    return it != m_containerReader.m_demuxers.end() && it->second.m_demuxer
                               ? it->second.m_demuxer->getTrackPacketFps(packetPid, fieldCoded)
                               : 0.0;
                });
        }
    }

    // fields for SS PG stream
    auto pgStream = dynamic_cast<PGSStreamReader*>(codecReader);
    if (pgStream)
    {
        pgStream->isSSPG = isSSPG;
        pgStream->ssPGOffset = ssPGOffset;
        pgStream->leftEyeSubStreamIdx = leftEyeSubStreamIdx;
        pgStream->rightEyeSubStreamIdx = rightEyeSubStreamIdx;
        for (int a = 0; a < 2; ++a)
        {
            pgStream->alignedSubStreamIdx[a] = alignedSubStreamIdx[a];
            pgStream->alignedOffset[a] = alignedOffset[a];
        }
        // A .sup file: its segments carry the "PG" marker that the reader checks and finds its way back on.
        if (strEndWith(tmpname, ".sup"))
            pgStream->setSrcContainerType(AbstractStreamReader::ContainerType::ctSUP);
    }
    //

    auto itr = addParams.find("timeshift");
    bool timeShiftGiven = itr != addParams.end();  // by the meta or by the file name: the source's start is not used
    if (itr != addParams.end())
    {
        // A number without a unit is in ms. "500us" used to be 500 s, "2S" 2 ms, a fraction was cut off and a value
        // over 1742.9 s ran over 64 bits.
        int64_t value = 0;
        if (!parseTimeToTicks(itr->second, "ms", &value))
            THROW(ERR_COMMON, "timeshift=" << itr->second
                                           << " is not a time. Write a number followed by ns, us, ms or s, such as "
                                              "-120ms or 1.5s; a number alone is in ms.")
        streamInfo.m_timeShift = value;
        if (value > 0)
            streamInfo.m_lastDTS = value;
    }
    else if (!fileList.empty())
    {
        // No explicit timeshift, so fall back to a delay recorded in the file name by a
        // previous demux (or by eac3to, which uses the same convention). An explicit
        // timeshift always wins, because this is the else branch of the test above.
        const int64_t delayMs = delayFromFileName(fileList[0]);
        if (delayMs != 0)
        {
            const int64_t value = delayMs * INTERNAL_PTS_FREQ / 1000;
            streamInfo.m_timeShift = value;
            if (value > 0)
                streamInfo.m_lastDTS = value;
            timeShiftGiven = true;
            LTRACE(LT_INFO, 2, "Applying delay " << delayMs << " ms taken from the file name");
        }
    }
    // save-srt writes the subtitles at the times of their pictures, which the timeshift moves
    if (streamInfo.m_timeShift != 0)
    {
        const int64_t shiftMs =
            llround(static_cast<double>(streamInfo.m_timeShift) * 1000.0 / static_cast<double>(INTERNAL_PTS_FREQ));
        if (auto* srtReader = dynamic_cast<SRTStreamReader*>(codecReader))
            srtReader->setSaveSrtShiftMs(shiftMs);
        if (const auto ass = m_containerReader.m_assDrawing.find({fileList[0], pid});
            ass != m_containerReader.m_assDrawing.end() && ass->second.srtFile)
            ass->second.srtFile->setShiftMs(shiftMs);
    }

    // A subtitle track whose file name says "forced" is the forced track. A Blu-ray has no
    // per track attribute for this: the only thing a disc carries is the navigation command
    // saying which subtitle a player selects at startup and whether it shows just the forced
    // elements of it, which is what default=forced writes. The tools that split the forced
    // pictures into a file of their own put the word in the name, so that is where it comes
    // from, exactly as the language and the delay do.
    //
    // An explicit default= always wins, because that is the user saying it outright.
    if (!codec.empty() && codec[0] == 'S' && addParams.find("default") == addParams.end() && !fileList.empty() &&
        isForcedSubtitleName(fileList[0]))
    {
        streamInfo.m_addParams["default"] = "forced";
        streamInfo.m_addParams["defaultfromname"] = "";  // given way to a default= on another subtitle line
        LTRACE(LT_INFO, 2, "Marking this subtitle as the forced track, taken from the file name");
    }
    // With default= given, the name still says what the track is: a Matroska output marks it forced (FlagForced)
    // unless forced= says otherwise. The window writes default=all for the subtitle chosen as the default one, and
    // a forced file chosen so lost its flag.
    else if (!codec.empty() && codec[0] == 'S' && addParams.find("forced") == addParams.end() && !fileList.empty() &&
             isForcedSubtitleName(fileList[0]))
        streamInfo.m_addParams["forced"] = "";

    itr = addParams.find("lang");
    if (itr != addParams.end())
    {
        // The value goes into the 3 byte language field of a transport stream, the Language element of a Matroska
        // file and the name of a demuxed file, and was taken as written: a TS kept its first three characters without
        // a word, a demux tried to make a file name of it, and a long one overran a buffer in the Matroska output. So
        // it has to be three letters, as the help has always said. lang= with nothing after it stays allowed: it
        // leaves the track without a language.
        //
        // Or a BCP 47 tag (de, pt-BR, zh-Hant-TW), as a Matroska file names a language. The track then gets the
        // ISO 639-2 code the tag stands for, in the form --lang-codes asks for, which is what a disc, a transport
        // stream and a demuxed file name carry; a Matroska output writes the tag itself as well.
        if (!itr->second.empty() && !isLangCodeForMeta(itr->second))
        {
            const std::string& value = itr->second;
            const std::string code = isBcp47Tag(value) ? iso639FromBcp47(value) : std::string();
            if (code.empty())
            {
                const std::string shown =
                    value.size() <= 40 ? value
                                       : value.substr(0, 40) + "... (" + std::to_string(value.size()) + " characters)";
                THROW(ERR_COMMON, "lang=" << shown << " on the line of " << codec
                                          << " is not a language code. lang= takes three letters, an ISO 639-2 code "
                                             "such as eng, ger or deu, or a BCP 47 tag such as en, de-CH or pt-BR; "
                                             "lang=und marks an undetermined language, and lang= with nothing after "
                                             "it leaves the track without one.")
            }
            streamInfo.m_lang = applyLangCodeStyle(code);
            streamInfo.m_langBCP47 = value;
            streamInfo.m_addParams["lang"] = streamInfo.m_lang;
            streamInfo.m_addParams["langbcp47"] = value;
        }
        else
        {
            // in lower case, as the language field of a disc, a transport stream and Matroska is: lang=ENG was
            // written as ENG
            streamInfo.m_lang = strToLowerCase(itr->second);
            streamInfo.m_addParams["lang"] = streamInfo.m_lang;
        }
    }
    else if (dataReader != &m_containerReader && !fileList.empty() && !langFromFileName(fileList[0]).empty())
    {
        // A raw elementary stream has nowhere to carry its language, so the file name is the only
        // place it can be, and that is where a demux puts it: ours writes "...track_4352_eng.ac3",
        // and the same convention is what people ask for after demuxing elsewhere. Read alongside
        // the delay, which has been taken from the file name in the same way for a long time.
        //
        // Only the last token before the extension, and only if it really is an ISO 639-2 code, so
        // a name that merely contains three such letters is not mistaken for a tagged one.
        streamInfo.m_lang = langFromFileName(fileList[0]);
        streamInfo.m_langFromName = true;
        LTRACE(LT_INFO, 2, "Applying language " << streamInfo.m_lang << " taken from the file name");
    }
    int64_t mergeAc3StartNs = 0;  // where the AC-3 track of merge-ac3-track starts against the TrueHD track
    if (dataReader == &m_containerReader)
    {
        // Ask the container about the track, exactly as the FPS block above asks it for a frame rate: its language
        // where the meta gave no lang=, and, whatever the meta says, what only the Matroska muxer reads. Those used to
        // be asked only without a lang=, so a lang= on the line dropped the source's default flag as well.
        //
        // A track that was tagged "deu" in its source used to come out untagged: the language was parsed and printed
        // but never reached a muxer. It is deliberately NOT written into addParams["lang"]: SingleFileMuxer builds
        // demuxed file names from that key, so filling it would silently rename every file a --demux produces.
        // muxerManager passes this field on under its own key instead.
        //
        // ONLY for a single file, and that restriction is load bearing. getTrackList CONSUMES the
        // buffered reader, reading until it has every PMT. With one file that costs nothing, the
        // muxing read is a separate pass and the produced bytes are provably unchanged. With a
        // JOINED track, "a"+"b", the files share one FileListIterator, so consuming the reader
        // here walks it past the first file and that file's picture never reaches the mux, while
        // the log still says "Mux successful complete". A first file smaller than one read block
        // vanished completely. Asking the container is not worth silently dropping a reel, and an
        // explicit lang= still works on joined tracks.
        //
        // A join of Matroska files is asked through a reader of its own on the first file, which reads that file's
        // header and nothing of the join's stream. Not asked, a joined track lost its language, and a joined 3D file
        // whose base view is the right eye was written with the eyes swapped (StereoMode 13 in place of 14).
        AbstractDemuxer* factsDemuxer = nullptr;
        std::unique_ptr<AbstractDemuxer> joinProbe;
        if (fileList.size() == 1 && !anyPart)
        {
            if (const auto demuxerIt = m_containerReader.m_demuxers.find(fileList[0]);
                demuxerIt != m_containerReader.m_demuxers.end())
                factsDemuxer = demuxerIt->second.m_demuxer;
        }
        else if (fileList.size() > 1)
        {
            const std::string first = unquoteStr(trimStr(fileList[0]));
            const std::string ext = strToUpperCase(extractFileExt(first));
            if (ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
            {
                joinProbe = std::make_unique<MatroskaDemuxer>(m_readManager);
                joinProbe->openFile(first);
                factsDemuxer = joinProbe.get();
            }
        }
        if (factsDemuxer)
        {
            std::map<int32_t, TrackInfo> trackList;
            factsDemuxer->getTrackList(trackList);
            // Each track's start against the first video track (srcstartdelay), of the AC-3 track merged into this
            // TrueHD track by merge-ac3-track as of the TrueHD track itself (TrueHDAC3MergeReader::setCoreStart)
            if (const auto merge = addParams.find("merge-ac3-track");
                merge != addParams.end() && !merge->second.empty())
            {
                const auto listed = [&](const int32_t id)
                {
                    const auto t = trackList.find(id);
                    return t != trackList.end() && t->second.m_srcParams.count("srcstartdelay") > 0;
                };
                const auto startOf = [&](const int32_t id) -> int64_t
                {
                    if (!listed(id))
                        return 0;
                    return strtoll(trackList.at(id).m_srcParams.at("srcstartdelay").c_str(), nullptr, 10);
                };
                const int32_t ac3Track = strToInt32(merge->second.c_str());
                mergeAc3StartNs = startOf(ac3Track) - startOf(pid);
                // Neither start listed: both start with the video, or the file has none and lists no start at all. A
                // TrueHD and AC-3 file without video gave the core the TrueHD's start, 500 ms early for one 500 ms
                // after it; the two first blocks give the distance in either case.
                if (!listed(ac3Track) && !listed(pid))
                    if (const auto* mkv = dynamic_cast<MatroskaDemuxer*>(factsDemuxer))
                        mkv->trackStartAgainstNano(ac3Track, pid, mergeAc3StartNs);
            }
            // A line naming a track the file does not have, or one its reader leaves out, made an empty output: the
            // job went on, said it was successful, and warned that the codec on the line was not the codec of the
            // track. It stops here, before anything is written. Only for Matroska and MP4, whose headers list every
            // track; a transport or program stream is listed from its first part only, and a track may start later.
            AbstractDemuxer* const fileDemuxer = factsDemuxer;
            if (pid > 0 && !SubTrackFilter::isSubTrack(pid) && !trackList.empty() &&
                (dynamic_cast<MatroskaDemuxer*>(fileDemuxer) || dynamic_cast<MovDemuxer*>(fileDemuxer)))
            {
                const auto named = trackList.find(pid);
                if (named == trackList.end())
                {
                    std::string tracks;
                    for (const auto& [id, info] : trackList)
                        tracks += (tracks.empty() ? "" : ", ") + std::to_string(id);
                    THROW(ERR_COMMON, unquoteStr(fileList[0]) << " has no track " << pid << " (its tracks are "
                                                              << tracks << "). Nothing was written.")
                }
                if (const auto why = named->second.m_srcParams.find("srcleftout");
                    why != named->second.m_srcParams.end())
                    THROW(ERR_COMMON, "Track " << pid << " of " << unquoteStr(fileList[0])
                                               << " cannot be read: " << why->second << ". Nothing was written.")
            }
            // A 3D track is taken by its views (subTrack=), under a number of its own: of its source track only the
            // base view's eye is carried over, for the disc's playlist and a Matroska output's StereoMode
            if (SubTrackFilter::isSubTrack(pid))
                if (const auto base = trackList.find(pid >> 16); base != trackList.end())
                    if (const auto eye = base->second.m_srcParams.find("srcrighteye");
                        eye != base->second.m_srcParams.end() && addParams.find("srcrighteye") == addParams.end())
                        streamInfo.m_addParams["srcrighteye"] = eye->second;
            if (const auto trackIt = trackList.find(pid); trackIt != trackList.end())
            {
                // TrueHD from a Matroska file has no AC-3 core: Matroska keeps the two apart. A line that takes it as
                // A_AC3, to keep the core with down-to-ac3, ran through and ended with "produced no frames at all",
                // which said only that the codec on the line was wrong. It is said what the track is, and which
                // tracks of the file are AC-3.
                if (const auto id = trackIt->second.m_srcParams.find("srccodecid");
                    codecName == "A_AC3" && id != trackIt->second.m_srcParams.end() &&
                    id->second == MATROSKA_CODEC_ID_AUDIO_TRUEHD)
                {
                    std::string ac3;
                    for (const auto& [other, info] : trackList)
                        if (const auto oid = info.m_srcParams.find("srccodecid");
                            oid != info.m_srcParams.end() &&
                            (oid->second == MATROSKA_CODEC_ID_AUDIO_AC3 || oid->second == MATROSKA_CODEC_ID_AUDIO_EAC3))
                            ac3 += (ac3.empty() ? "" : ", ") + std::to_string(other);
                    if (const auto last = ac3.rfind(", "); last != std::string::npos)
                        ac3.replace(last, 2, " and ");
                    THROW(ERR_COMMON, "Track " << pid << " of " << unquoteStr(fileList[0])
                                               << " is TrueHD without an AC-3 core, so there is no AC-3 to take from "
                                                  "it: in a Matroska file an AC-3 core is "
                                                  "a track of its own, and tsMuxeR does not encode sound. "
                                               << (ac3.empty() ? std::string("The file has no AC-3 track. ")
                                                               : (ac3.find(" and ") == std::string::npos
                                                                      ? "The AC-3 track of the file is "
                                                                      : "The AC-3 tracks of the file are ") +
                                                                     ac3 + ": take one of them on an A_AC3 line. ")
                                               << "To keep the TrueHD, mux it as A_MLP. Nothing was written.")
                }
                // An encrypted track cannot be read without its key, which no file carries (RFC 9559, 14). Its blocks
                // were muxed as they stand, and the output held noise in place of the track.
                if (trackIt->second.m_srcParams.count("encrypted"))
                    THROW(ERR_COMMON, "Track " << pid << " of " << unquoteStr(fileList[0])
                                               << " is encrypted (Matroska ContentEncryption). tsMuxeR cannot "
                                                  "decrypt it; leave the track out of the meta file.")
                if (addParams.find("lang") == addParams.end())
                {
                    streamInfo.m_lang = trackIt->second.m_lang;
                    streamInfo.m_langBCP47 = trackIt->second.m_langBCP47;
                }
                // Under their own keys, which only the Matroska muxer reads, and never over what the meta says. Of a
                // join, the padding at the end is the last file's, which the Opus part below takes from each file.
                for (const auto& [key, value] : trackIt->second.m_srcParams)
                    if (addParams.find(key) == addParams.end() && (!joinProbe || key != "srcendpadding"))
                        streamInfo.m_addParams[key] = value;
                if (auto* pgStream = dynamic_cast<PGSStreamReader*>(codecReader))
                    pgStream->setRateGuessed(trackIt->second.m_srcParams.count("srcpgsrate") > 0);

                // The source's own default flag, under a key only the Matroska muxer reads. It was
                // parsed and thrown away, so a remux replaced the source's choice with "the first
                // track of each type". Not written as "default": that key also drives the disc
                // navigation commands, and filling it would start selecting a default audio or
                // subtitle on discs remuxed from a Matroska, which nobody asked for.
                if (addParams.find("default") == addParams.end() && trackIt->second.m_defaultFlag >= 0)
                    streamInfo.m_addParams["srcdefault"] = trackIt->second.m_defaultFlag ? "1" : "0";
            }
        }
    }
    // The language the clip information file of a disc clip gives the stream, which the listing shows
    const auto takeClipInfoLang = [&](const char* why)
    {
        const std::string m2ts = unquoteStr(trimStr(fileList[0]));
        const std::string clpiFileName =
            findBluRayFile(extractFileDir(m2ts), "CLIPINF", extractFileName(m2ts) + ".clpi");
        CLPIParser clpi;
        if (!clpiFileName.empty() && clpi.parse(clpiFileName.c_str()))
            if (const auto it = clpi.m_streamInfo.find(pid); it != clpi.m_streamInfo.end())
            {
                const std::string clpiLang = applyLangCodeStyle(it->second.language_code);
                if (isLanguageCode(clpiLang) && clpiLang != "und" && clpiLang != streamInfo.m_lang)
                {
                    LTRACE(LT_INFO, 2,
                           "Language for track " << pid << " taken from the clip information, " << clpiLang << why);
                    streamInfo.m_lang = clpiLang;
                }
            }
    };
    // A playlist source: the language the playlist gives the stream, as the listing shows it. The mux took the clip's,
    // so a disc whose playlist and clip disagree was listed in one language and muxed in the other.
    if (!mplsInfoList.empty() && pid > 0)
    {
        const bool langGiven = addParams.find("lang") != addParams.end();
        const MPLSStreamInfo fromPlaylist = mplsInfoList[0].getStreamByPID(pid);
        // Only a language code: three spaces in the playlist were written as the language of the TS, the Matroska
        // track and the clip info
        const std::string lang = fromPlaylist.streamPID ? applyLangCodeStyle(fromPlaylist.language_code) : "";
        if (!langGiven && isLanguageCode(lang) && lang != "und" && lang != streamInfo.m_lang)
        {
            LTRACE(LT_INFO, 2, "Language for track " << pid << " taken from the playlist, " << lang << ".");
            streamInfo.m_lang = lang;
        }
        // Where the playlist gives none, the listing shows the language of the clip information file, and the mux
        // now takes that one too. A stream the playlist does not declare was listed as English and muxed without a
        // language (00 00 00 in a disc's playlist, und in Matroska).
        else if (!langGiven && !(isLanguageCode(lang) && lang != "und") && !SubTrackFilter::isSubTrack(pid))
            takeClipInfoLang(": the playlist gives none.");
        if (auto* pgStream = dynamic_cast<PGSStreamReader*>(codecReader); pgStream && fromPlaylist.streamPID)
        {
            // A 3D playlist names the offset sequence each subtitle is placed by, and the listing shows it
            // (3d-plane:). The mux took it from 3d-plane= alone, so a meta written by hand or by a script made the
            // subtitles of a 3D disc sit at the depth of the screen, without a word. 3d-plane= still wins.
            if (fromPlaylist.offsetId != 0xff && addParams.find("3d-plane") == addParams.end())
            {
                pgStream->setOffsetId(fromPlaylist.offsetId);
                LTRACE(LT_INFO, 2,
                       "3D offset for track " << pid << " taken from the playlist, 3d-plane="
                                              << static_cast<int>(fromPlaylist.offsetId) << ".");
            }
            if (fromPlaylist.isPiP)
            {
                pgStream->isPiP = true;
                streamInfo.m_addParams["srcpip"] = "1";
                LTRACE(LT_INFO, 2,
                       "Track " << pid
                                << " is a picture-in-picture subtitle of the playlist. A disc gets it in the "
                                   "picture-in-picture list, with the picture-in-picture video.");
            }
        }
    }
    // A clip of a disc named on its own: the listing shows the language of its clip information file, and the mux now
    // takes it too. A disc clip need not name the language in the clip itself (one tsMuxeR wrote does not), so its
    // subtitles went into a Matroska file as und while the listing gave their language.
    else if (pid > 0 && !SubTrackFilter::isSubTrack(pid) && addParams.find("lang") == addParams.end())
    {
        const std::string ext = strToLowerCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        if (ext == "m2ts" || ext == "mts")
            takeClipInfoLang(".");
    }
    // A join: every Matroska, MP4 or transport stream file of it is asked whether it has the track, as a single file
    // is above. A join naming a track the files do not have was not checked, and made an empty output; a transport
    // stream without the PID was not asked at all, and its part of the join was left out without a word. The track
    // must also be of the same kind in every file of the same container: a file whose track of that number is a
    // subtitle was read as the AC-3 of the line, with "bad frame" for each packet, and the job said it had succeeded.
    // The clips of a playlist are not asked: a clip without the track, as a short logo without sound before the
    // film, is part of how a disc is made, and the track starts after it.
    if (fileList.size() > 1 && pid > 0 && !SubTrackFilter::isSubTrack(pid) && mplsInfoList.empty())
    {
        std::map<std::string, int> firstKind;  // per kind of container: the type of the track in its first file
        for (const auto& name : fileList)
        {
            const std::string file = unquoteStr(trimStr(name));
            const std::string ext = strToUpperCase(extractFileExt(file));
            std::unique_ptr<AbstractDemuxer> probe;
            std::string container;
            if (ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
            {
                probe = std::make_unique<MatroskaDemuxer>(m_readManager);
                container = "MKV";
            }
            else if (ext == "MP4" || ext == "M4A" || ext == "M4V" || ext == "MOV")
            {
                probe = std::make_unique<MovDemuxer>(m_readManager);
                container = "MP4";
            }
            else if (ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T")
            {
                probe = std::make_unique<TSDemuxer>(m_readManager, "");
                container = "TS";
            }
            else
                continue;
            probe->openFile(file);
            std::map<int32_t, TrackInfo> tracks;
            probe->getTrackList(tracks);
            if (tracks.empty())
                continue;
            const auto found = tracks.find(pid);
            if (found == tracks.end())
            {
                std::string list;
                for (const auto& [id, info] : tracks) list += (list.empty() ? "" : ", ") + std::to_string(id);
                THROW(ERR_COMMON, file << ", a file of the join, has no track " << pid << " (its tracks are " << list
                                       << "). Nothing was written.")
            }
            // a Matroska track is told by the kind its codec name begins with (A_, V_, S_), as its type names an audio
            // codec only when one was recognised; other containers by the type they give
            const auto codecId = found->second.m_srcParams.find("srccodecid");
            const int trackKind = codecId != found->second.m_srcParams.end() && !codecId->second.empty()
                                      ? 1000 + static_cast<uint8_t>(codecId->second[0])
                                      : found->second.m_trackType;
            const auto [kind, isFirst] = firstKind.try_emplace(container, trackKind);
            if (!isFirst && kind->second != 0 && trackKind != 0 && trackKind != kind->second)
                THROW(ERR_COMMON, file << ", a file of the join, has a track " << pid
                                       << " of another kind than the first file of the join has. Nothing was written.")
        }
    }
    // A join of Matroska files with Opus sound: each file is asked for the padding at its end (the DiscardPadding of
    // its last block). The reader leaves the samples it and the next file's pre-skip make out at each joint, and the
    // padding at the end of the join is the last file's. A join was not asked about its tracks at all, so the end
    // of the last file played its padding.
    if (fileList.size() > 1 && pid > 0 && codec == "A_OPUS")
    {
        if (auto* opus = dynamic_cast<OpusStreamReader*>(codecReader))
        {
            std::vector<int64_t> paddings;
            for (const auto& name : fileList)
            {
                const std::string file = unquoteStr(trimStr(name));
                const std::string ext = strToUpperCase(extractFileExt(file));
                // .mks and .mk3d are Matroska as well, and an MP4 gives the padding by the end of its edit list: a
                // join of such files got no correction, and each later file came 20 ms later
                std::unique_ptr<AbstractDemuxer> probe;
                if (ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
                    probe = std::make_unique<MatroskaDemuxer>(m_readManager);
                else if (ext == "MP4" || ext == "M4A" || ext == "M4V" || ext == "MOV")
                    probe = std::make_unique<MovDemuxer>(m_readManager);
                else
                {
                    paddings.clear();
                    break;
                }
                probe->openFile(file);
                std::map<int32_t, TrackInfo> tracks;
                probe->getTrackList(tracks);
                int64_t ns = 0;
                if (const auto t = tracks.find(pid); t != tracks.end())
                    if (const auto d = t->second.m_srcParams.find("srcendpadding"); d != t->second.m_srcParams.end())
                        ns = strtoll(d->second.c_str(), nullptr, 10);
                paddings.push_back(ns);
            }
            if (!paddings.empty())
            {
                if (paddings.back() > 0 && addParams.find("srcendpadding") == addParams.end())
                    streamInfo.m_addParams["srcendpadding"] = std::to_string(paddings.back());
                opus->setJoinEndPadding(paddings);
            }
        }
    }
    // The padding at the end of an Opus track, the last file's in a join: a transport stream says it in its last
    // access unit, as a demux does in its last page
    if (codec == "A_OPUS")
        if (auto* opus = dynamic_cast<OpusStreamReader*>(codecReader))
            if (const auto pad = streamInfo.m_addParams.find("srcendpadding"); pad != streamInfo.m_addParams.end())
                opus->setEndTrim(strtoll(pad->second.c_str(), nullptr, 10));
    // ** THE SOUND OF AN MP4 STARTS WHERE ITS EDIT LIST SAYS. ** An encoder puts start-up samples in front of the sound
    // (1024 for AAC, 256 for AC-3, about 1100 for a LAME MP3), and the MP4 says in its edit list to start that much
    // into the track. That was ignored: the first frame, start-up samples and all, was placed with the first picture,
    // and the sound came that much late against the picture, 21 ms for AAC. The track's times are now moved back by it,
    // as a timeshift would move them, so the start-up samples come before the first picture, where the MP4 has them. A
    // Matroska output of AAC says it with CodecDelay instead (MatroskaMuxer), and so is left alone here. So does a
    // Matroska output of Opus (the pre-skip of its OpusHead) and of Vorbis (its first packet): moved back as well, the
    // sound came early by both, 7 ms for Opus.
    // A join of MP4 files ("a.mp4"+"b.mp4") is not asked about its tracks (see above), so its first file's edit list
    // start was not known and the sound of the join began with the encoder's priming. The first file is read on its
    // own for it; the priming of every later file is left out at its joint by the MP4 reader (MovDemuxer::openFile).
    if (fileList.size() > 1 && pid > 0 && !codec.empty() && codec[0] == 'A' &&
        streamInfo.m_addParams.find("srccodecdelay") == streamInfo.m_addParams.end())
    {
        const std::string ext = strToLowerCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        if (ext == "mp4" || ext == "mov" || ext == "m4a" || ext == "m4v")
        {
            MovDemuxer probe(m_readManager);
            probe.openFile(unquoteStr(trimStr(fileList[0])));
            std::map<int32_t, TrackInfo> firstTracks;
            probe.getTrackList(firstTracks);
            if (const auto t = firstTracks.find(pid); t != firstTracks.end())
                for (const char* key : {"srccodecdelay", "srcvideolead", "srcopusskip", "srcstartdelay"})
                    if (const auto d = t->second.m_srcParams.find(key); d != t->second.m_srcParams.end())
                        streamInfo.m_addParams[key] = d->second;
        }
        // The same for the start of a track of a join of Matroska files, which is that of its first file
        else if (ext == "mkv" || ext == "mka" || ext == "mks" || ext == "mk3d" || ext == "webm")
        {
            MatroskaDemuxer probe(m_readManager);
            probe.openFile(unquoteStr(trimStr(fileList[0])));
            std::map<int32_t, TrackInfo> firstTracks;
            probe.getTrackList(firstTracks);
            if (const auto t = firstTracks.find(pid); t != firstTracks.end())
                if (const auto d = t->second.m_srcParams.find("srcstartdelay"); d != t->second.m_srcParams.end())
                    streamInfo.m_addParams["srcstartdelay"] = d->second;
        }
    }
    // ** AN MP4 TRACK STARTS WHERE THE FILE SAYS, ALSO ON THE COMMAND LINE. ** A track that starts after the first
    // video track (its leading empty edits, a fragmented track's first tfdt, the composition time of its first sample
    // without an edit list, a start offset written as a long first sample) was given that start only in the listing,
    // as its Stream delay, which the window writes as timeshift=. A meta written by hand muxed every track from the
    // same moment: the sound of a file whose sound starts 0.5 s after its picture came 0.5 s early. The start is
    // applied where the meta gives no timeshift= (and the file name no delay), so a meta from the window, which
    // carries it as timeshift=, gets it once, as before. A Matroska source gives the start of its tracks the same way
    // (MatroskaDemuxer::loadTrackExtras).
    //
    // The same for the sound of a transport stream, a join of them and a playlist: the first PES of the track against
    // the first picture of the video of the (first) file that is shown (tsTrackStart). The sound of a file whose sound
    // starts 0.5 s after its picture came 0.5 s early, and a disc clip whose sound starts a few ms before its picture
    // had it that much late. Not for a playlist whose first play item starts after the first key picture of its clip:
    // the sound of such a part is cut to the time of its first picture (TSDemuxer::keptInPart). Only for the sound, as
    // the window writes it: a second video track of a transport stream is the other view, the enhancement layer or the
    // picture in picture, which go with the main video.
    // ** THE EDIT LIST START OF AN MP4 IS PART OF ITS LISTED DELAY. ** The listing gives the sound of an MP4 track with
    // an edit list start (srccodecdelay) the delay it is muxed with (MovDemuxer::getTrackDelay): its start, less what
    // the edit list skips, plus the pictures a cut file's video keeps. The window writes that delay as timeshift=, so a
    // timeshift= on such a line stands for the listed delay, as for a fragmented file: the track is put where the file
    // says, moved by the difference. A line with the listed delay muxes as one without timeshift=, and a demux names
    // the file with that delay, so the demuxed files muxed again keep it (it was lost).
    bool mp4EditStart = false;
    int64_t mp4ListedMs = 0;
    if (pid > 0 && !codec.empty() && codec[0] == 'A' && !fileList.empty() &&
        streamInfo.m_addParams.find("srccodecdelay") != streamInfo.m_addParams.end())
    {
        const std::string ext = strToLowerCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        mp4EditStart = ext == "mp4" || ext == "mov" || ext == "m4a" || ext == "m4v";
    }
    if (mp4EditStart)
    {
        const auto paramNs = [&](const char* key) -> int64_t
        {
            const auto it = streamInfo.m_addParams.find(key);
            return it == streamInfo.m_addParams.end() ? 0 : strtoll(it->second.c_str(), nullptr, 10);
        };
        const int64_t lead = paramNs("srcvideolead");
        const int64_t listedNs = paramNs("srcstartdelay") + (lead > 0 ? lead : 0) - paramNs("srccodecdelay");
        mp4ListedMs = (listedNs >= 0 ? listedNs + 500000 : listedNs - 500000) / 1000000;
        if (timeShiftGiven)
        {
            streamInfo.m_timeShift -= mp4ListedMs * (INTERNAL_PTS_FREQ / 1000);
            streamInfo.m_lastDTS = (std::max)(streamInfo.m_timeShift, int64_t{0});
            timeShiftGiven = false;
        }
    }
    int64_t tsStart = 0;  // 90 kHz ticks
    if (!timeShiftGiven && pid > 0 && !codec.empty() && codec[0] == 'A' && !fileList.empty() &&
        streamInfo.m_addParams.find("srcstartdelay") == streamInfo.m_addParams.end() &&
        !(anyPart && !clipParts.empty() && clipParts[0].startCut))
    {
        const std::string first = unquoteStr(trimStr(fileList[0]));
        const std::string ext = strToUpperCase(extractFileExt(first));
        if (ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T" || ext == "SSIF")
            tsStart = tsTrackStart(m_readManager, first, SubTrackFilter::isSubTrack(pid) ? pid >> 16 : pid);
    }
    if (!timeShiftGiven)
        if (const auto start = streamInfo.m_addParams.find("srcstartdelay");
            start != streamInfo.m_addParams.end() || tsStart != 0)
        {
            const int64_t ns = tsStart != 0 ? tsStart * 100000 / 9 : strtoll(start->second.c_str(), nullptr, 10);
            const auto ticks =
                tsStart != 0
                    ? tsStart * (INTERNAL_PTS_FREQ / 90000)
                    : static_cast<int64_t>(static_cast<double>(ns) * INTERNAL_PTS_FREQ / 1e9 + (ns > 0 ? 0.5 : -0.5));
            if (ticks != 0)
            {
                // for the name of a demuxed file, which carries the start as its delay (SingleFileMuxer)
                streamInfo.m_addParams["srcstartms"] = int64ToStr((ns + (ns > 0 ? 500000 : -500000)) / 1000000);
                streamInfo.m_timeShift += ticks;
                if (streamInfo.m_timeShift > 0)
                    streamInfo.m_lastDTS = FFMAX(streamInfo.m_lastDTS, streamInfo.m_timeShift);
                LTRACE(LT_INFO, 2,
                       "Track " << pid << " of " << unquoteStr(fileList[0]) << " starts "
                                << doubleToStr(static_cast<double>(ns > 0 ? ns : -ns) / 1e6, 3) << " ms "
                                << (ns > 0 ? "after" : "before")
                                << " the first video track of the file, so its times are moved by that much.");
            }
        }
    if (mp4EditStart)
        streamInfo.m_addParams["srcstartms"] = int64ToStr(mp4ListedMs);
    // A cut file's video keeps the pictures before the cut (srcvideolead, see MovDemuxer::mov_read_moov), so the sound
    // is moved back by its edit list start less that pre-roll. A Matroska output of AAC says the edit list start as
    // CodecDelay, and one of Opus says the pre-skip of its OpusHead (srcopusskip): only the rest is moved.
    if (!codec.empty() && codec[0] == 'A')
    {
        const auto paramNs = [&](const char* key) -> int64_t
        {
            const auto it = streamInfo.m_addParams.find(key);
            return it == streamInfo.m_addParams.end() ? 0 : strtoll(it->second.c_str(), nullptr, 10);
        };
        const int64_t editNs = paramNs("srccodecdelay");
        const int64_t leadNs = paramNs("srcvideolead");
        // the start-up samples of AAC from a Matroska file (its CodecDelay) rather than from an MP4 edit list
        const std::string firstExt =
            fileList.empty() ? std::string() : strToLowerCase(extractFileExt(unquoteStr(trimStr(fileList[0]))));
        const bool codecDelayFromMkv =
            firstExt == "mkv" || firstExt == "mka" || firstExt == "mk3d" || firstExt == "webm";
        int64_t backNs = editNs - leadNs;
        if (m_matroskaOutput && codec == "A_AAC")
            backNs = -leadNs;  // CodecDelay is the whole edit list start
        else if (m_matroskaOutput && codec == "A_OPUS")
        {
            const int64_t skipNs = paramNs("srcopusskip");
            backNs = (skipNs > 0 && editNs > skipNs ? editNs - skipNs : 0) - leadNs;  // CodecDelay is the pre-skip
        }
        else if (m_matroskaOutput && codec == "A_VORBIS")
            backNs = -leadNs;  // CodecDelay is the first packet
        if (backNs != 0)
        {
            streamInfo.m_timeShift -=
                static_cast<int64_t>(static_cast<double>(backNs) * INTERNAL_PTS_FREQ / 1e9 + (backNs > 0 ? 0.5 : -0.5));
            if (leadNs > 0)
                LTRACE(LT_INFO, 2,
                       "Track " << pid << " of " << unquoteStr(fileList[0]) << ": its edit list starts the sound "
                                << doubleToStr(static_cast<double>(editNs) / 1e6, 3)
                                << " ms into the track and the video keeps "
                                << doubleToStr(static_cast<double>(leadNs) / 1e6, 3)
                                << " ms before the cut, so its times are moved " << (backNs > 0 ? "back" : "on")
                                << " by " << doubleToStr(static_cast<double>(backNs > 0 ? backNs : -backNs) / 1e6, 3)
                                << " ms.");
            else
                LTRACE(LT_INFO, 2,
                       "Track " << pid << " of " << unquoteStr(fileList[0])
                                << (codecDelayFromMkv ? ": its CodecDelay starts the sound "
                                                      : ": its edit list starts the sound ")
                                << doubleToStr(static_cast<double>(editNs) / 1e6, 3)
                                << " ms into the track, so its times are moved back by that much.");
        }
    }
    // The tags at the end of a raw audio file are not read (FileReaderData::openStream), so they are not in the size
    // the reading is measured against either: the check that a mux read all of its sources would stop a file with a
    // big tag, and the progress line would never reach the end
    if (dynamic_cast<BufferedFileReader*>(dataReader) != nullptr &&
        SimplePacketizerReader::cutsEndTags(codecReader->getCodecInfo().codecID))
        for (const std::string& name : fileList)
            if (File tagFile; tagFile.open(name.c_str(), File::ofRead))
                fileSize -= SimplePacketizerReader::tagChainAtFileEnd(tagFile);
    m_totalSize += fileSize;

    if (auto* mergeReader = dynamic_cast<TrueHDAC3MergeReader*>(codecReader))
    {
        const auto itTrack = addParams.find("merge-ac3-track");
        const auto itFile = addParams.find("merge-ac3-file");
        const bool haveTrack = (itTrack != addParams.end() && !itTrack->second.empty());
        const bool haveFile = (itFile != addParams.end() && !itFile->second.empty());
        if (haveTrack && haveFile)
            THROW(ERR_INVALID_CODEC_FORMAT, "Specify only one of merge-ac3-track or merge-ac3-file.")

        if (haveTrack)
        {
            if (dataReader != &m_containerReader)
                THROW(ERR_INVALID_CODEC_FORMAT,
                      "merge-ac3-track is only supported when the TrueHD stream is inside a container (e.g. MKV).")
            const int r2 = m_containerReader.createReader(mergeReader->getTmpBufferSize());
            if (!m_containerReader.openStream(r2, fileList[0].c_str(), mergeReader->mergeAc3TrackPid(), &ac3CodecInfo))
                THROW(ERR_CANT_OPEN_STREAM, "Can't open merge-ac3-track stream: " << fileList[0])
            streamInfo.m_mergeAc3ReaderId = r2;
            streamInfo.m_mergeAc3DataReader = &m_containerReader;
            if (mergeAc3StartNs != 0)
            {
                mergeReader->setCoreStart(
                    static_cast<int64_t>(static_cast<double>(mergeAc3StartNs) * INTERNAL_PTS_FREQ / 1e9 +
                                         (mergeAc3StartNs > 0 ? 0.5 : -0.5)));
                LTRACE(
                    LT_INFO, 2,
                    "Track " << mergeReader->mergeAc3TrackPid() << " of " << unquoteStr(fileList[0]) << " starts "
                             << doubleToStr(
                                    static_cast<double>(mergeAc3StartNs > 0 ? mergeAc3StartNs : -mergeAc3StartNs) / 1e6,
                                    3)
                             << " ms " << (mergeAc3StartNs > 0 ? "after" : "before") << " track " << pid
                             << ", so its AC-3 core frames are moved by that much against the TrueHD.");
            }
        }
        else if (haveFile)
        {
            const std::string ac3File = unquoteStr(trimStr(itFile->second));
            AbstractReader* ac3Reader = m_readManager.getReader(ac3File.c_str());
            if (ac3Reader == nullptr)
                THROW(ERR_INVALID_CODEC_FORMAT, "merge-ac3-file: can't open reader for " << ac3File)
            const int r2 = ac3Reader->createReader(mergeReader->getTmpBufferSize());
            if (!ac3Reader->openStream(r2, ac3File.c_str(), 0, &ac3CodecInfo))
                THROW(ERR_CANT_OPEN_STREAM, "Can't open merge-ac3-file stream: " << ac3File)
            streamInfo.m_mergeAc3ReaderId = r2;
            streamInfo.m_mergeAc3DataReader = ac3Reader;
            // A source like the others: an output of its name was not refused, and was written over it
            streamInfo.m_sourceFiles.push_back(ac3File);
        }
    }

    return streamIndex;
}

// Every parameter a track line may legally carry. Collected from BOTH ways the code consumes
// them: the addParams.find() lookups, and the subtitle options that are matched by ITERATING the
// map (metaDemuxer.cpp, "addParam.first == ..."). Missing the second group would have made this
// warn about perfectly valid font settings.
static bool isKnownTrackParam(const std::string& name)
{
    static const std::set<std::string> known = {
        // general
        "track", "subTrack", "lang", "timeshift", "default", "secondary", "subClip", "track-name", "title", "dvd-title",
        // video
        "fps", "ar", "level", "video-width", "video-height", "insertSEI", "forceSEI", "autoSEI", "contSPS",
        "delPulldown", "stretch", "3d-plane", "hdr10-override", "save-srt",
        // audio
        "down-to-ac3", "down-to-dts", "merge-ac3-track", "merge-ac3-file", "drop-ac3-core", "split-ac3-core", "w64",
        // picture in picture
        "pipCorner", "pipHOffset", "pipVOffset", "pipScale", "pipLumma",
        // text subtitles
        "font-name", "font-size", "font-color", "font-bold", "font-italic", "font-underline", "font-strike-out",
        "font-border", "font-charset", "line-spacing", "bottom-offset", "fadein-time", "fadeout-time", "keep-text",
        "fonts-dir", "ass-as-srt",
        // set internally, never written by a user, but accepted so a round tripped meta does
        // not warn. srcdefault is the source container's own default track flag.
        "forced", "hearing-impaired", "visual-impaired", "text-descriptions", "original", "commentary", "srclang",
        "srclangbcp47", "srcdefault", "srcname", "srcflags", "srccolour", "srcdisplay", "srcendpadding", "srcpgsrate",
        "srccrop", "srcfieldorder", "srcprojection", "srccodecdelay", "srcvideolead", "srcopusskip", "srcrighteye",
        "langbcp47", "srcvpxprivate", "srccodecid", "srcstartdelay", "srcstereomode", "srcalphamode", "srcstartms",
        "srcaacsbr",
        // a part of a playlist's PG track (an eye of a stereo subtitle, an aligned subtitle), a picture-in-picture one
        "srcpgpart", "srcpip"};
    return known.find(name) != known.end();
}

// The first source's title, and the attachments and file-wide tags of every source, each once.
std::string METADemuxer::getTitle()
{
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
        if (demuxerData.m_demuxer)
            if (std::string title = demuxerData.m_demuxer->getTitle(); !title.empty())
                return title;
    return {};
}

std::vector<std::vector<uint8_t>> METADemuxer::getPassThroughAttachments()
{
    std::vector<std::vector<uint8_t>> out;
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
        if (demuxerData.m_demuxer)
            for (auto& raw : demuxerData.m_demuxer->getPassThroughAttachments())
                if (std::find(out.begin(), out.end(), raw) == out.end())
                    out.push_back(std::move(raw));
    return out;
}

std::vector<uint8_t> METADemuxer::getPlainMatroskaChapters()
{
    // In the order getChapters asks, so that this is the file the output's chapters come from
    for (const StreamInfo& stream : m_codecInfo)
        if (m_mplsStreamMap.find(splitQuotedStr(stream.m_fullStreamName.c_str(), '+').front()) != m_mplsStreamMap.end())
            return {};
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
    {
        if (!demuxerData.m_demuxer)
            continue;
        auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxerData.m_demuxer);
        // A Matroska file counts by its chapters as stored, also when its default edition shows none of them
        if (mkv ? mkv->getSourceEditions().chapters.empty() : demuxerData.m_demuxer->getChapters().empty())
            continue;
        const auto* join = dynamic_cast<const FileListIterator*>(demuxerData.m_iterator);
        if (!mkv || (join && (join->getFiles().size() > 1 ||
                              (!join->getRanges().empty() && join->getRanges().front().startNs >= 0))))
        {
            // A join, or a cut of one file, keeps the chapters the default editions show, and no other edition. Where
            // the default edition shows none, its chapters all hidden, the output had no chapters, without a word.
            if (mkv && join && mkv->getChapters().empty())
                LTRACE(LT_WARN, 2,
                       "Warning: the default edition of "
                           << join->getFiles().front() << " shows no chapter (they are all hidden). "
                           << (join->getFiles().size() > 1 ? "A join" : "A cut")
                           << " keeps only the chapters of the default editions, so the output has none of them. The "
                              "other editions are kept only from a single file that is not cut.");
            return {};
        }
        return mkv->getSourceEditions().chapters;
    }
    return {};
}

AbstractDemuxer::SourceEditions METADemuxer::getSourceEditions()
{
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
        if (demuxerData.m_demuxer)
            if (SourceEditions editions = demuxerData.m_demuxer->getSourceEditions();
                !editions.chapters.empty() || !editions.segmentUid.empty())
                return editions;
    return {};
}

std::map<int, std::vector<AbstractDemuxer::SourceTrackTag>> METADemuxer::getTrackTagsByStream()
{
    std::map<int, std::vector<SourceTrackTag>> out;
    std::map<std::string, std::map<uint64_t, std::vector<SourceTrackTag>>> byFile;
    for (const StreamInfo& si : m_codecInfo)
    {
        if (si.m_srcTrackUid == 0 || si.m_streamReader == nullptr)
            continue;
        const auto dit = m_containerReader.m_demuxers.find(si.m_streamName);
        if (dit == m_containerReader.m_demuxers.end() || dit->second.m_demuxer == nullptr)
            continue;
        auto [fit, added] = byFile.try_emplace(si.m_streamName);
        if (added)
            fit->second = dit->second.m_demuxer->getTrackTags();
        if (const auto t = fit->second.find(si.m_srcTrackUid); t != fit->second.end())
            out[si.m_streamReader->getStreamIndex()] = t->second;
    }
    return out;
}

std::map<int, std::pair<uint64_t, std::vector<std::vector<uint8_t>>>> METADemuxer::getBlockAdditionInfoByStream()
{
    std::map<int, std::pair<uint64_t, std::vector<std::vector<uint8_t>>>> out;
    for (const StreamInfo& si : m_codecInfo)
    {
        if (si.m_streamReader == nullptr || si.m_codec.empty() || (si.m_codec[0] != 'V' && si.m_codec[0] != 'A') ||
            SubTrackFilter::isSubTrack(si.m_pid))
            continue;
        const auto dit = m_containerReader.m_demuxers.find(si.m_streamName);
        if (dit == m_containerReader.m_demuxers.end() || dit->second.m_demuxer == nullptr)
            continue;
        uint64_t maxId = 0;
        std::vector<std::vector<uint8_t>> mappings;
        if (dit->second.m_demuxer->getBlockAdditionInfo(si.m_pid, maxId, mappings))
            out[si.m_streamReader->getStreamIndex()] = {maxId, std::move(mappings)};
    }
    return out;
}

// The packets of a stream reach the muxer with its timeshift added (readPacket), the demuxer kept them without
bool METADemuxer::takeBlockAdditions(const int streamIndex, const int64_t pts, std::vector<uint8_t>& out)
{
    for (const StreamInfo& si : m_codecInfo)
    {
        if (si.m_streamReader == nullptr || si.m_streamReader->getStreamIndex() != streamIndex)
            continue;
        const auto dit = m_containerReader.m_demuxers.find(si.m_streamName);
        return dit != m_containerReader.m_demuxers.end() && dit->second.m_demuxer &&
               dit->second.m_demuxer->takeBlockAdditions(si.m_pid, pts - si.m_timeShift, out);
    }
    return false;
}

std::vector<std::vector<uint8_t>> METADemuxer::getGlobalTags()
{
    std::vector<std::vector<uint8_t>> out;
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
        if (demuxerData.m_demuxer)
            for (auto& raw : demuxerData.m_demuxer->getGlobalTags())
                if (std::find(out.begin(), out.end(), raw) == out.end())
                    out.push_back(std::move(raw));
    return out;
}

// The chapters of Matroska files joined one after the other, each file's moved to where the join puts it: where the
// video of the file before ends, less where the file's own video starts (see METADemuxer::getChapters). endNano, when
// given, gets where the last file's video ends on that clock, 0 when a length could not be read.
static std::vector<AVChapter> joinedMatroskaChapters(const BufferedReaderManager& readManager,
                                                     const std::vector<std::string>& parts, int64_t* endNano)
{
    std::vector<AVChapter> chapters;
    int64_t offsetNano = 0;
    int64_t prevEndNano = 0;
    if (endNano)
        *endNano = 0;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        MatroskaDemuxer partDemuxer(readManager);
        partDemuxer.openFile(parts[i]);
        int64_t startNano = 0, partEndNano = 0;
        if (!partDemuxer.getVideoSpanNano(startNano, partEndNano))
        {
            startNano = 0;
            partEndNano = partDemuxer.getFileDurationNano();
        }
        if (i > 0)
            offsetNano += prevEndNano - startNano;
        for (AVChapter chapter : partDemuxer.getChapters())
        {
            chapter.start += offsetNano;
            if (chapter.end >= 0)
                chapter.end += offsetNano;
            if (chapter.start >= 0)
                chapters.push_back(std::move(chapter));
        }
        if (partEndNano <= 0)
        {
            if (i + 1 < parts.size())
                LTRACE(LT_WARN, 2,
                       "Warning: the length of "
                           << parts[i]
                           << " cannot be read (it has no Duration and no Cues), so the chapters of the "
                              "files joined after it are left out.");
            return chapters;
        }
        prevEndNano = partEndNano;
        if (endNano && i + 1 == parts.size())
            *endNano = offsetNano + partEndNano;
    }
    return chapters;
}

std::vector<AVChapter> METADemuxer::getChapters()
{
    // A Blu-ray playlist keeps its chapters as playlist marks rather than in any demuxer, so it
    // has to be walked separately. Mark times are relative to the play item they belong to, and
    // the play items run back to back, so each one's span has to be accumulated. Same arithmetic
    // the playlist listing uses in main.cpp; all values are 45 kHz ticks.
    //
    // Playlists joined with "+" ("a.mpls"+"b.mpls") are walked in the order the line joins them,
    // and the span keeps adding up from one playlist to the next, which is what the window adds
    // when it shows the chapters of joined playlists. The cache of parsed playlists used to be
    // walked instead. It is ordered by name and gave the marks of ONE playlist: the others lost
    // theirs, and when the join did not follow the names, the second playlist's marks landed at
    // the start (#27).
    for (const StreamInfo& stream : m_codecInfo)
    {
        std::vector<AVChapter> chapters;
        int64_t playItemOffset = 0;
        for (const std::string& playlist : splitQuotedStr(stream.m_fullStreamName.c_str(), '+'))
        {
            // The same split getMplsInfo used to fill the cache, so a playlist is found under the
            // name it was stored with. A line that names anything else is not a playlist line.
            const auto cached = m_mplsStreamMap.find(playlist);
            if (cached == m_mplsStreamMap.end())
                break;
            const MPLSParser& mpls = cached->second;
            size_t markIndex = 0;
            for (size_t i = 0; i < mpls.m_playItems.size(); ++i)
            {
                for (; markIndex < mpls.m_marks.size(); ++markIndex)
                {
                    const PlayListMark& mark = mpls.m_marks[markIndex];
                    if (static_cast<size_t>(mark.m_playItemID) > i)
                        break;
                    const int64_t ticks =
                        static_cast<int64_t>(mark.m_markTime) - mpls.m_playItems[i].IN_time + playItemOffset;
                    if (ticks >= 0)
                        chapters.emplace_back(ticks * 1000000000LL / 45000LL, std::string());
                }
                playItemOffset += mpls.m_playItems[i].OUT_time - mpls.m_playItems[i].IN_time;
            }
        }
        if (!chapters.empty())
            return chapters;
    }
    // A DVD title keeps its chapters in the IFO files: where each starts in the title (dvdTitles.h)
    if (!m_dvdChapters.empty())
        return m_dvdChapters;

    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
    {
        if (!demuxerData.m_demuxer)
            continue;
        std::vector<AVChapter> chapters = demuxerData.m_demuxer->getChapters();
        // A join ("a.mkv"+"b.mkv") is read through one demuxer that moves on from file to file, and only the file it
        // had open was asked, so every part after the first lost its chapters (#27). Each part is opened on its own
        // here, and its chapters are moved to where the join puts that part.
        //
        // That is where the VIDEO of the part before it ends: the video reader counts frames, and the next part's
        // first picture follows the last picture of the one before. The parts' Durations were added up instead, and a
        // Duration is where the longest track ends. On a DVD rip whose subtitle runs past the picture, that put the
        // second part's chapters 1.03 s late, and 0.096 s late where the audio ran past it (both measured frame by
        // frame on the output). A part's own clock may also start late, as the parts of a linked split do, so its
        // chapters are moved by the end of the video before it less the start of its own video. These are the
        // numbers the listing prints as "Video start" and "Video end", and the window places the chapters it shows
        // the same way. A part whose video cannot be measured (no Cues) counts by its Duration, as before; a part with
        // neither ends the list, because the chapters after it could not be placed, and that is said.
        const auto* join = dynamic_cast<const FileListIterator*>(demuxerData.m_iterator);
        if (join && !join->getRanges().empty() && join->getRanges().front().startNs >= 0)
        {
            // A play timeline: a chapter where each part starts, named as the chapter of the edition it comes from.
            chapters.clear();
            for (const JoinRange& range : join->getRanges())
            {
                if (!chapters.empty())
                    chapters.back().end = range.offsetNs;  // a part ends where the next one starts
                chapters.emplace_back(range.offsetNs, range.name);
            }
        }
        else if (join && join->getFiles().size() > 1 && dynamic_cast<MatroskaDemuxer*>(demuxerData.m_demuxer))
            chapters = joinedMatroskaChapters(m_readManager, join->getFiles(), nullptr);
        if (!chapters.empty())
            return chapters;
    }
    return {};
}

// The length a container file states, opened on its own: one file of a join.
static int64_t fileDurationNano(const BufferedReaderManager& readManager, const std::string& fileName)
{
    const std::string ext = strToLowerCase(extractFileExt(fileName));
    if (ext == "ts" || ext == "m2ts" || ext == "mts" || ext == "m2t" || ext == "ssif")
        return getTSDuration(fileName.c_str()) * 1000000000LL / 90000LL;
    if (ext == "vob" || ext == "mpg" || ext == "mpeg" || ext == "evo")
        return getPSDuration(fileName.c_str()) * 1000000000LL / 90000LL;
    if (ext == "mkv" || ext == "mka" || ext == "mks" || ext == "mk3d" || ext == "webm")
    {
        MatroskaDemuxer part(readManager);
        part.openFile(fileName);
        return part.getFileDurationNano();
    }
    if (ext == "mp4" || ext == "m4v" || ext == "m4a" || ext == "mov")
    {
        MovDemuxer part(readManager);
        part.openFile(fileName);
        return part.getFileDurationNano();
    }
    return 0;  // a raw stream states no length
}

// The share of a transport stream file that the given tracks take, from the packets of 32 windows of 1 MB spread over
// the file; -1 when the file cannot be read or holds no packets there. pids as the meta gives them: a sub track is the
// stream of pid >> 16.
static double transportStreamShare(const std::string& fileName, const std::map<int32_t, DemuxerReadPolicy>& pids)
{
    File file;
    if (!file.open(fileName.c_str(), File::ofRead))
        return -1.0;
    int64_t fileSize = 0;
    file.size(&fileSize);
    std::set<int> wanted;
    for (const auto& [pid, policy] : pids) wanted.insert(SubTrackFilter::isSubTrack(pid) ? pid >> 16 : pid);
    constexpr int WINDOW_COUNT = 32;
    constexpr uint32_t WINDOW_BYTES = 1024 * 1024;
    std::vector<uint8_t> buf(WINDOW_BYTES);
    int64_t all = 0;
    int64_t kept = 0;
    for (int w = 0; w < WINDOW_COUNT; ++w)
    {
        const int64_t pos = fileSize > WINDOW_BYTES ? (fileSize - WINDOW_BYTES) / (WINDOW_COUNT - 1) * w : 0;
        if (file.seek(pos) != pos)
            break;
        const int n = file.read(buf.data(), WINDOW_BYTES);
        if (n <= 0)
            break;
        // the first sync byte that is followed by two more at the distance of a packet, of 188 or 192 bytes
        for (int i = 0; i + 2 * 192 < n;)
        {
            int size = 0;
            for (const int ps : {188, 192})
                if (buf[i] == 0x47 && buf[i + ps] == 0x47 && buf[i + 2 * ps] == 0x47)
                    size = ps;
            if (size == 0)
            {
                ++i;
                continue;
            }
            for (; i + size <= n && buf[i] == 0x47; i += size)
            {
                ++all;
                if (wanted.count((buf[i + 1] & 0x1f) << 8 | buf[i + 2]))
                    ++kept;
            }
        }
        if (fileSize <= WINDOW_BYTES)
            break;
    }
    file.close();
    return all > 0 ? static_cast<double>(kept) / static_cast<double>(all) : -1.0;
}

int64_t METADemuxer::selectedSourceSize()
{
    int64_t size = m_totalSize;
    for (const auto& [name, demuxerData] : m_containerReader.m_demuxers)
    {
        // A file of its own, not a join or the clips of a playlist, which keep their whole size
        if (!demuxerData.m_demuxer || demuxerData.m_iterator || demuxerData.m_pids.empty())
            continue;
        const std::string ext = strToUpperCase(extractFileExt(name));
        if (ext != "TS" && ext != "M2TS" && ext != "MTS" && ext != "M2T")
            continue;
        const double share = transportStreamShare(name, demuxerData.m_pids);
        if (share < 0)
            continue;
        int64_t fileSize = 0;
        if (File file; file.open(name.c_str(), File::ofRead))
        {
            file.size(&fileSize);
            file.close();
        }
        // a tenth more and a point, as the windows are a sample of the file
        const double keep = (std::min)(1.0, share * 1.1 + 0.01);
        size -= static_cast<int64_t>(static_cast<double>(fileSize) * (1.0 - keep));
    }
    return (std::max)(int64_t{0}, size);
}

int64_t METADemuxer::statedDurationNano(const bool sumJoins)
{
    int64_t longest = 0;
    std::set<std::string> joinsCounted;
    for (const StreamInfo& stream : m_codecInfo)
    {
        int64_t ticks = 0;  // 45 kHz, as in getChapters
        for (const std::string& playlist : splitQuotedStr(stream.m_fullStreamName.c_str(), '+'))
        {
            const auto cached = m_mplsStreamMap.find(playlist);
            if (cached == m_mplsStreamMap.end())
                break;
            for (const auto& item : cached->second.m_playItems)
                ticks += static_cast<int64_t>(item.OUT_time) - item.IN_time;
        }
        longest = (std::max)(longest, static_cast<int64_t>(ticks * 1000000000LL / 45000LL));
        // A join of files ("a.mkv"+"b.mkv") plays them one after the other, so its length is the sum of theirs. Its
        // demuxer, below, has only the first file open and states that file's length: a CBR disc of three joined
        // files was estimated at a third of its size and reported as fitting before the mux. Asked for by that
        // estimate only, as each file is opened again for it.
        if (!sumJoins || ticks != 0)
            continue;
        const std::vector<std::string> files = extractFileList(stream.m_fullStreamName);
        if (files.size() > 1 && joinsCounted.insert(stream.m_fullStreamName).second)
        {
            int64_t sum = 0;
            for (const std::string& file : files) sum += (std::max)(int64_t{0}, fileDurationNano(m_readManager, file));
            longest = (std::max)(longest, sum);
        }
    }
    for (auto& [name, demuxerData] : m_containerReader.m_demuxers)
        if (demuxerData.m_demuxer)
            longest = (std::max)(longest, demuxerData.m_demuxer->getFileDurationNano());
    return longest;
}

void METADemuxer::readClose()
{
    for (const auto& codecInfo : m_codecInfo)
    {
        if (codecInfo.m_mergeAc3ReaderId >= 0)
        {
            if (codecInfo.m_mergeAc3DataReader)
                codecInfo.m_mergeAc3DataReader->deleteReader(codecInfo.m_mergeAc3ReaderId);
            else
                codecInfo.m_dataReader->deleteReader(codecInfo.m_mergeAc3ReaderId);
        }
        codecInfo.m_dataReader->deleteReader(codecInfo.m_readerID);
        delete codecInfo.m_streamReader;
    }
    m_codecInfo.clear();
}

// ---------------------------------------------------------------------------
// Discovery phase: self-contained probe of all tracks
// ---------------------------------------------------------------------------

// One half of a merged Dolby Vision track, cut out of probed Annex B data as HevcDolbyVisionFilter cuts it: the
// enhancement layer is each type 63 NAL without its two wrapper bytes, and the RPU; the base layer is the rest. The
// picture size of the half, probed on its own, goes to its discovery data.
// A track whose probed data holds no wrapped enhancement layer NAL has nothing to split off, and is refused here, by
// what the stream holds: the filter refuses such a track only after 32 pictures without one, so a shorter plain HEVC
// track was split into an empty second track and called successful.
static void probeSubTrack(uint8_t* data, const size_t size, const int subTrackNo, StreamDiscoveryData& dd)
{
    static constexpr uint8_t startCode[4] = {0, 0, 0, 1};
    std::vector<uint8_t> half;
    int64_t wrapped = 0;
    int64_t rpu = 0;
    uint8_t* end = data + size;
    for (uint8_t* nal = NALUnit::findNextNAL(data, end); nal < end;)
    {
        uint8_t* next = NALUnit::findNALWithStartCode(nal, end, true);
        const uint8_t* nalEnd = next;
        if (next < end)
            while (nalEnd > nal && nalEnd[-1] == 0) --nalEnd;  // the zeroes of the next start code
        if (nalEnd - nal >= 2)
        {
            const auto type = static_cast<HevcUnit::NalType>(nal[0] >> 1 & 0x3f);
            const bool enhancement = type == HevcUnit::NalType::DVEL || type == HevcUnit::NalType::DVRPU;
            if (type == HevcUnit::NalType::DVEL)
                ++wrapped;
            else if (type == HevcUnit::NalType::DVRPU)
                ++rpu;
            if (enhancement == (subTrackNo == HevcDolbyVisionFilter::EL_SUB_TRACK))
            {
                half.insert(half.end(), startCode, startCode + 4);
                const uint8_t* from = type == HevcUnit::NalType::DVEL ? nal + 2 : nal;  // the wrapper
                half.insert(half.end(), from, nalEnd);
            }
        }
        nal = NALUnit::findNextNAL(next, end);
    }
    if (wrapped == 0)
        THROW(ERR_COMMON, (rpu > 0 ? "Dolby Vision: this track carries an RPU but no enhancement layer, so it is "
                                     "single layer and cannot be split into two."
                                   : "This HEVC track carries no Dolby Vision enhancement layer, so it cannot be "
                                     "split into two.")
                              << " Mux it as ONE track, without subTrack=.")
    // The enhancement layer half of a dual layer track is a layer whatever its RPUs say: a profile 8.1 file keeps the
    // disc's layer with converted, single layer RPUs (and the disc's own ones attached, put back as the layers are
    // separated), and that layer was taken for a picture of its own: a second track in Matroska, 0x1012 on a disc.
    if (subTrackNo == HevcDolbyVisionFilter::EL_SUB_TRACK)
        dd.dvRpuElPresent = 1;
    if (half.empty())
        return;
    HEVCStreamReader reader;
    if (!reader.checkStream(half.data(), static_cast<int>(half.size())).codecInfo.codecID)
        return;
    StreamDiscoveryData own;
    reader.fillVideoDiscoveryData(own);
    if (own.width > 0 && own.height > 0)
    {
        dd.width = own.width;
        dd.height = own.height;
    }
}

std::vector<StreamDiscoveryData> METADemuxer::discoverStreams() const
{
    const size_t trackCount = m_codecInfo.size();
    std::vector<StreamDiscoveryData> results(trackCount);

    // Group tracks by their source file so we only open each container once.
    // key = first file name, value = list of (index-into-m_codecInfo, pid)
    std::map<std::string, std::vector<std::pair<size_t, int>>> fileToTracks;
    for (size_t idx = 0; idx < trackCount; ++idx)
    {
        const StreamInfo& si = m_codecInfo[idx];
        fileToTracks[si.m_streamName].emplace_back(idx, si.m_pid);
    }

    for (const auto& [fileName, trackList] : fileToTracks)
    {
        const string unquoted = unquoteStr(fileName);
        const string fileExt = strToLowerCase(extractFileExt(unquoted));

        // --- Determine container type and create a temporary demuxer ---
        std::unique_ptr<AbstractDemuxer> demuxer;
        auto containerType = AbstractStreamReader::ContainerType::ctNone;

        if (fileExt == "m2ts" || fileExt == "mts" || fileExt == "ssif")
        {
            demuxer = std::make_unique<TSDemuxer>(m_readManager, "");
            containerType = AbstractStreamReader::ContainerType::ctM2TS;
        }
        else if (fileExt == "ts" || fileExt == "m2t")
        {
            demuxer = std::make_unique<TSDemuxer>(m_readManager, "");
            containerType = AbstractStreamReader::ContainerType::ctTS;
        }
        else if (fileExt == "vob" || fileExt == "mpg" || fileExt == "mpeg")
        {
            demuxer = std::make_unique<ProgramStreamDemuxer>(m_readManager);
            containerType = AbstractStreamReader::ContainerType::ctVOB;
        }
        else if (fileExt == "evo")
        {
            demuxer = std::make_unique<ProgramStreamDemuxer>(m_readManager);
            containerType = AbstractStreamReader::ContainerType::ctEVOB;
        }
        else if (fileExt == "mkv" || fileExt == "mka" || fileExt == "mks" || fileExt == "mk3d" || fileExt == "webm")
        {
            demuxer = std::make_unique<MatroskaDemuxer>(m_readManager);
            containerType = AbstractStreamReader::ContainerType::ctMKV;
        }
        else if (fileExt == "mp4" || fileExt == "m4v" || fileExt == "m4a" || fileExt == "mov")
        {
            demuxer = std::make_unique<MovDemuxer>(m_readManager);
            containerType = AbstractStreamReader::ContainerType::ctMOV;
        }

        if (demuxer)
        {
            // Open and demux initial data
            try
            {
                demuxer->openFile(fileName);
            }
            catch (...)
            {
                // If the file can't be opened, skip discovery for tracks in this file
                continue;
            }

            const uint32_t fileBlockSize = demuxer->getFileBlockSize();
            if (fileBlockSize == 0)
                continue;  // avoid divide-by-zero in the detect loop for empty/edge containers
            DemuxedData demuxedData;
            std::map<int32_t, TrackInfo> acceptedPidMap;
            demuxer->getTrackList(acceptedPidMap);
            PIDSet acceptedPidSet;
            for (const auto& [pid, info] : acceptedPidMap) acceptedPidSet.insert(pid);
            int64_t discardedSize = 0;

            for (unsigned i = 0; i < DETECT_STREAM_BUFFER_SIZE / fileBlockSize; i++)
                demuxer->simpleDemuxBlock(demuxedData, acceptedPidSet, discardedSize);
            readLateTracks(demuxer.get(), demuxedData, acceptedPidMap, discardedSize);

            // For each track of interest in this file, probe
            for (const auto& [idx, pid] : trackList)
            {
                const StreamInfo& si = m_codecInfo[idx];
                StreamDiscoveryData& dd = results[idx];
                dd.codecName = si.m_codec;
                dd.trackID = pid;

                // A subTrack line names a SUB-PID, (pid << 16) + subTrack, while this probe opens
                // the container WITHOUT a subTrack filter and only ever produces the raw pid. Every
                // lookup of a sub-PID therefore missed and the probe below was skipped, so
                // discovery never ran for a subTrack stream and nothing it finds was applied:
                // the Dolby Vision flags, the frame rate, the codec private data. A merged Dolby
                // Vision track consequently lost its Dolby Vision when muxed to Matroska, because
                // the flags those records are built from are set here or not at all.
                //
                // The raw track is also the RIGHT thing to probe. It still carries the wrapped
                // enhancement layer and the RPU, which is what has to be seen; a split half does
                // not necessarily carry either.
                const int probePid = SubTrackFilter::isSubTrack(pid) ? (pid >> 16) : pid;

                // 1) Extract codec-private from the container
                int cpSize = 0;
                const uint8_t* cp = demuxer->getTrackCodecPrivate(probePid, cpSize);
                if (cp && cpSize > 0)
                    dd.codecPrivate.assign(cp, cp + cpSize);

                // 2) Get the demuxed elementary stream data for this PID
                auto demuxIt = demuxedData.find(probePid);
                if (demuxIt == demuxedData.end() || demuxIt->second.size() == 0)
                {
                    // No data demuxed for this track -- skip probe
                    continue;
                }
                StreamData& vect = demuxIt->second;

                // 3) Create a temporary reader and probe
                int containerDataType = 0;
                int containerStreamIndex = 0;
                if (acceptedPidMap.find(probePid) != acceptedPidMap.end())
                    containerDataType = acceptedPidMap[probePid].m_trackType;

                // Use detectTrackReader-style probing: try each reader type
                // Actually, we know the codec from the meta file, so create directly
                std::unique_ptr<AbstractStreamReader> tmpReader;
                try
                {
                    tmpReader.reset(
                        createCodec(si.m_codec, si.m_addParams, si.m_streamName, std::vector<MPLSPlayItem>(), true));
                }
                catch (...)
                {
                    continue;
                }
                if (!tmpReader)
                    continue;

                // Try as SimplePacketizerReader (audio codecs)
                auto* audioReader = dynamic_cast<SimplePacketizerReader*>(tmpReader.get());
                if (audioReader)
                {
                    dd = audioReader->probeStream(vect.data(), static_cast<int>(vect.size()), containerType,
                                                  containerDataType, containerStreamIndex);
                    dd.codecName = si.m_codec;
                    dd.trackID = pid;
                    // Restore codec-private (probeStream doesn't know about container data)
                    if (dd.codecPrivate.empty() && cp && cpSize > 0)
                        dd.codecPrivate.assign(cp, cp + cpSize);
                    continue;
                }

                // Try as MPEGStreamReader (video codecs)
                auto* videoReader = dynamic_cast<MPEGStreamReader*>(tmpReader.get());
                if (videoReader)
                {
                    // Pre-seed the container FPS so that checkStream() has it
                    // available.  Without this, streams that lack embedded
                    // timing info would leave fps=0 in the discovery data.
                    const double containerFps = correctFps(demuxer->getTrackFps(probePid));
                    if (containerFps > 0.0 && videoReader->getFPS() == 0.0)
                        videoReader->setFPS(containerFps);

                    CheckStreamRez rez;
                    // Each video reader has its own checkStream signature
                    if (auto* h264 = dynamic_cast<H264StreamReader*>(videoReader))
                        rez = h264->checkStream(vect.data(), static_cast<int>(vect.size()));
                    else if (auto* hevc = dynamic_cast<HEVCStreamReader*>(videoReader))
                        rez = hevc->checkStream(vect.data(), static_cast<int>(vect.size()));
                    else if (auto* vvc = dynamic_cast<VVCStreamReader*>(videoReader))
                        rez = vvc->checkStream(vect.data(), static_cast<int>(vect.size()));
                    else if (auto* av1 = dynamic_cast<AV1StreamReader*>(videoReader))
                        rez = av1->checkStream(vect.data(), static_cast<int>(vect.size()));
                    else if (auto* mpeg2 = dynamic_cast<MPEG2StreamReader*>(videoReader))
                        rez = mpeg2->checkStream(vect.data(), static_cast<int>(vect.size()));

                    if (rez.codecInfo.codecID)
                    {
                        dd.discovered = true;
                        dd.codecName = si.m_codec;
                        dd.trackID = pid;
                        dd.streamDescr = rez.streamDescr;
                        videoReader->fillVideoDiscoveryData(dd);

                        // The probe saw the MERGED track, so it reports Dolby Vision for
                        // whichever half asked. Only the enhancement layer carries the RPU,
                        // which is what the same picture probed as two separate elementary
                        // streams reports, and what the pairing depends on: a base layer that
                        // claims an RPU stops being treated as a base layer, and the muxer
                        // then writes disc PIDs 0x1015 and 0x1016 instead of 0x1011 and
                        // 0x1015. Measured, which is why this is here.
                        const int subTrackNo = SubTrackFilter::isSubTrack(pid) ? (pid & 0xFFFF) : 0;
                        if (subTrackNo != 0 && subTrackNo != HevcDolbyVisionFilter::EL_SUB_TRACK)
                        {
                            dd.isDVRPU = false;
                            dd.isDVEL = false;
                        }
                        // The size, though, is that of the half: probed whole, both halves had the size of the
                        // base, 3840 wide, and with the enhancement layer listed first the base could not be told
                        // by its size: the 1080p enhancement layer took 0x1011 and the base
                        // 0x1012, with no 0x1015. The half is cut out of the probed data as the subTrack filter
                        // cuts it, and probed on its own.
                        if (subTrackNo != 0 && dynamic_cast<HEVCStreamReader*>(videoReader))
                            probeSubTrack(vect.data(), vect.size(), subTrackNo, dd);
                        // Restore codec-private from container
                        if (dd.codecPrivate.empty() && cp && cpSize > 0)
                            dd.codecPrivate.assign(cp, cp + cpSize);
                    }
                }
            }
            // demuxer goes out of scope and is destroyed
        }
        else
        {
            // Raw elementary stream (not in a container)
            for (const auto& [idx, pid] : trackList)
            {
                const StreamInfo& si = m_codecInfo[idx];
                StreamDiscoveryData& dd = results[idx];
                dd.codecName = si.m_codec;
                dd.trackID = pid;

                // Read raw bytes from the file
                File file;
                if (!file.open(unquoted.c_str(), File::ofRead))
                    continue;

                auto tmpBuffer = std::make_unique<uint8_t[]>(DETECT_STREAM_BUFFER_SIZE);
                const int len = file.read(tmpBuffer.get(), DETECT_STREAM_BUFFER_SIZE);
                file.close();
                if (len <= 0)
                    continue;

                // Create a temporary reader and probe
                std::unique_ptr<AbstractStreamReader> tmpReader;
                try
                {
                    tmpReader.reset(
                        createCodec(si.m_codec, si.m_addParams, si.m_streamName, std::vector<MPLSPlayItem>(), true));
                }
                catch (...)
                {
                    continue;
                }
                if (!tmpReader)
                    continue;

                auto* audioReader = dynamic_cast<SimplePacketizerReader*>(tmpReader.get());
                if (audioReader)
                {
                    dd = audioReader->probeStream(tmpBuffer.get(), len, containerType, 0, 0);
                    dd.codecName = si.m_codec;
                    dd.trackID = pid;
                    continue;
                }

                auto* videoReader = dynamic_cast<MPEGStreamReader*>(tmpReader.get());
                if (videoReader)
                {
                    CheckStreamRez rez;
                    if (auto* h264 = dynamic_cast<H264StreamReader*>(videoReader))
                        rez = h264->checkStream(tmpBuffer.get(), len);
                    else if (auto* hevc = dynamic_cast<HEVCStreamReader*>(videoReader))
                        rez = hevc->checkStream(tmpBuffer.get(), len);
                    else if (auto* vvc = dynamic_cast<VVCStreamReader*>(videoReader))
                        rez = vvc->checkStream(tmpBuffer.get(), len);
                    else if (auto* av1 = dynamic_cast<AV1StreamReader*>(videoReader))
                        rez = av1->checkStream(tmpBuffer.get(), len);
                    else if (auto* mpeg2 = dynamic_cast<MPEG2StreamReader*>(videoReader))
                        rez = mpeg2->checkStream(tmpBuffer.get(), len);

                    if (rez.codecInfo.codecID)
                    {
                        dd.discovered = true;
                        dd.codecName = si.m_codec;
                        dd.trackID = pid;
                        dd.streamDescr = rez.streamDescr;
                        videoReader->fillVideoDiscoveryData(dd);
                    }
                }
            }
        }
    }

    return results;
}

// ---------------------------------------------------------------------------

// A language may be written either way round: ISO 639-2 has a bibliographic code and a
// terminological one for twenty-four languages, German being "ger" and "deu".
//
// Three pairs were wrong until 6 September 2026: "may" gave Persian and "per" gave Malay, the two
// being swapped, and "mol" gave "rom", which is Romany rather than Romanian.
//
// The last column says whether the pair is still current. Four of these codes are DEPRECATED and
// are accepted on the way in only so that an older disc or an older meta file still reads: jaw for
// Javanese, mol for Moldavian, scc for Serbian and scr for Croatian. Twenty pairs remain, which is
// the number the registration authority publishes, and only those twenty may be PRODUCED. Without
// that distinction, asking for bibliographic codes would hand people codes that were withdrawn,
// and "ron" would come back as "mol" purely because it sits earlier in the table than "rum".
namespace
{
struct LangPair
{
    const char* bibliographic;
    const char* terminological;
    bool current;
};

const LangPair LANG_PAIRS[] = {
    {"alb", "sqi", true},  {"arm", "hye", true},  {"baq", "eus", true},  {"bur", "mya", true}, {"cze", "ces", true},
    {"chi", "zho", true},  {"dut", "nld", true},  {"ger", "deu", true},  {"gre", "ell", true}, {"fre", "fra", true},
    {"geo", "kat", true},  {"ice", "isl", true},  {"jaw", "jav", false}, {"mac", "mkd", true}, {"mao", "mri", true},
    {"may", "msa", true},  {"mol", "ron", false}, {"per", "fas", true},  {"rum", "ron", true}, {"scc", "srp", false},
    {"scr", "hrv", false}, {"slo", "slk", true},  {"tib", "bod", true},  {"wel", "cym", true},
};

LangCodeStyle g_langCodeStyle = LangCodeStyle::Terminological;

// Every two-letter code of ISO 639-1 with its ISO 639-2 codes, terminological and bibliographic, as the ISO 639-2
// registration authority lists them (183 rows). A BCP 47 tag uses the two-letter code where one exists.
struct Iso639_1
{
    const char* twoLetter;
    const char* terminological;
    const char* bibliographic;
};

const Iso639_1 ISO639_1[] = {
    {"aa", "aar", "aar"}, {"ab", "abk", "abk"}, {"ae", "ave", "ave"}, {"af", "afr", "afr"}, {"ak", "aka", "aka"},

    {"am", "amh", "amh"}, {"an", "arg", "arg"}, {"ar", "ara", "ara"}, {"as", "asm", "asm"}, {"av", "ava", "ava"},

    {"ay", "aym", "aym"}, {"az", "aze", "aze"}, {"ba", "bak", "bak"}, {"be", "bel", "bel"}, {"bg", "bul", "bul"},

    {"bi", "bis", "bis"}, {"bm", "bam", "bam"}, {"bn", "ben", "ben"}, {"bo", "bod", "tib"}, {"br", "bre", "bre"},

    {"bs", "bos", "bos"}, {"ca", "cat", "cat"}, {"ce", "che", "che"}, {"ch", "cha", "cha"}, {"co", "cos", "cos"},

    {"cr", "cre", "cre"}, {"cs", "ces", "cze"}, {"cu", "chu", "chu"}, {"cv", "chv", "chv"}, {"cy", "cym", "wel"},

    {"da", "dan", "dan"}, {"de", "deu", "ger"}, {"dv", "div", "div"}, {"dz", "dzo", "dzo"}, {"ee", "ewe", "ewe"},

    {"el", "ell", "gre"}, {"en", "eng", "eng"}, {"eo", "epo", "epo"}, {"es", "spa", "spa"}, {"et", "est", "est"},

    {"eu", "eus", "baq"}, {"fa", "fas", "per"}, {"ff", "ful", "ful"}, {"fi", "fin", "fin"}, {"fj", "fij", "fij"},

    {"fo", "fao", "fao"}, {"fr", "fra", "fre"}, {"fy", "fry", "fry"}, {"ga", "gle", "gle"}, {"gd", "gla", "gla"},

    {"gl", "glg", "glg"}, {"gn", "grn", "grn"}, {"gu", "guj", "guj"}, {"gv", "glv", "glv"}, {"ha", "hau", "hau"},

    {"he", "heb", "heb"}, {"hi", "hin", "hin"}, {"ho", "hmo", "hmo"}, {"hr", "hrv", "hrv"}, {"ht", "hat", "hat"},

    {"hu", "hun", "hun"}, {"hy", "hye", "arm"}, {"hz", "her", "her"}, {"ia", "ina", "ina"}, {"id", "ind", "ind"},

    {"ie", "ile", "ile"}, {"ig", "ibo", "ibo"}, {"ii", "iii", "iii"}, {"ik", "ipk", "ipk"}, {"io", "ido", "ido"},

    {"is", "isl", "ice"}, {"it", "ita", "ita"}, {"iu", "iku", "iku"}, {"ja", "jpn", "jpn"}, {"jv", "jav", "jav"},

    {"ka", "kat", "geo"}, {"kg", "kon", "kon"}, {"ki", "kik", "kik"}, {"kj", "kua", "kua"}, {"kk", "kaz", "kaz"},

    {"kl", "kal", "kal"}, {"km", "khm", "khm"}, {"kn", "kan", "kan"}, {"ko", "kor", "kor"}, {"kr", "kau", "kau"},

    {"ks", "kas", "kas"}, {"ku", "kur", "kur"}, {"kv", "kom", "kom"}, {"kw", "cor", "cor"}, {"ky", "kir", "kir"},

    {"la", "lat", "lat"}, {"lb", "ltz", "ltz"}, {"lg", "lug", "lug"}, {"li", "lim", "lim"}, {"ln", "lin", "lin"},

    {"lo", "lao", "lao"}, {"lt", "lit", "lit"}, {"lu", "lub", "lub"}, {"lv", "lav", "lav"}, {"mg", "mlg", "mlg"},

    {"mh", "mah", "mah"}, {"mi", "mri", "mao"}, {"mk", "mkd", "mac"}, {"ml", "mal", "mal"}, {"mn", "mon", "mon"},

    {"mr", "mar", "mar"}, {"ms", "msa", "may"}, {"mt", "mlt", "mlt"}, {"my", "mya", "bur"}, {"na", "nau", "nau"},

    {"nb", "nob", "nob"}, {"nd", "nde", "nde"}, {"ne", "nep", "nep"}, {"ng", "ndo", "ndo"}, {"nl", "nld", "dut"},

    {"nn", "nno", "nno"}, {"no", "nor", "nor"}, {"nr", "nbl", "nbl"}, {"nv", "nav", "nav"}, {"ny", "nya", "nya"},

    {"oc", "oci", "oci"}, {"oj", "oji", "oji"}, {"om", "orm", "orm"}, {"or", "ori", "ori"}, {"os", "oss", "oss"},

    {"pa", "pan", "pan"}, {"pi", "pli", "pli"}, {"pl", "pol", "pol"}, {"ps", "pus", "pus"}, {"pt", "por", "por"},

    {"qu", "que", "que"}, {"rm", "roh", "roh"}, {"rn", "run", "run"}, {"ro", "ron", "rum"}, {"ru", "rus", "rus"},

    {"rw", "kin", "kin"}, {"sa", "san", "san"}, {"sc", "srd", "srd"}, {"sd", "snd", "snd"}, {"se", "sme", "sme"},

    {"sg", "sag", "sag"}, {"si", "sin", "sin"}, {"sk", "slk", "slo"}, {"sl", "slv", "slv"}, {"sm", "smo", "smo"},

    {"sn", "sna", "sna"}, {"so", "som", "som"}, {"sq", "sqi", "alb"}, {"sr", "srp", "srp"}, {"ss", "ssw", "ssw"},

    {"st", "sot", "sot"}, {"su", "sun", "sun"}, {"sv", "swe", "swe"}, {"sw", "swa", "swa"}, {"ta", "tam", "tam"},

    {"te", "tel", "tel"}, {"tg", "tgk", "tgk"}, {"th", "tha", "tha"}, {"ti", "tir", "tir"}, {"tk", "tuk", "tuk"},

    {"tl", "tgl", "tgl"}, {"tn", "tsn", "tsn"}, {"to", "ton", "ton"}, {"tr", "tur", "tur"}, {"ts", "tso", "tso"},

    {"tt", "tat", "tat"}, {"tw", "twi", "twi"}, {"ty", "tah", "tah"}, {"ug", "uig", "uig"}, {"uk", "ukr", "ukr"},

    {"ur", "urd", "urd"}, {"uz", "uzb", "uzb"}, {"ve", "ven", "ven"}, {"vi", "vie", "vie"}, {"vo", "vol", "vol"},

    {"wa", "wln", "wln"}, {"wo", "wol", "wol"}, {"xh", "xho", "xho"}, {"yi", "yid", "yid"}, {"yo", "yor", "yor"},

    {"za", "zha", "zha"}, {"zh", "zho", "chi"}, {"zu", "zul", "zul"}};
}  // namespace

// why, as far as it can be told: the message named the file and nothing else, also for a file that is there and has a
// path too long for the system
// An image of a disc or a disc folder given as a source: an .iso was read as raw video ("Bad SEI detected", then a
// picture "larger than 256 MiB"), and a folder as an empty file on Linux, and nothing said what to give instead. The
// text says what the name is and what to give; empty for any other name.
std::string discSourceHint(const std::string& fileName)
{
    std::error_code ec;
    const std::filesystem::path path(std::u8string(fileName.begin(), fileName.end()));
    std::string name = fileName;
    while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
    // A folder inside a disc image counts as one on the drive.
    const bool inImage = discImage::isInside(name);
    const auto folderAt = [&](const std::string& sub)
    {
        if (inImage)
            return discImage::isDir(sub.empty() ? name : name + "/" + sub);
        return std::filesystem::is_directory(sub.empty() ? path : path / sub, ec);
    };
    if (folderAt(""))
    {
        const std::string last = strToUpperCase(name.substr(name.find_last_of("/\\") + 1));
        if (last == "BDMV" || folderAt("BDMV"))
            return "it is a Blu-ray folder. Give the playlist of the title as the source, BDMV/PLAYLIST/<number>.mpls, "
                   "or add title=main or title=<number> to the line (tsMuxeR <folder> lists the titles), or use "
                   "--bdmv-to-iso to write the folder into an ISO image";
        if (last == "VIDEO_TS" || folderAt("VIDEO_TS"))
            return "it is a DVD folder. Give the .vob files of the title in VIDEO_TS as the source";
        if (const size_t discs = discsInFolder(name).size(); discs > 0)
            return "it is a folder that holds " + std::to_string(discs) + (discs == 1 ? " disc" : " discs") +
                   " (tsMuxeR <folder> names them). Give one of them with title=main or title=<number>, or a file, "
                   "as the source";
        return "it is a folder. Give a file as the source";
    }
    if (!inImage && strToLowerCase(extractFileExt(name)) == "iso" &&
        std::filesystem::is_regular_file(std::u8string(name.begin(), name.end()), ec))
    {
        // An image is read where the path goes on inside it.
        if (discImage::isDir(name + "/BDMV"))
            return "it is a disc image. Give the playlist of the title inside it as the source, " + name +
                   "/BDMV/PLAYLIST/<number>.mpls, or add title=main or title=<number> to the line (tsMuxeR <image> "
                   "lists the titles)";
        if (discImage::isDir(name + "/VIDEO_TS"))
            return "it is a disc image. Give the .vob files of the title inside it as the source, " + name +
                   "/VIDEO_TS/VTS_<number>_<part>.VOB";
        if (const std::string reason = discImage::unreadableReason(name); !reason.empty())
            return "it is a disc image that cannot be read: " + reason;
        return "it is a disc image with no Blu-ray or DVD folder";
    }
    return {};
}

std::string openFailureReason(const std::string& fileName)
{
    std::error_code ec;
    const std::filesystem::path path(std::u8string(fileName.begin(), fileName.end()));
    if (discImage::isInside(fileName))
        return discImage::isDir(fileName) ? " (" + discSourceHint(fileName) + ")"
                                          : " (the disc image has no such file)";
    if (!std::filesystem::exists(path, ec) && !ec)
        return " (it does not exist)";
    if (std::filesystem::is_directory(path, ec))
        return " (" + discSourceHint(fileName) + ")";
#ifdef _WIN32
    if (fileName.size() >= 260)
        return " (its path has " + std::to_string(fileName.size()) +
               " characters, more than this system opens by default)";
#endif
    return " (the system refused to open it; check that it can be read and is not in use)";
}

std::string iso639FromBcp47(const std::string& tag)
{
    std::string primary = tag.substr(0, tag.find('-'));
    for (char& c : primary) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (primary.size() == 2)
    {
        for (const auto& row : ISO639_1)
            if (primary == row.twoLetter)
                return row.terminological;
        return {};
    }
    if (primary.size() == 3)
        return toIso639_2T(primary);
    return {};  // "x-...", "i-..." and other forms without a language subtag
}

void setLangCodeStyle(const LangCodeStyle style) { g_langCodeStyle = style; }

LangCodeStyle getLangCodeStyle() { return g_langCodeStyle; }

std::string toIso639_2T(const std::string& lang)
{
    for (const auto& [bibliographic, terminological, current] : LANG_PAIRS)
        if (lang == bibliographic)
            return terminological;
    return lang;
}

std::string toIso639_2B(const std::string& lang)
{
    for (const auto& [bibliographic, terminological, current] : LANG_PAIRS)
        if (current && lang == terminological)
            return bibliographic;
    return lang;
}

// What a language read out of a source is reported as. Nothing here touches what gets WRITTEN: a
// lang= in a meta file reaches the disc exactly as typed whatever this is set to.
std::string applyLangCodeStyle(const std::string& lang)
{
    switch (g_langCodeStyle)
    {
    case LangCodeStyle::Source:
        return lang;
    case LangCodeStyle::Bibliographic:
        return toIso639_2B(toIso639_2T(lang));
    case LangCodeStyle::Terminological:
    default:
        return toIso639_2T(lang);
    }
}

bool isLanguageCode(const std::string& lang)
{
    return lang.size() == 3 && std::all_of(lang.begin(), lang.end(), [](const char c)
                                           { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

// The kind a track's container declares (DeclaredKind). A transport stream by its stream type, ITU-T H.222.0 Table
// 2-34 and the Blu-ray stream_coding_type: 0x80 (LPCM on a disc, DigiCipher II video in ATSC) names no kind, and
// neither does 0x06 (private data) on its own. A DVB stream carries AC-3, E-AC-3 and DTS as stream type 0x06 with
// the descriptor of EN 300 468 that names the codec (TS 101 154), so 0x06 with such a descriptor is sound, as the
// stream types 0x81, 0x87 and 0x82 are (TrackInfo::m_soundDescriptor). A Matroska file by the first letter of the
// codec ID, A_ for sound and V_ for video, as its specification names them. Other containers declare nothing here.
// The descriptor of a stream type 0x06 track that names its sound codec, or 0. A subtitling or teletext descriptor
// beside it says what the track is: a DVB subtitle track whose PMT entry also held an AC-3 descriptor was probed as
// AC-3 only and listed "Can't detect stream type".
static int soundDescriptorOf(const TrackInfo& track)
{
    if (track.m_dvbDescriptor == static_cast<int>(TSDescriptorTag::DVB_SUBTITLING) ||
        track.m_dvbDescriptor == static_cast<int>(TSDescriptorTag::DVB_TELETEXT))
        return 0;
    return track.m_soundDescriptor;
}

static DeclaredKind declaredKindOf(const AbstractStreamReader::ContainerType containerType, const TrackInfo& track)
{
    using CT = AbstractStreamReader::ContainerType;
    if (containerType == CT::ctTS || containerType == CT::ctM2TS)
    {
        switch (track.m_trackType)
        {
        case 0x03:  // MPEG-1 audio
        case 0x04:  // MPEG-2 audio
        case 0x0F:  // AAC, ADTS
        case 0x11:  // AAC, LATM
        case 0x81:  // AC-3
        case 0x82:  // DTS
        case 0x83:  // TrueHD
        case 0x84:  // E-AC-3
        case 0x85:  // DTS-HD
        case 0x86:  // DTS-HD Master Audio
        case 0x87:  // E-AC-3 (ATSC), DRA (Blu-ray)
        case 0x88:
        case 0xA1:  // E-AC-3, secondary audio of a disc
        case 0xA2:  // DTS-HD, secondary audio of a disc
            return DeclaredKind::Sound;
        case 0x01:  // MPEG-1 video
        case 0x02:  // MPEG-2 video
        case 0x10:  // MPEG-4 Part 2
        case 0x1B:  // H.264
        case 0x20:  // MVC
        case 0x24:  // HEVC
        case 0x25:  // HEVC temporal subset
        case 0x33:  // VVC
        case 0xEA:  // VC-1
            return DeclaredKind::Video;
        case 0x06:  // private data, sound where a descriptor names its codec
            return soundDescriptorOf(track) ? DeclaredKind::Sound : DeclaredKind::None;
        default:
            return DeclaredKind::None;
        }
    }
    if (containerType == CT::ctMKV)
    {
        const auto id = track.m_srcParams.find("srccodecid");
        if (id != track.m_srcParams.end() && strStartWith(id->second, "A_"))
            return DeclaredKind::Sound;
        if (id != track.m_srcParams.end() && strStartWith(id->second, "V_"))
            return DeclaredKind::Video;
    }
    return DeclaredKind::None;
}

// A track found as the other kind than its container declares: detectTrackReader found nothing of the declared kind
// and took what the other kind's probes found, as it always did. That was said nowhere, so a sound track listed as
// AV1 looked like any other video track. Said once for each track and file.
static void warnKindMismatch(const std::string& fileName, const int trackId, const TrackInfo& track,
                             const DeclaredKind declared, const CheckStreamRez& found)
{
    const std::string& program = found.codecInfo.programName;
    const bool sound = strStartWith(program, "A_");
    const bool video = strStartWith(program, "V_");
    if (!(declared == DeclaredKind::Sound && video) && !(declared == DeclaredKind::Video && sound))
        return;
    static std::set<std::pair<std::string, int>> said;
    if (!said.insert({fileName, trackId}).second)
        return;
    std::ostringstream how;
    if (const auto id = track.m_srcParams.find("srccodecid"); id != track.m_srcParams.end() && !id->second.empty())
        how << "codec ID " << id->second;
    else
        how << "stream type 0x" << std::hex << track.m_trackType << std::dec;
    switch (static_cast<StreamType>(track.m_soundDescriptor))
    {
    case StreamType::AUDIO_AC3:
        how << " with the AC-3 descriptor";
        break;
    case StreamType::AUDIO_EAC3_ATSC:
        how << " with the E-AC-3 descriptor";
        break;
    case StreamType::AUDIO_DTS:
        how << " with the DTS descriptor";
        break;
    case StreamType::AUDIO_AAC:
        how << " with the AAC descriptor";
        break;
    default:
        break;
    }
    const char* declaredAs = declared == DeclaredKind::Sound ? "sound" : "video";
    const char* holds = video ? "video" : "sound";
    LTRACE(LT_WARN, 2,
           "Warning: track " << trackId << " of " << fileName << " is declared as " << declaredAs << " (" << how.str()
                             << "), but holds " << holds << " (" << found.codecInfo.displayName << "); it is muxed as "
                             << holds << ".");
}

DetectStreamRez METADemuxer::DetectStreamReader(const BufferedReaderManager& readManager, const string& fileName,
                                                bool calcDuration, const std::vector<std::string>& laterFiles)
{
    AVChapters chapters;
    int64_t fileDuration = 0;
    int64_t videoStart = 0, videoEnd = 0;
    std::string writingApp;
    vector<CheckStreamRez> streams, Vstreams;
    std::unique_ptr<AbstractDemuxer> demuxer;
    auto unquoted = unquoteStr(fileName);
    string fileExt = strToLowerCase(extractFileExt(unquoted));
    checkNameMatchesContent(unquoted, fileExt);
    AbstractStreamReader::ContainerType containerType = AbstractStreamReader::ContainerType::ctNone;
    CLPIParser clpi;
    bool clpiParsed = false;
    if (fileExt == "m2ts" || fileExt == "mts" || fileExt == "ssif")
    {
        demuxer = std::make_unique<TSDemuxer>(readManager, "");
        containerType = AbstractStreamReader::ContainerType::ctM2TS;
        string clpiFileName = findBluRayFile(extractFileDir(unquoted), "CLIPINF", extractFileName(unquoted) + ".clpi");
        if (!clpiFileName.empty())
            clpiParsed = clpi.parse(clpiFileName.c_str());
    }
    else if (fileExt == "ts" || fileExt == "m2t")
    {
        demuxer = std::make_unique<TSDemuxer>(readManager, "");
        containerType = AbstractStreamReader::ContainerType::ctTS;
    }
    else if (fileExt == "vob" || fileExt == "mpg" || fileExt == "mpeg")
    {
        demuxer = std::make_unique<ProgramStreamDemuxer>(readManager);
        containerType = AbstractStreamReader::ContainerType::ctVOB;
    }
    else if (fileExt == "evo")
    {
        demuxer = std::make_unique<ProgramStreamDemuxer>(readManager);
        containerType = AbstractStreamReader::ContainerType::ctEVOB;
    }
    else if (fileExt == "mkv" || fileExt == "mka" || fileExt == "mks" || fileExt == "mk3d" || fileExt == "webm")
    {
        demuxer = std::make_unique<MatroskaDemuxer>(readManager);
        containerType = AbstractStreamReader::ContainerType::ctMKV;
    }
    else if (fileExt == "mp4" || fileExt == "m4v" || fileExt == "m4a" || fileExt == "mov")
    {
        demuxer = std::make_unique<MovDemuxer>(readManager);
        containerType = AbstractStreamReader::ContainerType::ctMOV;
    }

    if (demuxer)
    {
        uint32_t fileBlockSize = demuxer->getFileBlockSize();

        if (const auto ts = dynamic_cast<TSDemuxer*>(demuxer.get()))
            ts->setLaterFiles(laterFiles);
        demuxer->openFile(fileName);
        int64_t discardedSize = 0;
        DemuxedData demuxedData;
        map<int32_t, TrackInfo> acceptedPidMap;
        demuxer->getTrackList(acceptedPidMap);
        PIDSet acceptedPidSet;
        for (const auto& itr : acceptedPidMap) acceptedPidSet.insert(itr.first);
        for (auto& itr : demuxedData)
        {
            StreamData& vect = itr.second;
            vect.reserve(fileBlockSize);
        }

        bool wholeTracks = false;  // the file was read to its end: the data of each track is all of it
        for (unsigned i = 0; i < DETECT_STREAM_BUFFER_SIZE / fileBlockSize; i++)
            if (demuxer->simpleDemuxBlock(demuxedData, acceptedPidSet, discardedSize) == BufferedReader::DATA_EOF)
                wholeTracks = true;
        readLateTracks(demuxer.get(), demuxedData, acceptedPidMap, discardedSize);
        // The Stream delay of the sound is taken against the picture the video reader shows first, as the mux takes it
        // (tsTrackStart): an HEVC stream that begins with a random access picture is timed from that picture
        if (auto* ts = dynamic_cast<TSDemuxer*>(demuxer.get()))
        {
            const auto video = acceptedPidMap.find(ts->firstVideoPid());
            const auto data = demuxedData.find(ts->firstVideoPid());
            ts->setVideoTimedFromFirstPes(video != acceptedPidMap.end() &&
                                          video->second.m_trackType == static_cast<int>(StreamType::VIDEO_H265) &&
                                          data != demuxedData.end() && hevcStartsAtRandomAccess(data->second) == 1);
        }

        for (auto& itr : demuxedData)
        {
            StreamData& vect = itr.second;
            const DeclaredKind declared = declaredKindOf(containerType, acceptedPidMap[itr.first]);
            // A stream type 0x06 track whose descriptor names its sound is read as the stream type of that codec:
            // a damaged AC-3, E-AC-3 or DTS track of a DVB stream is then found as it is under 0x81, 0x87 or 0x82
            // (the readers' declaredByContainer)
            const int soundDescriptor = soundDescriptorOf(acceptedPidMap[itr.first]);
            CheckStreamRez trackRez =
                detectTrackReader(vect.data(), static_cast<int>(vect.size()), containerType,
                                  soundDescriptor ? soundDescriptor : acceptedPidMap[itr.first].m_trackType, itr.first,
                                  declared, wholeTracks);
            warnKindMismatch(unquoted, itr.first, acceptedPidMap[itr.first], declared, trackRez);
            if (!trackRez.codecInfo.programName.empty())
            {
                if (trackRez.codecInfo.programName[0] != 'S')
                    trackRez.delay = demuxer->getTrackDelay(itr.first);
            }
            trackRez.trackID = itr.first;
            trackRez.lang = acceptedPidMap[trackRez.trackID].m_lang;
            {
                const auto& srcParams = acceptedPidMap[trackRez.trackID].m_srcParams;
                if (const auto name = srcParams.find("srcname"); name != srcParams.end())
                    trackRez.name = name->second;
                if (const auto flags = srcParams.find("srcflags"); flags != srcParams.end())
                    trackRez.flags = flags->second;
                if (const auto id = srcParams.find("srccodecid"); id != srcParams.end())
                    trackRez.codecId = id->second;
                // What the container says it cannot give is not guessed from its bytes: an encrypted MP4 video track
                // was listed as Opus and its audio as the codec it had, from noise
                if (const auto what = srcParams.find("srcnotsupported"); what != srcParams.end())
                {
                    trackRez.notSupported = what->second;
                    trackRez.codecInfo = CodecInfo();
                    trackRez.streamDescr.clear();
                }
            }
            // ASS, SSA and WebVTT from a Matroska file go through the SRT reader, which called every one of them
            // "SRT text subtitles". The listing names what the file holds. Stream type and Stream ID stay SRT and
            // S_TEXT/UTF8: the window and the meta file take the track by those.
            if (trackRez.codecInfo.codecID == CODEC_S_SRT)
            {
                const std::string& id = trackRez.codecId;
                if (id == MATROSKA_CODEC_ID_TEXT_ASS)
                    trackRez.streamDescr = "ASS text subtitles";
                else if (id == MATROSKA_CODEC_ID_TEXT_SSA)
                    trackRez.streamDescr = "SSA text subtitles";
                else if (id == MATROSKA_CODEC_ID_TEXT_WEBVTT || isWebmVttText(id))
                    trackRez.streamDescr = "WebVTT text subtitles";
            }
            if (clpiParsed)
            {
                map<int, CLPIStreamInfo>::const_iterator clpiStream = clpi.m_streamInfo.find(itr.first);
                if (clpiStream != clpi.m_streamInfo.end())
                    trackRez.lang = clpiStream->second.language_code;
            }
            trackRez.lang = applyLangCodeStyle(trackRez.lang);

            if (dynamic_cast<TSDemuxer*>(demuxer.get()))
                trackRez.containerStreamType = acceptedPidMap[itr.first].m_trackType;
            trackRez.containerDvbDescriptor = acceptedPidMap[itr.first].m_dvbDescriptor;

            if (strStartWith(trackRez.codecInfo.programName, "A_") && dynamic_cast<TSDemuxer*>(demuxer.get()))
            {
                if (trackRez.trackID >= 0x1A00)
                    trackRez.isSecondary = true;
            }

            // If video stream description lacks FPS or says "not found", try to get it from the container
            if (strStartWith(trackRez.codecInfo.programName, "V_"))
            {
                const size_t fpsNotFoundPos = trackRez.streamDescr.find("Frame rate: not found");
                const bool hasFpsField = trackRez.streamDescr.find("Frame rate:") != string::npos;
                if (fpsNotFoundPos != string::npos || !hasFpsField)
                {
                    double containerFps = correctFps(demuxer->getTrackFps(itr.first));
                    // A stream that names no rate in a transport stream: the times of its pictures, as the mux takes
                    // them. A field coded stream, whose times may be those of its fields, gets a rate only where the
                    // demuxer can tell its fields from its frames (H.264); an interlaced one coded as frames (MBAFF)
                    // has the time of a frame for each picture.
                    const size_t res = trackRez.streamDescr.find("Resolution: ");
                    const size_t scan =
                        res == string::npos ? res : trackRez.streamDescr.find_first_not_of("0123456789:x", res + 12);
                    const bool interlaced = scan != string::npos && trackRez.streamDescr[scan] == 'i';
                    if (containerFps <= 0.0)
                        containerFps =
                            correctFps(demuxer->getTrackPacketFps(itr.first, interlaced && !trackRez.framesOnly));
                    if (containerFps > 0.0)
                    {
                        if (fpsNotFoundPos != string::npos)
                            trackRez.streamDescr.replace(fpsNotFoundPos, 21,
                                                         "Frame rate: " + doubleToStr(containerFps));
                        else
                            trackRez.streamDescr += "  Frame rate: " + doubleToStr(containerFps);
                    }
                }
            }

            if (strStartWith(trackRez.codecInfo.programName, "V_"))
                addTrack(Vstreams, trackRez);
            else
                addTrack(streams, trackRez);
        }
        chapters = demuxer->getChapters();
        if (calcDuration)
        {
            fileDuration = demuxer->getFileDurationNano();
            // Where a join would put the file after this one (see getChapters).
            if (!demuxer->getVideoSpanNano(videoStart, videoEnd))
                videoStart = videoEnd = 0;
        }
        if (const auto mkv = dynamic_cast<MatroskaDemuxer*>(demuxer.get()))
            writingApp = mkv->writingApplication();
        // A Matroska file played by its ordered edition or with the files linked after it is muxed as that timeline,
        // so its length, its marks and its video span are the timeline's: the window takes the chapters it writes
        // into the meta from the marks.
        if (dynamic_cast<MatroskaDemuxer*>(demuxer.get()))
        {
            std::string how;
            const std::vector<MatroskaPlayPart> parts = MatroskaDemuxer::playTimeline(fileName, how);
            if (!parts.empty())
            {
                LTRACE(LT_INFO, 2, "Plays as: " << how);
                chapters.clear();
                if (parts.front().range.startNs >= 0)
                {
                    int64_t length = 0;
                    for (const MatroskaPlayPart& part : parts)
                    {
                        chapters.emplace_back(part.range.offsetNs, part.range.name);
                        length = part.range.offsetNs;
                    }
                    // the end of the last part: the timeline's length as playTimeline added it up
                    const JoinRange& last = parts.back().range;
                    MatroskaDemuxer lastPart(readManager);
                    lastPart.openFile(parts.back().file);
                    const int64_t lastEnd = last.endNs != INT64_MAX ? last.endNs : lastPart.getFileDurationNano();
                    length += (std::max)(int64_t{0}, lastEnd - last.startNs);
                    fileDuration = length;
                    videoStart = 0;
                    videoEnd = length;
                }
                else
                {
                    std::vector<std::string> files;
                    for (const MatroskaPlayPart& part : parts) files.push_back(part.file);
                    int64_t end = 0;
                    chapters = joinedMatroskaChapters(readManager, files, &end);
                    if (end > 0)
                    {
                        fileDuration = end - videoStart;
                        videoEnd = end;
                    }
                }
            }
        }
    }
    else
    {
        File file;
        containerType = AbstractStreamReader::ContainerType::ctNone;
        if (!file.open(fileName.c_str(), File::ofRead))
            return {};
        auto tmpBuffer = std::make_unique<uint8_t[]>(DETECT_STREAM_BUFFER_SIZE);
        int len = file.read(tmpBuffer.get(), DETECT_STREAM_BUFFER_SIZE);
        if (fileExt == "sup")
            containerType = AbstractStreamReader::ContainerType::ctSUP;
        else if (fileExt == "pcm" || fileExt == "lpcm" || fileExt == "wav" || fileExt == "w64")
            containerType = AbstractStreamReader::ContainerType::ctLPCM;
        // A WebVTT file is read by the SRT reader (SRTStreamReader::skipNonCueLine); it was listed as "Can't detect"
        else if (fileExt == "srt" || fileExt == "vtt")
            containerType = AbstractStreamReader::ContainerType::ctSRT;
        CheckStreamRez trackRez = detectTrackReader(tmpBuffer.get(), len, containerType, 0, 0);
        // DTS in 16-bit little-endian byte order, which tsMuxeR does not read, was "Can't detect stream type"
        if (trackRez.codecInfo.programName.empty() && len > 0 &&
            DTSStreamReader::isLittleEndianCore(tmpBuffer.get(), tmpBuffer.get() + len))
            trackRez.notSupported = "DTS in 16-bit little-endian byte order";

        if (strStartWith(trackRez.codecInfo.programName, "V_"))
            addTrack(Vstreams, trackRez);
        else
            addTrack(streams, trackRez);
    }
    Vstreams.insert(Vstreams.end(), streams.begin(), streams.end());

    DetectStreamRez rez;
    rez.chapters = chapters;
    rez.fileDurationNano = fileDuration;
    rez.videoStartNano = videoStart;
    rez.videoEndNano = videoEnd;
    rez.writingApp = writingApp;
    rez.streams = Vstreams;
    return rez;
}

void METADemuxer::addTrack(vector<CheckStreamRez>& rez, CheckStreamRez trackRez)
{
    if (trackRez.codecInfo.codecID == h264DepCodecInfo.codecID && trackRez.multiSubStream)
    {
        // split combined MVC/AVC track to substreams
        trackRez.subTrack = 1;  // the dependent view, which is what this codec id names
        rez.push_back(trackRez);

        trackRez.codecInfo = h264CodecInfo;
        trackRez.subTrack = 2;
        const size_t postfixPos = trackRez.streamDescr.find("3d-pg");
        if (postfixPos != string::npos)
            trackRez.streamDescr = trackRez.streamDescr.substr(0, postfixPos);

        rez.push_back(trackRez);
    }
    else if (trackRez.codecInfo.codecID == hevcCodecInfo.codecID && trackRez.multiSubStream)
    {
        // A dual layer Dolby Vision disc stored as ONE Matroska track: the base layer, the
        // enhancement layer wrapped in unspecified NALs, and one RPU per picture. Listing it as
        // two entries is what lets it be separated again without hand writing a meta file, since
        // the interface already turns a sub track number into subTrack= on its own.
        //
        // BEWARE THE ORDER. The numbering above names the DEPENDENT view 1; here 1 is the BASE
        // layer, which is the opposite way round. Getting it wrong swaps the layers and authors a
        // disc with the enhancement layer as the picture, and nothing downstream would notice.
        const std::string full = trackRez.streamDescr;
        const size_t dvPos = full.find(" Dolby Vision");

        trackRez.subTrack = HevcDolbyVisionFilter::BL_SUB_TRACK;
        // The base layer alone carries neither the RPU nor the enhancement layer, so it must not
        // keep a description that claims them.
        if (dvPos != std::string::npos)
            trackRez.streamDescr = full.substr(0, dvPos);
        trackRez.streamDescr += " (base layer)";
        rez.push_back(trackRez);

        trackRez.subTrack = HevcDolbyVisionFilter::EL_SUB_TRACK;
        // Described from the ENHANCEMENT layer's own parameter sets when the reader found them,
        // because the two layers are different pictures and `full` is the base layer's: a profile
        // 7 disc pairs 3840x2160 with 1920x1080. Falls back to `full` when they were not found,
        // which is what this always showed.
        const std::string& elFull = trackRez.elStreamDescr.empty() ? full : trackRez.elStreamDescr;
        trackRez.streamDescr = elFull + " (enhancement layer)";
        rez.push_back(trackRez);
    }
    else
        rez.push_back(trackRez);
}

// A frame rate in a meta file may be written either way round: 23.976 or 24000/1001. Only the
// decimal used to work. strToDouble stops at the slash, so the fraction became its numerator and
// a disc asked for 24000/1001 was built at 24000 frames a second, silently.
//
// The window has always split the fraction itself, which is why this only ever caught a meta
// written by hand.
//
// The same was still true of every other way to write it: fps=24000:1001 was 24000 frames a second, fps=23,976 was 23,
// and fps=abc was 0, all without a word. A frame rate is now a number with an optional decimal point, or a fraction
// written with "/" or ":", above 0 and up to 1000; anything else is refused.
static double parseFpsValue(const std::string& text)
{
    const std::string t = trimStr(text);
    auto number = [](const std::string& s, double& value)
    {
        if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos ||
            std::count(s.begin(), s.end(), '.') > 1 || s.find_first_of("0123456789") == std::string::npos)
            return false;
        value = strToDouble(s.c_str());
        return true;
    };
    double fps = 0;
    const size_t bar = t.find_first_of("/:");
    bool ok;
    if (bar == std::string::npos)
        ok = number(t, fps);
    else
    {
        double num = 0, den = 0;
        ok = number(t.substr(0, bar), num) && number(t.substr(bar + 1), den) && den > 0;
        if (ok)
            fps = num / den;
    }
    // At least one picture a second: fps=0.001 was taken, and a picture every 1000 s passes the 26.5 hour clock of a
    // transport stream after 96 pictures, where the times wrap round
    if (!ok || !(fps >= 1) || fps > 1000)
        THROW(ERR_COMMON, "fps=" << text
                                 << " is not a frame rate. Write it as a number such as 25 or 23.976, or as a fraction "
                                    "such as 24000/1001, from 1 up to 1000.")
    return fps;
}

// stretch= of an audio track: a number with an optional decimal point, or a fraction written with "/", such as 25/24.
// Only a 0 below the bar was refused: stretch=0 and text such as abc gave every frame the same time, stretch=-1 made
// the times run backwards and stretch=1000 spread 10 s of sound over 2.7 hours, all without a word. The factor is meant
// for the gap between two frame rates (25/24, 24000/25025), so it is kept from 0.5 up to 2.
static double parseStretchValue(const std::string& text)
{
    const std::string t = trimStr(text);
    auto number = [](const std::string& s, double& value)
    {
        if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos ||
            std::count(s.begin(), s.end(), '.') > 1 || s.find_first_of("0123456789") == std::string::npos)
            return false;
        value = strToDouble(s.c_str());
        return true;
    };
    double stretch = 0;
    const size_t bar = t.find('/');
    bool ok;
    if (bar == std::string::npos)
        ok = number(t, stretch);
    else
    {
        double num = 0, den = 0;
        ok = number(t.substr(0, bar), num) && number(t.substr(bar + 1), den);
        if (ok && den == 0.0)
            THROW(ERR_COMMON, "Second argument at stretch parameter can not be 0")
        if (ok)
            stretch = num / den;
    }
    if (!ok || !(stretch >= 0.5) || stretch > 2.0)
        THROW(ERR_COMMON, "stretch=" << text
                                     << " is not a factor tsMuxeR can stretch the sound by. Write it as a number such "
                                        "as 1.04 or as a fraction such as 25/24, from 0.5 up to 2.")
    return stretch;
}

// video-width= and video-height= of a subtitle track: a whole number of pixels. A negative size wrapped round to a
// large one (-720 became 64816, and the subtitles were drawn for 1920x1080), and text such as abc was read as 0, both
// in silence.
static uint16_t parsePictureSize(const char* name, const std::string& text)
{
    const std::string t = trimStr(text);
    const bool digits = !t.empty() && t.size() <= 5 && t.find_first_not_of("0123456789") == std::string::npos;
    const long value = digits ? strtol(t.c_str(), nullptr, 10) : 0;
    if (value < 1 || value > 65535)
        THROW(ERR_COMMON, name << "=" << text
                               << " is not a picture size. Write the size of the video in pixels, a whole number "
                                  "from 1 up to 65535, such as 1920.")
    return static_cast<uint16_t>(value);
}

// Subtitles are drawn on the picture that subtitles have on a disc for the video's width and frame rate, as they always
// were. Where that picture has fewer lines than the video (720x576 at 23.976 fps gets 720x480, portrait video
// 1280x720, 720x1080 gets 720x576) a player stretches the subtitles over the video, and that happened without a word.
// Video wider than 1920 is left out: on a UHD disc the subtitles are 1920x1080 by rule.
static void warnSubtitleCanvas(const std::string& what, const uint16_t width, const uint16_t height, const double fps)
{
    uint16_t canvasWidth = 0;
    uint16_t canvasHeight = 0;
    text_subtitles::TextToPGSConverter::enlargeCrop(width, height, fps, &canvasWidth, &canvasHeight);
    if (width <= 1920 && height > canvasHeight + 8)
        LTRACE(LT_WARN, 2,
               "Warning: " << what << " are drawn on a picture of " << canvasWidth << "x" << canvasHeight
                           << ", the size subtitles have on a disc for video " << width << " pixels wide at " << fps
                           << " fps. The video is " << width << "x" << height
                           << ", so a player stretches the subtitles over it.");
}

// fadein-time= and fadeout-time=: seconds, 0 or more. A negative fade made the subtitle invisible for all of its time,
// and text such as abc was taken as no fade, both in silence.
static float parseFadeTime(const char* name, const std::string& text)
{
    const std::string t = trimStr(text);
    const bool number = !t.empty() && t.find_first_not_of("0123456789.") == std::string::npos &&
                        std::count(t.begin(), t.end(), '.') <= 1 && t.find_first_of("0123456789") != std::string::npos;
    const double value = number ? strToDouble(t.c_str()) : -1.0;
    if (!(value >= 0.0) || value > 86400.0)
        THROW(ERR_COMMON,
              name << "=" << text << " is not a fade time. Write it in seconds, from 0 up to 86400, such as 0.25.")
    return static_cast<float>(value);
}

// font-size=, font-border= and line-spacing= of a drawn text subtitle. A font size of 0 or less drew nothing to see
// and the mux said it had succeeded; a negative border drew the outline wrong and a negative spacing drew the lines
// over each other; text such as abc was read as 0. All of them in silence. A size is a whole number of pixels from 1; a
// border and a spacing are numbers from 0.
static double parseFontNumber(const char* name, const std::string& text, const bool size)
{
    const std::string t = trimStr(text);
    const bool number = !t.empty() && t.size() <= 9 &&
                        t.find_first_not_of(size ? "0123456789" : "0123456789.") == std::string::npos &&
                        std::count(t.begin(), t.end(), '.') <= 1 && t.find_first_of("0123456789") != std::string::npos;
    const double value = number ? strToDouble(t.c_str()) : -1.0;
    if (!(value >= (size ? 1.0 : 0.0)))
    {
        if (size)
            THROW(ERR_COMMON, name << "=" << text
                                   << " is not a font size. Write the size in pixels, a whole number from 1, such as "
                                      "48.")
        THROW(ERR_COMMON, name << "=" << text << " is not a number of 0 or more, such as "
                               << (std::string(name) == "line-spacing" ? "1.2." : "2."))
    }
    return value;
}

// 3d-plane=: the number of a plane (offset sequence) of the MVC video, a whole number from 0, or 255 for none, which
// the window writes for "zero". Read as a byte, 300 became plane 44, abc and 1.5 planes 0 and 1, and -1 no plane, all
// without a word. Whether the MVC video has the plane is checked once every line is read (checkSubtitlePlanes).
uint8_t METADemuxer::parse3dPlane(const std::string& text)
{
    const std::string t = trimStr(text);
    const bool digits = !t.empty() && t.size() <= 3 && t.find_first_not_of("0123456789") == std::string::npos;
    const long value = digits ? strtol(t.c_str(), nullptr, 10) : -1;
    if (value < 0 || value > 255)
        THROW(ERR_COMMON, "3d-plane=" << text
                                      << " is not a 3D plane. Write the number of a plane of the MVC video, a whole "
                                         "number from 0 (its listing gives how many it has as 3d-pg-planes), or 255 "
                                         "for none.")
    return static_cast<uint8_t>(value);
}

// font-color=: 0xAARRGGBB or xAARRGGBB in hexadecimal, #RRGGBB or #AARRGGBB as in a <font color=> tag, or a decimal
// number. A value in no such form was read as 0: #FF0000 and FF0000 drew black text, without a word. Without 0x, x or
// # a value is decimal, as it always was, so FF0000 is refused rather than guessed.
static uint32_t parseFontColor(const std::string& text)
{
    const std::string t = trimStr(unquoteStr(trimStr(text)));
    bool ok;
    uint64_t value = 0;
    size_t prefix = 0;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        prefix = 2;
    else if (!t.empty() && (t[0] == 'x' || t[0] == 'X' || t[0] == '#'))
        prefix = 1;
    if (prefix)
    {
        const std::string hex = t.substr(prefix);
        ok = !hex.empty() && hex.size() <= 8 && (t[0] != '#' || hex.size() == 6 || hex.size() == 8) &&
             std::all_of(hex.begin(), hex.end(), [](const unsigned char c) { return isxdigit(c) != 0; });
        if (ok)
            value = strtoull(hex.c_str(), nullptr, 16);
    }
    else
    {
        ok = !t.empty() && t.size() <= 10 && t.find_first_not_of("0123456789") == std::string::npos;
        if (ok)
            value = strtoull(t.c_str(), nullptr, 10);
        ok = ok && value <= 0xFFFFFFFFu;
    }
    if (!ok)
        THROW(ERR_COMMON, "font-color=" << text
                                        << " is not a colour. Write it in hexadecimal as 0xRRGGBB or #RRGGBB, such "
                                           "as 0xFF0000 for red, with the alpha in front as 0xAARRGGBB, or as a "
                                           "decimal number.")
    return static_cast<uint32_t>(value);
}

CheckStreamRez METADemuxer::detectTrackReader(uint8_t* tmpBuffer, int len,
                                              AbstractStreamReader::ContainerType containerType, int containerDataType,
                                              int containerStreamIndex, const DeclaredKind declared,
                                              const bool wholeTrack)
{
    CheckStreamRez rez;

    auto pgsReader = std::make_unique<PGSStreamReader>();
    rez = pgsReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto srtReader = std::make_unique<SRTStreamReader>();
    rez = srtReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    if (len == 0)
        return rez;

    // VP8 and VP9 are known only by the name a Matroska source gives them: neither has start codes or any mark of its
    // own. Their reader is the only one asked. The other probes found an AV1 sequence header in VP8 and VP9 frames
    // and listed both as AV1, with a made up picture size, and a mux wrote them out as such.
    if (containerDataType == TRACKTYPE_VP8 || containerDataType == TRACKTYPE_VP9)
        return VpxStreamReader(containerDataType == TRACKTYPE_VP9).checkStream(tmpBuffer, len);
    // An IVF file, the elementary file of VP8, VP9 and AV1, says what it holds in its header.
    if (len >= 32 && memcmp(tmpBuffer, "DKIF", 4) == 0)
    {
        for (const bool vp9 : {false, true})
            if (rez = VpxStreamReader(vp9).checkStream(tmpBuffer, len); rez.codecInfo.codecID)
                return rez;
        if (memcmp(tmpBuffer + 8, "AV01", 4) == 0)
            return AV1StreamReader().checkStream(tmpBuffer, len);
    }
    // Vorbis the same way: its packets have no sync code to find.
    if (containerDataType == TRACKTYPE_VORBIS)
        return VorbisStreamReader().checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (containerDataType == TRACKTYPE_ALAC)
        return AlacStreamReader().checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    // MPEG-4 Part 2 and ProRes from an MP4 file: the MP4 reader says what they are, and their data has no mark of
    // its own another probe could take for something else
    if (containerDataType == TRACKTYPE_MP4V || containerDataType == TRACKTYPE_PRORES)
        return VideoCopyStreamReader::checkStream(tmpBuffer, len, containerDataType == TRACKTYPE_PRORES);

    // The container says what the track is, and that was being thrown away. Every video probe
    // below runs before the MPEG audio one, which is alone among the audio readers in sitting
    // after them, so a track a PMT declares as stream type 0x04 reached the AV1 detector first.
    // On a real broadcast capture that detector found a sequence header inside MP2 audio and
    // answered "AV1, 68x28, Profile 1, Level 30": a confident wrong answer with a resolution
    // attached, which is worse than refusing.
    //
    // Where the container names MPEG audio, ask that reader first. A declaration that turns out
    // to be wrong still falls through to every probe below, so nothing found before is lost.
    if (containerDataType == static_cast<int>(StreamType::AUDIO_MPEG1) ||
        containerDataType == static_cast<int>(StreamType::AUDIO_MPEG2))
    {
        auto declaredMpegAudio = std::make_unique<MpegAudioStreamReader>();
        rez = declaredMpegAudio->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
        if (rez.codecInfo.codecID)
            return rez;
    }

    // The same for video. Every audio probe below runs before the MPEG-2 video one, and on DVD video in a Matroska
    // the FLAC probe found frames in the picture data by chance: the track was listed and muxed as FLAC (#27). Where
    // the container names a video codec, that reader is asked first; a wrong name still falls through to the rest.
    switch (static_cast<StreamType>(containerDataType))
    {
    case StreamType::VIDEO_MPEG2:
        rez = std::make_unique<MPEG2StreamReader>()->checkStream(tmpBuffer, len);
        break;
    case StreamType::VIDEO_H264:
        rez = std::make_unique<H264StreamReader>()->checkStream(tmpBuffer, len);
        break;
    case StreamType::VIDEO_H265:
        rez = std::make_unique<HEVCStreamReader>()->checkStream(tmpBuffer, len);
        break;
    case StreamType::VIDEO_H266:
        rez = std::make_unique<VVCStreamReader>()->checkStream(tmpBuffer, len);
        break;
    case StreamType::VIDEO_VC1:
        rez = std::make_unique<VC1StreamReader>()->checkStream(tmpBuffer, len);
        break;
    default:
        break;
    }
    if (rez.codecInfo.codecID)
        return rez;

    // ** THE KIND THE CONTAINER DECLARES IS ASKED FIRST. ** The probes ran in one order for every track, sound and
    // pictures interleaved, and the first that answered decided: a transport stream track of stream type 0x87 (E-AC-3)
    // or a Matroska A_EAC3 or A_DTS track could be listed as AV1, one of type 0x82 (DTS) as MPEG-2 video, an A_AC3 or
    // A_DTS track as H.264, and a track of type 0x1B (H.264) as AC-3, DTS or MPEG audio, whenever a probe of the other
    // kind ran first and took the data for its own. The probes of the declared kind now run first, in the order they
    // always ran, so the data still decides within a kind (E-AC-3 under the AC-3 stream type, AC-3 in a track declared
    // AAC). Only when none of them answers are the probes of the other kind asked, as they always were: a declaration
    // that is wrong still finds what it found before, and DetectStreamReader names the mismatch in a warning. Nothing
    // declared (a file of one stream, stream type 0x80, 0x06 without a sound descriptor, a container that gives no
    // type): one pass, as before.
    enum Probe
    {
        LPCM,
        H264,
        DTS,
        AC3,
        MLP,
        AAC,
        FLAC,
        OPUS,
        VC1,
        HEVC,
        VVC,
        MPEG2,
        AV1,
        MPEG_AUDIO
    };
    static constexpr Probe ORDER[] = {LPCM, H264, DTS,  AC3, MLP,   AAC, FLAC,
                                      OPUS, VC1,  HEVC, VVC, MPEG2, AV1, MPEG_AUDIO};
    const auto probe = [&](const Probe p) -> CheckStreamRez
    {
        switch (p)
        {
        case LPCM:
            return std::make_unique<LPCMStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                     containerStreamIndex);
        case H264:
        {
            // The probe takes the last unit of its data as cut off unless the data ends the track, so a track of one
            // picture, whose slice is its last unit, was "Can't detect stream type" though a typed line muxes it
            auto h264 = std::make_unique<H264StreamReader>();
            if (wholeTrack)
                h264->setProbeHoldsAll();
            return h264->checkStream(tmpBuffer, len);
        }
        case DTS:
            return std::make_unique<DTSStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                    containerStreamIndex);
        case AC3:
            return std::make_unique<AC3StreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                    containerStreamIndex);
        case MLP:
            return std::make_unique<MLPStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                    containerStreamIndex);
        case AAC:
            return std::make_unique<AACStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                    containerStreamIndex);
        case FLAC:
            return std::make_unique<FLACStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                     containerStreamIndex);
        case OPUS:
            return std::make_unique<OpusStreamReader>()->checkStream(tmpBuffer, len, containerType, containerDataType,
                                                                     containerStreamIndex);
        case VC1:
            return std::make_unique<VC1StreamReader>()->checkStream(tmpBuffer, len);
        case HEVC:
            return std::make_unique<HEVCStreamReader>()->checkStream(tmpBuffer, len);
        case VVC:
            return std::make_unique<VVCStreamReader>()->checkStream(tmpBuffer, len);
        case MPEG2:
            return std::make_unique<MPEG2StreamReader>()->checkStream(tmpBuffer, len);
        case AV1:
            return std::make_unique<AV1StreamReader>()->checkStream(tmpBuffer, len);
        case MPEG_AUDIO:
        default:
            return std::make_unique<MpegAudioStreamReader>()->checkStream(tmpBuffer, len, containerType,
                                                                          containerDataType, containerStreamIndex);
        }
    };
    const auto isVideoProbe = [](const Probe p)
    { return p == H264 || p == VC1 || p == HEVC || p == VVC || p == MPEG2 || p == AV1; };
    for (const bool declaredPass : {true, false})
        for (const Probe p : ORDER)
        {
            const bool ofDeclaredKind =
                declared == DeclaredKind::None || isVideoProbe(p) == (declared == DeclaredKind::Video);
            if (ofDeclaredKind != declaredPass)
                continue;
            rez = probe(p);
            if (rez.codecInfo.codecID)
                return rez;
        }

    auto supReader = std::make_unique<DVBSubStreamReader>();
    rez = supReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    return rez;
}

VideoAspectRatio arNameToCode(const string& arName)
{
    if (arName == "9x16" || arName == "16x9" || arName == "9:16" || arName == "16:9")
        return VideoAspectRatio::AR_16_9;
    if (arName == "3:4" || arName == "4:3" || arName == "4x3" || arName == "3x4")
        return VideoAspectRatio::AR_3_4;
    if (arName == "Square" || arName == "VGA" || arName == "1:1" || arName == "1x1" || arName == "1:1 (Square)")
        return VideoAspectRatio::AR_VGA;
    if (arName == "WIDE" || arName == "1x2,21" || arName == "1x2.21" || arName == "1:2.21" || arName == "1:2,21" ||
        arName == "2.21:1" || arName == "2,21:1" || arName == "2.21x1" || arName == "2,21x1")
        return VideoAspectRatio::AR_221_100;
    // Any other value kept the stream's own shape without a word: ar=2.35:1 on a 16:9 stream gave 16:9, as did
    // ar=bogus and ar=0:0. These four are the shapes an MPEG-2 sequence header and a disc can give.
    THROW(ERR_COMMON, "ar=" << arName
                            << " is not an aspect ratio tsMuxeR can give the video. The values are 1:1, 4:3, 16:9 "
                               "and 2.21:1, also written with x, such as 16x9.")
}

// A value the help does not list (pipCorner=Middle, pipScale=2/3) was taken as BottomLeft or as no scale, without a
// word
PIPParams::PipCorner pipCornerFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));
    if (v == "topleft")
        return PIPParams::PipCorner::TopLeft;
    if (v == "topright")
        return PIPParams::PipCorner::TopRight;
    if (v == "bottomright")
        return PIPParams::PipCorner::BottomRight;
    if (v == "bottomleft")
        return PIPParams::PipCorner::BottomLeft;
    THROW(ERR_COMMON, "pipCorner=" << value
                                   << " is not a corner. The corners are TopLeft, TopRight, BottomRight and "
                                      "BottomLeft.")
}

int pipScaleFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));

    if (v == "1")
        return 1;
    if (v == "1/2" || v == "0.5")
        return 2;
    if (v == "1/4" || v == "0.25")
        return 3;
    if (v == "1.5")
        return 4;
    if (v == "fullscreen")
        return 5;
    THROW(ERR_COMMON,
          "pipScale=" << value << " is not a scale a disc has. The scales are 1, 1/2, 1/4, 1.5 and fullScreen.")
}

// pipHOffset, pipVOffset and pipLumma were read up to their first other character and written into fields of 12 and 8
// bits, where -1 became 4095 and a pipLumma of 256 became 0, without a word. They are whole numbers within the field.
static int pipNumber(const std::string& name, const std::string& value, const int most, const char* meaning)
{
    const std::string v = trimStr(value);
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos || v.size() > 5 ||
        strToInt32(v.c_str()) > most)
        THROW(ERR_COMMON,
              name << "=" << value << " is not " << meaning << ": write a whole number from 0 to " << most << ".")
    return strToInt32(v.c_str());
}

// Whether two names are one existing file, also when they differ in case, slashes or a relative part
static bool sameExistingFile(const std::string& a, const std::string& b)
{
    std::error_code ec;
    const std::filesystem::path pa(std::u8string(a.begin(), a.end()));
    const std::filesystem::path pb(std::u8string(b.begin(), b.end()));
    return std::filesystem::equivalent(pa, pb, ec) && !ec;
}

AbstractStreamReader* METADemuxer::createCodec(const string& codecName, const map<string, string>& addParams,
                                               const std::string& codecStreamName, const vector<MPLSPlayItem>& mplsInfo,
                                               const bool quiet)
{
    AbstractStreamReader* rez = nullptr;
    if (codecName == "V_MPEG4/ISO/AVC" || codecName == "V_MPEG4/ISO/MVC")
    {
        auto h264Reader = new H264StreamReader();
        rez = h264Reader;
        if (codecName == "V_MPEG4/ISO/MVC")
            h264Reader->setIsSubStream(true);

        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<H264StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<H264StreamReader*>(rez)->setRemovePulldown(true);

        itr = addParams.find("level");
        if (itr != addParams.end())
            // Rounded after the x10: the cast came first, so level=4.1 gave 4.0 and 5.1 gave 5.0. Only a level H.264
            // defines is taken (Table A-1): level=26 was written as level_idc 260 in a byte, which is 4.
            dynamic_cast<H264StreamReader*>(rez)->setForceLevel(static_cast<uint8_t>(
                checkedLevel(itr->second, "H.264", 10,
                             {10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62})));
        itr = addParams.find("insertSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertAuto);
        }
        itr = addParams.find("autoSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertAuto);
        }
        itr = addParams.find("forceSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertForce);
        }
        itr = addParams.find("contSPS");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setH264SPSCont(true);
        }
    }
    else if (codecName == "V_MPEGH/ISO/HEVC")
    {
        rez = new HEVCStreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<HEVCStreamReader*>(rez)->setFPS(fps);
        }
        itr = addParams.find("level");
        if (itr != addParams.end())
        {
            // HEVC counts levels in thirtieths, unlike H.264's tenths: level 5.1 is 153, not 51. Only a level HEVC
            // defines is taken (Table A.8, and 8.5 for a stream without limits): 2.7 was written as 81.
            dynamic_cast<HEVCStreamReader*>(rez)->setForceLevel(static_cast<uint8_t>(
                checkedLevel(itr->second, "HEVC", 30,
                             {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186, 189, 210, 213, 216, 255})));
        }
        // Mark an SDR stream as HDR10 on a UHD disc (#6). Only a disc carries the mark; a disc with HEVC video is
        // always a UHD one.
        if (addParams.find("hdr10-override") != addParams.end())
        {
            if (V3_flags & BLURAY_OUT)
                dynamic_cast<HEVCStreamReader*>(rez)->setHdr10Override(true);
            else
            {
                static bool said = false;  // once a run: the track's reader is made more than once
                if (!said)
                    LTRACE(LT_WARN, 2,
                           "Warning: hdr10-override applies to a UHD Blu-ray disc only, and is ignored for this "
                           "output.");
                said = true;
            }
        }
    }
    else if (codecName == "V_MPEGI/ISO/VVC")
    {
        rez = new VVCStreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<VVCStreamReader*>(rez)->setFPS(fps);
        }
    }
    else if (codecName == "V_AV1")
    {
        rez = new AV1StreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<AV1StreamReader*>(rez)->setFPS(fps);
        }
    }
    else if (codecName == "V_VP8" || codecName == "V_VP9")
    {
        rez = new VpxStreamReader(codecName == "V_VP9");
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<VpxStreamReader*>(rez)->setFPS(fps);
        }
    }
    else if (codecName == "V_MS/VFW/WVC1")
    {
        rez = new VC1StreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<VC1StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<VC1StreamReader*>(rez)->setRemovePulldown(true);
    }
    else if (codecName == "V_MPEG-2")
    {
        rez = new MPEG2StreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = parseFpsValue(itr->second);
            fps = correctFps(fps);
            dynamic_cast<MPEG2StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("ar");
        if (itr != addParams.end())
            dynamic_cast<MPEGStreamReader*>(rez)->setAspectRatio(arNameToCode(itr->second));

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<MPEG2StreamReader*>(rez)->setRemovePulldown(true);
    }
    else if (codecName == "A_AAC")
        rez = new AACStreamReader();
    else if (codecName == "A_MP3")
        rez = new MpegAudioStreamReader();
    else if (codecName == "S_SUP")
        rez = new DVBSubStreamReader();
    else if (codecName == "A_LPCM")
        rez = new LPCMStreamReader();
    else if (codecName == "A_MLP")
    {
        // down-to-ac3 keeps the AC-3 frames interleaved with the HD ones and drops the HD part; it
        // does not convert anything, and tsMuxeR has no audio encoder. A_MLP is a standalone
        // TrueHD stream, so by construction there is no core to keep. The option used to be
        // accepted here and silently ignored, which is the actual defect.
        // Warn rather than fail. Unlike the DTS Express case, where dropping the extension
        // leaves literally nothing and there is no valid output to produce, here ignoring the
        // option still yields a correct TrueHD track: exactly what the user already had,
        // because the option has always been a no-op on this codec. Failing would break
        // existing meta files for no gain. The defect was that it was ignored SILENTLY.
        // With merge-ac3-track or merge-ac3-file the line names its core itself, and the text below did not fit it.
        const bool mergedCore =
            addParams.find("merge-ac3-track") != addParams.end() || addParams.find("merge-ac3-file") != addParams.end();
        if (addParams.find("down-to-ac3") != addParams.end() && !quiet && mergedCore)
            LTRACE(LT_WARN, 2,
                   "Warning: down-to-ac3 has no effect on an A_MLP track and is being ignored. The TrueHD is "
                   "written together with the AC-3 core that merge-ac3-track or merge-ac3-file names. To keep the "
                   "AC-3 alone, mux that AC-3 on an A_AC3 line of its own instead of this line.");
        else if (addParams.find("down-to-ac3") != addParams.end() && !quiet)
            LTRACE(LT_WARN, 2,
                   "Warning: down-to-ac3 has no effect on an A_MLP track and is being ignored. "
                   "A standalone TrueHD stream has no AC-3 core to keep, and tsMuxeR does not "
                   "encode audio. If this stream came from a Blu-ray it may already carry an "
                   "interleaved core: run tsMuxeR on the file, and if it reports "
                   "\"A_AC3 / AC3 core + TRUE-HD\" then change this line's codec to A_AC3 and "
                   "down-to-ac3 will work. Otherwise encode a companion track externally "
                   "(ffmpeg -i in -c:a ac3 -b:a 640k -ac 6 compat.ac3) and either mux compat.ac3 "
                   "on its own line, or pass merge-ac3-file=\"compat.ac3\" here to keep the "
                   "TrueHD track with a compatible core alongside it.");
        if (mergedCore)
            rez = new TrueHDAC3MergeReader(addParams);
        else
            rez = new MLPStreamReader();
    }
    else if (codecName == "A_FLAC")
        rez = new FLACStreamReader();
    else if (codecName == "A_OPUS")
        rez = new OpusStreamReader();
    else if (codecName == "A_VORBIS")
        rez = new VorbisStreamReader();
    else if (codecName == "A_ALAC")
        rez = new AlacStreamReader();
    else if (codecName == "V_MPEG4/ISO/ASP" || codecName == "V_PRORES")
        rez = new VideoCopyStreamReader(codecName == "V_PRORES");
    else if (codecName == "A_DTS")
    {
        rez = new DTSStreamReader();
        auto itr = addParams.find("down-to-dts");
        if (itr != addParams.end())
            dynamic_cast<DTSStreamReader*>(rez)->setDownconvertToDTS(true);
    }
    else if (codecName == "A_AC3")
    {
        rez = new AC3StreamReader();
        auto itr = addParams.find("down-to-ac3");
        if (itr != addParams.end())
            dynamic_cast<AC3StreamReader*>(rez)->setDownconvertToAC3(true);
    }
    else if (codecName == "S_HDMV/PGS")
    {
        rez = new PGSStreamReader();
        auto itr = addParams.find("bottom-offset");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setBottomOffset(strToInt32(itr->second.c_str()));

        itr = addParams.find("3d-plane");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setOffsetId(parse3dPlane(itr->second));

        double fps = 0.0;
        uint16_t width = 0;
        uint16_t height = 0;
        itr = addParams.find("fps");
        if (itr != addParams.end())
            fps = parseFpsValue(itr->second);
        itr = addParams.find("video-width");
        if (itr != addParams.end())
            width = parsePictureSize("video-width", itr->second);
        itr = addParams.find("video-height");
        if (itr != addParams.end())
            height = parsePictureSize("video-height", itr->second);
        dynamic_cast<PGSStreamReader*>(rez)->setVideoInfo(width, height, fps);
        itr = addParams.find("font-border");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setFontBorder(strToInt32(itr->second.c_str()));
    }
    else if (codecName == "S_TEXT/UTF8")
    {
        auto srtReader = new SRTStreamReader();
        rez = srtReader;
        text_subtitles::Font font;
        uint16_t srtWidth = 0, srtHeight = 0;
        double fps = 0.0;
        text_subtitles::TextAnimation animation;
        for (const auto& addParam : addParams)
        {
            if (addParam.first == "font-name")
            {
                font.m_name = unquoteStr(addParam.second);
            }
            else if (addParam.first == "font-size")
                font.m_size = static_cast<int>(parseFontNumber("font-size", addParam.second, true));
            else if (addParam.first == "font-bold")
                font.m_opts |= text_subtitles::Font::BOLD;
            else if (addParam.first == "font-italic")
                font.m_opts |= text_subtitles::Font::ITALIC;
            else if (addParam.first == "font-underline")
                font.m_opts |= text_subtitles::Font::UNDERLINE;
            else if (addParam.first == "font-strike-out")
                font.m_opts |= text_subtitles::Font::STRIKE_OUT;
            else if (addParam.first == "font-color")
            {
                font.m_color = parseFontColor(addParam.second);
                if ((font.m_color & 0xff000000u) == 0)
                    font.m_color |= 0xff000000u;
            }
            else if (addParam.first == "line-spacing")
                font.m_lineSpacing = static_cast<float>(parseFontNumber("line-spacing", addParam.second, false));
            else if (addParam.first == "font-charset")
                font.m_charset = strToInt32(addParam.second.c_str());
            else if (addParam.first == "font-border")
                font.m_borderWidth = static_cast<float>(parseFontNumber("font-border", addParam.second, false));
            else if (addParam.first == "fps")
            {
                fps = parseFpsValue(addParam.second);
            }
            else if (addParam.first == "video-width")
                srtWidth = parsePictureSize("video-width", addParam.second);
            else if (addParam.first == "video-height")
                srtHeight = parsePictureSize("video-height", addParam.second);
            else if (addParam.first == "bottom-offset")
                srtReader->setBottomOffset(strToInt32(addParam.second.c_str()));
            else if (addParam.first == "fadein-time")
                animation.fadeInDuration = parseFadeTime("fadein-time", addParam.second);
            else if (addParam.first == "fadeout-time")
                animation.fadeOutDuration = parseFadeTime("fadeout-time", addParam.second);
            else if (addParam.first == "keep-text")
                srtReader->setKeepText(true);
            // A drawn subtitle is a PG stream of the disc like any other: 3d-plane= was read only on a PGS line, and
            // the subtitles of an SRT line sat at the depth of the screen on a 3D disc, without a word.
            else if (addParam.first == "3d-plane")
                srtReader->setOffsetId(parse3dPlane(addParam.second));
        }
        // save-srt (#643): an SRT copy of the subtitles beside an output that gets them as pictures. Kept as text, the
        // output itself holds the text, and a demux writes the .srt.
        if (const auto saveSrt = addParams.find("save-srt"); saveSrt != addParams.end() && !quiet)
        {
            if (srtReader->keepText())
                LTRACE(LT_WARN, 2, "Warning: save-srt is ignored on a line with keep-text: the subtitles stay text.");
            else if (unquoteStr(saveSrt->second).empty())
                THROW(ERR_COMMON, "save-srt needs the name of the SRT file to write, as save-srt=\"name.srt\"")
            // The file is opened for writing here, before the source is read: naming the source emptied it.
            else if (sameExistingFile(unquoteStr(saveSrt->second), codecStreamName))
                THROW(ERR_COMMON, "save-srt names the subtitle file the track is read from: "
                                      << unquoteStr(saveSrt->second) << ". Give it another name.")
            else
                srtReader->setSaveSrt(unquoteStr(saveSrt->second));
        }
        // The picture size and frame rate are only needed to draw the subtitles. Kept as text they
        // are not drawn, unless the output cannot carry text, and that output says so when it is.
        // The message named "SRT tracks" also for an ASS track and for a demux, and did not say which value was
        // missing or that keep-text needs none of them.
        if (!srtReader->keepText() && (srtWidth == 0 || srtHeight == 0 || fps == 0.0))
        {
            std::vector<const char*> missing;
            if (srtWidth == 0)
                missing.push_back("video-width");
            if (srtHeight == 0)
                missing.push_back("video-height");
            if (fps == 0.0)
                missing.push_back("fps");
            std::string list = missing[0];
            for (size_t i = 1; i < missing.size(); ++i)
                list += (i + 1 == missing.size() ? " and " : ", ") + std::string(missing[i]);
            const auto track = addParams.find("track");
            THROW(ERR_COMMON, "The text subtitles of "
                                  << (track != addParams.end() ? "track " + track->second + " of " : std::string())
                                  << unquoteStr(codecStreamName)
                                  << " are drawn as pictures, which needs the size and frame rate of the video: "
                                  << list << (missing.size() > 1 ? " are" : " is")
                                  << " not given on the line. Give video-width, video-height and fps, or keep-text "
                                     "to keep the subtitles as text (in a Matroska output or a demux).")
        }
        if (srtWidth && srtHeight && fps != 0.0)
        {
            srtReader->setVideoInfo(srtWidth, srtHeight, fps);
            if (!quiet && !srtReader->keepText())
                warnSubtitleCanvas("the text subtitles of " + unquoteStr(codecStreamName), srtWidth, srtHeight, fps);
        }
        srtReader->setFont(font);
        srtReader->setAnimation(animation);
    }
    else
        THROW(ERR_UNKNOWN_CODEC, "Unsupported codec " << codecName)

    if (codecName[0] == 'A')
    {
        auto itr = addParams.find("stretch");
        if (itr != addParams.end())
            dynamic_cast<SimplePacketizerReader*>(rez)->setStretch(parseStretchValue(itr->second));
    }
    // Only an audio track is stretched; on a video or subtitle line the parameter was taken and did nothing
    else if (addParams.find("stretch") != addParams.end() && !quiet)
        LTRACE(LT_WARN, 2,
               "Warning: stretch= applies to audio tracks only, and is ignored on the " << codecName << " track.");
    auto itr = addParams.find("secondary");
    if (itr != addParams.end())
        rez->setIsSecondary(true);

    PIPParams pipParams;
    itr = addParams.find("pipCorner");
    if (itr != addParams.end())
        pipParams.corner = pipCornerFromStr(itr->second);
    itr = addParams.find("pipHOffset");
    if (itr != addParams.end())
        pipParams.hOffset = pipNumber("pipHOffset", itr->second, 4095, "an offset in pixels");
    itr = addParams.find("pipVOffset");
    if (itr != addParams.end())
        pipParams.vOffset = pipNumber("pipVOffset", itr->second, 4095, "an offset in pixels");
    itr = addParams.find("pipLumma");
    if (itr != addParams.end())
        pipParams.lumma = pipNumber("pipLumma", itr->second, 255, "a luma value");
    itr = addParams.find("pipScale");
    if (itr != addParams.end())
        pipParams.scaleIndex = pipScaleFromStr(itr->second);
    rez->setPipParams(pipParams);

    if (!mplsInfo.empty() && dynamic_cast<SimplePacketizerReader*>(rez))
        dynamic_cast<SimplePacketizerReader*>(rez)->setMPLSInfo(mplsInfo);

    return rez;
}

std::vector<MPLSParser> METADemuxer::getMplsInfo(const string& mplsFileName)
{
    std::vector<MPLSParser> result;

    std::vector<std::string> mplsFiles = splitQuotedStr(mplsFileName.c_str(), '+');
    for (auto& i : mplsFiles)
    {
        auto itr = m_mplsStreamMap.find(i);
        if (itr != m_mplsStreamMap.end())
            result.push_back(itr->second);
        else
        {
            MPLSParser parser;
            if (!parser.parse(unquoteStr(i).c_str()))
            {
                // A playlist that is not there was "Can't parse play list file", as if it were damaged
                const std::string name = unquoteStr(i);
                if (File file; !file.open(name.c_str(), File::ofRead))
                    THROW(ERR_FILE_NOT_FOUND, "Can't open file: " << name << openFailureReason(name))
                THROW(ERR_COMMON, "Can't parse play list file " << i)
            }
            if (!m_quiet)
                parser.reportLeftOut(unquoteStr(i), false);
            cutPlayItems(parser, unquoteStr(i), m_playlistClipsPlayed);
            m_mplsStreamMap[i] = parser;
            result.push_back(parser);
        }
    }
    return result;
}

// The time of the key picture an entry point names, as its PES header gives it (the entry point map keeps the time
// without its 9 lowest bits), and the times of the first and the last picture shown before it of the pictures that
// follow it in the order of decoding, as an open GOP has them (-1: none). False when the clip file does not have the
// picture where the map says.
static bool keyPictureTimes(const string& m2tsFileName, const int pid, const ClipEntryPoint& key, int64_t& keyPts,
                            int64_t& firstShown, int64_t& lastBefore)
{
    lastBefore = -1;
    File file;
    if (!file.open(m2tsFileName.c_str(), File::ofRead) ||
        file.seek(static_cast<int64_t>(key.spn) * 192, File::SeekMethod::smBegin) == -1)
        return false;
    keyPts = -1;
    std::vector<uint8_t> buffer(192 * 2048);
    // up to the first picture after the key picture that is shown after it, within 64 MiB
    for (int block = 0; block < 170; ++block)
    {
        const int len = file.read(buffer.data(), static_cast<uint32_t>(buffer.size()));
        for (int pos = 0; pos + 192 <= len; pos += 192)
        {
            uint8_t* packet = buffer.data() + pos + 4;  // after the 4 bytes of arrival time
            if (packet[0] != 0x47 || ((packet[1] & 0x1f) << 8 | packet[2]) != pid || !(packet[1] & 0x40) ||
                !(packet[3] & 0x10))
                continue;
            const int offset = packet[3] & 0x20 ? 5 + packet[4] : 4;
            if (offset + 14 > 188)
                continue;
            auto* pes = reinterpret_cast<PESPacket*>(packet + offset);
            if (pes->startCode0 != 0 || pes->startCode1 != 0 || pes->startCode2 != 1 || !(pes->flagsLo & 0x80))
                continue;
            const int64_t pts = pes->getPts();
            if (keyPts < 0)
            {
                if ((pts & ~int64_t{511}) != key.pts)
                    return false;
                keyPts = firstShown = pts;
            }
            else if (pts > keyPts)
                return true;
            else
            {
                firstShown = (std::min)(firstShown, pts);
                lastBefore = (std::max)(lastBefore, pts);
            }
        }
        if (len < static_cast<int>(buffer.size()))
            break;
    }
    return keyPts >= 0;
}

// ** A PLAYLIST THAT PLAYS ONLY A PART OF A CLIP GETS ONLY THAT PART (--playlist-clips=played). ** A play item names a
// clip and the stretch of it that is played, from IN_time to OUT_time. Most play the whole clip, but a playlist may
// play the first seconds of each of many clips, or several stretches of one clip, as a version of a film made of the
// scenes of another. Every clip was muxed whole all the same, from its first picture to its last: the pictures the
// playlist leaves out were in the output, a clip played in several stretches came in whole that many times, and the
// playlist's length, its marks and the places of its play items no longer matched what was muxed.
//
// A clip is now cut where a player starts and stops in it: from the key picture at or before IN_time, where a player
// starts to decode, to the key picture at or after OUT_time, both as the entry point map of the clip information file
// lists them. Only the bytes of the clip around that part are read. The play item's IN_time and OUT_time are moved out
// to those key pictures, so that the length of the playlist, its chapters and the place of each part follow what is
// muxed. Once one play item of a playlist is cut, all of them are read as parts, from their first key picture, so
// that the sound of each part starts where the sound of the part before ends (TSDemuxer::keptInPart).
//
// --playlist-clips=whole keeps the whole clips; the log says when a playlist plays only parts of them. A 3D playlist
// keeps them too: the second view is read from files the map does not count in.
void METADemuxer::cutPlayItems(MPLSParser& mpls, const string& mplsFileName, const bool played)
{
    struct Plan
    {
        int pid = -1;
        std::vector<ClipEntryPoint> keys;
        size_t first = 0;      // the key picture the part starts at
        size_t last = 0;       // the key picture the part ends at; keys.size() at the end of the clip
        bool inFirst = false;  // IN_time is after the first key picture, inside its group of pictures
    };
    const string streamDir = playlistStreamDir(mplsFileName);
    std::vector<Plan> plans(mpls.m_playItems.size());
    size_t cutItems = 0;
    string unread;
    std::map<string, Plan> maps;  // a clip played in many items is read once
    for (size_t i = 0; i < plans.size(); ++i)
    {
        const MPLSPlayItem& item = mpls.m_playItems[i];
        Plan& plan = plans[i];
        auto known = maps.find(item.fileName);
        if (known == maps.end())
        {
            Plan read;
            const string clpi = findBluRayFile(streamDir, "CLIPINF", item.fileName + ".clpi");
            if (clpi.empty() || !readClipEntryPoints(clpi, read.pid, read.keys))
                read.keys.clear();
            known = maps.emplace(item.fileName, std::move(read)).first;
        }
        plan.pid = known->second.pid;
        plan.keys = known->second.keys;
        if (plan.keys.empty())
        {
            if (unread.empty())
                unread = item.fileName;
            continue;
        }
        const int64_t in = static_cast<int64_t>(item.IN_time) * 2;
        const int64_t out = static_cast<int64_t>(item.OUT_time) * 2;
        while (plan.first + 1 < plan.keys.size() && plan.keys[plan.first + 1].pts <= in) ++plan.first;
        // a key picture at or after OUT_time: the map keeps its time less up to 511
        plan.last = plan.first + 1;
        while (plan.last < plan.keys.size() && plan.keys[plan.last].pts <= out - 512) ++plan.last;
        // A play item that starts inside the first group of pictures of its clip, as one of a disc tsMuxeR cut with
        // --cut-start does (the clip begins at the key picture before the cut, and the play item at the cut): it was
        // read as the whole clip, so the length and the marks the playlist gives, from IN_time, were not those of what
        // was muxed, from the key picture. It is cut there now, as one that starts in a later group is.
        if (plan.first == 0 && in > plan.keys[0].pts + 511)
        {
            int64_t keyPts = -1, firstShown = -1, lastBefore = -1;
            const string m2ts = mplsTrackToFullName(mplsFileName, item.fileName);
            plan.inFirst = keyPictureTimes(m2ts, plan.pid, plan.keys[0], keyPts, firstShown, lastBefore) && in > keyPts;
        }
        if (plan.first > 0 || plan.inFirst || plan.last < plan.keys.size())
            ++cutItems;
    }
    if (cutItems == 0)
        return;
    // ** A CLIP MUXED WHOLE IS PLACED AS A WHOLE CLIP. ** The play items kept their IN_time and OUT_time, so the
    // chapters, the listing and the place each clip starts at were those of the parts the playlist plays, while the
    // output holds the whole clips: a mark 10.4 s into a clip played from there was a chapter at 0, and the marks of
    // the next clip came as much early as the parts before them were short of their clips. Each play item now spans
    // its clip as the clip information gives it (SequenceInfo), where the play item lies inside that span.
    const auto wholeClips = [&]()
    {
        for (MPLSPlayItem& item : mpls.m_playItems)
        {
            const string clpiName = findBluRayFile(streamDir, "CLIPINF", item.fileName + ".clpi");
            CLPIParser clpi;
            if (clpiName.empty() || !clpi.parse(clpiName.c_str()) ||
                clpi.presentation_start_time >= clpi.presentation_end_time ||
                item.IN_time < clpi.presentation_start_time || item.OUT_time > clpi.presentation_end_time)
                continue;
            item.IN_time = clpi.presentation_start_time;
            item.OUT_time = clpi.presentation_end_time;
        }
    };
    const string name = extractFileName2(mplsFileName, true);
    std::ostringstream what;
    what << name << " plays only a part of its clip in " << cutItems << " of its " << plans.size() << " play items";
    if (!played)
    {
        LTRACE(LT_INFO, 2,
               "Playlist " << what.str()
                           << ". Each clip is muxed whole (--playlist-clips=whole); --playlist-clips=played muxes only "
                              "the parts the playlist plays.");
        wholeClips();
        return;
    }
    if (mpls.isDependStreamExist)
    {
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << what.str() << ", but it is a 3D playlist, and its clips are muxed whole.");
        wholeClips();
        return;
    }
    if (!unread.empty())
    {
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << what.str() << ", but the clip information of clip " << unread
                                    << " gives no map of its key pictures, so the clips are muxed whole.");
        wholeClips();
        return;
    }
    // A play item that goes on in the same clip where the one before it ends, or inside its last group of pictures,
    // continues the same stretch of the clip. Each cut at its own key pictures, the group of pictures between them came
    // in twice. The first one now ends where the next one starts, and the next one goes on from it with nothing left
    // out. One that starts at the same key picture as the one before leaves nothing to that one, and goes on from the
    // one before that when that one is continued.
    // A play item that starts earlier in the stretch of the one before, or at the same time, plays a part of it again:
    // that is no continuation. It was taken for one, as its start lay between the first and the last key picture of
    // the one before, which was then cut back to it: a stretch played twice was muxed once, the first play item
    // listed with a length of 0, and two play items that overlap gave only the stretch from the start of the first to
    // the end of the second. Such a play item is now muxed as a part of its own, as a player plays it.
    std::vector<bool> continues(plans.size(), false);
    for (size_t i = 0; i + 1 < plans.size(); ++i)
        if (mpls.m_playItems[i + 1].fileName == mpls.m_playItems[i].fileName && plans[i + 1].first >= plans[i].first &&
            plans[i + 1].first <= plans[i].last && plans[i + 1].first + 1 >= plans[i].last &&
            mpls.m_playItems[i + 1].IN_time > mpls.m_playItems[i].IN_time)
        {
            plans[i].last = plans[i + 1].first;
            continues[i + 1] = plans[i + 1].first > plans[i].first || continues[i];
        }
    struct KeyTimes
    {
        bool found = false;
        int64_t pts = -1, firstShown = -1, lastBefore = -1;
    };
    std::map<std::pair<string, size_t>, KeyTimes> keyTimes;  // a key picture where one part ends and the next starts
    const auto timesOf = [&](const string& m2ts, const Plan& plan, const size_t key)
    {
        const auto it = keyTimes.find({m2ts, key});
        if (it != keyTimes.end())
            return it->second;
        KeyTimes t;
        t.found = keyPictureTimes(m2ts, plan.pid, plan.keys[key], t.pts, t.firstShown, t.lastBefore);
        return keyTimes[{m2ts, key}] = t;
    };
    std::vector<MPLSPlayItem> items = mpls.m_playItems;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const Plan& plan = plans[i];
        const string m2ts = mplsTrackToFullName(mplsFileName, items[i].fileName);
        ClipPart part;
        part.startCut = plan.first > 0 || plan.inFirst;
        part.continuesPrevious = continues[i];
        part.continuedByNext = i + 1 < items.size() && continues[i + 1];
        int64_t before = -1;
        KeyTimes t = timesOf(m2ts, plan, plan.first);
        bool found = t.found;
        part.startPts = t.pts;
        part.startShown = t.firstShown;
        // read from two key pictures earlier: the sound and subtitles of the first pictures may be stored before them
        if (part.startCut)
            part.firstByte = static_cast<int64_t>(plan.keys[plan.first >= 2 ? plan.first - 2 : 0].spn) * 192;
        if (found && plan.last == plan.first)
        {
            // nothing of its own: the next part starts at the same key picture
            part.endPts = part.startPts;
            part.shownEnd = part.firstShown();
        }
        else if (found && plan.last < plan.keys.size())
        {
            // The pictures of an open GOP shown before the last key picture are made from it and are left out with it,
            // unless the play item shows some of them: then they are kept, with the key picture. A part that the next
            // one continues leaves them to that one.
            t = timesOf(m2ts, plan, plan.last);
            found = t.found;
            part.endPts = t.pts;
            part.shownEnd = t.firstShown;
            before = t.lastBefore;
            if (found && !part.continuedByNext && before >= 0 &&
                static_cast<int64_t>(items[i].OUT_time) * 2 > part.shownEnd)
            {
                part.keepEndKey = true;
                part.shownEnd = part.endPts + (part.endPts - before);
            }
            // and on to 2 s after the last one, for the sound and subtitles stored after it
            size_t after = plan.last;
            while (after < plan.keys.size() && plan.keys[after].pts < plan.keys[plan.last].pts + 2 * 90000) ++after;
            part.endByte = after < plan.keys.size() ? static_cast<int64_t>(plan.keys[after].spn) * 192 : -1;
        }
        if (!found)
        {
            LTRACE(LT_WARN, 2,
                   "Warning: playlist " << what.str() << ", but clip " << items[i].fileName
                                        << " does not have a key picture where its clip information says, so the "
                                           "clips are muxed whole.");
            wholeClips();
            return;
        }
        if (part.startCut)
            items[i].IN_time = static_cast<uint32_t>(part.firstShown() / 2);
        if (part.endPts >= 0)
            items[i].OUT_time = (std::max)(items[i].IN_time, static_cast<uint32_t>(part.shownEnd / 2));
        items[i].part = part;
    }
    mpls.m_playItems = items;
    LTRACE(LT_INFO, 2,
           "Playlist " << what.str()
                       << ": only the parts played are muxed, each from the key picture at or before where it starts "
                          "to the key picture at or after where it ends (--playlist-clips=played).");
}

string METADemuxer::findBluRayFile(const string& streamDir, const string& requestDir, const string& requestFile)
{
    string dirName = streamDir.substr(0, streamDir.size() - 1);
    if (strEndWith(strToLowerCase(dirName), "ssif"))
    {
        const size_t pos = dirName.find_last_of(getDirSeparator());
        if (pos > 0)
            dirName = streamDir.substr(0, pos);
    }

    const size_t tmp = dirName.find_last_of(getDirSeparator());
    if (tmp != std::string::npos)
    {
        dirName = streamDir.substr(0, tmp + 1);
        string fileName = dirName + requestDir + getDirSeparator() + requestFile;
        if (!fileExists(fileName))
        {
            fileName = dirName + string("BACKUP") + getDirSeparator() + requestDir + getDirSeparator() + requestFile;
            if (!fileExists(fileName))
                return "";

            return fileName;
        }

        return fileName;
    }

    return "";
}

void METADemuxer::updateReport(const bool checkTime)
{
    const auto currentTime = std::chrono::steady_clock::now();
    if (!checkTime || currentTime - m_lastReportTime > std::chrono::microseconds(250000))
    {
        // The figure is the data read, and it stops at 99.9 percent: the files are still to be closed and the tracks
        // checked after the last byte is read, and that can fail. It said "100.0% complete" over a job that then
        // ended with an error. 100.0 is printed by the job itself, once every final check has passed.
        double progress = 99.9;
        if (m_totalSize > 0)
        {
            const int64_t currentProcessedSize = getDemuxedSize();
            progress = static_cast<double>(currentProcessedSize) / static_cast<double>(m_totalSize) * 100.0;
            if (progress > 99.9)
                progress = 99.9;
            // Never below what was shown already. A converted subtitle track hands on more bytes than it read, which
            // the count makes up with bytes it takes as discarded before the codec has used the ones it was given:
            // the line went below zero, to -145 percent for a DVB track alone. The count itself stays as it is,
            // because the check that a mux read all of its sources compares the same total.
            progress = (std::max)(progress, m_lastProgress);
            m_lastProgress = progress;
        }
        lineBack();
        cout << doubleToStr(progress, 1) << "% complete" << std::endl;
        m_lastReportTime = currentTime;
    }
}

void METADemuxer::lineBack()
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    const HANDLE consoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(consoleOutput, &csbi);
    if (csbi.dwCursorPosition.Y == m_lastProgressY)
    {
        csbi.dwCursorPosition.X = 0;
        m_lastProgressY = csbi.dwCursorPosition.Y;
        csbi.dwCursorPosition.Y--;
        SetConsoleCursorPosition(consoleOutput, csbi.dwCursorPosition);
    }
    else
        m_lastProgressY = csbi.dwCursorPosition.Y + 1;
#else
    // ESC M takes the cursor up a line, over the last progress line. In a file or a pipe it is two bytes in front of
    // the next line, and a script that reads "NN.N% complete" at the start of a line missed every one.
    if (!sLastMsg && isatty(STDOUT_FILENO))
        std::cout << "\x1bM";
    sLastMsg = false;
#endif
}

// ------------------- StreamInfo ---------------------
int StreamInfo::read()
{
    int readRez = 0;
    if (m_asyncMode && !m_notificated && !m_isEOF)
    {
        m_dataReader->notify(m_readerID, m_dataReader->getPreReadThreshold());
        m_notificated = true;
    }

    if (m_lastAVRez != 0)
    {
        if (m_isEOF)
        {
            // A merged TrueHD can reach its own end before its core does: a file of one read block
            // arrives whole, together with its end of data. Its reader then asks for more core, and the
            // core side used to be read only together with more lossless data, which no longer comes.
            // The wait never ended and the lossless audio it held back was dropped at the end, silently.
            // So the core side is read once more, and the reader gets back what it held.
            auto* mergeReader = m_mergeAc3ReaderId >= 0 ? dynamic_cast<TrueHDAC3MergeReader*>(m_streamReader) : nullptr;
            if (mergeReader && !mergeReader->ac3SideEof())
            {
                uint32_t ac3Cnt = 0;
                int ac3Rez = 0;
                AbstractReader* r = m_mergeAc3DataReader ? m_mergeAc3DataReader : m_dataReader;
                uint8_t* ac3Data = r->readBlock(m_mergeAc3ReaderId, ac3Cnt, ac3Rez);
                if (ac3Rez == BufferedFileReader::DATA_NOT_READY || ac3Rez == BufferedFileReader::DATA_DELAYED)
                    return ac3Rez;
                if (ac3Data != nullptr)
                    ac3Data += mergeReader->getTmpBufferSize();
                mergeReader->setAc3SideData(ac3Data, ac3Cnt);
                // Nothing read and no more promised is the end as well, or this would ask for ever.
                if (ac3Rez == BufferedFileReader::DATA_EOF || ac3Cnt == 0)
                    mergeReader->setAc3SideEof();
                static uint8_t noData[MAX_AV_PACKET_SIZE + 1];
                m_streamReader->setBuffer(noData, 0, true);
                m_lastAVRez = 0;
                return 0;
            }
            m_blockSize = 0;
            return BufferedFileReader::DATA_EOF2;
        }
        m_lastAVRez = 0;

        if (m_mergeAc3ReaderId >= 0)
        {
            auto* mergeReader = dynamic_cast<TrueHDAC3MergeReader*>(m_streamReader);
            if (mergeReader)
            {
                uint32_t ac3Cnt = 0;
                int ac3Rez = 0;
                AbstractReader* r = m_mergeAc3DataReader ? m_mergeAc3DataReader : m_dataReader;
                uint8_t* ac3Data = r->readBlock(m_mergeAc3ReaderId, ac3Cnt, ac3Rez);
                if (ac3Rez == BufferedFileReader::DATA_NOT_READY || ac3Rez == BufferedFileReader::DATA_DELAYED)
                {
                    m_lastAVRez = ac3Rez;
                    return ac3Rez;
                }
                // the reader protocol prefixes every block with a front pad of
                // getTmpBufferSize() bytes (the same value this reader was registered
                // with in createReader); the main path skips it inside setBuffer, and
                // skipping it here too is what makes the side data actual AC-3 instead
                // of pad garbage plus a truncated tail
                if (ac3Data != nullptr)
                    ac3Data += mergeReader->getTmpBufferSize();
                mergeReader->setAc3SideData(ac3Data, ac3Cnt);
                // The reader must be able to tell "none buffered yet" from "there will never be
                // any more", because a core frame that is due waits for one. With merge-ac3-file
                // the AC-3 can end while most of the lossless track is still to come, so the
                // lossless stream's own end of data is the wrong thing to ask.
                if (ac3Rez == BufferedFileReader::DATA_EOF)
                    mergeReader->setAc3SideEof();
            }
        }

        bool firstBlockOfFile = false;
        m_data = m_dataReader->readBlock(m_readerID, m_blockSize, readRez, &firstBlockOfFile);
        if (readRez == BufferedFileReader::DATA_NOT_READY || readRez == BufferedFileReader::DATA_DELAYED)
        {
            m_lastAVRez = readRez;
            return readRez;
        }
        if (readRez == BufferedFileReader::DATA_EOF)
            m_isEOF = true;
        // A gap in the stream's times, found where the demuxer opened a later file of a join, before the reader
        // gets that file's data
        if (auto* wrapper = dynamic_cast<ContainerToReaderWrapper*>(m_dataReader))
        {
            for (const auto& [atNs, gapNs] : wrapper->takeTimeGaps(m_readerID))
            {
                m_streamReader->addTimeGap(atNs, gapNs);
                m_joinGaps.push_back(llround(static_cast<double>(gapNs) * INTERNAL_PTS_FREQ / 1e9));
            }
            if (auto* srt = dynamic_cast<SRTStreamReader*>(m_streamReader))
            {
                for (const int64_t ms : wrapper->takeTouchingCues(m_readerID)) srt->addTouchingCue(ms);
                for (const auto& [ms, nextMs] : wrapper->takeNextCueStarts(m_readerID))
                    srt->addNextCueStart(ms, nextMs);
            }
        }
        // The file reader begins each later file of a join ("a.264"+"b.264") with a block of its own
        if (firstBlockOfFile && m_readCnt > 0 && m_blockSize > 0)
            m_streamReader->onJoinedFileStart();
        m_streamReader->setBuffer(m_data, m_blockSize, m_isEOF);
        m_readCnt += m_blockSize;
        m_notificated = false;
        // A stream that begins in a later clip of a join, such as the film's audio after a logo clip without any,
        // is moved to where that clip begins, as a timeshift= would move it. Its reader counts time from its own
        // first frame and cannot know this, so the audio used to start with the logo. The demuxer has seen the
        // stream's first PES by the time its first data arrives here, and for every stream of the first clip the
        // start is 0, which changes nothing.
        if (!m_clipStartTaken && m_readCnt > 0)
        {
            m_clipStartTaken = true;
            if (auto* wrapper = dynamic_cast<ContainerToReaderWrapper*>(m_dataReader))
            {
                const int64_t start = ptsToInternalClock(wrapper->getTrackClipStart(m_readerID));
                if (start > 0)
                {
                    m_timeShift += start;
                    m_lastDTS = FFMAX(m_lastDTS, m_timeShift);
                }
            }
        }
    }
    return readRez;
}

// ------------------------------ ContainerToReaderWrapper --------------------------------

uint8_t* ContainerToReaderWrapper::readBlock(const int readerID, uint32_t& readCnt, int& rez, bool* firstBlockVar)
{
    rez = 0;
    uint8_t* data = nullptr;
    const auto itr = m_readerInfo.find(readerID);
    if (itr == m_readerInfo.end())
        return nullptr;

    DemuxerData& demuxerData = itr->second.m_demuxerData;
    const int pid = itr->second.m_pid;
    const uint32_t nFileBlockSize = demuxerData.m_demuxer->getFileBlockSize();

    if (demuxerData.m_firstRead)
    {
        for (auto itr1 = demuxerData.m_pids.begin(); itr1 != demuxerData.m_pids.end(); ++itr1)
        {
            MemoryBlock& vect = demuxerData.demuxedData[itr1->first];
            vect.reserve(static_cast<int>(nFileBlockSize + m_readBuffOffset));
            vect.resize(static_cast<int>(m_readBuffOffset));
        }
        demuxerData.m_firstRead = false;
    }
    StreamData& streamData = demuxerData.demuxedData[pid];

    const uint32_t lastReadCnt = demuxerData.lastReadCnt[pid];
    if (lastReadCnt > 0)
    {
        demuxerData.lastReadCnt[pid] = 0;
        const size_t currentSize = streamData.size() - m_readBuffOffset;
        assert(currentSize >= lastReadCnt);
        if (currentSize > lastReadCnt)
        {
            uint8_t* dataStart = streamData.data() + m_readBuffOffset;
            memmove(dataStart, dataStart + lastReadCnt, static_cast<size_t>(currentSize) - lastReadCnt);
        }
        streamData.resize(static_cast<int>(m_readBuffOffset + currentSize - lastReadCnt));
        demuxerData.lastReadCnt[pid] = 0;
    }

    readCnt = static_cast<uint32_t>((FFMIN(streamData.size(), nFileBlockSize) - m_readBuffOffset));
    const DemuxerReadPolicy policy = demuxerData.m_pids[pid];
    // ** A SUBTITLE DUE BEFORE ITS PLACE IN THE FILE IS READ AHEAD FOR. ** The packets of a sparse track are taken
    // as the file is read for the other tracks, so with a negative timeshift each came as late as the shift: every
    // PGS of a Matroska file with timeshift=-3000ms reached the output 1.7 to 2.5 s after it was due, and of a disc
    // clip the same. While such a track has nothing buffered, the file is read on to the time its next packet can
    // be due at (setReadAhead), where the demuxer says how far it has read (Matroska, transport stream), as long as
    // no track of the file holds a quarter of the buffer limit.
    const int64_t aheadTo = itr->second.m_readAheadTo;
    const auto readAhead = [&]()
    {
        if (aheadTo == INT64_MIN || policy != DemuxerReadPolicy::drpFragmented)
            return false;
        const int64_t at = demuxerData.m_demuxer->getDemuxedTime();
        if (at < 0 || at >= aheadTo)
            return false;
        for (const auto& [otherPid, otherData] : demuxerData.demuxedData)
            if (otherData.size() > MAX_DEMUX_BUFFER_SIZE / 4)
                return false;
        return true;
    };
    // ** A TRACK THAT STARTS LATE DOES NOT HOLD UP THE OTHERS. ** A track of a file whose first data comes after more
    // than MAX_DEMUX_BUFFER_SIZE of the other tracks, as a commentary that begins 5 minutes into a film of 10 Mbit/s,
    // read the file on alone for its first block, and the job ended with "Reading buffer overflow". While such a track
    // has never had data, it waits (DATA_DELAYED) once another track of the file holds a quarter of that limit, and
    // the others are muxed on; it is read again as soon as the file has given it data.
    //
    // ** AND A TRACK THAT STOPS FOR A WHILE DOES NOT EITHER. ** A track that has had data and then has no packet for
    // as long, as a commentary or a second language that drops out for a minute of a recording, read the file on in
    // the same way to fill its next block, and the job ended with the same error. It waits in the same way when it
    // has nothing left while another track holds a quarter of the limit, and what it has left, less than a block,
    // is handed on first. Only for a file whose demuxer says how far it has read (transport stream, Matroska): an
    // MP4 file stores its tracks in chunks of its own and is read on as before.
    const bool waitsForStart = !itr->second.m_gotData && policy == DemuxerReadPolicy::drpReadSequence;
    const bool waitsInGap = itr->second.m_gotData && policy == DemuxerReadPolicy::drpReadSequence &&
                            demuxerData.m_demuxer->getDemuxedTime() >= 0;
    const auto othersHeld = [&]()
    {
        for (const auto& [otherPid, otherData] : demuxerData.demuxedData)
            if (otherPid != pid && otherData.size() > MAX_DEMUX_BUFFER_SIZE / 4)
                return true;
        return false;
    };
    // A short rest is handed on as it is once the demuxer has reached the end: reading on cannot add to it. The test
    // read lastReadCnt, a byte count that is always 0 here (taken above), twice against DATA_EOF2, so it never held.
    if ((readCnt > 0 && (policy == DemuxerReadPolicy::drpFragmented || demuxerData.lastReadRez[pid] == DATA_EOF ||
                         demuxerData.lastReadRez[pid] == DATA_EOF2)) ||
        readCnt >= MIN_READED_BLOCK)
    {
        data = streamData.data();
        demuxerData.lastReadCnt[pid] = readCnt;
        demuxerData.lastReadRez[pid] = 0;
        itr->second.m_gotData = true;
    }
    else if (demuxerData.lastReadRez[pid] != DATA_DELAYED || demuxerData.m_allFragmented || readAhead() ||
             ((waitsForStart || waitsInGap) && readCnt > 0))
    {
        int demuxRez;
        // what the file has given since this track asked, for the message below
        int64_t heldAtStart = 0;
        for (const auto& [anyPid, anyData] : demuxerData.demuxedData) heldAtStart += anyData.size();
        do
        {
            int64_t discardSize = 0;
            demuxRez =
                demuxerData.m_demuxer->simpleDemuxBlock(demuxerData.demuxedData, demuxerData.m_pidSet, discardSize);
            for (auto itr1 = demuxerData.demuxedData.begin(); itr1 != demuxerData.demuxedData.end() && !m_terminated;
                 ++itr1)
            {
                if (itr1->second.size() > MAX_DEMUX_BUFFER_SIZE)
                {
                    string ext = strToUpperCase(extractFileExt(demuxerData.m_streamName));
                    // ** SAY WHICH TRACK IS WAITED FOR. ** The message sent the user to fps= also where one track
                    // has no packets for a long stretch of the file, a track that starts late or stops for a while,
                    // and the file was read on for it until another track overflowed: no fps= helps there.
                    int64_t heldNow = 0;
                    for (const auto& [anyPid, anyData] : demuxerData.demuxedData) heldNow += anyData.size();
                    const bool waitedFor = itr1->first != pid && readCnt < MIN_READED_BLOCK &&
                                           heldNow - heldAtStart > MAX_DEMUX_BUFFER_SIZE / 4;
                    if (ext != "MOV" && ext != "MP4" && ext != "M4V" && ext != "M4A" && waitedFor)
                        THROW(
                            ERR_CONTAINER_STREAM_NOT_SYNC,
                            "Reading buffer overflow: track "
                                << pid << " of " << demuxerData.m_streamName << " has no data in "
                                << (heldNow - heldAtStart) / (1024 * 1024)
                                << " MiB of the file, as a track that starts late or stops for a while, and more "
                                   "than "
                                << MAX_DEMUX_BUFFER_SIZE / (1024 * 1024)
                                << " MiB of another track of it wait to be muxed. tsMuxeR waits for such a track in a "
                                   "transport stream or a Matroska file only: remux the file into one of them, or "
                                   "mux the track from a file of its own.")
                    if (ext != "MOV" && ext != "MP4" && ext != "M4V" && ext != "M4A")
                        THROW(ERR_CONTAINER_STREAM_NOT_SYNC,
                              "Reading buffer overflow: more than "
                                  << MAX_DEMUX_BUFFER_SIZE / (1024 * 1024) << " MiB of one track of "
                                  << demuxerData.m_streamName
                                  << " wait to be muxed. Either a picture of it is larger than 256 MiB, which tsMuxeR "
                                     "cannot read, or the tracks of the file are far out of step: check the fps= of "
                                     "its tracks.")
                }
            }
            m_discardedSize += discardSize;
            readCnt = static_cast<uint32_t>((FFMIN(streamData.size(), nFileBlockSize) - m_readBuffOffset));
        } while (demuxRez == 0 && !m_terminated &&
                 ((readCnt < MIN_READED_BLOCK && policy != DemuxerReadPolicy::drpFragmented &&
                   !(waitsForStart && readCnt == 0 && othersHeld()) && !(waitsInGap && othersHeld())) ||
                  (readCnt == 0 && readAhead())));

        demuxerData.lastReadCnt[pid] = readCnt;
        data = streamData.data();
        if (readCnt > 0)
        {
            itr->second.m_gotData = true;
            rez = demuxerData.m_demuxer->getLastReadRez();
            // ** THE END COMES WITH THE LAST BLOCK, NOT WITH ONE BEFORE IT. ** The demuxer can reach the end of its
            // file with more than a block of this track still held. The first block was handed on as the end, the
            // reader took no more, and the rest of the track was lost. An AC-3 track alone in a Matroska file lost up
            // to 30 KB at its end, without a word, when the last read of the file came just under a block. An ASS
            // track drawn with libass gives the pictures of its last event at the end of its file, 2.4 MB in one
            // packet for a fade with a move: the last animation was missing and the job ended with "The mux stopped
            // before the source was finished".
            if (rez == DATA_EOF && streamData.size() > nFileBlockSize)
                rez = 0;
        }
        else if (demuxerData.m_demuxer->getLastReadRez() == DATA_EOF)
            rez = DATA_EOF;
        else
        {
            if (policy == DemuxerReadPolicy::drpReadSequence && !((waitsForStart || waitsInGap) && demuxRez == 0))
                rez = DATA_NOT_READY;
            else
                rez = DATA_DELAYED;
        }
        demuxerData.lastReadRez[pid] = rez;
    }
    else
        rez = DATA_DELAYED;
    return data;
}

void ContainerToReaderWrapper::terminate()
{
    m_terminated = true;
    for (const auto& demuxer : m_demuxers) demuxer.second.m_demuxer->terminate();
}

const uint8_t* ContainerToReaderWrapper::getTrackCodecPrivate(const uint32_t readerID, int& size)
{
    const auto it = m_readerInfo.find(readerID);
    if (it == m_readerInfo.end())
    {
        size = 0;
        return nullptr;
    }
    const ReaderInfo& ri = it->second;
    return ri.m_demuxerData.m_demuxer->getTrackCodecPrivate(ri.m_pid, size);
}

void ContainerToReaderWrapper::setReadAhead(const uint32_t readerID, const int64_t until)
{
    const auto it = m_readerInfo.find(readerID);
    if (it != m_readerInfo.end())
        it->second.m_readAheadTo = until;
}

std::vector<int64_t> ContainerToReaderWrapper::takeTouchingCues(const uint32_t readerID)
{
    const auto it = m_readerInfo.find(readerID);
    if (it == m_readerInfo.end())
        return {};
    const ReaderInfo& ri = it->second;
    return ri.m_demuxerData.m_demuxer->takeTouchingCues(ri.m_pid);
}

std::vector<std::pair<int64_t, int64_t>> ContainerToReaderWrapper::takeNextCueStarts(const uint32_t readerID)
{
    const auto it = m_readerInfo.find(readerID);
    if (it == m_readerInfo.end())
        return {};
    const ReaderInfo& ri = it->second;
    return ri.m_demuxerData.m_demuxer->takeNextCueStarts(ri.m_pid);
}

std::vector<std::pair<int64_t, int64_t>> ContainerToReaderWrapper::takeTimeGaps(const uint32_t readerID)
{
    const auto it = m_readerInfo.find(readerID);
    if (it == m_readerInfo.end())
        return {};
    const ReaderInfo& ri = it->second;
    return ri.m_demuxerData.m_demuxer->takeTimeGaps(ri.m_pid);
}

int64_t ContainerToReaderWrapper::getTrackClipStart(const uint32_t readerID)
{
    const auto it = m_readerInfo.find(readerID);
    if (it == m_readerInfo.end())
        return 0;
    const ReaderInfo& ri = it->second;
    return ri.m_demuxerData.m_demuxer->getTrackClipStart(ri.m_pid);
}

void ContainerToReaderWrapper::resetDelayedMark() const
{
    for (auto& itr : m_readerInfo)
    {
        DemuxerData& demuxerData = itr.second.m_demuxerData;
        for (auto& itr2 : demuxerData.lastReadRez)
        {
            if (itr2.second == DATA_DELAYED)
                itr2.second = 0;
        }
    }
}

int ContainerToReaderWrapper::createReader(const int readBuffOffset)
{
    m_readBuffOffset = readBuffOffset;
    return ++m_readerCnt;
}

void ContainerToReaderWrapper::deleteReader(const int readerID)
{
    const auto itr = m_readerInfo.find(readerID);
    if (itr == m_readerInfo.end())
        return;
    const ReaderInfo& ri = itr->second;
    ri.m_demuxerData.m_pids.erase(ri.m_pid);
    if (ri.m_demuxerData.m_pids.empty())
    {
        delete ri.m_demuxerData.m_demuxer;
        m_demuxers.erase(ri.m_demuxerData.m_streamName);
    }
    m_readerInfo.erase(itr);
}

bool ContainerToReaderWrapper::openStream(int readerID, const char* streamName, int pid, const CodecInfo* codecInfo)
{
    AbstractDemuxer* demuxer = m_demuxers[streamName].m_demuxer;
    if (demuxer == nullptr)
    {
        string ext = strToUpperCase(extractFileExt(streamName));
        if ((ext == "264" || ext == "H264" || ext == "MVC") && pid)
        {
            demuxer = m_demuxers[streamName].m_demuxer = new CombinedH264Demuxer(m_readManager, "");
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T" || ext == "SSIF")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new TSDemuxer(m_readManager, "");
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "EVO" || ext == "VOB" || ext == "MPG" || ext == "MPEG")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new ProgramStreamDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new MatroskaDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "MOV" || ext == "MP4" || ext == "M4V" || ext == "M4A")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new MovDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else
            THROW(ERR_UNSUPPORTER_CONTAINER_FORMAT, "Unsupported container format: " << streamName)
        demuxer->setFileIterator(m_demuxers[streamName].m_iterator);

        demuxer->openFile(streamName);
    }

    if (SubTrackFilter::isSubTrack(pid))
    {
        if (!demuxer || !demuxer->isPidFilterSupported())
            THROW(ERR_INVALID_CODEC_FORMAT, "Unsupported parameter subTrack for format " << extractFileExt(streamName))
        int srcPID = pid >> 16;
        if (demuxer->getPidFilter(srcPID) == nullptr)
        {
            if (codecInfo->codecID == CODEC_V_MPEG4_H264 || codecInfo->codecID == CODEC_V_MPEG4_H264_DEP)
                demuxer->setPidFilter(srcPID, new CombinedH264Filter(srcPID));
            else if (codecInfo->codecID == CODEC_V_MPEG4_H265)
            {
                // A merged dual layer Dolby Vision track, split back into base layer and
                // enhancement layer. subTrack=1 is the base layer, subTrack=2 the enhancement.
                auto dvFilter = new HevcDolbyVisionFilter(srcPID);
                // If the file was written as profile 8.1 its RPUs were converted, and the disc's
                // own are attached beside them. Load them now, before anything is written, so a
                // file that no longer matches its metadata fails at once instead of half way
                // through authoring a disc.
                loadDvOriginalRpus(demuxer, dvFilter);
                demuxer->setPidFilter(srcPID, dvFilter);
            }
            else
                THROW(ERR_INVALID_CODEC_FORMAT, "Unsupported parameter subTrack for codec " << codecInfo->displayName)
        }
    }

    auto tsDemuxer = dynamic_cast<TSDemuxer*>(demuxer);
    if (tsDemuxer)
    {
        tsDemuxer->setReportDamage();
        auto itr = m_owner.m_mplsStreamMap.find(streamName);
        if (itr != m_owner.m_mplsStreamMap.end())
            tsDemuxer->setMPLSInfo(itr->second.m_playItems);
    }

    // A track copied as it is: every block of it from now on, and in every later file of a join
    if (codecInfo && codecInfo->codecID == CODEC_S_MKVCOPY)
        if (auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxer))
            mkv->setTrackCopy(pid);
    if (const auto ass = m_assDrawing.find({streamName, pid}); ass != m_assDrawing.end())
        if (auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxer))
            mkv->setAssDrawing(pid, ass->second);

    if (codecInfo && (codecInfo->codecID == CODEC_S_PGS || codecInfo->codecID == CODEC_S_SUP ||
                      codecInfo->codecID == CODEC_S_SRT || codecInfo->codecID == CODEC_S_MKVCOPY))
        m_demuxers[streamName].m_pids[pid] = DemuxerReadPolicy::drpFragmented;
    else
    {
        m_demuxers[streamName].m_pids[pid] = DemuxerReadPolicy::drpReadSequence;
        m_demuxers[streamName].m_allFragmented = false;
    }
    m_demuxers[streamName].m_pidSet.insert(pid);
    m_readerInfo.insert(std::make_pair(readerID, ReaderInfo(m_demuxers[streamName], pid)));
    return true;
}

void ContainerToReaderWrapper::setFileIterator(const char* streamName, FileNameIterator* itr)
{
    if (m_demuxers[streamName].m_iterator == nullptr)
        m_demuxers[streamName].m_iterator = itr;
}
