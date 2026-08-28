#ifndef TS_MUXER_H_
#define TS_MUXER_H_

#include <types/types.h>

#include <map>
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
    void setPCROnVideoPID(const bool val) { m_pcrOnVideo = val; }
    void setMaxBitrate(const int val) { m_cbrBitrate = val; }
    void setMinBitrate(const int val) { m_minBitrate = val; }
    void openDstFile() override;
    void setVBVBufferLen(int value);
    [[nodiscard]] const PIDListMap& getPidList() const { return m_pmt.pidList; }
    [[nodiscard]] std::vector<int64_t> getFirstPts() const;
    void alignPTS(TSMuxer* otherMuxer);
    [[nodiscard]] std::vector<int64_t> getLastPts() const;
    const std::vector<uint32_t>& getMuxedPacketCnt() { return m_muxedPacketCnt; }
    [[nodiscard]] size_t splitFileCnt() const { return m_fileNames.size(); }
    void setSplitDuration(const int64_t value) { m_splitDuration = value; }
    void setSplitSize(const int64_t value) { m_splitSize = value; }
    [[nodiscard]] bool isSplitting() const { return m_splitSize > 0 || m_splitDuration > 0; }
    void parseMuxOpt(const std::string& opts) override;

    void setFileName(const std::string& fileName, FileFactory* fileFactory) override;
    std::string getFileNameByIdx(size_t idx);
    [[nodiscard]] int getFirstFileNum() const;
    [[nodiscard]] bool isInterleaveMode() const;
    [[nodiscard]] std::vector<int32_t> getInterleaveInfo(size_t idx) const;
    [[nodiscard]] bool isSubStream() const { return m_subMode; }

    void setPtsOffset(int64_t value);

   protected:
    bool muxPacket(AVPacket& avPacket) override;
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
    void buildNULL();
    void buildPAT();
    void buildPMT();
    static void buildSIT();
    void addData(uint8_t pesStreamID, int pid, AVPacket& avPacket);
    void buildPesHeader(uint8_t pesStreamID, AVPacket& avPacket, int pid);
    void writePESPacket();
    void processM2TSPCR(int64_t pcrVal, int64_t pcrGAP);
    [[nodiscard]] inline int calcM2tsFrameCnt() const;
    static void writeM2TSHeader(uint8_t* buffer, const int64_t m2tsPCR)
    {
        const auto cur = reinterpret_cast<uint32_t*>(buffer);
        *cur = my_htonl(m2tsPCR & 0x3fffffff);
    }
    void writePATPMT(int64_t pcr, bool force = false);
    void writePCR(int64_t newPCR);
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

    AbstractOutputStream* m_muxFile;
    bool m_isExternalFile;

    int64_t m_fixed_pcr_offset;
    bool m_pcrOnVideo;
    int m_cbrBitrate;
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

    bool m_useNewStyleAudioPES;

    int64_t m_lastPESDTS;
    int64_t m_fullPesDTS;
    int64_t m_fullPesPTS;
    std::vector<std::pair<uint8_t*, int>>
        m_m2tsDelayBlocks;  // postpone M2TS PCR processing (fill previous data on next PCR after several PES packets)
    int m_prevM2TSPCROffset;
    int64_t m_prevM2TSPCR;
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

    struct StreamInfo
    {
        StreamInfo()
        {
            m_pts = m_dts = ULLONG_MAX;
            m_tsCnt = 0;
            m_mpegReader = nullptr;
            m_definesDuration = false;
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
    };

    int64_t m_minDts;
    bool m_beforePCRDataWrited;
    std::map<int, int> m_extIndexToTSIndex;
    uint16_t m_videoTrackCnt;
    uint16_t m_DVvideoTrackCnt;
    uint16_t m_videoSecondTrackCnt;
    uint16_t m_audioTrackCnt;
    uint16_t m_secondaryAudioTrackCnt;
    uint16_t m_pgsTrackCnt;
    int64_t m_lastPCR;
    std::map<int, StreamInfo> m_streamInfo;
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
    PriorityDataInfo m_priorityData;
    int64_t m_timeOffset;
    int64_t m_lastSITPCR;
    bool m_canSwithBlock;
    int64_t m_additionCLPISize;
    std::vector<std::string> m_fileNames;
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
