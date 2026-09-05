#ifndef SIMPLE_PACKETIZER_READER_H_
#define SIMPLE_PACKETIZER_READER_H_

#include "abstractStreamReader.h"
#include "avCodecs.h"
#include "avPacket.h"
#include "tsPacket.h"

class SimplePacketizerReader : public AbstractStreamReader
{
   public:
    SimplePacketizerReader();
    ~SimplePacketizerReader() override = default;

    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    int64_t getProcessedSize() override;

    /// Bytes read for this track that could not be parsed as its codec and were thrown away,
    /// counted only AFTER the first frame was found. What is skipped BEFORE that first frame is
    /// deliberately not counted: a stream that begins part way through a frame legitimately loses
    /// the leading fragment, and every source that starts mid frame would otherwise be reported as
    /// lossy. Measured on 48 correct sources, all of which return 0 here, and on 32 streams cut at
    /// a random byte pair, which return 0 here and account for their whole skip before the sync.
    ///
    /// A JOIN SHARES ONE READER, so the exemption covers the first part only. A LATER part that
    /// begins part way through a frame IS counted, and that is deliberate: the fragment at the
    /// seam really is dropped, and parts that do not line up are worth saying so about. Measured
    /// at 16,494 bytes of 6,449,476 for a deliberately misaligned pair, and at zero for a joined
    /// pair taken off a disc, where every part starts on a frame.
    [[nodiscard]] int64_t getLostSize() const { return m_lostBytes; }

    /// Bytes handed to this reader for this track, counted where they arrive rather than where
    /// they are consumed. This is the denominator the loss report needs: "of the N read". It is
    /// NOT getProcessedSize(), which counts what got through and therefore omits anything skipped
    /// on purpose, a trailing tag above all.
    [[nodiscard]] int64_t getReadSize() const { return m_readBytes; }
    virtual CheckStreamRez checkStream(uint8_t* buffer, int len, ContainerType containerType, int containerDataType,
                                       int containerStreamIndex);

    /// Enhanced detection: runs checkStream(), then extracts codec-specific
    /// properties (channels, sample rate, resolution, etc.) into a
    /// StreamDiscoveryData struct.  Subclasses override fillDiscoveryData()
    /// to add codec-specific fields.
    StreamDiscoveryData probeStream(uint8_t* buffer, int len, ContainerType containerType, int containerDataType,
                                    int containerStreamIndex);

    virtual int getFreq() = 0;
    virtual int getAltFreq() { return getFreq(); }
    virtual uint8_t getChannels() = 0;
    void setStretch(const double value) { m_stretch = value; }
    void setMPLSInfo(const std::vector<MPLSPlayItem>& mplsInfo)
    {
        m_mplsInfo = mplsInfo;
        if (!m_mplsInfo.empty())
        {
            m_curMplsIndex = 0;
            m_lastMplsTime = (m_mplsInfo[0].OUT_time - m_mplsInfo[0].IN_time) * (INTERNAL_PTS_FREQ / 45000.0);
        }
        else
            m_curMplsIndex = -1;
    }

    // split point can be on any frame
    virtual bool isIFrame(AVPacket* packet) { return true; }

    /// Maximum frame size (bytes) that readPacket will accept.
    /// Override for codecs whose frames can exceed MAX_AV_PACKET_SIZE (e.g. FLAC).
    virtual int getMaxFrameSize() { return MAX_AV_PACKET_SIZE; }

   protected:
    virtual int getHeaderLen() = 0;  // return fixed frame header size at bytes
    virtual int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes,
                            int& skipBeforeBytes) = 0;  // decode frame parameters. bitrate, channels for audio e.t.c
    virtual uint8_t* findFrame(uint8_t* buff, uint8_t* end) = 0;  // find forawrd nearest frame
    virtual double getFrameDuration() = 0;                        // frame duration at nano seconds
    virtual const std::string getStreamInfo() = 0;
    virtual void setTestMode(bool value) {}
    [[nodiscard]] virtual bool needMPLSCorrection() const { return true; }

    /// Called by probeStream() after a successful checkStream() to let each
    /// codec fill codec-specific fields in the discovery data.  Override in
    /// subclasses.  The base implementation fills sampleRate and channels.
    virtual void fillDiscoveryData(StreamDiscoveryData& data);

    virtual bool needSkipFrame(const AVPacket& packet) { return false; }

    // uint8_t* m_tmpBuffer;
    int m_curMplsIndex;
    double m_stretch;
    std::vector<uint8_t> m_tmpBuffer;
    int64_t m_processedBytes;
    int64_t m_lostBytes;
    int64_t m_readBytes;  // see getReadSize(), summed in setBuffer
    bool m_everSynced;    // a frame of this codec has been found at least once
    // Past the last byte of a metadata tag run that the recogniser WALKED AND VERIFIED at the
    // current search position, or the position itself when there is none. A frame search may
    // disbelieve a sync below this and nowhere else. See oneTagCeiling for why.
    const uint8_t* m_tagCeilingEnd;
    // Bytes abandoned with no frame found, held back rather than counted as lost until a frame IS
    // found again. TRAILING DATA IS, BY DEFINITION, DATA AFTER WHICH NO FRAME EVER FOLLOWS, so a
    // gap that is never closed was the tail and a gap that is closed was a real hole. An
    // identifier tag at the end of an audio file is the ordinary case and must not be reported as
    // loss.
    //
    // This deliberately does NOT use the end of stream flag the demuxer passes to setBuffer. That
    // flag is set on the read AFTER the data runs out, so the tail has already been discarded by
    // the time it turns true, which was measured: gating on it changed nothing at all.
    int64_t m_pendingLost;

    // Bytes recognised as a metadata tag while m_curPos was still standing on them, waiting to
    // cancel the loss they will later be charged as. Never used to move the read position: the
    // frame search does all the stepping over, exactly as it did before any of this existed.
    int64_t m_tagCredit;
    const uint8_t* m_tagProbePos;  // so one chain is not credited twice while the position is held

   public:
    [[nodiscard]] int64_t getDeliveredFrames() const override { return static_cast<int64_t>(m_frameNum); }

   protected:
    uint64_t m_frameNum;
    bool m_needSync;
    double m_curPts;
    double m_lastMplsTime;
    double m_mplsOffset;
    double m_halfFrameLen;

    int m_containerDataType;
    int m_containerStreamIndex;
    std::vector<MPLSPlayItem> m_mplsInfo;
    void doMplsCorrection();
};

#endif
