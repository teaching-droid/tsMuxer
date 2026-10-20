#include "tsDemuxer.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "abstractStreamReader.h"
#include "bitmapSubtitles.h"
#include "bufferedFileReader.h"
#include "matroskaParser.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

bool isM2TSExt(const std::string& streamName)
{
    const string sName = strToLowerCase(unquoteStr(streamName));
    return strEndWith(sName, ".m2ts") || strEndWith(sName, ".mts") || strEndWith(sName, ".ssif");
}

TSDemuxer::TSDemuxer(const BufferedReaderManager& readManager, const char* streamName)
    : m_readManager(readManager),
      m_curPos(nullptr),
      m_pmtPid(0),
      m_codecReady(false),
      m_readCnt(0),
      m_dataProcessed(0),
      m_notificated(false),
      m_tmpBuffer{}
{
    m_firstPCRTime = -1;
    m_bufferedReader = m_readManager.getReader(streamName);
    m_readerID = m_bufferedReader->createReader(TS_FRAME_SIZE);
    if (m_bufferedReader == nullptr)
        THROW(ERR_COMMON,
              "TS demuxer can't accept reader because this reader does not support BufferedReader interface")
    m_scale = 1;
    m_nptPos = 0;
    m_tmpBufferLen = 0;
    m_m2tsMode = false;
    m_lastReadRez = 0;
    m_m2tsHdrDiscarded = false;
    m_firstPTS = -1;
    m_firstSeenPTS = -1;
    m_lastPTS = -1;
    m_firstVideoPTS = -1;
    m_firstSeenVideoPTS = -1;
    m_lastVideoPTS = -1;
    m_lastVideoDTS = -1;
    m_videoDtsGap = -1;
    m_firstCall = true;
    m_prevFileLen = 0;
    m_curFileNum = 0;
    m_lastPCRVal = -1;
    m_nonMVCVideoFound = false;
    m_firstDemuxCall = true;
    memset(m_acceptedPidCache, 0, sizeof(m_acceptedPidCache));
}

bool TSDemuxer::mvcContinueExpected() const { return !m_nonMVCVideoFound && strEndWith(m_streamNameLow, "ssif"); }

void TSDemuxer::getTrackList(std::map<int32_t, TrackInfo>& trackList)
{
    uint8_t pmtBuffer[4096]{0};
    int pmtBufferLen = 0;
    uint32_t tmpBufferLen = 0;
    uint32_t readedBytes;
    uint32_t totalReadedBytes = 0;
    m_lastReadRez = 0;
    bool firstPAT = true;
    std::map<int, int> nonProcPMTPid;

    bool m2tsHdrDiscarded = false;
    int lastReadRez = 0;
    uint8_t* curPos = nullptr;
    while (totalReadedBytes < DETECT_STREAM_BUFFER_SIZE && lastReadRez != BufferedReader::DATA_EOF)
    {
        lastReadRez = 0;
        uint8_t* data = m_bufferedReader->readBlock(m_readerID, readedBytes, lastReadRez);
        totalReadedBytes += readedBytes;
        data += TS_FRAME_SIZE;
        if (tmpBufferLen > 0)
        {
            memcpy(data - tmpBufferLen, m_tmpBuffer, tmpBufferLen);
            data -= tmpBufferLen;
            readedBytes += tmpBufferLen;
            tmpBufferLen = 0;
        }
        uint8_t* lastFrameAddr = data + readedBytes - TS_FRAME_SIZE;
        TS_program_association_section pat;

        if (m_firstCall && !m_m2tsMode)
        {
            m_m2tsMode = checkForRealM2ts(data, lastFrameAddr);
            m_firstCall = false;
        }

        for (curPos = data; curPos <= lastFrameAddr; curPos += TS_FRAME_SIZE)
        {
            if (!m2tsHdrDiscarded && m_m2tsMode)
                curPos += 4;
            while (*curPos != 0x47 && curPos <= lastFrameAddr) curPos++;
            if (curPos > lastFrameAddr)
            {
                m2tsHdrDiscarded = true;
                break;
            }
            m2tsHdrDiscarded = false;
            auto tsPacket = reinterpret_cast<TSPacket*>(curPos);
            int pid = tsPacket->getPID();

            if (TS_FRAME_SIZE < tsPacket->getHeaderSize())
            {
                THROW(ERR_COMMON, "Invalid tsPacket->getHeaderSize")
                break;
            }
            if (pid == 0)
            {  // PAT
                pat.deserialize(curPos + tsPacket->getHeaderSize(), TS_FRAME_SIZE - tsPacket->getHeaderSize());
                if (firstPAT)
                {
                    firstPAT = false;
                    nonProcPMTPid = pat.pmtPids;
                }
            }
            else if (pat.pmtPids.find(pid) != pat.pmtPids.end())
            {  // PMT
                if (tsPacket->payloadStart || pmtBufferLen > 0)
                {
                    // A section that would outgrow the buffer is damaged, as on a broken off-air recording.
                    // Leaving the packet loop here used to strand the rest of the 2 MiB block, which the
                    // carry-over below then copied into a buffer of one packet. Drop the section and collect
                    // again from the next one that starts.
                    if (pmtBufferLen + TS_FRAME_SIZE - tsPacket->getHeaderSize() > static_cast<int>(sizeof(pmtBuffer)))
                    {
                        pmtBufferLen = 0;
                        if (!tsPacket->payloadStart)
                            continue;
                    }
                    memcpy(pmtBuffer + pmtBufferLen, curPos + tsPacket->getHeaderSize(),
                           TS_FRAME_SIZE - tsPacket->getHeaderSize());
                    pmtBufferLen += TS_FRAME_SIZE - tsPacket->getHeaderSize();
                    if (TS_program_map_section::isFullBuff(pmtBuffer, pmtBufferLen))
                    {
                        m_pmt.deserialize(pmtBuffer, pmtBufferLen);
                        if (m_pmt.video_type != static_cast<int>(StreamType::VIDEO_MVC))
                            m_nonMVCVideoFound = true;
                        m_pmtVersionPid = pid;
                        m_pmtVersion = -2;
                        pmtBufferLen = 0;
                        for (const auto& [fst, snd] : m_pmt.pidList)
                            trackList.insert(std::make_pair(
                                snd.m_pid, TrackInfo(isDvbSubtitlePid(snd.m_pid) ? TRACKTYPE_PGS
                                                                                 : static_cast<int>(snd.m_streamType),
                                                     snd.m_lang, 0, snd.m_dvbDescriptor)));
                        nonProcPMTPid.erase(pid);
                        if (nonProcPMTPid.empty() && !mvcContinueExpected())
                        {  // all pmt pids processed
                            auto br = dynamic_cast<BufferedFileReader*>(m_bufferedReader);
                            if (br)
                                br->incSeek(m_readerID, -static_cast<int64_t>(totalReadedBytes));
                            else
                                THROW(ERR_COMMON, "Function TSDemuxer::getTrackList required bufferedReader!")
                            return;
                        }
                    }
                }
            }
        }
        if (curPos < data + readedBytes)
        {
            // What is carried over is the part of a packet the block ended in, so it is shorter than a
            // packet. Anything longer would not fit in m_tmpBuffer, nor in the room reserved in front of
            // the next block.
            tmpBufferLen = static_cast<uint32_t>(data + readedBytes - curPos);
            if (tmpBufferLen > TS_FRAME_SIZE)
                THROW(ERR_COMMON, "TS track scan: " << tmpBufferLen << " bytes left over at the end of a block")
            memmove(m_tmpBuffer, curPos, tmpBufferLen);
        }
    }

    auto br = dynamic_cast<BufferedFileReader*>(m_bufferedReader);
    if (br)
        br->incSeek(m_readerID, -static_cast<int64_t>(totalReadedBytes));
    else
        THROW(ERR_COMMON, "Function TSDemuxer::getTrackList required bufferedReader!")
}

bool TSDemuxer::getTrackStart(const int32_t pid, const bool fromFirstPes, int64_t& ticks) const
{
    const auto first = m_firstPtsTime.find(pid);
    const int64_t reference = fromFirstPes ? m_firstSeenVideoPTS : m_firstVideoPTS;
    if (first == m_firstPtsTime.end() || reference == -1)
        return false;
    // A PTS has 33 bits and wraps after about 26.5 hours: a stream that starts just after the wrap and a video that
    // starts just before it are a moment apart, not a day
    static constexpr int64_t CLOCK_WRAP = 1LL << 33;
    ticks = (first->second - reference) % CLOCK_WRAP;
    if (ticks >= CLOCK_WRAP / 2)
        ticks -= CLOCK_WRAP;
    else if (ticks < -CLOCK_WRAP / 2)
        ticks += CLOCK_WRAP;
    return true;
}

bool TSDemuxer::isJoinOfFiles() const { return m_joinFiles.size() > 1 && m_mplsInfo.empty() && m_clipParts.empty(); }

// The decoding time of the first picture of a video stream of a file, from the PES headers; -1 where it is not found.
int64_t TSDemuxer::fileVideoStart(const std::string& fileName, const int videoPid) const
{
    static constexpr int64_t READ_LIMIT = 256LL * 1024 * 1024;
    TSDemuxer probe(m_readManager, "");
    probe.openFile(fileName);
    std::map<int32_t, TrackInfo> tracks;
    probe.getTrackList(tracks);
    if (tracks.find(videoPid) == tracks.end() || probe.getFileBlockSize() == 0)
        return -1;
    DemuxedData demuxedData;
    const PIDSet none;
    int64_t discardedSize = 0;
    for (int64_t read = 0; read < READ_LIMIT; read += probe.getFileBlockSize())
    {
        if (const auto first = probe.m_fileFirstDts.find(videoPid); first != probe.m_fileFirstDts.end())
            return first->second;
        if (probe.simpleDemuxBlock(demuxedData, none, discardedSize) == BufferedReader::DATA_EOF)
            break;
    }
    const auto first = probe.m_fileFirstDts.find(videoPid);
    return first != probe.m_fileFirstDts.end() ? first->second : -1;
}

// A file of a join ends: where the last PES of the sound of each track handed on so far is on the timeline of the join,
// and for the first file where the track begins; and where the video of the next file begins.
//
// The video readers count the pictures they are given, in decode order, so the pictures of the next file follow on
// from the decoding times of this one: the timeline is laid by the decoding times of the video, from the first picture
// shown of the first file. Where the decoding times of the next file run on from those of this one, the two files are
// on one clock, as where a recording was split into files, also in the middle of a group of pictures, where pictures
// shown before the last ones of a file are decoded in the next file: the times of both are then taken as they are.
void TSDemuxer::endJoinFile()
{
    const bool known = m_joinFileDts.first != -1 && (m_curFileNum > 0 || m_firstVideoPTS != -1);
    if (known && m_curFileNum == 0)
        m_joinBase = m_joinFileDts.first - m_firstVideoPTS;
    for (auto& [pid, sound] : m_joinSound)
    {
        if (sound.lastPts != -1 && known && m_joinBaseKnown)
        {
            const int64_t onTimeline = m_joinBase - m_joinFileDts.first;
            if (sound.origin == -1)
                sound.origin = sound.firstPts + onTimeline;
            sound.lastPes = sound.lastPts + onTimeline + sound.shift;
        }
        else
            sound.lastPes = -1;
        sound.shift = 0;
        sound.firstPts = sound.lastPts = sound.prevPts = -1;
    }
    memset(m_joinDrop, 0, sizeof(m_joinDrop));
    // the sound of the next file may come before its first picture in the file: that picture is looked up first
    m_joinVideoStart = -1;
    if (known && m_joinBaseKnown && !m_joinSound.empty() && m_curFileNum + 1 < m_joinFiles.size())
        m_joinVideoStart = fileVideoStart(unquoteStr(m_joinFiles[m_curFileNum + 1]), m_firstVideoPid);
    const int64_t step = (std::max)(m_videoDtsGap, int64_t{0});
    const int64_t runOn = m_joinVideoStart - m_joinFileDts.second;
    if (!known || !m_joinBaseKnown)
        m_joinBaseKnown = false;
    else if (m_joinVideoStart != -1 && step > 0 && runOn > step / 2 && runOn < step + step / 2)
        m_joinBase += m_joinVideoStart - m_joinFileDts.first;  // one clock
    else
        m_joinBase += m_joinFileDts.second - m_joinFileDts.first + step;
    m_joinFileDts = {-1, -1};
}

// Whether a PES of the sound of a join of files is kept, decided where it starts.
//
// The audio readers count the frames they are given: the sound of a later file followed on from the end of the sound
// of the file before, not from where its own file starts it against its picture. A file whose sound starts later than
// its picture, or ends before it, put the sound of every file after it early. The sound of each later file now starts
// where its picture puts it, within half a frame (or half a PES of frames, or a millisecond): where the sound before
// ends earlier, the track's times have a gap there (takeTimeGaps), and where it ends later, the PES of the file that
// it overlaps are left out, as a reader that counts frames cannot go back. A file whose sound follows on from the
// sound before, as it does where a recording was split into files, is muxed as before.
bool TSDemuxer::keepJoinSound(const int pid, const StreamType type, const int64_t pts)
{
    JoinSound& sound = m_joinSound[pid];
    if (sound.firstPts == -1)
    {
        if (m_curFileNum > 0 && sound.lastPes != -1 && m_joinVideoStart != -1 && m_joinBaseKnown)
        {
            const int64_t due = pts - m_joinVideoStart + m_joinBase;
            // A PES at the time of the last one of the file before carries the rest of the same frame (the
            // extension of a DTS-HD frame, the TrueHD unit after an AC-3 frame), as where a recording was split
            // into files: kept with it, and the time of the next one decides.
            if (due == sound.lastPes && sound.pesBytes >= 0)
                return true;
            // Where the sound before ends: the last PES of the file before may hold fewer frames than the others,
            // which for a sound of a constant bit rate its size tells, against the PES that gave the step. A PES
            // that a file split in the middle of a recording cut in two goes on at the start of this file, and that
            // part counts with it.
            if (sound.pesBytes >= 0)
            {
                const int64_t lastLen = sound.sizedFrames && sound.stepBytes > 0
                                            ? sound.step * sound.pesBytes / sound.stepBytes
                                            : sound.step;
                sound.end = sound.lastPes + (std::min)(lastLen, sound.step);
                sound.pesBytes = -1;  // counted; the PES left out of this file count as nothing
            }
            // half a frame, but at least a millisecond: the end of TrueHD, whose units of under a millisecond come
            // in PES of their own and with the AC-3 frames of its core, is not known closer than that
            const int64_t within = (std::max)(sound.step / 2, int64_t{90});
            if (due + within < sound.end)
                return false;
            // within that the sound goes on where the sound before ends: the times of this file's sound are that
            // much off, which the next joint takes into account, so that it cannot add up over many files
            const int64_t gap = due - sound.end;
            sound.shift = -gap;
            if (sound.step > 0 && gap > within)
            {
                sound.shift = 0;
                m_joinGaps[pid].emplace_back((sound.end - sound.origin) * 100000 / 9, gap * 100000 / 9);
                LTRACE(LT_INFO, 2,
                       "The sound of track " << pid << " in file " << m_curFileNum + 1 << " of the join starts "
                                             << (gap + 45) / 90 << " ms after the sound before it ends: it starts "
                                             << "after a gap, with its picture.");
            }
        }
        sound.firstPts = pts;
    }
    // the PES of one time are one frame, as a DTS-HD frame and its extension
    if (pts != sound.prevPts)
    {
        if (sound.prevPts != -1 && pts > sound.prevPts && (sound.step <= 0 || pts - sound.prevPts < sound.step))
        {
            sound.step = pts - sound.prevPts;
            sound.stepBytes = sound.pesBytes;
        }
        sound.pesBytes = 0;
    }
    sound.prevPts = pts;
    sound.sizedFrames = type == StreamType::AUDIO_AC3 || type == StreamType::AUDIO_DTS ||
                        type == StreamType::AUDIO_MPEG1 || type == StreamType::AUDIO_MPEG2 ||
                        type == StreamType::AUDIO_LPCM;
    m_joinSoundPid[pid] = 1;
    if (sound.lastPts == -1 || pts > sound.lastPts)
        sound.lastPts = pts;
    return true;
}

std::vector<std::pair<int64_t, int64_t>> TSDemuxer::takeTimeGaps(const int pid)
{
    // the core of a stream is a track of its own, on the same PID
    const auto gaps = m_joinGaps.find(pid >= 0x10000 ? pid >> 16 : pid);
    if (gaps == m_joinGaps.end())
        return {};
    size_t& taken = m_joinGapsTaken[pid];
    std::vector<std::pair<int64_t, int64_t>> result(gaps->second.begin() + static_cast<std::ptrdiff_t>(taken),
                                                    gaps->second.end());
    taken = gaps->second.size();
    return result;
}

bool TSDemuxer::hasVideoStream() const
{
    return std::any_of(m_pmt.pidList.begin(), m_pmt.pidList.end(),
                       [](const auto& stream) { return isVideoPID(stream.second.m_streamType); });
}

bool TSDemuxer::isVideoPID(const StreamType streamType)
{
    switch (streamType)
    {
    case StreamType::VIDEO_MPEG1:
    case StreamType::VIDEO_MPEG2:
    case StreamType::VIDEO_MPEG4:
    case StreamType::VIDEO_H264:
    case StreamType::VIDEO_MVC:
    case StreamType::VIDEO_VC1:
    case StreamType::VIDEO_H265:
    case StreamType::VIDEO_H266:
        return true;
    default:
        return false;
    }
}

// static uint64_t prevDts = 0;
// static int ggCnt = 0;

bool TSDemuxer::checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const
{
    for (const uint8_t* cur = buffer; cur < end; cur += 192)
        if (cur[4] != 0x47)
            return false;
    for (const uint8_t* cur = buffer; cur < end; cur += 188)
        if (*cur != 0x47)
        {
            LTRACE(LT_WARN, 2, "Warning! The file " << m_streamName << " has a M2TS format.");
            return true;
        }
    return false;
}

// The PAT and PMT of one block. For the first clip the PMT becomes the list of streams, as it always has. For a
// later clip of a join, a stream it declares is added when this demuxer delivers it to a track and does not know it
// yet. A playlist whose first clip is a short logo without audio declares the film's audio only from the next clip
// on, and a stream missing from the list keeps its PES headers in the data: the audio reader took each of them
// for a broken frame. A later clip only ever adds: a PMT that does not parse adds nothing, and a packet that stops
// the scan of the first clip ends the scan of a later one quietly, so no mux that ran before can stop here.
void TSDemuxer::readProgramTables(uint8_t* data, const uint8_t* lastFrameAddr, const bool laterClip)
{
    uint8_t pmtBuffer[4096]{0};
    int pmtBufferLen = 0;
    bool hdrDiscarded = m_m2tsHdrDiscarded;
    TS_program_association_section pat;
    for (uint8_t* cur = data; cur <= lastFrameAddr; cur += TS_FRAME_SIZE)
    {
        if (!hdrDiscarded && m_m2tsMode)
            cur += 4;
        while (cur <= lastFrameAddr && *cur != 0x47) cur++;
        if (cur > lastFrameAddr)
            break;
        hdrDiscarded = false;

        const auto tsPacket = reinterpret_cast<TSPacket*>(cur);
        const int pid = tsPacket->getPID();
        if (TS_FRAME_SIZE < tsPacket->getHeaderSize())
        {
            if (laterClip)
                break;
            THROW(ERR_COMMON, "Invalid tsPacket->getHeaderSize")
        }
        if (pid == 0)
        {  // PAT
            pat.deserialize(cur + tsPacket->getHeaderSize(), TS_FRAME_SIZE - tsPacket->getHeaderSize());
        }
        else if (pat.pmtPids.find(pid) != pat.pmtPids.end())
        {  // PMT
            if (tsPacket->payloadStart || pmtBufferLen > 0)
            {
                if (pmtBufferLen + TS_FRAME_SIZE - tsPacket->getHeaderSize() > 4096)
                    break;
                memcpy(pmtBuffer + pmtBufferLen, cur + tsPacket->getHeaderSize(),
                       TS_FRAME_SIZE - tsPacket->getHeaderSize());
                pmtBufferLen += TS_FRAME_SIZE - tsPacket->getHeaderSize();
                if (TS_program_map_section::isFullBuff(pmtBuffer, pmtBufferLen))
                {
                    if (!laterClip)
                    {
                        m_pmt.deserialize(pmtBuffer, pmtBufferLen);
                        if (m_pmt.video_type != static_cast<int>(StreamType::VIDEO_MVC))
                            m_nonMVCVideoFound = true;
                        m_pmtVersionPid = pid;
                        m_pmtVersion = -2;
                    }
                    else
                    {
                        TS_program_map_section clipPmt;
                        if (clipPmt.deserialize(pmtBuffer, pmtBufferLen))
                            for (const auto& [streamPid, info] : clipPmt.pidList)
                                if (streamPid >= 0 && streamPid < 8192 && m_acceptedPidCache[streamPid])
                                    m_pmt.pidList.emplace(streamPid, info);  // a known stream stays as it is
                    }
                    pmtBufferLen = 0;
                }
            }
        }
    }
}

// The full time of a PES time: of the three it can be round the 33 bit wrap, the one nearest the last full time.
// After the wrap the times went back to 0, so the subtitles after it were placed hours before the start and lost.
int64_t TSDemuxer::fullPts(const int64_t raw)
{
    constexpr int64_t WRAP = int64_t{1} << 33;
    int64_t full = raw + m_ptsWrapOffset;
    if (m_lastFullPts != -1)
    {
        if (full < m_lastFullPts - WRAP / 2)
        {
            m_ptsWrapOffset += WRAP;
            full += WRAP;
        }
        else if (full > m_lastFullPts + WRAP / 2 && full >= WRAP)
            full -= WRAP;  // a packet from before the wrap, after one from after it
    }
    m_lastFullPts = full;
    return full;
}

// A broadcast recording can change its program map, at a change of programme: new PIDs for the sound, a second
// sound or subtitles, or another codec on a PID. The tracks are those of the first map, and the new ones were left
// out without a word. Said once, with the tracks the new map adds or changes.
void TSDemuxer::checkPmtVersion(uint8_t* payload, const uint8_t* end)
{
    const uint8_t* section = payload + 1 + payload[0];
    if (section + 8 > end || section[0] != 2)
        return;
    const int version = section[5] >> 1 & 0x1f;
    if (m_pmtVersion == -2)
        m_pmtVersion = version;
    if (version == m_pmtVersion || m_pmtChangeReported)
        return;
    m_pmtVersion = version;
    TS_program_map_section pmt;
    const int len = static_cast<int>(end - payload);
    if (!TS_program_map_section::isFullBuff(payload, len) || !pmt.deserialize(payload, len))
        return;
    std::string added;
    std::string changed;
    for (const auto& [streamPid, info] : pmt.pidList)
    {
        const auto known = m_pmt.pidList.find(streamPid);
        std::string& list = known == m_pmt.pidList.end() ? added : changed;
        if (known == m_pmt.pidList.end() || known->second.m_streamType != info.m_streamType)
            list += (list.empty() ? "" : ", ") + std::to_string(streamPid);
    }
    if (added.empty() && changed.empty())
        return;
    m_pmtChangeReported = true;
    // once per file: the file is read by more than one demuxer (the tracks are told before the mux)
    static std::set<std::string> reported;
    if (!reported.insert(m_streamNameLow).second)
        return;
    LTRACE(LT_WARN, 2,
           "Warning: the program map of the transport stream changes inside the file."
               << (added.empty() ? "" : " Tracks " + added + " are not in the first map and are left out.")
               << (changed.empty()
                       ? ""
                       : " Tracks " + changed + " change their kind of stream; they are read as the first map says."));
}

int TSDemuxer::simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
{
    if (m_firstDemuxCall)
    {
        for (const int acceptedPID : acceptedPIDs) m_acceptedPidCache[acceptedPID] = 1;
        m_firstDemuxCall = false;
    }

    MemoryBlock* vect = nullptr;
    int lastPid = -1;

    for (int acceptedPID : acceptedPIDs) demuxedData[acceptedPID];

    discardSize = 0;
    uint32_t readedBytes;
    int readRez = 0;
    bool isFirstBlock = false;
    uint8_t* data = m_bufferedReader->readBlock(m_readerID, readedBytes, readRez, &isFirstBlock);  // blocked read mode
    if (readRez == BufferedFileReader::DATA_NOT_READY)
    {
        m_lastReadRez = readRez;
        return BufferedFileReader::DATA_NOT_READY;
    }
    if (readedBytes + m_tmpBufferLen == 0 || (readedBytes == 0 && m_lastReadRez == BufferedReader::DATA_EOF))
    {
        if (!m_dvbSubtitles.empty() && !m_dvbSubtitlesFinished)
        {
            m_dvbSubtitlesFinished = true;
            for (auto& [pid, sub] : m_dvbSubtitles)
                if (m_acceptedPidCache[pid])
                    convertDvbSubtitlePes(pid, sub, demuxedData, true, discardSize);
            m_lastReadRez = readRez;
            return 0;
        }
        m_lastReadRez = readRez;
        return BufferedReader::DATA_EOF;
    }
    if (readedBytes > 0)
        m_bufferedReader->notify(m_readerID, readedBytes);
    m_lastReadRez = readRez;
    data += TS_FRAME_SIZE;
    if (m_tmpBufferLen > 0)
    {
        memcpy(data - m_tmpBufferLen, m_tmpBuffer, m_tmpBufferLen);
        data -= m_tmpBufferLen;
        readedBytes += static_cast<uint32_t>(m_tmpBufferLen);
        m_tmpBufferLen = 0;
    }

    int64_t handedOn = 0;  // bytes of subtitles written here (clearSubtitlesAtPartEnd), counted against the discarded
    if (isFirstBlock)
    {
        if (isJoinOfFiles())
            endJoinFile();
        handedOn = clearSubtitlesAtPartEnd(demuxedData);
        if (m_curFileNum < m_mplsInfo.size())
            m_prevFileLen +=
                static_cast<int64_t>(m_mplsInfo[m_curFileNum].OUT_time - m_mplsInfo[m_curFileNum].IN_time) *
                2;  // in 90Khz clock
        else
        {
            // A part cut from a clip counts its pictures by the step of their decoding times: the first two of them
            // need not be one picture apart, where the pictures shown before its first key picture were left out
            const bool cutPart = m_curFileNum < m_clipParts.size() && m_clipParts[m_curFileNum].isCut();
            if (m_firstVideoPTS != -1 && m_lastVideoPTS != -1)
                m_prevFileLen += (m_lastVideoPTS - m_firstVideoPTS +
                                  (cutPart && m_partVideoStep > 0 ? m_partVideoStep : m_videoDtsGap));
            else  // no video file
                m_prevFileLen += (m_lastPTS - m_firstPTS);
        }
        m_firstPTS = -1;
        m_firstSeenPTS = -1;
        m_lastPTS = -1;
        m_ptsWrapOffset = 0;  // each file has its own clock
        m_lastFullPts = -1;
        m_firstVideoPTS = -1;
        m_firstSeenVideoPTS = -1;
        m_lastVideoPTS = -1;
        m_curFileNum++;
        startPart();
        m_fileFirstDts.clear();
    }

    discardSize = -handedOn;

    if (readedBytes < TS_FRAME_SIZE)
    {
        discardSize += readedBytes;
        return 0;
    }

    const uint8_t* lastFrameAddr = data + readedBytes - TS_FRAME_SIZE;

    if (m_firstCall && !m_m2tsMode)
    {
        m_m2tsMode = checkForRealM2ts(data, lastFrameAddr);
        m_firstCall = false;
    }

    if (m_pmt.pidList.empty() || mvcContinueExpected())
        readProgramTables(data, lastFrameAddr, false);
    else if (isFirstBlock)
        readProgramTables(data, lastFrameAddr, true);
    if (isFirstBlock && m_pmtVersion != -1)
        m_pmtVersion = -2;  // each clip of a playlist has a program map of its own

    // A block holds data of one file only
    const ClipPart* part =
        m_curFileNum < m_clipParts.size() && m_clipParts[m_curFileNum].isCut() ? &m_clipParts[m_curFileNum] : nullptr;

    for (m_curPos = data; m_curPos <= lastFrameAddr; m_curPos += TS_FRAME_SIZE)
    {
        if (!m_m2tsHdrDiscarded && m_m2tsMode)
        {
            discardSize += 4;
            m_curPos += 4;
        }
        while (m_curPos <= lastFrameAddr && *m_curPos != 0x47)
        {
            discardSize++;
            m_curPos++;
        }
        if (m_curPos > lastFrameAddr)
        {
            m_m2tsHdrDiscarded = true;
            break;
        }
        m_m2tsHdrDiscarded = false;

        const auto tsPacket = reinterpret_cast<TSPacket*>(m_curPos);
        int pid = tsPacket->getPID();
        discardSize += TS_FRAME_SIZE;

        // pcrFrames++;

        if (tsPacket->afExists)
        {
            if (tsPacket->adaptiveField.pcrExist)
            {
                // if (prevPCR == -1)
                //	prevPCR = tsPacket->adaptiveField.getPCR33();

                // prevPCR = tsPacket->adaptiveField.getPCR33();
                // LTRACE(LT_INFO, 2, "pcr: " << prevPCR - 55729419ll);
                // pcrFrames = 0;
            }
        }

        uint8_t* frameData = m_curPos + tsPacket->getHeaderSize();
        if (pid == m_pmtVersionPid && tsPacket->payloadStart)
            checkPmtVersion(frameData, m_curPos + TS_FRAME_SIZE);
        const bool pesStartCode = frameData[0] == 0 && frameData[1] == 0 && frameData[2] == 1 && tsPacket->payloadStart;
        if (part != nullptr && !pesStartCode && !m_partKeep[pid])
            continue;
        if (!pesStartCode && m_joinDrop[pid])
            continue;
        if (pesStartCode)
        {
            const auto pesPacket = reinterpret_cast<PESPacket*>(frameData);
            auto streamInfo = m_pmt.pidList.find(pid);
            if (part != nullptr &&
                !keptInPart(*part, pid,
                            streamInfo != m_pmt.pidList.end() ? streamInfo->second.m_streamType : StreamType::RESERVED,
                            pesPacket, TS_FRAME_SIZE - (frameData - m_curPos)))
                continue;
            m_clipStartOfPid.try_emplace(pid, m_prevFileLen);

            if ((pesPacket->flagsLo & 0x80) == 0x80)
            {
                const int64_t curPts = fullPts(pesPacket->getPts());
                int64_t curDts = curPts;

                if ((pesPacket->flagsLo & 0xc0) == 0xc0)
                    curDts = fullPts(pesPacket->getDts());

                if (m_lastPTS == -1 || curPts > m_lastPTS)
                    m_lastPTS = curPts;

                if (m_firstPTS == -1 || curPts < m_firstPTS)
                    m_firstPTS = curPts;

                if (m_firstSeenPTS == -1)
                    m_firstSeenPTS = curPts;

                if (streamInfo != m_pmt.pidList.end() && isVideoPID(streamInfo->second.m_streamType))
                {
                    if (m_firstVideoPTS == -1 || curPts < m_firstVideoPTS)
                        m_firstVideoPTS = curPts;
                    if (m_firstSeenVideoPTS == -1)
                        m_firstSeenVideoPTS = curPts;
                    if (m_firstVideoPid == -1)
                        m_firstVideoPid = pid;
                    m_videoPesRead++;
                    m_fileFirstDts.try_emplace(pid, curDts);
                    if (pid == m_firstVideoPid && isJoinOfFiles())
                    {
                        if (m_joinFileDts.first == -1)
                            m_joinFileDts.first = curDts;
                        m_joinFileDts.second = (std::max)(m_joinFileDts.second, curDts);
                    }
                    if (curPts > m_lastVideoPTS)
                        m_lastVideoPTS = curPts;
                    if (m_lastVideoDTS == -1)
                        m_lastVideoDTS = curDts;
                    if (m_videoDtsGap == -1 && curDts > m_lastVideoDTS)
                        m_videoDtsGap = curDts - m_lastVideoDTS;
                    if (part != nullptr)
                    {
                        if (m_partLastVideoDts != -1 && curDts > m_partLastVideoDts &&
                            (m_partVideoStep <= 0 || curDts - m_partLastVideoDts < m_partVideoStep))
                            m_partVideoStep = curDts - m_partLastVideoDts;
                        m_partLastVideoDts = curDts;
                    }
                }

                if (m_firstPtsTime.find(pid) == m_firstPtsTime.end())
                    m_firstPtsTime[pid] = curPts;

                if (m_acceptedPidCache[pid] && streamInfo != m_pmt.pidList.end() &&
                    isAudioStreamType(streamInfo->second.m_streamType) && isJoinOfFiles())
                    m_joinDrop[pid] = !keepJoinSound(pid, streamInfo->second.m_streamType, curPts);
            }
            if (m_joinDrop[pid])
                continue;

            if (m_acceptedPidCache[pid] && isDvbSubtitlePid(pid))
            {
                DvbSubtitlePes& sub = m_dvbSubtitles[pid];
                convertDvbSubtitlePes(pid, sub, demuxedData, false, discardSize);
                if ((pesPacket->flagsLo & 0x80) == 0x80)
                {
                    // on the PGS timeline: from the first video picture, joined files one after another
                    const int64_t ptsBase = m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS;
                    sub.pts = (fullPts(pesPacket->getPts()) - ptsBase + m_prevFileLen) * INTERNAL_PTS_FREQ / 90000;
                }
                else
                    sub.pts = -1;
            }
            if (streamInfo != m_pmt.pidList.end() &&
                streamInfo->second.m_streamType != StreamType::SUB_PGS)  // demux PGS with PES headers
                frameData += pesPacket->getHeaderLength();
            else
            {
                // A part cut from a clip starts at its key picture, also for a subtitle read before that picture
                const int64_t ptsBase = part != nullptr && part->startCut ? part->firstShown()
                                        : m_firstVideoPTS != -1           ? m_firstVideoPTS
                                                                          : m_firstPTS;
                if ((pesPacket->flagsLo & 0xc0) == 0xc0)
                {
                    const int64_t pts = fullPts(pesPacket->getPts()) - ptsBase + m_prevFileLen;
                    int64_t dts = fullPts(pesPacket->getDts()) - ptsBase + m_prevFileLen;
                    if (part != nullptr && dts < 0)
                        dts = 0;  // decoded before the cut, at the start of the output
                    pesPacket->setPtsAndDts(pts, dts);
                }
                else if ((pesPacket->flagsLo & 0x80) == 0x80)
                {
                    const int64_t pts = fullPts(pesPacket->getPts()) - ptsBase + m_prevFileLen;
                    pesPacket->setPts(pts);
                }
            }
        }

        // if (acceptedPIDs.find(pid) == acceptedPIDs.end())
        if (!m_acceptedPidCache[pid])
            continue;

        const int64_t payloadLen = TS_FRAME_SIZE - (frameData - m_curPos);
        if (payloadLen > 0 && isDvbSubtitlePid(pid))
        {
            DvbSubtitlePes& sub = m_dvbSubtitles[pid];
            sub.pes.insert(sub.pes.end(), frameData, frameData + payloadLen);
            continue;
        }
        if (payloadLen > 0)
        {
            if (pid != lastPid)
            {
                vect = &demuxedData[pid];
                lastPid = pid;
            }
            if (vect != nullptr)
            {
                vect->grow(payloadLen);
                uint8_t* dst = vect->data() + vect->size() - payloadLen;
                memcpy(dst, frameData, payloadLen);
            }
            if (m_joinSoundPid[pid])
                m_joinSound[pid].pesBytes += payloadLen;
        }
        discardSize -= payloadLen;
    }
    if (m_curPos < data + readedBytes)
    {
        m_tmpBufferLen = data + readedBytes - m_curPos;
        memmove(m_tmpBuffer, m_curPos, m_tmpBufferLen);
    }

    return 0;
}

// A clip file of a playlist begins: the state of the part before is cleared. The sound of a cut part carries where
// it ended in the output over to the next part, which starts its own sound where that leaves off.
void TSDemuxer::startPart()
{
    const bool prevCut =
        m_curFileNum > 0 && m_curFileNum - 1 < m_clipParts.size() && m_clipParts[m_curFileNum - 1].isCut();
    for (auto& [pid, cut] : m_partCut)
    {
        // a part that kept nothing of the sound passes on where the sound before it ended
        if (!prevCut)
            cut.outEnd = -1;
        else if (cut.keptAny && cut.outPos >= 0)
            cut.outEnd = cut.outPos + cut.step;
        cut.keep = cut.ended = cut.keptAny = false;
        cut.lastSeenPts = cut.firstKeptPts = cut.lastKeptPts = -1;
        cut.pgsShowing = cut.pgsClearing = cut.pgsCleared = false;
    }
    memset(m_partKeep, 0, sizeof(m_partKeep));
    m_partLastVideoDts = -1;
}

// Whether a PES of a clip of which only a part is muxed is kept, decided where it starts; the packets that carry the
// rest of it go with it. The part is read from a little before its first key picture to a little after its last one.
//
// Video is kept in the order of decoding from the key picture at or before the play item's IN time, where a player
// starts to decode, up to the key picture at or after its OUT time, which is left out: a stream that is not encoded
// again can only be cut where a group of pictures begins. The pictures shown before the first key picture are made
// from pictures before the cut and are left out too. The readers count the pictures they are given, so the parts
// follow one another without a gap.
//
// The sound is cut to the same times. Its readers count frames as well, so a part's sound starts where the sound of
// the part before ends in the output: the first frame kept is the one that belongs nearest to that point, at most
// half a frame away, and the sound cannot drift from the pictures over many parts. A part that ends at a key picture
// ends its sound there, within half a frame.
//
// Subtitles are kept from the first key picture to the last one, at their times moved with the part. A subtitle still
// on the screen where the part ends is taken off there: the display set after the end that clears the screen is kept,
// moved to the end of the part.
bool TSDemuxer::keptInPart(const ClipPart& part, const int pid, const StreamType type, PESPacket* pes,
                           const int64_t payloadLen)
{
    PartCut& cut = m_partCut[pid];
    if ((pes->flagsLo & 0x80) == 0)  // no time: it goes with the PES before it
    {
        m_partKeep[pid] = cut.keep;
        return cut.keep;
    }
    const int64_t pts = pes->getPts();
    bool keep;
    if (isVideoPID(type))
    {
        // the last key picture itself is kept with the pictures shown before it that the play item shows (keepEndKey)
        if (part.endPts >= 0 && (part.keepEndKey ? pts > part.endPts : pts >= part.endPts))
            cut.ended = true;
        keep = !cut.ended && (!part.startCut || pts >= part.firstShown());
        if (keep)
        {
            // the first picture of every part after the first, by its number in the track, for the video reader; a
            // part that continues the one before goes on without a break
            if (!cut.keptAny && m_curFileNum > 0 && !part.continuesPrevious)
                m_partStartPictures[pid].insert(m_keptPictures[pid]);
            cut.keptAny = true;
            m_keptPictures[pid]++;
        }
    }
    else if (type == StreamType::SUB_PGS)
    {
        const uint8_t* segment = reinterpret_cast<const uint8_t*>(pes) + pes->getHeaderLength();
        const int64_t segmentLen = payloadLen - pes->getHeaderLength();
        const bool composition = segmentLen >= 14 && segment[0] == 0x16;  // presentation composition segment
        keep = (!part.startCut || pts >= part.firstShown()) && (part.endPts < 0 || pts < part.shownEnd);
        if (!keep && part.endPts >= 0 && !part.continuedByNext && pts >= part.shownEnd && !cut.pgsCleared &&
            (cut.pgsClearing || (cut.pgsShowing && composition && segment[13] == 0)))
        {
            keep = cut.pgsClearing = true;
            if (segmentLen >= 1 && segment[0] == 0x80)  // the end of the display set
            {
                cut.pgsClearing = false;
                cut.pgsCleared = true;
            }
            if ((pes->flagsLo & 0xc0) == 0xc0)
                pes->setPtsAndDts(part.shownEnd, part.shownEnd);
            else
                pes->setPts(part.shownEnd);
        }
        if (keep && composition)
        {
            cut.pgsShowing = segment[13] != 0;
            memcpy(cut.pgsVideo, segment + 3, sizeof(cut.pgsVideo));
        }
        // a window definition, kept for a display set written at the end of the part (clearSubtitlesAtPartEnd)
        if (keep && segmentLen >= 3 && segment[0] == 0x17 && 3 + (segment[1] << 8 | segment[2]) <= segmentLen)
            cut.pgsWindow.assign(segment, segment + 3 + (segment[1] << 8 | segment[2]));
    }
    else
    {
        // A TrueHD track carries each AC-3 core frame with the lossless frames of its time after it, and its reader
        // takes them in that order: the track is cut where a core frame begins, and the lossless frames go with it
        if (type == StreamType::AUDIO_TRUE_HD)
        {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(pes) + pes->getHeaderLength();
            if (payloadLen - pes->getHeaderLength() < 2 || data[0] != 0x0B || data[1] != 0x77)
            {
                m_partKeep[pid] = cut.keep;
                return cut.keep;
            }
        }
        if (cut.lastSeenPts >= 0 && pts > cut.lastSeenPts && (cut.step == 0 || pts - cut.lastSeenPts < cut.step))
            cut.step = pts - cut.lastSeenPts;
        cut.lastSeenPts = pts;
        const int64_t half = cut.step / 2;
        if (cut.keptAny)
            keep = pts >= cut.firstKeptPts;
        else if (part.startPts >= 0 && (part.startCut || cut.outEnd >= 0))
        {
            // where this frame belongs in the output, against where the sound before it ends
            const int64_t at = m_prevFileLen + pts - part.firstShown();
            keep = at >= (cut.outEnd >= 0 ? cut.outEnd : m_prevFileLen) - half;
        }
        else
            keep = true;
        if (keep && part.endPts >= 0 && pts >= part.shownEnd - half)
            keep = false;
        if (keep)
        {
            if (!cut.keptAny)
            {
                cut.keptAny = true;
                cut.firstKeptPts = pts;
                cut.outPos = cut.outEnd >= 0 ? cut.outEnd : m_prevFileLen;
            }
            else
                cut.outPos += pts - cut.lastKeptPts;
            cut.lastKeptPts = pts;
        }
    }
    cut.keep = keep;
    m_partKeep[pid] = keep;
    return keep;
}

// A part that ends at a key picture while a subtitle is on the screen, and whose own display set that takes it off
// comes after the bytes read of the part, left the subtitle on into the next part, until a display set of that part
// replaced it. A display set that clears the screen is written at the end of the part, with the window of the
// subtitle, as the stream's own would be.
int64_t TSDemuxer::clearSubtitlesAtPartEnd(DemuxedData& demuxedData)
{
    int64_t written = 0;
    if (m_curFileNum >= m_clipParts.size())
        return written;
    const ClipPart& part = m_clipParts[m_curFileNum];
    if (part.startPts < 0 || part.endPts < 0 || part.continuedByNext)
        return written;
    const int64_t pts = part.shownEnd - (part.startCut ? part.firstShown() : m_firstVideoPTS) + m_prevFileLen;
    for (auto& [pid, cut] : m_partCut)
    {
        if (!cut.pgsShowing || cut.pgsCleared || cut.pgsWindow.empty() || pid < 0 || pid >= 8192 ||
            !m_acceptedPidCache[pid])
            continue;
        const auto composition = static_cast<uint16_t>((cut.pgsVideo[5] << 8 | cut.pgsVideo[6]) + 1);
        std::vector<std::vector<uint8_t>> segments;
        segments.push_back({0x16, 0, 11, cut.pgsVideo[0], cut.pgsVideo[1], cut.pgsVideo[2], cut.pgsVideo[3],
                            cut.pgsVideo[4], static_cast<uint8_t>(composition >> 8), static_cast<uint8_t>(composition),
                            0, 0, 0, 0});  // normal case, no object
        segments.push_back(cut.pgsWindow);
        segments.push_back({0x80, 0, 0});
        StreamData& out = demuxedData[pid];
        for (const auto& segment : segments)
        {
            constexpr int headerLen = PESPacket::HEADER_SIZE + 2 * PESPacket::PTS_SIZE;
            const auto len = static_cast<int>(headerLen + segment.size());
            out.grow(len);
            uint8_t* dst = out.data() + out.size() - len;
            auto* pes = reinterpret_cast<PESPacket*>(dst);
            pes->serialize(pts, pts, PES_PRIVATE_DATA1);
            pes->setPacketLength(len);
            memcpy(dst + headerLen, segment.data(), segment.size());
            written += len;
        }
        cut.pgsShowing = false;
        cut.pgsCleared = true;
    }
    return written;
}

bool TSDemuxer::isDvbSubtitlePid(const int pid) const
{
    const auto info = m_pmt.pidList.find(pid);
    return info != m_pmt.pidList.end() && info->second.m_streamType == StreamType::PRIVATE_DATA &&
           info->second.m_dvbDescriptor == static_cast<int>(TSDescriptorTag::DVB_SUBTITLING);
}

void TSDemuxer::convertDvbSubtitlePes(const int pid, DvbSubtitlePes& sub, DemuxedData& demuxedData, const bool last,
                                      int64_t& discardSize)
{
    if (!sub.converter)
    {
        // The composition and ancillary page of the subtitling descriptor, as a Matroska track keeps them.
        uint8_t pages[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        bool havePages = false;
        const auto info = m_pmt.pidList.find(pid);
        if (info != m_pmt.pidList.end())
            for (int i = 0; i + 2 <= info->second.m_esInfoLen;)
            {
                const uint8_t tag = info->second.m_esInfoData[i];
                const uint8_t len = info->second.m_esInfoData[i + 1];
                if (tag == static_cast<uint8_t>(TSDescriptorTag::DVB_SUBTITLING) && len >= 8 &&
                    i + 2 + 8 <= info->second.m_esInfoLen)
                {
                    memcpy(pages, info->second.m_esInfoData + i + 2 + 4, 4);
                    havePages = true;
                    break;
                }
                i += 2 + len;
            }
        sub.converter = std::make_shared<ParsedBitmapSubTrackData>(
            new DvbSubDecoder(havePages ? pages : nullptr, havePages ? 4 : 0, 0.0));
    }
    StreamData& out = demuxedData[pid];
    const auto append = [&out, &discardSize](const AVPacket& pkt)
    {
        if (pkt.size <= 0 || !pkt.data)
            return;
        discardSize -= pkt.size;
        out.grow(pkt.size);
        memcpy(out.data() + out.size() - pkt.size, pkt.data, pkt.size);
    };
    if (!sub.pes.empty() && sub.pts >= 0)
    {
        AVPacket pkt;
        pkt.pts = sub.pts;
        pkt.duration = AV_NOPTS_VALUE_INTERNAL;
        sub.converter->extractData(&pkt, sub.pes.data(), static_cast<int>(sub.pes.size()));
        append(pkt);
        delete[] pkt.data;
    }
    sub.pes.clear();
    if (last)
    {
        AVPacket pkt;
        if (sub.converter->finish(&pkt))
        {
            append(pkt);
            delete[] pkt.data;
        }
    }
}

void TSDemuxer::openFile(const std::string& streamName)
{
    m_streamName = streamName;
    m_streamNameLow = strToLowerCase(unquoteStr(streamName));
    readClose();
    m_ptsWrapOffset = 0;
    m_lastFullPts = -1;
    m_lastPesPts.clear();
    m_dvbSubtitles.clear();
    m_dvbSubtitlesFinished = false;
    m_lastPesDts.clear();
    m_fullPtsTime.clear();
    m_fullDtsTime.clear();
    m_firstPtsTime.clear();
    m_firstDtsTime.clear();
    m_clipStartOfPid.clear();
    m_firstPCRTime = -1;
    m_lastPCRVal = -1;

    m_m2tsMode = isM2TSExt(streamName);

    if (!m_bufferedReader->openStream(m_readerID, m_streamName.c_str()))
        THROW(ERR_FILE_NOT_FOUND, "Can't open stream " << m_streamName)

    m_pmtPid = -1;
    m_codecReady = false;
    m_readCnt = 0;
    m_dataProcessed = 0;
    m_notificated = false;
}

void TSDemuxer::readClose() {}

TSDemuxer::~TSDemuxer() { m_bufferedReader->deleteReader(m_readerID); }

int64_t TSDemuxer::getDemuxedSize() { return m_dataProcessed; }

void TSDemuxer::setFileIterator(FileNameIterator* itr)
{
    // The files of a join or a playlist, for getTrackPacketFps: a first clip may be too short to give a rate
    if (const auto* list = dynamic_cast<const FileListIterator*>(itr))
    {
        m_joinFiles = list->getFiles();
        m_clipParts = list->getClipParts();
    }
    const auto br = dynamic_cast<BufferedFileReader*>(m_bufferedReader);
    if (br)
        br->setFileIterator(itr, m_readerID);
    else if (itr != nullptr)
        THROW(ERR_COMMON, "Can not set file iterator. Reader does not support bufferedReader interface.")
}

int64_t getLastPCR(const File& file, const int bufferSize, const int frameSize, const int64_t fileSize,
                   const int pcrPid)
{
    // pcr from end of file
    const auto tmpBuffer = new uint8_t[bufferSize];
    file.seek(FFMAX(fileSize - fileSize % frameSize - bufferSize, 0), File::SeekMethod::smBegin);
    const int len = file.read(tmpBuffer, bufferSize);
    if (len < 1)
        return -2;  // read error
    uint8_t* curPtr = tmpBuffer;
    const uint8_t* bufferEnd = tmpBuffer + len;
    int64_t lastPcrVal = -1;
    while (curPtr <= bufferEnd - frameSize)
    {
        const auto tsPacket = reinterpret_cast<TSPacket*>(curPtr + frameSize - 188);
        if (tsPacket->afExists && tsPacket->adaptiveField.length && tsPacket->adaptiveField.pcrExist &&
            (pcrPid < 0 || tsPacket->getPID() == pcrPid))
        {
            lastPcrVal = tsPacket->adaptiveField.getPCR33();
        }
        curPtr += frameSize;
    }
    delete[] tmpBuffer;
    return lastPcrVal;
}

int64_t getTSDuration(const char* fileName)
{
    try
    {
        int frameSize = 188;
        if (isM2TSExt(fileName))
            frameSize = 192;
        int64_t fileSize;
        File file(fileName, File::ofRead);
        if (!file.size(&fileSize))
            return -1;
        int bufferSize = 1024 * 256;
        bufferSize -= bufferSize % frameSize;
        const auto tmpBuffer = new uint8_t[bufferSize];
        // pcr from start of file
        const int len = file.read(tmpBuffer, bufferSize);
        if (len < 1)
        {
            delete[] tmpBuffer;
            return 0;
        }
        uint8_t* curPtr = tmpBuffer;
        const uint8_t* bufferEnd = tmpBuffer + len;
        int64_t firstPcrVal = 0;
        int pcrPid = -1;
        while (curPtr <= bufferEnd - frameSize)
        {
            const auto tsPacket = reinterpret_cast<TSPacket*>(curPtr + frameSize - 188);
            if (tsPacket->afExists && tsPacket->adaptiveField.length && tsPacket->adaptiveField.pcrExist)
            {
                firstPcrVal = tsPacket->adaptiveField.getPCR33();
                pcrPid = tsPacket->getPID();
                break;
            }
            curPtr += frameSize;
        }
        delete[] tmpBuffer;

        int64_t lastPcrVal;
        bufferSize = 1024 * 256;
        bufferSize -= bufferSize % frameSize;
        do
        {
            lastPcrVal = getLastPCR(file, bufferSize, frameSize, fileSize, pcrPid);
            bufferSize *= 4;
        } while (lastPcrVal == -1 && bufferSize <= 1024 * 1024);

        file.close();
        if (lastPcrVal < 0)
            return 0;
        // The first and last clock reference come from the SAME PID: a broadcast capture carries one
        // clock per programme, and subtracting one programme's clock from another's printed a
        // duration of minus twelve hours. And the 33 bit clock may wrap once inside a file.
        int64_t span = lastPcrVal - firstPcrVal;
        if (span < 0)
            span += 1ll << 33;
        return span;
    }
    catch (...)
    {
        return 0;
    }
}

int64_t TSDemuxer::getFileDurationNano() const { return getTSDuration(m_streamName.c_str()) * 1000000000ll / 90000ll; }

namespace
{
// Counts the pictures that begin in each PES of a video stream, from the NAL unit (or VC-1 start code) after each
// start code: the first slice of a picture, or a VVC picture header. A start code split between two TS packets is
// still found, as the bytes run on through one counter.
class PictureStarts
{
   public:
    explicit PictureStarts(const StreamType type)
    {
        switch (type)
        {
        case StreamType::VIDEO_H264:
            m_need = 2;
            break;
        case StreamType::VIDEO_H265:
        case StreamType::VIDEO_H266:
            m_need = 3;
            break;
        case StreamType::VIDEO_VC1:
            m_need = 1;
            break;
        default:
            m_need = 0;  // not counted
        }
        m_type = type;
    }
    void newPes()
    {
        m_inPes = 0;
        m_last = 0xffffffff;
        m_have = -1;
    }
    void feed(const uint8_t* p, const uint8_t* end)
    {
        if (m_need == 0)
            return;
        for (; p < end; ++p)
        {
            if (m_have >= 0)
            {
                m_header[m_have++] = *p;
                if (m_have == m_need)
                {
                    m_have = -1;
                    if (isPictureStart())
                        ++m_inPes;
                }
            }
            m_last = m_last << 8 | *p;
            if ((m_last & 0xffffff) == 1)
                m_have = 0;
        }
    }
    [[nodiscard]] int inPes() const { return m_inPes; }

   private:
    [[nodiscard]] bool isPictureStart() const
    {
        const uint8_t* h = m_header;
        switch (m_type)
        {
        case StreamType::VIDEO_H264:  // a slice with first_mb_in_slice 0
        {
            const int type = h[0] & 0x1f;
            return (type == 1 || type == 5) && h[1] & 0x80;
        }
        case StreamType::VIDEO_H265:  // a slice of layer 0 with first_slice_segment_in_pic_flag
        {
            const int type = h[0] >> 1 & 0x3f;
            const int layer = (h[0] & 1) << 5 | h[1] >> 3;
            return type < 32 && layer == 0 && h[2] & 0x80;
        }
        case StreamType::VIDEO_H266:  // a picture header, or a slice of layer 0 that carries one
        {
            const int layer = h[0] & 0x3f;
            const int type = h[1] >> 3;
            return layer == 0 && (type == 19 || (type < 12 && h[2] & 0x80));
        }
        case StreamType::VIDEO_VC1:  // a frame start code
            return h[0] == 0x0d;
        default:
            return false;
        }
    }
    StreamType m_type;
    int m_need;
    int m_inPes = 0;
    uint32_t m_last = 0xffffffff;
    uint8_t m_header[3]{};
    int m_have = -1;  // -1: no start code just read
};

// The times in the PES headers of one PID in one file, in file order, with what stops them giving the rate.
struct PesTimes
{
    std::vector<int64_t> times;
    int64_t bytes = 0;
    bool withoutTime = false;   // a PES start without a time
    bool manyPictures = false;  // a PES that carries more than one picture
    bool unreadable = false;    // not a transport stream
};

PesTimes readPesTimes(const std::string& name, const uint32_t pid, const StreamType type, const size_t maxTimes,
                      const int64_t maxBytes)
{
    PesTimes r;
    File f;
    if (!f.open(name.c_str(), File::ofRead))
    {
        r.unreadable = true;
        return r;
    }
    static constexpr int CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK + 2 * 192);
    PictureStarts pictures(type);
    bool inPes = false;
    int stride = 0;   // 188, or 192 for an M2TS: found from the first packets
    size_t have = 0;  // bytes in buf
    // The PES in progress when the window is full is counted too: its pictures so far are all it can hold.
    const auto endPes = [&]
    {
        if (inPes && pictures.inPes() > 1)
            r.manyPictures = true;
    };
    while (r.times.size() < maxTimes && !r.withoutTime && !r.manyPictures && r.bytes < maxBytes)
    {
        const int len = f.read(buf.data() + have, CHUNK);
        if (len <= 0)
            break;
        r.bytes += len;
        have += len;
        size_t pos = 0;
        if (stride == 0)
        {
            for (const int s : {192, 188})
            {
                const size_t first = s - 188;  // the sync byte follows the 4-byte M2TS header
                if (have >= first + 8 * static_cast<size_t>(s))
                {
                    bool all = true;
                    for (int k = 0; k < 8 && all; ++k) all = buf[first + k * s] == 0x47;
                    if (all)
                    {
                        stride = s;
                        pos = first;
                        break;
                    }
                }
            }
            if (stride == 0)
            {
                r.unreadable = true;
                return r;
            }
        }
        else
            pos = stride - 188;
        while (pos + stride <= have && r.times.size() < maxTimes && !r.manyPictures)
        {
            const uint8_t* pk = buf.data() + pos;
            if (pk[0] != 0x47)
            {
                // lost the packet rhythm: find the next sync byte that has another one a packet later
                size_t next = pos + 1;
                while (next + stride < have && !(buf[next] == 0x47 && buf[next + stride] == 0x47)) ++next;
                pos = next;
                continue;
            }
            pos += stride;
            const uint32_t packetPid = (pk[1] & 0x1f) << 8 | pk[2];
            const int afc = pk[3] >> 4 & 3;
            if (packetPid != pid || !(afc & 1))
                continue;
            const int off = afc & 2 ? 5 + pk[4] : 4;
            if (off >= 188)
                continue;
            if (!(pk[1] & 0x40))
            {
                if (inPes)
                    pictures.feed(pk + off, pk + 188);
                continue;
            }
            if (off + 14 > 188)
                continue;
            const uint8_t* pes = pk + off;
            if (pes[0] != 0 || pes[1] != 0 || pes[2] != 1)
                continue;
            endPes();
            if (!(pes[7] & 0x80))
            {
                r.withoutTime = true;
                break;
            }
            int64_t pts = static_cast<int64_t>(pes[9] & 0x0e) << 29 | static_cast<int64_t>(pes[10]) << 22 |
                          static_cast<int64_t>(pes[11] & 0xfe) << 14 | static_cast<int64_t>(pes[12]) << 7 |
                          pes[13] >> 1;
            if (!r.times.empty() && pts < r.times.front() - (1ll << 32))
                pts += 1ll << 33;  // the 33-bit clock wrapped
            r.times.push_back(pts);
            inPes = true;
            pictures.newPes();
            const int payload = off + 9 + pes[8];
            if (payload < 188)
                pictures.feed(pk + payload, pk + 188);
        }
        // The packet not yet complete goes to the front for the next read, an M2TS one with its 4-byte header, so
        // that its sync byte is again where the next round starts.
        const size_t from = pos - (stride - 188);
        const size_t keep = from < have ? have - from : 0;
        if (keep)
            memmove(buf.data(), buf.data() + from, keep);
        have = keep;
    }
    if (!r.withoutTime)
        endPes();
    return r;
}
}  // namespace

// A video stream may name no frame rate of its own: HEVC, H.264 or VVC without timing information, AV1 without a timing
// info, which an encoder is free to leave out. In a transport stream the PES header of each picture still carries its
// time, so the rate is the spacing of those times. It was taken to be 25, and a 23.976 film played 4 % fast. The times
// of the track's first pictures are read on a handle of their own, sorted, since pictures come in decoding order, and
// the usual step between them gives the rate: a picture still to come leaves a hole in the order shown, and a jump
// inside a file is not a step. Each clip of a join or a playlist is sorted on its own, and a first clip with fewer
// pictures than the rate needs is followed by the next ones. Nothing is said, and the stream keeps the default, when a
// PES start in the window carries no time (H.222.0 asks for one only every 0.7 s), when a PES carries more than one
// picture, when the steps vary, or when the rate they give is no standard one: a stream without B pictures that put
// two pictures in each PES stepped two pictures at a time and was muxed at 11.988, at half its speed.
double TSDemuxer::getTrackPacketFps(const uint32_t trackId)
{
    if (const auto known = m_packetFps.find(trackId); known != m_packetFps.end())
        return known->second;
    m_packetFps[trackId] = 0.0;
    if (trackId == 0 || trackId >= 0x1fff || m_streamName.empty())
        return 0.0;

    std::vector<std::string> files{unquoteStr(m_streamName)};
    if (!m_joinFiles.empty() && unquoteStr(m_joinFiles.front()) == files.front())
        for (size_t i = 1; i < m_joinFiles.size(); ++i) files.push_back(unquoteStr(m_joinFiles[i]));
    else
        for (const std::string& later : m_laterFiles) files.push_back(unquoteStr(later));
    const auto info = m_pmt.pidList.find(static_cast<int>(trackId));
    const StreamType type = info != m_pmt.pidList.end() ? info->second.m_streamType : StreamType::RESERVED;

    static constexpr size_t PICTURES = 240;
    std::vector<int64_t> steps;
    size_t timed = 0;
    int64_t read = 0;
    for (const std::string& name : files)
    {
        if (timed >= PICTURES || read >= DETECT_STREAM_BUFFER_SIZE)
            break;
        PesTimes clip = readPesTimes(name, trackId, type, PICTURES - timed, DETECT_STREAM_BUFFER_SIZE - read);
        if (clip.unreadable && name == files.front())
            return 0.0;
        if (clip.withoutTime || clip.manyPictures)
            return 0.0;
        read += clip.bytes;
        timed += clip.times.size();
        std::vector<int64_t>& times = clip.times;
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        for (size_t i = 1; i < times.size(); ++i) steps.push_back(times[i] - times[i - 1]);
    }
    if (steps.size() < 7)
        return 0.0;
    std::vector<int64_t> sorted = steps;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const int64_t median = sorted[sorted.size() / 2];
    int64_t sum = 0;
    size_t count = 0;
    for (const int64_t step : steps)
        if (std::llabs(step - median) * 8 <= median)
        {
            sum += step;
            ++count;
        }
    if (median <= 0 || count * 4 < steps.size() * 3)
        return 0.0;  // the steps vary: no one rate
    const double fps = 90000.0 * static_cast<double>(count) / static_cast<double>(sum);
    for (const double standard : {12.5, 15000.0 / 1001, 15.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 30.0,
                                  48000.0 / 1001, 48.0, 50.0, 60000.0 / 1001, 60.0, 100.0, 120000.0 / 1001, 120.0})
        if (std::fabs(fps - standard) < standard * 0.0004)
        {
            m_packetFps[trackId] = standard;
            return standard;
        }
    return 0.0;  // no standard rate: the steps are not those of single pictures
}
