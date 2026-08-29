#ifndef AV1_STREAM_READER_H_
#define AV1_STREAM_READER_H_

#include <vector>

#include "abstractDemuxer.h"
#include "av1.h"
#include "mpegStreamReader.h"

class AV1StreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    AV1StreamReader();
    ~AV1StreamReader() override = default;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    [[nodiscard]] bool needSPSForSplit() const override { return false; }
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;

   protected:
    const CodecInfo& getCodecInfo() override;
    int intDecodeNAL(uint8_t* buff) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    bool getInterlaced() override { return false; }
    bool isIFrame() override { return m_lastIFrame; }

    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    void onSplitEvent() override { m_firstFileFrame = true; }

   private:
    void incTimings();
    uint8_t* writeNalPrefix(uint8_t* curPos) const;
    uint8_t* writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const;

    /*
     * An AV1 elementary stream on disk is written in the low overhead bitstream format of the
     * AV1 specification: a bare chain of OBUs, each carrying obu_has_size_field and a leb128
     * length, with NO start codes anywhere. tsMuxeR frames AV1 internally the way it frames
     * H.264 and HEVC instead, a start code before every OBU with the size field cleared and an
     * emulation prevented payload, which is what matroskaParser and movDemuxer already build
     * out of container OBUs. These turn the one into the other as the file is read.
     */
    static bool isLowOverhead(const uint8_t* buf, const uint8_t* end);
    size_t convertPending(bool lastBlock);

    enum class Av1Framing
    {
        UNDECIDED,
        START_CODES,
        LOW_OVERHEAD
    };
    Av1Framing m_framing;
    bool m_brokenObuWarned;
    std::vector<uint8_t> m_pending;     // source bytes not yet converted, an OBU may span blocks
    std::vector<uint8_t> m_convBuffer;  // front pad plus the converted OBUs handed to the base

    Av1SequenceHeader m_seqHdr;
    bool m_seqHdrFound;
    bool m_firstFrame;
    bool m_lastIFrame;
    bool m_firstFileFrame;
    int m_frameNum;

    MemoryBlock m_seqHdrBuffer;

    // Fields for the AV1 video descriptor (from sequence header or container config)
    uint8_t m_hdrWcgIdc;
};

#endif  // AV1_STREAM_READER_H_
