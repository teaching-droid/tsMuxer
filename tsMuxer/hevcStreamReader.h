#ifndef HEVC_STREAM_READER_H_
#define HEVC_STREAM_READER_H_

#include <map>
#include <set>

#include "abstractDemuxer.h"
#include "hevc.h"
#include "mpegStreamReader.h"

class HEVCStreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    HEVCStreamReader();
    // hdr10-override: an SDR stream is marked as HDR10 on the disc (getStreamHDR), as asked for in the meta file
    void setHdr10Override(const bool on) { m_hdr10Override = on; }
    ~HEVCStreamReader() override;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int setDoViDescriptor(uint8_t* dstBuff) const;
    // The PID of the base layer when the transport stream muxer has placed this stream as a Dolby Vision enhancement
    // layer on a PID of its own; 0 when this stream's PID carries the base layer (setDoViDescriptor)
    void setDoViBaseLayerPid(const int pid) { m_doViBaseLayerPid = pid; }
    // The derivation behind the Matroska record. The transport stream descriptor states its layers itself.
    bool getDoViParams(int& profile, int& level, int& compatibility, bool& isDVBLOut) const;
    // Said once per stream. doViProfileAndCompatibility is const and is called more than once, from
    // the descriptor and from the Matroska configuration record.
    mutable bool m_warnedDvProfile5 = false;
    // Writes 24 bytes and returns the Matroska BlockAddIDType (dvcC or dvvC), or 0 if not DV.
    [[nodiscard]] uint32_t buildDoViConfigRecord(uint8_t* dst) const;
    // The same 24 bytes for a MERGED dual layer track. Called on the BASE layer reader.
    [[nodiscard]] uint32_t buildDoViConfigRecordDualLayer(uint8_t* dst, const HEVCStreamReader& el) const;
    [[nodiscard]] uint32_t buildDoViConfigRecordProfile81(uint8_t* dst) const;
    // The profile / level tables, shared so there is exactly one copy of each.
    void doViProfileAndCompatibility(bool isEnhancementLayer, int& profile, int& compatibility) const;
    // Says once, for a SINGLE LAYER Dolby Vision stream, whether it leaves a picture that a player
    // without Dolby Vision can show. Profile 5 does not.
    void warnIfDvHasNoFallback();
    static int doViLevelFor(unsigned width, uint32_t pixelRate);
    [[nodiscard]] uint32_t doViPixelRate() const;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    // Force general_level_idc in every VPS and SPS written out, in units of 30 (level 5.1 = 153).
    // Zero leaves the stream alone. The H.264 reader has had this for years (setForceLevel there);
    // without it on HEVC the only way to change a level is to re-encode the whole track.
    void setForceLevel(const uint8_t value) { m_forcedLevel = value; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillVideoDiscoveryData(StreamDiscoveryData& data) override;
    // What the stream's first readable RPU says of the layers: 1 the picture is built with an enhancement layer (the
    // RPU of a profile 7 disc's enhancement layer), 0 a single layer (profile 5 or 8), -1 no RPU read (HevcDvRpuUnit).
    [[nodiscard]] int dvRpuElPresent() const { return m_dvRpuElPresent; }
    [[nodiscard]] bool needSPSForSplit() const override { return false; }

   protected:
    const CodecInfo& getCodecInfo() override { return hevcCodecInfo; }
    int intDecodeNAL(uint8_t* buff) override;
    bool unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] double getSampleAspectRatio() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    [[nodiscard]] int getStreamHDR() const override;
    // This stream's mastering display and light level values, packed as in HDR10_metadata (zeros where none came)
    [[nodiscard]] const unsigned* getHdr10Metadata() const { return m_hdr->hdr10Metadata; }
    // The stream carries a mastering display SEI, HDR10 or not (an SDR stream can have one too)
    [[nodiscard]] bool hasMasteringDisplay() const { return m_hdr->hasMasteringDisplay; }
    [[nodiscard]] bool getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const override;
    bool getInterlaced() override { return false; }
    // Each picture is a field (VUI field_seq_flag), for a Matroska output to say so; the disc path is left as it was
    [[nodiscard]] bool fieldCoded() const { return m_sps && m_sps->field_seq_flag; }
    bool m_fieldsOnDiscSaid = false;  // the warning that a disc takes no HEVC fields was given
    bool isIFrame() override { return m_lastIFrame; }

    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    int getFrameDepth() override { return m_frameDepth; }
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    void onSplitEvent() override { m_firstFileFrame = true; }
    bool skipNal(uint8_t* nal) override;

   private:
    [[nodiscard]] bool isSlice(HevcUnit::NalType nalType) const;
    [[nodiscard]] bool isSuffix(HevcUnit::NalType nalType) const;
    // Reads the enhancement layer's own VPS or SPS out of one wrapper NAL of a merged dual layer
    // track. Keeps the first of each and ignores anything it cannot parse.
    void readElParameterSet(const uint8_t* nal, const uint8_t* nextNal);
    void incTimings();
    int toFullPicOrder(const HevcSliceHeader* slice, unsigned pic_bits);
    int picturesShownBeforeFirst(uint8_t* nal, uint8_t* end) const;
    int placesOfLeadingPictures(uint8_t* nal, uint8_t* end) const;
    int depthOfPeriod(uint8_t* nal, uint8_t* end) const;
    // The full order a picture would get as the next of the period (toFullPicOrder, without moving its state)
    [[nodiscard]] int nextFullPicOrder(const HevcSliceHeader* slice) const;
    std::set<int> m_periodOrders;  // the full orders given since the last IDR or BLA picture, for a join
    bool m_joinRestart = false;    // the picture being read begins a later file of a join as a stream of its own
    static void storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd);
    void captureHdrSei(const uint8_t* nal, const uint8_t* nalEnd);
    // A mastering display SEI makes the stream HDR10 unless its SPS says SDR or HLG (see the definition)
    void markHdr10ByMasteringSei();
    uint8_t* writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const;
    uint8_t* writeNalPrefix(uint8_t* curPos) const;

    typedef std::map<int, HevcVpsUnit*> VPSMap;

    HevcVpsUnit* m_vps;
    HevcSpsUnit* m_sps;
    HevcPpsUnit* m_pps;
    HevcHdrUnit* m_hdr;
    // The ENHANCEMENT layer's own parameter sets, read out of the wrapper NALs of a merged dual
    // layer track. Only the probe fills these, and only to describe the second of the two rows
    // that track is listed as. A profile 7 disc pairs a 3840x2160 base layer with a 1920x1080
    // enhancement layer, so describing both rows from m_sps reported the wrong picture for one.
    HevcVpsUnit* m_elVps = nullptr;
    HevcSpsUnit* m_elSps = nullptr;
    int m_dvRpuElPresent = -1;   // dvRpuElPresent
    int m_doViBaseLayerPid = 0;  // setDoViBaseLayerPid
    int m_seiParseWarns = 0;
    bool m_hdr10Override = false;  // setHdr10Override
    bool m_multiLayer = false;     // a picture of a layer above the base layer was seen (refuseMultiLayer)
    [[noreturn]] void refuseMultiLayer() const;
    HevcSliceHeader* m_slice;
    bool m_firstFrame;

    int m_frameNum;
    int m_fullPicOrder;
    int m_picOrderBase;
    int m_maxFullPicOrder = -1;  // the latest place a picture was given, for a Dolby Vision gap (toFullPicOrder)
    bool m_dvGapReported = false;
    int m_frameDepth;

    int m_picOrderMsb;
    int m_prevPicOrder;
    bool m_picOrderStarted = false;  // the first picture of the stream has set m_picOrderBase
    int m_firstLeading = 0;          // picturesShownBeforeFirst of the first picture, when it is not IRAP
    int m_restartLeading = 0;        // placesOfLeadingPictures of an IDR or BLA picture after the first picture
    bool m_lastIFrame;

    MemoryBlock m_vpsBuffer;
    MemoryBlock m_spsBuffer;
    MemoryBlock m_ppsBuffer;
    int m_damagedUnitWarnings = 0;   // damaged parameter sets passed through, warned once
    int m_damagedSliceWarnings = 0;  // damaged slice headers, warned once
    // The order of each picture of the GOP so far and of the GOP before, as an offset from its place in decode order.
    std::vector<int> m_gopOrderOffsets;
    std::vector<int> m_prevGopOrderOffsets;
    int guessPicOrder(HevcUnit::NalType nalType);
    void notePicOrder(HevcUnit::NalType nalType);
    template <typename Unit>
    void keepLastGoodUnit(Unit*& unit, MemoryBlock& lastGood, const char* name);
    bool m_firstFileFrame;
    int m_vpsCounter;
    int m_vpsSizeDiff;
    uint8_t m_forcedLevel;
    bool m_levelChangeReported;
    // The stream's OWN mastering display and content light level SEI, kept so they can be repeated
    // at every IRAP. Nothing is fabricated; these are the source's bytes.
    MemoryBlock m_masteringSeiBuffer;
    MemoryBlock m_cllSeiBuffer;
    bool m_auHasMasteringSei;
    bool m_auHasCllSei;
    // The kept mastering display unit carries the content light level as well, so repeating it repeats both.
    bool m_cllInMasteringSei;
    bool m_hdrSeiRepeatReported;
    // Lowest slice_type seen in the current access unit, for the pic_type of a generated access
    // unit delimiter. HEVC orders them B=0, P=1, I=2, so the lowest is the most permissive.
    int m_auMinSliceType;
    bool m_audInsertReported;

    // Rewrite general_level_idc in a VPS or SPS that is already sitting in the stream buffer.
    // buff points at the NAL header, nextNal at the start code of the following NAL.
    void applyForcedLevel(HevcUnitWithProfile* unit, uint8_t* buff, const uint8_t* nextNal);
    // Put the HDR SEI back at an IRAP that lacks them. Returns bytes inserted before slicePos.
    int repeatHdrSeiAtIrap(uint8_t* slicePos);
};

#endif  // _HEVC_STREAM_READER_H_
