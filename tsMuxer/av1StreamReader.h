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
    // From an IVF file, the bytes read from it, as for VP8 and VP9 (vpxStreamReader.cpp)
    int64_t getProcessedSize() override;
    int prepareDemuxPacket(uint8_t* data, int size, const uint8_t** outData) override;

    // The last sequence header OBU of the stream as a file stores it: with its size field, without emulation
    // prevention. Empty before one was found.
    [[nodiscard]] std::vector<uint8_t> sequenceHeaderObu() const;

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
    void onSplitEvent() override
    {
        m_firstFileFrame = true;
        m_demuxSawTd = false;  // a new part is a new file and starts with a temporal delimiter too
    }

   private:
    void incTimings();
    void setHdrWcgIdc();  // m_hdrWcgIdc from m_seqHdr
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
        LOW_OVERHEAD,
        IVF,     // low overhead temporal units, each behind the 12 byte frame header of an IVF file
        ANNEX_B  // length delimited temporal units (Annex B of the AV1 specification), read as IVF is
    };
    static bool isAnnexB(const uint8_t* buf, const uint8_t* end);
    Av1Framing m_framing;
    bool m_brokenObuWarned;
    std::vector<uint8_t> m_pending;      // source bytes not yet converted, an OBU may span blocks
    std::vector<uint8_t> m_ivfRaw;       // IVF (or Annex B) bytes whose frame is not complete yet
    int m_ivfHeaderLeft = 0;             // bytes of the IVF file header still to skip
    int64_t m_ivfRead = 0;               // the bytes of an IVF file handed over so far
    int64_t m_ivfLeftOut = 0;            // of them, the bytes left out after a damaged frame size
    bool m_ivfDamaged = false;           // a frame size no frame can have was met: the rest is left out
    std::vector<uint8_t> m_convBuffer;   // front pad plus the converted OBUs handed to the base
    std::vector<uint8_t> m_demuxBuffer;  // the same OBUs turned back into the on disk form
    bool m_demuxSeqHdrPending;           // a sequence header owed to the head of a demuxed part
    std::vector<uint8_t> m_demuxHeld;    // OBUs seen before the first temporal delimiter of a part
    bool m_demuxSawTd;                   // the part has begun, so nothing is held back any more

    Av1SequenceHeader m_seqHdr;
    bool m_seqHdrFound;
    bool m_firstFrame;
    bool m_lastIFrame;
    bool m_firstFileFrame;
    bool m_notAv1 = false;  // checkStream saw data that does not begin as AV1
    // A stream without temporal delimiters, which a temporal unit must begin with: -1 until its first frame is read,
    // then 1 when no delimiter came before it. Its temporal units are then found by their frames (intDecodeNAL), and
    // each gets a delimiter on output (writeAdditionData, prepareDemuxPacket).
    int m_noTd = -1;
    bool m_tdSeen = false;  // a temporal delimiter was read
    int m_frameNum;

    MemoryBlock m_seqHdrBuffer;

    // Fields for the AV1 video descriptor (from sequence header or container config)
    uint8_t m_hdrWcgIdc;
};

#endif  // AV1_STREAM_READER_H_
