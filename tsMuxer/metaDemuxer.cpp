#include "metaDemuxer.h"

#include <fs/directory.h>
#include <fs/systemlog.h>

#include <fs/textfile.h>
#include <types/types.h>
#include <algorithm>
#include <climits>
#include <cmath>
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
#include "dtsStreamReader.h"
#include "dvbSubStreamReader.h"
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

static constexpr int MAX_DEMUX_BUFFER_SIZE = 1024 * 1024 * 192;
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
    }
    return NamedAs::ctUnknown;
}

static void checkNameMatchesContent(const std::string& fileName, const std::string& ext)
{
    const NamedAs named = familyOfExt(ext);
    if (named == NamedAs::ctUnknown)
        return;

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
    : m_containerReader(*this, readManager), m_readManager(readManager)
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
            if (!m_flushDataMode)
            {
                if (m_codecInfo[minDtsIndex].lastReadRez != BufferedFileReader::DATA_EOF2)
                {
                    const int res = m_codecInfo[minDtsIndex].m_streamReader->readPacket(avPacket);
                    m_codecInfo[minDtsIndex].m_lastAVRez = res;
                }
                else
                {
                    // flush single stream
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
            }
            else
            {  // flush all streams
                m_codecInfo[minDtsIndex].m_streamReader->flushPacket(avPacket);
                m_codecInfo[minDtsIndex].m_flushed = true;
            }
            updateReport(true);
            return 0;
        }
        if (!m_flushDataMode)
            m_flushDataMode = true;
        else
        {
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
        str = trimStr(str);
        if (str.length() == 0 || str[0] == '#')
        {
            file.readLine(str);
            continue;
        }
        if (strStartWith(str.substr((std::min)(str.find_first_not_of('	'), str.size())), "MUXOPT"))
        {
            file.readLine(str);
            continue;
        }
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
            addParams[paramName] = trimStr(eq == string::npos ? "" : params[i].substr(eq + 1));
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
        codec = strToUpperCase(codec);
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
        addStream(codec, codecStreamName, addParams);
        file.readLine(str);
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
}

std::string METADemuxer::mplsTrackToFullName(const std::string& mplsFileName, const std::string& mplsNum)
{
    string path = toNativeSeparators(extractFilePath(mplsFileName));
    // A bare name with no directory means the playlist is in the working directory, so the disc
    // root is one above it. See getBlurayStreamDir in main.cpp, which had the same gap.
    const size_t tmp = path.find_last_of(getDirSeparator());
    if (tmp == string::npos && !path.empty())
        return {};
    path = (path.empty() ? string("..") + getDirSeparator() : path.substr(0, tmp + 1)) + string("STREAM") +
           getDirSeparator();

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string m2tsExt = (mplsExt == "mpls") ? "m2ts" : "mts";

    return path + mplsNum + string(".") + m2tsExt;
}

std::string METADemuxer::mplsTrackToSSIFName(const std::string& mplsFileName, const std::string& mplsNum)
{
    string path = toNativeSeparators(extractFilePath(mplsFileName));
    // A bare name with no directory means the playlist is in the working directory, so the disc
    // root is one above it. See getBlurayStreamDir in main.cpp, which had the same gap.
    const size_t tmp = path.find_last_of(getDirSeparator());
    if (tmp == string::npos && !path.empty())
        return {};
    path = (path.empty() ? string("..") + getDirSeparator() : path.substr(0, tmp + 1)) + string("STREAM") +
           getDirSeparator() + string("SSIF") + getDirSeparator();

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string ssifExt = mplsExt == "mpls" ? "ssif" : "sif";

    return path + mplsNum + string(".") + ssifExt;
}

int METADemuxer::addPGSubStream(const string& codec, const string& _codecStreamName,
                                const map<string, string>& addParams, const MPLSStreamInfo* subStream)
{
    map<string, string> params = addParams;
    params["track"] = int32ToStr(subStream->streamPID);
    if (subStream->type == 2)
        params["subClip"] = "1";
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

int METADemuxer::addStream(const string& codec, const string& codecStreamName, const map<string, string>& addParams)
{
    int32_t pid = 0;
    auto tmpitr = addParams.find("track");
    if (tmpitr != addParams.end())
        pid = strToInt32(tmpitr->second.c_str());

    tmpitr = addParams.find("subTrack");
    if (tmpitr != addParams.end())
        pid = SubTrackFilter::pidToSubPid(pid, strToInt32(tmpitr->second));

    vector<string> fileList;
    string unquotedStreamName = unquoteStr(codecStreamName);
    string fileExt = strToLowerCase(extractFileExt(codecStreamName));
    std::vector<MPLSParser> mplsInfoList;

    // filds for SS PG
    int leftEyeSubStreamIdx = -1;
    int rightEyeSubStreamIdx = -1;
    bool isSSPG = false;
    int ssPGOffset = 0xff;
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
            }

            MPLSStreamInfo streamInfo = mplsInfo.getStreamByPID(pid);
            if (streamInfo.stream_coding_type == StreamType::SUB_PGS && streamInfo.isSSPG)
            {
                // add refs to addition tracks for stereo subtitles
                leftEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, addParams, streamInfo.leftEye);
                rightEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, addParams, streamInfo.rightEye);

                isSSPG = streamInfo.isSSPG;
                ssPGOffset = streamInfo.SS_PG_offset_sequence_id;
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

    for (int i = static_cast<int>(fileList.size()) - 1; i >= 0; --i)
    {
        string trackKey = fileList[i] + string("_#") + int32ToStr(i) + string("_");
        if (addParams.find("subClip") == addParams.end())
            trackKey += string("_track_");
        else
            trackKey += string("_subtrack_");
        trackKey += int32ToStr(pid);
        if (m_processedTracks.find(trackKey) != m_processedTracks.end())
        {
            fileList.erase(fileList.begin() + i);
            if (static_cast<size_t>(i) < timelineRanges.size())
                timelineRanges.erase(timelineRanges.begin() + i);
        }
        else
            m_processedTracks.insert(trackKey);
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
    if (fileList.size() > 1 || !timelineRanges.empty())
    {
        listIterator = new FileListIterator();
        m_iterators.push_back(listIterator);
        for (size_t i = 0; i < fileList.size(); ++i)
            if (i < timelineRanges.size())
                listIterator->addFile(fileList[i], timelineRanges[i]);
            else
                listIterator->addFile(fileList[i]);
    }

    int64_t fileSize = 0;

    if (m_containerReader.m_demuxers.find(fileList[0]) == m_containerReader.m_demuxers.end())
    {
        File tmpFile;
        for (const string& fileName : fileList)
        {
            if (!tmpFile.open(fileName.c_str(), File::ofRead))
                THROW(ERR_INVALID_CODEC_FORMAT, "Can't open file: " << fileName.c_str())
            int64_t tmpSize = 0;
            tmpFile.size(&tmpSize);
            fileSize += tmpSize;
            tmpFile.close();
        }
    }

    // delPulldown on MPEG-2 promises that the frame rate goes from 30 to 24 when pulldown is present.
    // Only the window kept that promise, by writing fps=24000/1001 into the meta itself. A meta
    // written by hand, or the command line, cleared the repeat flags and kept 29.97, so film played
    // a quarter too fast. Ask the stream, as the listing does, and supply the rate when it is
    // pulldown and nobody chose one.
    map<string, string> readerParams = addParams;
    if (codec == "V_MPEG-2" && addParams.count("delPulldown") && !addParams.count("fps"))
    {
        const DetectStreamRez probe = DetectStreamReader(m_readManager, fileList[0], false);
        for (const CheckStreamRez& s : probe.streams)
        {
            if (s.codecInfo.codecID != CODEC_V_MPEG2 || (pid && s.trackID != pid))
                continue;
            const size_t at = s.streamDescr.find("Frame rate: ");
            if (s.streamDescr.find("(pulldown)") == string::npos || at == string::npos)
                break;
            const double streamFps = strToDouble(s.streamDescr.substr(at + 12).c_str());
            const char* filmFps = fabs(streamFps - 29.97) < 0.01  ? "24000/1001"
                                  : fabs(streamFps - 30.0) < 0.01 ? "24"
                                                                  : nullptr;
            if (filmFps)
            {
                readerParams["fps"] = filmFps;
                LTRACE(LT_INFO, 2,
                       "delPulldown: the MPEG-2 stream carries pulldown at " << streamFps << " fps, so it is muxed at "
                                                                             << filmFps << " fps.");
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
                    id == "S_TEXT/ASS" || id == "S_TEXT/SSA" || id == "S_TEXT/WEBVTT" || id == "D_WEBVTT/SUBTITLES";
                const bool bitmap = id == "S_VOBSUB" || id == "S_DVBSUB";
                const bool copy = !m_matroskaOutput ? text && codec == "S_TEXT/UTF8"
                                  : m_matroskaOutput == 2
                                      ? id == "D_WEBVTT/SUBTITLES"
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
    if (codecReader == nullptr)
        codecReader = createCodec(codec, readerParams, fileList[0], mergePlayItems(mplsInfoList));
    codecReader->setStreamIndex(static_cast<int>(m_codecInfo.size() + 1));
    codecReader->setStreamName(fileList[0]);
    codecReader->setTimeOffset(m_timeOffset);

    if (m_codecInfo.empty())
        codecReader->m_flags |= AVPacket::PCR_STREAM;

    AbstractReader* dataReader;
    string tmpname = strToLowerCase(fileList[0]);
    tmpname = unquoteStr(trimStr(tmpname));

    checkNameMatchesContent(unquoteStr(trimStr(fileList[0])), strToLowerCase(extractFileExt(tmpname)));

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
            dataReader = &m_containerReader;
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
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MKV container need track parameter.")
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
        dataReader = m_readManager.getReader(fileList[0].c_str());
    }
    if (dataReader == nullptr)
    {
        delete codecReader;
        THROW(ERR_INVALID_CODEC_FORMAT, "This version do not support multicast or other network steams for muxing")
    }

    m_codecInfo.emplace_back(dataReader, codecReader, fileList[0], codecStreamName, pid, isSubStream);
    if (listIterator)
    {
        auto fileReader = dynamic_cast<BufferedFileReader*>(dataReader);
        if (fileReader)
            fileReader->setFileIterator(listIterator, m_codecInfo.rbegin()->m_readerID);
    }

    StreamInfo& streamInfo = *m_codecInfo.rbegin();
    // The track's own UID in its source, for the tags that source has about it. Its header has been read by now, and
    // asking reads nothing else. A sub-track is half of a track the output writes as one, or two of: it gets none.
    if (dataReader == &m_containerReader && pid > 0 && !SubTrackFilter::isSubTrack(pid))
        if (const auto it = m_containerReader.m_demuxers.find(fileList[0]);
            it != m_containerReader.m_demuxers.end() && it->second.m_demuxer)
            streamInfo.m_srcTrackUid = it->second.m_demuxer->getTrackUid(pid);
    // A video or audio track of a Matroska output keeps the BlockAdditions of its frames: asked for before anything of
    // the file is read, so that none is missed and none is kept for a track nobody muxes
    if (m_matroskaOutput && dataReader == &m_containerReader && pid > 0 && !SubTrackFilter::isSubTrack(pid) &&
        (codec[0] == 'V' || codec[0] == 'A'))
        if (const auto it = m_containerReader.m_demuxers.find(fileList[0]);
            it != m_containerReader.m_demuxers.end() && it->second.m_demuxer)
        {
            uint64_t maxId = 0;
            std::vector<std::vector<uint8_t>> mappings;
            if (it->second.m_demuxer->getBlockAdditionInfo(pid, maxId, mappings))  // the header declares them
                it->second.m_demuxer->keepBlockAdditions(pid);
        }
    streamInfo.m_codec = codec;
    streamInfo.m_addParams = addParams;
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
        // A .sup file: its segments carry the "PG" marker that the reader checks and finds its way back on.
        if (strEndWith(tmpname, ".sup"))
            pgStream->setSrcContainerType(AbstractStreamReader::ContainerType::ctSUP);
    }
    //

    auto itr = addParams.find("timeshift");
    if (itr != addParams.end())
    {
        string timeShift = itr->second;
        size_t pos = 0;
        int64_t coeff = 1;
        int64_t value = 0;
        // "ns" is tested before "s": the other order read 500ns as 500 seconds.
        if ((pos = timeShift.find("ms")) != std::string::npos)
        {
            coeff = 1000000;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else if ((pos = timeShift.find("ns")) != std::string::npos)
        {
            coeff = 1;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else if ((pos = timeShift.find('s')) != std::string::npos)
        {
            coeff = 1000000000;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else
        {
            coeff = 1000000;
            value = strToInt32(timeShift.c_str());
        }
        value = value * coeff / 1000 * INTERNAL_PTS_FREQ / 1000000;
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
            LTRACE(LT_INFO, 2, "Applying delay " << delayMs << " ms taken from the file name");
        }
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
        LTRACE(LT_INFO, 2, "Marking this subtitle as the forced track, taken from the file name");
    }

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
            streamInfo.m_lang = itr->second;
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
        LTRACE(LT_INFO, 2, "Applying language " << streamInfo.m_lang << " taken from the file name");
    }
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
        if (const auto demuxerIt = fileList.size() == 1 ? m_containerReader.m_demuxers.find(fileList[0])
                                                        : m_containerReader.m_demuxers.end();
            demuxerIt != m_containerReader.m_demuxers.end() && demuxerIt->second.m_demuxer)
        {
            std::map<int32_t, TrackInfo> trackList;
            demuxerIt->second.m_demuxer->getTrackList(trackList);
            // A line naming a track the file does not have, or one its reader leaves out, made an empty output: the
            // job went on, said it was successful, and warned that the codec on the line was not the codec of the
            // track. It stops here, before anything is written. Only for Matroska and MP4, whose headers list every
            // track; a transport or program stream is listed from its first part only, and a track may start later.
            AbstractDemuxer* const fileDemuxer = demuxerIt->second.m_demuxer;
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
            if (const auto trackIt = trackList.find(pid); trackIt != trackList.end())
            {
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
                // Under their own keys, which only the Matroska muxer reads, and never over what the meta says.
                for (const auto& [key, value] : trackIt->second.m_srcParams)
                    if (addParams.find(key) == addParams.end())
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
    // ** THE SOUND OF AN MP4 STARTS WHERE ITS EDIT LIST SAYS. ** An encoder puts start-up samples in front of the sound
    // (1024 for AAC, 256 for AC-3, about 1100 for a LAME MP3), and the MP4 says in its edit list to start that much
    // into the track. That was ignored: the first frame, start-up samples and all, was placed with the first picture,
    // and the sound came that much late against the picture, 21 ms for AAC. The track's times are now moved back by it,
    // as a timeshift would move them, so the start-up samples come before the first picture, where the MP4 has them. A
    // Matroska output of AAC says it with CodecDelay instead (MatroskaMuxer), and so is left alone here.
    if (const auto delay = streamInfo.m_addParams.find("srccodecdelay"); delay != streamInfo.m_addParams.end() &&
                                                                         !codec.empty() && codec[0] == 'A' &&
                                                                         !(m_matroskaOutput && codec == "A_AAC"))
    {
        const int64_t ns = strtoll(delay->second.c_str(), nullptr, 10);
        if (ns > 0)
        {
            streamInfo.m_timeShift -= static_cast<int64_t>(static_cast<double>(ns) * INTERNAL_PTS_FREQ / 1e9 + 0.5);
            LTRACE(LT_INFO, 2,
                   "Track " << pid << " of " << unquoteStr(fileList[0]) << ": its edit list starts the sound "
                            << doubleToStr(static_cast<double>(ns) / 1e6, 3)
                            << " ms into the track, so its times are moved back by that much.");
        }
    }
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
        "track", "subTrack", "lang", "timeshift", "default", "secondary", "subClip", "track-name",
        // video
        "fps", "ar", "level", "video-width", "video-height", "insertSEI", "forceSEI", "autoSEI", "contSPS",
        "delPulldown", "stretch", "3d-plane", "hdr10-override",
        // audio
        "down-to-ac3", "down-to-dts", "merge-ac3-track", "merge-ac3-file", "drop-ac3-core", "split-ac3-core", "w64",
        // picture in picture
        "pipCorner", "pipHOffset", "pipVOffset", "pipScale", "pipLumma",
        // text subtitles
        "font-name", "font-size", "font-color", "font-bold", "font-italic", "font-underline", "font-strike-out",
        "font-border", "font-charset", "line-spacing", "bottom-offset", "fadein-time", "fadeout-time", "keep-text",
        // set internally, never written by a user, but accepted so a round tripped meta does
        // not warn. srcdefault is the source container's own default track flag.
        "forced", "hearing-impaired", "visual-impaired", "text-descriptions", "original", "commentary", "srclang",
        "srclangbcp47", "srcdefault", "srcname", "srcflags", "srccolour", "srcdisplay", "srcendpadding", "srcpgsrate",
        "srccrop", "srcfieldorder", "srcprojection", "srccodecdelay", "langbcp47", "srcvpxprivate", "srccodecid"};
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
        if (!demuxerData.m_demuxer || demuxerData.m_demuxer->getChapters().empty())
            continue;
        auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxerData.m_demuxer);
        const auto* join = dynamic_cast<const FileListIterator*>(demuxerData.m_iterator);
        if (!mkv || (join && (join->getFiles().size() > 1 ||
                              (!join->getRanges().empty() && join->getRanges().front().startNs >= 0))))
            return {};
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

int64_t METADemuxer::statedDurationNano()
{
    int64_t longest = 0;
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

DetectStreamRez METADemuxer::DetectStreamReader(const BufferedReaderManager& readManager, const string& fileName,
                                                bool calcDuration)
{
    AVChapters chapters;
    int64_t fileDuration = 0;
    int64_t videoStart = 0, videoEnd = 0;
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

        for (unsigned i = 0; i < DETECT_STREAM_BUFFER_SIZE / fileBlockSize; i++)
            demuxer->simpleDemuxBlock(demuxedData, acceptedPidSet, discardedSize);

        for (auto& itr : demuxedData)
        {
            StreamData& vect = itr.second;
            CheckStreamRez trackRez = detectTrackReader(vect.data(), static_cast<int>(vect.size()), containerType,
                                                        acceptedPidMap[itr.first].m_trackType, itr.first);
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
                    const double containerFps = correctFps(demuxer->getTrackFps(itr.first));
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
        else if (fileExt == "srt")
            containerType = AbstractStreamReader::ContainerType::ctSRT;
        CheckStreamRez trackRez = detectTrackReader(tmpBuffer.get(), len, containerType, 0, 0);

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
static double parseFpsValue(const std::string& text)
{
    const size_t slash = text.find('/');
    if (slash == std::string::npos)
        return strToDouble(text.c_str());

    const double num = strToDouble(text.substr(0, slash).c_str());
    const double den = strToDouble(text.substr(slash + 1).c_str());
    if (den <= 0 || num <= 0)
    {
        LTRACE(LT_WARN, 2, "Ignoring fps=" << text << ": it is not a usable fraction");
        return 0;
    }
    return num / den;
}

CheckStreamRez METADemuxer::detectTrackReader(uint8_t* tmpBuffer, int len,
                                              AbstractStreamReader::ContainerType containerType, int containerDataType,
                                              int containerStreamIndex)
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
    // An IVF file, the elementary file of VP8 and VP9, says what it holds in its header.
    if (len >= 32 && memcmp(tmpBuffer, "DKIF", 4) == 0)
        for (const bool vp9 : {false, true})
            if (rez = VpxStreamReader(vp9).checkStream(tmpBuffer, len); rez.codecInfo.codecID)
                return rez;
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

    auto lpcmReader = std::make_unique<LPCMStreamReader>();
    rez = lpcmReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto h264codec = std::make_unique<H264StreamReader>();
    rez = h264codec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto dtscodec = std::make_unique<DTSStreamReader>();
    rez = dtscodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto ac3codec = std::make_unique<AC3StreamReader>();
    rez = ac3codec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto mlpcodec = std::make_unique<MLPStreamReader>();
    rez = mlpcodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto aaccodec = std::make_unique<AACStreamReader>();
    rez = aaccodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto flacCodec = std::make_unique<FLACStreamReader>();
    rez = flacCodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto opusCodec = std::make_unique<OpusStreamReader>();
    rez = opusCodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

    auto vc1ccodec = std::make_unique<VC1StreamReader>();
    rez = vc1ccodec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto hevcCodec = std::make_unique<HEVCStreamReader>();
    rez = hevcCodec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto vvcCodec = std::make_unique<VVCStreamReader>();
    rez = vvcCodec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto mpeg2ccodec = std::make_unique<MPEG2StreamReader>();
    rez = mpeg2ccodec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto av1Codec = std::make_unique<AV1StreamReader>();
    rez = av1Codec->checkStream(tmpBuffer, len);
    if (rez.codecInfo.codecID)
        return rez;

    auto mpegAudioCodec = std::make_unique<MpegAudioStreamReader>();
    rez = mpegAudioCodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    if (rez.codecInfo.codecID)
        return rez;

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

    return VideoAspectRatio::AR_KEEP_DEFAULT;
}

PIPParams::PipCorner pipCornerFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));
    if (v == "topleft")
        return PIPParams::PipCorner::TopLeft;
    if (v == "topright")
        return PIPParams::PipCorner::TopRight;
    if (v == "bottomright")
        return PIPParams::PipCorner::BottomRight;

    return PIPParams::PipCorner::BottomLeft;
}

int pipScaleFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));

    if (v == "1/2" || v == "0.5")
        return 2;
    if (v == "1/4" || v == "0.25")
        return 3;
    if (v == "1.5")
        return 4;
    if (v == "fullscreen")
        return 5;

    return 1;  // default
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
            // Rounded after the x10: the cast came first, so level=4.1 gave 4.0 and 5.1 gave 5.0.
            dynamic_cast<H264StreamReader*>(rez)->setForceLevel(
                static_cast<uint8_t>(lround(strToDouble(itr->second.c_str()) * 10)));
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
            // HEVC counts levels in thirtieths, unlike H.264's tenths: level 5.1 is 153, not 51.
            const double level = strToDouble(itr->second.c_str());
            if (level < 1.0 || level > 8.5)
                THROW(ERR_COMMON, "Invalid HEVC level " << itr->second << ", expected a value like 5.1")
            dynamic_cast<HEVCStreamReader*>(rez)->setForceLevel(static_cast<uint8_t>(lround(level * 30.0)));
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
        if (addParams.find("down-to-ac3") != addParams.end() && !quiet)
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
        if (addParams.find("merge-ac3-track") != addParams.end() || addParams.find("merge-ac3-file") != addParams.end())
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
            dynamic_cast<PGSStreamReader*>(rez)->setOffsetId(strToInt8u(itr->second.c_str()));

        double fps = 0.0;
        uint16_t width = 0;
        uint16_t height = 0;
        itr = addParams.find("fps");
        if (itr != addParams.end())
            fps = parseFpsValue(itr->second);
        itr = addParams.find("video-width");
        if (itr != addParams.end())
            width = strToInt16u(itr->second.c_str());
        itr = addParams.find("video-height");
        if (itr != addParams.end())
            height = strToInt16u(itr->second.c_str());
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
                font.m_size = strToInt32(addParam.second.c_str());
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
                const string& s = addParam.second;
                if (s.size() >= 2 && s[0] == '0' && s[1] == 'x')
                    font.m_color = strToInt32u(s.substr(2, s.size() - 2).c_str(), 16);
                else if (!s.empty() && s[0] == 'x')
                    font.m_color = strToInt32u(s.substr(1, s.size() - 1).c_str(), 16);
                else
                    font.m_color = strToInt32u(s.c_str());
                if ((font.m_color & 0xff000000u) == 0)
                    font.m_color |= 0xff000000u;
            }
            else if (addParam.first == "line-spacing")
                font.m_lineSpacing = strToFloat(addParam.second.c_str());
            else if (addParam.first == "font-charset")
                font.m_charset = strToInt32(addParam.second.c_str());
            else if (addParam.first == "font-border")
                font.m_borderWidth = strToFloat(addParam.second.c_str());
            else if (addParam.first == "fps")
            {
                fps = parseFpsValue(addParam.second);
            }
            else if (addParam.first == "video-width")
                srtWidth = strToInt16u(addParam.second.c_str());
            else if (addParam.first == "video-height")
                srtHeight = strToInt16u(addParam.second.c_str());
            else if (addParam.first == "bottom-offset")
                srtReader->setBottomOffset(strToInt32(addParam.second.c_str()));
            else if (addParam.first == "fadein-time")
                animation.fadeInDuration = strToFloat(addParam.second.c_str());
            else if (addParam.first == "fadeout-time")
                animation.fadeOutDuration = strToFloat(addParam.second.c_str());
            else if (addParam.first == "keep-text")
                srtReader->setKeepText(true);
        }
        // The picture size and frame rate are only needed to draw the subtitles. Kept as text they
        // are not drawn, unless the output cannot carry text, and that output says so when it is.
        if (!srtReader->keepText() && (srtWidth == 0 || srtHeight == 0 || fps == 0.0))
            THROW(ERR_COMMON, "video-width, video-height and fps parameters MUST be provided for SRT tracks")
        if (srtWidth && srtHeight && fps != 0.0)
            srtReader->setVideoInfo(srtWidth, srtHeight, fps);
        srtReader->setFont(font);
        srtReader->setAnimation(animation);
    }
    else
        THROW(ERR_UNKNOWN_CODEC, "Unsupported codec " << codecName)

    if (codecName[0] == 'A')
    {
        auto itr = addParams.find("stretch");
        if (itr != addParams.end())
        {
            double stretch;
            size_t divPos = itr->second.find('/');
            if (divPos == std::string::npos)
                stretch = strToDouble(itr->second.c_str());
            else
            {
                string firstStr = itr->second.substr(0, divPos);
                string secondStr = itr->second.substr(divPos + 1, itr->second.size() - divPos - 1);
                double dFirst = strToDouble(firstStr.c_str());
                double dSecond = strToDouble(secondStr.c_str());
                if (dSecond != 0.0)
                    stretch = dFirst / dSecond;
                else
                    THROW(ERR_COMMON, "Second argument at stretch parameter can not be 0")
            }
            if (itr != addParams.end())
                dynamic_cast<SimplePacketizerReader*>(rez)->setStretch(stretch);
        }
    }
    auto itr = addParams.find("secondary");
    if (itr != addParams.end())
        rez->setIsSecondary(true);

    PIPParams pipParams;
    itr = addParams.find("pipCorner");
    if (itr != addParams.end())
        pipParams.corner = pipCornerFromStr(itr->second);
    itr = addParams.find("pipHOffset");
    if (itr != addParams.end())
        pipParams.hOffset = strToInt32(itr->second);
    itr = addParams.find("pipVOffset");
    if (itr != addParams.end())
        pipParams.vOffset = strToInt32(itr->second);
    itr = addParams.find("pipLumma");
    if (itr != addParams.end())
        pipParams.lumma = strToInt32(itr->second);
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
                THROW(ERR_COMMON, "Can't parse play list file " << i)
            m_mplsStreamMap[i] = parser;
            result.push_back(parser);
        }
    }
    return result;
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
        double progress = 100.0;
        if (m_totalSize > 0)
        {
            const int64_t currentProcessedSize = getDemuxedSize();
            progress = static_cast<double>(currentProcessedSize) / static_cast<double>(m_totalSize) * 100.0;
            if (progress > 100.0)
                progress = 100.0;
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
    if (!sLastMsg)
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

        m_data = m_dataReader->readBlock(m_readerID, m_blockSize, readRez);
        if (readRez == BufferedFileReader::DATA_NOT_READY || readRez == BufferedFileReader::DATA_DELAYED)
        {
            m_lastAVRez = readRez;
            return readRez;
        }
        if (readRez == BufferedFileReader::DATA_EOF)
            m_isEOF = true;
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
    if ((readCnt > 0 && (policy == DemuxerReadPolicy::drpFragmented || demuxerData.lastReadCnt[pid] == DATA_EOF2 ||
                         demuxerData.lastReadCnt[pid] == DATA_EOF2)) ||
        readCnt >= MIN_READED_BLOCK)
    {
        data = streamData.data();
        demuxerData.lastReadCnt[pid] = readCnt;
        demuxerData.lastReadRez[pid] = 0;
    }
    else if (demuxerData.lastReadRez[pid] != DATA_DELAYED || demuxerData.m_allFragmented)
    {
        int demuxRez;
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
                    if (ext != "MOV" && ext != "MP4" && ext != "M4V" && ext != "M4A")
                        THROW(ERR_CONTAINER_STREAM_NOT_SYNC,
                              "Reading buffer overflow. Possible container streams are not syncronized. Please, verify "
                              "stream fps. File name: "
                                  << demuxerData.m_streamName)
                }
            }
            m_discardedSize += discardSize;
            readCnt = static_cast<uint32_t>((FFMIN(streamData.size(), nFileBlockSize) - m_readBuffOffset));
        } while (demuxRez == 0 && readCnt < MIN_READED_BLOCK && policy != DemuxerReadPolicy::drpFragmented &&
                 !m_terminated);

        demuxerData.lastReadCnt[pid] = readCnt;
        data = streamData.data();
        if (readCnt > 0)
        {
            rez = demuxerData.m_demuxer->getLastReadRez();
        }
        else if (demuxerData.m_demuxer->getLastReadRez() == DATA_EOF)
            rez = DATA_EOF;
        else
        {
            if (policy == DemuxerReadPolicy::drpReadSequence)
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
        auto itr = m_owner.m_mplsStreamMap.find(streamName);
        if (itr != m_owner.m_mplsStreamMap.end())
            tsDemuxer->setMPLSInfo(itr->second.m_playItems);
    }

    // A track copied as it is: every block of it from now on, and in every later file of a join
    if (codecInfo && codecInfo->codecID == CODEC_S_MKVCOPY)
        if (auto* mkv = dynamic_cast<MatroskaDemuxer*>(demuxer))
            mkv->setTrackCopy(pid);

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
