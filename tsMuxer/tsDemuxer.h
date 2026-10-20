#ifndef TS_DEMUXER_H
#define TS_DEMUXER_H

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aac.h"
#include "abstractDemuxer.h"
#include "bufferedReader.h"
#include "bufferedReaderManager.h"
#include "ioContextDemuxer.h"
#include "pesPacket.h"
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
    // Where a stream starts against the video, in 90 kHz ticks, for the mux (METADemuxer::addStream): its first PTS
    // less the smallest PTS of the video read so far, or with fromFirstPes the PTS of the first video PES, across a
    // wrap of the 33 bit clock. False while no PES of the stream or of the video has been read.
    [[nodiscard]] bool getTrackStart(int32_t pid, bool fromFirstPes, int64_t& ticks) const;
    // The program map lists a video stream
    [[nodiscard]] bool hasVideoStream() const;
    // The video PES read, of all video streams and files, and the stream of the first one (-1: none yet)
    [[nodiscard]] int64_t videoPesRead() const { return m_videoPesRead; }
    [[nodiscard]] int firstVideoPid() const { return m_firstVideoPid; }
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
    bool isPartStartPicture(const int32_t pid, const int64_t picture) override
    {
        const auto starts = m_partStartPictures.find(pid);
        return starts != m_partStartPictures.end() && starts->second.count(picture) != 0;
    }
    // The clips that follow the one opened, when a playlist is listed: read by getTrackPacketFps when the first clip
    // holds too few pictures. A join or a playlist that is muxed gives them through setFileIterator.
    void setLaterFiles(const std::vector<std::string>& files) { m_laterFiles = files; }
    void setMPLSInfo(const std::vector<MPLSPlayItem>& mplsInfo) { m_mplsInfo = mplsInfo; }
    [[nodiscard]] int64_t getFileDurationNano() const override;
    // The latest PES time read, on the PGS timeline the PES headers are rewritten to (see AbstractDemuxer)
    [[nodiscard]] int64_t getDemuxedTime() const override
    {
        if (m_lastPTS == -1)
            return -1;
        const int64_t ptsBase = m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS;
        return (m_lastPTS - ptsBase + m_prevFileLen) * INT_FREQ_TO_TS_FREQ;
    }

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

    // A playlist of which only the part each play item plays is muxed: one ClipPart for each clip file, and what is
    // kept of each stream in the part being read (see keptInPart)
    struct PartCut
    {
        bool keep = false;         // the PES being read is kept, and the packets that carry the rest of it
        bool ended = false;        // video: the key picture the part ends at has been reached
        bool keptAny = false;      // something of the stream is kept in this part
        int64_t lastSeenPts = -1;  // sound: the time of the PES before, kept or not
        int64_t step = 0;          // sound: the time from one PES to the next
        int64_t firstKeptPts = -1;
        int64_t lastKeptPts = -1;
        int64_t outPos = -1;       // sound: where the last PES kept is in the output, on the timeline of the join
        int64_t outEnd = -1;       // sound: where the sound of the part before ends in the output; -1: not known
        bool pgsShowing = false;   // a subtitle is on the screen
        bool pgsClearing = false;  // the display set that takes it off at the end of the part is being kept
        bool pgsCleared = false;
        uint8_t pgsVideo[7]{};           // size, rate and composition number of the last composition kept
        std::vector<uint8_t> pgsWindow;  // the last window definition segment kept
    };
    std::vector<ClipPart> m_clipParts;
    std::map<int, PartCut> m_partCut;
    uint8_t m_partKeep[8192]{};
    int64_t m_partVideoStep = -1;                          // the smallest step of the decoding times of the video kept
    int64_t m_partLastVideoDts = -1;                       // the decoding time of the last picture kept
    std::map<int, int64_t> m_keptPictures;                 // the pictures of each video track kept so far
    std::map<int, std::set<int64_t>> m_partStartPictures;  // their numbers where a part begins (isPartStartPicture)
    void startPart();
    int64_t clearSubtitlesAtPartEnd(DemuxedData& demuxedData);
    bool keptInPart(const ClipPart& part, int pid, StreamType type, PESPacket* pes, int64_t payloadLen);

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
    std::vector<std::string> m_joinFiles;    // the files of a join or a playlist, the first one included
    std::vector<std::string> m_laterFiles;   // see setLaterFiles
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
    int64_t m_videoPesRead = 0;   // see videoPesRead
    int m_firstVideoPid = -1;     // see firstVideoPid
    int64_t m_lastVideoPTS;
    int64_t m_lastVideoDTS;
    int64_t m_videoDtsGap;
    std::vector<MPLSPlayItem> m_mplsInfo;
    int64_t m_lastPCRVal;
    bool m_nonMVCVideoFound;

    // cache to improve speed
    uint8_t m_acceptedPidCache[8192];
    bool m_firstDemuxCall;

    // The times of the file on one line: a PES time is 33 bits and goes round to 0 every 26.5 hours, as it can do in
    // the middle of a long recording. The offset added to the times since the wrap, and the last full time.
    int64_t m_ptsWrapOffset = 0;
    int64_t m_lastFullPts = -1;
    int64_t fullPts(int64_t raw);

    // The version of the program map the tracks were taken from (-1: none yet, -2: the next one seen, as at the start
    // of a clip), the PID it comes on, and whether a change was said.
    int m_pmtVersion = -1;
    int m_pmtVersionPid = -1;
    bool m_pmtChangeReported = false;
    void checkPmtVersion(uint8_t* payload, const uint8_t* end);

    static bool isVideoPID(StreamType streamType);
    bool checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const;
};

#endif
