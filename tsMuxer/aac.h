#ifndef AAC_CODEC_H_
#define AAC_CODEC_H_

#include <types/types.h>

#include <string>
#include <vector>

static constexpr int AAC_HEADER_LEN = 7;

// A program config element (ISO/IEC 14496-3, 4.4.1.1): the channel layout of AAC whose channel configuration is 0.
struct AacProgramConfig
{
    std::vector<uint8_t> bits;     // its fields up to its byte alignment, one bit per entry
    std::vector<uint8_t> comment;  // the comment bytes after the alignment
    int channels = 0;
};

class AACCodec
{
   public:
    AACCodec()
        : m_id(0),
          m_layer(0),
          m_channels(0),
          m_sample_rate(48000),
          m_samples(0),
          m_bit_rate(0),
          m_sample_rates_index(0),
          m_channels_index(0),
          m_profile(0),
          m_rdb(0)
    {
    }
    // tagCeiling: past the last byte of a VERIFIED metadata tag at the search position, below
    // which a sync may be disbelieved. Pass buffer, or nullptr, to disbelieve nothing.
    static uint8_t* findAacFrame(uint8_t* buffer, const uint8_t* end, const uint8_t* tagCeiling);
    static int getFrameSize(const uint8_t* buffer);
    bool decodeFrame(uint8_t* buffer, const uint8_t* end);
    void buildADTSHeader(uint8_t* buffer, unsigned frameSize);
    void setSampleRate(const int value) { m_sample_rate = value; }
    void readConfig(uint8_t* buff, int size);
    // The element that carries m_pce inside a frame, as whole bytes: empty without a program config element.
    [[nodiscard]] const std::vector<uint8_t>& inBandPce() const { return m_inBandPce; }
    // An AudioSpecificConfig of 2 bytes, or with the program config element when the stream has one.
    [[nodiscard]] std::vector<uint8_t> audioSpecificConfig() const;
    // The output sample rate of an AudioSpecificConfig that names HE-AAC (SBR, object type 5) or HE-AAC v2 (PS, 29)
    // as its object type; 0 for any other.
    static int explicitSbrRate(const uint8_t* config, int size);
    // The srcaacsbr track parameter for such a config, "<output rate>:<the config in hex>"; empty for any other.
    static std::string sbrParam(const uint8_t* config, int size);

    int m_id;
    int m_layer;
    uint8_t m_channels;
    int m_sample_rate;
    int m_samples;
    int m_bit_rate;
    uint8_t m_sample_rates_index;
    uint8_t m_channels_index;
    uint8_t m_profile;
    uint8_t m_rdb;       // ch, sr;
    bool m_crc = false;  // the ADTS header is followed by its CRC (protection_absent 0)
    // channel configuration 0 only
    AacProgramConfig m_pce;

   private:
    std::vector<uint8_t> m_inBandPce;
    void setPce(const AacProgramConfig& pce);
};

#endif
