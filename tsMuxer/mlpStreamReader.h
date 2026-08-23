#ifndef MLP_STREAM_READER_H_
#define MLP_STREAM_READER_H_

#include "avPacket.h"
#include "mlpCodec.h"
#include "simplePacketizerReader.h"

class MLPStreamReader : public SimplePacketizerReader, public MLPCodec
{
   public:
    MLPStreamReader()
    {
        m_demuxedTHDSamples = 0;
        m_totalTHDSamples = 0;
        m_coreProbed = false;
    }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int getFreq() override { return m_samplerate; }
    uint8_t getChannels() override { return m_channels; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;

   protected:
    int getHeaderLen() override;
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override { return static_cast<double>(MLPCodec::getFrameDuration()); }
    const CodecInfo& getCodecInfo() override { return mlpCodecInfo; }
    const std::string getStreamInfo() override;

    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;

   protected:
    int m_demuxedTHDSamples;
    int64_t m_totalTHDSamples;
    // findFrame is also the resync path, so the "is there a core in front of this" test runs on
    // the first call only, which is the one that sees the start of the stream.
    bool m_coreProbed;

   private:
};

#endif
