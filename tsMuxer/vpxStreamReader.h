// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef VPX_STREAM_READER_H_
#define VPX_STREAM_READER_H_

#include <vector>

#include "mpegStreamReader.h"

// VP8 and VP9 video, read from Matroska and WebM. Neither codec has start codes or any framing of its own: in a
// container each block is one frame (in VP9 a block may be a superframe, several frames that belong together). The
// Matroska parser (ParsedVpxTrackData) frames each block the way tsMuxeR frames AV1 inside: a start code in front of
// it and its bytes escaped as in H.264, so that no start code can appear inside. One byte 0x80 follows, because a
// frame may end in zero bytes and those would be taken for the start of the next start code. The Matroska muxer takes
// all of it off again (unframe).
//
// An IVF file, the elementary file of both codecs, is read as well: its frames are framed the same way as they are
// read (setBuffer).
//
// Neither codec has a form in a transport stream or on a Blu-ray disc: they go into a Matroska output, or are demuxed
// into an IVF file (prepareDemuxPacket). MuxerManager::checkTrackList refuses the rest.
class VpxStreamReader final : public MPEGStreamReader
{
   public:
    explicit VpxStreamReader(bool vp9);
    ~VpxStreamReader() override = default;

    CheckStreamRez checkStream(uint8_t* buffer, int len);
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override { return 0; }
    [[nodiscard]] bool needSPSForSplit() const override { return false; }
    int prepareDemuxPacket(uint8_t* data, int size, const uint8_t** outData) override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    // From an IVF file, the bytes read from it: its headers are not in the frames that are counted otherwise
    int64_t getProcessedSize() override;

    // An IVF file header: its codec (true for VP9), picture size and frame rate. False when data is not one.
    static bool readIvfHeader(const uint8_t* data, int size, bool& vp9, double& fps);
    // The header of an IVF file holding the codec of this fourcc, and the frame rate its first frames give
    static bool readIvfHeaderOf(const uint8_t* data, int size, const char* fourcc, double& fps);

    // One framed frame as it arrives here, back to the frame itself: the start code, the escaping and the stop byte
    // taken off.
    static std::vector<uint8_t> unframe(const uint8_t* data, int size);

   protected:
    const CodecInfo& getCodecInfo() override;
    int intDecodeNAL(uint8_t* buff) override;

    double getStreamFPS(void* curNalUnit) override { return 0.0; }  // neither codec says its frame rate
    [[nodiscard]] unsigned getStreamWidth() const override { return m_width; }
    [[nodiscard]] unsigned getStreamHeight() const override { return m_height; }
    bool getInterlaced() override { return false; }
    bool isIFrame() override { return m_lastIFrame; }
    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override {}
    void onSplitEvent() override { m_ivfHeaderWritten = false; }

   private:
    // Reads the header at the start of a frame. True for a keyframe, which also sets the size, the profile and the
    // bit depth.
    bool parseFrameHeader(const uint8_t* frame, int size);
    void incTimings();

    bool m_vp9;
    unsigned m_width;
    unsigned m_height;
    int m_profile;
    int m_bitDepth;
    bool m_lastIFrame;
    bool m_firstFrame;
    // input: an IVF file, its frames framed as they arrive
    enum class Input
    {
        UNDECIDED,
        FRAMED,
        IVF
    };
    Input m_input = Input::UNDECIDED;
    std::vector<uint8_t> m_pending;     // bytes of the file not yet framed, a frame may span blocks
    std::vector<uint8_t> m_convBuffer;  // front pad plus the framed frames handed to the base
    int m_ivfHeaderLeft = 0;            // bytes of the file header still to skip
    uint32_t m_ivfDamagedSize = 0;      // a frame size no frame can have: the file is damaged from there
    int64_t m_ivfRead = 0;              // the bytes of an IVF file handed over so far
    int64_t m_ivfLeftOut = 0;           // the bytes of the file left out after that
    // demux: an IVF file
    std::vector<uint8_t> m_demuxBuffer;
    bool m_ivfHeaderWritten = false;
    int64_t m_demuxFrames = 0;  // the frames written to the file, its time stamps
    int64_t m_demuxSkip = 0;    // the rest of a frame already written, which comes in later packets
};

#endif  // VPX_STREAM_READER_H_
