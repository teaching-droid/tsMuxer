#include "tsDemuxer.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "abstractStreamReader.h"
#include "bitmapSubtitles.h"
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

    if (isFirstBlock)
    {
        if (m_curFileNum < m_mplsInfo.size())
            m_prevFileLen +=
                static_cast<int64_t>(m_mplsInfo[m_curFileNum].OUT_time - m_mplsInfo[m_curFileNum].IN_time) *
                2;  // in 90Khz clock
        else
        {
            if (m_firstVideoPTS != -1 && m_lastVideoPTS != -1)
                m_prevFileLen += (m_lastVideoPTS - m_firstVideoPTS + m_videoDtsGap);
            else  // no video file
                m_prevFileLen += (m_lastPTS - m_firstPTS);
        }
        m_firstPTS = -1;
        m_firstSeenPTS = -1;
        m_lastPTS = -1;
        m_firstVideoPTS = -1;
        m_firstSeenVideoPTS = -1;
        m_lastVideoPTS = -1;
        m_curFileNum++;
    }

    discardSize = 0;

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
        const bool pesStartCode = frameData[0] == 0 && frameData[1] == 0 && frameData[2] == 1 && tsPacket->payloadStart;
        if (pesStartCode)
        {
            m_clipStartOfPid.try_emplace(pid, m_prevFileLen);
            const auto pesPacket = reinterpret_cast<PESPacket*>(frameData);
            auto streamInfo = m_pmt.pidList.find(pid);

            if ((pesPacket->flagsLo & 0x80) == 0x80)
            {
                const int64_t curPts = pesPacket->getPts();
                int64_t curDts = curPts;

                if ((pesPacket->flagsLo & 0xc0) == 0xc0)
                    curDts = pesPacket->getDts();

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
                    if (curPts > m_lastVideoPTS)
                        m_lastVideoPTS = curPts;
                    if (m_lastVideoDTS == -1)
                        m_lastVideoDTS = curDts;
                    if (m_videoDtsGap == -1 && curDts > m_lastVideoDTS)
                        m_videoDtsGap = curDts - m_lastVideoDTS;
                }

                if (m_firstPtsTime.find(pid) == m_firstPtsTime.end())
                    m_firstPtsTime[pid] = curPts;
            }

            if (m_acceptedPidCache[pid] && isDvbSubtitlePid(pid))
            {
                DvbSubtitlePes& sub = m_dvbSubtitles[pid];
                convertDvbSubtitlePes(pid, sub, demuxedData, false, discardSize);
                if ((pesPacket->flagsLo & 0x80) == 0x80)
                {
                    // on the PGS timeline: from the first video picture, joined files one after another
                    const int64_t ptsBase = m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS;
                    sub.pts = (pesPacket->getPts() - ptsBase + m_prevFileLen) * INTERNAL_PTS_FREQ / 90000;
                }
                else
                    sub.pts = -1;
            }
            if (streamInfo != m_pmt.pidList.end() &&
                streamInfo->second.m_streamType != StreamType::SUB_PGS)  // demux PGS with PES headers
                frameData += pesPacket->getHeaderLength();
            else
            {
                const int64_t ptsBase = m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS;
                if ((pesPacket->flagsLo & 0xc0) == 0xc0)
                {
                    const int64_t pts = pesPacket->getPts() - ptsBase + m_prevFileLen;
                    const int64_t dts = pesPacket->getDts() - ptsBase + m_prevFileLen;
                    pesPacket->setPtsAndDts(pts, dts);
                }
                else if ((pesPacket->flagsLo & 0x80) == 0x80)
                {
                    const int64_t pts = pesPacket->getPts() - ptsBase + m_prevFileLen;
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

// A video stream may name no frame rate of its own: HEVC, H.264 or VVC without timing information, AV1 without a timing
// info, which an encoder is free to leave out. In a transport stream the PES header of each picture still carries its
// time, so the rate is the spacing of those times. It was taken to be 25, and a 23.976 film played 4 % fast. The times
// of the track's first pictures are read on a handle of their own, sorted, since pictures come in decoding order, and
// the usual step between them gives the rate: a picture still to come leaves a hole in the order shown, and a later
// clip of a join a jump, and neither is a step. Nothing is said when a PES start in the window carries no time (H.222.0
// asks for one only every 0.7 s), or when the steps vary.
double TSDemuxer::getTrackPacketFps(const uint32_t trackId)
{
    if (const auto known = m_packetFps.find(trackId); known != m_packetFps.end())
        return known->second;
    m_packetFps[trackId] = 0.0;
    File f;
    if (trackId == 0 || trackId >= 0x1fff || m_streamName.empty() ||
        !f.open(unquoteStr(m_streamName).c_str(), File::ofRead))
        return 0.0;

    static constexpr size_t PICTURES = 240;
    static constexpr int CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK + 2 * 192);
    std::vector<int64_t> times;
    int stride = 0;    // 188, or 192 for an M2TS: found from the first packets
    size_t have = 0;   // bytes in buf
    int64_t read = 0;  // bytes read from the file
    bool withoutTime = false;
    while (times.size() < PICTURES && !withoutTime && read < DETECT_STREAM_BUFFER_SIZE)
    {
        const int len = f.read(buf.data() + have, CHUNK);
        if (len <= 0)
            break;
        read += len;
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
                return 0.0;
        }
        else
            pos = stride - 188;
        while (pos + stride <= have && times.size() < PICTURES)
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
            const uint32_t pid = (pk[1] & 0x1f) << 8 | pk[2];
            const int afc = pk[3] >> 4 & 3;
            if (pid != trackId || !(pk[1] & 0x40) || !(afc & 1))
                continue;
            const int off = afc & 2 ? 5 + pk[4] : 4;
            if (off + 14 > 188)
                continue;
            const uint8_t* pes = pk + off;
            if (pes[0] != 0 || pes[1] != 0 || pes[2] != 1)
                continue;
            if (!(pes[7] & 0x80))
            {
                withoutTime = true;
                break;
            }
            int64_t pts = static_cast<int64_t>(pes[9] & 0x0e) << 29 | static_cast<int64_t>(pes[10]) << 22 |
                          static_cast<int64_t>(pes[11] & 0xfe) << 14 | static_cast<int64_t>(pes[12]) << 7 |
                          pes[13] >> 1;
            if (!times.empty() && pts < times.front() - (1ll << 32))
                pts += 1ll << 33;  // the 33-bit clock wrapped
            times.push_back(pts);
        }
        // The packet not yet complete goes to the front for the next read, an M2TS one with its 4-byte header, so
        // that its sync byte is again where the next round starts.
        const size_t from = pos - (stride - 188);
        const size_t keep = from < have ? have - from : 0;
        if (keep)
            memmove(buf.data(), buf.data() + from, keep);
        have = keep;
    }
    if (withoutTime)
        return 0.0;
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() < 8)
        return 0.0;
    std::vector<int64_t> steps;
    for (size_t i = 1; i < times.size(); ++i) steps.push_back(times[i] - times[i - 1]);
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
    double fps = 90000.0 * static_cast<double>(count) / static_cast<double>(sum);
    for (const double standard : {12.5, 15000.0 / 1001, 15.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 30.0,
                                  48000.0 / 1001, 48.0, 50.0, 60000.0 / 1001, 60.0, 100.0, 120000.0 / 1001, 120.0})
        if (std::fabs(fps - standard) < standard * 0.0004)
            fps = standard;
    if (fps < 1.0 || fps > 300.0)
        return 0.0;
    m_packetFps[trackId] = fps;
    return fps;
}
