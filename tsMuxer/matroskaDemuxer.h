#ifndef MATROSKA_STREAM_READER_H_
#define MATROSKA_STREAM_READER_H_

#include <map>
#include <memory>
#include <set>

#include "assRender.h"
#include "bufferedFileReader.h"
#include "ioContextDemuxer.h"
#include "matroskaParser.h"
#include "vorbisStreamReader.h"

// One part of the timeline a Matroska file plays (MatroskaDemuxer::playTimeline).
struct MatroskaPlayPart
{
    std::string file;
    JoinRange range;
};

class MatroskaDemuxer final : public IOContextDemuxer
{
   public:
    MatroskaDemuxer(const BufferedReaderManager& readManager);
    ~MatroskaDemuxer() override { readClose(); }
    void openFile(const std::string& streamName) override;
    int readPacket(AVPacket& avPacket);  // not implemented
    void readClose() override;
    static void freeTrack(MatroskaTrack* track);
    int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo>& trackList) override;
    std::vector<AVChapter> getChapters() override;
    // The end of the data only once the stream has been demuxed to its end. The last block of a file comes with the
    // end flag, and this demuxer reads it byte by byte and may stop part way through it, its demux block full. The
    // flag was passed on as it stood, and the reader of the track took the data of that call for the last: the rest of
    // the block was never demuxed. A join of two Matroska files of 1.26 MB each lost 45 of its 254 pictures that way.
    // Once the demux has ended it is the end, also where the parser stopped before the end of the file, at data it
    // cannot read: the reader of the file had not reached its end, the track readers were told "not ready", and each
    // asked again for ever, the parser ending at once each time.
    int getLastReadRez() override
    {
        if (m_demuxEnded)
            return AbstractReader::DATA_EOF;
        return m_lastReadRez == AbstractReader::DATA_EOF && (m_curPos < m_bufEnd || !packets.empty()) ? 0
                                                                                                      : m_lastReadRez;
    }
    // The parts a player plays for this file, in order: those of its default edition when that edition is ordered,
    // or the files of a hard-linked chain when this file is the first of it. Empty when the file plays as it is.
    // `how` says what was found, for the log.
    static std::vector<MatroskaPlayPart> playTimeline(const std::string& file, std::string& how);
    [[nodiscard]] SourceEditions getSourceEditions() override;

    [[nodiscard]] bool isPidFilterSupported() const override { return true; }
    // Whether the track of a pid of getTrackList is audio or video
    [[nodiscard]] bool isAudioOrVideo(const int32_t pid) const
    {
        return pid >= 1 && pid <= num_tracks && tracks[pid - 1] &&
               (tracks[pid - 1]->type == MatroskaTrackType::VIDEO || tracks[pid - 1]->type == MatroskaTrackType::AUDIO);
    }
    int64_t getTrackDelay(const int32_t pid) override
    {
        if (m_firstTimecode.find(pid) == m_firstTimecode.end())
            return 0;

        // Find the first video track's timecode as reference point,
        // matching how TS/PS demuxers compute delay relative to video
        for (int i = 0; i < num_tracks; i++)
        {
            if (tracks[i]->type == IOContextTrackType::VIDEO)
            {
                // Measured from the picture the video shows first, which for VC-1 in the Video for Windows
                // mapping can come after its first block (vfwVc1FirstShown), and for MPEG-1 and MPEG-2 before
                // it, or after it in a file cut inside a group; a second MPEG video track from the picture it
                // shows first as well.
                const auto it = m_firstTimecode.find(tracks[i]->num);
                const auto shown = m_shownAfterFirstBlock.find(tracks[i]->num);
                const auto own = m_shownAfterFirstBlock.find(pid);
                if (it != m_firstTimecode.end() && tracks[i]->num != pid)
                    return m_firstTimecode[pid] + (own == m_shownAfterFirstBlock.end() ? 0 : own->second) -
                           it->second - (shown == m_shownAfterFirstBlock.end() ? 0 : shown->second);
                if (it != m_firstTimecode.end())
                    return 0;
            }
        }

        return 0;
    }
    double getTrackFps(uint32_t trackId) override;
    const uint8_t* getTrackCodecPrivate(int32_t pid, int& size) override;

    [[nodiscard]] int64_t getFileDurationNano() const override { return fileDuration; }
    // The file's WritingApp (Info), empty when it has none
    [[nodiscard]] std::string writingApplication() const { return writing_app ? writing_app : ""; }
    [[nodiscard]] int64_t getDemuxedTime() const override { return m_demuxedTime; }
    bool getVideoSpanNano(int64_t& startNano, int64_t& endNano) override;

    // Fetch an attached file by name. The bytes are read from the file when asked for, not held
    // from the header parse: one of these attachments is the preserved Dolby Vision metadata of a
    // whole feature, tens of megabytes, and the file is opened and thrown away twice during track
    // detection before anything wants it.
    bool getAttachment(const std::string& name, std::vector<uint8_t>& out) override;
    std::string getTitle() override { return m_title; }
    std::vector<std::vector<uint8_t>> getPassThroughAttachments() override;
    std::vector<std::vector<uint8_t>> getGlobalTags() override;
    std::map<uint64_t, std::vector<SourceTrackTag>> getTrackTags() override;
    [[nodiscard]] uint64_t getTrackUid(int pid) const override
    {
        return pid >= 1 && pid <= num_tracks && tracks[pid - 1] ? tracks[pid - 1]->uid : 0;
    }

    // A joined input, "a"+"b": the files in the order they are read, so that each part is framed as its own record
    // says when the stream reaches it.
    void setFileIterator(FileNameIterator* itr) override;

    // What a track needs to be copied as it is into a Matroska output: the source's CodecID, CodecPrivate and
    // BlockAdditionMapping bodies, from the header alone, so asking reads nothing else of the file.
    struct CopySource
    {
        std::string codecId;
        std::vector<uint8_t> codecPrivate;
        std::vector<std::vector<uint8_t>> addMappings;
        uint64_t maxBlockAddId = 0;  // MaxBlockAdditionID, 0 when the track's blocks have no BlockAdditions
    };
    bool getCopySource(int pid, CopySource& out) const;
    // From now on, and in every later file of a join, hand the track on block for block (ParsedCopyTrackData).
    void setTrackCopy(int pid);
    // From now on, and in every later file of a join, draw the ASS or SSA track with libass into PGS (AssDecoder).
    void setAssDrawing(int pid, const ass_render::Drawing& drawing);
    bool getBlockAdditionInfo(int pid, uint64_t& maxId, std::vector<std::vector<uint8_t>>& mappings) const override;
    bool takeBlockAdditions(int pid, int64_t pts, std::vector<uint8_t>& out) override;
    void keepBlockAdditions(const int pid) override { m_keepAdditionPids.insert(pid); }
    // A file cut off inside a block: from when (s, in the file's own time) and in which track (as track= counts) the
    // first of it is missing. Asked once the mux is over (main.cpp), as MovDemuxer::cutOffFiles.
    struct CutOff
    {
        double from = 0;
        unsigned track = 0;
    };
    static std::map<std::string, CutOff>& cutOffFiles();

   private:
    typedef Track MatroskaTrack;
    // setTrackCopy's tracks, and every track's BlockAdditionMapping bodies, by the pid of getTrackList (index + 1).
    std::set<int> m_copyPids;
    std::map<int, std::vector<uint8_t>> m_copyPrivates;  // the CodecPrivate of each copied track in the first file
    std::map<int, ass_render::Drawing> m_assPids;        // setAssDrawing's tracks
    std::map<int, std::vector<std::vector<uint8_t>>> m_addMappings;
    std::map<const Track*, std::vector<std::vector<uint8_t>>> m_addMappingsByTrack;  // while the header is read
    std::map<int, uint64_t> m_maxAddIds;                                             // MaxBlockAdditionID by pid
    std::map<const Track*, uint64_t> m_maxAddIdsByTrack;                             // while the header is read
    std::vector<uint8_t> m_pendingAdditions;  // the BlockAdditions of the BlockGroup being read
    // The BlockAdditions of the blocks of every other track, by pid and time, until the muxer takes them
    std::map<int, std::map<int64_t, std::vector<uint8_t>>> m_blockAddLog;
    std::set<int> m_keepAdditionPids;  // keepBlockAdditions
    void applyTrackCopy();
    void applyAssDrawing();
    typedef IOContextTrackType MatroskaTrackType;

    typedef struct MatroskaLevel
    {
        int64_t start;
        int64_t length;
        bool unknownSize;  // the size field has all its value bits set (RFC 8794, 6.2)
    } MatroskaLevel;

    typedef struct MatroskaDemuxIndex
    {
        uint64_t pos;  /* of the corresponding *cluster*! */
        int16_t track; /* reference to 'num' */
        uint64_t time; /* in nanoseconds */
    } MatroskaDemuxIndex;

    // ffmpeg matroska vars
    MatroskaLevel levels[EBML_MAX_DEPTH];
    std::queue<AVPacket*> packets;
    std::vector<MatroskaDemuxIndex> indexes;
    // std::vector<MatroskaDemuxLevel> levels;
    int num_levels;
    int level_up;
    int peek_id;
    bool done;
    bool m_demuxEnded = false;      // simpleDemuxBlock has met the end of the stream
    int64_t m_joinShiftNs = 0;      // a plain join: where the part being read is put, against its own clock
    bool m_tracksFinished = false;  // the converters were asked for what they still held
    bool queueFinalPackets();
    char m_title[1024];

    uint64_t segment_start;
    uint64_t created;
    int64_t fileDuration;
    bool m_infoRead = false;  // the Info element was read in the header
    char* writing_app;
    char* muxing_app;
    uint64_t time_scale;
    std::map<int64_t, int64_t> m_firstTimecode;
    // For a video track whose first picture is shown after its first block, how long after, in the file's units; less
    // than 0 where it is shown before it (the B pictures of an MPEG-1 or MPEG-2 group that is not closed). Either way
    // for MPEG-1 and MPEG-2 whose first blocks come before the first sequence header, which are not decoded.
    std::map<int64_t, int64_t> m_shownAfterFirstBlock;
    bool index_parsed;
    bool metadata_parsed;
    bool attachments_parsed;
    int num_streams;

    // Where each attached file's payload lives, rather than the payload itself.
    struct MatroskaAttachment
    {
        std::string name;
        int64_t dataPos = 0;
        int64_t dataSize = 0;
    };
    std::vector<MatroskaAttachment> m_attachments;
    std::string m_attachmentSource;  // the file the payloads are read back from
    std::string m_sourceName;        // the file openFile opened: getVideoSpanNano reads it on a handle of its own
    void loadAttachments(const std::string& fileName);
    void loadChapters(const std::string& fileName);
    void loadTrackExtras(const std::string& fileName);
    // Per track number, what loadTrackExtras found, under the keys the muxers read (srcflags, srccolour, ...).
    std::map<int64_t, std::map<std::string, std::string>> m_trackExtras;
    std::map<uint32_t, double> m_estimatedFps;  // getTrackFps for a video track without DefaultDuration, or of fields
    bool measureBlockTimes(const MatroskaTrack* track, bool& fields, double& spacingNs, size_t& count,
                           bool* progressive = nullptr);
    static double standardFps(double fps);
    void applyStartCodeRule();
    std::vector<std::string> m_joinFiles;  // the files of a joined input, in order; empty for a single file
    size_t m_joinPart = 0;                 // the part being read: one more for each header met inside the stream
    std::vector<JoinRange> m_joinRanges;   // a play timeline: the stretch of each part; empty for a plain join
    bool m_partVideoOn = false;            // the part's video has reached its first key frame
    bool m_partVideoDone = false;          // and its last
    bool keepBlock(const MatroskaTrack* track, int64_t ns, bool keyFrame);
    void nextJoinPart();
    // A plain join: the sound of each later part starts against its own picture (see keepJoinSound). Times in ns, on
    // the timeline of the join (m_joinShiftNs).
    struct JoinSound
    {
        int64_t origin = -1;       // where the first frame of the track is, the start of its reader's own clock
        int64_t end = -1;          // where the sound handed on so far ends; -1: not known
        int64_t prev = -1;         // the frame before in the part, for the step
        int64_t step = 0;          // the shortest time from one frame to the next
        int64_t lastStep = 0;      // the time from the frame before to the last one
        int vorbisFilePrev = 0;    // Vorbis: the block size of the packet before in the part being read, 0 at its start
        int vorbisHandedPrev = 0;  // and of the packet handed on before, which the reader counts on from
        int64_t shift = 0;         // how much later than its own times the sound of the part being read is muxed
        int64_t pad = 0;           // the padding at the end of the sound handed on so far (DiscardPadding)
        int64_t excess = 0;        // what is not sound at the joints so far and is still played (a codec delay)
        bool offered = false;      // a frame of the track has come in the part
        bool started = false;      // a frame of the track has been kept in the part
        bool pause = false;        // the sound of the part pauses before the frame that comes next (soundPause)
    };
    std::map<int, JoinSound> m_joinSound;  // by track (stream index)
    bool m_joinShiftKnown = true;  // where each part is put is known (m_joinShiftNs): a part of unknown length is not
    std::map<int, std::vector<std::pair<int64_t, int64_t>>> m_timeGaps;  // by track, see takeTimeGaps
    // A sound track of one file that has no block for a while, with the picture going on (see noteSoundGap), in ns
    struct SoundGap
    {
        int64_t origin = -1;  // the first block of the track, the start of its reader's own clock
        int64_t prev = -1;    // the latest block time
        int64_t step = 0;     // the shortest step from one block to the next
        int videoJumps = 0;   // m_videoJumps at the block before
    };
    std::map<int, SoundGap> m_soundGap;  // by track (stream index)
    int64_t m_gapVideoPrev = INT64_MIN;  // the time of the first video track's block before
    int m_gapVideoTrack = -1;
    int m_videoJumps = 0;  // how often the first video's time jumped, see noteSoundGap
    void noteSoundGap(int track, int64_t ns);
    int64_t soundPause(int track, int64_t ns);
    void noteGapVideo(int track, int64_t ns);
    bool keepJoinSound(int track, int64_t ns, int64_t lengthNs, int64_t leadNs = 0, int64_t padNs = 0,
                       bool readerTrims = false, bool counted = false, bool variable = false);
    static bool isJoinTimedCodec(const MatroskaTrack* track);
    static bool isJoinDelayCodec(const MatroskaTrack* track);
    static bool isJoinVarCodec(const MatroskaTrack* track);
    std::map<int, std::unique_ptr<VorbisStreamReader>> m_joinVorbis;  // by track: the modes of its setup header
    int joinVorbisBlock(int track, const uint8_t* packet, int size);
    static int flacFrameSamples(const uint8_t* p, int size);
    static int flacSampleRate(const MatroskaTrack* track);
    int64_t m_blockDiscardNs = 0;  // the DiscardPadding of the block group being read
    std::vector<std::pair<int64_t, int64_t>> takeTimeGaps(int pid) override;
    std::vector<int64_t> takeTouchingCues(int pid) override;
    std::vector<std::pair<int64_t, int64_t>> takeNextCueStarts(int pid) override;

   public:
    // A join of Matroska files without video, beside a video joined from as many files: where each file starts, in
    // ns, the first at 0 (METADemuxer::placeJoinedSubtitles)
    void setJoinStarts(std::vector<int64_t> ns) { m_joinStartsNs = std::move(ns); }

   private:
    std::vector<int64_t> m_joinStartsNs;
    std::map<int, std::vector<int64_t>> m_touchingCues;  // by track, see takeTouchingCues
    std::map<int, std::vector<std::pair<int64_t, int64_t>>> m_nextCueStarts;  // by track, see takeNextCueStarts
    int64_t m_clusterPos = -1;                           // where the cluster being read begins in the file
    std::unique_ptr<File> m_aheadFile;                   // the file again, for nextBlockAt
    std::string m_aheadName;
    int64_t m_aheadEnd = 0;
    int64_t nextBlockAt(int64_t trackNumber, int64_t after);

    AVPacket* m_lastDeliveryPacket;

    uint32_t ebml_peek_id(int* levelUp);
    int ebml_read_element_id(uint32_t* id, int* levelUp);
    int ebml_read_num(int max_size, int64_t* number);
    int ebml_read_element_level_up();
    int matroska_parse_cluster();
    // clusterTime: the element is a block of the cluster of that time, which may be cut off by the end of the file
    static constexpr int64_t NOT_A_BLOCK = INT64_MIN;
    int ebml_read_binary(uint32_t* id, uint8_t** binary, int* size, int64_t clusterTime = NOT_A_BLOCK);
    int ebml_read_element_length(int64_t* length);
    int ebml_read_master(uint32_t* id);
    int ebml_read_skip();
    int ebml_read_uint(uint32_t* id, int64_t* num);
    int ebml_read_sint(uint32_t* id, int64_t* num);
    static int matroska_ebmlnum_sint(const uint8_t* data, int32_t size, int64_t* num);
    int matroska_parse_blockgroup(int64_t cluster_time);
    static int matroska_ebmlnum_uint(const uint8_t* data, int32_t size, uint64_t* num);
    [[nodiscard]] int matroska_find_track_by_num(int64_t num) const;
    int matroska_parse_block(uint8_t* data, int size, int64_t pos, int64_t cluster_time, int64_t duration,
                             int is_keyframe, int is_bframe);
    static int rv_offset(const uint8_t* data, int slice, int slices);
    void matroska_queue_packet(AVPacket* pkt);
    int matroska_deliver_packet(AVPacket*& avPacket);
    int matroska_read_header();
    int ebml_read_header(char** doctype, int* version);
    int ebml_read_ascii(uint32_t* id, char** str);
    int matroska_parse_index();
    int matroska_parse_info();
    int ebml_read_date(uint32_t* id, int64_t* date);
    int ebml_read_float(uint32_t* id, double* num);

    int ebml_read_utf8(uint32_t* id, char** str);
    int matroska_parse_metadata();
    int ebml_read_seek(int64_t offset);
    int matroska_parse_tracks();
    // Tracks or Info, read where the SeekHead says when they follow the clusters
    void readAfterClusters(uint32_t wanted);
    int matroska_add_stream();
    static int getTrackType(const MatroskaTrack* track);

    int readTrackEncodings(MatroskaTrack* track);
    int readTrackEncoding(MatroskaTrack* track);
    int readEncodingCompression(MatroskaTrack* track);
    void decompressData(uint8_t* data, int size);

    std::map<uint64_t, AVChapter> chapters;
    MemoryBlock m_tmpBuffer;
    int64_t m_demuxedTime = -1;  // the latest block time read, as the packets carry it (getDemuxedTime)
};

#endif
