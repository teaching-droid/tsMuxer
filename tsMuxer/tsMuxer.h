// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef TS_MUXER_H_
#define TS_MUXER_H_

#include <types/types.h>

#include <deque>
#include <map>
#include <set>
#include <vector>

#include "abstractMuxer.h"
#include "avPacket.h"
#include "hevc.h"
#include "limits.h"

enum V3Flags
{
    HDMV_V3 = 1,
    HDR10 = 2,
    DV = 4,
    SL_HDR2 = 8,
    HDR10PLUS = 16,
    FOUR_K = 32,
    BL_TRACK = 64,
    BL_NOTCOMPAT = 128,
    // Set when the output is a Blu-ray structure, so a stream reader can tell disc authoring from a
    // plain .ts or a demux. Readers have no other way to know: H264StreamReader's m_blurayMode is
    // hardcoded true in its constructor and never reflects the output type.
    BLURAY_OUT = 256
};

extern int V3_flags;
extern unsigned HDR10_metadata[6];
extern bool isV3();
extern bool is4K();

class MPEGStreamReader;

static constexpr int MAX_PES_HEADER_LEN = 512;

// Raise the option refusals that main() would otherwise raise AFTER the destination has been
// created, which replaced the user existing image with a stub. Called while the meta file is
// still being read. Sets nothing; the checks inside TSMuxer::parseMuxOpt stay as the backstop.
void validateMuxOptsEarly(const std::string& opts);

// The size a --split-size value works out at in bytes, or a refusal (tsMuxer.cpp).
int64_t checkedSplitSize(const std::string& value);
int64_t checkedSplitDuration(const std::string& value);

class TSMuxer final : public AbstractMuxer
{
    typedef AbstractMuxer base_class;

   public:
    TSMuxer(MuxerManager* owner);
    ~TSMuxer() override;
    void intAddStream(const std::string& streamName, const std::string& codecName, int streamIndex,
                      const std::map<std::string, std::string>& params, AbstractStreamReader* codecReader) override;
    bool doFlush() override;
    bool close() override;

    [[nodiscard]] int64_t getVBVLength() const { return m_vbvLen / 90; }
    void setNewStyleAudioPES(const bool val) { m_useNewStyleAudioPES = val; }
    void setM2TSMode(const bool val) { m_m2tsMode = val; }
    [[nodiscard]] bool isM2TSMode() const { return m_m2tsMode; }
    void setPCROnVideoPID(const bool val) { m_pcrOnVideo = val; }
    // --bitrate feeds this. It drives the CBR clock and, with a minimum, the null padding.
    void setMuxRate(const int val) { m_cbrBitrate = val; }
    // --maxbitrate feeds this. It still sets the rate above, so everything that option did
    // before it does now, and it additionally names a ceiling the pacing may not cross.
    void setMaxBitrate(const int val)
    {
        m_cbrBitrate = val;
        m_maxBitrate = val;
    }
    void setMinBitrate(const int val) { m_minBitrate = val; }
    void openDstFile() override;
    void setVBVBufferLen(int value);
    [[nodiscard]] const PIDListMap& getPidList() const { return m_pmt.pidList; }
    [[nodiscard]] int getPcrPid() const { return m_pmt.pcr_pid; }
    [[nodiscard]] std::vector<int64_t> getFirstPts() const;
    void alignPTS(TSMuxer* otherMuxer);
    [[nodiscard]] std::vector<int64_t> getLastPts() const;
    // --cut-pictures=cover: the first picture of the main video shown at or after --cut-start and --cut-end, where a
    // disc playlist starts and stops playing, on the internal clock (-1: its first and last packet); and before the
    // mux, the time the chapters of a --cut-start are counted from, the cut (-1: the first packet)
    void setCutPlayTimes(const int64_t from, const int64_t to)
    {
        m_cutPlayFrom = from;
        m_cutPlayTo = to;
    }
    void setChapterOrigin(const int64_t origin) { m_chapterOrigin = origin; }
    // The same on the clock of the clips (90 kHz with the start time), -1 when not set
    [[nodiscard]] int64_t getCutPlayFromPts() const { return clipPts(m_cutPlayFrom); }
    [[nodiscard]] int64_t getCutPlayToPts() const { return clipPts(m_cutPlayTo); }
    [[nodiscard]] int64_t getChapterOriginPts() const { return clipPts(m_chapterOrigin); }
    const std::vector<uint32_t>& getMuxedPacketCnt() { return m_muxedPacketCnt; }
    [[nodiscard]] size_t splitFileCnt() const { return m_fileNames.size(); }
    void setSplitDuration(const int64_t value) { m_splitDuration = value; }
    void setSplitSize(const int64_t value) { m_splitSize = value; }
    [[nodiscard]] bool isSplitting() const
    {
        return m_splitSize > 0 || m_splitDuration > 0 || (m_splitByChapters && !m_chapterPts.empty());
    }
    void setSplitByChapters(const bool value) { m_splitByChapters = value; }
    void setChapters(const std::vector<double>& chapters) override;
    void parseMuxOpt(const std::string& opts) override;

    void setFileName(const std::string& fileName, FileFactory* fileFactory) override;
    std::string getFileNameByIdx(size_t idx);
    [[nodiscard]] int getFirstFileNum() const;
    [[nodiscard]] bool isInterleaveMode() const;
    [[nodiscard]] std::vector<int32_t> getInterleaveInfo(size_t idx) const;
    [[nodiscard]] bool isSubStream() const { return m_subMode; }
    // The rate of the clip on a disc, in bit/s: the TS_recording_rate of its clip information and the peak rate of its
    // SIT, which a disc gives the same value
    [[nodiscard]] int discRecordingRate() const;

    void setPtsOffset(int64_t value);

   protected:
    bool muxPacket(AVPacket& avPacket) override;
    void reportFrame(const AVPacket& avPacket, int pid, int64_t clock, bool newPES);
    void flushReportFrame();
    void internalReset();
    void setMuxFormat(const std::string& format);
    [[nodiscard]] bool isSplitPoint(const AVPacket& avPacket) const;
    [[nodiscard]] bool blockFull() const;

   private:
    bool doFlush(int64_t newPCR, int64_t pcrGAP);
    void flushTSFrame();
    int writeTSFrames(int pid, const uint8_t* buffer, int64_t len, bool priorityData, bool payloadStart);
    void writeSIT();
    void writePMT();
    void writePAT();
    void writeNullPackets(int cnt);
    void writeOutBuffer();
    void writeEmptyPacketWithPCR(int64_t pcrVal);
    int pcrOnlyPacketCounter();
    void buildNULL();
    void buildPAT();
    void buildPMT();
    static void buildSIT();
    void addData(uint8_t pesStreamID, int pid, AVPacket& avPacket);
    void buildPesHeader(uint8_t pesStreamID, AVPacket& avPacket, int pid);
    void writePESPacket();
    void processM2TSPCR(int64_t pcrVal, int64_t pcrGAP);
    [[nodiscard]] inline int calcM2tsFrameCnt() const;
    [[nodiscard]] bool maxRateClockDue() const;
    void insertHeldPCRIfDue();
    void warnV3Video(MPEGStreamReader* video);
    void warnDiscVideo(MPEGStreamReader* video);
    void pcrInsideTablesIfDue();
    void refuseLongNonVideoPes() const;
    void noteLateArrival();
    void reportLateArrival() const;
    void noteCbrRate(int64_t pcr, int64_t paddingBits);
    void reportCbrExcess() const;
    // Key pictures of each video PID on a disc: the time of the last one, the longest stretch without one, the time of
    // the last picture; said once when a stretch is long (reportKeyPictureGaps).
    struct KeyGap
    {
        int64_t lastKey = -1;
        int64_t longest = 0;
        int64_t lastPts = -1;
    };
    std::map<int, KeyGap> m_keyGaps;
    bool m_keyGapsReported = false;
    void noteKeyGap(int pid, int64_t pts, bool key);
    void reportKeyPictureGaps();
    [[nodiscard]] int64_t cbrClock() const;
    [[nodiscard]] int ceilingRate() const;
    [[nodiscard]] int64_t pcrForMaxRate(int64_t newPCR, int64_t extraFrames, int64_t pcrGAP) const;
    [[nodiscard]] bool nextPCRChanceTooLate(int tsIndex, int64_t dts) const;
    void noteAtsPass(int frames, double gap);
    // Sending ahead under --maxbitrate, see sendAheadAdvance in tsMuxer.cpp.
    void sendAheadRecordPass(int64_t pcrVal, int64_t floorPcr, int64_t pcrGAP, int64_t oldPrevHi, bool hasPcrPacket);
    void sendAheadNotePes();
    void sendAheadAdvance(int64_t muxDts);
    void sendAheadFinishAll();
    void sendAheadFinalizeFront();
    [[nodiscard]] bool sendAheadReach(int64_t* dts) const;
    void sendAheadReleaseDone(bool all);
    void sendAheadHold(uint8_t* data, int len);
    void sendAheadPatch(int64_t pos, const uint8_t* src, int len);
    void writeBlockToOwner(uint8_t* data, int len);
    static void writeM2TSHeader(uint8_t* buffer, const int64_t m2tsPCR)
    {
        const auto cur = UnalignedPtr<uint32_t>(buffer);
        *cur = my_htonl(m2tsPCR & 0x3fffffff);
    }
    void writePATPMT(int64_t pcr, bool force = false);
    void writePCR(int64_t newPCR);
    void writeFloorPadding(int64_t newPCR);
    std::string getNextName(std::string curName) override;
    void writeEmptyPacketWithPCRTest(int64_t pcrVal);
    bool appendM2TSNullPacketToFile(int64_t curFileSize, int counter, int* packetsWrited) const;
    int writeOutFile(const uint8_t* buffer, int len) const;

    void joinToMasterFile() override;
    void setSubMode(AbstractMuxer* mainMuxer, bool flushInterleavedBlock) override;
    void setMasterMode(AbstractMuxer* subMuxer, bool flushInterleavedBlock) override;

    [[nodiscard]] AbstractOutputStream* getDstFile() const { return m_muxFile; }
    void flushTSBuffer();
    void finishFileBlock(int64_t newPts, int64_t newPCR, bool doChangeFile, bool recursive = true);
    void gotoNextFile(int64_t newPts);
    void startIndexLists(PMTStreamInfo& streamInfo) const;

    AbstractOutputStream* m_muxFile;
    bool m_isExternalFile;

    int64_t m_fixed_pcr_offset;
    bool m_pcrOnVideo;
    int m_cbrBitrate;
    int m_maxBitrate;
    int m_minBitrate;
    int m_pcr_delta;    // how often write PCR
    int m_patPmtDelta;  // how often write PAT/PMT
    bool m_m2tsMode;
    int m_curFileNum;
    bool m_bluRayMode;
    bool m_hdmvDescriptors;
    // 64 bit: a split size is a byte count and 4 GiB is a perfectly ordinary one. As a uint32_t it
    // wrapped, so 4 GiB became 0 and turned splitting off, and 4.5 GB became 205 MB.
    int64_t m_splitSize;
    int64_t m_splitDuration;
    // Splitting at chapter boundaries. The times arrive in seconds from the start of the film and
    // are kept as internal clock ticks measured from the first packet, because that is what an
    // AVPacket's pts is counted in and a transport stream does not start at zero.
    bool m_splitByChapters;
    std::vector<int64_t> m_chapterPts;
    size_t m_nextChapter;

    bool m_useNewStyleAudioPES;

    int64_t m_lastPESDTS;
    int64_t m_fullPesDTS;
    int64_t m_fullPesPTS;
    std::vector<std::pair<uint8_t*, int>>
        m_m2tsDelayBlocks;  // postpone M2TS PCR processing (fill previous data on next PCR after several PES packets)
    int m_prevM2TSPCROffset;
    int64_t m_prevM2TSPCR;
    // How hard the finished disc asks the player to read. Filled in while the arrival timestamps
    // are written and reported once at the end. See reportReadRate.
    double m_minAtsGap;
    int64_t m_atsPackets;
    int64_t m_atsOverLimit;
    void reportReadRate() const;
    int64_t m_endStreamDTS;
    int m_lastTSIndex;
    int m_lastPesLen;
    int m_pcrBits;
    // CBR pacing evaluation skip (writeTSFrames): last m_lastPCR the estimate was made
    // for, and the m_pcrBits value before which no crossing is possible
    int64_t m_cbrEvalPCR;
    int64_t m_cbrNextEvalBits;

    // In-place PES fast path (VBR only): once an accumulation is provably larger than
    // 65541 bytes (no PES length patch possible), its remaining payload is packetized
    // straight into m_outBuf instead of being staged in m_pesData first.
    bool m_inplacePending;
    int64_t m_cappedPictures;  // video PES written under --maxbitrate (noteLateArrival)
    int64_t m_latePictures;    // of them, the ones whose last packet arrives after their DTS
    int64_t m_worstLateness;   // the largest such lateness, 90 kHz
    // --bitrate: the output measured in windows of at least a second of the clock (noteCbrRate)
    bool m_cbrGiven;                // --bitrate was given (--maxbitrate alone also sets m_cbrBitrate)
    bool m_ceilingFromBitrate;      // --bitrate holds the clock and is the ceiling of the late count (K18)
    int64_t m_cbrWinBits;           // bits of content (not the null packets of the padding) in the current window
    int64_t m_cbrWinTicks;          // its length so far, 90 kHz
    int64_t m_cbrWinStart;          // the PCR it began at
    int64_t m_cbrCarryBits;         // bits the last PCR counted into the next interval, written before it
    int64_t m_floorPadBits;         // null packets the floor padding wrote since the last PCR (writeFloorPadding)
    int m_floorPadRest;             // what that padding wrote beyond the floor, carried into the next interval
    int64_t m_cbrPeakRate;          // the highest rate of a window above --bitrate, bits per second; 0: none
    int64_t m_cbrPeakAt;            // the middle of that window, on the clock
    bool m_midPesPCR;               // a PCR went in between the packets of the pending PES (insertHeldPCRIfDue)
    int m_pendingStartOffset;       // first TS packet of the pending PES in m_outBuf
    uint32_t m_pendingStartPktCnt;  // m_muxedPacketCnt at PES start, for the seek index
    uint32_t m_pendingTsPackets;    // TS packets emitted for the pending PES itself
    bool m_pendingPayloadStart;     // the next emitted packet is the PES's first
    bool m_pendingTailPriority;     // priority flag of the open segment
    int m_pendingTailLen;           // open-segment remainder, < 184 bytes
    uint8_t m_pendingTail[TS_FRAME_SIZE];
    uint64_t m_pendingPesPts;  // captured for the seek index (m_pesData is empty later)
    bool m_pendingPesHasPts;
    MemoryBlock m_pendingSpill;  // evacuation area for mid-PES PAT/PMT/PCR writes
    int m_outBufCapacity;

    // note: m_sectorSize is NOT part of the gate; every m2ts mux sets it for the
    // end-of-file 6 KB rounding, which runs long after any pending PES was finalized
    [[nodiscard]] bool inplaceEligible() const
    {
        return m_cbrBitrate == -1 && m_interliaveBlockSize == 0 && !m_subMode && !m_masterMode;
    }
    void ensureOutBufSpace(int needed);
    void switchToInplace();
    void emitInplacePacket(const uint8_t* payload, int payloadLen, bool priority);
    void emitInplacePayload(const uint8_t* data, int64_t len, bool priority);
    void inplaceCloseSegment();
    void finalizeInplacePes();
    std::vector<int64_t> m_lastPts;
    std::vector<int64_t> m_firstPts;
    int64_t m_cutPlayFrom = -1;
    int64_t m_cutPlayTo = -1;
    int64_t m_chapterOrigin = -1;
    [[nodiscard]] int64_t clipPts(int64_t internal) const;

    struct StreamInfo
    {
        StreamInfo()
        {
            m_pts = m_dts = ULLONG_MAX;
            m_tsCnt = 0;
            m_mpegReader = nullptr;
            m_definesDuration = false;
            m_dtsStep = 0;
            m_steadyPace = false;
        }
        int64_t m_pts;
        int64_t m_dts;
        int m_tsCnt;
        // downcast of the stream's codec reader, cached at intAddStream so muxPacket
        // does not pay a dynamic_cast per AVPacket; nullptr for non-video streams
        MPEGStreamReader* m_mpegReader;
        // a primary video stream, the kind that gets PID 0x1011: neither secondary nor a
        // Dolby Vision enhancement layer. These are the streams a title is measured by.
        bool m_definesDuration;
        // For the PCR timing: the step between the stream's last two PES, in the internal clock, and
        // whether its PES come at a steady pace, which video and audio do and subtitles do not.
        int64_t m_dtsStep;
        bool m_steadyPace;
    };

    int64_t m_minDts;
    bool m_beforePCRDataWrited;
    std::map<int, int> m_extIndexToTSIndex;
    uint16_t m_videoTrackCnt;
    int m_firstBaseWidth = 0;  // the width of the first primary video: an enhancement layer is never wider
    // The primary video of a V3 disc whose picture size is not read yet at intAddStream, tested at its first packet
    MPEGStreamReader* m_v3VideoPending = nullptr;
    int m_v3VideoPendingIndex = -1;
    MPEGStreamReader* m_discVideoPending = nullptr;  // the primary video of a disc, checked at its first packet
    int m_discVideoPendingIndex = -1;
    uint16_t m_DVvideoTrackCnt;
    // The PIDs of the base layers that carry no Dolby Vision of their own, in order: the n-th enhancement layer
    // belongs to the n-th of them (setDoViBaseLayerPid)
    std::vector<int> m_dvBasePids;
    uint16_t m_videoSecondTrackCnt;
    uint16_t m_audioTrackCnt;
    uint16_t m_secondaryAudioTrackCnt;
    uint16_t m_pgsTrackCnt;
    int64_t m_lastPCR;
    std::map<int, StreamInfo> m_streamInfo;
    // the PES of the frames file (--report-frames) not yet written: its line without the byte count, and the count
    std::string m_reportFrameHead;
    std::string m_reportFrameTail;
    int64_t m_reportFrameBytes = 0;
    int64_t m_lastPMTPCR;
    uint8_t* m_outBuf;
    int32_t m_outBufLen;
    int m_nullCnt;
    int m_pmtCnt;
    int m_patCnt;
    int m_sitCnt;
    uint32_t m_lastGopNullCnt;

    uint8_t m_pmtBuffer[4096];
    uint8_t m_patBuffer[TS_FRAME_SIZE];
    uint8_t m_nullBuffer[TS_FRAME_SIZE];
    TS_program_map_section m_pmt;
    TS_program_association_section m_pat;
    std::map<int, uint8_t> m_pesType;
    bool m_needTruncate;
    int64_t m_lastMuxedDts;
    MemoryBlock m_pesData;
    int m_pesPID;
    std::vector<uint32_t> m_muxedPacketCnt;
    bool m_pesIFrame;
    bool m_pesSpsPps;
    bool m_computeMuxStats;
    int64_t m_pmtFrames;
    int64_t m_curFileStartPts;
    int64_t m_vbvLen;
    int m_mainStreamIndex;
    std::map<std::string, std::set<std::string>> m_bcp47ByLang;  // the BCP 47 tags seen for each language code
    // rank of the stream currently holding the main role: 3 primary video, 2 other video,
    // 1 audio, 0 nothing chosen yet. A lower rank never displaces a higher one.
    int m_mainStreamRank;

    std::string m_outFileName;
    int m_writeBlockSize;
    int m_frameSize;
    int64_t m_processedBlockSize;
    TSMuxer* m_sublingMuxer;
    std::vector<std::vector<int32_t>> m_interleaveInfo;  // ssif interliave info, should be written to CLPI
    bool m_masterMode;
    bool m_subMode;
    bool m_sitHevc = false;  // an HEVC track, for the SIT outside a disc
    PriorityDataInfo m_priorityData;
    int64_t m_timeOffset;
    int64_t m_lastSITPCR;
    bool m_canSwithBlock;
    int64_t m_additionCLPISize;
    std::vector<std::string> m_fileNames;
    // Sending ahead under --maxbitrate. A pass is the run of packets that one PCR (or the end of a block) closes; its
    // arrival times are spread evenly between the end of the pass before and its own end.
    struct SendAheadPass
    {
        int64_t endPos;     // output position just after the pass's last packet
        int64_t frames;     // packets in the pass
        int64_t oldPcr;     // the time its end was given when it was written, 90 kHz
        int64_t oldPrevHi;  // where it began then, 27 MHz
        int64_t pcrGap;     // 27 MHz held back from the end of a pass that closes a block
        int64_t bits;       // a transport stream: bits since the PCR before, as the pacing counted them
        int64_t dtsClock;   // the time the picture times asked for, 90 kHz
        int64_t muxDts;     // the latest decoding time muxed by then, 90 kHz
        bool hasPcrPacket;
        bool first;
        int64_t idle;  // how much later its end came than the cap alone needed, 90 kHz: the link stood idle
    };
    struct SendAheadPes
    {
        int64_t endPos;  // output position just after the PES's last packet
        int64_t dts;     // 90 kHz, the clock of the PCR
        bool video;
    };
    struct SendAheadBlock
    {
        uint8_t* data;
        int len;
        int64_t pos;
    };
    bool m_sendAhead;
    int64_t m_outBufPos;  // output position of m_outBuf[0]
    std::deque<SendAheadPass> m_saPasses;
    std::deque<SendAheadPes> m_saPes;
    std::deque<SendAheadBlock> m_saHeld;
    int64_t m_saHeldEnd;     // output position after the last byte handed to the hold
    int64_t m_saRecEnd;      // output position after the last pass recorded
    int64_t m_saRecOldHi;    // its end as written, 27 MHz
    int64_t m_saDoneEnd;     // output position up to which every time is final
    int64_t m_saPrevHi;      // the final time of that position, 27 MHz
    int64_t m_saPrevPcr;     // the final value of the last PCR, 90 kHz
    int64_t m_saPrevOldPcr;  // and its value as written
    int64_t m_saPassBits;    // m_pcrBits at the PCR being written
    int64_t m_saFloorPcr;    // the earliest time the cap allowed that PCR
    bool m_saFirstPass;
    int64_t m_saDtsClock;
    int64_t m_saMuxDts;
#ifdef _DEBUG
    int64_t m_lastProcessedDts;
    int m_lastStreamIndex;
#endif
};

class TSMuxerFactory final : public AbstractMuxerFactory
{
   public:
    [[nodiscard]] std::unique_ptr<AbstractMuxer> newInstance(MuxerManager* owner) const override
    {
        return std::make_unique<TSMuxer>(owner);
    }
};

#endif
