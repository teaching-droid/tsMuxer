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

        // Folding a second video stream into one track. A disc can carry one picture on two
        // video streams, and Matroska carries both in ONE track. Two kinds do this:
        //
        //   dual layer Dolby Vision   a base layer and a quarter resolution enhancement layer
        //                             holding the RPU
        //   3D                        the base view and the dependent MVC view
        //
        // The mechanism is the same for both and lives in the fields below: match by pts, hold
        // the base frames until their partner arrives, append, write one block. Only the
        // conversion of the folded bytes and the configuration written beside them differ.
        //
        // On the base track foldElStreamIndex names the stream being folded in. On the folded
        // track foldedIntoStream names the base track it went into, and no track entry and no
        // block of its own are written for it.
        //
        // dvElConfig is Dolby Vision only: the enhancement stream's own HEVC configuration
        // record, written as the "hvcE" block addition mapping.
        enum class FoldKind : uint8_t
        {
            None,
            DolbyVisionEl,  // wrapped in NAL type 63, which an unaware decoder skips
            MvcDependent    // the dependent view, appended as it is with its delimiter dropped
        };
        FoldKind foldKind;

        int foldElStreamIndex;
        int foldedIntoStream;
        std::vector<uint8_t> dvElConfig;

        // The MVC configuration record, on the base track of a 3D pair. It carries BOTH views'
        // parameter sets, so it is the one thing that says how to decode the second view. Written
        // beside the video as the "mvcC" block addition mapping, and again at the end of
        // CodecPrivate, which is where the reference 3D files put it.
        std::vector<uint8_t> mvcConfig;

        // A base layer frame waiting for its enhancement layer. The two layers do not arrive in
        // step, so frames are held in arrival order and released as their partner turns up.
        struct HeldFrame
        {
            std::vector<uint8_t> data;
            int64_t pts;
            uint8_t flags;
        };
        std::deque<HeldFrame> heldFrames;
        std::map<int64_t, std::vector<uint8_t>> elDone;  // completed enhancement AUs, by pts
        std::vector<uint8_t> pendingElData;              // the enhancement AU still arriving
        int64_t elPts;
        int64_t elFramesMerged;
        int64_t elFramesUnmatched;

        // How the SOURCE framed its NALs, per NAL type: a start code is three bytes or four, both
        // are legal, and the coded video is identical either way. Matroska stores these codecs
        // LENGTH PREFIXED, so the framing is not in the file at all and a disc rebuilt from it
        // always came out four byte. Discs differ: one measured here uses four bytes only for the
        // access unit delimiter and the parameter sets, three for everything else.
        //
        // Recorded per TYPE rather than as one value, because that is what the discs actually do. A
        // type seen with both lengths sets startCodeMixed, and nothing is then claimed: a rule that
        // does not reproduce the source is worse than no rule.
        std::map<int, int> startCodeByType;
        bool startCodeMixed;

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
              foldKind(FoldKind::None),
              foldElStreamIndex(-1),
              foldedIntoStream(-1),
              elPts(-1),
              elFramesMerged(0),
              elFramesUnmatched(0),
              startCodeMixed(false),
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
    static std::vector<uint8_t> buildMVCDecoderConfigRecord(AbstractStreamReader* baseReader,
                                                            AbstractStreamReader* depReader);
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
    [[nodiscard]] std::string buildDvManifest(uint64_t rpuBytes, uint32_t rpuCrc, uint64_t ptsBytes, uint32_t ptsCrc,
                                              const std::string& scRule) const;
    // A manifest carrying only the start code rule, for a file that is not a profile 8.1 carrier.
    void writeStartCodeOnlyManifest(const std::string& scRule);
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
    // Has the destination actually been created yet? openDstFile deliberately does not create it.
    bool m_destinationOpen;
    // Stream indices that have sent at least one packet AND need to before the header can be
    // written, which is every kind of track except subtitles. See muxPacket.
    std::set<int> m_seenStreams;

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
    // Is a Dolby Vision fold possible at all, in the sense that the track list does not rule it
    // out? A necessary condition only, asked before the destination is opened so that a mux which
    // cannot run does not truncate a file. The exact test lives in refreshTrackProperties.
    [[nodiscard]] bool couldFoldDualLayer() const;
    // Create the destination and write the EBML and Segment headers. Called from
    // writeDeferredHeader, AFTER refreshTrackProperties has had its chance to refuse, so that a
    // mux which does not run never touches the file at that path. Does nothing if already open.
    void openDestination();
    // Write the deferred header (SegmentInfo + Tracks)
    void writeDeferredHeader();
    // Replay buffered pre-header packets (sets m_firstTimecode to min PTS)
    void replayBufferedPackets();
    [[nodiscard]] size_t tracksNeedingFirstPacket() const;

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
    // Not static: it records how the source framed each NAL type as it passes, which is the only
    // point at which that information still exists.
    std::vector<uint8_t> convertAnnexBToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data, int size);
    // Note one observation. len is 3 or 4.
    static void noteStartCode(MkvTrackInfo& track, int nalType, int len);
    // The NAL type of a NAL, by codec. Returns -1 when the codec has no such notion.
    static int nalTypeOf(int codecID, const uint8_t* nal, int size);
    // "4:35,32,33,34 3:1,39" or empty when nothing definite can be said.
    [[nodiscard]] static std::string startCodeRule(const MkvTrackInfo& track);
    // The same, for the enhancement layer half of a dual layer Dolby Vision access unit: every NAL
    // wrapped in an unspecified type 63 NAL, except the RPU which passes through unchanged.
    // Not static: in profile 8.1 mode it converts the RPU as it passes and keeps the original.
    std::vector<uint8_t> convertDvElToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data, int size);
    std::vector<uint8_t> convertMvcDepToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data, int size);
    void pairMvcViews();

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
