// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef OPUS_STREAM_READER_H_
#define OPUS_STREAM_READER_H_

#include <vector>

#include "avPacket.h"
#include "simplePacketizerReader.h"

/// Stream reader for Opus audio.
///
/// Opus packets from MKV arrive length-prefixed (4-byte BE) via
/// ParsedOpusTrackData.  The reader uses those length prefixes to
/// locate frame boundaries (Opus itself has no sync codes).
///
/// For demux output the reader produces OGG Opus files: it writes
/// OGG page headers in writeAdditionData() and the raw Opus packet
/// data is appended by the muxer, forming valid OGG pages.
class OpusStreamReader final : public SimplePacketizerReader
{
    friend class MatroskaMuxer;

   public:
    OpusStreamReader();
    ~OpusStreamReader() override = default;

    int getFreq() override { return 48000; }  // Opus always decodes at 48 kHz
    uint8_t getChannels() override { return m_channels; }
    int getOriginalSampleRate() const { return m_originalSampleRate; }
    uint16_t getPreSkip() const { return m_preSkip; }

    /// Store the raw CodecPrivate blob (OpusHead) from the MKV container.
    void setCodecPrivate(const uint8_t* data, int size);

    /// Return the stored CodecPrivate (OpusHead).
    const std::vector<uint8_t>& getCodecPrivate() const { return m_codecPrivate; }

    void applyDiscoveryData(const StreamDiscoveryData& data) override;

    // Demux: the last page is marked as the end of the stream, its granule position less the samples the source trims
    // from the end of its last packet (its DiscardPadding, setEndTrim). A transport stream says them in the control
    // header of the last access unit (writeAdditionData).
    bool beforeFileCloseEvent(File& file) override;
    void setEndTrim(const int64_t nanoseconds) { m_endTrimNs = nanoseconds; }
    // A join: the end padding of each file, in nanoseconds, in the order of the join (the DiscardPadding of its last
    // block). At each joint the end padding of the file before and the pre-skip of the file after add up to samples
    // that are not sound; whole packets of them are left out (readPacket).
    void setJoinEndPadding(std::vector<int64_t> nanoseconds) { m_joinEndPadNs = std::move(nanoseconds); }
    int readPacket(AVPacket& avPacket) override;
    // The source counts the packets readPacket leaves out at the joints: a gap is put that much earlier.
    void addTimeGap(const int64_t atNs, const int64_t gapNs) override
    {
        SimplePacketizerReader::addTimeGap(atNs - m_joinLeftOutNs, gapNs);
    }

   protected:
    void fillDiscoveryData(StreamDiscoveryData& data) override;
    int getHeaderLen() override { return 4; }  // 4-byte length prefix
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override;
    const std::string getStreamInfo() override;
    void setTestMode(bool value) override { m_testMode = value; }
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;

   private:
    bool m_testMode;
    bool m_firstFrame;

    uint8_t m_channels;
    uint16_t m_preSkip;
    int m_originalSampleRate;

    /// Duration of the current frame in 48 kHz samples.
    int m_curFrameSamples;

    /// Running granule position for OGG page output.
    int64_t m_granulePos;

    /// OGG page sequence number.
    uint32_t m_oggPageSeqNo;

    /// OGG bitstream serial number.
    uint32_t m_oggSerialNo;

    std::vector<uint8_t> m_lastPage;  // the last packet's page, header and packet, rewritten at the end
    int m_lastPageHeader = 0;
    int64_t m_endTrimNs = 0;
    std::vector<int64_t> m_joinEndPadNs;
    size_t m_joinFile = 0;     // the file of the join being read
    int64_t m_joinExcess = 0;  // samples at the joints so far that are not sound and are still played
    int m_nextPreSkip = -1;    // decodeFrame: the packet just decoded is the last of its file; the next file's pre-skip
    bool m_lastOfStream = false;  // decodeFrame: the packet just decoded ends the stream
    int64_t m_joinLeftOutNs = 0;  // the packets left out at the joints so far, in nanoseconds
    int m_startTrimLeft = -1;     // the pre-skip still to be trimmed in a transport stream; -1 before the first packet

    /// Raw CodecPrivate data (OpusHead).
    std::vector<uint8_t> m_codecPrivate;

    /// Parse the OpusHead structure.
    bool parseOpusHead(const uint8_t* data, int size);

    /// Parse the Opus TOC byte and return the number of 48 kHz PCM samples
    /// this packet produces.  Returns 0 on failure.
    int parseFrameSamples(const uint8_t* opusData, int size) const;

    /// Write a complete OGG page (header + segment table + payload) into dst.
    /// Returns bytes written, or 0 if not enough space.
    int writeOggPage(uint8_t* dst, uint8_t* dstEnd, const uint8_t* payload, int payloadSize, uint8_t headerType,
                     int64_t granule);

    /// Write just the OGG page header + segment table (no payload) into dst.
    /// The caller (writeAdditionData) writes this, then the muxer appends the
    /// Opus packet data to complete the page.
    int writeOggPageHeader(uint8_t* dst, uint8_t* dstEnd, int payloadSize, uint8_t headerType, int64_t granule);
};

#endif  // OPUS_STREAM_READER_H_
