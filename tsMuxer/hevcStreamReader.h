#ifndef HEVC_STREAM_READER_H_
#define HEVC_STREAM_READER_H_

#include <map>

#include "abstractDemuxer.h"
#include "hevc.h"
#include "mpegStreamReader.h"

class HEVCStreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    HEVCStreamReader();
    ~HEVCStreamReader() override;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int setDoViDescriptor(uint8_t* dstBuff) const;
    // Shared derivation behind both the Blu-ray descriptor and the Matroska record.
    bool getDoViParams(int& profile, int& level, int& compatibility, bool& isDVBLOut) const;
    // Writes 24 bytes and returns the Matroska BlockAddIDType (dvcC or dvvC), or 0 if not DV.
    [[nodiscard]] uint32_t buildDoViConfigRecord(uint8_t* dst) const;
    // The same 24 bytes for a MERGED dual layer track. Called on the BASE layer reader.
    [[nodiscard]] uint32_t buildDoViConfigRecordDualLayer(uint8_t* dst, const HEVCStreamReader& el) const;
    [[nodiscard]] uint32_t buildDoViConfigRecordProfile81(uint8_t* dst) const;
    // The profile / level tables, shared so there is exactly one copy of each.
    void doViProfileAndCompatibility(bool isEnhancementLayer, int& profile, int& compatibility) const;
    static int doViLevelFor(unsigned width, uint32_t pixelRate);
    [[nodiscard]] uint32_t doViPixelRate() const;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    // Force general_level_idc in every VPS and SPS written out, in units of 30 (level 5.1 = 153).
    // Zero leaves the stream alone. The H.264 reader has had this for years (setForceLevel there);
    // without it on HEVC the only way to change a level is to re-encode the whole track.
    void setForceLevel(const uint8_t value) { m_forcedLevel = value; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillVideoDiscoveryData(StreamDiscoveryData& data) override;
    [[nodiscard]] bool needSPSForSplit() const override { return false; }

   protected:
    const CodecInfo& getCodecInfo() override { return hevcCodecInfo; }
    int intDecodeNAL(uint8_t* buff) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    [[nodiscard]] int getStreamHDR() const override;
    [[nodiscard]] bool getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const override;
    bool getInterlaced() override { return false; }
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
    static void storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd);
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
    int m_seiParseWarns = 0;
    HevcSliceHeader* m_slice;
    bool m_firstFrame;

    int m_frameNum;
    int m_fullPicOrder;
    int m_picOrderBase;
    int m_frameDepth;

    int m_picOrderMsb;
    int m_prevPicOrder;
    bool m_lastIFrame;

    MemoryBlock m_vpsBuffer;
    MemoryBlock m_spsBuffer;
    MemoryBlock m_ppsBuffer;
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
