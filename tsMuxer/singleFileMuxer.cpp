// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "singleFileMuxer.h"

#include <algorithm>

#include <fs/directory.h>
#include <fs/systemlog.h>
#include <fs/textfile.h>

#include "abstractMuxer.h"
#include "ac3Codec.h"
#include "ac3StreamReader.h"
#include "lpcmStreamReader.h"
#include "mkvCopyStreamReader.h"
#include "mpegAudioStreamReader.h"
#include "muxerManager.h"
#include "opusStreamReader.h"
#include "pgsStreamReader.h"
#include "srtStreamReader.h"
#include "trueHDAC3MergeReader.h"
#include "vodCoreException.h"
#include "vorbisStreamReader.h"

#ifndef win32
#include <cstdio>
#endif

using namespace std;

std::string getNewName(const std::string& oldName, const int cnt)
{
    if (strEndWith(oldName, ".wav"))
        return oldName.substr(0, oldName.size() - 4) + "." + int32ToStr(cnt) + ".wav";
    return oldName + ".wav" + int32ToStr(cnt);
}

SingleFileMuxer::SingleFileMuxer(MuxerManager* owner) : AbstractMuxer(owner), m_lastIndex(-1) {}

SingleFileMuxer::~SingleFileMuxer()
{
    for (const auto& [index, outputs] : m_streamInfo)
        for (StreamInfo* si : outputs) delete si;
}

void SingleFileMuxer::intAddStream(const std::string& streamName, const std::string& codecName, int streamIndex,
                                   const map<string, string>& params, AbstractStreamReader* codecReader)
{
    codecReader->setDemuxMode(true);
    string fileExt = "track";

    // this call seemingly does nothing, but it actually makes some
    // StreamReaders refresh/set their private variables, enabling proper
    // codec->extension mapping in here. don't remove it.
    uint8_t descrBuffer[188];
    codecReader->getTSDescriptor(descrBuffer, true, true);

    if (codecName == "A_AAC")
    {
        fileExt = ".aac";
    }
    else if (codecName == "A_MP3")
    {
        const auto mp3Reader = dynamic_cast<MpegAudioStreamReader*>(codecReader);
        if (mp3Reader->getLayer() == 3)
            fileExt = ".mp3";
        else
            fileExt = ".mpa";
    }
    else if (codecName == "A_MLP")
    {
        if (dynamic_cast<TrueHDAC3MergeReader*>(codecReader))
            fileExt = ".ac3+thd";
        else
            fileExt = ".thd";
    }
    else if (codecName == "A_DTS")
    {
        fileExt = ".dts";
    }
    else if (codecName == "A_LPCM")
    {
        // A RIFF header states its sizes in 32 bits, so it cannot describe a file past 4 GiB and a
        // long track has to come out as two, each valid and with nothing lost. Wave64 is the same
        // audio with 16 byte chunk ids and 64 bit sizes, and tsMuxeR has always been able to READ
        // it: this is the writing side.
        if (params.find("w64") != params.end())
        {
            fileExt = ".w64";
            if (const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(codecReader))
                lpcmReader->setDemuxToW64(true);
        }
        else
            fileExt = ".wav";
    }
    else if (codecName == "A_FLAC")
    {
        fileExt = ".flac";
    }
    else if (codecName == "A_WAVPACK4")
    {
        fileExt = ".wv";
    }
    else if (codecName == "A_OPUS")
    {
        fileExt = ".opus";
        // the samples the source trims from the end of the last packet, for the granule position of the last page
        if (const auto src = params.find("srcendpadding"); src != params.end())
            if (const auto opus = dynamic_cast<OpusStreamReader*>(codecReader))
                opus->setEndTrim(strtoll(src->second.c_str(), nullptr, 10));
    }
    else if (codecName == "A_VORBIS")
    {
        fileExt = ".ogg";
        // the samples the source trims from the end of the last packet, for the granule position of the last page
        if (const auto src = params.find("srcendpadding"); src != params.end())
            if (const auto vorbis = dynamic_cast<VorbisStreamReader*>(codecReader))
                vorbis->setEndTrim(strtoll(src->second.c_str(), nullptr, 10));
    }
    else if (codecName == "A_AC3")
    {
        const auto ac3Reader = dynamic_cast<AC3StreamReader*>(codecReader);
        if (ac3Reader->isTrueHD() && !ac3Reader->getDownconvertToAC3())
            fileExt = params.find("drop-ac3-core") != params.end() ? ".thd" : ".ac3+thd";
        else if (ac3Reader->isEAC3() && !ac3Reader->getDownconvertToAC3())
        {
            if (ac3Reader->isAC3())
                fileExt = ".ac3+ec3";
            else
                fileExt = ".ec3";
        }
        else
            fileExt = ".ac3";
    }
    else if (codecName == "S_SUP" || codecName == "S_HDMV/PGS" || codecName == "S_TEXT/UTF8")
    {
        // An SRT track kept as text is written back as an SRT file, every other one as pictures. ASS, SSA and WebVTT
        // from a Matroska file, kept as text, are written in their own form.
        const auto srt = dynamic_cast<SRTStreamReader*>(codecReader);
        if (const auto copy = dynamic_cast<MatroskaCopyStreamReader*>(codecReader))
        {
            const std::string& id = copy->sourceCodecId();
            fileExt = id == "S_VOBSUB" ? ".sub" : id == "S_TEXT/ASS" ? ".ass" : id == "S_TEXT/SSA" ? ".ssa" : ".vtt";
        }
        else
            fileExt = srt && srt->keepText() ? ".srt" : ".sup";
    }
    else if (codecName[0] == 'V')
    {
        if (codecName == "V_MS/VFW/WVC1")
        {
            fileExt = ".vc1";
        }
        else if (codecName == "V_MPEG4/ISO/AVC")
        {
            fileExt = ".264";
        }
        else if (codecName == "V_MPEG4/ISO/MVC")
        {
            fileExt = ".mvc";
        }
        else if (codecName == "V_MPEGH/ISO/HEVC")
        {
            fileExt = ".hevc";
        }
        else if (codecName == "V_MPEGI/ISO/VVC")
        {
            fileExt = ".vvc";
        }
        else if (codecName == "V_AV1")
        {
            fileExt = ".obu";
        }
        else if (codecName == "V_VP8" || codecName == "V_VP9")
        {
            fileExt = ".ivf";
        }
        else
        {
            fileExt = ".mpv";
        }
    }

    vector<string> fileList = extractFileList(streamName);
    string fileName;
    if (fileList.size() < 3)
        for (int i = 0; i < static_cast<int>(fileList.size()); i++)
        {
            if (i > 0)
                fileName += '+';
            fileName += extractFileName(fileList[i]);
        }
    else
        fileName = extractFileName(fileList[0]) + "+___+" + extractFileName(fileList[fileList.size() - 1]);

    auto itr = params.find("track");
    if (itr != params.end())
    {
        if (params.find("subClip") != params.end())
            fileName += ".subclip_track_";
        else
            fileName += ".track_";
        fileName += itr->second;
    }

    int cnt = ++m_trackNameTmp[fileName];
    if (cnt > 1)
    {
        fileName += "_";
        fileName += int32ToStr(cnt);
    }

    itr = params.find("lang");
    if (itr != params.end())
    {
        fileName += "_";
        fileName += itr->second;
    }

    // A raw elementary stream cannot carry a timestamp, so a track that was offset against
    // the video in its container comes out of a demux with that offset lost, and muxing it
    // back puts it at zero. Record the offset in the file name using the convention eac3to
    // established, e.g. "00011.track_4352 DELAY -20ms.ac3", so the round trip stays in sync
    // and other tools that already speak this convention can read it.
    // Any stale token in the source name is stripped first, or a file that has been through
    // the loop once accumulates one token per generation.
    // The start a track has against the picture in its file is such an offset too, when no timeshift= is given: it
    // was lost on a demux, so the sound of the demuxed files muxed again started with the picture.
    itr = params.find("timeshift");
    if (itr == params.end())
        itr = params.find("srcstartms");
    if (itr != params.end())
    {
        const int64_t delayMs = parseTimeshiftToMs(itr->second);
        // a sound track says its delay also where it is zero: the name then tells "no delay" from "not known" (#259)
        if (delayMs != 0 || (!codecName.empty() && codecName[0] == 'A'))
        {
            fileName = stripDelayToken(fileName);
            fileName += " DELAY ";
            fileName += int32ToStr(static_cast<int32_t>(delayMs));
            fileName += "ms";
        }
    }

    // a PGS track: its forced pictures also go to a file of their own, unless the line says no-forced-track, gives
    // the forced pictures alone (forced-pictures), or is itself the forced track of its language
    if (const auto* pgs = dynamic_cast<PGSStreamReader*>(codecReader); pgs && fileExt == ".sup")
    {
        const auto lang = params.find("lang");
        m_pgsLanguage[streamIndex] = lang != params.end() ? lang->second : std::string();
        const auto flags = params.find("srcflags");
        const bool markedForced = params.count("forced") > 0 ||
                                  (flags != params.end() && flags->second.find("forced=1") != std::string::npos);
        if (markedForced)
            m_forcedPgsLanguages.insert(m_pgsLanguage[streamIndex]);
        else if (!pgs->forcedOnly() && params.count("no-forced-track") == 0)
            m_forcedPgs[streamIndex].fileName = fileName + ".forced" + fileExt;
    }

    // a track of DVD subtitles written as a .sub file: its forced pictures also go to a pair of their own, unless
    // the line says no-forced-track
    if (const auto* copy = dynamic_cast<MatroskaCopyStreamReader*>(codecReader);
        copy && fileExt == ".sub" && params.count("no-forced-track") == 0 && params.count("forced") == 0)
        m_forcedVobSub[streamIndex].fileName = fileName + ".forced" + fileExt;

    auto streamInfo = new StreamInfo(static_cast<unsigned>(DEFAULT_FILE_BLOCK_SIZE));
    streamInfo->m_fileName = fileName + fileExt;
    if (streamInfo->m_fileName.size() > 254)
        LTRACE(LT_ERROR, 2, "Error: File name too long.");
    streamInfo->m_codecReader = codecReader;
    if (strEndWith(fileExt, string(".thd")) && codecName == "A_AC3")
        streamInfo->m_part_of = StreamInfo::Part::LosslessOnly;
    m_streamInfo[streamIndex].push_back(streamInfo);

    // split-ac3-core: write the two halves as files of their own beside the pair, in the same
    // pass. A disc TrueHD track is the only thing that has halves, and only when it really
    // carries a core, which is what the .ac3+thd name means here.
    if (fileExt != ".ac3+thd" && params.find("split-ac3-core") != params.end())
        LTRACE(LT_WARN, 2,
               "Warning: split-ac3-core is ignored for track "
                   << codecReader->getStreamIndex()
                   << ": it splits a TrueHD track that holds its AC-3 core (the disc form on an A_AC3 line, without "
                      "drop-ac3-core or down-to-ac3), and this track is written as "
                   << fileExt << ".");
    if (fileExt == ".ac3+thd" && params.find("split-ac3-core") != params.end())
    {
        for (const auto& [part, ext] : {std::pair{StreamInfo::Part::LosslessOnly, string(".thd")},
                                        std::pair{StreamInfo::Part::CoreOnly, string(".ac3")}})
        {
            auto extra = new StreamInfo(static_cast<unsigned>(DEFAULT_FILE_BLOCK_SIZE));
            extra->m_fileName = fileName + ext;
            if (extra->m_fileName.size() > 254)
                LTRACE(LT_ERROR, 2, "Error: File name too long.");
            extra->m_codecReader = codecReader;
            extra->m_part_of = part;
            m_streamInfo[streamIndex].push_back(extra);
        }
    }
}

void SingleFileMuxer::openDstFile()
{
    const string dir = closeDirPath(toNativeSeparators(m_origFileName));
    // if (!createDir(dstFileName, true))
    //	THROW(ERR_CANT_CREATE_FILE, "Can't create output directory " << dstFileName);
    int systemFlags = 0;
#ifdef _WIN32
    if (m_owner->isAsyncMode())
        systemFlags += FILE_FLAG_NO_BUFFERING;
#endif
    // A demux names each file after its source, so a raw stream demuxed into its own folder (the window proposes the
    // source's folder) was the source itself: opened for writing, it was emptied while it was read, and the demux
    // still said it was complete. Every name is compared with the sources before any file is opened, in the folder the
    // files end up in when they are written into a temporary folder first.
    const string finalDir =
        m_owner->demuxFolder().empty() ? dir : closeDirPath(toNativeSeparators(m_owner->demuxFolder()));
    for (auto& [index, outputs] : m_streamInfo)
        for (StreamInfo* si : outputs)
            if (const std::string src = m_owner->sourceSameAs(finalDir + si->m_fileName); !src.empty())
                THROW(ERR_CANT_CREATE_FILE, "The demux would write " << finalDir + si->m_fileName
                                                                     << ", which is the source " << src
                                                                     << " itself, and destroy it. Choose another "
                                                                        "output folder.")
    for (auto& [index, outputs] : m_streamInfo)
        for (StreamInfo* si : outputs)
        {
            si->m_fileName = dir + si->m_fileName;
            if (!si->m_file.open(si->m_fileName.c_str(), File::ofWrite, systemFlags))
                THROW(ERR_CANT_CREATE_FILE, "Can't create output file " << si->m_fileName)
        }
}

void SingleFileMuxer::writeOutBuffer(StreamInfo* streamInfo)
{
    constexpr int blockSize = DEFAULT_FILE_BLOCK_SIZE;
    if (streamInfo->m_bufLen >= blockSize)
    {
        constexpr int toFileLen = blockSize & 0xffff0000;
        if (m_owner->isAsyncMode())
        {
            const auto newBuf = new uint8_t[blockSize + MAX_AV_PACKET_SIZE + ADD_DATA_SIZE];
            memcpy(newBuf, streamInfo->m_buffer + toFileLen, streamInfo->m_bufLen - toFileLen);
            m_owner->asyncWriteBuffer(this, streamInfo->m_buffer, toFileLen, &streamInfo->m_file);
            streamInfo->m_buffer = newBuf;
        }
        else
        {
            m_owner->syncWriteBuffer(this, streamInfo->m_buffer, toFileLen, &streamInfo->m_file);
            memmove(streamInfo->m_buffer, streamInfo->m_buffer + toFileLen, streamInfo->m_bufLen - toFileLen);
        }
        streamInfo->m_totalWrited += toFileLen;
        streamInfo->m_bufLen -= toFileLen;
    }

    // Wave64 has no 4 GiB ceiling, so a track written in that form is never broken into parts.
    // This is the whole reason the option exists.
    const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(streamInfo->m_codecReader);
    if (lpcmReader && !lpcmReader->isDemuxToW64() && streamInfo->m_totalWrited >= 0xffff0000ul - blockSize)
    // if (lpcmReader && streamInfo->m_totalWrited >= 0x0ffffffful)
    {
        if (m_owner->isAsyncMode())
            m_owner->waitForWriting();
        streamInfo->m_file.close();
        streamInfo->m_file.open(streamInfo->m_fileName.c_str(), File::ofWrite + File::ofAppend);
        streamInfo->m_file.write(streamInfo->m_buffer, streamInfo->m_bufLen);
        streamInfo->m_file.close();
        streamInfo->m_file.open(streamInfo->m_fileName.c_str(), File::ofWrite + File::ofNoTruncate);
        lpcmReader->beforeFileCloseEvent(streamInfo->m_file);
        streamInfo->m_file.close();
        const std::string newName = getNewName(streamInfo->m_fileName, streamInfo->m_part);
        deleteFile(newName);
        // in a run that writes nothing the part is a file that was only counted, and takes its name there
        if (!(dryRun::enabled() && dryRun::rename(streamInfo->m_fileName, newName)) &&
            rename(streamInfo->m_fileName.c_str(), newName.c_str()) != 0)
            THROW(ERR_OUTPUT, "Can't rename file " << streamInfo->m_fileName << " to " << newName)
        streamInfo->m_part++;
        int systemFlags = 0;
        streamInfo->m_bufLen = 0;
#ifdef _WIN32
        if (m_owner->isAsyncMode())
            systemFlags += FILE_FLAG_NO_BUFFERING;
#endif
        if (!streamInfo->m_file.open(streamInfo->m_fileName.c_str(), File::ofWrite + systemFlags))
            THROW(ERR_OUTPUT, "Can't open file " << streamInfo->m_fileName)
        lpcmReader->setFirstFrame(true);
        streamInfo->m_totalWrited = 0;
    }
}

// The forced pictures of a PGS track, to their file: each segment with the header a .sup file gives it, "PG", its
// presentation time and its decoding time
void SingleFileMuxer::writeForcedPgs(const int streamIndex, std::deque<PgsForcedFilter::Segment>& segments)
{
    ForcedPgs& forced = m_forcedPgs[streamIndex];
    if (!forced.open && !forced.failed)
    {
        // a track of the same language that is marked forced is that file already
        if (m_forcedPgsLanguages.count(m_pgsLanguage[streamIndex]) > 0)
            forced.failed = true;
        else
        {
            const std::string dir = closeDirPath(toNativeSeparators(m_origFileName));
            forced.fileName = dir + forced.fileName;
            if (forced.file.open(forced.fileName.c_str(), File::ofWrite))
            {
                forced.open = true;
                LTRACE(LT_INFO, 2,
                       "Track " << streamIndex + 1 << " has forced pictures beside its others. They are also written "
                                << "to " << extractFileName2(forced.fileName)
                                << "; the whole track stays as it is. no-forced-track on the meta line leaves that "
                                   "file out.");
            }
            else
            {
                forced.failed = true;
                LTRACE(LT_WARN, 2,
                       "Warning: the file " << forced.fileName << " of the forced pictures cannot be "
                                            << "made, so they are written with the whole track only.");
            }
        }
    }
    if (forced.open)
        for (const PgsForcedFilter::Segment& seg : segments)
        {
            const auto pts = static_cast<uint32_t>((std::max)(int64_t{0}, seg.pts));
            const auto dts = static_cast<uint32_t>((std::max)(int64_t{0}, seg.dts));
            const uint8_t header[10] = {'P',
                                        'G',
                                        static_cast<uint8_t>(pts >> 24),
                                        static_cast<uint8_t>(pts >> 16),
                                        static_cast<uint8_t>(pts >> 8),
                                        static_cast<uint8_t>(pts),
                                        static_cast<uint8_t>(dts >> 24),
                                        static_cast<uint8_t>(dts >> 16),
                                        static_cast<uint8_t>(dts >> 8),
                                        static_cast<uint8_t>(dts)};
            forced.file.write(header, sizeof(header));
            forced.file.write(seg.data.data(), static_cast<uint32_t>(seg.data.size()));
        }
    segments.clear();
}

// A unit of a DVD subtitle track: a forced one goes to the pair of the forced pictures as well, once the track has
// shown both kinds
void SingleFileMuxer::writeForcedVobSub(const int streamIndex, const AVPacket& avPacket)
{
    ForcedVobSub& forced = m_forcedVobSub[streamIndex];
    auto* const source = dynamic_cast<MatroskaCopyStreamReader*>(avPacket.codec);
    if (forced.failed || !source || !source->vobSubFileOutput())
        return;
    std::vector<VobSubForcedTrack::Unit> units;
    forced.track.push(
        {avPacket.pts, avPacket.duration, std::vector<uint8_t>(avPacket.data, avPacket.data + avPacket.size), {}},
        units);
    if (units.empty())
        return;
    if (!forced.open)
    {
        const std::string dir = closeDirPath(toNativeSeparators(m_origFileName));
        forced.fileName = dir + forced.fileName;
        auto packer = std::make_unique<MatroskaCopyStreamReader>("S_VOBSUB", source->codecPrivate(),
                                                                 std::vector<std::vector<uint8_t>>(), 0);
        packer->setVobSubFileOutput(source->vobSubLang());
        forced.packer = std::move(packer);
        if (forced.file.open(forced.fileName.c_str(), File::ofWrite))
        {
            forced.open = true;
            LTRACE(LT_INFO, 2,
                   "Track " << streamIndex + 1 << " has forced pictures beside its others. They are also written "
                            << "to " << extractFileName2(forced.fileName)
                            << " and its .idx file; the whole track stays as it is. no-forced-track on the meta line "
                               "leaves them out.");
        }
        else
        {
            forced.failed = true;
            LTRACE(LT_WARN, 2,
                   "Warning: the file " << forced.fileName << " of the forced pictures cannot be "
                                        << "made, so they are written with the whole track only.");
            return;
        }
    }
    auto* const packer = static_cast<MatroskaCopyStreamReader*>(forced.packer.get());
    for (const VobSubForcedTrack::Unit& unit : units)
    {
        const uint8_t* packs = nullptr;
        const int size = packer->packVobSub(unit.pts, unit.data.data(), unit.data.size(), &packs);
        if (size > 0 && forced.file.write(packs, static_cast<uint32_t>(size)) != size)
        {
            forced.failed = true;
            LTRACE(LT_WARN, 2, "Warning: the file " << forced.fileName << " of the forced pictures cannot be written.");
            return;
        }
    }
}

bool SingleFileMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
        return true;
    if (m_forcedVobSub.count(avPacket.stream_index) > 0)
        writeForcedVobSub(avPacket.stream_index, avPacket);
    if (const auto forced = m_forcedPgs.find(avPacket.stream_index); forced != m_forcedPgs.end())
    {
        std::deque<PgsForcedFilter::Segment> segments;
        forced->second.track.push(avPacket.data, static_cast<size_t>(avPacket.size), internalClockToPts(avPacket.pts),
                                  internalClockToPts(avPacket.dts), segments);
        if (!segments.empty())
            writeForcedPgs(avPacket.stream_index, segments);
    }
    // The disc form of a TrueHD track interleaves an AC-3 core with the lossless frames, and a
    // decoder handed the pair reports inconsistent timestamps or refuses the file outright. Each
    // output of this track says which of the two it wants, so the same packet can go to the file
    // holding the pair and to the file holding its own half, in one pass over the source.
    const bool isCore = (avPacket.flags & AVPacket::IS_CORE_PACKET) || isTrueHDCorePacket(avPacket.data, avPacket.size);
    bool rez = true;
    for (StreamInfo* streamInfo : m_streamInfo[avPacket.stream_index])
        if (streamInfo->takes(isCore))
            rez = muxPacketTo(streamInfo, avPacket) && rez;
    return rez;
}

bool SingleFileMuxer::muxPacketTo(StreamInfo* streamInfo, AVPacket& avPacket)
{
    if (avPacket.dts != streamInfo->m_dts || avPacket.pts != streamInfo->m_pts ||
        m_lastIndex != avPacket.stream_index || avPacket.flags & AVPacket::FORCE_NEW_FRAME)
    {
        constexpr uint32_t blockSize = DEFAULT_FILE_BLOCK_SIZE;
        streamInfo->m_bufLen += avPacket.codec->writeAdditionData(
            streamInfo->m_buffer + streamInfo->m_bufLen,
            streamInfo->m_buffer + blockSize + MAX_AV_PACKET_SIZE + ADD_DATA_SIZE, avPacket, nullptr);
        writeOutBuffer(streamInfo);
    }
    m_lastIndex = avPacket.stream_index;
    streamInfo->m_dts = avPacket.dts;
    streamInfo->m_pts = avPacket.pts;

    // What goes in the file is the codec's ON DISK form, which for everything but AV1 is the packet
    // exactly as it stands. The size is taken AFTER any conversion, so the buffer test below is
    // made against what is really about to be written.
    const uint8_t* outData = avPacket.data;
    auto* const vobSub = dynamic_cast<MatroskaCopyStreamReader*>(streamInfo->m_codecReader);
    const int outSize =
        vobSub && vobSub->vobSubFileOutput()
            ? vobSub->packVobSub(avPacket.pts, avPacket.data, static_cast<size_t>(avPacket.size), &outData)
            : avPacket.codec->prepareDemuxPacket(avPacket.data, avPacket.size, &outData);

    // If the frame would overflow the output buffer, force a flush first.
    // The buffer is blockSize + MAX_AV_PACKET_SIZE + ADD_DATA_SIZE; frames
    // larger than MAX_AV_PACKET_SIZE (e.g. multichannel FLAC) need this.
    constexpr int bufCapacity = DEFAULT_FILE_BLOCK_SIZE + MAX_AV_PACKET_SIZE + ADD_DATA_SIZE;
    if (streamInfo->m_bufLen + outSize > bufCapacity && streamInfo->m_bufLen > 0)
    {
        if (m_owner->isAsyncMode())
        {
            // Only whole 64 KiB units go to the file here, as everywhere else on this path: the file
            // is written unbuffered, and Windows refuses a write of any other size (error 87). This
            // flush sent the whole buffer, so a FLAC frame larger than the room left (7.1, 24 bit,
            // 96 kHz: 72 to 193 KB each) stopped the demux after 2 MiB with a bare "T" (W1-4). The
            // rest stays in front of the frame, as doFlush keeps it; the new buffer is made large
            // enough for the two together.
            const int toFileLen = streamInfo->m_bufLen & 0x7fff0000;
            const int rest = streamInfo->m_bufLen - toFileLen;
            // The new buffer has the usual size: a frame that does not fit beside the rest goes in by
            // parts below.
            const auto newBuf = new uint8_t[bufCapacity];
            memcpy(newBuf, streamInfo->m_buffer + toFileLen, rest);
            if (toFileLen > 0)
                m_owner->asyncWriteBuffer(this, streamInfo->m_buffer, toFileLen, &streamInfo->m_file);
            else
                delete[] streamInfo->m_buffer;
            streamInfo->m_buffer = newBuf;
            streamInfo->m_totalWrited += toFileLen;
            streamInfo->m_bufLen = rest;
        }
        else
        {
            m_owner->syncWriteBuffer(this, streamInfo->m_buffer, streamInfo->m_bufLen, &streamInfo->m_file);
            streamInfo->m_totalWrited += streamInfo->m_bufLen;
            streamInfo->m_bufLen = 0;
        }
    }

    // A frame larger than the room left goes in by parts, and the whole blocks each part completes
    // are written before the next part is copied. The buffer holds 2 MiB + 34 KiB, and a larger
    // picture was copied into it whole: a demux of lossless 4K video (pictures of 13 to 40 MiB) wrote
    // past the buffer and ended in an access violation, with 12 MiB of a 40 MiB picture written or nothing at all.
    // Every frame that fits is copied in one part, as before.
    int copied = 0;
    do
    {
        const int part = (std::min)(outSize - copied, bufCapacity - streamInfo->m_bufLen);
        if (part > 0)
            memcpy(streamInfo->m_buffer + streamInfo->m_bufLen, outData + copied, part);
        streamInfo->m_bufLen += part;
        copied += part;
        writeOutBuffer(streamInfo);
    } while (copied < outSize);
    return true;
}

bool SingleFileMuxer::doFlush()
{
    for (const auto& [fst, outputs] : m_streamInfo)
        for (StreamInfo* streamInfo : outputs)
        {
            const int lastBlockSize = streamInfo->m_bufLen & 0xffff;  // last 64K of data
            const int roundBufLen = streamInfo->m_bufLen & 0x7fff0000;
            if (m_owner->isAsyncMode())
            {
                if (lastBlockSize > 0)
                {
                    const auto newBuff = new uint8_t[lastBlockSize];
                    memcpy(newBuff, streamInfo->m_buffer + roundBufLen, lastBlockSize);
                    m_owner->asyncWriteBuffer(this, streamInfo->m_buffer, roundBufLen, &streamInfo->m_file);
                    streamInfo->m_buffer = newBuff;
                }
                else
                {
                    m_owner->asyncWriteBuffer(this, streamInfo->m_buffer, roundBufLen, &streamInfo->m_file);
                    streamInfo->m_buffer = nullptr;
                }
            }
            else
            {
                m_owner->syncWriteBuffer(this, streamInfo->m_buffer, roundBufLen, &streamInfo->m_file);
                memmove(streamInfo->m_buffer, streamInfo->m_buffer + roundBufLen, lastBlockSize);
            }
            streamInfo->m_bufLen = lastBlockSize;
        }
    return true;
}

static void reportUnfinished(const std::string& fileName)
{
    LTRACE(LT_ERROR, 2, "Error: the end of " << fileName << " could not be written.");
}

// Every track is finished, also after one of them failed: the first failure used to end the loop, and the end of
// every track after it was never written, while the run went on as if all were done. Each file that cannot be
// finished is named, and the result says whether all were.
bool SingleFileMuxer::close()
{
    bool allFinished = true;
    for (auto& [streamIndex, forced] : m_forcedPgs)
    {
        std::deque<PgsForcedFilter::Segment> segments;
        forced.track.finish(segments);
        if (!segments.empty())
            writeForcedPgs(streamIndex, segments);
        if (forced.open && !forced.file.close())
        {
            LTRACE(LT_ERROR, 2, "Error: the end of " << forced.fileName << " could not be written.");
            allFinished = false;
        }
        forced.open = false;
    }
    // the pairs of forced DVD subtitle pictures: the .sub file is closed, and its .idx file written beside it
    for (auto& [streamIndex, forced] : m_forcedVobSub)
    {
        if (!forced.open)
            continue;
        forced.open = false;
        const auto* packer = static_cast<const MatroskaCopyStreamReader*>(forced.packer.get());
        const std::string text = packer->vobSubIndex();
        const std::string name = forced.fileName.substr(0, forced.fileName.size() - 4) + ".idx";
        File idx;
        if (!forced.file.close() || forced.failed || !idx.open(name.c_str(), File::ofWrite) ||
            idx.write(text.data(), static_cast<uint32_t>(text.size())) != static_cast<int>(text.size()) || !idx.close())
        {
            LTRACE(LT_ERROR, 2, "Error: the end of " << forced.fileName << " could not be written.");
            allFinished = false;
        }
    }
    for (const auto& [fst, outputs] : m_streamInfo)
        for (StreamInfo* streamInfo : outputs)
        {
            const auto failed = [&]
            {
                reportUnfinished(streamInfo->m_fileName);
                allFinished = false;
            };
            if (!streamInfo->m_file.close())
            {
                failed();
                continue;
            }
            // The end of a file is written also when nothing is left over to write. It used to be skipped then, and
            // that is where a WAV or W64 file gets its sizes: a demux whose length came out a whole number of 64 KiB
            // blocks kept the placeholders, 0xFFFFFFFF in a WAV. The last part of a split track kept the name it was
            // written under, too.
            if (!streamInfo->m_file.open(streamInfo->m_fileName.c_str(), File::ofWrite + File::ofAppend) ||
                (streamInfo->m_bufLen > 0 &&
                 streamInfo->m_file.write(streamInfo->m_buffer, streamInfo->m_bufLen) != streamInfo->m_bufLen) ||
                (streamInfo->m_codecReader && !streamInfo->m_codecReader->beforeFileCloseEvent(streamInfo->m_file)))
            {
                streamInfo->m_file.close();
                failed();
                continue;
            }
            if (!streamInfo->m_file.close())
            {
                failed();
                continue;
            }
            // DVD subtitles: the .idx file beside the .sub file
            if (const auto* copy = dynamic_cast<const MatroskaCopyStreamReader*>(streamInfo->m_codecReader);
                copy && copy->vobSubFileOutput())
            {
                const std::string text = copy->vobSubIndex();
                const std::string& sub = streamInfo->m_fileName;
                const std::string name = sub.substr(0, sub.size() - 4) + ".idx";
                File idx;
                if (!idx.open(name.c_str(), File::ofWrite) ||
                    idx.write(text.data(), static_cast<uint32_t>(text.size())) != static_cast<int>(text.size()) ||
                    !idx.close())
                {
                    reportUnfinished(name);
                    allFinished = false;
                }
            }

            // ** A DEMUXED SOUND FILE SAYS THE GAP IT STARTS AFTER. ** The sound of a part a playlist cuts starts after a
            // gap where its first PES are left out (TSDemuxer::demuxOnePacket), which a mux puts in its times; a raw
            // file has none, and the demuxed files muxed again put that sound early by the gap. Known only once the
            // track is read, so it is added to the name here, to the delay the name says already.
            if (const auto* sound = dynamic_cast<const SimplePacketizerReader*>(streamInfo->m_codecReader);
                sound && sound->startGapNs() > 0 && streamInfo->m_part <= 1)
            {
                // only the name of the file changes, not its folder (whose name can hold a dot or a DELAY too)
                const std::string& old = streamInfo->m_fileName;
                const size_t slash = old.find_last_of("/\\");
                const std::string dir = slash == std::string::npos ? std::string() : old.substr(0, slash + 1);
                const std::string name = slash == std::string::npos ? old : old.substr(slash + 1);
                const size_t dot = name.find_last_of('.');
                const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
                const int64_t ms = delayFromFileName(name) + (sound->startGapNs() + 500000) / 1000000;
                const std::string newName = dir + stripDelayToken(base) + " DELAY " + int64ToStr(ms) + "ms" + ext;
                // a name that is too long, a source, or a rename that fails: the file keeps its name, and it is said;
                // the other files are finished all the same
                std::string why;
                if (newName.size() - dir.size() > 254)
                    why = "the name would be too long";
                else if (m_owner)
                {
                    // where the files end up (a demux written into a temporary folder first: openDstFile)
                    const std::string finalDir = m_owner->demuxFolder().empty()
                                                     ? dir
                                                     : closeDirPath(toNativeSeparators(m_owner->demuxFolder()));
                    if (!m_owner->sourceSameAs(finalDir + newName.substr(dir.size())).empty())
                        why = "a source has that name";
                }
                // the same name (a gap that rounds to 0 ms, a name that says it already): nothing to rename, and the
                // file is not deleted under its own name
#ifdef _WIN32
                const bool sameName = strToLowerCase(newName) == strToLowerCase(old);
#else
                const bool sameName = newName == old;
#endif
                if (why.empty() && !sameName)
                {
                    deleteFile(newName);
                    if (!(dryRun::enabled() && dryRun::rename(old, newName)) && rename(old.c_str(), newName.c_str()) != 0)
                        why = "it could not be renamed";
                }
                if (why.empty())
                    streamInfo->m_fileName = newName;
                else
                    LTRACE(LT_WARN, 2,
                           "Warning: " << old << " starts " << ms << " ms after the start of the film, which its name "
                                       << "does not say (" << why << "). Give the track timeshift=" << ms
                                       << "ms when it is muxed again.");
            }
            if (streamInfo->m_part > 1)
            {
                std::string newName = getNewName(streamInfo->m_fileName, streamInfo->m_part);
                deleteFile(newName);
                if (!(dryRun::enabled() && dryRun::rename(streamInfo->m_fileName, newName)) &&
                    rename(streamInfo->m_fileName.c_str(), newName.c_str()) != 0)
                    THROW(ERR_OUTPUT, "Can't rename file " << streamInfo->m_fileName << " to " << newName)
            }
        }
    return allFinished;
}

void SingleFileMuxer::parseMuxOpt(const std::string& opts)
{
    // Split outside quotes, as the line is split everywhere else: a word of a quoted value is no option.
    const vector<string> params = splitQuotedStr(opts.c_str(), ' ');
    for (auto& i : params)
    {
        vector<string> paramPair = splitOption(trimStr(i));
        if (paramPair.empty())
            continue;
        if (paramPair[0] == "--split-duration")
        {
            LTRACE(LT_WARN, 2,
                   "Warning! Splitting does not implemented for demux mode. Parameter " << paramPair[0] << " ignored.");
        }
        if (paramPair[0] == "--split-size")
        {
            LTRACE(LT_WARN, 2,
                   "Warning! Splitting does not implemented for demux mode. Parameter " << paramPair[0] << " ignored.");
        }
        if (paramPair[0] == "--split-chapters")
        {
            LTRACE(LT_WARN, 2,
                   "Warning! Splitting does not implemented for demux mode. Parameter " << paramPair[0] << " ignored.");
        }
    }
}
