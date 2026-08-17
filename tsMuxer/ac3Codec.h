#ifndef AC3_CODEC_H_
#define AC3_CODEC_H_

#include "avPacket.h"
#include "mlpCodec.h"

struct CodecInfo;

class AC3Codec
{
   public:
    static constexpr int AC3_HEADER_SIZE = 7;

    enum class AC3State
    {
        stateDecodeAC3,
        stateDecodeAC3Plus,
        stateDecodeTrueHDFirst,
        stateDecodeTrueHD
    };

    enum class AC3ParseError
    {
        NO_ERROR = 0,
        SYNC = -1,
        BSID = -2,
        SAMPLE_RATE = -3,
        FRAME_SIZE = -4,
        CRC2 = -5,
        NOT_ENOUGH_BUFFER = -10
    };

    AC3Codec()
        : m_fscod(0),
          m_frmsizecod(0),
          m_bsmod(0),
          m_acmod(0),
          m_halfratecod(0),
          m_sample_rate(0),
          m_frame_size(0),
          m_samples(0)
    {
        m_downconvertToAC3 = m_true_hd_mode = false;
        m_state = AC3State::stateDecodeAC3;
        m_waitMoreData = false;
        AC3Codec::setTestMode(false);
        m_frameDuration = 0;
        m_bit_rateExt = 0;
        m_bit_rate = 0;
        m_channels = 0;
        m_lfeon = 0;
        m_extChannelsExists = false;
        m_bsid = m_bsidBase = 0;
        m_strmtyp = 0;
        m_dsurmod = 0;
        m_mixinfoexists = false;
        m_isAtmos = false;
        m_jocObjects = 0;
        m_atmosHits = 0;
        m_atmosFramesProbed = 0;
    }

    virtual ~AC3Codec() = default;

    virtual int getHeaderLen() { return AC3_HEADER_SIZE; }
    [[nodiscard]] bool isEAC3() const { return m_bsid > 10; }
    [[nodiscard]] bool isAC3() const { return m_bsidBase > 0; }
    void setDownconvertToAC3(const bool value) { m_downconvertToAC3 = value; }
    [[nodiscard]] bool getDownconvertToAC3() const { return m_downconvertToAC3; }
    [[nodiscard]] bool isTrueHD() const { return m_true_hd_mode; }
    virtual void setTestMode(const bool value) { m_testMode = value; }
    [[nodiscard]] bool getTestMode() const { return m_testMode; }

   protected:
    int decodeFrame(uint8_t* buf, uint8_t* end, int& skipBytes);
    static uint8_t* findFrame(uint8_t* buffer, const uint8_t* end);
    [[nodiscard]] uint64_t getFrameDuration() const;
    virtual const CodecInfo& getCodecInfo();
    virtual const std::string getStreamInfo();

    AC3State m_state;
    bool m_waitMoreData;
    bool m_downconvertToAC3;
    bool m_true_hd_mode;
    uint8_t m_fscod;
    int m_frmsizecod;
    uint8_t m_bsid;
    uint8_t m_bsidBase;
    uint8_t m_strmtyp;
    uint8_t m_bsmod;
    uint8_t m_acmod;
    uint8_t m_dsurmod;
    uint8_t m_lfeon;
    uint8_t m_halfratecod;
    int m_sample_rate;
    int m_bit_rate;
    uint8_t m_channels;
    int m_frame_size;
    bool m_mixinfoexists;
    bool m_isAtmos;

    // Dolby Atmos carried in E-AC-3 as joint object coding. The marker sits in the addbsi field
    // and is a Type A extension per ETSI TS 103 420, together with a complexity index that IS the
    // object count.
    //
    // One frame is not enough to say so. The walk to addbsi passes through a dozen variable length
    // fields, so a single frame landing on the pattern by accident used to latch the flag on for
    // the whole track and it was never cleared. Agreement across frames is demanded instead, on
    // the same object count, which is the discipline the DTS:X badge already uses.
    uint8_t m_jocObjects;     // complexity_index_type_a, the number of objects
    int m_atmosHits;          // frames agreeing on it
    int m_atmosFramesProbed;  // frames looked at, so the probe can stop
    static constexpr int ATMOS_MIN_HITS = 2;
    static constexpr int ATMOS_PROBE_FRAMES = 10;  // checkStream decodes this many before asking

    MLPCodec mlp;

    int m_samples;

    uint32_t m_bit_rateExt;
    bool m_extChannelsExists;

    static bool crc32(const uint8_t* buf, int length);
    AC3ParseError parseHeader(uint8_t* buf, const uint8_t* end);

    AC3ParseError testParseHeader(uint8_t* buf, uint8_t* end) const;
    bool testDecodeTestFrame(uint8_t* buf, uint8_t* end) const;

    bool m_testMode;
    int64_t m_frameDuration;
};

#endif
