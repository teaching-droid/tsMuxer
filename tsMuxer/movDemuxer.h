// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef MOV_DEMUXER_H_
#define MOV_DEMUXER_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "bufferedReaderManager.h"
#include "ioContextDemuxer.h"

class MovDemuxer final : public IOContextDemuxer
{
   public:
    MovDemuxer(const BufferedReaderManager& readManager);
    ~MovDemuxer() override { readClose(); }
    void openFile(const std::string& streamName) override;
    // The file open now (for a reader's messages)
    [[nodiscard]] const std::string& fileName() const { return m_fileName; }
    // A file whose index lists data past its end, as a file that was cut off: from when (s, in the file's own time)
    // and in which track the first of it is missing. Asked once the mux is over (main.cpp).
    struct CutOff
    {
        double from = 0;
        unsigned track = 0;
    };
    static std::map<std::string, CutOff>& cutOffFiles();
    std::string getTitle() override;
    std::vector<AVChapter> getChapters() override;
    std::vector<std::vector<uint8_t>> getPassThroughAttachments() override;
    void readClose() override;
    int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo>& trackList) override;
    int64_t getTrackDelay(const int32_t pid) override
    {
        // This track's start relative to the first video track's, in ms (rounded), as the TS and PS demuxers give it.
        // For a track with an edit list start (srccodecdelay), less what it skips and plus the pictures a cut file's
        // video keeps (srcvideolead): where its sound is put when no timeshift= is given (METADemuxer::addStream). It
        // was left out, so the listing gave a plain MP4's sound no delay, and a demux and a mux of the demuxed files
        // lost it, the priming of AAC (23 ms at 44.1 kHz) put the sound that much late. A fragmented file says the same
        // start in its tfdt, which was listed.
        int64_t ns = trackStartFromVideoNs(pid);
        if (const auto edit = m_codecDelayNs.find(pid); edit != m_codecDelayNs.end() && edit->second > 0)
            ns += (m_videoLeadNs > 0 ? m_videoLeadNs : 0) - edit->second;
        return (ns >= 0 ? ns + 500000 : ns - 500000) / 1000000;
    }
    double getTrackFps(uint32_t trackId) override;
    static int readPacket(AVPacket&) { return 0; }
    void setFileIterator(FileNameIterator* itr) override;
    [[nodiscard]] bool isPidFilterSupported() const override { return true; }
    [[nodiscard]] int64_t getFileDurationNano() const override;
    const uint8_t* getTrackCodecPrivate(int32_t pid, int& size) override;
    // where track pid starts against the first video track, in ns (the text reader moves its cues by it)
    [[nodiscard]] int64_t trackStartNs(const int32_t pid) const { return trackStartFromVideoNs(pid); }
    // Where the open file of a join starts: the pictures of the files before it, as the video reader times them (the
    // text reader moves its cues by it as well)
    [[nodiscard]] int64_t joinStartNs() const { return m_joinStartNs; }
    // How long the pictures of the open file last as the video reader times them, one usual picture length each; -1
    // for a file without video
    [[nodiscard]] int64_t videoLengthNs() const;

   private:
    struct MOVAtom
    {
        MOVAtom() : type(0), offset(0), size(0) {}
        MOVAtom(const uint32_t _type, const int64_t _offset, const int64_t _size)
            : type(_type), offset(_offset), size(_size)
        {
        }
        uint32_t type;
        int64_t offset;
        int64_t size;  // total size (excluding the size and type fields)
    };

    struct MOVFragment
    {
        int track_id;
        int64_t base_data_offset;
        int64_t moof_offset;     // where the next track fragment's data begins when its header names no base
        int64_t moof_start = 0;  // where the moof box begins
        int runs = 0;            // track runs read in the current track fragment
        unsigned stsd_id;
        unsigned duration;
        unsigned size;
        unsigned flags;
    };

    struct MOVTrackExt
    {
        int track_id;
        unsigned stsd_id;
        unsigned duration;
        unsigned size;
        unsigned flags;
    };

    int found_moov;  // when both 'moov' and 'mdat' sections has been found
    bool found_moof;
    int64_t m_mdat_pos;
    int64_t m_mdat_size;
    int64_t m_fileSize;
    int m_overreadWarnings = 0;  // boxes whose parser read past their end, warned once
    int m_pastEndWarnings = 0;   // boxes whose size reaches past the end of the file, warned once
    void warnUnreadBoxes(int64_t from) const;
    int m_joinPart = 0;  // which file of a join is open: 0 the first
    // where the open file of a join starts (joinStartNs)
    int64_t m_joinStartNs = 0;
    // An audio track of a join (dropJoinPriming): where its first frame plays against the join's first picture, the
    // frames handed on so far, and where the pictures of the parts so far end
    struct JoinAudio
    {
        int64_t firstOffsetNs = 0;
        int64_t frames = 0;
        int64_t videoEndNs = 0;
        int64_t timeScale = 0;
        int64_t frameLen = 0;
        int dropped = 0;    // frames left out at the start of the open part
        int64_t gapNs = 0;  // gaps put into the track's times so far (m_timeGaps)
        int64_t padNs = 0;  // Opus: the padding at the end of the part read last, after its edit
        // Opus: as its reader counts at each joint (OpusStreamReader::readPacket), the padding and pre-skip not yet
        // made up for by a packet left out, in samples, and the packets it left out since the last gap
        int64_t opusExcess = 0;
        int64_t opusLeftOutNs = 0;
    };
    std::map<int, std::vector<std::pair<int64_t, int64_t>>> m_timeGaps;  // by track, see takeTimeGaps
    std::map<int, JoinAudio> m_joinAudio;
    [[nodiscard]] int64_t soundOffsetNs(int index) const;
    [[nodiscard]] int64_t partVideoNs(int audioIndex) const;
    void accountJoinPart();
    void dropJoinPriming();
    std::vector<std::pair<int64_t, int64_t>> takeTimeGaps(int pid) override;
    std::map<int, std::vector<int64_t>> m_touchingCues;                       // by track, see takeTouchingCues
    std::map<int, std::vector<std::pair<int64_t, int64_t>>> m_nextCueStarts;  // by track, see takeNextCueStarts

   public:
    void addTouchingCue(const int pid, const int64_t ms) { m_touchingCues[pid].push_back(ms); }
    std::vector<int64_t> takeTouchingCues(int pid) override;
    void addNextCueStart(const int pid, const int64_t ms, const int64_t nextMs)
    {
        m_nextCueStarts[pid].emplace_back(ms, nextMs);
    }
    std::vector<std::pair<int64_t, int64_t>> takeNextCueStarts(int pid) override;

   private:
    std::vector<std::pair<int64_t, int64_t>> topLevelMdats() const;
    uint32_t m_timescale;
    // Where each track starts on the movie's timeline, in ns: its leading empty edits, or without an edit list the
    // composition time of its first sample, a fragmented track's first tfdt, and a start offset written as a long
    // first sample (longFirstSamples). The listing gives it as the Stream delay against the first video track, and
    // the mux applies it where the meta gives no timeshift= (srcstartdelay).
    std::map<int32_t, int64_t> m_trackStartNs;
    [[nodiscard]] int64_t trackStartFromVideoNs(int32_t pid) const;
    void longFirstSamples();
    void pictureLengths();
    void aacWithoutConfig();
    std::map<int32_t, int64_t> m_emptyEditUnits;  // leading empty edits per track, in movie units (mov_read_elst)
    std::map<int32_t, int64_t> m_editMediaTime;   // where a track's first edit starts in its media (mov_read_elst)
    std::map<int32_t, int64_t> m_editDuration;    // how long that edit plays, in the movie's time scale
    std::map<int32_t, int64_t> m_codecDelayNs;    // an audio track's edit list start, as CodecDelay for Matroska
    int64_t m_videoLeadNs = 0;     // what the first video track's edit list skips beyond its first composition offset
    int32_t m_videoLeadTrack = 0;  // the track m_videoLeadNs is taken from (0: none)
    // A fragmented file: where each track's first fragment starts (tfdt) and the composition offset of its first
    // sample, in the track's own time scale, and the edit list start of a video track (fragmentStartDelays)
    std::map<int32_t, int64_t> m_firstTfdt;
    std::map<int32_t, int64_t> m_firstCompositionOffset;
    std::map<int32_t, int64_t> m_videoMediaTime;
    // the edits that play something, per track: their duration in movie units and their start in media units
    std::map<int32_t, std::vector<std::pair<int64_t, int64_t>>> m_playedEdits;
    int mov_read_tfdt(MOVAtom atom);
    void fragmentStartDelays();
    // List<chunk offset, chunk size>
    std::vector<std::pair<int64_t, int64_t>> m_mdat_data;
    // The size of the chunk of a fragmented file at offset (from the first mdat at mdatPos) whose successor is at next.
    [[nodiscard]] int fragmentChunkSize(int64_t mdatPos, int64_t offset, int64_t next, bool last) const;
    int itunes_metadata;  ///< metadata are itunes style
    int64_t moof_offset;
    std::map<std::string, std::string> metaData;
    // Metadata a Matroska output can carry (getTitle, getChapters, getPassThroughAttachments, the track names)
    std::vector<AVChapter> m_chplChapters;    // Nero chapters (udta/chpl)
    std::vector<uint32_t> m_chapterTrackIds;  // tracks a tref 'chap' names: QuickTime chapter text, not media
    std::vector<uint8_t> m_cover;             // ilst covr: the picture
    uint32_t m_coverType = 0;                 // its data type: 13 JPEG, 14 PNG, 27 BMP
    bool m_inIlst = false;
    bool m_inTrak = false;  // a udta string inside a trak names that track, not the movie
    int mov_read_meta(MOVAtom atom);
    int mov_read_ilst(MOVAtom atom);
    int mov_read_covr(MOVAtom atom);
    int mov_read_chpl(MOVAtom atom);
    int mov_read_tref_chap(MOVAtom atom);
    int mov_read_colour(MOVAtom atom);  // colr, mdcv, clli, CoLL and pasp of a video sample entry
    std::vector<AVChapter> chapterTrackChapters() const;
    MOVFragment fragment;  ///< current fragment in moof atom
    std::vector<MOVTrackExt> trex_data;
    int64_t fileDuration;
    int isom;
    std::vector<std::pair<int64_t, int64_t>> chunks;
    // The size of each chunk of `chunks` by its own track's tables (stsc and stsz), or -1 where they do not say.
    std::vector<int64_t> m_chunkBytes;
    size_t m_curChunk;
    AVPacket m_deliveredPacket;
    std::vector<uint8_t> m_tmpChunkBuffer;
    bool m_firstDemux;
    bool m_readerUsed;
    FileNameIterator* m_fileIterator;
    std::string m_fileName;
    MemoryBlock m_filterBuffer;
    int64_t m_firstHeaderSize;
    // Where the box that ended the header walk ends, or 0 when the walk ended without one.
    int64_t m_headerWalkEnd = 0;

    void readHeaders();
    void buildIndex();
    void takeFreshReader();
    void seekOnFreshReader(int64_t pos);
    int ParseTableEntry(MOVAtom atom);
    // The bytes of a box not read yet. A table of a damaged file names more entries than its box holds, up to 2^32,
    // which were asked for in memory or read one by one far past the box: no table is longer than its box.
    [[nodiscard]] int64_t atomLeft(const MOVAtom& atom) const { return atom.size - (m_processedBytes - atom.offset); }
    int mov_read_default(MOVAtom atom);
    int mov_read_extradata(MOVAtom atom);
    int mov_read_mdat(MOVAtom atom);
    int mov_read_stss(MOVAtom atom);
    int mov_read_stsz(MOVAtom atom);
    int mov_read_stts(MOVAtom atom);
    int mov_read_tkhd(MOVAtom atom);
    int mov_read_tfhd(MOVAtom atom);
    int mov_read_trak(MOVAtom atom);
    int mov_read_trex(MOVAtom atom);
    int mov_read_trkn(MOVAtom atom);
    int mov_read_trun(MOVAtom atom);
    int mov_read_wide(MOVAtom atom);
    int mov_read_cmov(MOVAtom atom);
    int mov_read_udta_string(MOVAtom atom);
    int mov_read_moof(MOVAtom atom);
    int mov_read_moov(MOVAtom atom);
    int mov_read_mvhd(MOVAtom atom);
    int mov_read_mdhd(MOVAtom atom);
    int mov_read_stsd(MOVAtom atom);
    int mov_read_stco(MOVAtom atom);
    int mov_read_glbl(MOVAtom atom);
    int mov_read_vvcc(MOVAtom atom);
    int mov_read_ftyp(MOVAtom atom);
    int mov_read_hdlr(MOVAtom atom);
    int mov_read_esds(MOVAtom atom);
    int mov_read_dref(MOVAtom atom);
    int mov_read_stsc(MOVAtom atom);
    int mov_read_wave(MOVAtom atom);
    int mov_read_ctts(MOVAtom atom);
    int mov_read_elst(MOVAtom atom);

    // int mov_read_enda(MOVAtom atom);
    // int mov_read_ilst(MOVAtom atom);
    // int mov_read_meta(MOVAtom atom);
    // int mov_read_pasp(MOVAtom atom);
    int mp4_read_descr(int* tag);
};

#endif
