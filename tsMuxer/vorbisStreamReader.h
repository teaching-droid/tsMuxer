#ifndef VORBIS_STREAM_READER_H_
#define VORBIS_STREAM_READER_H_

#include <vector>

#include "simplePacketizerReader.h"

// Vorbis audio, read from Matroska and WebM. Like Opus, a Vorbis packet has no sync code: the Matroska parser
// (ParsedMarkedTrackData) puts a 4 byte big endian length in front of each, and the three header packets of the
// track's CodecPrivate in front of the first, behind the mark "VORBISCP" and their own length.
//
// A packet's length in samples is not written anywhere. It follows from the setup header: each packet names a mode,
// the mode says a short or a long block, and a packet gives a quarter of the block before it and a quarter of its
// own. The first gives nothing, and is timed as a container places it: one packet length early, with the length of
// a packet whose block before it was the same as its own, so the sound starts at the time of the second.
//
// Vorbis has no form in a transport stream or on a Blu-ray disc: it goes into a Matroska output, or is demuxed into an
// Ogg file (writeAdditionData). MuxerManager::checkTrackList refuses the rest.
class VorbisStreamReader final : public SimplePacketizerReader
{
   public:
    VorbisStreamReader();
    ~VorbisStreamReader() override = default;

    int getFreq() override { return m_sampleRate; }
    uint8_t getChannels() override { return m_channels; }

    // The track's three header packets as Matroska's CodecPrivate holds them, Xiph laced.
    [[nodiscard]] const std::vector<uint8_t>& getCodecPrivate() const { return m_codecPrivate; }

    // Reads the CodecPrivate of a Matroska track (Xiph laced identification, comment and setup headers).
    bool setCodecPrivate(const uint8_t* data, int size);

    // The length in samples of the first packet, which gives no sound: a Matroska output's CodecDelay.
    [[nodiscard]] int leadSamples() const { return m_leadSamples; }

    // The block size of an audio packet, by the modes of the setup header (setCodecPrivate); 0 for any other packet.
    [[nodiscard]] int blockSizeOf(const uint8_t* packet, int size) const;
    [[nodiscard]] int sampleRate() const { return m_sampleRate; }

   protected:
    int getHeaderLen() override { return 4; }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override { return 0; }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override;
    const std::string getStreamInfo() override;
    // Demux: the Ogg page header of each packet, the header packets' pages in front of the first.
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;

   public:
    // Demux: the last page is marked as the end of the stream, its granule position less the samples the source trims
    // from the end of its last packet (its DiscardPadding, setEndTrim).
    bool beforeFileCloseEvent(File& file) override;
    void setEndTrim(const int64_t nanoseconds) { m_endTrimNs = nanoseconds; }

   protected:
   private:
    // An Ogg page header (RFC 3533) for packets of the given sizes, its CRC over the header and the payload.
    int oggPageHeader(uint8_t* dst, const uint8_t* dstEnd, const std::vector<int>& packets, uint8_t headerType,
                      int64_t granule, const uint8_t* payload, int payloadSize);
    bool parseIdentification(const uint8_t* data, int size);
    bool parseSetup(const uint8_t* data, int size);

    std::vector<uint8_t> m_codecPrivate;
    int m_sampleRate;
    uint8_t m_channels;
    int m_nominalBitrate;
    int m_blockSize[2];
    std::vector<bool> m_modeLong;  // the block flag of each mode, from the setup header
    int m_modeBits;                // the bits a packet names its mode in
    int m_prevBlock;               // the block size of the packet before, 0 before the first
    int m_curFrameSamples;
    int m_leadSamples;
    // the three header packets, cut out of the CodecPrivate (setCodecPrivate)
    std::vector<uint8_t> m_headers[3];
    bool m_oggHeadersWritten = false;
    int64_t m_granulePos = 0;
    int m_oggPrevBlock = 0;
    uint32_t m_oggPageSeqNo = 0;
    std::vector<uint8_t> m_lastPage;  // the last packet's page, header and packet, rewritten at the end
    int m_lastPageHeader = 0;
    int64_t m_endTrimNs = 0;
};

#endif  // VORBIS_STREAM_READER_H_
