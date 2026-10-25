#ifndef MLP_STREAM_READER_H_
#define MLP_STREAM_READER_H_

#include "avPacket.h"
#include "mlpCodec.h"
#include "simplePacketizerReader.h"

class MLPStreamReader : public SimplePacketizerReader, public MLPCodec
{
   public:
    MLPStreamReader() { m_totalTHDSamples = 0; }
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
    void addTimeGap(int64_t atNs, int64_t gapNs) override { m_thdGaps.emplace_back(atNs, gapNs); }

   protected:
    int64_t m_totalTHDSamples;
    bool m_coreSaid = false;  // the AC-3 core left out was named once
    // The access units before the first major sync, which cannot be decoded and are left out, counted while no
    // access unit has been handed on yet; -1 when they could not be counted
    int m_preSyncUnits = 0;
    bool m_started = false;
    // The gaps of addTimeGap: the times of the units are counted in samples (m_totalTHDSamples), from the first
    // access unit of the track, not by the base class
    std::vector<std::pair<int64_t, int64_t>> m_thdGaps;

   private:
};

#endif
