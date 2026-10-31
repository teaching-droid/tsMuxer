#ifndef TS_DEMUXER_H
#define TS_DEMUXER_H

#include <cmath>
#include <cstdint>
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
    // The demuxer of a mux (not of a listing or a probe) says at the end of the file what of its tracks was damaged
    void setReportDamage() { m_reportDamage = true; }
    void setFileIterator(FileNameIterator* itr) override;
    int64_t getTrackDelay(const int32_t pid) override
    {
        // The delay of a stream is measured from the FIRST timestamp seen for it, not its smallest one. Of the sound
        // it is measured against the picture the video reader shows first, as the mux applies it (getTrackStart):
        // the readers of H.264, MPEG-2 and VC-1 show first the pictures that follow the first coded picture of an open
        // GOP at the start and are shown before it, so that is the smallest PTS of the video. Against the first coded
        // picture, as it was, the sound of such a cut came that much early, 40 ms for H.264 behind one B picture. A
        // video stream is measured against the first video PES, as before, so the video itself has no delay.
        const auto stream = m_pmt.pidList.find(pid);
        int64_t clockTicks = 0;
        if (m_firstVideoPTS != -1 && stream != m_pmt.pidList.end() && !isVideoPID(stream->second.m_streamType) &&
            getTrackStart(pid, m_videoTimedFromFirstPes, clockTicks))
            return llround(static_cast<double>(clockTicks) / 90.0);  // convert to ms
        if (m_firstPtsTime.find(pid) != m_firstPtsTime.end())
        {
            clockTicks = m_firstPtsTime[pid] - (m_firstSeenVideoPTS != -1 ? m_firstSeenVideoPTS : m_firstSeenPTS);
            return llround(static_cast<double>(clockTicks) / 90.0);  // convert to ms
        }

        return 0;
    }
    // The video is timed from its first PES (getTrackStart): an HEVC stream that begins with a random access picture,
    // whose leading pictures the reader times before it, as a decoder leaves them out
    void setVideoTimedFromFirstPes(const bool fromFirstPes) { m_videoTimedFromFirstPes = fromFirstPes; }
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
    double getTrackPacketFps(uint32_t trackId, bool fieldCoded) override;
    bool isPartStartPicture(const int32_t pid, const int64_t picture) override
    {
        const auto starts = m_partStartPictures.find(pid);
        return starts != m_partStartPictures.end() && starts->second.count(picture) != 0;
    }
    // The clips that follow the one opened, when a playlist is listed: read by getTrackPacketFps when the first clip
    // holds too few pictures. A join or a playlist that is muxed gives them through setFileIterator.
    void setLaterFiles(const std::vector<std::string>& files) { m_laterFiles = files; }
    void setMPLSInfo(const std::vector<MPLSPlayItem>& mplsInfo) { m_mplsInfo = mplsInfo; }
    std::vector<std::pair<int64_t, int64_t>> takeTimeGaps(int pid) override;
    [[nodiscard]] int64_t getFileDurationNano() const override;
    // The latest PES time read, on the PGS timeline the PES headers are rewritten to (see AbstractDemuxer)
    // How long the first video stream of a file lasts on the timeline of a join, in ns; -1 where it is not found
    static int64_t fileVideoLengthNs(const BufferedReaderManager& readManager, const std::string& fileName);
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
    void readProgramTables(uint8_t* data, const uint8_t* lastFrameAddr, bool laterClip, bool firstBlock);

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

    // A join of whole files ("+"), not a playlist: the sound of each later file starts against its own first picture
    // (see keepJoinSound). Times in 90 kHz ticks, on the timeline of the join (see endJoinFile).
    struct JoinSound
    {
        int64_t origin = -1;    // where the first frame of the track is, the start of its reader's own clock
        int64_t lastPes = -1;   // where the last PES of the file before is; -1: not known
        int64_t end = -1;       // where the sound handed on so far ends
        int64_t shift = 0;      // how much later than its own times the sound of the file being read is muxed
        int64_t firstPts = -1;  // the first PES kept in the file being read, and the last one, on its own clock
        int64_t lastPts = -1;
        int64_t prevPts = -1;      // the PES before, for the step
        int64_t step = 0;          // the shortest time from one PES to the next: a frame, or a PES of frames
        int64_t stepBytes = 0;     // the bytes of the sound of the PES (of one time) that gave the step
        int64_t pesBytes = 0;      // the bytes of the sound of the PES (of one time) being read
        bool sizedFrames = false;  // a constant bit rate: the size of a PES tells its length
        // where the sound of the file being read is on the timeline of the join, less its own times; INT64_MIN: not
        // known
        int64_t onTimeline = INT64_MIN;
    };
    std::map<int, JoinSound> m_joinSound;
    uint8_t m_joinSoundPid[8192]{};  // the PID has a JoinSound
    uint8_t m_joinDrop[8192]{};  // the PES being read of the PID is left out, and the packets that carry the rest of it
    std::map<int, int64_t> m_joinSkip;  // bytes of sound left out at the start of the PES being read (keepJoinSound)
    int64_t m_joinVideoStart = -1;      // the first decoding time of the video of the file being read (fileVideoStart)
    int64_t m_joinBase = 0;             // where the first decoding time of the video of the file being read is
    bool m_joinBaseKnown = true;
    bool m_joinOneClock = false;  // the file being read goes on from the clock of the file before (endJoinFile)
    std::pair<int64_t, int64_t> m_joinFileDts{-1, -1};  // the first and the last decoding time of the video in the file
    std::map<int, int64_t> m_fileFirstDts;              // the first decoding time of each video stream in the file
    std::pair<int64_t, int64_t> m_videoSpanDts{-1, -1};  // the first and the last decoding time of the first video
    std::map<int, std::vector<std::pair<int64_t, int64_t>>> m_joinGaps;  // by PID, see takeTimeGaps
    std::map<int, size_t> m_joinGapsTaken;                               // by track asking, see takeTimeGaps
    [[nodiscard]] bool isJoinOfFiles() const;
    [[nodiscard]] int64_t fileVideoStart(const std::string& fileName, int videoPid) const;
    void endJoinFile();
    bool keepJoinSound(int pid, StreamType type, int64_t pts, const uint8_t* data = nullptr,
                       const uint8_t* dataEnd = nullptr, int64_t pesPayload = -1);
    // A sound track of one file that has no PES for a while, with the picture going on (see noteSoundGap). Times in
    // 90 kHz ticks.
    struct SoundGap
    {
        int64_t origin = -1;  // the first PES of the track, the start of its reader's own clock
        int64_t prev = -1;    // the latest PES time
        int64_t step = 0;     // the shortest step from one PES to the next
        int videoJumps = 0;   // m_videoJumps at the PES before
    };
    std::map<int, SoundGap> m_soundGap;
    int64_t m_gapVideoPrev = -1;  // the decoding time of the first video's PES before
    int m_videoJumps = 0;         // how often the first video's time jumped (a discontinuity), see noteSoundGap
    void noteSoundGap(int pid, int64_t pts, bool cutPart);
    int64_t soundPause(int pid, int64_t pts, int64_t& end);
    // The sound of a file whose first PES does not begin with a frame (skipsLeadingPes)
    static constexpr uint8_t LEAD_PES_MAX = 4;
    static constexpr uint8_t LEAD_PES_DONE = 0xFF;
    uint8_t m_leadPes[8192]{};   // 0: not decided; 1 to LEAD_PES_MAX: PES left out so far; LEAD_PES_DONE: decided
    uint8_t m_leadDrop[8192]{};  // the PES being read of the PID is held back, and the packets carrying its rest
    // The packets of the PES held back, by PID, until the search ends (giveBackLeadPes)
    std::map<int, std::vector<uint8_t>> m_leadHeld;
    uint8_t m_leadFirstSeen[8192]{};  // the first PES held back of the PID came before any other PES of the file
    enum class LeadPes
    {
        Hold,     // left out for now: no frame begins it, and the search goes on
        Keep,     // it begins with a frame: read it, and drop the PES held before it
        GiveBack  // the search ended without a frame: read the PES held before it, then this one
    };
    LeadPes skipsLeadingPes(int pid, StreamType type, const uint8_t* payload, const uint8_t* end);
    void demuxPacket(uint8_t* pkt, int pid, DemuxedData& demuxedData, int64_t& discardSize, const ClipPart* part,
                     MemoryBlock*& vect, int& lastPid);
    void giveBackLeadPes(int pid, DemuxedData& demuxedData, int64_t& discardSize, const ClipPart* part,
                         MemoryBlock*& vect, int& lastPid);
    // A packet sent twice (ISO/IEC 13818-1, 2.4.3.3): the continuity counter and a hash of the payload of the last
    // packet with payload of each PID, the counter 0xFF before the first
    uint8_t m_lastCounter[8192];
    uint64_t m_lastPayloadHash[8192]{};

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
    bool m_videoTimedFromFirstPes = false;
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

    // ** DAMAGE A DECODER SEES AND THE MUX DID NOT. ** Packets marked scrambled (transport_scrambling_control, the
    // recording of an encrypted channel without its key) and packets lost (the continuity counter skips) went into the
    // tracks without a word: the pictures were damaged at exit 0. Counted for each track muxed, and said once at the
    // end of the file (reportDamage); a track whose every packet is scrambled is refused. Lost packets of a sound track
    // are left to its reader, which says how many bytes it could not use.
    struct PidDamage
    {
        int64_t packets = 0;    // with payload
        int64_t scrambled = 0;  // of those, marked scrambled
        int64_t lost = 0;       // packets missing by the continuity counter (at least)
        int64_t scrambledAt = -1;
        int64_t lostAt = -1;
        int lastCounter = -1;  // -1 at the start of each file
    };
    bool m_reportDamage = false;
    bool m_damageReported = false;
    std::map<int, PidDamage> m_damage;
    std::map<int, int64_t> m_damagePts;  // the last PES time of each such track, for where the damage starts
    // Bytes that are no TS packet, passed over to find the next sync byte (a part of the file overwritten with zeros or
    // lost), and the bytes of a packet the file ends inside (a file cut off): said at the end with the other damage.
    int64_t m_noPacketBytes = 0;
    int m_endInsidePacket = 0;
    void noteDamage(const TSPacket* tsPacket, int pid);
    void reportDamage();

    static bool isVideoPID(StreamType streamType);
    bool checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const;
};

// The length of a TS or M2TS file in 90 kHz ticks, from the clock at its start and at its end
int64_t getTSDuration(const char* fileName);

#endif
