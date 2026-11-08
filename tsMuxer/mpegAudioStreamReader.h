// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef MPEG_AUDIO_STREAM_READER_
#define MPEG_AUDIO_STREAM_READER_

#include "mp3Codec.h"
#include "simplePacketizerReader.h"

class MpegAudioStreamReader final : public SimplePacketizerReader, MP3Codec
{
   public:
    static constexpr uint32_t DTS_HD_PREFIX = 0x64582025;
    MpegAudioStreamReader() = default;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    [[nodiscard]] int getLayer() const { return m_layer; }
    int getFreq() override { return m_sample_rate; }
    uint8_t getChannels() override { return static_cast<uint8_t>(m_nb_channels); }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    int readPacket(AVPacket& avPacket) override;
    // Matroska reads the encoder delay and padding from the first Xing or Info frame and leaves it out itself.
    void keepInfoFrames() { m_keepInfoFrames = true; }

   protected:
    int getHeaderLen() override { return MPEG_AUDIO_HEADER_SIZE; }
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    int wholeFrameSizeAt(const uint8_t* p, const uint8_t* end) override;
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override { return mpegAudioCodecInfo; }
    const std::string getStreamInfo() override;

   private:
    static constexpr int MPEG_AUDIO_HEADER_SIZE = 4;
    bool m_keepInfoFrames = false;
};

#endif
