// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef MLP_CODEC_H_
#define MLP_CODEC_H_

#include <types/types.h>

static constexpr int MLP_HEADER_LEN = 7;

// Smallest buffer MLPCodec::decodeFrame() can reach a verdict on for a MAJOR SYNC: the 4 byte
// access unit header plus 132 bits of major sync header, which ends inside byte 20. Below this
// it can only answer "false", which is not the same as "broken", hence NOT_ENOUGH_BUFFER.
// Note this does NOT bound isMinorSync(), which walks 4 + 4 * m_substreams bytes; that is why
// m_substreams is clamped where it is parsed.
static constexpr int MLP_FULL_HEADER_LEN = 21;

enum class MlpSubType
{
    stUnknown,
    stTRUEHD,
    stMLP
};

class MLPCodec
{
   public:
    MLPCodec()
        : m_channels(0), m_samples(0), m_samplerate(0), m_bitrate(0), m_substreams(0), m_subType(MlpSubType::stUnknown)
    {
    }
    static uint8_t* findFrame(uint8_t* buffer, const uint8_t* end);
    static int getFrameSize(const uint8_t* buffer);
    bool decodeFrame(uint8_t* buffer, uint8_t* end);
    bool isMinorSync(const uint8_t* buffer, uint8_t* end) const;
    [[nodiscard]] uint64_t getFrameDuration() const;
    static int mlp_samplerate(int ratebits);

    uint8_t m_channels;
    int m_samples;
    int m_samplerate;  // Sample rate of first substream
    int m_bitrate;     // Peak bitrate for VBR, actual bitrate for CBR
    uint8_t m_substreams;
    MlpSubType m_subType;
};

#endif
