// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef AC3_STREAM_READER_H_
#define AC3_STREAM_READER_H_

#include <array>
#include <vector>

#include "abstractDemuxer.h"
#include "ac3Codec.h"
#include "avPacket.h"
#include "simplePacketizerReader.h"

class AC3StreamReader final : public SimplePacketizerReader, public AC3Codec
{
   public:
    AC3StreamReader() : m_useNewStyleAudioPES(false)
    {
        m_downconvertToAC3 = m_true_hd_mode = false;
        m_state = AC3State::stateDecodeAC3;
        m_waitMoreData = false;

        m_totalTHDSamples = 0;
        m_nextAc3Time = 0;
        m_needTrueHDProbe = false;
        m_keepDamaged = true;  // AC3Codec::keepDamagedFrame
    }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    void setNewStyleAudioPES(const bool value) { m_useNewStyleAudioPES = value; }
    // The track goes to a transport stream that is neither a disc nor M2TS, where E-AC-3 has the ATSC stream type 0x87
    void setAtscEac3Type(const bool value) { m_atscEac3Type = value; }
    void setTestMode(const bool value) override { AC3Codec::setTestMode(value); }
    int getFreq() override { return m_sample_rate; }
    int getAltFreq() override
    {
        if (m_downconvertToAC3)
            return m_sample_rate;

        return mlp.m_subType == MlpSubType::stUnknown ? m_sample_rate : mlp.m_samplerate;
    }
    uint8_t getChannels() override { return m_channels; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    bool isPriorityData(AVPacket* packet) override;
    bool isIFrame(AVPacket* packet) override { return isPriorityData(packet); }
    bool isSecondary() override;
    void addTimeGap(int64_t atNs, int64_t gapNs) override;
    // TrueHD with its core times its units by samples (readPacketTHD): the gap, as before; AC-3 alone by its data.
    // Which it is, is known once the reader reads (probeTrueHD): decided there (takePendingJumps).
    void addTimeJump(const int64_t atByte, const int64_t toNs, const int64_t atNs, const int64_t gapNs) override
    {
        m_pendingJumps.push_back({atByte, toNs, atNs, gapNs});
    }
    std::vector<std::array<int64_t, 4>> m_pendingJumps;
    void takePendingJumps()
    {
        for (const auto& j : m_pendingJumps)
            if (m_true_hd_mode && !m_downconvertToAC3)
                addTimeGap(j[2], j[3]);
            else
                SimplePacketizerReader::addTimeJump(j[0], j[1], j[2], j[3]);
        m_pendingJumps.clear();
    }

    // ** AC-3 INSIDE A WAV. ** A WAVE file can hold AC-3 or E-AC-3 in place of samples, in two forms. With frames as
    // its data (format tag 0x2000, as an encoder or a capture card writes it) the frames are those of an .ac3 file,
    // and the file is listed and muxed as the AC-3 it holds. Packed for a digital output (IEC 61937 bursts in 16-bit
    // stereo samples, as made to be played over S/PDIF or HDMI) it was listed as LPCM and muxed as such, which plays
    // as noise, and then refused with the reason. The frames are taken out of the bursts now (unpackBursts), in the
    // listing and by the reader of a mux (setBuffer), and the track is the AC-3 or E-AC-3 the file holds.
    enum class WavForm
    {
        none,
        frames,  // AC-3 frames as the data of the file
        bursts   // IEC 61937 bursts in the samples
    };
    // `data` is set to where the data of the file begins. `more`, when given, is set where the bytes are the start
    // of a WAVE file and too few to tell: its data, or its first bursts, begin further on.
    static WavForm wavForm(const uint8_t* buf, int len, int& data, bool* more = nullptr);
    // The frames of the bursts in `in`, added to `out`. What stays in `in` is the start of a burst that goes on in the
    // next block; with `last` nothing stays, and a burst the file ends inside gives the part of its frame that is
    // there.
    // `outside`, when given, is raised by the bytes that are neither a burst nor the zeros between bursts.
    static void unpackBursts(std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool last,
                             int64_t* outside = nullptr);
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    int64_t getProcessedSize() override;

   private:
    enum class Bursts
    {
        unknown,
        no,
        yes
    };
    Bursts m_bursts = Bursts::unknown;
    std::vector<uint8_t> m_burstPending;
    std::vector<uint8_t> m_burstOut;
    int64_t m_burstIn = 0;       // the bytes of the file read so far
    int64_t m_burstPassed = 0;   // the bytes of frames handed on for them
    int64_t m_burstOutside = 0;  // the bytes that were no burst and no filling, left out
    // Find the lossless substream on THIS reader. Only runs for a track the discovery phase already
    // identified as TrueHD, and only once.
    void probeTrueHD();
    void decodeLeadingFrames(uint8_t* frame);
    void findTrueHDMajorSync();
    bool m_needTrueHDProbe;
    bool m_eac3NoCoreReported = false;
    bool m_atscEac3Type = false;  // see setAtscEac3Type
    // E-AC-3 without an AC-3 core: the timestamp of the last independent frame, which the
    // dependent frames after it belong to
    void stampDependentFrame(AVPacket& avPacket);
    int64_t m_lastIndependentPts = -1;

   protected:
    int getHeaderLen() override { return AC3Codec::getHeaderLen(); }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override
    {
        skipBeforeBytes = 0;
        const int len = AC3Codec::decodeFrame(buff, end, skipBytes);
        if (m_frameDamaged)
            noteDamagedFrame(m_readBytes - (m_bufEnd - buff));
        return len;
    }
    // A frame that failed its check sum and is kept (AC3Codec::keepDamagedFrame), at the place at in the data of the
    // track: a warning for the first, a line for each after it, as for a frame left out. A frame is decoded more than
    // once (at a sync and again to send it, and again after more data came), so it is counted by its place.
    void noteDamagedFrame(int64_t at);
    int64_t m_lastDamagedAt = -1;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    int wholeFrameSizeAt(const uint8_t* p, const uint8_t* end) override;
    [[nodiscard]] bool declaredByContainer() const override;
    void resetDecodeState() override;
    [[nodiscard]] bool frameProvesItself() const override { return true; }
    double getFrameDuration() override { return static_cast<double>(AC3Codec::getFrameDuration()); }
    const CodecInfo& getCodecInfo() override { return AC3Codec::getCodecInfo(); }
    const std::string getStreamInfo() override { return AC3Codec::getStreamInfo(); }
    std::string getStreamInfoAt(const uint8_t* frame, const uint8_t* end) override
    {
        return AC3Codec::describeAt(frame, end);
    }
    void writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket) override;

    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    int readPacketTHD(AVPacket& avPacket);

    [[nodiscard]] bool needMPLSCorrection() const override;

   private:
    bool m_useNewStyleAudioPES;

    int64_t m_totalTHDSamples;
    int64_t m_nextAc3Time;
    // The TrueHD access units in front of the first AC-3 core frame, which are left out, counted once from the start of
    // the track; -1 when they could not be counted
    int m_leadUnits = 0;
    bool m_leadCounted = false;
    bool m_leadApplied = false;
    void applyLeadUnits();
    // The gaps of addTimeGap for TrueHD with its AC-3 core (readPacketTHD): its core frames and its access units are
    // timed by clocks of their own (m_nextAc3Time, m_totalTHDSamples), not by the base class, and each takes each gap
    std::vector<std::pair<int64_t, int64_t>> m_coreGaps;
    std::vector<std::pair<int64_t, int64_t>> m_unitGaps;
};

#endif
