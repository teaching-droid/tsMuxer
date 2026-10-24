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
    // The length of one frame in the internal clock, as far as the stream read so far tells: the step the frame
    // times go by. 0 where it is not known yet.
    double frameDurationInternal() { return getFrameDuration(); }
    virtual int getAltFreq() { return getFreq(); }
    virtual uint8_t getChannels() = 0;
    void setStretch(const double value) { m_stretch = value; }
    void addTimeGap(int64_t atNs, int64_t gapNs) override;
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
    // ** THE LENGTH A FRAME HAS WHEN IT IS WHOLE, FROM ITS HEADER ALONE. ** decodeFrame cannot say whether the LAST
    // frame of a file is all there: most readers look a few bytes past a frame for the next one, and at the end of
    // the data there is none, so a whole last frame and one the end of the file cut off can both answer
    // NOT_ENOUGH_BUFFER. This reads only the header of the frame that begins at p, and nothing behind the frame:
    //   > 0                the length the header gives. More than end - p: the end of the file cut the frame off.
    //   NOT_ENOUGH_BUFFER  p holds the start of a header of this codec, and the header itself is cut off.
    //   0                  this reader cannot tell, or p holds no header of it. This is the default, and a reader
    //                      that gives 0 keeps the behaviour it had before the question was asked.
    // checkStream counts a whole last frame with it, and flushPacket leaves out a cut one.
    virtual int wholeFrameSizeAt(const uint8_t* p, const uint8_t* end) { return 0; }
    virtual double getFrameDuration() = 0;  // frame duration at nano seconds
    virtual const std::string getStreamInfo() = 0;
    virtual void setTestMode(bool value) {}
    [[nodiscard]] virtual bool needMPLSCorrection() const { return true; }

    /// Called by probeStream() after a successful checkStream() to let each
    /// codec fill codec-specific fields in the discovery data.  Override in
    /// subclasses.  The base implementation fills sampleRate and channels.
    virtual void fillDiscoveryData(StreamDiscoveryData& data);

    virtual bool needSkipFrame(AVPacket& packet) { return false; }

    // The end of the source cut the last frame off: its bytes are left out with a warning, and counted as read.
    void leaveOutCutFrame(int64_t bytes);

    // The transport stream that carries the track declares it as this reader's codec by its stream type, as checkStream
    // has it in m_containerType and m_containerDataType. checkStream then asks less of a chain behind damage. False,
    // the default, for any other source and any stream type that names no codec, as 0x06 private data.
    [[nodiscard]] virtual bool declaredByContainer() const { return false; }

    // ** THE STATE A CHAIN OF FRAMES LEAVES IN THE READER, CLEARED. ** checkStream decodes frames in test mode, and a
    // reader keeps what they say: TrueHD behind an AC-3 frame, a DTS-HD extension substream, the Atmos and DTS:X
    // counters, the rate and the channels. A chain that broke on damage must not hand that to the next one. This puts
    // the codec state back to what a new reader has, and keeps the settings (down-to-ac3, down-to-dts, test mode).
    // The default does nothing: a reader that keeps no such state needs nothing cleared.
    virtual void resetDecodeState() {}

    // What a good frame must keep from the one before it for goodFramesAnywhere to count it: by default its sample
    // rate and channels. DTS adds the length of its core frame and its samples a frame, as it has no CRC.
    virtual uint64_t frameLayoutKey() { return static_cast<uint64_t>(getFreq()) << 8 | getChannels(); }

    // Whether decodeFrame proves a frame good on its own, so that goodFramesAnywhere may count it where it stands:
    // AC-3 and E-AC-3 by the CRC over the whole frame, DTS by its 32 bit sync, a header it checks and a frame that
    // does not run into the next one. False, the default, for a codec whose frames prove nothing alone: an ADTS
    // header is 12 bits of sync and a length, and five of them in a row of one layout turned up in PCM in a track
    // declared as AAC, which was then listed as AAC, 96 kHz, 8 channels.
    [[nodiscard]] virtual bool frameProvesItself() const { return false; }

   private:
    uint8_t* frameChainBreak(uint8_t* from, uint8_t* end, int needFrames, bool firstChain, const uint8_t* lastStart);
    bool goodFramesAnywhere(uint8_t* buffer, uint8_t* end);

   protected:
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
    bool m_lastBlock = false;  // the block being read is the last of the stream
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
    int m_firstFreq = 0;  // the sample rate of the first frame, for the warning of a later one (readPacket)
    bool m_laterFreqSaid = false;
    bool m_needSync;
    double m_curPts;
    std::vector<std::pair<int64_t, int64_t>> m_timeGaps;  // addTimeGap, in the order they come
    void applyTimeGaps();
    double m_lastMplsTime;
    double m_mplsOffset;
    double m_halfFrameLen;

    int m_containerDataType;
    int m_containerStreamIndex;
    std::vector<MPLSPlayItem> m_mplsInfo;
    void doMplsCorrection();

   public:
    // For the end of the job, beside getLostSize: what the frames found carry (with what a frame skips), what the tags
    // recognised hold, and how often the sample rate or the channels were others where the stream was found again.
    // The loss leaves out on purpose what comes before the first frame and after the last; other audio read as this
    // codec is mostly that, with a frame or two found by chance. Its frames, found one by one, each say another rate.
    [[nodiscard]] int64_t getFrameBytes() const { return m_frameBytes; }
    // With the tags at the very end of the data (trailingTagBytes), which no frame search ever stands on
    [[nodiscard]] int64_t getTagBytes() const;
    [[nodiscard]] int64_t getSyncChanges() const { return m_syncChanges; }
    // The padding the frame search stepped over, runs of zeros or 0xFF of PAD_RUN_MIN bytes or more (countPadding)
    [[nodiscard]] int64_t getPadBytes() const { return m_padBytes; }

   protected:
    int64_t m_frameBytes = 0;
    int64_t m_tagBytes = 0;

   private:
    int64_t m_syncChanges = 0;
    int m_syncFreq = 0;  // the rate and channels at the sync before
    int m_syncChannels = 0;
    // ** PADDING IS NOT ANOTHER CODEC. ** The frames-under-half rule counted every byte that was not a frame or a tag
    // as data of some other codec, and zeros are no codec: real AAC behind 2 MiB of zeros, or AC-3, MP3 and DTS
    // followed by zeros of twice their size, were muxed whole and then ended with that error. The same held for 0xFF,
    // the fill of erased flash memory, which the frame search steps over as it does zeros (the same output). Runs of
    // either that the search steps over are counted here and left aside as the tags are. A short run is not padding
    // but data (PCM a little above silence has a zero byte in every other place), and a file of nothing but padding
    // has no frames at all, which the rule for empty tracks refuses.
    static constexpr int64_t PAD_RUN_MIN = 64;
    int64_t m_padBytes = 0;
    void countPadding(const uint8_t* from, const uint8_t* to);
    // ** A TAG AT THE END IS FOUND FROM THE END. ** An APE tag that has only a footer (APEv1 always, APEv2 as Monkey's
    // Audio writes it) begins with its items, so nothing marks its start for the forward reading: note 117 left it a
    // stated limit, the bytes are held and never counted as lost. The frames-under-half rule then counted them as audio
    // that is not frames, and an MP3 of 320 KB with a 1.5 MB cover in such a tag was refused, where 2.20.0 muxed it
    // whole. The last bytes that arrived are kept so the chain of tags ending the data (ID3v1, APE footer, Lyrics3) can
    // be read backwards from its footers, as their formats are meant to be read, for that rule alone.
    [[nodiscard]] int64_t trailingTagBytes() const;
    static constexpr size_t TAIL_KEEP = 16384 + 256;  // Lyrics3 v1's search bound, and ID3v1 and the footers
    std::vector<uint8_t> m_tail;  // the last bytes given to setBuffer, TAIL_KEEP up to twice that
    // The stream offsets of the last tag chain credited going forward, so a tag credited there is not counted twice
    int64_t m_creditFrom = 0;
    int64_t m_creditTo = 0;
};

#endif
