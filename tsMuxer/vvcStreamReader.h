#ifndef VVC_STREAM_READER_H_
#define VVC_STREAM_READER_H_

#include <map>

#include "abstractDemuxer.h"
#include "mpegStreamReader.h"
#include "vvc.h"

class VVCStreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    VVCStreamReader();
    ~VVCStreamReader() override;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    [[nodiscard]] bool needSPSForSplit() const override { return false; }

   protected:
    const CodecInfo& getCodecInfo() override { return vvcCodecInfo; }
    int intDecodeNAL(uint8_t* buff) override;
    bool unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    bool getInterlaced() override { return false; }
    bool isIFrame() override { return m_lastIFrame; }

    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    int getFrameDepth() override { return m_frameDepth; }
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    void onSplitEvent() override { m_firstFileFrame = true; }
    bool skipNal(uint8_t* nal) override;

   private:
    [[nodiscard]] bool isSlice(VvcUnit::NalType nalType) const;
    [[nodiscard]] bool isSuffix(VvcUnit::NalType nalType) const;
    void incTimings();
    int toFullPicOrder(const VvcSliceHeader* slice, int pic_bits);
    int placesOfLeadingPictures(uint8_t* nal, uint8_t* end) const;
    static void storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd);
    uint8_t* writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const;
    uint8_t* writeNalPrefix(uint8_t* curPos) const;

    typedef std::map<int, VvcVpsUnit*> VPSMap;

    VvcVpsUnit* m_vps;
    VvcSpsUnit* m_sps;
    VvcPpsUnit* m_pps;
    VvcSliceHeader* m_slice;
    bool m_firstFrame;
    bool m_subProfilesWarned = false;  // the warning that the sub-profiles do not fit the descriptor was given

    int m_frameNum;
    int m_fullPicOrder;
    int m_picOrderBase;
    int m_restartLeading = 0;  // placesOfLeadingPictures of an IDR picture after the first picture
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

    // The picture order count of the last picture header unit, for the slices after it that do not carry their own
    // picture header, and whether that unit was damaged.
    uint16_t m_phPocLsb = 0;
    bool m_phDamaged = false;
    // As for HEVC: damaged parameter sets and picture headers, warned once of each kind.
    int m_damagedUnitWarnings = 0;
    int m_damagedSliceWarnings = 0;
    std::vector<int> m_gopOrderOffsets;
    std::vector<int> m_prevGopOrderOffsets;
    int guessPicOrder(VvcUnit::NalType nalType);
    void notePicOrder(VvcUnit::NalType nalType);
    template <typename Unit>
    void keepLastGoodUnit(Unit*& unit, MemoryBlock& lastGood, const char* name);
};

#endif  // _VVC_STREAM_READER_H_
