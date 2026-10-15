#include "muxerManager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fs/systemlog.h>
#include "fs/textfile.h"

#include "chapterFile.h"
#include "h264StreamReader.h"
#include "iso_writer.h"
#include "matroskaMuxer.h"
#include "mpegStreamReader.h"
#include "tsMuxer.h"
#include "vodCoreException.h"

using namespace std;

// static const int SSIF_INTERLEAVE_BLOCKSIZE = 1024 * 1024 * 7;
static constexpr int MAX_FRAME_SIZE = 1200000;  // 1.2m

namespace
{
template <typename TrackTypeFn>
int seekDefaultTrack(const std::vector<StreamInfo>& tracks, std::string& param, TrackTypeFn f)
{
    int trackTypeIdx = 0;
    for (auto&& track : tracks)
    {
        if (f(track))
        {
            auto&& params = track.m_addParams;
            auto it = params.find("default");
            // A track that says default=none is NOT the default track. This used to return the first
            // track that merely carried the key, so default=0 selected it.
            if (it != std::end(params) && !defaultParamMeansNo(it->second))
            {
                param = it->second;
                return trackTypeIdx;
            }
            ++trackTypeIdx;
        }
    }
    return -1;
}
}  // namespace

MuxerManager::MuxerManager(const BufferedReaderManager& readManager, AbstractMuxerFactory& factory)
    : m_metaDemuxer(readManager), m_factory(factory)
{
    m_asyncMode = true;
    m_cutStart = 0;
    m_cutEnd = 0;
    m_allowStereoMux = false;
    m_interleave = false;
    m_subBlockFinished = false;
    m_mainBlockFinished = false;
    m_ptsOffset = 54000000ll;
    m_mvcBaseViewR = false;
    m_extraIsoBlocks = 0;
    m_bluRayMode = false;
    m_demuxMode = false;
}

MuxerManager::~MuxerManager() = default;

void MuxerManager::preinitMux(const std::string& outFileName, FileFactory* fileFactory)
{
    // Blu-ray navigation needs the mux start time at or above 524280 ticks of the 45 kHz
    // clock (11.65 s). Below that, players subtract the decoder preroll from timestamps
    // near zero and their 32-bit 45 kHz registers underflow, so skipping back to the start
    // of the disc fails. Commercial discs never master lower (Universal uses exactly
    // 524280). Raise a lower explicit value for every output except plain *.ts; an absent
    // --start-time already falls back to the safe 600 s default before this point.
    static constexpr int64_t MIN_BD_PTS_OFFSET = 524280ll * 2;  // 45 kHz -> 90 kHz ticks
    // Only a transport-stream mux carries this offset at all. Matroska and the single-file
    // muxers ignore m_ptsOffset, so warning about Blu-ray navigation and rewriting the value
    // for a .mkv would be noise about something that cannot happen.
    const std::string outExt = strToLowerCase(extractFileExt(outFileName));
    const bool tsFamilyOutput =
        outExt != "mkv" && outExt != "mka" && outExt != "mks" && outExt != "mk3d" && outExt != "webm" && outExt != "ts";
    if (!m_demuxMode && tsFamilyOutput && m_ptsOffset < MIN_BD_PTS_OFFSET)
    {
        LTRACE(LT_WARN, 2,
               "--start-time below 524280 (45 kHz clock) breaks Blu-ray navigation; raised to 524280 (11.65 s)");
        m_ptsOffset = MIN_BD_PTS_OFFSET;
    }

    vector<StreamInfo>& ci = m_metaDemuxer.getCodecInfo();

    // A negative timeshift moves a track EARLIER, and if it moves it past the start of the mux
    // the timestamp goes below zero. A PTS field is 33 bits and has no sign, so what got written
    // was the value wrapped round: an audio track asked for 827 ms early against a start time of
    // zero came out at 8,589,860,162, which is 95,442 s, more than a day after the picture. The
    // mux reported success.
    //
    // Move the start of the mux instead. Every track moves by the same amount, so the offset the
    // user asked for between them is exactly what they get, and only the absolute start changes,
    // which is the thing that could not be honoured in the first place.
    int64_t mostNegativeShift = 0;
    for (const StreamInfo& si : ci)
        if (si.m_timeShift < mostNegativeShift)
            mostNegativeShift = si.m_timeShift;
    if (mostNegativeShift < 0)
    {
        static constexpr int64_t INTERNAL_PER_90KHZ = INTERNAL_PTS_FREQ / 90000;
        const int64_t needed = (-mostNegativeShift + INTERNAL_PER_90KHZ - 1) / INTERNAL_PER_90KHZ;
        if (m_ptsOffset < needed)
        {
            LTRACE(LT_WARN, 2,
                   "Warning: a timeshift of " << (-mostNegativeShift * 1000 / INTERNAL_PTS_FREQ)
                                              << " ms starts a track before the start of the mux, which cannot be "
                                                 "written. Raising the start time from "
                                              << m_ptsOffset << " to " << needed
                                              << " (90 kHz clock) so it fits. Use --start-time to choose your own.");
            m_ptsOffset = needed;
        }
    }

    bool mvcTrackFirst = false;
    bool firstH264Track = true;
    for (const StreamInfo& si : ci)
    {
        const auto h264Reader = dynamic_cast<H264StreamReader*>(si.m_streamReader);
        if (h264Reader)
        {
            h264Reader->setStartPTS(m_ptsOffset);
            if (firstH264Track)
            {
                if (si.m_isSubStream)
                    mvcTrackFirst = true;
                firstH264Track = false;
            }
        }
        if (si.m_isSubStream && m_allowStereoMux)
        {
            if (!m_subMuxer)
            {
                m_subMuxer = m_factory.newInstance(this);
                const auto tsMuxer = dynamic_cast<TSMuxer*>(m_subMuxer.get());
                if (tsMuxer)
                    tsMuxer->setPtsOffset(m_ptsOffset);
                m_subMuxer->parseMuxOpt(m_muxOpts);
            }
        }
        else
        {
            if (!m_mainMuxer)
            {
                m_mainMuxer = m_factory.newInstance(this);
                const auto tsMuxer = dynamic_cast<TSMuxer*>(m_mainMuxer.get());
                if (tsMuxer)
                    tsMuxer->setPtsOffset(m_ptsOffset);
                m_mainMuxer->parseMuxOpt(m_muxOpts);
                m_mainMuxer->setSourceExtras(m_metaDemuxer.getTitle(), m_metaDemuxer.getPassThroughAttachments(),
                                             m_metaDemuxer.getGlobalTags());
                m_mainMuxer->setSourceTrackTags(m_metaDemuxer.getTrackTagsByStream());
                m_mainMuxer->setSourceBlockAdditions(m_metaDemuxer.getBlockAdditionInfoByStream());
                // --keep-editions: the source's chapters, every edition of them, and its segment identity, as they
                // stand. They are times on the file's own timeline, which a cut would move, and they replace the
                // chapters the output would otherwise get.
                if (m_keepEditions)
                {
                    auto* const mkv = dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get());
                    if (!mkv)
                        THROW(ERR_COMMON, "--keep-editions applies to a Matroska output (.mkv) only.")
                    if (m_cutStart > 0 || m_cutEnd > 0)
                        THROW(ERR_COMMON,
                              "--keep-editions keeps the source's chapter times, which a cut would move. "
                              "Leave out --cut-start and --cut-end, or --keep-editions.")
                    if (!m_chapters.empty())
                        THROW(ERR_COMMON,
                              "--keep-editions keeps the source's own chapters, and cannot be used with "
                              "--custom-chapters or --chapters-file.")
                    mkv->setSourceEditions(m_metaDemuxer.getSourceEditions());
                }
                // Now that the muxer exists it can be given the chapter list main() supplied.
                // With no --custom-chapters, fall back to whatever the source container carries,
                // the same way a source language fills in for a missing lang=. An MKV with its
                // own chapter list used to lose every mark on a remux. --no-chapters turns this
                // off (see sourceChapters).
                if (m_chapters.empty())
                {
                    // The name comes along with the time. It used to be dropped here, and the
                    // chapters then came out numbered instead of named.
                    m_chapterInfo = sourceChapters();
                    for (const auto& chapter : m_chapterInfo)
                        m_chapters.push_back(static_cast<double>(chapter.start) / 1e9);
                    // A Matroska output also gets every edition of a Matroska source, and its hidden and nested
                    // chapters, moved by the cut the same way (MatroskaMuxer::setSourceChapterTree)
                    if (auto* const mkv = dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get());
                        mkv && !m_chapterInfo.empty())
                    {
                        const auto toNano = [](const int64_t pts) {
                            return static_cast<int64_t>(
                                static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ) + 0.5);
                        };
                        mkv->setSourceChapterTree(m_metaDemuxer.getPlainMatroskaChapters(),
                                                  m_cutStart > 0 ? toNano(m_cutStart) : 0,
                                                  m_cutEnd > 0 ? toNano(m_cutEnd) : INT64_MAX);
                    }
                }
                if (!m_chapters.empty())
                {
                    m_mainMuxer->setChapters(m_chapters);
                    m_mainMuxer->setChapterInfo(m_chapterInfo);
                }
            }
        }
    }

    if (m_mainMuxer)
    {
        m_mainMuxer->setFileName(outFileName, fileFactory);
        // m_mainMuxer->openDstFile();
    }

    if (m_subMuxer)
    {
        m_subMuxer->setFileName(outFileName, fileFactory);
        if (strEndWith(strToLowerCase(outFileName), ".ssif"))
        {
            m_interleave = true;
        }
        else
        {
            m_subMuxer->setFileName(m_subMuxer->getNextName(outFileName), fileFactory);
            if (fileFactory && fileFactory->isVirtualFS())
                m_interleave = true;
        }
        m_subMuxer->setBlockMuxMode(SUB_INTERLEAVE_BLOCKSIZE - MAX_FRAME_SIZE, BLURAY_SECTOR_SIZE);
        if (m_mainMuxer)
            m_mainMuxer->setBlockMuxMode(MAIN_INTERLEAVE_BLOCKSIZE - MAX_FRAME_SIZE, BLURAY_SECTOR_SIZE);
    }

    if (m_subMuxer && m_mainMuxer)
    {
        m_subMuxer->setSubMode(m_mainMuxer.get(), mvcTrackFirst);
        m_mainMuxer->setMasterMode(m_subMuxer.get(), !mvcTrackFirst);
    }

    for (size_t i = 0; i < ci.size(); ++i)
    {
        StreamInfo& si = ci[i];
        si.read();

        // Apply discovery-phase metadata to the stream reader so that
        // getTSDescriptor(), getStreamInfo(), etc. have correct values
        // from the very first packet (channels, sample rate, codec-private,
        // resolution, FPS, etc.).
        if (i < m_discoveryData.size() && m_discoveryData[i].discovered)
        {
            si.m_streamReader->applyDiscoveryData(m_discoveryData[i]);
        }

        // A player seeks a disc through the EP map of its clips, and an H.264 picture gets an entry there only where
        // its group carries the parameter sets (TSMuxer, needSPSForSplit). A stream from MP4 or Matroska holds them
        // once, ahead of its first picture, so a disc made from one had a single entry and could not be seeked: the
        // picture froze or the player started over (#646). For disc output they are now repeated before each key
        // frame where the stream does not repeat them itself, which is what contSPS asks for. A stream that does
        // repeat them, as every disc does, is left exactly as it is. The second view of a 3D stream keeps its own rule.
        if (m_bluRayMode)
        {
            auto* h264 = dynamic_cast<H264StreamReader*>(si.m_streamReader);
            if (h264 != nullptr && !h264->isSubStream())
                h264->setH264SPSCont(true);
        }

        // A language taken from the source container travels under its own key. An explicit lang=
        // is already in m_addParams and must win, so only fill the gap. Keeping it out of "lang"
        // is what stops SingleFileMuxer from renaming demuxed files: that muxer reads only "lang"
        // and never sees this one.
        //
        // In the form asked for with --lang-codes, terminological unless said otherwise, as the track list shows it and
        // as the window writes it. The code went on as the source spelled it, so a Matroska track tagged "ger" became
        // "ger" on a disc from the command line and "deu" from the window. The discs measured all carry the
        // terminological form. A BCP 47 tag the source had travels beside it, for Matroska output.
        if (!si.m_lang.empty() && si.m_addParams.find("lang") == si.m_addParams.end())
        {
            si.m_addParams["srclang"] = applyLangCodeStyle(si.m_lang);
            if (!si.m_langBCP47.empty())
                si.m_addParams["srclangbcp47"] = si.m_langBCP47;
        }

        if (si.m_isSubStream && m_allowStereoMux)
        {
            m_subStreamIndex.insert(si.m_streamReader->getStreamIndex());
            m_subMuxer->intAddStream(si.m_fullStreamName, si.m_codec, si.m_streamReader->getStreamIndex(),
                                     si.m_addParams, si.m_streamReader);
        }
        else
        {
            m_mainMuxer->intAddStream(si.m_fullStreamName, si.m_codec, si.m_streamReader->getStreamIndex(),
                                      si.m_addParams, si.m_streamReader);
        }
    }

    checkTrackList(ci);

    if (m_mainMuxer)
        m_mainMuxer->openDstFile();
    if (m_subMuxer)
    {
        if (!m_interleave || (fileFactory && fileFactory->isVirtualFS()))
            m_subMuxer->openDstFile();
        else
            m_subMuxer->joinToMasterFile();  // direct ssif writing
    }
}

void MuxerManager::checkTrackList(const vector<StreamInfo>& ci) const
{
    checkMatroskaOnlyCodecs(ci, m_demuxMode || !dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get()));
    if (m_demuxMode)
        return;
    checkOtherTracks(ci);
}

// VP8, VP9, Vorbis, FLAC and ALAC have no form in a transport stream or on a Blu-ray disc: a Matroska output is the
// only one that can carry them. A demux writes VP8 and VP9 into an IVF file, Vorbis into an Ogg file and FLAC into a
// .flac file; only ALAC has no file of its own. FLAC was refused by the transport stream writer, after the folders
// of a disc or an ISO image of the same name had been made.
void MuxerManager::checkMatroskaOnlyCodecs(const vector<StreamInfo>& ci, const bool notMatroska) const
{
    if (notMatroska)
        for (const StreamInfo& si : ci)
            for (const CodecInfo* only : {&vp8CodecInfo, &vp9CodecInfo, &vorbisCodecInfo, &flacCodecInfo,
                                          &alacCodecInfo, &mpeg4AspCodecInfo, &proresCodecInfo})
                if (si.m_codec == only->programName &&
                    !(m_demuxMode && only != &alacCodecInfo && only != &mpeg4AspCodecInfo && only != &proresCodecInfo))
                    THROW(ERR_COMMON, only->displayName
                                          << " can only be muxed into a Matroska file (.mkv). It has no form in a "
                                             "transport stream or on a Blu-ray disc, and a demux has no file to write "
                                             "it to. Choose MKV output, or leave the track out.")
}

void MuxerManager::checkOtherTracks(const vector<StreamInfo>& ci) const
{
    bool avcFound = false;
    bool mvcFound = false;
    bool aacFound = false;
    bool mlpFound = false;
    bool mlpMergedWithAc3 = false;
    std::vector<std::string> notOnDisc;  // codecs a Blu-ray player cannot play, in the order of the tracks

    for (const StreamInfo& si : ci)
    {
        if (si.m_codec == h264CodecInfo.programName)
            avcFound = true;
        else if (si.m_codec == h264DepCodecInfo.programName)
            mvcFound = true;
        else if (si.m_codec == aacCodecInfo.programName)
            aacFound = true;
        else if (si.m_codec == mpegAudioCodecInfo.programName)
            notOnDisc.emplace_back("MPEG audio (MP1, MP2 or MP3)");
        else if (si.m_codec == opusCodecInfo.programName)
            notOnDisc.emplace_back("Opus");
        else if (si.m_codec == vvcCodecInfo.programName)
            notOnDisc.emplace_back("VVC (H.266)");
        else if (si.m_codec == av1CodecInfo.programName)
            notOnDisc.emplace_back("AV1");
        else if (si.m_codec == mlpCodecInfo.programName)
        {
            mlpFound = true;
            // TrueHD is BD-compliant when interleaved with an AC-3 core, whether the core
            // comes from another track (merge-ac3-track) or a separate file (merge-ac3-file).
            if (si.m_addParams.find("merge-ac3-track") != si.m_addParams.end() ||
                si.m_addParams.find("merge-ac3-file") != si.m_addParams.end())
                mlpMergedWithAc3 = true;
        }
    }

    // Each warning that applies is printed. They were one chain, so a disc with AAC never heard about a TrueHD
    // track without its core, nor about a single-track Dolby Vision, and MPEG audio, Opus, VVC and AV1, which no
    // Blu-ray player plays either, were not mentioned at all.
    if (m_bluRayMode)
    {
        if (aacFound)
            LTRACE(LT_ERROR, 2,
                   "Warning! AAC codec is not standard for BD disks, the disk will not play in a Blu-ray player.");
        if (mlpFound && !mlpMergedWithAc3)
            LTRACE(LT_ERROR, 2,
                   "Warning! MLP codec is not standard for BD disks, the disk will not play in a Blu-ray player.");
        for (const auto& codec : notOnDisc)
            LTRACE(LT_ERROR, 2,
                   "Warning! " << codec << " is not a Blu-ray format, the disk will not play in a Blu-ray player.");
        if ((V3_flags & DV) && !(V3_flags & BL_TRACK))
            LTRACE(LT_ERROR, 2,
                   "Warning! Dolby Vision Double Layer Single Tracks are not standard for BD disks, the disk will "
                   "not play in a Blu-ray player.");
    }
    if (!avcFound && mvcFound)
        THROW(ERR_INVALID_STREAMS_SELECTED,
              "Fatal error: MVC depended view track can't be muxed without AVC base view track")
}

// A demux already puts a track's language and its delay into the file name. The same request asked
// for the chapters as well, and those cannot go in a file name, so they go in a text file beside
// the tracks.
//
// One time per line, hh:mm:ss.mmm. That is deliberately the form --custom-chapters accepts and the
// form the window's chapter box holds, so the file can be pasted straight back in and the marks
// rebuilt on a new disc without anyone retyping them.
//
// The list itself is not new: METADemuxer::getChapters() already walks a playlist's marks, adding
// each play item's own span as it goes, and falls back to the chapter list of a container such as
// Matroska. The mux path has used it for a while; nothing asked for it on a demux until now.
//
// A source with no chapters writes no file rather than an empty one, and a file that cannot be
// opened is a warning and not a failure: the tracks are the point of the run and they are already
// on disk by then.
// The chapters a source carries are times on the source's own timeline. A cut (--cut-start, --cut-end) keeps only a
// part of it, and the output begins at the cut start, so they were written where they no longer belong: a three-minute
// cut out of a film carried all 81 chapters of the film, at their old times. Only the chapters inside the cut are kept
// now, moved back by the cut start, and the chapter that is already playing where the cut starts opens the output at 0.
std::vector<AVChapter> MuxerManager::sourceChapters()
{
    if (!m_sourceChapters)
        m_sourceChapters = readSourceChapters();
    return *m_sourceChapters;
}

std::vector<AVChapter> MuxerManager::readSourceChapters()
{
    // Asked for an output without the source's chapters. Nothing else said so: with no chapter option at all the
    // source's own were taken over, so a Matroska output got them although the window's "No chapters" was chosen.
    if (m_noChapters)
        return {};
    std::vector<AVChapter> chapters = m_metaDemuxer.getChapters();
    if (m_cutStart <= 0 && m_cutEnd <= 0)
        return chapters;

    // The cut is in the internal clock; double, because the clock times 1e9 does not fit in 64 bits.
    const auto toNano = [](const int64_t pts)
    { return static_cast<int64_t>(static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ) + 0.5); };
    const int64_t startNano = m_cutStart > 0 ? toNano(m_cutStart) : 0;
    const int64_t endNano = m_cutEnd > 0 ? toNano(m_cutEnd) : std::numeric_limits<int64_t>::max();

    std::vector<AVChapter> kept;
    const AVChapter* playing = nullptr;  // the last chapter that starts at or before the cut start
    for (const AVChapter& chapter : chapters)
    {
        if (chapter.start <= startNano)
        {
            if (!playing || chapter.start > playing->start)
                playing = &chapter;
        }
        else if (chapter.start < endNano)
        {
            AVChapter moved = chapter;
            moved.start -= startNano;
            if (moved.end >= 0)
                moved.end -= startNano;
            kept.push_back(std::move(moved));
        }
    }
    if (playing)
    {
        AVChapter first = *playing;
        first.start = 0;
        first.end = first.end > startNano ? first.end - startNano : -1;
        kept.insert(kept.begin(), std::move(first));
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

// The room a disc image keeps for its file system (its metadata area) is set aside before the mux and cannot grow
// later. Every part of a split adds to it: the clip, its clip information and the backup of that, three files, and
// in 3D the second view's clip, its two clip information files and the interleaved file as well. The default held
// about forty parts, and a split into more ran out of room only when the image was finished, after the whole mux:
// 79 parts of a disc feature cut every 2 seconds did. So the parts are reckoned before the mux where that can be done:
// the sources' size over the part size, the length of the cut (or the length the sources state) over the part length,
// or the number of chapters. The room grows only when the default would not hold them, so a split into a few parts
// gives the same image as before. An --extra-iso-space that is given is taken as it is, and a split that could not be
// reckoned still gets the plain refusal when the image is finished.
int MuxerManager::getExtraISOBlocks()
{
    if (m_extraIsoBlocksGiven)
        return m_extraIsoBlocks;
    int64_t parts = 0;
    if (m_isoSplitBytes > 0)
        // A part is cut at the first key frame past the size, so none is smaller, and the transport stream adds
        // about 5 per cent to what the sources hold.
        parts = (std::max)(parts, totalSize() / 100 * 106 / m_isoSplitBytes + 1);
    if (m_isoSplitSeconds > 0)
    {
        const auto toNano = [](const int64_t pts)
        { return static_cast<int64_t>(static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ)); };
        const int64_t fromNano = m_cutStart > 0 ? toNano(m_cutStart) : 0;
        const int64_t toEndNano = m_cutEnd > 0 ? toNano(m_cutEnd) : m_metaDemuxer.statedDurationNano();
        if (toEndNano > fromNano)
            parts = (std::max)(parts,
                               static_cast<int64_t>((toEndNano - fromNano) / (m_isoSplitSeconds * 1000000000LL) + 1));
    }
    if (m_isoSplitChapters)
    {
        const size_t marks = m_chapters.empty() ? sourceChapters().size() : m_chapters.size();
        parts = (std::max)(parts, static_cast<int64_t>(marks) + 1);
    }
    if (parts <= 1)
        return m_extraIsoBlocks;

    // About twenty sectors for what every disc has, and three a part in 2D, seven in 3D; counted with room to spare.
    constexpr int64_t SECTORS_PER_BLOCK = ALLOC_BLOCK_SIZE / SECTOR_SIZE;
    const int64_t needed = 64 + parts * (getMvcFound() ? 8 : 4);
    const int64_t room = IsoWriter::metadataBlocks(totalSize(), m_extraIsoBlocks) * SECTORS_PER_BLOCK;
    if (needed <= room)
        return m_extraIsoBlocks;
    return m_extraIsoBlocks + static_cast<int>((needed - room + SECTORS_PER_BLOCK - 1) / SECTORS_PER_BLOCK);
}

void MuxerManager::writeChapterFile(const std::string& outDirName)
{
    const std::vector<AVChapter> chapters = sourceChapters();
    if (chapters.empty())
        return;

    const std::string fileName = closeDirPath(toNativeSeparators(outDirName)) + "chapters.txt";
    TextFile file;
    if (!file.open(fileName.c_str(), File::ofWrite))
    {
        LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << fileName);
        return;
    }
    for (const auto& chapter : chapters)
        if (!file.writeLine(floatToTime(static_cast<double>(chapter.start) / 1e9)))
        {
            LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << fileName);
            file.close();
            return;
        }
    file.close();
    LTRACE(LT_INFO, 2, "Chapters written to " << fileName << ", " << chapters.size() << " of them");

    // chapters.txt is deliberately left as it is: one bare time per line, because it is meant to be
    // pasted into the window's chapter box and because someone's script may be reading it. It has
    // no room for the names, so they go into a second file in the form other tools read back.
    const std::string xmlName = closeDirPath(toNativeSeparators(outDirName)) + "chapters.xml";
    if (writeChapterXml(xmlName, chapters))
        LTRACE(LT_INFO, 2, "Chapters with their names written to " << xmlName);
    else
        LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << xmlName);
}

void MuxerManager::doMux(const string& outFileName, FileFactory* fileFactory)
{
    // Run the discovery phase: probe all tracks to collect metadata
    // (channels, sample rate, resolution, codec-private, etc.) before
    // the main mux pipeline starts.  This is self-contained -- all
    // temporary I/O is closed before discoverStreams() returns.
    m_discoveryData = m_metaDemuxer.discoverStreams();

    preinitMux(outFileName, fileFactory);

    if (m_demuxMode)
        writeChapterFile(outFileName);

    m_fileWriter = std::make_unique<BufferedFileWriter>();
    AVPacket avPacket;
    bool anyMuxed = false;

    while (true)
    {
        const int avRez = m_metaDemuxer.readPacket(avPacket);

        if (avRez == BufferedReader::DATA_EOF)
            break;
        if (m_cutStart > 0)
        {
            if (avPacket.pts < m_cutStart || !keptAfterCutStart(avPacket))
                continue;
        }
        if (m_cutEnd > 0 && avPacket.pts >= m_cutEnd)
        {
            // A subtitle packet past the cut is dropped, but it does not end the cut. Its timestamp
            // can run more than a second ahead of the pictures still being read, so stopping at it
            // cut the video and audio short: 213 pictures of a 10 second cut instead of 240.
            if (avPacket.codecID == CODEC_S_PGS || avPacket.codecID == CODEC_S_SRT || avPacket.codecID == CODEC_S_SUP ||
                avPacket.codecID == CODEC_S_MKVCOPY)
                continue;
            break;
        }

        anyMuxed = true;
        if (m_subStreamIndex.find(avPacket.stream_index) != m_subStreamIndex.end())
            m_subMuxer->muxPacket(avPacket);
        else
            m_mainMuxer->muxPacket(avPacket);
    }

    LTRACE(LT_INFO, 2, "Flushing write buffer");

    if (m_subMuxer)
        m_subMuxer->doFlush();
    m_mainMuxer->doFlush();

    for (auto& i : m_delayedData) asyncWriteBlock(i);

    waitForWriting();

    // What close reports was not looked at, so a file whose end could not be written was reported as a finished mux.
    bool closed = m_mainMuxer->close();
    if (m_subMuxer && !m_subMuxer->close())
        closed = false;

    // Drain any writes the muxers queued during flush/close, then surface a failed
    // async write (e.g. disk full at the tail of a large image) instead of silently
    // reporting success over a truncated, non-playable output.
    waitForWriting();
    if (m_fileWriter->hasError())
    {
        const std::string err = m_fileWriter->getLastError();
        m_fileWriter.reset();
        throw std::runtime_error("Failed to write output (disk full or I/O error): " + err);
    }

    m_fileWriter.reset();
    if (!closed)
        throw std::runtime_error("Failed to write the end of the output (disk full or I/O error)");
    // A start past the end of the sources dropped every packet, and the empty output was reported as a finished mux.
    if (m_cutStart > 0 && !anyMuxed)
    {
        std::ostringstream msg;
        msg << "--cut-start (" << static_cast<double>(m_cutStart) / INTERNAL_PTS_FREQ
            << "s) is at or past the end of the sources: the output holds no media.";
        throw std::runtime_error(msg.str());
    }
}

int MuxerManager::addStream(const string& codecName, const string& fileName, const map<string, string>& addParams)
{
    const int rez = m_metaDemuxer.addStream(codecName, fileName, addParams);
    return rez;
}

bool MuxerManager::openMetaFile(const string& fileName)
{
    TextFile file(fileName.c_str(), File::ofRead);
    std::string str;
    std::string allMuxOpts;  // every MUXOPT line of the meta, for the muxers
    file.readLine(str);
    while (str.length() > 0)
    {
        // Spaces or tabs in front of MUXOPT are allowed, as they are on a track line (see checkBluRayMux).
        const size_t start = str.find_first_not_of(" 	");
        if (start != string::npos && strStartWith(str.substr(start), "MUXOPT"))
        {
            const string line = muxOptLine(str);
            // Every MUXOPT line counts. The muxers were given the last one only, so the options of an earlier line
            // (--no-pcr-on-video-pid, --vbr, --vbv-len...) were lost without a word, while the rest of the program
            // read them all.
            allMuxOpts = allMuxOpts.empty()
                             ? line
                             : allMuxOpts + line.substr(line.find(' ') == string::npos ? line.size() : line.find(' '));
            warnUnknownMuxOpts(line);
            // Refuse a bad option HERE, while the meta is still being read and no destination
            // exists. Raised any later it replaced the user existing output image with a stub.
            validateMuxOptsEarly(line);
            parseMuxOpt(line);
            m_muxOpts = allMuxOpts;  // parseMuxOpt set it to this line alone
            // Both cut values are positions from the start of the source, so an end at or before the
            // start selects nothing. That used to produce an empty file and report success, which is the
            // worst of the three possible outcomes. Refuse it and say what the two values were. It is
            // refused here, before an output exists: refused when muxing began, an existing ISO image of
            // the same name had already been emptied.
            if (m_cutEnd > 0 && m_cutEnd <= m_cutStart)
                THROW(ERR_COMMON, "Nothing to mux: --cut-end ("
                                      << m_cutEnd / INTERNAL_PTS_FREQ << "s) is not after --cut-start ("
                                      << m_cutStart / INTERNAL_PTS_FREQ
                                      << "s). Both are positions from the start of the source, so the end "
                                         "must be the later of the two.")
            m_mvcBaseViewR = m_muxOpts.find("right-eye") != string::npos;
        }
        file.readLine(str);
    }
    file.close();

    m_metaDemuxer.setDemuxOutput(m_demuxMode);
    m_metaDemuxer.openFile(fileName);
    return true;
}

void MuxerManager::muxBlockFinished(const AbstractMuxer* muxer)
{
    if (muxer == m_subMuxer.get())
        m_subBlockFinished = true;
    else
        m_mainBlockFinished = true;

    if (m_subBlockFinished && m_mainBlockFinished)
    {
        for (auto& i : m_delayedData) asyncWriteBlock(i);

        m_delayedData.clear();
        m_subBlockFinished = false;
        m_mainBlockFinished = false;
    }
}

void MuxerManager::asyncWriteBuffer(const AbstractMuxer* muxer, uint8_t* buff, const int len,
                                    AbstractOutputStream* dstFile)
{
    WriterData data;
    data.m_buffer = buff;
    data.m_bufferLen = len;
    data.m_mainFile = dstFile;
    data.m_command = WriterData::Commands::wdWrite;

    if (m_interleave && muxer == m_mainMuxer.get())
    {
        // do interleave of SSIF blocks. Place sub channel blocks first, delay main muxer blocks
        m_delayedData.push_back(data);
        return;
    }

    asyncWriteBlock(data);
}

void MuxerManager::asyncWriteBlock(const WriterData& data) const
{
    static constexpr int nMaxWriteQueueSize = 256 * 1024 * 1024 / DEFAULT_FILE_BLOCK_SIZE;
    while (m_fileWriter->getQueueSize() > nMaxWriteQueueSize)
    {
        Process::sleep(1);
    }
    m_fileWriter->addWriterData(data);
}

int MuxerManager::syncWriteBuffer(AbstractMuxer* muxer, const uint8_t* buff, const int len,
                                  AbstractOutputStream* dstFile) const
{
    assert(m_interleave == 0);
    const int rez = dstFile->write(buff, len);
    dstFile->sync();
    return rez;
}

namespace
{
// Parse a --disc-size value: a bd25/bd50/bd100/bd128 keyword, or a raw number with an optional
// unit suffix (k/m/g = decimal 10^3/10^6/10^9; ki/mi/gi = binary; none or b = bytes).
// Returns the capacity in bytes, or 0 if the value is unparseable.
int64_t parseDiscSizeValue(const string& raw)
{
    const string s = strToUpperCase(trimStr(raw));
    if (s == "BD25" || s == "25")
        return BD25_CAPACITY;
    if (s == "BD50" || s == "50")
        return BD50_CAPACITY;
    if (s == "BD100" || s == "100")
        return BD100_CAPACITY;
    if (s == "BD128" || s == "128")
        return BD128_CAPACITY;
    size_t i = 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
    if (i == 0)
        return 0;
    double num = 0.0;
    try
    {
        num = std::stod(s.substr(0, i));
    }
    catch (...)
    {
        return 0;
    }
    const string unit = s.substr(i);
    double mult;
    if (unit.empty() || unit == "B")
        mult = 1.0;
    else if (unit == "K" || unit == "KB")
        mult = 1e3;
    else if (unit == "M" || unit == "MB")
        mult = 1e6;
    else if (unit == "G" || unit == "GB")
        mult = 1e9;
    else if (unit == "KI" || unit == "KIB")
        mult = 1024.0;
    else if (unit == "MI" || unit == "MIB")
        mult = 1024.0 * 1024.0;
    else if (unit == "GI" || unit == "GIB")
        mult = 1024.0 * 1024.0 * 1024.0;
    else
        return 0;
    return static_cast<int64_t>(num * mult);
}
}  // namespace

// An option this program does not know was ignored without a word, so a misspelt one (--no-pcr-on-vide-pid) changed
// nothing and nobody was told. Each option of a MUXOPT line is checked against the ones a meta file can carry; one
// that is not among them is named. Options of --bdmv-to-iso and --lang-codes belong on the command line.
// The options of a MUXOPT line were split at spaces only, so a tab joined the texts on either side of it: after an
// option with a value, such as --cut-end=5s, the tab and what followed became part of that value without a word, and
// elsewhere the joined options were not recognised. A tab inside quotes, in a disc label for instance, is kept.
string MuxerManager::muxOptLine(const string& line)
{
    string out = line;
    bool quoted = false;
    for (char& c : out)
    {
        if (c == '"')
            quoted = !quoted;
        else if (c == '\t' && !quoted)
            c = ' ';
    }
    const size_t start = out.find_first_not_of(' ');
    return start == string::npos ? string() : out.substr(start);
}

void MuxerManager::warnUnknownMuxOpts(const string& opts)
{
    static const std::set<std::string> metaOptions = {"--allow-oversize",
                                                      "--auto-chapters",
                                                      "--avchd",
                                                      "--bitrate",
                                                      "--blankOffset",
                                                      "--blu-ray",
                                                      "--blu-ray-v3",
                                                      "--cbr",
                                                      "--chapters-file",
                                                      "--constant-iso-hdr",
                                                      "--custom-chapters",
                                                      "--cut-end",
                                                      "--cut-start",
                                                      "--demux",
                                                      "--disc-size",
                                                      "--dv-profile",
                                                      "--extra-iso-space",
                                                      "--insertBlankPL",
                                                      "--keep-editions",
                                                      "--label",
                                                      "--layer-break-guard",
                                                      "--layer-break-guard-before",
                                                      "--layer-break-lbn",
                                                      "--m2tsOffset",
                                                      "--maxbitrate",
                                                      "--minbitrate",
                                                      "--mplsOffset",
                                                      "--new-audio-pes",
                                                      "--no-asyncio",
                                                      "--no-chapters",
                                                      "--no-hdmv-descriptors",
                                                      "--no-pcr-on-video-pid",
                                                      "--right-eye",
                                                      "--split-chapters",
                                                      "--split-duration",
                                                      "--split-size",
                                                      "--start-time",
                                                      "--title",
                                                      "--vbr",
                                                      "--vbv-len"};
    static const std::set<std::string> commandLineOptions = {
        "--3d-single-copy",   "--bdmv-to-iso", "--disc-capacity", "--inner-only",
        "--keep-extra-files", "--lang-codes",  "--no-layer-fit",  "--original-order"};
    for (const auto& token : splitQuotedStr(opts.c_str(), ' '))
    {
        const string name = trimStr(token).substr(0, trimStr(token).find('='));
        if (name.empty() || name == "MUXOPT" || metaOptions.count(name))
            continue;
        if (commandLineOptions.count(name))
            LTRACE(
                LT_WARN, 2,
                "Warning: " << name << " is a command line option and has no effect in a meta file. It was ignored.");
        else
            LTRACE(LT_WARN, 2, "Warning: unknown option " << name << " in the MUXOPT line. It was ignored.");
    }
}

void MuxerManager::parseMuxOpt(const string& opts)
{
    // The muxers are made later, in preinitMux, and take their options from m_muxOpts. The blank playlist's mux
    // (muxBlankPL) comes here without a meta file, and m_muxOpts was set only where a meta file is read, so its muxer
    // got no options at all: it was not in disc mode and kept no seek index. The blank playlist came out with no play
    // item, its one mark pointing at play item 0xFFFF, and its clip information with no entry point map.
    m_muxOpts = opts;
    const vector<string> params = splitQuotedStr(opts.c_str(), ' ');
    for (auto& i : params)
    {
        vector<string> paramPair = splitStr(trimStr(i).c_str(), '=');
        if (paramPair.empty())
            continue;

        if (paramPair[0] == "--start-time" && paramPair.size() > 1)
        {
            if (paramPair[1].find(":") != string::npos)
                m_ptsOffset = llround(timeToFloat(paramPair[1]) * 90000.0);
            else
                m_ptsOffset = strToInt64(paramPair[1].c_str()) * 2;  // source in a 45Khz clock
        }
        else if (paramPair[0] == "--no-asyncio")
            setAsyncMode(false);
        else if (paramPair[0] == "--keep-editions")
        {
            m_keepEditions = true;
            m_metaDemuxer.setKeepEditions(true);
        }
        else if (paramPair[0] == "--cut-start" || paramPair[0] == "--cut-end")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            // A value without a unit was taken in the internal clock's ticks, 5,292,000,000 a second, and a clock
            // time such as 00:01:30 as 0, which switched the cut off; both in silence.
            int64_t cut = 0;
            if (!parseTimeToTicks(paramPair[1], nullptr, &cut) || cut < 0)
                THROW(ERR_COMMON, paramPair[0] << "=" << paramPair[1]
                                               << " is not a time. Write a number followed by ms, s or min, such as "
                                                  "90s or 1.5min, or a clock time such as 00:01:30.")
            if (paramPair[0] == "--cut-start")
                setCutStart(cut);
            else
                setCutEnd(cut);
        }
        else if (paramPair[0] == "--split-duration" || paramPair[0] == "--split-size")
        {
            if (m_extraIsoBlocks == 0)
                m_extraIsoBlocks = 4;
            // Kept for getExtraISOBlocks. A missing value is refused where the split is made, and only there: a demux
            // ignores a split with a warning.
            if (paramPair.size() >= 2 && paramPair[0] == "--split-size")
                m_isoSplitBytes = checkedSplitSize(paramPair[1]);
            else if (paramPair.size() >= 2)
                m_isoSplitSeconds = strToInt64(paramPair[1].c_str());
        }
        else if (paramPair[0] == "--split-chapters")
            m_isoSplitChapters = true;
        else if (paramPair[0] == "--extra-iso-space")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            m_extraIsoBlocks = strToInt32(paramPair[1]);
            m_extraIsoBlocksGiven = true;
        }
        else if (paramPair[0] == "--blu-ray" || paramPair[0] == "--blu-ray-v3" || paramPair[0] == "--avchd")
        {
            m_bluRayMode = true;
            V3_flags |= BLURAY_OUT;
        }
        else if (paramPair[0] == "--demux")
        {
            m_demuxMode = true;
        }
        else if (paramPair[0] == "--constant-iso-hdr")
        {
            m_reproducibleIsoHeader = true;
        }
        else if (paramPair[0] == "--disc-size")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            m_discSizeLimit = parseDiscSizeValue(paramPair[1]);
            if (m_discSizeLimit <= 0)
                THROW(ERR_COMMON, "Invalid --disc-size value '"
                                      << paramPair[1]
                                      << "'. Use bd25/bd50/bd100/bd128, or a byte count with an optional "
                                         "k/m/g/gib suffix (e.g. 25000000000, 24g, 23gib).");
        }
        else if (paramPair[0] == "--allow-oversize")
        {
            m_allowOversize = true;
        }
        else if (paramPair[0] == "--layer-break-guard")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            m_layerBreakGuardMB = strToInt32(paramPair[1]);
            if (m_layerBreakGuardMB < 0 || m_layerBreakGuardMB > 1024)
                THROW(ERR_COMMON,
                      "Invalid --layer-break-guard value '" << paramPair[1] << "'. Expected 0..1024 (megabytes).");
        }
        else if (paramPair[0] == "--layer-break-guard-before")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            m_layerBreakGuardBeforeMB = strToInt32(paramPair[1]);
            if (m_layerBreakGuardBeforeMB < 0 || m_layerBreakGuardBeforeMB > 1024)
                THROW(ERR_COMMON, "Invalid --layer-break-guard-before value '" << paramPair[1]
                                                                               << "'. Expected 0..1024 (megabytes).");
        }
        else if (paramPair[0] == "--no-chapters")
            m_noChapters = true;
        else if (paramPair[0] == "--layer-break-lbn")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            // One sector for BD-R/RE DL, or a comma-separated list for BDXL (100 GB = 2 breaks,
            // 128 GB = 3): e.g. --layer-break-lbn=15768576,31537152
            m_layerBreakLbns.clear();
            for (const auto& tok : splitStr(paramPair[1].c_str(), ','))
            {
                const int lbn = strToInt32(trimStr(tok).c_str());
                // one rule, shared with the --bdmv-to-iso path, which used to accept anything
                std::string warning;
                const std::string err = checkLayerBreakLbn(lbn, warning);
                if (!err.empty())
                    THROW(ERR_COMMON, "Invalid --layer-break-lbn value '" << paramPair[1] << "': " << err);
                if (!warning.empty())
                    LTRACE(LT_WARN, 2, "Warning: " << warning);
                m_layerBreakLbns.push_back(lbn);
            }
        }
    }
}

// A cut that does not start at the beginning kept every packet from the cut time on, so its video began with whatever
// picture came next. A picture other than an I-frame cannot be decoded without the ones before it, and a stream from
// MP4 or Matroska holds its parameter sets once, ahead of its first picture, which the cut had dropped: such a cut
// gave sound but no picture at all. Each video now begins with its first I-frame at or after the cut, and that picture
// is told it opens a new file, the way the first picture of a split part is, so the reader writes the parameter sets
// in front of it when its group has none. The pictures between the cut and that I-frame, and the ones shown before
// it, are left out: none of them could be shown. A layer that only adds to another video (the second view of a 3D
// stream, a Dolby Vision enhancement layer) begins at the same picture as the main video rather than at an I-frame of
// its own. Audio and subtitles keep to the cut time, as before.
bool MuxerManager::keptAfterCutStart(AVPacket& avPacket)
{
    auto* video = dynamic_cast<MPEGStreamReader*>(avPacket.codec);
    if (video == nullptr)
        return true;
    if (!m_cutLayersKnown)
    {
        // In the order of the meta file, the order the transport stream muxer tells the layers apart in: a Dolby
        // Vision stream is an enhancement layer only when a main video came before it (profiles 5 and 8 have one
        // stream, and it is the main video).
        m_cutLayersKnown = true;
        int mainVideos = 0;
        for (const StreamInfo& si : m_metaDemuxer.getStreamInfo())
        {
            const auto* reader = dynamic_cast<MPEGStreamReader*>(si.m_streamReader);
            if (reader == nullptr)
                continue;
            const auto* h264 = dynamic_cast<const H264StreamReader*>(reader);
            if (si.m_isSubStream || (h264 != nullptr && h264->isSubStream()) ||
                (reader->getStreamHDR() == 4 && mainVideos > 0))
                m_cutLayers.insert(si.m_streamReader->getStreamIndex());
            else if (!si.m_streamReader->isSecondary())
                mainVideos++;
        }
    }
    const auto begun = m_cutVideoFrom.find(avPacket.stream_index);
    if (begun != m_cutVideoFrom.end())
        return avPacket.pts >= begun->second;
    if (m_cutLayers.count(avPacket.stream_index) > 0)
    {
        if (m_cutVideoStart < 0 || avPacket.pts < m_cutVideoStart)
            return false;
    }
    else
    {
        if (!(avPacket.flags & AVPacket::IS_IFRAME))
            return false;
        if (m_cutVideoStart < 0 && !video->isSecondary())
            m_cutVideoStart = avPacket.pts;
    }
    m_cutVideoFrom[avPacket.stream_index] = avPacket.pts;
    video->onSplitEvent();
    return true;
}

void MuxerManager::waitForWriting() const
{
    while (!m_fileWriter->isQueueEmpty()) Process::sleep(1);
}

std::unique_ptr<AbstractMuxer> MuxerManager::createMuxer() { return m_factory.newInstance(this); }

AbstractMuxer* MuxerManager::getMainMuxer() const { return m_mainMuxer.get(); }

AbstractMuxer* MuxerManager::getSubMuxer() const { return m_subMuxer.get(); }

bool MuxerManager::isStereoMode() const { return m_subMuxer != nullptr; }

void MuxerManager::setAllowStereoMux(const bool value) { m_allowStereoMux = value; }

int MuxerManager::getDefaultAudioTrackIdx() const
{
    std::string paramVal;
    return seekDefaultTrack(m_metaDemuxer.getStreamInfo(), paramVal,
                            [](auto&& streamInfo) { return streamInfo.m_codec[0] == 'A'; });
}

int MuxerManager::getDefaultSubTrackIdx(SubTrackMode& mode) const
{
    std::string paramVal;
    const auto idx = seekDefaultTrack(m_metaDemuxer.getStreamInfo(), paramVal,
                                      [](auto&& streamInfo) { return streamInfo.m_codec[0] == 'S'; });
    if (idx != -1)
    {
        if (paramVal == "all")
        {
            mode = SubTrackMode::All;
        }
        else if (paramVal == "forced")
        {
            mode = SubTrackMode::Forced;
        }
        else
        {
            LTRACE(LT_WARN, 2, "Invalid 'default' parameter value for subtitle track " << idx << ", ignoring");
            return -1;
        }
    }
    return idx;
}
