// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "muxerManager.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <fs/systemlog.h>
#include "fs/textfile.h"

#include "ac3StreamReader.h"
#include "chapterFile.h"
#include "h264StreamReader.h"
#include "hevcStreamReader.h"
#include "iso_writer.h"
#include "matroskaMuxer.h"
#include "mpegStreamReader.h"
#include "subTrackFilter.h"
#include "tsMuxer.h"
#include "videoCopyStreamReader.h"
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
    : m_metaDemuxer(readManager), m_readManager(readManager), m_factory(factory)
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

// The order in which the streams go to a transport stream muxer. The muxer takes a Dolby Vision stream for the
// enhancement layer only when a base layer came before it (tsMuxer.cpp, the PID rule), so a meta file that lists
// the enhancement layer first put the 1080p layer on 0x1011, where a player looks for the primary video, and the
// 4K base after it. The layers are told apart by what they ARE: the base layer of a dual layer disc carries no
// Dolby Vision data, the enhancement layer does, and it is never larger than its base. When the first primary
// HEVC video carries Dolby Vision and a later one carries none and is at least as wide, that later one goes first.
// Every other stream keeps its place, and a meta file in the usual order is muxed exactly as before.
static std::vector<size_t> transportStreamAddOrder(const std::vector<StreamInfo>& ci,
                                                   const std::vector<StreamDiscoveryData>& dd)
{
    std::vector<size_t> order(ci.size());
    for (size_t i = 0; i < ci.size(); ++i) order[i] = i;

    // The base is the first later primary HEVC video that is WIDER than the first Dolby Vision one, with or without
    // Dolby Vision data of its own (a base whose RPU was not taken out still carries it). One only as wide is no base
    // of it: a profile 8.1 picture listed before a plain one of the same size went to the enhancement layer PID.
    const auto primaryHevc = [&](const size_t i)
    {
        AbstractStreamReader* reader = ci[i].m_streamReader;
        return dynamic_cast<HEVCStreamReader*>(reader) != nullptr && !reader->isSecondary() && !ci[i].m_isSubStream;
    };
    // The width the probe found: the readers have not parsed a picture yet when the order is decided, so theirs was 0
    // here and a test on it never held. Through the base class: the HEVC reader keeps its own protected.
    const auto width = [&](const size_t i) -> int64_t
    {
        if (i < dd.size() && dd[i].discovered && dd[i].width > 0)
            return dd[i].width;
        return dynamic_cast<MPEGStreamReader*>(ci[i].m_streamReader)->getStreamWidth();
    };
    size_t dv = ci.size();
    for (size_t i = 0; i < ci.size() && dv == ci.size(); ++i)
        if (primaryHevc(i))
        {
            if (ci[i].m_streamReader->getStreamHDR() != 4)
                return order;  // a primary video without Dolby Vision comes first: the usual order
            // A first stream whose RPU says single layer (profile 8.1: HevcDvRpuUnit) is a picture of its own, not an
            // enhancement layer listed early.
            if (const auto* hevc = dynamic_cast<HEVCStreamReader*>(ci[i].m_streamReader);
                hevc != nullptr && hevc->dvRpuElPresent() == 0)
                return order;
            dv = i;
        }
    if (dv == ci.size())
        return order;
    size_t base = ci.size();
    for (size_t i = dv + 1; i < ci.size() && base == ci.size(); ++i)
        if (primaryHevc(i) && width(i) > width(dv))
            base = i;
    if (base == ci.size())
        return order;

    order.erase(order.begin() + static_cast<std::ptrdiff_t>(base));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(dv), base);
    LTRACE(LT_INFO, 2,
           "Dolby Vision: the enhancement layer (track "
               << ci[dv].m_streamReader->getStreamIndex() << ") is listed before its base layer (track "
               << ci[base].m_streamReader->getStreamIndex() << "). The base layer is muxed as the primary video.");
    return order;
}

std::string MuxerManager::sourceSameAs(const std::string& fileName) const
{
    std::error_code ec;
    const std::filesystem::path out(std::u8string(fileName.begin(), fileName.end()));
    if (!std::filesystem::exists(out, ec))
        return {};
    for (const auto& si : getStreamInfo())
    {
        std::vector<std::string> names;
        for (const std::string& part : splitQuotedStr(si.m_fullStreamName.c_str(), '+'))
            names.push_back(unquoteStr(trimStr(part)));
        names.insert(names.end(), si.m_sourceFiles.begin(), si.m_sourceFiles.end());
        for (const std::string& name : names)
        {
            const std::filesystem::path src(std::u8string(name.begin(), name.end()));
            if (std::filesystem::equivalent(src, out, ec) && !ec)
                return name;
        }
    }
    return {};
}

void MuxerManager::preinitMux(const std::string& outFileName, FileFactory* fileFactory)
{
    // Blu-ray navigation needs the mux start time at or above 524280 ticks of the 45 kHz
    // clock (11.65 s). Below that, players subtract the decoder preroll from timestamps
    // near zero and their 32-bit 45 kHz registers underflow, so skipping back to the start
    // of the disc fails. No disc measured starts lower (the lowest start at exactly
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
    checkTimeshiftFitsClock();  // also here, for an output that is not a disc
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
                if (auto* const mkv = dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get()))
                    mkv->setRightEyeFirst(isMvcBaseViewR());
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
                    // --no-chapters was ignored without a word, and the chapters kept
                    if (m_noChaptersTyped)
                        THROW(ERR_COMMON,
                              "--keep-editions keeps the source's own chapters, and --no-chapters leaves them out: "
                              "use one of the two.")
                    // An MP4 source was taken: the option kept nothing, and its chapters were left out as well.
                    bool fromMatroska = false;
                    for (const auto& si : m_metaDemuxer.getStreamInfo())
                    {
                        const std::string ext = strToUpperCase(
                            extractFileExt(unquoteStr(trimStr(splitQuotedStr(si.m_fullStreamName.c_str(), '+')[0]))));
                        fromMatroska = fromMatroska || ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" ||
                                       ext == "WEBM";
                    }
                    if (!fromMatroska)
                        THROW(ERR_COMMON,
                              "--keep-editions keeps the editions of a Matroska source, and no track "
                              "here comes from a Matroska file. Leave it out.")
                    // A Matroska source without editions has nothing to keep, and nothing said so
                    if (m_metaDemuxer.getSourceEditions().chapters.empty())
                        LTRACE(LT_WARN, 2,
                               "Warning: --keep-editions: the Matroska source has no chapters or editions, so there "
                               "is nothing to keep.");
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
                    // chapters, moved by the cut the same way (MatroskaMuxer::setSourceChapterTree). Also when the
                    // default edition shows nothing: its chapters all hidden, the other editions were lost with it.
                    if (auto* const mkv = dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get()); mkv && !m_noChapters)
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
                // Times typed or edited without names (--custom-chapters) where the source names its chapters: a time
                // that is one of the source's keeps that chapter's name and language. Editing the window's list of a
                // source's chapters, even to add one, turned every name into "Chapter n".
                else if (m_chapterInfo.empty())
                {
                    const std::vector<AVChapter> source = sourceChapters();
                    std::vector<AVChapter> named(m_chapters.size());
                    bool anyNamed = false;
                    for (size_t i = 0; i < m_chapters.size(); ++i)
                    {
                        const auto at = llround(m_chapters[i] * 1e9);
                        named[i].start = at;
                        for (const auto& chapter : source)
                            if (std::llabs(chapter.start - at) <= 1000000 && !chapter.cTitle.empty())
                            {
                                named[i] = chapter;
                                named[i].start = at;
                                // The end is where the edited list goes on, not where the source's chapter ended:
                                // 0;4;6;8 over chapters 0-4, 4-8 and 8-10 gave 4-8 and a chapter at 6 inside it, and
                                // 0;8 a gap from 4 to 8. The last one keeps its end only where it was the source's
                                // last, which ends with the title.
                                named[i].end = -1;
                                if (i + 1 < m_chapters.size())
                                {
                                    const auto next = llround(m_chapters[i + 1] * 1e9);
                                    if (next > at)
                                        named[i].end = next;
                                }
                                else if (std::none_of(source.begin(), source.end(), [&](const AVChapter& later)
                                                      { return later.start > chapter.start; }))
                                    named[i].end = chapter.end;
                                anyNamed = true;
                                break;
                            }
                    }
                    if (anyNamed)
                        m_chapterInfo = named;
                }
                if (!m_chapters.empty())
                {
                    m_mainMuxer->setChapters(m_chapters);
                    m_mainMuxer->setChapterInfo(m_chapterInfo);
                }
            }
        }
    }
    // The second view of 3D video (subClip=, or an MVC dependent view) goes to a muxer of its own beside the one of the
    // first view. With only such tracks there was no muxer for the first view, and the mux crashed after the whole
    // source had been read.
    if (m_subMuxer && !m_mainMuxer)
        THROW(ERR_COMMON,
              "Every track of the meta file is the second view of 3D video (subClip= or an MVC dependent view), so "
              "there is nothing for the first view. Add the track of the first view, or leave subClip= out.")

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

    // Every stream is read before any is given to a muxer, so that the order they are given in can depend on what
    // each one is (transportStreamAddOrder).
    for (size_t i = 0; i < ci.size(); ++i)
    {
        StreamInfo& si = ci[i];
        if (!m_firstBlocksRead)
            si.read();

        // Apply discovery-phase metadata to the stream reader so that
        // getTSDescriptor(), getStreamInfo(), etc. have correct values
        // from the very first packet (channels, sample rate, codec-private,
        // resolution, FPS, etc.).
        if (i < m_discoveryData.size() && m_discoveryData[i].discovered)
        {
            si.m_streamReader->applyDiscoveryData(m_discoveryData[i]);
        }
    }

    // drop-ac3-core leaves the AC-3 core out of a TrueHD track. On AC-3 or Dolby Digital Plus it did nothing, and said
    // nothing: the E-AC-3 part of a disc's Dolby Digital Plus only adds to its core and cannot be played without it.
    // TrueHD is known from the probe here; the reader finds it out only as it reads on.
    for (size_t i = 0; i < ci.size(); ++i)
    {
        const StreamInfo& si = ci[i];
        const bool trueHD = i < m_discoveryData.size() && m_discoveryData[i].discovered && m_discoveryData[i].isTrueHD;
        // split-ac3-core works on a demux only (SingleFileMuxer says when the track has no pair to split), and
        // drop-ac3-core on a Matroska output or a demux: a TS or a disc keeps the core, which is what a player needs.
        // Both were ignored there without a word.
        if (!m_demuxMode && si.m_addParams.count("split-ac3-core"))
            LTRACE(LT_WARN, 2,
                   "Warning: split-ac3-core is used on a demux only, and is ignored for track "
                       << si.m_streamReader->getStreamIndex() << ". Add --demux to get the .thd and .ac3 files.");
        if (!m_demuxMode && si.m_addParams.count("drop-ac3-core") && trueHD && si.m_codec != "A_MLP" &&
            dynamic_cast<TSMuxer*>(m_mainMuxer.get()) != nullptr)
            LTRACE(LT_WARN, 2,
                   "Warning: drop-ac3-core is used for a Matroska output or a demux only. It is ignored for track "
                       << si.m_streamReader->getStreamIndex()
                       << ": a TS or a Blu-ray keeps the AC-3 core of a TrueHD track, as a player needs it.");
        // Every track that is not TrueHD is told so: only AC-3 and Dolby Digital Plus were, and DTS, AAC or LPCM
        // took the parameter without a word. With down-to-ac3 the track is not kept
        // whole, as the warning said: the core is kept alone.
        if (!si.m_addParams.count("drop-ac3-core") || trueHD || si.m_codec == "A_MLP")
            continue;
        const auto* ac3 = dynamic_cast<AC3StreamReader*>(si.m_streamReader);
        if (ac3 != nullptr && ac3->isTrueHD())
            continue;
        std::string rest;
        if (ac3 == nullptr)
            rest = " It is ignored.";
        else if (si.m_addParams.count("down-to-ac3"))
            rest = " It is ignored; down-to-ac3 keeps the AC-3 core alone.";
        else
            rest =
                " It is ignored, and the track is kept whole. If the track is Dolby Digital Plus with an AC-3 core, "
                "down-to-ac3 keeps the core alone; the E-AC-3 part cannot be played without it.";
        LTRACE(LT_WARN, 2,
               "Warning: drop-ac3-core applies to TrueHD only, and track " << si.m_streamReader->getStreamIndex()
                                                                           << " is not TrueHD." << rest);
    }

    std::vector<size_t> order(ci.size());
    for (size_t i = 0; i < ci.size(); ++i) order[i] = i;
    if (dynamic_cast<TSMuxer*>(m_mainMuxer.get()) != nullptr)
        order = transportStreamAddOrder(ci, m_discoveryData);

    for (const size_t i : order)
    {
        StreamInfo& si = ci[i];

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
        // terminological form. A BCP 47 tag the source had travels beside it, for Matroska output. A language typed
        // into the file name of a raw stream is written as typed, as the listing shows it and as USAGE says: ger in a
        // name was listed as ger and written as deu.
        if (!si.m_lang.empty() && si.m_addParams.find("lang") == si.m_addParams.end())
        {
            si.m_addParams["srclang"] = si.m_langFromName ? si.m_lang : applyLangCodeStyle(si.m_lang);
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

    // A .ssif written directly holds both views of 3D video in one file, which the muxer of the second view writes
    // through the first view's (joinToMasterFile). A split of it went wrong: each view began new parts of its own, and
    // the mux stopped at the third part with "Can't create file", after the first parts had been written. It is refused
    // before anything is written. A Blu-ray folder or an ISO image keeps the views in clips of their own and splits.
    const auto* const mainTs = dynamic_cast<const TSMuxer*>(m_mainMuxer.get());
    if (m_subMuxer && m_interleave && !(fileFactory && fileFactory->isVirtualFS()) && mainTs && mainTs->isSplitting())
        THROW(ERR_COMMON,
              "A .ssif output cannot be split. Leave out the split options, or write a Blu-ray folder or "
              "an ISO image (--blu-ray), whose clips are split instead.")

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

// subTrack= on HEVC splits a merged dual layer Dolby Vision track into its base and enhancement layer, and the split
// refuses a track with no enhancement layer at its first packets (hevcDolbyVisionFilter.cpp). The first block of every
// stream is read when the mux begins (preinitMux), which for a disc is after its folders are made, so a refusal left
// an empty folder tree. When such a line is there, the first blocks are read here instead, before anything is made.
void MuxerManager::checkTimeshiftFitsClock()
{
    // Only a transport stream (a TS, an M2TS, a disc) has that clock: Matroska keeps the shift in its own times and a
    // demux writes no times at all, and both were refused all the same
    if (dynamic_cast<TSMuxerFactory*>(&m_factory) == nullptr)
        return;
    const std::vector<StreamInfo>& ci = m_metaDemuxer.getStreamInfo();
    // A negative timeshift raises the start of the mux by what it needs (preinitMux). The 90 kHz clock of a transport
    // stream wraps after 2^33 ticks, about 26.5 hours, and the raise had no bound: a timeshift of -95440 s put the
    // start so high that the times of a 10 s clip wrapped round halfway through it, and went backwards there.
    int64_t mostNegativeShift = 0;
    for (const StreamInfo& si : ci) mostNegativeShift = std::min(mostNegativeShift, si.m_timeShift);
    if (mostNegativeShift < 0)
    {
        static constexpr int64_t CLOCK_WRAP = 1LL << 33;  // 90 kHz ticks
        const int64_t needed = -mostNegativeShift / (INTERNAL_PTS_FREQ / 90000) + 1;
        // The length of the sources where their containers say it; a raw stream does not, and then the start alone
        // may not pass half the clock, which leaves more than 13 hours for the mux itself.
        const int64_t length = m_metaDemuxer.statedDurationNano() / 1000000 * 90;  // ns to ms to 90 kHz ticks
        if (std::max(needed, m_ptsOffset) + (length > 0 ? length : CLOCK_WRAP / 2) >= CLOCK_WRAP)
            THROW(ERR_COMMON, "A timeshift of " << (-mostNegativeShift * 1000 / INTERNAL_PTS_FREQ)
                                                << " ms needs the start of the mux raised so far that the times would "
                                                   "pass the clock's limit of about 26.5 hours and wrap round. Use a "
                                                   "smaller timeshift.")
    }
    // --start-time and a positive timeshift move the times up as well, and had no bound at all: --start-time=26:30:40
    // and a timeshift of 95440 s wrapped round within the first seconds. The same rule as
    // above: the start, the largest shift and the stated length, or half the clock for a raw stream, stay below it.
    int64_t mostPositiveShift = 0;
    for (const StreamInfo& si : ci) mostPositiveShift = std::max(mostPositiveShift, si.m_timeShift);
    {
        static constexpr int64_t CLOCK_WRAP = 1LL << 33;  // 90 kHz ticks
        const int64_t shift = mostPositiveShift / (INTERNAL_PTS_FREQ / 90000);
        const int64_t length = m_metaDemuxer.statedDurationNano() / 1000000 * 90;  // ns to ms to 90 kHz ticks
        if (m_ptsOffset >= CLOCK_WRAP || shift >= CLOCK_WRAP ||
            m_ptsOffset + shift + (length > 0 ? length : CLOCK_WRAP / 2) >= CLOCK_WRAP)
            THROW(ERR_COMMON, "--start-time (" << m_ptsOffset / 90000 << " s) and a timeshift of "
                                               << mostPositiveShift * 1000 / INTERNAL_PTS_FREQ
                                               << " ms put the times of the mux past the clock's limit of about 26.5 "
                                                  "hours, where they wrap round. Use a smaller start or timeshift.")
    }
}

void MuxerManager::checkTracksBeforeOutput()
{
    std::vector<StreamInfo>& ci = m_metaDemuxer.getCodecInfo();
    checkMatroskaOnlyCodecs(ci, true);

    // A disc has 32 PIDs for each kind of stream: primary audio 0x1100 to 0x111F, subtitles 32 from 0x1200 (0x12A0 on
    // a UHD disc), secondary audio from 0x1A00 and secondary video from 0x1B00. The 33rd took the next PID without a
    // word, outside its range, where no player looks for that kind of stream: a 33rd primary audio track got 0x1120.
    if (m_bluRayMode)
    {
        int audio = 0, secondaryAudio = 0, subtitles = 0, secondaryVideo = 0;
        for (const StreamInfo& si : ci)
        {
            const bool secondary = si.m_addParams.find("secondary") != si.m_addParams.end();
            if (si.m_codec.empty() || si.m_isSubStream)
                continue;
            if (si.m_codec[0] == 'A')
                ++(secondary ? secondaryAudio : audio);
            else if (si.m_codec[0] == 'S')
                ++subtitles;
            else if (si.m_codec[0] == 'V' && secondary)
                ++secondaryVideo;
        }
        for (const auto& [count, kind] : {std::pair<int, const char*>{audio, "audio"},
                                          {secondaryAudio, "secondary audio"},
                                          {subtitles, "subtitle"},
                                          {secondaryVideo, "secondary video"}})
            if (count > 32)
                THROW(ERR_COMMON, "A Blu-ray disc holds at most 32 " << kind << " tracks, and " << count
                                                                     << " were given. Leave out " << count - 32
                                                                     << " of them.")
    }
    checkTimeshiftFitsClock();

    const bool hevcSplit =
        std::any_of(ci.begin(), ci.end(), [](const StreamInfo& si)
                    { return SubTrackFilter::isSubTrack(si.m_pid) && si.m_codec == hevcCodecInfo.programName; });
    // PCM of an MP4 or MOV file is found to be floating point, which is refused, at its first packet: a disc or a
    // demux had made its folders by then, and left them empty. Its first block is read here as well.
    const bool movPcm =
        std::any_of(ci.begin(), ci.end(),
                    [](const StreamInfo& si)
                    {
                        const std::string ext = strToLowerCase(extractFileExt(si.m_streamName));
                        return si.m_codec == "A_LPCM" && (ext == "mp4" || ext == "mov" || ext == "m4a" || ext == "m4v");
                    });
    if ((!hevcSplit && !movPcm) || m_firstBlocksRead)
        return;
    // The probe refuses a split of a track that holds no enhancement layer, by what the track holds; the first
    // blocks below are refused by the subTrack filter only after 32 pictures without one.
    if (!m_discovered)
    {
        m_discoveryData = m_metaDemuxer.discoverStreams();
        m_discovered = true;
    }
    for (StreamInfo& si : ci) si.read();
    m_firstBlocksRead = true;
}

void MuxerManager::checkTrackList(const vector<StreamInfo>& ci) const
{
    checkMatroskaOnlyCodecs(ci, m_demuxMode || !dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get()));
    // w64 names the file a demux writes for an LPCM track. On any other track, or on a mux, it did nothing, without a
    // word.
    for (const StreamInfo& si : ci)
        if (si.m_addParams.count("w64"))
        {
            if (si.m_codec != lpcmCodecInfo.programName)
                LTRACE(LT_WARN, 2,
                       "Warning: w64 applies to an A_LPCM track only, and is ignored for this " << si.m_codec
                                                                                                << " track.");
            else if (!m_demuxMode)
                LTRACE(LT_WARN, 2, "Warning: w64 applies to a demux only (--demux), and is ignored for this mux.");
        }
    if (m_demuxMode)
        return;
    checkOtherTracks(ci);
}

// VP8, VP9, Vorbis, FLAC and ALAC have no form in a transport stream or on a Blu-ray disc: a Matroska output is the
// only one that can carry them. A demux writes VP8 and VP9 into an IVF file, Vorbis into an Ogg file and FLAC into a
// .flac file; only ALAC has no file of its own. FLAC was refused by the transport stream writer, after the folders
// of a disc or an ISO image of the same name had been made.
// The refusal said of all of them that a demux has no file to write them to, also where a demux writes one: VP9 to a
// disc was told so, and the demux of the same line works. It now says what a demux does for the codec at hand.
void MuxerManager::checkMatroskaOnlyCodecs(const vector<StreamInfo>& ci, const bool notMatroska) const
{
    if (notMatroska)
        for (const StreamInfo& si : ci)
            for (const CodecInfo* only : {&vp8CodecInfo, &vp9CodecInfo, &vorbisCodecInfo, &flacCodecInfo,
                                          &alacCodecInfo, &mpeg4AspCodecInfo, &proresCodecInfo})
            {
                const bool demuxWrites =
                    only != &alacCodecInfo && only != &mpeg4AspCodecInfo && only != &proresCodecInfo;
                if (si.m_codec != only->programName || (m_demuxMode && demuxWrites))
                    continue;
                if (m_demuxMode)
                    THROW(ERR_COMMON, only->displayName
                                          << " can only be muxed into a Matroska file (.mkv): a demux has no file "
                                             "of its own to write it to. Choose MKV output, or leave the track out.")
                THROW(ERR_COMMON, only->displayName
                                      << " can only be muxed into a Matroska file (.mkv). It has no form in a "
                                         "transport stream or on a Blu-ray disc. Choose MKV output"
                                      << (demuxWrites ? ", a demux (--demux)" : "") << ", or leave the track out.")
            }
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
        // ** OPUS HAS NO FORM ON A DISC. ** A disc's clip information has no stream coding type for it, so its entry
        // there was written without one, and a disc of Opus alone was refused for want of "one audio or video stream"
        // to index, though there was one. It is refused as what it is, as the usage says: TS, MKV and demux only.
        else if (si.m_codec == opusCodecInfo.programName && m_bluRayMode)
            THROW(ERR_COMMON,
                  "Opus has no form on a Blu-ray or AVCHD disc: the clip information of a disc has no "
                  "stream coding type for it. Choose TS or MKV output, or leave the track out.")
        else if (si.m_codec == vvcCodecInfo.programName)
            notOnDisc.emplace_back("VVC (H.266)");
        // ** AV1 HAS NO FORM ON A DISC EITHER. ** As with Opus, the clip information has no stream coding type for it:
        // a disc of AV1 alone was refused for want of "one audio or video stream" to index, and AV1 beside an audio
        // track ended "Mux successful" with a playlist that has no video entry, a clip information entry of length 0
        // and a line about menus and DVB subtitles. It is refused as what it is.
        else if (si.m_codec == av1CodecInfo.programName && m_bluRayMode)
            THROW(ERR_COMMON,
                  "AV1 has no form on a Blu-ray or AVCHD disc: the clip information of a disc has no stream coding "
                  "type for it, so a disc could not index or list the video. Choose TS or MKV output, or leave the "
                  "track out.")
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
        // It named no way out, though there is one: a TrueHD track from a Matroska file usually has its AC-3 core
        // beside it as a track of its own, which is how tsMuxeR writes it, and the merge puts it back.
        if (mlpFound && !mlpMergedWithAc3)
            LTRACE(LT_ERROR, 2,
                   "Warning! MLP codec is not standard for BD disks, the disk will not play in a Blu-ray player. "
                   "A disc carries TrueHD with its AC-3 core on the same stream: add merge-ac3-track=<the AC-3 "
                   "track of the same file> or merge-ac3-file=<AC-3 file> to the A_MLP line, and leave that AC-3 "
                   "out as a track of its own.");
        for (const auto& codec : notOnDisc)
            LTRACE(LT_ERROR, 2,
                   "Warning! " << codec << " is not a Blu-ray format, the disk will not play in a Blu-ray player.");
        if ((V3_flags & DV) && !(V3_flags & BL_TRACK))
            LTRACE(LT_ERROR, 2,
                   "Warning! Dolby Vision Double Layer Single Tracks are not standard for BD disks, the disk will "
                   "not play in a Blu-ray player.");
    }
    // An AVCHD player plays H.264 video with AC-3 or LPCM sound. The codecs a Blu-ray player plays beyond those were
    // taken for an AVCHD disc without a word (decided: a warning, as a Blu-ray disc gets for the codecs it lacks).
    if (m_avchdMode)
    {
        std::vector<std::string> notOnAvchd;
        for (const StreamInfo& si : ci)
        {
            std::string what;
            if (si.m_codec == "V_MPEG-2")
                what = "MPEG-2 video";
            else if (si.m_codec == hevcCodecInfo.programName)
                what = "HEVC (H.265)";
            else if (si.m_codec == "V_MS/VFW/WVC1")
                what = "VC-1";
            else if (si.m_codec == mlpCodecInfo.programName)
                what = "TrueHD";
            else if (si.m_codec == "A_DTS")
                what = "DTS";
            else if (const auto* ac3 = dynamic_cast<const AC3StreamReader*>(si.m_streamReader))
            {
                if (ac3->isTrueHD() && si.m_addParams.find("down-to-ac3") == si.m_addParams.end())
                    what = "TrueHD";
                else if (ac3->isEAC3() && si.m_addParams.find("down-to-ac3") == si.m_addParams.end())
                    what = "E-AC-3 (Dolby Digital Plus)";
            }
            if (!what.empty() && std::find(notOnAvchd.begin(), notOnAvchd.end(), what) == notOnAvchd.end())
                notOnAvchd.push_back(what);
        }
        for (const auto& codec : notOnAvchd)
            LTRACE(LT_ERROR, 2,
                   "Warning! " << codec
                               << " is not an AVCHD format, the disk will not play in an AVCHD player, which plays "
                                  "H.264 video with AC-3 or LPCM sound. Choose --blu-ray for it, or convert it.");
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

// --custom-chapters and --chapters-file are times on the source's timeline, as the source's own chapters are, and a cut
// moves them the same way: the window writes a disc's chapters from the source's as --custom-chapters. A disc moved
// them back by the cut start and dropped those before it, so it had no mark at its start; a Matroska output did not
// move them at all, and kept chapters past its end. Both now keep the chapters inside the cut, moved back, with the one
// playing at the cut start opening the output at 0.
void MuxerManager::moveChaptersByCut(std::vector<double>& times, std::vector<AVChapter>& info) const
{
    if (m_cutStart <= 0 && m_cutEnd <= 0)
        return;
    const double start = m_cutStart > 0 ? static_cast<double>(m_cutStart) / INTERNAL_PTS_FREQ : 0.0;
    const double end = m_cutEnd > 0 ? static_cast<double>(m_cutEnd) / INTERNAL_PTS_FREQ : 1e300;
    std::vector<double> keptTimes;
    std::vector<AVChapter> keptInfo;
    int playing = -1;
    for (size_t i = 0; i < times.size(); ++i)
    {
        if (times[i] <= start)
        {
            if (playing < 0 || times[i] > times[playing])
                playing = static_cast<int>(i);
        }
        else if (times[i] < end)
        {
            keptTimes.push_back(times[i] - start);
            if (i < info.size())
            {
                AVChapter moved = info[i];
                moved.start = llround(keptTimes.back() * 1e9);
                moved.end = moved.end >= 0 ? moved.end - llround(start * 1e9) : -1;
                keptInfo.push_back(std::move(moved));
            }
        }
    }
    if (playing >= 0)
    {
        keptTimes.insert(keptTimes.begin(), 0.0);
        if (static_cast<size_t>(playing) < info.size())
        {
            AVChapter first = info[playing];
            first.start = 0;
            first.end = first.end > llround(start * 1e9) ? first.end - llround(start * 1e9) : -1;
            keptInfo.insert(keptInfo.begin(), std::move(first));
        }
    }
    times = keptTimes;
    if (!info.empty())
        info = keptInfo;
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
// gives the same image as before. A split that could not be reckoned still gets the plain refusal when the image is
// finished, which asks for --extra-iso-space, and that is added on top of the room worked out here. It replaced that
// room: the room of a split fell by the 4 blocks of its default, the message's own figure was too small, and a split
// counted from the length its source states was refused with --extra-iso-space=1 although it fit without the option.
int MuxerManager::getExtraISOBlocks()
{
    const int given = m_extraIsoBlocksGiven ? m_extraIsoSpace : 0;
    int64_t parts = 0;
    if (m_isoSplitBytes > 0)
        // A part is cut at the first key frame past the size, so none is smaller, and the transport stream adds
        // about 5 per cent to what the sources hold.
        parts = (std::max)(parts, totalSize() / 100 * 106 / m_isoSplitBytes + 1);
    if (m_isoSplitTicks > 0)
    {
        const auto toNano = [](const int64_t pts)
        { return static_cast<int64_t>(static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ)); };
        const int64_t fromNano = m_cutStart > 0 ? toNano(m_cutStart) : 0;
        const int64_t toEndNano = m_cutEnd > 0 ? toNano(m_cutEnd) : m_metaDemuxer.statedDurationNano();
        const double partNano = static_cast<double>(m_isoSplitTicks) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ);
        if (toEndNano > fromNano)
            parts = (std::max)(
                parts, static_cast<int64_t>((std::min)(static_cast<double>(toEndNano - fromNano) / partNano, 1e9)) + 1);
    }
    if (m_isoSplitChapters)
    {
        const size_t marks = m_chapters.empty() ? sourceChapters().size() : m_chapters.size();
        parts = (std::max)(parts, static_cast<int64_t>(marks) + 1);
    }
    if (parts <= 1)
        return m_extraIsoBlocks + given;

    // About twenty sectors for what every disc has, and three a part in 2D, seven in 3D; counted with room to spare.
    constexpr int64_t SECTORS_PER_BLOCK = ALLOC_BLOCK_SIZE / SECTOR_SIZE;
    const int64_t needed = 64 + parts * (getMvcFound() ? 8 : 4);
    const int64_t room = IsoWriter::metadataBlocks(totalSize(), m_extraIsoBlocks) * SECTORS_PER_BLOCK;
    if (needed <= room)
        return m_extraIsoBlocks + given;
    return m_extraIsoBlocks + static_cast<int>((needed - room + SECTORS_PER_BLOCK - 1) / SECTORS_PER_BLOCK) + given;
}

void MuxerManager::writeChapterFile(const std::string& outDirName)
{
    std::vector<AVChapter> chapters = sourceChapters();
    if (chapters.empty())
        return;
    // --cut-pictures=cover: the demuxed video begins at its key picture before the cut, and the chapters, times from
    // the cut, describe that video. The chapter playing at the cut stays at its start.
    const auto key = m_cutKeyFrom.find(m_cutMainVideo);
    const int64_t before = key != m_cutKeyFrom.end() ? cutKeyBefore(key->second) : 0;
    if (m_cutCover && m_cutStart > 0 && before > 0)
    {
        const auto shift =
            static_cast<int64_t>(static_cast<double>(before) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ) + 0.5);
        for (AVChapter& chapter : chapters)
        {
            if (chapter.start > 0)
                chapter.start += shift;
            if (chapter.end >= 0)
                chapter.end += shift;
        }
    }

    const std::string fileName = closeDirPath(toNativeSeparators(outDirName)) + "chapters.txt";
    const std::string shownName = shownOutputName(fileName);  // the demux may be written into a temporary folder
    TextFile file;
    if (!file.open(fileName.c_str(), File::ofWrite))
    {
        LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << shownName);
        return;
    }
    for (const auto& chapter : chapters)
        if (!file.writeLine(floatToTime(static_cast<double>(chapter.start) / 1e9)))
        {
            LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << shownName);
            file.close();
            return;
        }
    file.close();
    LTRACE(LT_INFO, 2, "Chapters written to " << shownName << ", " << chapters.size() << " of them");

    // chapters.txt is deliberately left as it is: one bare time per line, because it is meant to be
    // pasted into the window's chapter box and because someone's script may be reading it. It has
    // no room for the names, so they go into a second file in the form other tools read back.
    const std::string xmlName = closeDirPath(toNativeSeparators(outDirName)) + "chapters.xml";
    if (writeChapterXml(xmlName, chapters))
        LTRACE(LT_INFO, 2, "Chapters with their names written to " << shownOutputName(xmlName));
    else
        LTRACE(LT_WARN, 2, "Warning: can't write the chapter list to " << shownOutputName(xmlName));
}

void MuxerManager::doMux(const string& outFileName, FileFactory* fileFactory)
{
    // Run the discovery phase: probe all tracks to collect metadata
    // (channels, sample rate, resolution, codec-private, etc.) before
    // the main mux pipeline starts.  This is self-contained -- all
    // temporary I/O is closed before discoverStreams() returns.
    if (!m_discovered)
        m_discoveryData = m_metaDemuxer.discoverStreams();
    m_discovered = true;

    preinitMux(outFileName, fileFactory);

    // cover at a --cut-end: on a Blu-ray or AVCHD disc only, whose playlist stops playing at the cut (OUT_time). In a
    // file the pictures kept past the cut are shown, after a hole in the display order where the left out ones were.
    const bool coverEnd = m_cutCover && m_bluRayMode;
    // cover: the main video is known before the mux (findCutKeyPictures, the end of a --cut-end); inside: at its first
    // packet, as before
    if (m_cutCover && (m_cutStart > 0 || (coverEnd && m_cutEnd > 0)))
        findCutLayers(m_metaDemuxer.getStreamInfo());
    if (m_cutStart > 0)
        for (const StreamInfo& si : m_metaDemuxer.getStreamInfo())
            if (auto* copied = dynamic_cast<VideoCopyStreamReader*>(si.m_streamReader))
                copied->configUntilCutStart();
    findCutKeyPictures();
    applyCutOrigin();

    if (m_demuxMode)
        writeChapterFile(outFileName);

    m_fileWriter = std::make_unique<BufferedFileWriter>();
    AVPacket avPacket;
    bool anyMuxed = false;
    bool anyAfterCutStart = false;       // a packet of the sources at or after --cut-start, kept or not
    std::set<int> endedByCut;            // streams that have reached --cut-end
    std::set<int> withPackets;           // streams that have given a packet, subtitles left out (--cut-end)
    std::map<int, int64_t> firstMuxed;   // the time of each stream's first packet muxed (the demux of a --cut-start)
    int64_t mainVideoDts = -1;           // the decoding time of the main video's last packet (--cut-end)
    std::set<int> shownBeforeEnd;        // the video streams that have given a picture shown before --cut-end
    std::set<int> mutedByCut;            // --cut-start: the video streams that gave pictures, none of them kept
    std::map<int, int64_t> lastPicture;  // --cut-start: per video stream, the time of its last picture, and
    std::map<int, int64_t> lastKey;      // of its last key picture
    // A picture a cut leaves out is not counted in the reader's "Processed N video frames", which counted every
    // picture of the source: part 2 of a film cut in two said 15000 for its 7488
    const auto leftOut = [](const AVPacket& packet)
    {
        if (auto* video = dynamic_cast<MPEGStreamReader*>(packet.codec); video != nullptr && packet.size > 0)
            video->frameLeftOut();
    };

    // Only this loop looks at the signal: a signal before or after it ends the program as before (main)
    struct StopWatch
    {
        StopWatch() { setStopWatched(true); }
        ~StopWatch() { setStopWatched(false); }
        StopWatch(const StopWatch&) = delete;
        StopWatch& operator=(const StopWatch&) = delete;
    } stopWatch;
    while (true)
    {
        // Ctrl+C or SIGTERM ended the program where it stood, and the part written so far stayed under the name of
        // the output with no word. It now ends as a failed job does: the unfinished output is removed and said so.
        if (const int sig = stopSignal(); sig != 0)
            THROW(ERR_COMMON, "The job was stopped by " << (sig == SIGINT ? "Ctrl+C (SIGINT)" : "SIGTERM")
                                                        << " before it was finished.")
        const int avRez = m_metaDemuxer.readPacket(avPacket);

        if (avRez == BufferedReader::DATA_EOF)
            break;
        if (m_cutStart > 0)
        {
            if (avPacket.pts >= m_cutStart)
                anyAfterCutStart = true;
            // A video stream begins at its key picture at or before the cut (findCutKeyPictures), the rest at the cut
            const auto key = m_cutKeyFrom.find(avPacket.stream_index);
            int64_t from = key != m_cutKeyFrom.end() ? key->second : m_cutStart;
            // A Matroska file keeps its times in whole milliseconds, and the picture times are worked out from the
            // frame rate, so a key picture at 2043.708 ms is stored, listed and given as 2044 ms. A cut at that time
            // was taken to be after the picture, and the video began at the next key picture, 2 s later, with
            // --cut-pictures=inside and in 2.20.0 (measured: 382 pictures for 431). A picture of the video that is
            // less than half a millisecond before the cut is at it.
            if (key == m_cutKeyFrom.end() && isCutVideo(avPacket.codec))
                from -= INTERNAL_PTS_FREQ / 2000;
            if (isCutVideo(avPacket.codec))
            {
                lastPicture[avPacket.stream_index] = (std::max)(lastPicture[avPacket.stream_index], avPacket.pts);
                if (avPacket.flags & AVPacket::IS_IFRAME)
                    lastKey[avPacket.stream_index] = avPacket.pts;
                if (firstMuxed.count(avPacket.stream_index) == 0)
                    mutedByCut.insert(avPacket.stream_index);
            }
            if (avPacket.pts < from || !keptAfterCutStart(avPacket))
            {
                leftOut(avPacket);
                continue;
            }
            mutedByCut.erase(avPacket.stream_index);
        }
        if (m_cutEnd > 0)
        {
            // A subtitle packet past the cut is dropped, but it does not end the cut. Its timestamp
            // can run more than a second ahead of the pictures still being read, so stopping at it
            // cut the video and audio short: 213 pictures of a 10 second cut instead of 240.
            const bool subtitle = avPacket.codecID == CODEC_S_PGS || avPacket.codecID == CODEC_S_SRT ||
                                  avPacket.codecID == CODEC_S_SUP || avPacket.codecID == CODEC_S_MKVCOPY;
            // A stream ends at its first packet that is shown at or after the cut: a B picture that follows
            // such a picture refers to it and cannot be kept without it. The mux used to end for every stream
            // at that first packet of ANY stream. Video comes in decoding order, so the picture of another
            // stream decoded at nearly the same time was lost: the two layers of a Dolby Vision disc ended
            // with different pictures, and the last audio before the cut was lost as well.
            //
            // The mux ends when every stream that has given a packet has ended. The packets come nearly,
            // not exactly, in the order of their decoding times: the mux used to end at the first packet
            // decoded at or after the cut, and the sound read a little later than the pictures lost its
            // last frames before the cut, 2 AC-3 frames (64 ms) at a cut on an I picture. A stream that gives nothing
            // more ends the mux 2 s of decoding time past the cut, as nothing of it can be that late.
            //
            // --cut-pictures=cover: a video stream ends at its first picture DECODED at or after the cut. The B
            // pictures shown just before the cut are made from a picture shown at or after it, which is decoded before
            // them; it was left out, and they with it, so the pictures of up to a group were in neither part of a film
            // cut in two. A disc does not show what is kept past the cut (OUT_time, m_cutShownTo), and only a disc
            // ends so (coverEnd): any other output ends as with inside.
            // A video that shows nothing before the cut (it begins after it) ends as before, at its first picture shown
            // at or after the cut: nothing it shows needs that picture.
            if (!subtitle)
                withPackets.insert(avPacket.stream_index);
            const bool video = isCutVideo(avPacket.codec);
            if (video && avPacket.pts < m_cutEnd && endedByCut.count(avPacket.stream_index) == 0)
                shownBeforeEnd.insert(avPacket.stream_index);
            const bool endByDecoding = coverEnd && video && shownBeforeEnd.count(avPacket.stream_index) != 0;
            if ((endByDecoding ? avPacket.dts : avPacket.pts) >= m_cutEnd)
                endedByCut.insert(avPacket.stream_index);
            // With cover, the first picture of the main video shown at or after the cut, kept or not: the one a disc
            // stops before, and the next part starts with. It is known once a picture decoded after its time is read.
            const bool mainShownBefore = shownBeforeEnd.count(m_cutMainVideo) != 0;
            if (coverEnd && avPacket.stream_index == m_cutMainVideo && mainShownBefore)
            {
                if (avPacket.pts >= m_cutEnd && (m_cutShownTo < 0 || avPacket.pts < m_cutShownTo))
                    m_cutShownTo = avPacket.pts;
                mainVideoDts = avPacket.dts;
            }
            const bool shownToKnown =
                !coverEnd || !mainShownBefore || (m_cutShownTo >= 0 && mainVideoDts > m_cutShownTo);
            if (!subtitle && avPacket.dts >= m_cutEnd)
            {
                const bool allEnded = std::all_of(withPackets.begin(), withPackets.end(),
                                                  [&](const int stream) { return endedByCut.count(stream) != 0; });
                if ((allEnded && shownToKnown) || avPacket.dts >= m_cutEnd + 2 * INTERNAL_PTS_FREQ)
                    break;
            }
            if (endedByCut.count(avPacket.stream_index) != 0)
            {
                leftOut(avPacket);
                continue;
            }
        }

        // The first picture of the main video shown at or after --cut-start: where a disc starts playing
        // (TSMuxer::setCutPlayTimes)
        if (m_cutStartCovered && avPacket.stream_index == m_cutMainVideo && avPacket.pts >= m_cutStart &&
            (m_cutShownFrom < 0 || avPacket.pts < m_cutShownFrom))
            m_cutShownFrom = avPacket.pts;
        firstMuxed.emplace(avPacket.stream_index, avPacket.pts);
        anyMuxed = true;
        if (m_subStreamIndex.find(avPacket.stream_index) != m_subStreamIndex.end())
            m_subMuxer->muxPacket(avPacket);
        else
            m_mainMuxer->muxPacket(avPacket);
    }

    // Everything is read. What follows can take a while on a large output and can still fail, so the progress line
    // stays below 100 percent (METADemuxer::updateReport) and this says what is going on.
    if (!m_finishingNote.empty())
        LTRACE(LT_INFO, 2, m_finishingNote);

    // A demux writes no times: its files all start at 0. The sound of a --cut-start begins at the cut, and the picture
    // at a key picture, before the cut (--cut-pictures=cover) or after it (inside), and nothing said by how much they
    // differ, which a mux of the files needs to know.
    const auto mainFirst = firstMuxed.find(m_cutMainVideo);
    if (m_demuxMode && m_cutStart > 0 && mainFirst != firstMuxed.end())
        for (const StreamInfo& si : m_metaDemuxer.getStreamInfo())
        {
            const auto first = firstMuxed.find(si.m_streamReader->getStreamIndex());
            if (si.m_codec.empty() || si.m_codec[0] != 'A' || first == firstMuxed.end())
                continue;
            const int64_t ms = llround(static_cast<double>(first->second - mainFirst->second) * 1000.0 /
                                       static_cast<double>(INTERNAL_PTS_FREQ));
            if (ms != 0)
                LTRACE(LT_INFO, 2,
                       "The sound of track " << si.m_streamReader->getStreamIndex() << " starts " << std::llabs(ms)
                                             << " ms " << (ms > 0 ? "after" : "before")
                                             << " the first picture: give it timeshift=" << ms
                                             << "ms when these files are muxed together.");
        }

    // The places a disc playlist starts and stops playing at, before the clips are closed
    for (AbstractMuxer* muxer : {m_mainMuxer.get(), m_subMuxer.get()})
        if (auto* ts = dynamic_cast<TSMuxer*>(muxer))
            ts->setCutPlayTimes(m_cutShownFrom, m_cutShownTo);

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
    // The sources can go on past the start and still give nothing: a video track starts at a key picture, and an
    // 8 s stream with one every 8 s cut at 7.5 s has none after the cut. That was reported as a start at or past
    // the end.
    if (m_cutStart > 0 && !anyMuxed)
    {
        std::ostringstream msg;
        msg << "--cut-start (" << static_cast<double>(m_cutStart) / INTERNAL_PTS_FREQ << "s) ";
        if (anyAfterCutStart)
            msg << "leaves nothing to mux: the sources go on after it, but a video track starts at a key picture, and "
                   "there is none after the cut. The output holds no media. Choose an earlier start.";
        else
            msg << "is at or past the end of the sources: the output holds no media.";
        throw std::runtime_error(msg.str());
    }
    // ** A CUT THAT LEAVES THE MAIN VIDEO OUT IS NOT A SUCCESS EITHER. ** A cut inside the last picture of a film, or
    // with --cut-pictures=inside after its last key picture, kept no picture but the sound that runs a frame or two
    // past it, and the output was reported as a finished mux: a TS with one AC-3 frame and no video, a disc playlist
    // without a play item. The rule above saw that something was muxed. The track empty after a cut is not checked
    // later (checkEmptyTracks), so it is said here, with the latest start that keeps a picture.
    if (m_cutStart > 0 && !mutedByCut.empty())
        findCutLayers(m_metaDemuxer.getStreamInfo());
    if (m_cutStart > 0 && mutedByCut.count(m_cutMainVideo) != 0)
    {
        const bool cover = m_cutCover && !m_cutPassFailed;
        const auto at = [](const int64_t t) { return floatToTime(static_cast<double>(t) / INTERNAL_PTS_FREQ); };
        const auto key = lastKey.find(m_cutMainVideo);
        std::ostringstream msg;
        msg << "--cut-start (" << static_cast<double>(m_cutStart) / INTERNAL_PTS_FREQ
            << "s) leaves no picture of track " << m_cutMainVideo << ", the video: ";
        if (cover || key == lastKey.end())
            msg << "its last picture is shown from " << at(lastPicture[m_cutMainVideo])
                << ", before the cut, and nothing of it after the cut.";
        else
            msg << "with --cut-pictures=inside a video begins at a key picture at or after the cut, and its last key "
                   "picture is at "
                << at(key->second) << ".";
        msg << " The output would hold no video. Choose a start at or before "
            << at(cover || key == lastKey.end() ? lastPicture[m_cutMainVideo] : key->second);
        if (cover && key != lastKey.end())
            msg << ", where the picture begins at the key picture at " << at(key->second);
        msg << (cover ? "." : ", or --cut-pictures=cover.");
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
        }
        file.readLine(str);
    }
    file.close();

    m_metaDemuxer.setDemuxOutput(m_demuxMode);
    m_metaDemuxer.openFile(fileName);
    m_metaFileName = fileName;
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
    // A quote that is not closed took the rest of the line into one value: --label="Disc --cut-end=5s made a label
    // of all of it, and the cut was not made, without a word
    if (quoted)
        THROW(ERR_COMMON, "A quote on the MUXOPT line is not closed: " << trimStr(line))
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
                                                      "--cut-pictures",
                                                      "--cut-start",
                                                      "--demux",
                                                      "--disc-size",
                                                      "--dv-profile",
                                                      "--extra-iso-space",
                                                      "--inner-only",
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
                                                      "--no-layer-break-guard",
                                                      "--no-pcr-on-video-pid",
                                                      "--playlist-clips",
                                                      "--right-eye",
                                                      "--split-chapters",
                                                      "--split-duration",
                                                      "--split-size",
                                                      "--start-time",
                                                      "--title",
                                                      "--vbr",
                                                      "--vbv-len"};
    static const std::set<std::string> commandLineOptions = {"--3d-single-copy",   "--bdmv-to-iso", "--disc-capacity",
                                                             "--keep-extra-files", "--lang-codes",  "--no-layer-fit",
                                                             "--original-order"};
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
        vector<string> paramPair = splitOption(trimStr(i));
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
        else if (paramPair[0] == "--playlist-clips")
        {
            const string how = paramPair.size() > 1 ? strToLowerCase(paramPair[1]) : string();
            if (how != "played" && how != "whole")
                THROW(ERR_COMMON, "--playlist-clips takes played or whole, not \""
                                      << (paramPair.size() > 1 ? paramPair[1] : "") << "\".")
            m_metaDemuxer.setPlaylistClipsPlayed(how == "played");
        }
        else if (paramPair[0] == "--cut-pictures")
        {
            const string how = paramPair.size() > 1 ? strToLowerCase(paramPair[1]) : string();
            if (how != "cover" && how != "inside")
                THROW(ERR_COMMON, "--cut-pictures takes cover or inside, not \""
                                      << (paramPair.size() > 1 ? paramPair[1] : "") << "\".")
            m_cutCover = how == "cover";
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
            {
                // Read as the split reads it (checkedSplitDuration): as whole seconds, 2000ms was 2000 s, so an image
                // split into 2 s parts got the room of one part and was refused after the mux.
                // A value the split refuses is left for it to refuse.
                int64_t ticks = 0;
                if (parseTimeToTicks(paramPair[1], "s", &ticks) && ticks > 0)
                    m_isoSplitTicks = ticks;
            }
        }
        else if (paramPair[0] == "--split-chapters")
            m_isoSplitChapters = true;
        else if (paramPair[0] == "--extra-iso-space")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            m_extraIsoSpace = strToInt32(paramPair[1]);
            m_extraIsoBlocksGiven = true;
        }
        else if (paramPair[0] == "--blu-ray" || paramPair[0] == "--blu-ray-v3" || paramPair[0] == "--avchd")
        {
            m_bluRayMode = true;
            m_avchdMode = paramPair[0] == "--avchd";
            V3_flags |= BLURAY_OUT;
        }
        else if (paramPair[0] == "--demux")
        {
            m_demuxMode = true;
        }
        // It was looked for in the text of the line, so a label or a title holding "right-eye" made the right eye
        // the base view of a 3D disc.
        else if (paramPair[0] == "--right-eye")
            m_mvcBaseViewR = true;
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
        // Read here as well for the fit-to-disc estimate; the transport stream muxer reads and checks them for the mux
        else if ((paramPair[0] == "--bitrate" || paramPair[0] == "--maxbitrate" || paramPair[0] == "--minbitrate") &&
                 paramPair.size() > 1)
        {
            const auto rate = static_cast<int64_t>(strToDouble(paramPair[1].c_str()) * 1000.0);
            if (paramPair[0] == "--bitrate")
                m_padRateGiven = rate;
            else if (paramPair[0] == "--maxbitrate")
                m_padMaxRate = rate;
            else
                m_padMinRate = rate;
        }
        // It was a command line option of --bdmv-to-iso only and was ignored here with a warning: an image muxed
        // directly could not keep its data on the inner part of the disc, only one made from a folder in a second step.
        else if (paramPair[0] == "--inner-only")
            m_innerOnly = true;
        else if (paramPair[0] == "--no-layer-break-guard")
            m_noLayerBreakGuard = true;
        else if (paramPair[0] == "--layer-break-guard")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            if (const std::string err = parseGuardMB(paramPair[0], paramPair[1], m_layerBreakGuardMB); !err.empty())
                THROW(ERR_COMMON, err);
        }
        else if (paramPair[0] == "--layer-break-guard-before")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            if (const std::string err = parseGuardMB(paramPair[0], paramPair[1], m_layerBreakGuardBeforeMB);
                !err.empty())
                THROW(ERR_COMMON, err);
        }
        else if (paramPair[0] == "--no-chapters")
            m_noChapters = m_noChaptersTyped = true;
        // An empty list is a list: the window writes it when every line of its chapter box was deleted. It was read as
        // no list at all, and the source's chapters came back. A list of empty places only (;) is empty as well.
        else if (paramPair[0] == "--custom-chapters" &&
                 (paramPair.size() < 2 || trimStr(paramPair[1]).find_first_not_of("; \t\"") == std::string::npos))
            m_noChapters = true;
        else if (paramPair[0] == "--layer-break-lbn")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0]);
            // One sector for BD-R/RE DL, or a comma-separated list for BDXL (100 GB = 2 breaks,
            // 128 GB = 3): e.g. --layer-break-lbn=15768576,31537152
            // one reading, shared with the --bdmv-to-iso path
            std::vector<std::string> warnings;
            if (const std::string err = parseLayerBreakList(paramPair[1], m_layerBreakLbns, warnings); !err.empty())
                THROW(ERR_COMMON, "Invalid --layer-break-lbn value '" << paramPair[1] << "': " << err);
            for (const auto& warning : warnings) LTRACE(LT_WARN, 2, "Warning: " << warning);
        }
    }
    // --inner-only works out the zeros at each layer break itself, from the disc and the data, so a guard given by
    // hand on the same line is one of two answers to one question. It was decided (12 Oct) that on a meta line the two
    // exclude each other and the job ends before anything is written, naming both, as the window's tick already does
    // by greying the guard boxes. The mux printed one line saying the guard is not used and went on with rc 0, so a
    // script that asked for a 64 MB guard got another padding without an error. --bdmv-to-iso keeps taking both, as
    // it did when it was released.
    if (m_innerOnly && (m_layerBreakGuardMB >= 0 || m_layerBreakGuardBeforeMB >= 0))
    {
        const char* guardName = m_layerBreakGuardMB >= 0 && m_layerBreakGuardBeforeMB >= 0
                                    ? "--layer-break-guard and --layer-break-guard-before"
                                : m_layerBreakGuardMB >= 0 ? "--layer-break-guard"
                                                           : "--layer-break-guard-before";
        THROW(ERR_COMMON, "--inner-only and " << guardName
                                              << " cannot be used together. --inner-only works out its own padding at "
                                                 "the layer break from the size of the disc. Remove one of the two. "
                                                 "Nothing has been written.")
    }
    // Decided 27 Oct: a guard given by size is written, and the line says the other option is not used.
    if (m_noLayerBreakGuard && (m_layerBreakGuardMB >= 0 || m_layerBreakGuardBeforeMB >= 0))
        LTRACE(LT_WARN, 2,
               "Warning: --no-layer-break-guard and a guard size are both given; the guard of the size given is "
               "used.");
}

void MuxerManager::findCutLayers(const std::vector<StreamInfo>& ci)
{
    if (m_cutLayersKnown)
        return;
    // In the order of the meta file, the order the transport stream muxer tells the layers apart in: a Dolby
    // Vision stream is an enhancement layer only when a main video came before it (profiles 5 and 8 have one
    // stream, and it is the main video).
    m_cutLayersKnown = true;
    int mainVideos = 0;
    for (const StreamInfo& si : ci)
    {
        if (!isCutVideo(si.m_streamReader))
            continue;
        const auto* reader = dynamic_cast<const MPEGStreamReader*>(si.m_streamReader);
        const auto* h264 = dynamic_cast<const H264StreamReader*>(reader);
        if (reader != nullptr && (si.m_isSubStream || (h264 != nullptr && h264->isSubStream()) ||
                                  (reader->getStreamHDR() == 4 && mainVideos > 0)))
            m_cutLayers.insert(si.m_streamReader->getStreamIndex());
        else if (!si.m_streamReader->isSecondary())
        {
            if (mainVideos++ == 0)
                m_cutMainVideo = si.m_streamReader->getStreamIndex();
        }
    }
}

// A video that a cut begins at a key picture: every video a parser reads, and a video copied from an MP4 file into a
// Matroska one, whose key pictures the source marks
bool MuxerManager::isCutVideo(const BaseAbstractStreamReader* reader)
{
    return dynamic_cast<const MPEGStreamReader*>(reader) != nullptr ||
           dynamic_cast<const VideoCopyStreamReader*>(reader) != nullptr;
}

// ** --cut-pictures=cover: THE PICTURE OF A --cut-start BEGINS AT THE KEY PICTURE AT OR BEFORE THE CUT. ** A video that
// is not encoded again can begin only at a key picture, and it began at the first one after the cut: the pictures
// between the cut and that key picture were in no part of a film cut in two, up to a group of pictures, 0.44 s
// on a DVD source. It now begins at the last key picture at or before the cut, so that every picture after the cut is
// there; the sound and the subtitles still begin at the cut, and a disc playlist starts playing at the first picture
// shown at or after it (TSMuxer::setCutPlayTimes), so a player does not show the pictures before it.
//
// That key picture is known only once a picture decoded after the cut has been read, and the muxer asks a reader for
// what goes with a packet when it takes it, from the reader's state at that moment, so the packets cannot be held back
// until then. The sources are read up to the cut by a pass of their own first, from the same meta file and with the
// same settings, which gives the same times: each video's last key picture with a time at or before the cut. Should
// the mux not meet a key picture at that time, it begins at the first one after it, as before at worst.
void MuxerManager::findCutKeyPictures()
{
    if (m_cutStart <= 0 || !m_cutCover || m_metaFileName.empty())
        return;
    const std::vector<StreamInfo>& main = m_metaDemuxer.getStreamInfo();
    std::set<int> own;  // the videos that begin at a key picture of their own
    for (const StreamInfo& si : main)
        if (isCutVideo(si.m_streamReader) && m_cutLayers.count(si.m_streamReader->getStreamIndex()) == 0)
            own.insert(si.m_streamReader->getStreamIndex());
    if (own.empty())
        return;
    LTRACE(LT_INFO, 2,
           "Looking for the key picture at or before --cut-start ("
               << floatToTime(static_cast<double>(m_cutStart) / INTERNAL_PTS_FREQ) << ")...");

    // The pass says nothing: what its readers have to say, the mux says again
    struct NullBuffer final : std::streambuf
    {
        int overflow(const int c) override { return traits_type::not_eof(c); }
    };
    struct Silence
    {
        NullBuffer null;
        std::streambuf* out = std::cout.rdbuf(&null);
        std::streambuf* err = std::cerr.rdbuf(&null);
        Silence() = default;
        Silence(const Silence&) = delete;
        Silence& operator=(const Silence&) = delete;
        ~Silence()
        {
            std::cout.rdbuf(out);
            std::cerr.rdbuf(err);
        }
    };
    std::map<int, int64_t> found;
    std::string failure;
    {
        Silence silence;
        try
        {
            METADemuxer pass(m_readManager);
            pass.takeSettingsOf(m_metaDemuxer);
            pass.setQuiet(true);
            pass.openFile(m_metaFileName);
            std::vector<StreamInfo>& ci = pass.getCodecInfo();
            if (ci.size() != main.size())
                throw std::runtime_error("the meta file gave other tracks");
            for (size_t i = 0; i < ci.size(); ++i)
            {
                if (auto* h264 = dynamic_cast<H264StreamReader*>(ci[i].m_streamReader))
                    h264->setStartPTS(m_ptsOffset);
                ci[i].read();
                if (i < m_discoveryData.size() && m_discoveryData[i].discovered)
                    ci[i].m_streamReader->applyDiscoveryData(m_discoveryData[i]);
            }
            // A picture decoded after the cut is shown after it as well, and so is every one after it
            std::set<int> past;
            std::set<int> reaches;  // the videos with a picture shown at or after the cut
            AVPacket packet;
            while (past.size() < own.size() && pass.readPacket(packet) != BufferedReader::DATA_EOF)
            {
                if (own.count(packet.stream_index) == 0)
                    continue;
                if ((packet.flags & AVPacket::IS_IFRAME) && packet.pts <= m_cutStart)
                    found[packet.stream_index] = packet.pts;
                if (packet.pts >= m_cutStart)
                    reaches.insert(packet.stream_index);
                if (packet.dts > m_cutStart)
                    past.insert(packet.stream_index);
            }
            // A video that ends before the cut keeps the rule of inside: it gives nothing, and a cut at or past the
            // end of the sources is refused as before, with the output that was there kept
            for (auto it = found.begin(); it != found.end();)
                it = reaches.count(it->first) != 0 ? std::next(it) : found.erase(it);
        }
        catch (const std::exception& e)
        {
            failure = e.what();
        }
        catch (...)
        {
            failure = "unknown error";
        }
    }
    m_cutPassFailed = !failure.empty();
    if (!failure.empty())
    {
        LTRACE(LT_WARN, 2,
               "Warning: the key picture before --cut-start could not be found ("
                   << failure << "). The picture begins at the first key picture after the cut.");
        return;
    }
    m_cutKeyFrom = found;
    // A layer begins with the main video (keptAfterCutStart): its packets before that are not dropped by their time
    const auto mainKey = found.find(m_cutMainVideo);
    m_cutStartCovered = mainKey != found.end();
    if (mainKey != found.end())
        for (const int layer : m_cutLayers) m_cutKeyFrom[layer] = mainKey->second;
    if (mainKey != found.end() && cutKeyBefore(mainKey->second) > 0)
        LTRACE(LT_INFO, 2,
               "The picture begins at the key picture at "
                   << floatToTime(static_cast<double>(mainKey->second) / INTERNAL_PTS_FREQ) << ", "
                   << llround(static_cast<double>(m_cutStart - mainKey->second) * 1000.0 / INTERNAL_PTS_FREQ)
                   << " ms before the cut; the sound and subtitles begin at the cut.");
}

// How long before --cut-start a key picture is, in whole ticks of the 90 kHz clock. A key picture on the cut whose
// time came through another clock (the ns of a Matroska file) is a few internal ticks before it, and moved the
// chapters by 11 ns.
int64_t MuxerManager::cutKeyBefore(const int64_t key) const
{
    return key < m_cutStart ? (m_cutStart - key) / INT_FREQ_TO_TS_FREQ * INT_FREQ_TO_TS_FREQ : 0;
}

// --cut-pictures=cover with a --cut-start: the chapters of the cut are times from the cut (moveChaptersByCut,
// readSourceChapters), and the outputs that are timed from their first picture now start before it
void MuxerManager::applyCutOrigin()
{
    if (!m_cutStartCovered)
        return;
    // A transport stream: a --split-chapters part starts at the key picture of its chapter, counted from the cut, not
    // from the first packet
    for (AbstractMuxer* muxer : {m_mainMuxer.get(), m_subMuxer.get()})
        if (auto* ts = dynamic_cast<TSMuxer*>(muxer))
            ts->setChapterOrigin(m_cutStart);
    // Matroska: time 0 is the first picture
    int64_t origin = m_cutStart;
    for (const auto& [stream, from] : m_cutKeyFrom) origin = (std::min)(origin, from);
    if (auto* mkv = dynamic_cast<MatroskaMuxer*>(m_mainMuxer.get()); mkv != nullptr && cutKeyBefore(origin) > 0)
        mkv->setCutOrigin(cutKeyBefore(origin));
}

// A cut that does not start at the beginning kept every packet from the cut time on, so its video began with whatever
// picture came next. A picture other than an I-frame cannot be decoded without the ones before it, and a stream from
// MP4 or Matroska holds its parameter sets once, ahead of its first picture, which the cut had dropped: such a cut
// gave sound but no picture at all. Each video now begins with a key picture, at or before the cut (cover) or at or
// after it (inside), and that picture is told it opens a new file, the way the first picture of a split part is, so
// the reader writes the parameter sets in front of it when its group has none. The pictures shown before it are left
// out: none of them could be shown. A layer that only adds to another video (the second view of a 3D stream, a Dolby
// Vision enhancement layer) begins at the same picture as the main video rather than at an I-frame of its own. Audio
// and subtitles keep to the cut time, as before. A video copied from an MP4 file into a Matroska one began at any
// picture, one that could not be decoded: it begins at a key picture as well.
bool MuxerManager::keptAfterCutStart(AVPacket& avPacket)
{
    auto* video = dynamic_cast<AbstractStreamReader*>(avPacket.codec);
    if (!isCutVideo(video))
        return true;
    findCutLayers(m_metaDemuxer.getStreamInfo());
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
        // The safety net of findCutKeyPictures: said, not hidden
        const auto key = m_cutKeyFrom.find(avPacket.stream_index);
        if (key != m_cutKeyFrom.end() && avPacket.pts != key->second)
            LTRACE(LT_WARN, 2,
                   "Warning: track " << avPacket.stream_index << " has no key picture at "
                                     << floatToTime(static_cast<double>(key->second) / INTERNAL_PTS_FREQ)
                                     << " in the mux; it begins at the one at "
                                     << floatToTime(static_cast<double>(avPacket.pts) / INTERNAL_PTS_FREQ) << ".");
    }
    // A layer keeps every picture of the main video's from its first one on. With its own first picture as the
    // bound, a layer whose pictures are reordered otherwise than the main video's lost the ones decoded after it and
    // shown before it: the enhancement layer of a Dolby Vision file began 2 pictures after its base layer.
    m_cutVideoFrom[avPacket.stream_index] =
        m_cutLayers.count(avPacket.stream_index) > 0 ? m_cutVideoStart : avPacket.pts;
    video->onSplitEvent();
    return true;
}

void MuxerManager::waitForWriting() const
{
    while (!m_fileWriter->isQueueEmpty()) Process::sleep(1);
}

std::unique_ptr<AbstractMuxer> MuxerManager::createMuxer() { return m_factory.newInstance(this); }

// ** THE FIT-TO-DISC ESTIMATE DID NOT COUNT THE NULL PACKETS OF A CONSTANT BITRATE. **
//
// It is the size of the sources and five per cent, which holds while the muxer writes what it is given. With
// --bitrate it pads every second up to the rate, and the output is the rate times the length, whatever the sources
// hold: an image of 449 MB was estimated at 208 MB and passed for a disc of 300 MB. The rate padded to is the one
// TSMuxer::parseMuxOpt settles on: --bitrate, held to --maxbitrate when that is lower; without --bitrate, --minbitrate
// under a --maxbitrate (alone it pads nothing). The length is what the sources state, less a cut. Each 188 byte
// packet the rate counts is 192 bytes in a disc's clip.
static int64_t padRateOf(const int64_t given, const int64_t maxRate, const int64_t minRate)
{
    if (given > 0)
        return maxRate > 0 ? (std::min)(given, maxRate) : given;
    if (maxRate > 0 && minRate > 0)
        return (std::min)(minRate, maxRate);
    return 0;
}

bool MuxerManager::paddedLengthUnknown()
{
    return padRateOf(m_padRateGiven, m_padMaxRate, m_padMinRate) > 0 && paddedOutputSize() == 0;
}

int64_t MuxerManager::paddedOutputSize()
{
    const int64_t rate = padRateOf(m_padRateGiven, m_padMaxRate, m_padMinRate);
    if (rate <= 0)
        return 0;
    const auto toNano = [](const int64_t pts)
    { return static_cast<int64_t>(static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ)); };
    const int64_t stated = m_metaDemuxer.statedDurationNano(true);
    if (stated <= 0)
        return 0;
    const int64_t fromNano = m_cutStart > 0 ? toNano(m_cutStart) : 0;
    const int64_t toEndNano = m_cutEnd > 0 ? (std::min)(toNano(m_cutEnd), stated) : stated;
    if (toEndNano <= fromNano)
        return 0;
    // 3D is written as two transport streams, one for each view (preinitMux), and each is padded to the rate
    int views = 1;
    if (m_allowStereoMux)
        for (const StreamInfo& si : m_metaDemuxer.getStreamInfo())
            if (si.m_isSubStream)
                views = 2;
    const double bytes = static_cast<double>(rate) / 8.0 * static_cast<double>(toEndNano - fromNano) / 1e9 * views;
    return static_cast<int64_t>(bytes / 188.0 * 192.0);
}

// ** THE FIT-TO-DISC ESTIMATE COUNTED THE WHOLE SOURCE FILES. **
//
// A cut of a minute out of a clip larger than a BD-25 was estimated at more than the whole clip and refused for a
// BD-25 before the mux, and a cut that fits a BD-50 lost the layer break guard as "not fitting" it. The cut takes its
// share of the stated length, a quarter more and a point, as the rate of a part can run above the average of the whole;
// without a stated length the whole sources are counted, as before.
int64_t MuxerManager::muxedSourceSize()
{
    const int64_t size = m_metaDemuxer.selectedSourceSize();
    if (m_cutStart <= 0 && m_cutEnd <= 0)
        return size;
    const auto toNano = [](const int64_t pts)
    { return static_cast<int64_t>(static_cast<double>(pts) * 1e9 / static_cast<double>(INTERNAL_PTS_FREQ)); };
    const int64_t stated = m_metaDemuxer.statedDurationNano(true);
    if (stated <= 0)
        return size;
    const int64_t fromNano = m_cutStart > 0 ? toNano(m_cutStart) : 0;
    const int64_t toEndNano = m_cutEnd > 0 ? (std::min)(toNano(m_cutEnd), stated) : stated;
    if (toEndNano <= fromNano)
        return size;
    const double share =
        (std::min)(1.0, static_cast<double>(toEndNano - fromNano) / static_cast<double>(stated) * 1.25 + 0.01);
    return static_cast<int64_t>(static_cast<double>(size) * share);
}

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
    // Counted as a disc numbers its subtitles: the eyes of a stereo subtitle, its aligned subtitles (srcpgpart) and
    // picture-in-picture subtitles (srcpip) are no entries of the PG list. Counted with them, the disc start of a
    // stereo subtitle marked default selected its left eye.
    const auto idx = seekDefaultTrack(m_metaDemuxer.getStreamInfo(), paramVal,
                                      [](auto&& streamInfo)
                                      {
                                          return streamInfo.m_codec[0] == 'S' &&
                                                 !streamInfo.m_addParams.count("srcpgpart") &&
                                                 !streamInfo.m_addParams.count("srcpip");
                                      });
    if (idx != -1)
    {
        // Read as the Matroska output reads it: default=Forced, yes and 1 gave a disc no default subtitle, with only
        // a warning, while a Matroska output marked the track. A value that is none of them is refused with the track.
        mode = defaultParamKind(paramVal) == 2 ? SubTrackMode::Forced : SubTrackMode::All;
    }
    return idx;
}
