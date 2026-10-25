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
    void setChapterInfo(const std::vector<AVChapter>& info) override { m_chapterInfo = info; }
    // --cut-pictures=cover: the output begins this long (internal clock) before --cut-start, at the key picture before
    // it, and its chapters, times from the cut, are moved by as much; the chapter playing at the cut stays at 0
    void setCutOrigin(int64_t before);
    void setSourceExtras(const std::string& title, const std::vector<std::vector<uint8_t>>& attachments,
                         const std::vector<std::vector<uint8_t>>& tags) override;
    void setSourceTrackTags(const std::map<int, std::vector<AbstractDemuxer::SourceTrackTag>>& tags) override;
    void setSourceBlockAdditions(
        const std::map<int, std::pair<uint64_t, std::vector<std::vector<uint8_t>>>>& info) override
    {
        m_srcBlockAdd = info;
    }

   private:
    std::vector<double> m_chapters;        // start times in seconds
    std::vector<AVChapter> m_chapterInfo;  // same order as m_chapters, may be short or empty
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
        std::string languageBCP47;    // the source's BCP 47 tag, written as LanguageBCP47 when there is one
        std::string name;             // track-name=, optional
        bool defaultExplicit;         // the meta carried "default" on this track at all
        bool markedDefault;           // and it did not say none, no, 0, false or off
        bool isDefault;               // resolved: exactly one per track type
        // FlagForced and the five accessibility and origin flags, by element id, as the source or the meta gave them.
        std::map<uint32_t, int> flags;
        // From the source container, for what the stream itself does not say (srccolour, srcdisplay).
        std::map<std::string, double> srcColour;  // m, r, t, p, cll, fall, rx ... lmin, as present
        unsigned srcDisplayWidth = 0;
        uint64_t srcCrop[4] = {0, 0, 0, 0};  // top, bottom, left, right (srccrop)
        int srcFieldOrder = -1;              // srcfieldorder, -1 when the source gave none
        std::vector<uint8_t> srcProjection;  // the body of the source's Projection element
        std::vector<uint8_t> srcVpxPrivate;  // a VP9 source's CodecPrivate (srcvpxprivate)
        // A track copied as it is (CODEC_S_MKVCOPY): the source's BlockAdditionMapping bodies and MaxBlockAdditionID
        std::vector<std::vector<uint8_t>> copyAddMappings;
        uint64_t copyMaxBlockAddId = 0;
        bool frameAdditions = false;  // the source stored BlockAdditions beside the frames of this track
        int64_t lastBlockRelMs = 0;   // the time of the track's last block, for a ReferenceBlock
        unsigned srcDisplayHeight = 0;
        unsigned srcStereoMode = 0;         // the source's StereoMode (srcstereomode), 0 when it gave none
        int64_t endPaddingNs = 0;           // DiscardPadding for the track's last block (Opus, Vorbis), from the source
        uint64_t srcCodecDelayNs = 0;       // CodecDelay from the source's edit list (srccodecdelay), AAC from MP4
        std::vector<uint8_t> srcAacConfig;  // the AudioSpecificConfig of HE-AAC the source names (srcaacsbr)
        int outputSampleRate = 0;           // its output sample rate, OutputSamplingFrequency; 0 for none
        bool mp3InfoChecked = false;        // the first MPEG audio frame was looked at for a Xing/Info frame
        int mp3Delay = -1;                  // its LAME encoder delay and padding in samples, -1 without one
        int mp3Padding = 0;
        bool finalFlush = false;  // the frame being written is the track's last
        AbstractStreamReader* codecReader;

        // Video-specific
        unsigned width;
        unsigned height;
        double fps;
        bool interlaced;
        bool fieldPictures = false;  // each picture of the stream is one field (HEVC field_seq_flag)
        VideoAspectRatio streamAR;
        double sampleAR = 0;    // the stream's own pixel shape, 0 where it gives none
        bool arForced = false;  // ar= in the meta

        // Colour description from the bitstream, and HDR10 mastering data when the stream has it.
        bool hasColourDesc;
        uint8_t colourPrimaries;
        uint8_t colourTransfer;
        uint8_t colourMatrix;
        bool isHdr10;
        unsigned hdr10Metadata[6] = {0, 0, 0, 0, 0, 0};  // the track's own, packed as in HDR10_metadata

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
        uint64_t audioFrameNs = 0;  // DefaultDuration for audio of a fixed frame length, 0 for the others
        int aacSplit = 1;  // AAC whose ADTS frames carry this many raw blocks, each written as a block of its own

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
        int64_t pendingDts;  // a VC-1 block is stamped with it, see writeBlock's caller
        uint8_t pendingFlags;
        bool hasPendingFrame;

        // The two largest block timestamps written on this track, in internal units, from which
        // close() measures where the track ends. The largest, not the latest: video is written in
        // decode order.
        int64_t maxBlockPts;
        int64_t nextMaxBlockPts;  // the largest below maxBlockPts
        bool hasMaxBlockPts;
        bool hasNextMaxBlockPts;
        // A text subtitle has a length of its own: the latest end of one, for the Duration
        int64_t maxTextEnd = 0;
        bool hasTextEnd = false;

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
              defaultExplicit(false),
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
              pendingDts(0),
              pendingFlags(0),
              hasPendingFrame(false),
              maxBlockPts(0),
              nextMaxBlockPts(0),
              hasMaxBlockPts(false),
              hasNextMaxBlockPts(false)
        {
        }
    };

    // ── Cue point for building the Cues element at close ──
    struct CueEntry
    {
        int64_t timecodeMs;  // cluster-relative time in ms
        int trackNumber;
        int64_t clusterOffset;          // byte offset of the cluster from segment data start
        int64_t relativePosition = -1;  // where the block lies in its cluster, when it is not the cluster's first
        int64_t durationMs = -1;        // a subtitle's BlockDuration, when it has one
    };
    void addCue(const MkvTrackInfo& track, int64_t relMs, int64_t durationMs);
    bool audioCueDue(const MkvTrackInfo& track, int64_t relMs);

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
    std::vector<uint8_t> buildTracksElement(std::vector<uint8_t>& allEntries,
                                            std::map<int, int64_t>& channelsAtInEntries, int sizeBytes);
    void noteTracksWritten(int64_t elementFilePos, const std::vector<uint8_t>& element,
                           std::vector<uint8_t>& allEntries, const std::map<int, int64_t>& channelsAtInEntries);
    void writeVoidExact(int64_t totalBytes);
    void rewriteTracks();
    void lateTrackArrived(int streamIdx);
    // Write a single TrackEntry and return its serialized bytes
    std::vector<uint8_t> buildTrackEntry(const MkvTrackInfo& track, int* channelsValueAt = nullptr);
    static int writeColourInfo(uint8_t* dst, const MkvTrackInfo& track);

    // Start a new cluster at the given timecode (milliseconds)
    // Returns how far the cluster it closed moved the part's timeline back (flushCluster): a time taken before the call
    // is that much later than the same time taken after it.
    int64_t startCluster(int64_t timecodeMs);
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
    // "  start-code-track-uid-<TrackUID>   rule" for each video track whose source used a framing of its own
    std::string m_startCodeTrackLines;
    // Build the SeekHead element, header included, so it can be written in both places.
    [[nodiscard]] std::vector<uint8_t> buildSeekHead() const;
    // Write the SeekHead at the end of the file, where it goes only when it does not fit at the front.
    void writeSeekHead();
    // Write the SeekHead into the space reserved at the front of the segment. False when it does not fit.
    bool writeFrontSeekHead();
    // Write a Void element occupying exactly this many bytes, header included.
    void writeVoid(int totalBytes);
    void putCrcElement(uint8_t* at, uint32_t crc) const;
    // A .webm output: DocType webm and the WebM subset of Matroska (openDstFile).
    bool m_webm = false;
    // A split part starts at the earliest time any picture of it might show, which is not yet known when it is cut. Its
    // first cluster is still in memory when that time is known, and is moved back to it (flushCluster).
    bool m_partOriginPending = false;
    int64_t m_partMinRelMs = INT64_MAX;
    int64_t m_partShiftApplied = 0;  // set by flushCluster when it moved a part back, taken by startCluster
    // --keep-editions: the source's Chapters body and segment identity, written as they stand.
    bool m_keepEditions = false;
    AbstractDemuxer::SourceEditions m_sourceEditions;
    std::vector<uint8_t> m_sourceChapterTree;  // setSourceChapterTree
    int64_t m_treeCutStartNs = 0;
    int64_t m_treeOriginBeforeNs = 0;  // setCutOrigin: how long before the cut the output begins
    int64_t m_treeCutEndNs = INT64_MAX;
    bool m_writingPart = false;  // writePartChapters is writing the chapters of one part of a split
    double m_partStartSec = 0, m_partEndSec = 0;
    bool m_rightEyeFirst = false;
    bool writeChapterTree();

   public:
    // The base view of a 3D track is the right eye (--right-eye, or a playlist that says so): StereoMode 14.
    void setRightEyeFirst(const bool value) { m_rightEyeFirst = value; }
    void setSourceEditions(const AbstractDemuxer::SourceEditions& editions)
    {
        m_keepEditions = true;
        m_sourceEditions = editions;
    }
    // A Matroska source's Chapters element and the cut, on the source's clock in ns (cutEnd INT64_MAX without one).
    // Kept only when the flat list of the default edition would lose something: another edition, a hidden chapter or a
    // nested one. Every edition is then written, moved by the cut and by each part of a split.
    void setSourceChapterTree(const std::vector<uint8_t>& chapters, int64_t cutStartNs, int64_t cutEndNs);

   private:
    // Low-level: write bytes to the output file
    void writeToFile(const uint8_t* data, int len);
    // CRC-32 as the first child of every top-level element (RFC 9559, 6.2). beginCrc writes the element with a
    // zero value and counts every byte written from then on; endCrc writes the value it came to.
    void beginCrc();
    void endCrc();
    // Change bytes of the Info or Tracks body already written, and the CRC-32 that covers them.
    void patchHeaderBody(std::vector<uint8_t>& body, int64_t bodyFilePos, int64_t crcValueFilePos, size_t at,
                         const uint8_t* bytes, size_t len);
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
    int m_clusterBlocks = 0;                  // blocks written to the open cluster
    std::map<int, int64_t> m_lastAudioCueMs;  // track number -> time of its last cue point, in a file without video

    // Positions of key elements (relative to segment data start) for SeekHead
    int64_t m_segmentInfoPos;
    int64_t m_tracksPos;
    int64_t m_cuesPos;
    int64_t m_attachmentsPos = 0;
    int64_t m_chaptersPos = 0;
    int64_t m_tagsPos = 0;
    // Splitting (--split-size, --split-duration, --split-chapters), as for a transport stream.
    int64_t m_splitSize = 0;      // bytes
    int64_t m_splitDuration = 0;  // internal clock
    bool m_splitByChapters = false;
    int m_partNum = 0;  // the part being written, from 1; 0 when the output is not split
    int64_t m_muxStartPts = 0;
    bool m_muxStartSet = false;
    std::vector<int64_t> m_chapterSplitPts;  // --split-chapters: the marks, internal clock from the start
    size_t m_nextChapterSplit = 0;
    std::vector<double> m_allChapters;  // a split output's chapters, seconds from the start of the mux
    std::vector<AVChapter> m_allChapterInfo;
    uint64_t m_mainTrackNumber = 0;  // the track whose key frames a split may start at
    bool m_splitByAnyAudio = false;  // an output without video: every audio track's frames may start a part
    bool m_startingPart = false;     // startNextPart is writing what belongs to the part that ends
    [[nodiscard]] bool drivesSplit(const MkvTrackInfo& track) const;
    [[nodiscard]] bool isSplitting() const { return m_splitSize > 0 || m_splitDuration > 0 || m_splitByChapters; }
    [[nodiscard]] std::string partFileName(int part) const;
    bool splitDue(int64_t pts);
    void finishPart(int64_t partEndPts);
    void writePartChapters(int64_t partEndPts);
    std::string m_title;  // --title=, or the first source's title
    bool m_titleFromMuxOpt = false;
    std::vector<std::vector<uint8_t>> m_sourceAttachments;                                // whole AttachedFile elements
    std::vector<std::vector<uint8_t>> m_sourceTags;                                       // whole Tag elements
    std::map<int, std::vector<AbstractDemuxer::SourceTrackTag>> m_sourceTrackTags;        // by stream index
    std::map<int, std::pair<uint64_t, std::vector<std::vector<uint8_t>>>> m_srcBlockAdd;  // setSourceBlockAdditions
    [[nodiscard]] uint64_t sourceAttachmentBytes() const;
    void writeSourceAttachments();
    void writeTags();

    // Space held at the front of the segment for the seek index, filled in at close. Cues and the
    // attachments are written after the clusters because they are not known until then, and
    // without an index at the front other software does not find them.
    int64_t m_seekHeadReservePos = 0;
    static constexpr int SEEKHEAD_RESERVE = 256;  // five entries need about 120
    static constexpr int VOID_MIN_BYTES = 3;      // one byte of ID plus a two byte size

    // Timecode tracking
    int64_t m_firstTimecode;  // the timeline's origin in INTERNAL_PTS_FREQ units (see replayBufferedPackets)
    bool m_firstTimecodeSet;
    int64_t m_durationValueFilePos;  // absolute file position of the Duration float64 value
    bool m_crcActive = false;
    uint32_t m_crcValue = 0;
    int64_t m_crcValuePos = 0;
    // The bodies of Info and Tracks as written, for the values patched in at the end, and where they and their
    // CRC-32 values are in the file.
    std::vector<uint8_t> m_infoBody, m_tracksBody;
    int64_t m_infoBodyFilePos = 0, m_infoCrcFilePos = 0, m_tracksBodyFilePos = 0, m_tracksCrcFilePos = 0;
    // stream index -> absolute file position of the one byte value of an audio track's Channels element
    std::map<int, int64_t> m_channelsValueFilePos;

    // Deferred header writing: SegmentInfo + Tracks are written once ALL tracks
    // have received at least one packet, because stream readers haven't fully
    // initialized (e.g. audio sample rate / channels) at openDstFile time. A track
    // that has sent nothing after ten seconds of packets is not waited for (muxPacket).
    bool m_headerWritten;
    // Has the destination actually been created yet? openDstFile deliberately does not create it.
    bool m_destinationOpen;
    // Stream indices that have sent at least one packet AND need to before the header can be
    // written, which is every kind of track except subtitles. See muxPacket.
    std::set<int> m_seenStreams;
    // Pictures each video track has delivered before the header, counted by distinct timestamp
    std::map<int, std::pair<int64_t, int>> m_preHeaderPictures;
    static constexpr int PRE_HEADER_PICTURES = 16;
    // ... or, for a video track with fewer pictures than that, until the held packets span this long
    static constexpr int64_t PRE_HEADER_MAX_SPAN = 10 * INTERNAL_PTS_FREQ;
    bool m_preHeaderSpanStarted = false;
    int64_t m_preHeaderMinPts = 0;
    // Tracks the header did not wait for, because the packets held for them already spanned PRE_HEADER_MAX_SPAN with
    // nothing from them (muxPacket). Each keeps this much room after the Tracks element, to complete its entry in.
    std::set<int> m_lateStreams;
    static constexpr int64_t LATE_TRACK_RESERVE = 64 * 1024;
    // Absolute file position where the room after the Tracks element of the current file ends, 0 for none
    int64_t m_tracksReservedEnd = 0;

    // Buffered packets accumulated before the header is written.
    struct BufferedPacket
    {
        int stream_index;
        int64_t pts;
        int64_t dts;
        uint32_t flags;
        int64_t duration = 0;
        std::vector<uint8_t> data;
    };
    std::vector<BufferedPacket> m_preHeaderPackets;

    // Refresh track properties from codec readers (called when readers are initialized)
    void refreshTrackProperties();
    void refreshTrackFromReader(MkvTrackInfo& track);
    bool makeAc3CoreTrack(int streamIdx, MkvTrackInfo& track, MkvTrackInfo& core);
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
    // Replay buffered pre-header packets; sets m_firstTimecode (the earliest PTS, or 0 for subtitles alone)
    void replayBufferedPackets();
    [[nodiscard]] size_t tracksNeedingFirstPacket() const;

    // Flush any pending accumulated frame data for a track as a single SimpleBlock.
    void flushPendingFrame(MkvTrackInfo& track);
    void startNextPart(MkvTrackInfo& current, int64_t origin);
    // Emit one completed frame. Shared by the ordinary path and by the dual layer Dolby Vision
    // path, which writes its frames later than it finishes them.
    void writeBlock(MkvTrackInfo& track, const uint8_t* frameData, int frameSize, int64_t pts, uint8_t pendingFlags);
    bool writeAacRawBlocks(MkvTrackInfo& track, const uint8_t* frame, int frameSize, int rdb);
    // Text subtitles: one BlockGroup with its BlockDuration per subtitle, held until the other tracks
    // reach its time so that every cluster is written in time order
    void writeTextBlock(MkvTrackInfo& track, const uint8_t* data, int size, int64_t pts, int64_t duration,
                        const std::vector<uint8_t>& additions = {});
    void flushHeldText(int64_t upToPts);
    struct HeldText
    {
        int streamIndex;
        int64_t duration;
        std::vector<uint8_t> data;
        std::vector<uint8_t> additions;  // the BlockAdditions body of a block copied as it is
    };
    std::multimap<int64_t, HeldText> m_heldText;
    void drainHeldFrames(MkvTrackInfo& track, bool atEndOfStream);
    void finishFoldedTracks();  // release every held fold frame and report the merge, once
    bool m_foldsFinished = false;

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
