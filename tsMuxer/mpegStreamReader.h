#ifndef MPEG_STREAM_READER_H_
#define MPEG_STREAM_READER_H_

#include <cstddef>
#include <functional>

#include "abstractStreamReader.h"
#include "limits.h"
#include "streamDiscoveryData.h"
#include "vod_common.h"

// The buffer a video reader collects its input in starts at TMP_BUFFER_SIZE and grows, up to MAX_TMP_BUFFER_SIZE, when
// one picture with what comes after it does not fit (setBuffer). A fixed 8 MiB lost every picture larger than that: an
// 8K picture coded without loss is about 42 MiB. The limit is above an 8K 4:4:4 picture of 16 bits before coding.
static constexpr int TMP_BUFFER_SIZE = 1024 * 1024 * 8;
static constexpr int64_t MAX_TMP_BUFFER_SIZE = 256ll * 1024 * 1024;

class MPEGStreamReader : public AbstractStreamReader
{
   public:
    MPEGStreamReader()
    {
        m_tmpBufferLen = 0;
        setFPS(0);
        m_eof = false;
        m_lastDecodeOffset = LONG_MAX;
        m_tmpBuffer = new uint8_t[TMP_BUFFER_SIZE];
        m_tmpBufferSize = TMP_BUFFER_SIZE;
        m_lastDecodedPos = nullptr;
        m_cachedMarker = nullptr;
        m_scanHighWater = nullptr;
        m_curPts = m_curDts = PTS_CONST_OFFSET;
        m_processedBytes = 0;
        m_totalFrameNum = 0;
        m_syncToStream = false;
        m_isFirstFpsWarn = true;
        m_shortStartCodes = true;
        m_pcrIncPerFrame = m_pcrIncPerField = 0;
        m_longCodesAllowed = true;
        m_removePulldown = false;
        m_pulldownWarnCnt = 1;
        m_testPulldownDts = 0;
        m_streamAR = m_ar = VideoAspectRatio::AR_KEEP_DEFAULT;
        m_spsPpsFound = false;
    }
    ~MPEGStreamReader() override
    {
        delete[] m_tmpBuffer;
        delete[] m_retiredBuffer;
    }
    void setFPS(const double fps)
    {
        m_fps = fps;
        if (fps > 0)
            m_pcrIncPerFrame = static_cast<int64_t>(static_cast<double>(INTERNAL_PTS_FREQ) / fps);
        else
            m_pcrIncPerFrame = 0;
        m_pcrIncPerField = m_pcrIncPerFrame / 2;
    }

    // The rate the container gives for the track, used where the meta line gives no fps=. Unlike fps=, it gives way
    // to the stream's own rate where the two differ by no more than a container can be off by (see updateFPS).
    void setContainerFPS(const double fps)
    {
        setFPS(fps);
        m_fpsFromContainer = true;
    }

    // Where the rate comes from for a stream that names none, when no fps= and no container rate is given either: the
    // times of its packets in a transport stream. Asked only then, since it reads the file; without it, 25. It is told
    // whether the stream may code fields (see AbstractDemuxer::getTrackPacketFps).
    void setPacketFpsSource(std::function<double(bool fieldCoded)> source) { m_packetFpsSource = std::move(source); }
    // Of a playlist whose clips are cut to the parts played: whether the picture with this number (counted from 0, in
    // the order of decoding) begins a part. Its order count does not follow on from the pictures before it, which the
    // cut left out, so the count starts again from it as from an IDR picture.
    void setPartStartSource(std::function<bool(int64_t)> source) { m_partStartSource = std::move(source); }

    [[nodiscard]] double getFPS() const { return m_fps; }
    [[nodiscard]] VideoAspectRatio getStreamAR() const
    {
        return m_ar != VideoAspectRatio::AR_KEEP_DEFAULT ? m_ar : m_streamAR;
    }
    void setAspectRatio(const VideoAspectRatio ar) { m_ar = ar; }
    // The aspect ratio the meta line gave (ar=), which comes before what the stream says
    [[nodiscard]] bool aspectRatioForced() const { return m_ar != VideoAspectRatio::AR_KEEP_DEFAULT; }
    // The shape of a pixel as the stream gives it (width / height), 0 where it gives none. getStreamAR is the nearest
    // of the few a Blu-ray disc can say; a Matroska output can say this one.
    [[nodiscard]] virtual double getSampleAspectRatio() const { return m_sampleAR; }
    // A later parameter set gave another pixel shape than the first (laterSampleAspectRatio: the first other one)
    [[nodiscard]] bool sampleAspectRatioChanged() const { return m_laterSampleAR >= 0; }
    [[nodiscard]] double laterSampleAspectRatio() const { return m_laterSampleAR; }
    int64_t getProcessedSize() override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    void onJoinedFileStart() override { m_joinPending = true; }
    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    /*
     * Colour description from the bitstream, as the coded values of ISO/IEC 23091-4. Returns
     * false when the stream does not signal one. Matroska output writes these into the Colour
     * element; the Blu-ray path does not need them because a disc carries HDR differently.
     */
    [[nodiscard]] virtual bool getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const
    {
        return false;
    }

    [[nodiscard]] virtual unsigned getStreamWidth() const = 0;
    [[nodiscard]] virtual unsigned getStreamHeight() const = 0;
    virtual bool getInterlaced() = 0;
    void setRemovePulldown(const bool value) { m_removePulldown = value; }
    // The rate was chosen by delPulldown, which has said so: the line that reports fps= changing the stream's rate
    // would report it a second time, as if fps= had been given.
    void setFpsAnnounced() { m_isFirstFpsWarn = false; }
    virtual int getFrameDepth() { return 1; }
    // The decoding time of the picture read last: m_curDts moved back by the depth of the B pyramid, and while a
    // deeper pyramid is caught up (deepenAfterOutput), a step after the time given before.
    int64_t pictureDts();

    /// Probe a buffer of elementary stream data and populate a
    /// StreamDiscoveryData struct with video-specific properties.
    /// Subclasses must implement checkStream() with their own signature;
    /// this helper fills common video fields after a successful check.
    virtual void fillVideoDiscoveryData(StreamDiscoveryData& data);

    virtual void onShiftBuffer(int offset);
    // The buffer was moved to a bigger one (setBuffer): every pointer kept into it moves by delta
    virtual void onMoveBuffer(std::ptrdiff_t delta) {}

   protected:
    VideoAspectRatio m_ar;
    VideoAspectRatio m_streamAR;
    double m_sampleAR = 0;
    // The pixel shape of a new parameter set (0 where it gives none). The first one is kept as the stream's: a
    // Matroska track has one display size, and it was the shape of the last parameter set read before the header was
    // written, so a stream that changes its shape after a second was shown in the later shape from its start.
    void noteSampleAspectRatio(double sar);
    bool m_sampleARKnown = false;
    double m_laterSampleAR = -1;
    bool m_shortStartCodes;
    int64_t m_curPts;
    int64_t m_curDts;
    int64_t m_processedBytes;
    bool m_eof;
    double m_fps;
    bool m_fpsFromContainer = false;
    int64_t m_pcrIncPerFrame;
    int64_t m_pcrIncPerField;
    const uint8_t* m_lastDecodedPos;
    // start-code scan cache for readPacket: m_cachedMarker points at the 01 byte of the
    // next start code ahead of m_curPos (nullptr = unknown); m_scanHighWater is the old
    // m_bufEnd a scan already covered without finding one. Both rebased like
    // m_lastDecodedPos in storeBufferRest. Without this, every readPacket call re-walked
    // the remaining NAL tail (quadratic for NALs above MAX_AV_PACKET_SIZE).
    uint8_t* m_cachedMarker;
    uint8_t* m_scanHighWater;
    // Where a later file of a join begins in the buffer, until the reader has looked at its first picture; rebased
    // like m_cachedMarker
    uint8_t* m_joinPos = nullptr;
    bool m_joinPending = false;  // onJoinedFileStart: the next setBuffer begins a later file
    int m_totalFrameNum;

   public:
    [[nodiscard]] int64_t getDeliveredFrames() const override { return m_totalFrameNum; }
    // The bytes of the units the reader went through, from the first start code on, and how many of them are in
    // units the codec cannot have (unusableUnit). Video of another codec has start codes too, and some of what
    // follows them passes for a parameter set or a picture: a track mostly of such units is not this codec.
    [[nodiscard]] int64_t getUnitBytes() const { return m_unitBytes; }
    [[nodiscard]] int64_t getUnusableBytes() const;

    // A packet read and left out of the output by a cut: its picture is not counted in "Processed N video frames". A
    // picture comes in several packets, which the count of pictures read so far tells apart.
    void frameLeftOut()
    {
        if (m_totalFrameNum != m_lastFrameLeftOut)
            ++m_framesLeftOut;
        m_lastFrameLeftOut = m_totalFrameNum;
    }

   private:
    int m_framesLeftOut = 0;
    int m_lastFrameLeftOut = -1;

   protected:
    // Whether the unit whose header begins at unit, behind its start code, is one the codec cannot use: a header it
    // forbids or reserves, or a picture whose first fields are impossible. maxLen is set to the longest a unit of this
    // type can be (0: no limit); a longer one cannot be what its header says. end: the end of what is buffered of it.
    virtual bool unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen)
    {
        maxLen = 0;
        return false;
    }
    // The first bytes of a unit without its emulation prevention bytes, to read the first fields of its header. The
    // reading begins behind the headerBytes of the unit header.
    class UnitHead
    {
       public:
        UnitHead(const uint8_t* unit, const uint8_t* end, int headerBytes);
        bool skipBits(int n);
        bool readUe(uint32_t& value);  // false: the code runs past what was read, or is longer than 32 bits
        bool readBit(int& value);

       private:
        uint8_t m_data[32];
        int m_bits = 0;
        int m_pos = 0;
    };

   protected:
    virtual double getStreamFPS(void* curNalUnit) = 0;
    void updateFPS(void* curNALUnit, uint8_t* buff, uint8_t* nextNal, int oldSPSLen);
    // The rate updateFPS takes when neither the stream nor anything else names one: the packet times, else 25. The
    // unit is the parameter set being read, for a reader that keeps it only after updateFPS.
    double fallbackFps(void* curNalUnit = nullptr);
    // Whether the stream is field coded, as the parameter set being read says (see fallbackFps)
    virtual bool interlacedBy(void* curNalUnit) { return getInterlaced(); }
    // Whether the stream is interlaced, for a rate of fields that a container gives (updateFPS)
    virtual bool interlacedForRate(void* curNalUnit) { return interlacedBy(curNalUnit); }
    // Whether an interlaced stream codes whole frames only, so that the time of each picture is that of a frame
    virtual bool codesFramesOnly(void* curNalUnit) { return false; }
    // The rate a descriptor written before updateFPS has run must carry: the one updateFPS will take
    double headerFps(double streamFps);
    std::function<double(bool fieldCoded)> m_packetFpsSource;
    std::function<bool(int64_t)> m_partStartSource;
    [[nodiscard]] bool isPartStart() const { return m_partStartSource && m_partStartSource(m_totalFrameNum); }
    double m_packetFps = -1.0;  // -1: not asked yet
    virtual void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) = 0;
    virtual void updateStreamAR(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) {}
    void fillAspectBySAR(double sar);
    virtual bool isIFrame() = 0;

    virtual bool skipNal(uint8_t* nal) { return false; }

    virtual int intDecodeNAL(uint8_t* buff) = 0;
    bool m_longCodesAllowed;
    bool m_removePulldown;
    int64_t m_testPulldownDts;
    int64_t m_pulldownDtsStart = 0;  // the decoding time of the first picture the pulldown check counts
    int64_t m_pulldownStep = 0;      // the frame step the pulldown check counts in
    bool m_pulldownMarkSeen = false;  // a picture shown longer than a frame: the stream has pulldown
    void checkPulldownSync();
    // The depth (getFrameDepth) grew from depthBefore before the picture being read, after pictures were given their
    // decoding times: the times of the next pictures catch up in steps shorter than a frame (pictureDts).
    void deepenAfterOutput(int depthBefore);
    uint8_t* m_tmpBuffer;
    int64_t m_tmpBufferSize;  // the size of m_tmpBuffer, TMP_BUFFER_SIZE until it grows
    // The buffer before the last growth, freed at the next: a packet handed out just before may still point into it
    uint8_t* m_retiredBuffer = nullptr;
    bool m_spsPpsFound;

   private:
    int64_t m_pulldownWarnCnt;
    int64_t m_catchUpStep = 0;    // 0: no catch-up running
    int64_t m_catchUpDts = 0;     // the decoding time given last while catching up
    int64_t m_catchUpCurDts = 0;  // m_curDts of the picture that got it
    long m_lastDecodeOffset;
    bool m_syncToStream;
    bool m_isFirstFpsWarn;
    bool m_badFpsSaid = false;  // the warning about a frame rate no video has was given (updateFPS)
    [[nodiscard]] int bufFromNAL() const;
    virtual int decodeNal(uint8_t* buff);
    void storeBufferRest();
    // getUnitBytes / getUnusableBytes. A unit goes out in one or more packets; the first begins with its start code
    // (startCodeLen > 0), the rest continue it.
    void countUnitBytes(const uint8_t* pos, int64_t len, int startCodeLen);
    void countUnits(const uint8_t* from, const uint8_t* end);  // the units of a block, as flushPacket sends it
    int64_t m_unitBytes = 0;
    int64_t m_unusableBytes = 0;  // of the units before the current one, and of the current one so far
    int64_t m_unitLen = 0;        // of the current unit, without its start code
    int m_unitStartCodeLen = 0;
    int m_unitMaxLen = 0;
    [[nodiscard]] int64_t overLongUnitBytes() const;  // the current unit, where it is longer than its type allows
    bool m_unitUnusable = false;
    bool m_unitOpen = false;
};

#endif
