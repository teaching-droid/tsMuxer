// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef AAC_STREAM_READER_H_
#define AAC_STREAM_READER_H_

#include "aac.h"
#include "simplePacketizerReader.h"

class AACStreamReader final : public SimplePacketizerReader, public AACCodec
{
   public:
    AACStreamReader() = default;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int getFreq() override { return m_sample_rate; }
    uint8_t getChannels() override { return m_channels; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len, ContainerType containerType, int containerDataType,
                               int containerStreamIndex) override;

   protected:
    int getHeaderLen() override;
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override { return findAacFrame(buff, end, m_tagCeilingEnd); }
    int wholeFrameSizeAt(const uint8_t* p, const uint8_t* end) override;
    // ADTS, stream type 0x0F; LATM (0x11) is another framing, which this reader does not read
    [[nodiscard]] bool declaredByContainer() const override
    {
        return (m_containerType == ContainerType::ctTS || m_containerType == ContainerType::ctM2TS) &&
               m_containerDataType == static_cast<int>(StreamType::AUDIO_AAC);
    }
    double getFrameDuration() override { return static_cast<double>(m_samples) * INTERNAL_PTS_FREQ / m_sample_rate; }
    const CodecInfo& getCodecInfo() override { return aacCodecInfo; }
    const std::string getStreamInfo() override;

   private:
    // What the listing saw: the audio bits and the playing time of every frame in the data it was given.
    int64_t m_seenBits = 0;
    double m_seenSeconds = 0;
};

#endif
