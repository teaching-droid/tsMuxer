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
    // times of its packets in a transport stream. Asked only then, since it reads the file; without it, 25.
    void setPacketFpsSource(std::function<double()> source) { m_packetFpsSource = std::move(source); }

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
    int64_t getProcessedSize() override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
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
    int m_totalFrameNum;

   public:
    [[nodiscard]] int64_t getDeliveredFrames() const override { return m_totalFrameNum; }

   protected:
    virtual double getStreamFPS(void* curNalUnit) = 0;
    void updateFPS(void* curNALUnit, uint8_t* buff, uint8_t* nextNal, int oldSPSLen);
    // The rate updateFPS takes when neither the stream nor anything else names one: the packet times, else 25. The
    // unit is the parameter set being read, for a reader that keeps it only after updateFPS.
    double fallbackFps(void* curNalUnit = nullptr);
    // Whether the stream is field coded, as the parameter set being read says (see fallbackFps)
    virtual bool interlacedBy(void* curNalUnit) { return getInterlaced(); }
    // The rate a descriptor written before updateFPS has run must carry: the one updateFPS will take
    double headerFps(double streamFps);
    std::function<double()> m_packetFpsSource;
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
    void checkPulldownSync();
    uint8_t* m_tmpBuffer;
    int64_t m_tmpBufferSize;  // the size of m_tmpBuffer, TMP_BUFFER_SIZE until it grows
    // The buffer before the last growth, freed at the next: a packet handed out just before may still point into it
    uint8_t* m_retiredBuffer = nullptr;
    bool m_spsPpsFound;

   private:
    int64_t m_pulldownWarnCnt;
    long m_lastDecodeOffset;
    bool m_syncToStream;
    bool m_isFirstFpsWarn;
    [[nodiscard]] int bufFromNAL() const;
    virtual int decodeNal(uint8_t* buff);
    void storeBufferRest();
};

#endif
