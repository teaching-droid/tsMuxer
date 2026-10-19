#ifndef MPEG2_STREAM_READER_H_
#define MPEG2_STREAM_READER_H_

#include "avCodecs.h"
#include "mpegStreamReader.h"
#include "mpegVideo.h"

class MPEG2StreamReader final : public MPEGStreamReader
{
   public:
    MPEG2StreamReader() : m_sequence(0), m_frame(0)
    {
        m_isFirstFrame = true;
        spsFound = false;
        m_framesAtGop = 0;
        m_lastRef = -1;
        m_seqExtFound = false;
        m_streamMsgPrinted = false;
        m_lastIFrame = false;
        m_longCodesAllowed = false;
    }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    void applyDiscoveryData(const StreamDiscoveryData& data) override;

    [[nodiscard]] unsigned getStreamWidth() const override { return m_sequence.width; }
    [[nodiscard]] unsigned getStreamHeight() const override { return m_sequence.height; }
    [[nodiscard]] int getStreamHDR() const override { return 0; }
    bool getInterlaced() override { return !m_sequence.progressive_sequence; }

    // An MPEG-1 video stream has no sequence extension; every MPEG-2 stream has one right after its sequence header
    [[nodiscard]] bool isMpeg1() const { return spsFound && !m_seqExtFound; }

   protected:
    const CodecInfo& getCodecInfo() override { return mpeg2CodecInfo; }
    int intDecodeNAL(uint8_t* buff) override;
    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    void updateStreamAR(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    double getStreamFPS(void* curNalUnit) override { return m_sequence.getFrameRate(); }
    bool isIFrame() override { return m_lastIFrame; }

   private:
    bool m_streamMsgPrinted;
    int m_lastRef;
    int m_framesAtGop;
    bool m_isFirstFrame;
    bool spsFound;
    bool m_seqExtFound;
    bool m_lastIFrame;
    // The last I or P picture: when it is shown, and for how long
    bool m_anchorFound = false;
    int64_t m_anchorPts = 0;
    int64_t m_anchorDuration = 0;
    // When the next B picture is shown
    int64_t m_nextShownPts = PTS_CONST_OFFSET;
    MPEGSequenceHeader m_sequence;
    MPEGGOPHeader m_gop;
    MPEGPictureHeader m_frame;
    int findFrameExt(uint8_t* buffer);
    [[nodiscard]] int64_t pictureDuration(bool repeatFirstField, bool topFieldFirst) const;
    int timeOfFollowingB(uint8_t* buff, int64_t& shownFor);
    int decodePicture(uint8_t* buff);
    int processExtStartCode(uint8_t* buff);
    int processSeqStartCode(uint8_t* buff);
};

#endif
