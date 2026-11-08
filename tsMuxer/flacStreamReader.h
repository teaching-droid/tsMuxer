// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef FLAC_STREAM_READER_H_
#define FLAC_STREAM_READER_H_

#include <vector>

#include "avPacket.h"
#include "simplePacketizerReader.h"

class FLACStreamReader final : public SimplePacketizerReader
{
    friend class MatroskaMuxer;

   public:
    FLACStreamReader();
    ~FLACStreamReader() override = default;

    int getMaxFrameSize() override
    {
        return DEFAULT_FILE_BLOCK_SIZE;
    }  // FLAC frames can be very large for multichannel hi-res audio
    int getFreq() override { return m_sampleRate; }
    uint8_t getChannels() override { return m_channels; }
    int getBitsPerSample() const { return m_bitsPerSample; }

    /// Store the raw CodecPrivate blob from the MKV container.
    /// Expected layout: "fLaC" (4 bytes) + metadata blocks (STREAMINFO first).
    void setCodecPrivate(const uint8_t* data, int size);

    /// Return the stored CodecPrivate (fLaC + STREAMINFO).
    const std::vector<uint8_t>& getCodecPrivate() const { return m_codecPrivate; }

    void applyDiscoveryData(const StreamDiscoveryData& data) override;

   protected:
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    int getHeaderLen() override { return 4; }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override;
    const std::string getStreamInfo() override;
    void setTestMode(bool value) override
    {
        m_testMode = value;
        m_testPrevValid = false;
        m_testLastFrame = nullptr;
    }
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;

   private:
    bool m_testMode;
    bool m_firstFrame;

    int m_sampleRate;
    uint8_t m_channels;
    int m_bitsPerSample;
    int m_minBlockSize;
    int m_maxBlockSize;
    int m_curBlockSize;

    /// Raw CodecPrivate data (fLaC marker + STREAMINFO metadata block, possibly more).
    std::vector<uint8_t> m_codecPrivate;
    // The file header (marker and metadata blocks) is counted with the tags, once: a picture in it is no sound, and
    // can be larger than the sound of a short file (see SimplePacketizerReader::getFrameBytes)
    bool m_headerCounted = false;
    void countHeader(int64_t len);

    // What one frame header says that must stay the same from frame to frame of one stream, and its number.
    struct FrameFields
    {
        bool variableBlocks = false;  // the blocking strategy bit
        int sampleRateCode = 0;
        int channels = 0;  // the channel count; the stereo modes all count as 2
        int sampleSizeCode = 0;
        uint64_t number = 0;  // frame number (fixed block size) or first sample number (variable block size)
        int blockSize = 0;
    };

    // Detection only: the frame accepted last, so that the next one can be checked to follow it.
    bool m_testPrevValid = false;
    FrameFields m_testPrev;
    const uint8_t* m_testLastFrame = nullptr;
    int m_testLastLen = 0;

    /// Parse a 34-byte STREAMINFO block to extract audio parameters.
    bool parseStreamInfo(const uint8_t* data, int size);

    /// Parse the FLAC frame header starting at \p buff and return the block size.
    /// Returns 0 on failure. \p fields, when given, receives what the header says.
    int parseFrameHeader(const uint8_t* buff, const uint8_t* end, FrameFields* fields = nullptr) const;

    /// Detection only: the length of the frame at \p buff, or 0 when it is not a whole FLAC frame that follows the
    /// one accepted before it.
    int testFrameLength(uint8_t* buff, uint8_t* end, const FrameFields& fields);
};

#endif  // FLAC_STREAM_READER_H_
