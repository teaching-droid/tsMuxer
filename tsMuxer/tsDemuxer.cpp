// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "tsDemuxer.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "abstractStreamReader.h"
#include "ac3Codec.h"
#include "bitmapSubtitles.h"
#include "bufferedFileReader.h"
#include "matroskaParser.h"
#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

namespace
{
// ** A FILE CUT AT ANY BYTE IS READ FROM ITS FIRST WHOLE PACKET. ** The next packet from cur on. Where cur is not the
// sync byte of a packet, as at the start of a file cut inside a packet or after a damaged part, the search went to the
// next 0x47 byte, and a 0x47 inside the payload in front of the first packet was taken for one: its header said a
// size no packet has, and the file could not even be listed. A 0x47 is now taken when the packet after it, step bytes
// on, begins with 0x47 too (or lies beyond the block). checkFirst asks the same of the first packet of a file, whose
// start may be any byte. A packet found where it was expected is taken as before.
uint8_t* findPacket(uint8_t* cur, const uint8_t* lastFrameAddr, const int step, const bool checkFirst)
{
    if (!checkFirst && cur <= lastFrameAddr && *cur == 0x47)
        return cur;
    for (; cur <= lastFrameAddr; ++cur)
        if (*cur == 0x47 && (cur + step > lastFrameAddr || cur[step] == 0x47))
            break;
    return cur;
}
}  // namespace

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
    memset(m_lastCounter, 0xFF, sizeof(m_lastCounter));
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
    bool firstPacket = true;
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
            curPos = findPacket(curPos, lastFrameAddr, m_m2tsMode ? 192 : TS_FRAME_SIZE, firstPacket);
            if (curPos > lastFrameAddr)
            {
                m2tsHdrDiscarded = true;
                break;
            }
            m2tsHdrDiscarded = false;
            firstPacket = false;
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
                        {
                            TrackInfo track(
                                isDvbSubtitlePid(snd.m_pid) ? TRACKTYPE_PGS : static_cast<int>(snd.m_streamType),
                                snd.m_lang, 0, snd.m_dvbDescriptor);
                            if (snd.m_streamType == StreamType::PRIVATE_DATA)
                                track.m_soundDescriptor = snd.m_soundDescriptor;
                            trackList.insert(std::make_pair(snd.m_pid, track));
                        }
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

// The sound of a file cut at any byte, or written by a muxer that does not start each PES with a frame, can begin
// inside a frame. Its first PES then begins with the rest of a frame, and the time in its header is that of the frame
// in progress, or of the frame that starts later in the PES: which of the two depends on the muxer that wrote it. The
// readers leave the rest of the frame out and gave that time to the first whole frame, which then came a frame early
// (about 11 ms for DTS). Such PES are left out at the start of the file, up to the first one that begins with a frame,
// whose time is that frame's: the first PES is never more than a few frames. Only for the kinds whose frames begin with
// a sync word; a stream none of whose first LEAD_PES_MAX PES begins with a frame is read from where it starts, as
// before. The ExSS sync word begins a frame of a stream without a core only, as DTS Express is: where there is a core,
// a PES that begins with the extension of a frame carries the end of a frame whose core was before it.
//
// ** A PES THAT BEGINS PICTURES IS NOT THE REST OF A SOUND FRAME. ** A track whose stream type declares sound but that
// holds video is still found as video, with a warning (METADemuxer::detectTrackReader). Its PES begin with pictures,
// never with a sound frame, so its first four PES were left out: the pictures that carry the parameter sets were lost,
// and H.264, HEVC, MPEG-2 and AV1 under the AC-3, DTS or AAC stream type were listed as "Can't detect stream type",
// where 2.20.0 found them. A PES that begins where a picture stream can be entered ends the search at once, and the
// track is read from there, as before. Only those units count, not any start code: the sound of a mid-frame PES can
// hold 00 00 01 by chance (a DTS frame of silence does), and leading pictures that cannot be decoded are no loss.
static bool beginsPictures(const uint8_t* p, const uint8_t* end)
{
    if (end - p >= 3 && p[0] == 0x12 && p[1] == 0x00 && p[2] == 0x0A)
        return true;  // AV1: a temporal delimiter, then a sequence header (OBU types 2 and 1, with their sizes)
    if (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        ++p;
    if (end - p < 5 || p[0] != 0 || p[1] != 0 || p[2] != 1)
        return false;
    const uint8_t c = p[3];
    if (c == 0xB3 || c == 0xB8 || c == 0x0F)
        return true;  // MPEG-1/2 sequence header or GOP, VC-1 sequence header
    if ((c & 0x80) == 0 && ((c & 0x1F) == 9 || (c & 0x1F) == 7 || (c & 0x1F) == 5 || c == 0x06))
        return true;  // H.264 access unit delimiter, sequence parameter set, IDR slice, SEI
    // HEVC: a random access picture, VPS, SPS, access unit delimiter or prefix SEI, of layer 0
    const int hevcType = c >> 1 & 0x3F;
    return (c & 0x81) == 0 && p[4] >= 1 && p[4] <= 7 &&
           ((hevcType >= 16 && hevcType <= 21) || hevcType == 32 || hevcType == 33 || hevcType == 35 || hevcType == 39);
}

TSDemuxer::LeadPes TSDemuxer::skipsLeadingPes(const int pid, const StreamType type, const uint8_t* payload,
                                              const uint8_t* end)
{
    const auto at = [&](const std::initializer_list<uint8_t> bytes)
    {
        if (end - payload < static_cast<int64_t>(bytes.size()))
            return false;
        return std::equal(bytes.begin(), bytes.end(), payload);
    };
    bool known = true;
    bool frameStart = false;
    switch (type)
    {
    case StreamType::AUDIO_AC3:
    case StreamType::AUDIO_EAC3:
    case StreamType::AUDIO_EAC3_SECONDARY:
        frameStart = at({0x0B, 0x77});
        break;
    case StreamType::AUDIO_DTS:
    case StreamType::AUDIO_DTS_HD:
    case StreamType::AUDIO_DTS_HD_MA:
        frameStart = at({0x7F, 0xFE, 0x80, 0x01});
        break;
    case StreamType::AUDIO_DTS_HD_SECONDARY:
        frameStart = at({0x7F, 0xFE, 0x80, 0x01}) || at({0x64, 0x58, 0x20, 0x25});
        break;
    case StreamType::AUDIO_MPEG1:
    case StreamType::AUDIO_MPEG2:
        frameStart = end - payload >= 2 && payload[0] == 0xFF && (payload[1] & 0xE0) == 0xE0;
        break;
    case StreamType::AUDIO_AAC:
        frameStart = end - payload >= 2 && payload[0] == 0xFF && (payload[1] & 0xF0) == 0xF0;
        break;
    default:
        known = false;
    }
    if (beginsPictures(payload, end))
        known = false;
    // a PES whose payload does not reach into this packet is taken as it is
    if (known && !frameStart && end - payload >= 4 && ++m_leadPes[pid] <= LEAD_PES_MAX)
        return LeadPes::Hold;
    m_leadPes[pid] = LEAD_PES_DONE;
    // only a PES that begins with a frame drops the PES held before it; any other end of the search gives them back
    return frameStart ? LeadPes::Keep : LeadPes::GiveBack;
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

// How long the first video stream of a file lasts on the timeline of a join, as the join lays its files (endJoinFile):
// from its first decoding time to its last, and one step more; in ns, -1 where it is not found. The whole file is read.
int64_t TSDemuxer::fileVideoLengthNs(const BufferedReaderManager& readManager, const std::string& fileName)
{
    TSDemuxer probe(readManager, "");
    probe.openFile(fileName);
    std::map<int32_t, TrackInfo> tracks;
    probe.getTrackList(tracks);
    if (probe.getFileBlockSize() == 0)
        return -1;
    DemuxedData demuxedData;
    const PIDSet none;
    int64_t discardedSize = 0;
    // each block is read in turn, as fileVideoStart reads them, until the end of the file
    for (int64_t blocks = 0; blocks < INT64_C(1) << 40; ++blocks)
        if (probe.simpleDemuxBlock(demuxedData, none, discardedSize) == BufferedReader::DATA_EOF)
            break;
    if (probe.m_videoSpanDts.first == -1 || probe.m_videoDtsGap <= 0)
        return -1;
    const int64_t ticks = probe.m_videoSpanDts.second - probe.m_videoSpanDts.first + probe.m_videoDtsGap;
    return ticks * 100000 / 9;
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
        // A file without sound of the track (a pause that lasts the whole file) keeps where the sound before ended:
        // the sound of the next file starts after a gap, with its picture, and not where the sound before ends.
        else if (sound.lastPts != -1 || !known || !m_joinBaseKnown)
            sound.lastPes = -1;
        sound.shift = 0;
        sound.firstPts = sound.lastPts = sound.prevPts = -1;
        sound.onTimeline = INT64_MIN;
    }
    // each file has its own clock: a pause of the sound is looked for in each file on its own (soundPause)
    m_soundGap.clear();
    m_gapVideoPrev = -1;
    memset(m_joinDrop, 0, sizeof(m_joinDrop));
    m_joinSkip.clear();
    // the sound of the next file may come before its first picture in the file: that picture is looked up first
    m_joinVideoStart = -1;
    if (known && m_joinBaseKnown && !m_joinSound.empty() && m_curFileNum + 1 < m_joinFiles.size())
        m_joinVideoStart = fileVideoStart(unquoteStr(m_joinFiles[m_curFileNum + 1]), m_firstVideoPid);
    const int64_t step = (std::max)(m_videoDtsGap, int64_t{0});
    const int64_t runOn = m_joinVideoStart - m_joinFileDts.second;
    m_joinOneClock = false;
    if (!known || !m_joinBaseKnown)
        m_joinBaseKnown = false;
    else if (m_joinVideoStart != -1 && step > 0 && runOn > step / 2 && runOn < step + step / 2)
    {
        m_joinBase += m_joinVideoStart - m_joinFileDts.first;  // one clock
        m_joinOneClock = true;
    }
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
bool TSDemuxer::keepJoinSound(const int pid, const StreamType type, const int64_t pts, const uint8_t* data,
                              const uint8_t* dataEnd, const int64_t pesPayload)
{
    JoinSound& sound = m_joinSound[pid];
    m_joinSkip.erase(pid);
    if (sound.firstPts == -1)
    {
        if (m_curFileNum > 0 && sound.lastPes != -1 && m_joinVideoStart != -1 && m_joinBaseKnown)
        {
            int64_t due = pts - m_joinVideoStart + m_joinBase;
            // A PES at the time of the last one of the file before carries the rest of the same frame (the
            // extension of a DTS-HD frame, the TrueHD unit after an AC-3 frame), as where a recording was split
            // into files: kept with it, and the time of the next one decides. Only where both files are on one
            // clock: files of their own clocks whose sound runs one frame past the picture, as a 60 s film with
            // 60.032 s of AC-3, have the first frame of the next file due where the last frame of the file before
            // starts. That frame was kept as the rest of the one before, and every frame after it with it: the
            // sound of each later file came a frame later than its picture, and over 8 files 224 ms.
            if (due == sound.lastPes && sound.pesBytes >= 0 && m_joinOneClock)
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
            int64_t within = (std::max)(sound.step / 2, int64_t{90});
            // ** A PES OF SEVERAL FRAMES IS CUT AT THE FRAME. ** A PES was kept or left out whole, so where it holds
            // several frames, as AC-3 in the transport streams of some tools does (three, 96 ms), the sound of a
            // later file was up to half a PES off its picture: 32 ms early or late at alternate joints. Where the PES
            // begins with an AC-3 or E-AC-3 frame and holds a whole number of frames of that size, its frames are
            // taken one by one: those the sound before overlaps by more than half a frame are left out at the start
            // of the PES (m_joinSkip), and the next frame is due within half a frame.
            const bool ac3 = type == StreamType::AUDIO_AC3 || type == StreamType::AUDIO_EAC3 ||
                             type == StreamType::AUDIO_EAC3_SECONDARY || type == StreamType::AUDIO_EAC3_ATSC;
            const int frameBytes = ac3 && data != nullptr ? AC3Codec::frameSizeAt(data, dataEnd) : 0;
            const int64_t frames =
                frameBytes > 0 && pesPayload > 0 && pesPayload % frameBytes == 0 ? pesPayload / frameBytes : 0;
            if (frames >= 2 && sound.step > 0 && sound.step % frames == 0)
            {
                const int64_t frameLen = sound.step / frames;
                within = (std::max)(frameLen / 2, int64_t{90});
                int64_t skip = 0;
                while (skip < frames && due + skip * frameLen + within < sound.end) ++skip;
                if (skip == frames)
                    return false;
                if (skip > 0)
                {
                    m_joinSkip[pid] = skip * frameBytes;
                    due += skip * frameLen;
                }
            }
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
            sound.onTimeline = m_joinBase - m_joinVideoStart + sound.shift;
        }
        sound.firstPts = pts;
    }
    // ** AND A PAUSE INSIDE A FILE OF THE JOIN GOES ON AT ITS OWN TIME TOO. ** As for the sound of one file
    // (noteSoundGap): where the sound of a file pauses for more than 10 seconds while its picture goes on, the track's
    // times have a gap there. It was looked for in the first file of one only, so in a join the sound after such a
    // pause came straight after the sound before it, up to a minute early. The pause is put where the sound before it
    // ends on the timeline of the join: the reader of the track counts from the first frame of the file that has its
    // first sound, and from the start of each later file, where the joint put it (onTimeline).
    int64_t end = 0;
    if (const int64_t pause = soundPause(pid, pts, end); pause > 0)
    {
        if (sound.origin != -1 && sound.onTimeline == INT64_MIN)
            THROW(ERR_COMMON, "Track " << pid << " in file " << m_curFileNum + 1 << " of the join has no data for "
                                       << (pause + 45) / 90
                                       << " ms while the picture goes on, and where its sound after that comes "
                                          "cannot be told: a file of the join before it has no picture to put its "
                                          "sound against. Mux the files of the join one by one.")
        const int64_t at = sound.origin == -1 ? end - sound.firstPts : end + sound.onTimeline - sound.origin;
        m_joinGaps[pid].emplace_back(at * 100000 / 9, pause * 100000 / 9);
        LTRACE(LT_INFO, 2,
               "Track " << pid << " in file " << m_curFileNum + 1 << " of the join has no data from "
                        << floatToTime(static_cast<double>(at) / 90000.0) << " for " << (pause + 45) / 90
                        << " ms, while the picture goes on: its sound goes on after a gap, at its own time.");
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

// ** A SOUND TRACK THAT STOPS FOR A WHILE GOES ON AT ITS OWN TIME. ** The audio readers count the frames they are
// given, so where a track of one file has no packets for a while, as a commentary or a second language that drops
// out for a minute of a recording, its sound after that came straight after the sound before: a minute early against
// the picture, to its end. Where the next PES comes more than GAP_MIN after the one before while the first video goes
// on without a jump of its own time and is near the time the sound comes back at, the track's times have a gap there
// (takeTimeGaps), as at a joint of a join. A jump of the picture's time too is a discontinuity of the clock, which is
// left as it was, as are shorter gaps.
void TSDemuxer::noteSoundGap(const int pid, const int64_t pts, const bool cutPart)
{
    int64_t end = 0;
    if (const int64_t pause = soundPause(pid, pts, end); pause > 0)
    {
        // A join of files looks for it in each file (keepJoinSound). The sound of a later clip of a playlist, and of a
        // clip a playlist cuts, is not put against its picture: it follows on from the sound handed on before it, so
        // where the sound after the pause comes is not known. It came straight after the sound before the pause.
        if (m_curFileNum > 0 || cutPart)
            THROW(ERR_COMMON, "Track " << pid << " has no data for " << (pause + 45) / 90 << " ms in clip "
                                       << m_curFileNum + 1
                                       << " of the playlist while the picture goes on, and where its sound after "
                                          "that comes cannot be told in a playlist that plays a part of a clip: "
                                          "tsMuxeR puts such a pause at its own time in one file or in a join of "
                                          "whole files only. Mux the playlist with --playlist-clips=whole, or its "
                                          "clips as a join of their files.")
        const int64_t origin = m_soundGap[pid].origin;
        m_joinGaps[pid].emplace_back((end - origin) * 100000 / 9, pause * 100000 / 9);
        LTRACE(LT_INFO, 2,
               "Track " << pid << " has no data from " << floatToTime(static_cast<double>(end - origin) / 90000.0)
                        << " for " << (pause + 45) / 90
                        << " ms, while the picture goes on: its sound goes on after a gap, at its own time.");
    }
}

// The pause of the sound of a track that ends with its PES at pts, on the clock of the file: 0 for none. end: where
// the sound before it ends. A pause is longer than GAP_MIN, with the first video going on without a jump of its own
// time and near the time the sound comes back at.
int64_t TSDemuxer::soundPause(const int pid, const int64_t pts, int64_t& end)
{
    constexpr int64_t GAP_MIN = 10 * 90000;
    SoundGap& sound = m_soundGap[pid];
    int64_t pause = 0;
    if (sound.origin == -1)
    {
        sound.origin = sound.prev = pts;
        sound.videoJumps = m_videoJumps;
        return 0;
    }
    if (pts > sound.prev)
    {
        // and the sound comes back where the picture is: a jump of the sound's own times (a broken or rewritten
        // PES time, 6.6 hours in one test file) is not a gap
        if (sound.step > 0 && pts - sound.prev > GAP_MIN && m_firstVideoPid != -1 && sound.videoJumps == m_videoJumps &&
            std::llabs(pts - m_gapVideoPrev) < 5 * 90000)
        {
            end = sound.prev + sound.step;
            pause = pts - end;
        }
        else if (sound.step <= 0 || pts - sound.prev < sound.step)
            sound.step = pts - sound.prev;
        sound.prev = pts;
    }
    sound.videoJumps = m_videoJumps;
    return pause;
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

// A file of 192-byte packets under another name than .m2ts. Each grid is looked for from any of the first bytes, as a
// file cut at any byte starts inside a packet; it was looked for from the first byte only, and such a cut was read as
// 188-byte packets.
bool TSDemuxer::checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const
{
    auto gridFound = [buffer, end](const int step)
    {
        for (int first = 0; first < step && buffer + first < end; ++first)
        {
            const uint8_t* cur = buffer + first;
            while (cur < end && *cur == 0x47) cur += step;
            if (cur >= end)
                return true;
        }
        return false;
    };
    if (!gridFound(192) || gridFound(188))
        return false;
    LTRACE(LT_WARN, 2, "Warning! The file " << m_streamName << " has a M2TS format.");
    return true;
}

// The PAT and PMT of one block. For the first clip the PMT becomes the list of streams, as it always has. For a
// later clip of a join, a stream it declares is added when this demuxer delivers it to a track and does not know it
// yet. A playlist whose first clip is a short logo without audio declares the film's audio only from the next clip
// on, and a stream missing from the list keeps its PES headers in the data: the audio reader took each of them
// for a broken frame. A later clip only ever adds: a PMT that does not parse adds nothing, and a packet that stops
// the scan of the first clip ends the scan of a later one quietly, so no mux that ran before can stop here.
void TSDemuxer::readProgramTables(uint8_t* data, const uint8_t* lastFrameAddr, const bool laterClip,
                                  const bool firstBlock)
{
    uint8_t pmtBuffer[4096]{0};
    int pmtBufferLen = 0;
    bool hdrDiscarded = m_m2tsHdrDiscarded;
    bool firstPacket = firstBlock;
    TS_program_association_section pat;
    for (uint8_t* cur = data; cur <= lastFrameAddr; cur += TS_FRAME_SIZE)
    {
        if (!hdrDiscarded && m_m2tsMode)
            cur += 4;
        cur = findPacket(cur, lastFrameAddr, m_m2tsMode ? 192 : TS_FRAME_SIZE, firstPacket);
        if (cur > lastFrameAddr)
            break;
        hdrDiscarded = false;
        firstPacket = false;

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
        // A PID has 13 bits. A track number above 8191 in the meta wrote past the table, into whatever followed it.
        for (const int acceptedPID : acceptedPIDs)
            if (acceptedPID >= 0 && acceptedPID < 8192)
                m_acceptedPidCache[acceptedPID] = 1;
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
        // A track of LEAD_PES_MAX or fewer PES in the whole file, none of which begins with a frame, was still held
        // back when the file ended, and was lost: the search for a PES that begins with a frame ends here, and they
        // are given back as at any other end of it (giveBackLeadPes).
        if (!m_leadHeld.empty())
        {
            const ClipPart* part = m_curFileNum < m_clipParts.size() && m_clipParts[m_curFileNum].isCut()
                                       ? &m_clipParts[m_curFileNum]
                                       : nullptr;
            std::vector<int> pids;
            for (const auto& [pid, held] : m_leadHeld) pids.push_back(pid);
            for (const int pid : pids)
            {
                m_leadPes[pid] = LEAD_PES_DONE;
                m_leadDrop[pid] = false;
                giveBackLeadPes(pid, demuxedData, discardSize, part, vect, lastPid);
            }
            m_lastReadRez = readRez;
            return 0;
        }
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
        reportDamage();
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
        else
        {
            m_soundGap.clear();  // each clip of a playlist has its own clock (noteSoundGap)
            m_gapVideoPrev = -1;
        }
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
        for (auto& [pid, damage] : m_damage) damage.lastCounter = -1;  // each file counts on its own
    }

    discardSize = -handedOn;

    if (readedBytes < TS_FRAME_SIZE)
    {
        discardSize += readedBytes;
        if (readedBytes > 0 && m_endInsidePacket == 0)
            m_endInsidePacket = static_cast<int>(readedBytes);
        return 0;
    }

    const uint8_t* lastFrameAddr = data + readedBytes - TS_FRAME_SIZE;

    if (m_firstCall && !m_m2tsMode)
    {
        m_m2tsMode = checkForRealM2ts(data, lastFrameAddr);
        m_firstCall = false;
    }

    if (m_pmt.pidList.empty() || mvcContinueExpected())
        readProgramTables(data, lastFrameAddr, false, isFirstBlock);
    else if (isFirstBlock)
        readProgramTables(data, lastFrameAddr, true, true);
    if (isFirstBlock && m_pmtVersion != -1)
        m_pmtVersion = -2;  // each clip of a playlist has a program map of its own

    // A block holds data of one file only
    const ClipPart* part =
        m_curFileNum < m_clipParts.size() && m_clipParts[m_curFileNum].isCut() ? &m_clipParts[m_curFileNum] : nullptr;

    bool firstPacket = isFirstBlock;
    for (m_curPos = data; m_curPos <= lastFrameAddr; m_curPos += TS_FRAME_SIZE)
    {
        if (!m_m2tsHdrDiscarded && m_m2tsMode)
        {
            discardSize += 4;
            m_curPos += 4;
        }
        uint8_t* const found = findPacket(m_curPos, lastFrameAddr, m_m2tsMode ? 192 : TS_FRAME_SIZE, firstPacket);
        discardSize += found - m_curPos;
        m_noPacketBytes += found - m_curPos;
        m_curPos = found;
        if (m_curPos > lastFrameAddr)
        {
            m_m2tsHdrDiscarded = true;
            break;
        }
        m_m2tsHdrDiscarded = false;
        firstPacket = false;

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
        // ** A PACKET SENT TWICE IS READ ONCE. ** A stream may send a packet again right after it, with the same
        // continuity counter and the same payload (ISO/IEC 13818-1, 2.4.3.3), which a decoder discards. Both were read:
        // the payload of the PES came twice, and an AC-3 track whose every 30th packet was sent twice was no longer
        // recognised at all.
        if (tsPacket->dataExists && frameData < m_curPos + TS_FRAME_SIZE)
        {
            uint64_t hash = 14695981039346656037ULL;
            for (const uint8_t* b = frameData; b < m_curPos + TS_FRAME_SIZE; ++b) hash = (hash ^ *b) * 1099511628211ULL;
            const bool duplicate = m_lastCounter[pid] == tsPacket->counter && m_lastPayloadHash[pid] == hash;
            m_lastCounter[pid] = static_cast<uint8_t>(tsPacket->counter);
            m_lastPayloadHash[pid] = hash;
            if (duplicate)
                continue;
        }
        if (pid == m_pmtVersionPid && tsPacket->payloadStart)
            checkPmtVersion(frameData, m_curPos + TS_FRAME_SIZE);
        if (m_reportDamage && m_acceptedPidCache[pid])
            noteDamage(tsPacket, pid);
        demuxPacket(m_curPos, pid, demuxedData, discardSize, part, vect, lastPid);
    }
    if (m_curPos < data + readedBytes)
    {
        m_tmpBufferLen = data + readedBytes - m_curPos;
        memmove(m_tmpBuffer, m_curPos, m_tmpBufferLen);
    }

    return 0;
}

// One TS packet of a PID from its payload on: the PES header and its times, and the payload into demuxedData
void TSDemuxer::demuxPacket(uint8_t* pkt, const int pid, DemuxedData& demuxedData, int64_t& discardSize,
                            const ClipPart* part, MemoryBlock*& vect, int& lastPid)
{
    const auto tsPacket = reinterpret_cast<TSPacket*>(pkt);
    uint8_t* frameData = pkt + tsPacket->getHeaderSize();
    const bool pesStartCode = frameData[0] == 0 && frameData[1] == 0 && frameData[2] == 1 && tsPacket->payloadStart;
    if (part != nullptr && !pesStartCode && !m_partKeep[pid])
        return;
    if (!pesStartCode && m_leadDrop[pid])
    {
        m_leadHeld[pid].insert(m_leadHeld[pid].end(), pkt, pkt + TS_FRAME_SIZE);
        return;
    }
    if (!pesStartCode && m_joinDrop[pid])
        return;
    // ** A TRUEHD TRACK BEGINS WITH ITS FIRST PES. ** A file cut from a clip begins inside a PES, and the rest of it
    // was handed on in front of the first PES header. Its bytes have no time, and they are the end of an access unit
    // the cut has cut, so the readers could not count the access units from the start of the track to the time of the
    // first PES: the TrueHD of such a cut lost the time of the units it leaves out and came 30 ms early. Only for
    // TrueHD, whose readers count those units; a part of a playlist leaves out such bytes for every stream already.
    // The same for LPCM, each PES of which begins with its header of the samples' layout, and for DTS Express, whose
    // reader tells a stream without a core by the first bytes it is given: from the rest of a PES before the first
    // header neither was found, and a cut of a disc clip listed both as "Can't detect stream type".
    if (!pesStartCode && m_clipStartOfPid.find(pid) == m_clipStartOfPid.end())
        if (const auto info = m_pmt.pidList.find(pid);
            info != m_pmt.pidList.end() && (info->second.m_streamType == StreamType::AUDIO_TRUE_HD ||
                                            info->second.m_streamType == StreamType::AUDIO_LPCM ||
                                            info->second.m_streamType == StreamType::AUDIO_DTS_HD_SECONDARY))
            return;
    if (pesStartCode)
    {
        const auto pesPacket = reinterpret_cast<PESPacket*>(frameData);
        auto streamInfo = m_pmt.pidList.find(pid);
        if (part != nullptr &&
            !keptInPart(*part, pid,
                        streamInfo != m_pmt.pidList.end() ? streamInfo->second.m_streamType : StreamType::RESERVED,
                        pesPacket, TS_FRAME_SIZE - (frameData - pkt)))
            return;
        m_leadDrop[pid] = false;
        if (m_leadPes[pid] != LEAD_PES_DONE && streamInfo != m_pmt.pidList.end())
        {
            if ((pesPacket->flagsLo & 0x80) == 0x80)
            {
                const LeadPes lead = skipsLeadingPes(pid, streamInfo->second.m_streamType,
                                                     frameData + pesPacket->getHeaderLength(), pkt + TS_FRAME_SIZE);
                if (lead == LeadPes::Hold)
                {
                    if (m_leadHeld[pid].empty())
                        m_leadFirstSeen[pid] = m_firstSeenPTS == -1;
                    m_leadDrop[pid] = true;
                }
                else if (lead == LeadPes::GiveBack)
                    giveBackLeadPes(pid, demuxedData, discardSize, part, vect, lastPid);
                else
                    m_leadHeld.erase(pid);
            }
            else  // a PES without a time among the held ones stays with them
                m_leadDrop[pid] = m_leadHeld.find(pid) != m_leadHeld.end();
            if (m_leadDrop[pid])
            {
                m_leadHeld[pid].insert(m_leadHeld[pid].end(), pkt, pkt + TS_FRAME_SIZE);
                return;
            }
        }
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
                if (pid == m_firstVideoPid)
                {
                    // a jump of the picture's own time, forwards or back, is a discontinuity, not a gap
                    if (m_gapVideoPrev != -1 &&
                        (curDts - m_gapVideoPrev > 2 * 90000 || curDts < m_gapVideoPrev - 2 * 90000))
                        m_videoJumps++;
                    m_gapVideoPrev = curDts;
                    if (m_videoSpanDts.first == -1)
                        m_videoSpanDts.first = curDts;
                    m_videoSpanDts.second = (std::max)(m_videoSpanDts.second, curDts);
                }
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
            if (m_reportDamage && m_acceptedPidCache[pid])
                m_damagePts[pid] = curPts;

            if (m_acceptedPidCache[pid] && streamInfo != m_pmt.pidList.end() &&
                isAudioStreamType(streamInfo->second.m_streamType) && isJoinOfFiles())
                m_joinDrop[pid] =
                    !keepJoinSound(pid, streamInfo->second.m_streamType, curPts,
                                   frameData + pesPacket->getHeaderLength(), pkt + TS_FRAME_SIZE,
                                   (pesPacket->m_pesPacketLenHi << 8 | pesPacket->m_pesPacketLenLo) != 0
                                       ? (pesPacket->m_pesPacketLenHi << 8 | pesPacket->m_pesPacketLenLo) + 6 -
                                             pesPacket->getHeaderLength()
                                       : -1);
            else if (m_acceptedPidCache[pid] && streamInfo != m_pmt.pidList.end() &&
                     isAudioStreamType(streamInfo->second.m_streamType))
                noteSoundGap(pid, curPts, part != nullptr);
        }
        if (m_joinDrop[pid])
            return;

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
        return;

    const int64_t payloadLen = TS_FRAME_SIZE - (frameData - pkt);
    if (payloadLen > 0 && isDvbSubtitlePid(pid))
    {
        DvbSubtitlePes& sub = m_dvbSubtitles[pid];
        sub.pes.insert(sub.pes.end(), frameData, frameData + payloadLen);
        return;
    }
    if (payloadLen > 0)
    {
        if (pid != lastPid)
        {
            vect = &demuxedData[pid];
            lastPid = pid;
        }
        // the frames of a PES left out at a joint of a join (keepJoinSound) are not handed on
        int64_t skip = 0;
        if (m_joinSoundPid[pid])
            if (const auto joint = m_joinSkip.find(pid); joint != m_joinSkip.end())
            {
                skip = (std::min)(joint->second, payloadLen);
                if ((joint->second -= skip) == 0)
                    m_joinSkip.erase(joint);
            }
        if (vect != nullptr && payloadLen > skip)
        {
            vect->grow(payloadLen - skip);
            uint8_t* dst = vect->data() + vect->size() - (payloadLen - skip);
            memcpy(dst, frameData + skip, payloadLen - skip);
        }
        if (m_joinSoundPid[pid])
            m_joinSound[pid].pesBytes += payloadLen;
        discardSize -= payloadLen - skip;
        return;
    }
    discardSize -= payloadLen;
}

// ** THE PES LEFT OUT AT THE START ARE GIVEN BACK WHERE NO FRAME FOLLOWS. ** skipsLeadingPes leaves out the first PES
// of a sound stream up to the first that begins with a frame. Where none of the first LEAD_PES_MAX does, the PES of
// the stream do not begin with frames at all, as many broadcast streams are written, and the stream is to be read
// from where it starts, as before. But the PES left out were dropped, so such a stream lost its first four PES: on a
// broadcast AC-3 stream of one frame in each PES, 128 ms of sound, and the stream started 128 ms later than 2.20.0
// started it. They are now held back, packet by packet, and read here as they would have been read, times and all,
// before the PES that ended the search. A PES that begins with a frame within the limit still drops them, which is
// the case skipsLeadingPes is for. Where the first PES held back was the first PES of the file, its time is the
// first time of the file again (m_firstSeenPTS, which times the sound of a file without video).
void TSDemuxer::giveBackLeadPes(const int pid, DemuxedData& demuxedData, int64_t& discardSize, const ClipPart* part,
                                MemoryBlock*& vect, int& lastPid)
{
    const auto held = m_leadHeld.find(pid);
    if (held == m_leadHeld.end())
        return;
    std::vector<uint8_t> packets = std::move(held->second);
    m_leadHeld.erase(held);
    if (m_leadFirstSeen[pid])
        m_firstSeenPTS = -1;
    for (size_t i = 0; i + TS_FRAME_SIZE <= packets.size(); i += TS_FRAME_SIZE)
        demuxPacket(packets.data() + i, pid, demuxedData, discardSize, part, vect, lastPid);
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
    memset(m_leadPes, 0, sizeof(m_leadPes));
    memset(m_leadDrop, 0, sizeof(m_leadDrop));
    memset(m_leadFirstSeen, 0, sizeof(m_leadFirstSeen));
    m_leadHeld.clear();
    memset(m_lastCounter, 0xFF, sizeof(m_lastCounter));
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

TSDemuxer::~TSDemuxer()
{
    try
    {
        reportDamage();  // a mux that ends before the end of the file, as with --cut-end
    }
    catch (...)
    {
    }
    m_bufferedReader->deleteReader(m_readerID);
}

void TSDemuxer::noteDamage(const TSPacket* tsPacket, const int pid)
{
    if (!tsPacket->dataExists)
        return;  // the counter goes on with a payload only
    PidDamage& damage = m_damage[pid];
    // the time of the last PES of the track, from its first one: where the damage begins
    const auto at = [&]
    {
        const auto last = m_damagePts.find(pid);
        const auto first = m_firstPtsTime.find(pid);
        return last != m_damagePts.end() && first != m_firstPtsTime.end() ? last->second - first->second : -1;
    };
    damage.packets++;
    if (tsPacket->sc != 0 && damage.scrambled++ == 0)
        damage.scrambledAt = at();
    // A discontinuity the stream declares (discontinuity_indicator) starts the count again
    const bool declared =
        tsPacket->afExists && tsPacket->adaptiveField.length > 0 && tsPacket->adaptiveField.discontinuityIndicator;
    // The same counter again is a packet sent twice with another payload (one with the same payload is passed over
    // before this is called, simpleDemuxBlock): no packet is missing.
    if (damage.lastCounter >= 0 && !declared && static_cast<int>(tsPacket->counter) != damage.lastCounter)
    {
        const int lost = (static_cast<int>(tsPacket->counter) - damage.lastCounter - 1) & 15;
        if (lost > 0 && damage.lost == 0)
            damage.lostAt = at();
        damage.lost += lost;
    }
    damage.lastCounter = static_cast<int>(tsPacket->counter);
}

void TSDemuxer::reportDamage()
{
    if (!m_reportDamage || m_damageReported)
        return;
    m_damageReported = true;
    const auto where = [](const int64_t at)
    { return at < 0 ? std::string() : ", the first at " + floatToTime(static_cast<double>(at) / 90000.0, '.'); };
    // A rest of a packet at the start of a file cut at any byte is passed over without a word; more than two packets
    // of no packet at all are a damaged part, which a TS cut off or overwritten with zeros lost without a word
    if (m_noPacketBytes > 2 * 192)
        LTRACE(LT_WARN, 2,
               "Warning: " << m_noPacketBytes << " bytes of \"" << m_streamName
                           << "\" are not TS packets and were passed over, as in a damaged part of the file or one "
                              "overwritten with zeros. What the tracks held there is lost, and the frames around it "
                              "may be damaged.");
    if (m_endInsidePacket > 0)
        LTRACE(LT_WARN, 2,
               "Warning: \"" << m_streamName << "\" ends " << m_endInsidePacket
                             << " bytes into a TS packet: the file was cut off, so the last frames of its tracks may "
                                "be damaged.");
    for (const auto& [pid, damage] : m_damage)
    {
        if (damage.packets > 0 && damage.scrambled == damage.packets)
            THROW(ERR_COMMON, "Track " << pid << " of \"" << m_streamName << "\" is scrambled: all " << damage.packets
                                       << " of its packets are marked so (transport_scrambling_control), and what "
                                          "they hold cannot be read. A recording of an encrypted channel needs to be "
                                          "decrypted before muxing; or leave the track out.")
        if (damage.scrambled > 0)
            LTRACE(LT_WARN, 2,
                   "Warning: " << damage.scrambled << " of the " << damage.packets << " packets of track " << pid
                               << " of \"" << m_streamName << "\""
                               << " are marked scrambled (transport_scrambling_control)" << where(damage.scrambledAt)
                               << ". What they hold cannot be read, and the frames they belong to are damaged.");
        const auto info = m_pmt.pidList.find(pid);
        if (damage.lost > 0 && info != m_pmt.pidList.end() && isVideoPID(info->second.m_streamType))
            LTRACE(LT_WARN, 2,
                   "Warning: at least " << damage.lost << " packets of track " << pid << " of \"" << m_streamName
                                        << "\" are missing: its continuity counter skips" << where(damage.lostAt)
                                        << ". The pictures they belong to are damaged.");
    }
}

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
//
// ** H.264 CODED AS FIELDS (PAFF). ** Each field has a first slice of its own. A PES that carried a field pair counted
// two pictures and gave no rate, and a PES for each field gave the field rate, which the readers refuse for an
// interlaced stream: both were muxed at 25, and a 29.97 disc clip written without timing played 17 % slow (600 frames
// over 23.96 s, not 19.99 s). So for H.264 the head of each first slice is read, with the SPS and PPS met before it,
// and the slice is a frame, a first field, or the second field of a pair: of the other parity, directly after a field
// that has no pair yet, with its frame_num, and no IDR (the second field of an IDR picture is not one). A reference
// field and a non-reference field are paired as well. A second field is no new picture, and a PES whose first
// picture start is one gives no time, as its time is that of a field. A first slice that cannot be read so, as
// before the first SPS of a cut stream, gives its PES no time either: a wrong reading would give a field stream the
// field rate and play it twice as fast, so where nothing can be told nothing is said. The NAL units are gathered
// across the TS packets of their PES, so a parameter set or slice header split between packets is read whole.
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
        m_first = Kind::NONE;
        m_unknown = false;
        m_last = 0xffffffff;
        m_have = -1;
    }
    // The PES ends: an H.264 NAL unit still being gathered was its last one
    void endPes()
    {
        if (m_type == StreamType::VIDEO_H264)
            endNal();
    }
    void feed(const uint8_t* p, const uint8_t* end)
    {
        if (m_need == 0)
            return;
        if (m_type == StreamType::VIDEO_H264)
        {
            feedH264(p, end);
            return;
        }
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
    // The pictures that begin in the PES; for H.264 the frames and first fields
    [[nodiscard]] int inPes() const { return m_inPes; }
    // Whether the time in the header of the PES is the time of a picture. A PES in which no picture starts keeps its
    // time, as before.
    [[nodiscard]] bool givesTime() const { return !m_unknown && m_first != Kind::SECOND_FIELD; }

   private:
    enum class Kind
    {
        NONE,
        FRAME,
        FIRST_FIELD,
        SECOND_FIELD
    };
    // What is gathered of an H.264 NAL unit: an SPS whole, of a PPS its two ids, of a slice its header as far as
    // bottom_field_flag (at most about 11 bytes, with room for emulation prevention bytes)
    static constexpr size_t SPS_BYTES = 4096;
    static constexpr size_t PPS_BYTES = 16;
    static constexpr size_t SLICE_BYTES = 48;
    static constexpr int NO_NAL = -1;
    static constexpr int NAL_HEADER_NEXT = -2;

    void feedH264(const uint8_t* p, const uint8_t* end)
    {
        for (; p < end; ++p)
        {
            m_last = m_last << 8 | *p;
            if ((m_last & 0xffffff) == 1)
            {
                // the zero bytes of the start code are not part of the NAL unit before it
                while (!m_nal.empty() && m_nal.back() == 0) m_nal.pop_back();
                endNal();
                m_nalType = NAL_HEADER_NEXT;
                continue;
            }
            if (m_nalType == NAL_HEADER_NEXT)
            {
                const int type = *p & 0x1f;
                m_nalBytes = type == 7 ? SPS_BYTES : type == 8 ? PPS_BYTES : type == 1 || type == 5 ? SLICE_BYTES : 0;
                m_nalType = m_nalBytes > 0 ? type : NO_NAL;
            }
            if (m_nalType >= 0)
            {
                m_nal.push_back(*p);
                if (m_nal.size() >= m_nalBytes)
                    endNal();  // all that is read of it is here
            }
        }
    }
    void endNal()
    {
        const int type = m_nalType;
        m_nalType = NO_NAL;
        if (type == 7)
            readSps();
        else if (type == 8)
            readPps();
        else if (type == 1 || type == 5)
            readSlice(type == 5);
        m_nal.clear();
    }
    void readSps()
    {
        auto sps = std::make_unique<SPSUnit>();
        sps->decodeBuffer(m_nal.data(), m_nal.data() + m_nal.size());
        if (sps->deserialize() != 0)
        {
            m_sps.clear();  // which one it was cannot be told: none is trusted until one is read again
            return;
        }
        const unsigned id = sps->seq_parameter_set_id;
        m_sps[id] = std::move(sps);
    }
    void readPps()
    {
        uint8_t rbsp[PPS_BYTES + 8]{};
        const int len = NALUnit::decodeNAL(m_nal.data() + 1, m_nal.data() + m_nal.size(), rbsp, sizeof rbsp);
        try
        {
            BitStreamReader reader{};
            reader.setBuffer(rbsp, rbsp + (std::max)(len, 0));
            const unsigned ppsId = NALUnit::extractUEGolombCode(reader);
            const unsigned spsId = NALUnit::extractUEGolombCode(reader);
            if (ppsId < 256 && spsId < 32)
            {
                m_pps[ppsId] = spsId;
                return;
            }
        }
        catch (BitStreamException&)
        {
        }
        m_pps.clear();  // as for an SPS that cannot be read
    }
    void readSlice(const bool idr)
    {
        uint8_t rbsp[SLICE_BYTES + 8]{};
        const int len = NALUnit::decodeNAL(m_nal.data() + 1, m_nal.data() + m_nal.size(), rbsp, sizeof rbsp);
        SliceUnit::FieldHeader head;
        const bool read = len > 0 && SliceUnit::readFieldHeader(
                                         rbsp, rbsp + len,
                                         [this](const unsigned ppsId) -> const SPSUnit*
                                         {
                                             const auto pps = m_pps.find(ppsId);
                                             if (pps == m_pps.end())
                                                 return nullptr;
                                             const auto sps = m_sps.find(pps->second);
                                             return sps == m_sps.end() ? nullptr : sps->second.get();
                                         },
                                         head);
        if (read && head.firstMb != 0)
            return;  // a further slice of its picture
        if (!read)
        {
            // a first slice, or one too short to tell, with no SPS or PPS for it yet, or damaged
            m_unknown = true;
            m_unpaired = false;
            return;
        }
        Kind kind;
        if (!head.field)
        {
            kind = Kind::FRAME;
            m_unpaired = false;
        }
        else if (m_unpaired && head.bottom != m_unpairedBottom && head.frameNum == m_unpairedFrameNum && !idr)
        {
            kind = Kind::SECOND_FIELD;
            m_unpaired = false;
        }
        else
        {
            kind = Kind::FIRST_FIELD;
            m_unpaired = true;
            m_unpairedBottom = head.bottom;
            m_unpairedFrameNum = head.frameNum;
        }
        if (kind != Kind::SECOND_FIELD)
            ++m_inPes;
        if (m_first == Kind::NONE)
            m_first = kind;
    }
    [[nodiscard]] bool isPictureStart() const
    {
        const uint8_t* h = m_header;
        switch (m_type)
        {
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
    // H.264
    Kind m_first = Kind::NONE;  // the first picture start of the PES
    bool m_unknown = false;     // a slice in the PES that may begin a picture and could not be read
    std::vector<uint8_t> m_nal;
    int m_nalType = NO_NAL;
    size_t m_nalBytes = 0;
    std::map<unsigned, std::unique_ptr<SPSUnit>> m_sps;
    std::map<unsigned, unsigned> m_pps;  // pic_parameter_set_id: seq_parameter_set_id
    bool m_unpaired = false;             // the last picture start was a field that has no pair yet
    bool m_unpairedBottom = false;
    unsigned m_unpairedFrameNum = 0;
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

// pictures: the counter of the track, which goes on from one file of a join or a playlist to the next, as the stream
// does: a later file need not repeat the parameter sets, and its pictures are read with those met before.
PesTimes readPesTimes(const std::string& name, const uint32_t pid, PictureStarts& pictures, const size_t maxTimes,
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
    bool inPes = false;
    int64_t pesTime = 0;  // the time in the header of the PES in progress
    int stride = 0;       // 188, or 192 for an M2TS: found from the first packets
    size_t have = 0;      // bytes in buf
    // The PES in progress when the window is full is counted too: its pictures so far are all it can hold. Its time is
    // taken when it ends, as only then is it known whether it is the time of a picture (see PictureStarts).
    const auto endPes = [&]
    {
        pictures.endPes();
        if (!inPes)
            return;
        inPes = false;
        if (pictures.inPes() > 1)
            r.manyPictures = true;
        else if (pictures.givesTime())
        {
            if (!r.times.empty() && pesTime < r.times.front() - (1ll << 32))
                pesTime += 1ll << 33;  // the 33-bit clock wrapped
            r.times.push_back(pesTime);
        }
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
            if (r.manyPictures || r.times.size() >= maxTimes)
                break;
            if (!(pes[7] & 0x80))
            {
                r.withoutTime = true;
                break;
            }
            pesTime = static_cast<int64_t>(pes[9] & 0x0e) << 29 | static_cast<int64_t>(pes[10]) << 22 |
                      static_cast<int64_t>(pes[11] & 0xfe) << 14 | static_cast<int64_t>(pes[12]) << 7 | pes[13] >> 1;
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
// A stream that may code fields gets a rate only from an H.264 track, whose field pairs are counted as one picture
// (PictureStarts); for every other codec the times of its PES may be those of fields, and it keeps the default.
double TSDemuxer::getTrackPacketFps(const uint32_t trackId, const bool fieldCoded)
{
    const auto info = m_pmt.pidList.find(static_cast<int>(trackId));
    const StreamType type = info != m_pmt.pidList.end() ? info->second.m_streamType : StreamType::RESERVED;
    if (fieldCoded && type != StreamType::VIDEO_H264)
        return 0.0;
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

    static constexpr size_t PICTURES = 240;
    PictureStarts pictures(type);
    std::vector<int64_t> steps;
    size_t timed = 0;
    int64_t read = 0;
    for (const std::string& name : files)
    {
        if (timed >= PICTURES || read >= DETECT_STREAM_BUFFER_SIZE)
            break;
        PesTimes clip = readPesTimes(name, trackId, pictures, PICTURES - timed, DETECT_STREAM_BUFFER_SIZE - read);
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
