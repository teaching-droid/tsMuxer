#ifndef MATROSKA_MUXER_H_
#define MATROSKA_MUXER_H_

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "abstractMuxer.h"
#include "avPacket.h"

// ──────────────────────────────── EBML writing helpers ────────────────────────────────

// Return the number of bytes needed for an EBML element ID.
int ebml_id_size(uint32_t id);

// Write an EBML element ID to dst, return number of bytes written.
int ebml_write_id(uint8_t* dst, uint32_t id);

// Return the number of bytes needed to encode `size` as an EBML VINT (data-size).
int ebml_size_size(uint64_t size);

// Write an EBML data-size VINT to dst using the minimum number of bytes.
// Returns the number of bytes written.
int ebml_write_size(uint8_t* dst, uint64_t size);

// Write an EBML data-size VINT using exactly `bytes` bytes.
// Returns the number of bytes written (== bytes).
int ebml_write_size_fixed(uint8_t* dst, uint64_t size, int bytes);

// "Unknown" size encoded as 0xFF (1 byte) or all-ones for the given width.
int ebml_write_unknown_size(uint8_t* dst, int bytes);

// Write helpers for typed EBML elements – each writes <ID><size><payload>.
// Returns total bytes written.
int ebml_write_uint(uint8_t* dst, uint32_t id, uint64_t value);
int ebml_write_sint(uint8_t* dst, uint32_t id, int64_t value);
int ebml_write_float(uint8_t* dst, uint32_t id, double value);
int ebml_write_string(uint8_t* dst, uint32_t id, const std::string& value);
int ebml_write_binary(uint8_t* dst, uint32_t id, const uint8_t* data, int len);
int ebml_write_master_open(uint8_t* dst, uint32_t id, uint64_t contentSize);

// ──────────────────────────────── Matroska Muxer ─────────────────────────────────────

class MatroskaMuxer final : public AbstractMuxer
{
   public:
    MatroskaMuxer(MuxerManager* owner);
    ~MatroskaMuxer() override;

    void intAddStream(const std::string& streamName, const std::string& codecName, int streamIndex,
                      const std::map<std::string, std::string>& params, AbstractStreamReader* codecReader) override;
    bool muxPacket(AVPacket& avPacket) override;
    bool doFlush() override;
    bool close() override;
    void openDstFile() override;
    void parseMuxOpt(const std::string& opts) override;
    void setChapters(const std::vector<double>& chapters) override { m_chapters = chapters; }

   private:
    std::vector<double> m_chapters;  // start times in seconds
    void writeChapters();

    // ── Track information collected during intAddStream ──
    struct MkvTrackInfo
    {
        int streamIndex;              // external stream index
        int trackNumber;              // 1-based Matroska track number
        uint64_t trackUID;            // random UID
        std::string matroskaCodecID;  // e.g. "V_MPEG4/ISO/AVC"
        int codecID;                  // internal CODEC_* constant
        uint8_t trackType;            // 1=video, 2=audio, 17=subtitle
        std::string language;         // ISO 639-2 code, empty means write "und"
        std::string name;             // track-name=, optional
        bool markedDefault;           // the meta carried "default" on this track
        bool isDefault;               // resolved: exactly one per track type
        AbstractStreamReader* codecReader;

        // Video-specific
        unsigned width;
        unsigned height;
        double fps;
        bool interlaced;
        VideoAspectRatio streamAR;

        // Colour description from the bitstream, and HDR10 mastering data when the stream has it.
        bool hasColourDesc;
        uint8_t colourPrimaries;
        uint8_t colourTransfer;
        uint8_t colourMatrix;
        bool isHdr10;

        // Dolby Vision configuration record and its BlockAddIDType (dvcC or dvvC), 0 if not DV.
        uint32_t dvBlockAddIdType;
        uint8_t dvConfig[24];

        // Dual layer Dolby Vision. A disc carries the picture on two video streams, a base layer
        // and a quarter resolution enhancement layer holding the RPU; Matroska carries both in ONE
        // track. On the base track dvElStreamIndex names the enhancement stream being folded in and
        // dvElConfig holds that stream's own HEVC configuration record, written beside the Dolby
        // Vision record as the "hvcE" block addition mapping. On the enhancement track
        // dvMergedIntoStream names the base track it was folded into, and no track entry and no
        // block of its own are written for it.
        int dvElStreamIndex;
        int dvMergedIntoStream;
        std::vector<uint8_t> dvElConfig;

        // A base layer frame waiting for its enhancement layer. The two layers do not arrive in
        // step, so frames are held in arrival order and released as their partner turns up.
        struct HeldFrame
        {
            std::vector<uint8_t> data;
            int64_t pts;
            uint8_t flags;
        };
        std::deque<HeldFrame> dvHeldFrames;
        std::map<int64_t, std::vector<uint8_t>> dvElDone;  // completed enhancement AUs, by pts
        std::vector<uint8_t> pendingElData;                // the enhancement AU still arriving
        int64_t dvElPts;
        int64_t dvElFramesMerged;
        int64_t dvElFramesUnmatched;

        // Audio-specific
        int sampleRate;
        int channels;
        int bitDepth;

        // A Blu-ray TrueHD stream is two things braided onto one PID: the lossless MLP frames and a
        // 448 kbps AC-3 core beside them, so a player that cannot decode the lossless stream still
        // has something to play. Matroska has no such arrangement, an A_TRUEHD track holds the
        // lossless stream alone, so the core is carried as a track of its own instead of being
        // discarded. On the TrueHD track ac3CoreStreamIndex names that companion; on the companion
        // ac3CoreOfStream names the TrueHD track whose core it carries. dropAc3Core is the per track
        // opt out for anyone who wants the lossless stream on its own.
        int ac3CoreStreamIndex;
        int ac3CoreOfStream;
        bool dropAc3Core;

        // Codec-private data (built at openDstFile time)
        std::vector<uint8_t> codecPrivate;

        // Frame accumulation: the MPEG stream reader may split large frames
        // into multiple packets with the same PTS. We buffer them here and
        // write a single SimpleBlock when PTS changes or at flush time.
        std::vector<uint8_t> pendingFrameData;
        int64_t pendingPts;
        uint8_t pendingFlags;
        bool hasPendingFrame;
        bool anyBlockWritten;  // used to spot the leading AC-3 core frame of a TrueHD track

        MkvTrackInfo()
            : streamIndex(0),
              trackNumber(0),
              trackUID(0),
              codecID(0),
              trackType(0),
              codecReader(nullptr),
              width(0),
              height(0),
              fps(0),
              interlaced(false),
              streamAR(VideoAspectRatio::AR_KEEP_DEFAULT),
              markedDefault(false),
              isDefault(false),
              hasColourDesc(false),
              colourPrimaries(2),
              colourTransfer(2),
              colourMatrix(2),
              isHdr10(false),
              dvBlockAddIdType(0),
              dvConfig{},
              dvElStreamIndex(-1),
              dvMergedIntoStream(-1),
              dvElPts(-1),
              dvElFramesMerged(0),
              dvElFramesUnmatched(0),
              sampleRate(0),
              channels(0),
              bitDepth(0),
              ac3CoreStreamIndex(-1),
              ac3CoreOfStream(-1),
              dropAc3Core(false),
              pendingPts(0),
              pendingFlags(0),
              hasPendingFrame(false),
              anyBlockWritten(false)
        {
        }
    };

    // ── Cue point for building the Cues element at close ──
    struct CueEntry
    {
        int64_t timecodeMs;  // cluster-relative time in ms
        int trackNumber;
        int64_t clusterOffset;  // byte offset of the cluster from segment data start
    };

    // Helper: map internal codec name to Matroska codec ID string
    static std::string codecNameToMatroskaID(const std::string& codecName, int codecID);

    // Build codec-private data for a track
    void buildCodecPrivate(MkvTrackInfo& track);

    // Build the AVCDecoderConfigurationRecord from H.264 SPS/PPS
    static std::vector<uint8_t> buildAVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build the HEVCDecoderConfigurationRecord from HEVC VPS/SPS/PPS
    static std::vector<uint8_t> buildHEVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build the VVCDecoderConfigurationRecord
    static std::vector<uint8_t> buildVVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build AV1CodecConfigurationRecord
    static std::vector<uint8_t> buildAV1ConfigRecord(AbstractStreamReader* reader);
    // Build AAC AudioSpecificConfig
    static std::vector<uint8_t> buildAACConfig(AbstractStreamReader* reader);

    // Write the EBML header to the output file
    void writeEBMLHeader();
    // Write Segment Info element into m_segmentBody
    void writeSegmentInfo();
    // Write Tracks element into m_segmentBody
    void writeTracks();
    // Write a single TrackEntry and return its serialized bytes
    std::vector<uint8_t> buildTrackEntry(const MkvTrackInfo& track);
    static int writeColourInfo(uint8_t* dst, const MkvTrackInfo& track);

    // Start a new cluster at the given timecode (milliseconds)
    void startCluster(int64_t timecodeMs);
    // Flush the current cluster to the file
    void flushCluster();

    // Write Cues element at end of file
    void writeCues();
    // Write the Attachments element: the preserved original RPUs and the manifest that explains
    // them. Only in profile 8.1 mode, and only when RPUs were actually converted.
    void writeAttachments();
    // The manifest text, built once the frame count and the checksum are known.
    [[nodiscard]] std::string buildDvManifest(uint64_t rpuBytes, uint32_t rpuCrc, uint64_t ptsBytes,
                                              uint32_t ptsCrc) const;
    // Build the SeekHead element, header included, so it can be written in both places.
    [[nodiscard]] std::vector<uint8_t> buildSeekHead() const;
    // Write SeekHead element at end of file
    void writeSeekHead();
    // Write the same SeekHead into the space reserved at the front of the segment.
    void writeFrontSeekHead();
    // Write a Void element occupying exactly this many bytes, header included.
    void writeVoid(int totalBytes);

    // Low-level: write bytes to the output file
    void writeToFile(const uint8_t* data, int len);
    void writeToFile(const std::vector<uint8_t>& data);

    // ── Data members ──
    File m_file;
    std::string m_fileName;

    std::map<int, MkvTrackInfo> m_tracks;  // keyed by streamIndex
    int m_nextTrackNumber;

    // --dv-profile=8.1. A dual layer profile 7 disc is understood by few devices, while profile 8.1
    // plays almost everywhere, so the RPU is converted to 8.1 as the track is written and the track
    // declares itself single layer. The enhancement layer still rides along, wrapped in unspecified
    // NAL type 63, which a decoder skips: that is what keeps the original disc recoverable, and it
    // is why the file does NOT get smaller the way the usual one way conversion does.
    //
    // The ORIGINAL profile 7 RPUs are kept and written out as an attachment, in the layout an
    // extracted RPU file uses (a 4-byte start code then the RPU payload without its 2-byte NAL
    // header), so the preserved original is readable by the existing tooling and not only by
    // tsMuxeR. Without it the conversion would be one way: it is many to one, so no procedure of
    // any kind can recover a profile 7 RPU from an 8.1 one.
    //
    // The payloads sit end to end in one buffer with an index beside them, rather than as a vector
    // of vectors, because a feature has upwards of 160,000 of them.
    //
    // ORDER. They are collected as the mux walks the pictures, which is DECODE order, and written
    // out in DISPLAY order, because that is the order an extracted RPU file uses everywhere else.
    // Keeping decode order would produce a file that other tools parse happily and use wrongly.
    // The manifest states the order, and the split reorders on the way back.
    struct DvRpuEntry
    {
        int64_t pts;
        uint64_t offset;
        uint32_t length;
    };
    bool m_dvWriteProfile81 = false;
    std::vector<uint8_t> m_dvRpuPayload;
    std::vector<DvRpuEntry> m_dvRpuIndex;
    int64_t m_dvCurrentRpuPts = 0;
    int64_t m_dvRpusConverted = 0;
    // The record this file WOULD have declared as a dual layer profile 7 track. Not used by the
    // track itself, which declares 8.1; it goes into the manifest so a rebuild can restore it.
    uint8_t m_dvProfile7Config[24] = {};
    uint32_t m_dvProfile7ConfigType = 0;

    // Segment layout
    int64_t m_segmentStartPos;  // file position of the first byte after the Segment header
    int64_t m_segmentSizePos;   // file position where the segment's VINT size is written

    // Cluster buffering
    std::vector<uint8_t> m_clusterBuf;  // current cluster data
    int64_t m_clusterTimecodeMs;        // timecode of current cluster in ms
    int64_t m_clusterStartFilePos;      // file offset where current cluster begins
    bool m_clusterOpen;
    int64_t m_clusterDataSize;  // data written in current cluster

    // Cue tracking
    std::vector<CueEntry> m_cueEntries;

    // Positions of key elements (relative to segment data start) for SeekHead
    int64_t m_segmentInfoPos;
    int64_t m_tracksPos;
    int64_t m_cuesPos;
    int64_t m_attachmentsPos = 0;

    // Space held at the front of the segment for the seek index, filled in at close. Cues and the
    // attachments are written after the clusters because they are not known until then, and
    // without an index at the front other software does not find them.
    int64_t m_seekHeadReservePos = 0;
    static constexpr int SEEKHEAD_RESERVE = 256;  // five entries need about 120
    static constexpr int VOID_MIN_BYTES = 3;      // one byte of ID plus a two byte size

    // Timecode tracking
    int64_t m_firstTimecode;  // first PTS seen (in INTERNAL_PTS_FREQ units) – used as reference
    bool m_firstTimecodeSet;
    int64_t m_lastTimecodeMs;        // last PTS seen (in ms relative to start) for Duration
    int64_t m_durationValueFilePos;  // absolute file position of the Duration float64 value

    // Deferred header writing: SegmentInfo + Tracks are written once ALL tracks
    // have received at least one packet, because stream readers haven't fully
    // initialized (e.g. audio sample rate / channels) at openDstFile time.
    bool m_headerWritten;
    std::set<int> m_seenStreams;  // stream indices that have sent at least one packet

    // Buffered packets accumulated before the header is written.
    struct BufferedPacket
    {
        int stream_index;
        int64_t pts;
        uint32_t flags;
        std::vector<uint8_t> data;
    };
    std::vector<BufferedPacket> m_preHeaderPackets;

    // Refresh track properties from codec readers (called when readers are initialized)
    void refreshTrackProperties();
    // Write the deferred header (SegmentInfo + Tracks)
    void writeDeferredHeader();
    // Replay buffered pre-header packets (sets m_firstTimecode to min PTS)
    void replayBufferedPackets();

    // Flush any pending accumulated frame data for a track as a single SimpleBlock.
    void flushPendingFrame(MkvTrackInfo& track);
    // Emit one completed frame. Shared by the ordinary path and by the dual layer Dolby Vision
    // path, which writes its frames later than it finishes them.
    void writeBlock(MkvTrackInfo& track, const uint8_t* frameData, int frameSize, int64_t pts, uint8_t pendingFlags);
    void drainHeldFrames(MkvTrackInfo& track, bool atEndOfStream);

    // Internal mux logic (called after header is written)
    bool muxPacketInternal(AVPacket& avPacket);

    // Convert frame data from internal start-code format to MKV format.
    // For AV1: start-code OBUs → low-overhead OBUs (obu_has_size_field + LEB128)
    // For H.264/HEVC/VVC: Annex B start codes → 4-byte length-prefixed NALUs
    // Returns empty vector if no conversion needed (data is passed through as-is).
    static std::vector<uint8_t> convertAV1ToLowOverhead(const uint8_t* data, int size);
    static std::vector<uint8_t> convertAnnexBToLengthPrefixed(const uint8_t* data, int size);
    // The same, for the enhancement layer half of a dual layer Dolby Vision access unit: every NAL
    // wrapped in an unspecified type 63 NAL, except the RPU which passes through unchanged.
    // Not static: in profile 8.1 mode it converts the RPU as it passes and keeps the original.
    std::vector<uint8_t> convertDvElToLengthPrefixed(const uint8_t* data, int size);

    // Cluster splitting thresholds
    static constexpr int64_t CLUSTER_MAX_DURATION_MS = 5000;      // 5 seconds
    static constexpr int64_t CLUSTER_MAX_SIZE = 5 * 1024 * 1024;  // 5 MB
};

class MatroskaMuxerFactory final : public AbstractMuxerFactory
{
   public:
    [[nodiscard]] std::unique_ptr<AbstractMuxer> newInstance(MuxerManager* owner) const override
    {
        return std::make_unique<MatroskaMuxer>(owner);
    }
};

#endif
