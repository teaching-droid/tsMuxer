// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef DTS_STREAM_READER_
#define DTS_STREAM_READER_

#include <vector>

#include "simplePacketizerReader.h"

class DTSStreamReader final : public SimplePacketizerReader
{
   public:
    enum class DTSHD_SUBTYPE
    {
        DTS_SUBTYPE_UNINITIALIZED,
        DTS_SUBTYPE_MASTER_AUDIO,
        DTS_SUBTYPE_HIGH_RES,
        DTS_SUBTYPE_EXPRESS,
        DTS_SUBTYPE_EX,
        DTS_SUBTYPE_96,
        DTS_SUBTYPE_OTHER
    };

    static constexpr uint32_t DTS_HD_PREFIX = 0x64582025;

    DTSStreamReader() : nblks(0), i_frame_size(0), pi_audio_mode(0), pi_bit_rate_index(0), pi_channels_conf(0)
    {
        pi_sample_rate_index = 0;
        pi_bit_rate = 0;
        m_dts_hd_mode = false;
        m_downconvertToDTS = false;
        m_useNewStyleAudioPES = false;
        m_state = DTSDecodeState::stDecodeDTS;
        m_hdFrameLen = 0;
        m_frameDuration = 0;
        m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_UNINITIALIZED;
        hd_bitDepth = 0;
        hd_pi_lfeCnt = pi_lfeCnt = 0;
        hd_pi_channels = pi_channels = 0;
        m_hdBitrate = 0;
        hd_pi_sample_rate = pi_sample_rate = 0;
        m_isCoreExists = true;
        m_firstCall = true;
        pi_frame_length = 0;  // populated from extension substream header (nuExSSFrameDurationCode) when core is absent
        m_skippingSamples = 0;
        m_dataSegmentLen = 0;
        core_ext_mask = 0;
        m_dtsEsChannels = 0;
        m_testMode = false;
        m_isDtsX = false;
        m_isDtsXImax = false;
        m_dtsxFramesProbed = 0;
        m_dtsxHits = 0;
        m_dtsxImaxHits = 0;
    }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    void setDownconvertToDTS(const bool value) { m_downconvertToDTS = value; }
    [[nodiscard]] bool getDownconvertToDTS() const { return m_downconvertToDTS; }
    [[nodiscard]] DTSHD_SUBTYPE getDTSHDMode() const { return m_hdType; }
    void setNewStyleAudioPES(const bool value) { m_useNewStyleAudioPES = value; }
    int getFreq() override { return hd_pi_sample_rate ? hd_pi_sample_rate : pi_sample_rate; }
    // The core's rate where a DTS-HD part above it has its own. With down-to-dts only the core is written, so the
    // higher rate is not named either.
    int getCoreFreq() override { return m_isCoreExists && hd_pi_sample_rate ? pi_sample_rate : 0; }
    int getAltFreq() override { return m_downconvertToDTS && getCoreFreq() ? getCoreFreq() : getFreq(); }
    uint8_t getChannels() override { return hd_pi_channels ? hd_pi_channels : pi_channels; }
    void applyDiscoveryData(const StreamDiscoveryData& data) override;
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    bool isPriorityData(AVPacket* packet) override;
    bool isIFrame(AVPacket* packet) override { return isPriorityData(packet); }
    void setTestMode(const bool value) override { m_testMode = value; }
    bool isSecondary() override;
    // The bit depth of the DTS-HD extension, 0 without one.
    [[nodiscard]] int getHDBitDepth() const { return hd_bitDepth; }
    // The form a DTS stream stores its words in. A disc and a TS carry the core in 16-bit big-endian words; a stream
    // can also be stored in 16-bit little-endian words, or with 14 bits in each 16-bit word of either order, as on a
    // DTS music CD. The reader turns the other forms into the first one (setBuffer).
    enum class WordForm
    {
        unknown,
        be16,
        le16,
        be14,
        le14
    };
    // The form of the first DTS core frame in [buff, end) that a second frame of the same form follows (or, after a
    // core, a DTS-HD substream); unknown when there is none
    static WordForm wordFormAt(const uint8_t* buff, const uint8_t* end);
    [[nodiscard]] WordForm wordForm() const { return m_words; }
    // The stream in [buff, end) in 16-bit big-endian words; form: its form (not unknown). in: the bytes not used yet
    // (a frame of a 14-bit form, or a byte of a 16-bit word, that the data ends inside), kept for the next call; at the
    // end of the stream (last) also those, as far as they go.
    static void toBigEndian16(WordForm form, std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool last);
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    // in bytes of the source, also for a stream turned into another word form: the progress and the check that the
    // whole source was read compare it with the file size
    int64_t getProcessedSize() override;

   protected:
    int getHeaderLen() override { return DTS_HEADER_SIZE; }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    int wholeFrameSizeAt(const uint8_t* p, const uint8_t* end) override;
    [[nodiscard]] bool declaredByContainer() const override;
    void resetDecodeState() override;
    [[nodiscard]] bool frameProvesItself() const override { return true; }
    uint64_t frameLayoutKey() override;
    double getFrameDuration() override;
    // virtual bool isSubFrame() {return m_state == stDecodeHD2;}
    const CodecInfo& getCodecInfo() override
    {
        if (m_dts_hd_mode)
        {
            if (m_hdType != DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS)
                return dtshdCodecInfo;
        }
        return dtsCodecInfo;
    }

    const std::string getStreamInfo() override;
    bool needSkipFrame(AVPacket& packet) override;
    void writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket) override;

   private:
    DTSHD_SUBTYPE m_hdType;
    enum class DTSDecodeState
    {
        stDecodeDTS,
        stDecodeHD,
        stDecodeHD2
    };
    DTSDecodeState m_state;
    int m_hdFrameLen;
    bool m_useNewStyleAudioPES;
    bool m_dts_hd_mode;
    bool m_downconvertToDTS;
    static const CodecInfo m_codecInfo;
    static constexpr unsigned DTS_HEADER_SIZE = 14;

    // unsigned int i_audio_mode;
    int nblks;
    int i_frame_size;
    int pi_audio_mode;
    int pi_sample_rate;
    int hd_pi_sample_rate;
    unsigned pi_sample_rate_index;
    unsigned pi_bit_rate_index;
    int pi_bit_rate;
    int pi_frame_length;
    int pi_channels_conf;

    uint8_t pi_channels;
    int pi_lfeCnt;

    uint8_t hd_pi_channels;
    int hd_pi_lfeCnt;

    int hd_bitDepth;
    int m_hdBitrate;
    bool m_isCoreExists;
    bool m_firstCall;
    int m_skippingSamples;
    int64_t m_dataSegmentLen;
    int core_ext_mask;
    int m_dtsEsChannels;
    bool m_testMode;
    bool m_isDtsX;
    bool m_isDtsXImax;
    int m_dtsxFramesProbed;
    int m_dtsxHits;
    int m_dtsxImaxHits;

    double m_frameDuration;
    int m_coreFrameSize = 0;        // the length of the last core frame read, for frameLayoutKey
    bool m_coreAlone = false;       // the core just read is sent without its extension, which was cut (decodeFrame)
    int64_t m_cutExtensionAt = -1;  // where in the stream the last cut extension was reported as lost
    int syncInfo16be(const uint8_t* p_buf);
    static const uint8_t* nextFrameInside(const uint8_t* frame, const uint8_t* next, const uint8_t* end);
    int testSyncInfo16be(const uint8_t* p_buf);
    static int buf14To16(uint8_t* p_out, const uint8_t* p_in, int i_in, int i_le);
    static void BufLeToBe(uint8_t* p_out, const uint8_t* p_in, int i_in);
    [[nodiscard]] int getSurroundModeCode() const;
    int decodeHdInfo(uint8_t* buff, const uint8_t* end);
    void probeDtsX(uint8_t* buff, const uint8_t* end);
    void checkIfOnlyHDDataExists(uint8_t* buff, const uint8_t* end);
    // setBuffer: the form of the stream, the bytes not turned into 16-bit big-endian words yet, and what is handed on
    WordForm m_words = WordForm::unknown;
    std::vector<uint8_t> m_wordsPending;
    std::vector<uint8_t> m_wordsOut;
    int64_t m_wordsIn = 0;      // the bytes of the source given to setBuffer
    int64_t m_wordsPassed = 0;  // the bytes handed on for them
    bool m_wordsSaid = false;
};

#endif
