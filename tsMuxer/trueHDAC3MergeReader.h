#ifndef TRUE_HD_AC3_MERGE_READER_H_
#define TRUE_HD_AC3_MERGE_READER_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "abstractDemuxer.h"
#include "ac3Codec.h"
#include "avPacket.h"
#include "mlpStreamReader.h"

// Interleaves a standalone TrueHD (A_MLP) track with a separate AC-3 track from the
// same MKV for Blu-ray–style TrueHD + core muxing (merge-ac3-track meta option).

class TrueHDAC3MergeReader final : public MLPStreamReader
{
   public:
    // The progress figure sums this over every muxed track and divides by the size of the
    // source files. The AC-3 stream folded in here is part of that size, so if its bytes are
    // left out the total can never be reached: a mux of a file whose AC-3 track is one
    // twentieth of a percent of it stopped at 99.4 and stayed there. Measured with and
    // without the merge on one file: 99.3 against 100.0.
    int64_t getProcessedSize() override { return MLPStreamReader::getProcessedSize() + m_ac3BytesConsumed; }

    explicit TrueHDAC3MergeReader(const std::map<std::string, std::string>& addParams);
    TrueHDAC3MergeReader(const TrueHDAC3MergeReader&) = delete;
    TrueHDAC3MergeReader& operator=(const TrueHDAC3MergeReader&) = delete;

    ~TrueHDAC3MergeReader() override;

    [[nodiscard]] int mergeAc3TrackPid() const { return m_mergeAc3Pid; }

    // The AC-3 companion is a stream in its own right, and a container that cannot carry it inside
    // the lossless track has to be told what it is. A disc TrueHD track answers the same question
    // through AC3Codec::isTrueHD, which this reader is not in a position to answer because its two
    // halves came from two separate sources. These figures describe the CORE, not the lossless
    // stream the rest of this reader reports on, and they are read from the first core frame that
    // actually parsed rather than from the meta file.
    [[nodiscard]] bool hasAc3Core() const { return m_ac3CoreSampleRate > 0; }
    [[nodiscard]] int ac3CoreSampleRate() const { return m_ac3CoreSampleRate; }
    [[nodiscard]] uint8_t ac3CoreChannels() const { return m_ac3CoreChannels; }

    void setNewStyleAudioPES(bool value) { m_useNewStyleAudioPES = value; }

    void setAc3SideData(const uint8_t* data, uint32_t len);

    const CodecInfo& getCodecInfo() override;
    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket) override;
    [[nodiscard]] bool needMPLSCorrection() const override;
    const std::string getStreamInfo() override;

   private:
    struct Ac3QueuedFrame
    {
        std::vector<uint8_t> data;
        int samples = 0;
        int sample_rate = 0;
    };

    void extractAc3FramesFromAccum();
    void fillDelayedFromQueue();
    void reportCoreCoverage();

    int m_mergeAc3Pid;
    bool m_useNewStyleAudioPES;

    std::vector<uint8_t> m_ac3Accum;
    std::deque<Ac3QueuedFrame> m_ac3FrameQueue;

    bool m_thdDemuxWaitAc3;
    MemoryBlock m_delayedAc3Buffer;
    MemoryBlock m_immediateAc3Buffer;
    AVPacket m_delayedAc3Packet;
    int m_demuxedTHDSamplesForAc3;
    int64_t m_nextAc3Time;
    int m_ac3SamplesPerSyncFrame;
    int m_pendingEmitSamples;
    int m_pendingEmitSampleRate;

    // A short AC-3 source used to be merged in silence: the lossless track came out complete, the
    // core simply stopped part way, and the mux reported success. Nothing in the log said that half
    // the title had no compatibility core under it. These two count what actually went out so the
    // gap can be reported at the end, when it is a fact rather than a guess about the meta file.
    int64_t m_ac3FramesEmitted;
    // Bytes of the merged AC-3 stream this reader has taken in. They arrive through
    // setAc3SideData rather than setBuffer, so nothing upstream counts them.
    int64_t m_ac3BytesConsumed = 0;
    bool m_coverageReported;

    // Taken from the first AC-3 frame that parsed, so they are a measurement and not a promise.
    int m_ac3CoreSampleRate;
    uint8_t m_ac3CoreChannels;

    struct Ac3FrameParser : AC3Codec
    {
        int parse(uint8_t* b, uint8_t* e, int& sk) { return decodeFrame(b, e, sk); }
        uint8_t* findAc3Sync(uint8_t* b, const uint8_t* e) { return findFrame(b, e); }
        [[nodiscard]] int frameSamples() const { return m_samples; }
        [[nodiscard]] int frameSampleRate() const { return m_sample_rate; }
        [[nodiscard]] uint8_t frameChannels() const { return m_channels; }
    };
    Ac3FrameParser m_ac3Parser;
};

#endif
