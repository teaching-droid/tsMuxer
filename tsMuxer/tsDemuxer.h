#ifndef TS_DEMUXER_H
#define TS_DEMUXER_H

#include <cmath>
#include <map>
#include <set>
#include <string>

#include "aac.h"
#include "abstractDemuxer.h"
#include "bufferedReader.h"
#include "bufferedReaderManager.h"
#include "ioContextDemuxer.h"
#include "tsPacket.h"

// typedef StreamReaderMap std::map<int, AbstractStreamReader*>;
class TSDemuxer final : public AbstractDemuxer
{
   public:
    TSDemuxer(const BufferedReaderManager& readManager, const char* streamName);
    ~TSDemuxer() override;
    void openFile(const std::string& streamName) override;
    void readClose() override;
    int64_t getDemuxedSize() override;
    int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo>& trackList) override;
    int getLastReadRez() override { return m_lastReadRez; }
    void setFileIterator(FileNameIterator* itr) override;
    int64_t getTrackDelay(const int32_t pid) override
    {
        // Track delay must be measured against the FIRST timestamp seen for each PID, not the
        // smallest one. m_firstVideoPTS / m_firstPtsTime are running minima (they are used
        // elsewhere to measure the presented span of a clip), so on an open-GOP stream whose
        // leading B pictures are presented before the first coded picture they report the
        // display start instead of the stream's anchor. The muxer anchors every track on its
        // first coded unit, so the delay written here has to use the same reference or the
        // reorder lead-in is silently dropped on demux/remux.
        //
        // Found on AVCHD .MTS clips from a Sony ZV-1, which writes its audio 80 ms ahead of the
        // video and opens with two leading B pictures: the minimum video PTS then equals the
        // audio's first PTS, the delay computes as 0, and a zero is not reported at all, so the
        // offset was silently lost on a demux and remux. Clips from a Panasonic Lumix DMC-TZ110
        // do not reproduce it, because their first coded picture is also their first displayed
        // one. Nothing here is specific to AVCHD; that is only where it was noticed.
        if (m_firstPtsTime.find(pid) != m_firstPtsTime.end())
        {
            const int64_t clockTicks =
                m_firstPtsTime[pid] - (m_firstSeenVideoPTS != -1 ? m_firstSeenVideoPTS : m_firstSeenPTS);
            return llround(static_cast<double>(clockTicks) / 90.0);  // convert to ms
        }

        return 0;
    }
    // Where a stream starts on the timeline of a join, in 90 kHz: the start of the clip in which its first PES was
    // seen, so 0 for every stream of the first clip. An audio or video reader counts its own time from its first
    // frame and needs this added; PGS times already carry it in their PES headers.
    int64_t getTrackClipStart(const int32_t pid) override
    {
        const auto info = m_pmt.pidList.find(pid);
        const auto start = m_clipStartOfPid.find(pid);
        if (info == m_pmt.pidList.end() || info->second.m_streamType == StreamType::SUB_PGS ||
            start == m_clipStartOfPid.end())
            return 0;
        return start->second;
    }
    double getTrackPacketFps(uint32_t trackId) override;
    void setMPLSInfo(const std::vector<MPLSPlayItem>& mplsInfo) { m_mplsInfo = mplsInfo; }
    [[nodiscard]] int64_t getFileDurationNano() const override;

   private:
    [[nodiscard]] bool mvcContinueExpected() const;

    // DVB subtitles (stream type 0x06 with a subtitling descriptor), which tsMuxeR writes as PGS: each PES is gathered,
    // and when the next one begins, its pictures are drawn as PGS display sets, handed on as the PID's data.
    struct DvbSubtitlePes
    {
        std::vector<uint8_t> pes;  // the payload of the PES being gathered
        int64_t pts = -1;          // its time, internal clock, on the timeline the PGS of a TS is on
        std::shared_ptr<ParsedTrackPrivData> converter;
    };
    std::map<int, DvbSubtitlePes> m_dvbSubtitles;
    bool m_dvbSubtitlesFinished = false;
    [[nodiscard]] bool isDvbSubtitlePid(int pid) const;
    // The PGS handed on is read by the PGS reader and counted as read there; the DVB bytes it came from count as
    // discarded, less what was handed on, so that the two add up to the file.
    void convertDvbSubtitlePes(int pid, DvbSubtitlePes& sub, DemuxedData& demuxedData, bool last, int64_t& discardSize);
    void readProgramTables(uint8_t* data, const uint8_t* lastFrameAddr, bool laterClip);

    int64_t m_firstPCRTime;
    bool m_m2tsHdrDiscarded;
    int m_lastReadRez;
    bool m_m2tsMode;
    int m_scale;
    int m_nptPos;
    const BufferedReaderManager& m_readManager;
    std::string m_streamName;
    std::string m_streamNameLow;
    std::map<int, int64_t> m_lastPesPts;
    std::map<int, int64_t> m_lastPesDts;
    std::map<int, int64_t> m_firstPtsTime;  // first PTS per PID, in file order (never a minimum)
    std::map<int, int64_t> m_firstDtsTime;
    std::map<int, int64_t> m_clipStartOfPid;  // see getTrackClipStart
    std::map<int, int64_t> m_fullPtsTime;
    std::map<int, int64_t> m_fullDtsTime;
    std::map<uint32_t, double> m_packetFps;  // getTrackPacketFps, once per track
    AbstractReader* m_bufferedReader;
    int m_readerID;
    uint8_t* m_curPos;
    int m_pmtPid;
    bool m_codecReady;
    bool m_firstCall;
    int64_t m_readCnt;
    int64_t m_dataProcessed;
    bool m_notificated;
    TS_program_map_section m_pmt;
    uint8_t m_tmpBuffer[TS_FRAME_SIZE + 4];
    int64_t m_tmpBufferLen;
    // int64_t m_firstDTS;
    int64_t m_firstPTS;
    int64_t m_firstSeenPTS;  // first PTS seen on any PID, in file order (never a minimum)
    int64_t m_lastPTS;
    int64_t m_prevFileLen;
    uint32_t m_curFileNum;
    int64_t m_firstVideoPTS;
    int64_t m_firstSeenVideoPTS;  // first PTS seen on a video PID, in file order (never a minimum)
    int64_t m_lastVideoPTS;
    int64_t m_lastVideoDTS;
    int64_t m_videoDtsGap;
    std::vector<MPLSPlayItem> m_mplsInfo;
    int64_t m_lastPCRVal;
    bool m_nonMVCVideoFound;

    // cache to improve speed
    uint8_t m_acceptedPidCache[8192];
    bool m_firstDemuxCall;

    static bool isVideoPID(StreamType streamType);
    bool checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const;
};

#endif
