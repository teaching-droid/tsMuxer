#ifndef H264_STREAM_READER_H_
#define H264_STREAM_READER_H_

#include <map>
#include <set>
#include <vector>

#include "avPacket.h"
#include "mpegStreamReader.h"
#include "nalUnits.h"
#include "vod_common.h"

class H264StreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    enum class SeiMethod
    {
        SEI_DoNotInsert,
        SEI_InsertAuto,
        SEI_InsertForce,
        SEI_NotDefined
    };

    H264StreamReader();
    ~H264StreamReader() override;
    void setForceLevel(const uint8_t value) { m_forcedLevel = value; }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void setH264SPSCont(const bool val) { m_h264SPSCont = val; }
    // ** THE READER'S OWN TEST, NOT A NEW ONE. ** m_spsCounter < 2 is exactly what decides, a few
    // lines into readPacket, that a stream does not repeat its parameter sets and needs them
    // inserted. Asking a different question here would be a second predicate that can drift from
    // the first. A source that DOES repeat them is counted past 2 and needs no warning.
    [[nodiscard]] bool spsPpsRepeatedInStream() const { return m_h264SPSCont || m_spsCounter >= 2; }

    void setInsertSEI(const SeiMethod value) { m_insertSEIMethod = value; }
    [[nodiscard]] SeiMethod getInsertSEI() const { return m_insertSEIMethod; }

    void setIsSubStream(const bool value) { m_mvcSubStream = value; }
    [[nodiscard]] bool isSubStream() const { return m_mvcSubStream; }

    [[nodiscard]] int getOffsetSeqCnt() const { return number_of_offset_sequences; }

    // used for correction offset metadata
    void setStartPTS(const int64_t pts) { m_startPts = pts; }
    [[nodiscard]] bool needSPSForSplit() const override { return true; }

   protected:
    void onSplitEvent() override { m_firstFileFrame = true; }
    const CodecInfo& getCodecInfo() override;
    int intDecodeNAL(uint8_t* buff) override;
    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    double getStreamFPS(void* curNalUnit) override
    {
        const auto sps = static_cast<SPSUnit*>(curNalUnit);
        return sps->getFPS();
    }
    // updateFPS runs before the SPS is kept, so getInterlaced does not know it yet
    bool interlacedBy(void* curNalUnit) override
    {
        return curNalUnit ? !static_cast<SPSUnit*>(curNalUnit)->frame_mbs_only_flag : getInterlaced();
    }
    bool codesFramesOnly(void* curNalUnit) override;

    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    // With pulldown a picture is shown for up to 3 frames, and the decoding times have to be moved back by as much
    // time as the pictures shown before them in the order shown take, not by as many frames.
    int getFrameDepth() override { return (m_frameDepth * m_pulldownMaxFields + 1) / 2; }
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    [[nodiscard]] int getStreamHDR() const override { return 0; }
    [[nodiscard]] bool getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const override;
    bool getInterlaced() override;
    bool isIFrame() override { return m_lastIFrame; }
    // virtual bool isIFrame() { return m_lastSliceIDR; }

    bool isPriorityData(AVPacket* packet) override;
    void onShiftBuffer(int offset) override;
    void onMoveBuffer(std::ptrdiff_t delta) override;
    bool skipNal(uint8_t* nal) override;

   private:
    [[nodiscard]] bool replaceToOwnSPS() const;
    // pic_order_cnt_type 1 (H.264 8.2.1.2): a picture's order count follows from its frame_num, with the offset of
    // frame_num at the picture before (each wrap of frame_num adds one MaxFrameNum), and the cycle of offsets of the
    // SPS. It is given to the rest of the reader as a count of type 0 of 16 bits (fillPoc1), so that each reading of
    // the counts, from a picture on or ahead of it, follows such a stream as it follows one of type 0.
    struct Poc1State
    {
        int prevFrameNum = 0;
        int64_t prevFrameNumOffset = 0;
        int lastOrder = 0;  // the count of the picture read last, for its other slices
    };
    Poc1State m_poc1;  // at the picture the reader is at
    static void fillPoc1(SliceUnit& slice, Poc1State& state);
    // poc1: the state the slices read follow on from; without one, each slice is read as if after an IDR
    int deserializeSliceHeader(SliceUnit& slice, const uint8_t* buff, const uint8_t* sliceEnd,
                               Poc1State* poc1 = nullptr);
    void checkPyramid(int frameNum, int* fullPicOrder, bool nextFrameFound);

    bool m_nextFrameFound;
    bool m_nextFrameIdr;

    uint32_t m_idrSliceCnt;
    bool m_bSliceFound;
    int m_picOrderOffset;
    int m_frameDepth;

    int m_spsCounter;
    // An access unit is read again from its start when its picture needs data the buffer does not hold yet: its SPS
    // is counted once, by where it stands in the stream (countSps)
    int64_t m_spsCountedTo = -1;
    void countSps(const uint8_t* nal);
    bool m_h264SPSCont;
    bool m_lastSliceIDR;
    uint32_t m_lastSliceSPS;
    uint32_t m_lastSlicePPS;
    bool m_firstAUDWarn;
    bool m_firstSPSWarn;
    bool m_firstSEIWarn;
    bool m_delimiterFound;
    uint8_t m_forcedLevel;
    int m_pict_type;
    int m_iFramePtsOffset;
    int prevPicOrderCntMsb;
    int prevPicOrderCntLsb;
    int m_forceLsbDiv;
    int m_fieldFirst;
    int m_lastMessageLen;
    bool m_isFirstFrame;
    SeiMethod m_insertSEIMethod;
    bool m_needSeiCorrection;
    int number_of_offset_sequences;
    int m_frameNum;
    bool m_lastIFrame;
    bool m_firstDecodeNal;
    int8_t m_lastPictStruct;
    bool m_firstFileFrame;
    std::map<uint32_t, SPSUnit*> m_spsMap;
    std::map<uint32_t, PPSUnit*> m_ppsMap;
    std::set<uint32_t> updatedSPSList;
    // Each SPS as it came in the stream, by its id: a later one with the same id and other content is read again
    std::map<uint32_t, std::vector<uint8_t>> m_spsSource;
    std::map<uint32_t, std::vector<uint8_t>> m_spsRewritten;  // the same SPS after a new rate was written into it
    bool m_spsChangeSaid = false;
    std::vector<uint8_t> m_lastSeiMvcHeader;
    // The SPS of the last IDR, as stored in m_spsMap. An IDR with a different one begins a new stream,
    // and processSliceNal decides the picture order count step again there.
    std::vector<uint8_t> m_idrSps;
    int64_t m_lastDtsInc;
    bool orig_hrd_parameters_present_flag;
    bool orig_vcl_parameters_present_flag;
    bool m_mvcSubStream;
    bool m_mvcPrimaryStream;
    bool m_blurayMode;
    int64_t m_startPts;

    void additionalStreamCheck(uint8_t* buff, uint8_t* end);
    int calcPicOrder(const SliceUnit& slice);
    int getIdrPrevFrames(uint8_t* buff, const uint8_t* bufEnd);
    int leadingPicturesOfFirst(const SliceUnit& firstSlice, uint8_t* buff);
    int depthOfIdrPeriod(const SliceUnit& idrSlice, uint8_t* buff);
    // The full order a picture would get as the next of the period (calcPicOrder, without moving its state)
    [[nodiscard]] int nextPicOrder(const SliceUnit& slice) const;
    // the full orders given in the current IDR period, for a join (see processSliceNal)
    std::set<int> m_periodOrders;
    int64_t m_maxPts = 0;       // the latest presentation time given, for the start of the next IDR period
    int64_t m_maxPtsShown = 0;  // how long the picture at m_maxPts is shown: a frame, or its fields with pulldown
    // Pulldown with pictures shown out of decode order (B pictures): a picture is shown for 2 to 6 fields, so its
    // presentation time is the time the pictures shown before it take. pulldownFieldsAhead reads the ones that
    // come later in decode order.
    int m_pulldownMaxFields = 2;       // the most fields one picture is shown for, from the start of the stream
    int m_pulldownPendingFields = 2;   // the same for every picture read so far: taken at the next IDR
    std::map<int, int> m_fieldsShown;  // full order -> fields shown, in the current IDR period
    std::vector<std::pair<int, int>> m_fieldsAhead;  // (order relative to this picture, fields) read ahead
    int64_t m_pulldownOrigin = 0;                    // presentation time of the first picture shown in the period
    int m_pulldownFirstOrder = 0;                    // the full order of that picture
    int m_pulldownSumOrder = 0;                      // m_pulldownSumFields counts the orders before this one
    int64_t m_pulldownSumFields = 0;                 // the fields shown from m_pulldownFirstOrder to m_pulldownSumOrder
    void pulldownFieldsAhead(const SliceUnit& slice, uint8_t* buff);
    bool m_ptsGiven = false;
    bool m_fieldPicturesSeen = false;  // coded fields: the depth from checkPyramid is held at 3
    int lsbDivOfIdrPeriod(const SliceUnit& idrSlice, uint8_t* buff);
    // auFirst: the slice follows the delimiter, SPS, PPS or SEI that began its access unit, so it is the first slice of
    // its picture whatever its header says.
    int processSliceNal(uint8_t* buff, bool auFirst = false);
    int processSPS(uint8_t* buff);
    int processPPS(uint8_t* buff);
    int detectPrimaryPicType(const SliceUnit& firstSlice, uint8_t* buff);
    [[nodiscard]] int sliceTypeToPictType(uint32_t slice_type) const;
    uint8_t* writeNalPrefix(uint8_t* curPos) const;
    bool findPPSForward(uint8_t* buff);
    void updateHRDParam(SPSUnit* sps) const;
    int processSEI(uint8_t* buff);
    int getNalHrdLen(const uint8_t* nal) const;
    int writeSEIMessage(uint8_t* dstBuffer, const uint8_t* dstEnd, const SEIUnit& sei, uint8_t payloadType) const;

    std::vector<uint8_t> m_bdRomMetaDataMsg;  // copy metadata to buffer, then isert during sei rebuild process
    int m_bdRomMetaDataMsgPtsPos;
    uint8_t* m_priorityNalAddr;  // just correct pts, keep other data unchanged
    uint8_t* m_OffsetMetadataPtsAddr;
    std::vector<uint8_t> m_decodedSliceHeader;
    int m_removalDelay;
    bool m_spsChangeWarned;
    // Whether the two flags above hold a real stream's values yet, as opposed to the
    // constructor's false. Without it the first SPS was compared against the defaults and
    // reported as a change.
    bool m_spsFlagsKnown;
    // A unit that ends before its fields do (a damaged or cut recording) is skipped or guessed, with one warning
    // of each kind.
    int m_damagedUnitWarnings = 0;
    int m_damagedSliceWarnings = 0;
    // The order of each picture of the GOP so far and of the GOP before, as an offset from its place in decode order,
    // taken where checkPyramid receives it. A picture whose header is damaged takes the offset of the picture at the
    // same place in the GOP before.
    std::vector<int> m_gopOrderOffsets;
    std::vector<int> m_prevGopOrderOffsets;
    bool isDamagedUnit(int nalRez, const uint8_t* nextNal) const;
    void warnDamagedUnit(const char* name);
};

#endif
