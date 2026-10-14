#ifndef VIDEO_COPY_STREAM_READER_H_
#define VIDEO_COPY_STREAM_READER_H_

#include <string>
#include <vector>

#include "abstractStreamReader.h"

// MPEG-4 Part 2 and ProRes video from an MP4 or MOV file, copied picture for picture into a Matroska output. tsMuxeR
// has no reader that understands either, and neither has a form in a transport stream or on a disc; Matroska keeps
// each picture as the source has it. The MP4 reader (MovParsedVideoCopyData) hands the track on as records:
//
//     "VCFG"  be16 width, be16 height, be32 FourCC, be32 length, the configuration (MPEG-4: the decoder specific
//             information of the esds, the VOS and VOL headers), once at the start
//     "VCPY"  be64 pts, be64 dts, be64 duration (INTERNAL_PTS_FREQ units), u8 key frame, be32 length, the picture
//
// and this reader turns each picture record into one packet with the source's own times.
class VideoCopyStreamReader final : public AbstractStreamReader
{
   public:
    explicit VideoCopyStreamReader(bool prores);
    ~VideoCopyStreamReader() override = default;

    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    int64_t getProcessedSize() override { return m_processedSize; }
    [[nodiscard]] int64_t getDeliveredFrames() const override { return m_frames; }
    const CodecInfo& getCodecInfo() override;

    // What a listing says: the codec and the picture size, from the configuration record
    static CheckStreamRez checkStream(const uint8_t* buffer, int len, bool prores);

    [[nodiscard]] unsigned width() const { return m_width; }
    [[nodiscard]] unsigned height() const { return m_height; }
    [[nodiscard]] uint32_t fourcc() const { return m_fourcc; }
    // The frame rate the first two pictures give, 0 before then
    [[nodiscard]] double fps() const { return m_fps; }

   private:
    bool m_prores;
    unsigned m_width = 0;
    unsigned m_height = 0;
    uint32_t m_fourcc = 0;
    double m_fps = 0;
    int64_t m_firstDuration = 0;
    std::vector<uint8_t> m_config;  // MPEG-4: put in front of the first picture, as Matroska has no place for it
    bool m_configSent = false;
    std::vector<uint8_t> m_input;
    size_t m_inputPos = 0;
    std::vector<uint8_t> m_packet;
    int64_t m_processedSize = 0;
    int64_t m_frames = 0;
};

#endif
