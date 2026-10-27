#ifndef MUXER_MANAGER_H_
#define MUXER_MANAGER_H_

#include <memory>
#include <optional>

#include "abstractMuxer.h"
#include "bufferedFileWriter.h"
#include "bufferedReaderManager.h"
#include "metaDemuxer.h"

class FileFactory;

class MuxerManager final
{
   public:
    // The unit of an unbuffered write. A volume whose sectors are larger is written through the system cache
    // instead (TSMuxer::openDstFile).
    static constexpr int32_t PHYSICAL_SECTOR_SIZE = 2048;
    static constexpr int BLURAY_SECTOR_SIZE =
        PHYSICAL_SECTOR_SIZE * 3;  // real sector size is 2048, but M2TS frame required addition rounding by 3 blocks

    MuxerManager(const BufferedReaderManager& readManager, AbstractMuxerFactory& factory);
    ~MuxerManager();

    void setAsyncMode(const bool val) { m_asyncMode = val; }

    [[nodiscard]] bool isAsyncMode() const { return m_asyncMode; }

    bool openMetaFile(const std::string& fileName);
    int addStream(const std::string& codecName, const std::string& fileName,
                  const std::map<std::string, std::string>& addParams);

    void doMux(const std::string& outFileName, FileFactory* fileFactory);

    // Stored, not forwarded here: the output muxer does not exist yet when main() calls this,
    // it is created inside doMux(). Forwarding immediately silently did nothing.
    // Matroska writes these as a Chapters master; every other muxer ignores them, because the
    // Blu-ray path takes the same list a different way.
    void setChapters(const std::vector<double>& chapters) { m_chapters = chapters; }
    void moveChaptersByCut(std::vector<double>& times, std::vector<AVChapter>& info) const;
    void setChapterInfo(const std::vector<AVChapter>& info) { m_chapterInfo = info; }

    void setCutStart(const int64_t value) { m_cutStart = value; }
    [[nodiscard]] int64_t getCutStart() const { return m_cutStart; }

    void setCutEnd(const int64_t value) { m_cutEnd = value; }
    [[nodiscard]] int64_t getCutEnd() const { return m_cutEnd; }
    // --cut-pictures: cover (true, the default) keeps the pictures a cut needs to show every picture inside it, inside
    // (false) keeps only the pictures shown inside the cut, as earlier versions did
    [[nodiscard]] bool cutPicturesCover() const { return m_cutCover; }

    void waitForWriting() const;

    void asyncWriteBuffer(const AbstractMuxer* muxer, uint8_t* buff, int len, AbstractOutputStream* dstFile);
    int syncWriteBuffer(AbstractMuxer* muxer, const uint8_t* buff, int len, AbstractOutputStream* dstFile) const;
    void muxBlockFinished(const AbstractMuxer* muxer);

    void parseMuxOpt(const std::string& opts);
    static void warnUnknownMuxOpts(const std::string& opts);
    // A MUXOPT line with every tab outside quotes made a space, and nothing before MUXOPT: the options are separated
    // by spaces, and a tab joined the texts on either side of it into one option
    static std::string muxOptLine(const std::string& line);
    int getTrackCnt() { return static_cast<int>(m_metaDemuxer.getCodecInfo().size()); }

    /// The tracks and their readers, for a caller that wants to ask them what they did.
    /// Still valid after doMux: METADemuxer::readClose(), which deletes the readers, runs only
    /// from openFile and from the destructor.
    // The MUXOPT line as it was read. main() needs it to say when an option was accepted and
    // then ignored because it does not apply to the chosen output.
    [[nodiscard]] const std::string& getMuxOpts() const { return m_muxOpts; }
    [[nodiscard]] const std::vector<StreamInfo>& getStreamInfo() const { return m_metaDemuxer.getStreamInfo(); }
    // The source file the given name is the same file as, or an empty string: every file a track reads counts
    [[nodiscard]] std::string sourceSameAs(const std::string& fileName) const;
    // A demux into a folder that exists is written into a temporary folder inside it first (KeepOldOutput in main):
    // the names a demux writes are compared with the sources by the folder they end up in, set here
    void setDemuxFolder(const std::string& folder) { m_demuxFolder = folder; }
    // --split-duration, --split-size or --split-chapters was given with a value the split takes
    [[nodiscard]] bool splitAsked() const { return m_isoSplitBytes > 0 || m_isoSplitTicks > 0 || m_isoSplitChapters; }
    [[nodiscard]] const std::string& demuxFolder() const { return m_demuxFolder; }
    // The line doMux prints when the sources are read and the output is being finished; none when empty
    void setFinishingNote(const std::string& note) { m_finishingNote = note; }
    // The length the containers of the sources state, 0 when none does (a raw stream)
    [[nodiscard]] int64_t statedDurationNano() { return m_metaDemuxer.statedDurationNano(); }
    [[nodiscard]] bool getHevcFound() const { return m_metaDemuxer.m_HevcFound; }
    [[nodiscard]] bool getMvcFound() const { return m_metaDemuxer.m_MvcFound; }
    [[nodiscard]] AbstractMuxer* getMainMuxer() const;
    [[nodiscard]] AbstractMuxer* getSubMuxer() const;
    [[nodiscard]] bool isStereoMode() const;

    void setAllowStereoMux(bool value);
    // Before openMetaFile: the output is Matroska (1) or WebM (2), so subtitles of a Matroska source keep their form
    void setMatroskaOutput(const int kind) { m_metaDemuxer.setMatroskaOutput(kind); }
    // The BlockAdditions the source stored beside a frame of a stream, for a Matroska output (see METADemuxer)
    bool takeBlockAdditions(const int streamIndex, const int64_t pts, std::vector<uint8_t>& out)
    {
        return m_metaDemuxer.takeBlockAdditions(streamIndex, pts, out);
    }

    // --right-eye, or a source playlist that says its base view is the right eye: a disc made from such a playlist was
    // written with the left eye as the base view
    [[nodiscard]] bool isMvcBaseViewR() const { return m_mvcBaseViewR || m_metaDemuxer.mplsBaseViewRight(); }
    [[nodiscard]] int64_t totalSize() const { return m_metaDemuxer.totalSize(); }
    // What has actually been read out of the sources. The ratio of this to totalSize() is the
    // figure the progress line prints, and a finished mux must have consumed everything.
    [[nodiscard]] int64_t getDemuxedSize() { return m_metaDemuxer.getDemuxedSize(); }
    // The extra room a disc image keeps for its file system, in blocks of 64 KB (see muxerManager.cpp).
    [[nodiscard]] int getExtraISOBlocks();

    [[nodiscard]] bool useReproducibleIsoHeader() const { return m_reproducibleIsoHeader; }

    // Fit-to-disc guard: target disc capacity in bytes (0 = disabled) + oversize override.
    [[nodiscard]] int64_t getDiscSizeLimit() const { return m_discSizeLimit; }
    [[nodiscard]] bool getAllowOversize() const { return m_allowOversize; }
    // The bytes a constant bitrate output takes, null packets included: the rate the muxer pads to (--bitrate, or
    // --minbitrate under --maxbitrate) over the length of the title, in 192 byte packets. 0 when nothing is padded,
    // and when the sources do not state their length (a raw stream).
    [[nodiscard]] int64_t paddedOutputSize();
    // The source bytes the mux takes, for the fit-to-disc estimate: the sources less the tracks left out of a transport
    // stream file (METADemuxer::selectedSourceSize), and of a cut the part its length takes of the stated length
    [[nodiscard]] int64_t muxedSourceSize();
    // A rate is padded to, and the sources do not state the length it is padded over
    [[nodiscard]] bool paddedLengthUnknown();

    // Dual-layer guard band: MB of zero-fill on each side of the BD-R DL layer break (-1 = off),
    // plus an optional break-sector override for testing / non-standard geometries.
    [[nodiscard]] int getLayerBreakGuardMB() const { return m_layerBreakGuardMB; }
    [[nodiscard]] int getLayerBreakGuardBeforeMB() const { return m_layerBreakGuardBeforeMB; }
    // --no-layer-break-guard: an ISO image that crosses a layer break is written without the guard it gets by default
    [[nodiscard]] bool getNoLayerBreakGuard() const { return m_noLayerBreakGuard; }
    [[nodiscard]] const std::vector<int>& getLayerBreakLbns() const { return m_layerBreakLbns; }
    // --inner-only: a disc image keeps its data on the inner part of each layer (see innerOnlyIsoAsked in main.cpp)
    [[nodiscard]] bool getInnerOnly() const { return m_innerOnly; }
    // --extra-iso-space as it was given, 0 without it, for an image laid out from a folder
    [[nodiscard]] int getExtraIsoSpaceGiven() const { return m_extraIsoBlocksGiven ? m_extraIsoSpace : 0; }

    enum class SubTrackMode
    {
        All,
        Forced
    };

    [[nodiscard]] int getDefaultAudioTrackIdx() const;
    int getDefaultSubTrackIdx(SubTrackMode& mode) const;

   private:
    void preinitMux(const std::string& outFileName, FileFactory* fileFactory);
    void writeChapterFile(const std::string& outDirName);
    // The source's chapters on the output's timeline: only those inside a --cut-start/--cut-end, moved by the start.
    // Read once and kept: the disc path asks before the mux as well, and reading a join's chapters opens every part.
    std::vector<AVChapter> sourceChapters();
    std::vector<AVChapter> readSourceChapters();
    // --cut-start: whether a packet the cut start has not dropped by its time is kept (see doMux).
    bool keptAfterCutStart(AVPacket& avPacket);
    // The video streams a cut begins at a key picture of their own, and the ones that only add to another video
    // (keptAfterCutStart); a video that is not one of these is not video (null)
    void findCutLayers(const std::vector<StreamInfo>& ci);
    static bool isCutVideo(const BaseAbstractStreamReader* reader);
    // --cut-pictures=cover: the time each video's key picture at or before --cut-start has, read before the mux by a
    // pass of its own over the sources (see muxerManager.cpp)
    void findCutKeyPictures();
    // --cut-pictures=cover: what the outputs that keep their own times are told once the start is known
    void applyCutOrigin();
    [[nodiscard]] int64_t cutKeyBefore(int64_t key) const;
    std::unique_ptr<AbstractMuxer> createMuxer();
    void asyncWriteBlock(const WriterData& data) const;
    void checkTrackList(const std::vector<StreamInfo>& ci) const;
    void checkMatroskaOnlyCodecs(const std::vector<StreamInfo>& ci, bool notMatroska) const;
    void checkOtherTracks(const std::vector<StreamInfo>& ci) const;

   public:
    // The codecs only a Matroska output carries, checked before anything is written for an output that is not one: a
    // demux made its output folder first, and a disc its folder tree, and a refused track left them there empty
    [[nodiscard]] bool keepEditions() const { return m_keepEditions; }
    // Also a subTrack= split of an HEVC track that has no enhancement layer to split off.
    void checkTracksBeforeOutput();
    void checkTimeshiftFitsClock();

   private:
    std::vector<double> m_chapters;        // chapter start times in seconds, applied once the muxer exists
    std::vector<AVChapter> m_chapterInfo;  // the same chapters with their names, when known
    std::unique_ptr<AbstractMuxer> m_mainMuxer;
    bool m_keepEditions = false;  // --keep-editions
    std::unique_ptr<AbstractMuxer> m_subMuxer;

    bool m_asyncMode;
    // int32_t m_fileBlockSize;
    std::string m_outFileName;
    std::condition_variable reinitCond;
    METADemuxer m_metaDemuxer;
    int64_t m_cutStart;
    int64_t m_cutEnd;
    // --cut-start: for each video stream already begun, the time of its first picture; the streams that only add to
    // another (a 3D second view, a Dolby Vision enhancement layer); and the time the main video begins at.
    std::map<int, int64_t> m_cutVideoFrom;
    std::set<int> m_cutLayers;
    bool m_cutLayersKnown = false;
    int64_t m_cutVideoStart = -1;
    bool m_cutCover = true;               // --cut-pictures
    int m_cutMainVideo = -1;              // the stream index of the main video, the one a disc playlist follows
    std::map<int, int64_t> m_cutKeyFrom;  // cover: per video stream, the time of its key picture at or before the cut
    bool m_cutStartCovered = false;  // cover: the main video begins at its key picture at or before --cut-start
    bool m_cutPassFailed = false;    // cover: findCutKeyPictures could not read the sources, and inside applies
    int64_t m_cutShownFrom = -1;     // cover: the first picture of the main video shown at or after --cut-start
    int64_t m_cutShownTo = -1;    // cover: the first picture of the main video shown at or after --cut-end
    std::string m_metaFileName;   // the meta file, read again by findCutKeyPictures
    const BufferedReaderManager& m_readManager;
    std::unique_ptr<BufferedFileWriter> m_fileWriter;
    AbstractMuxerFactory& m_factory;
    bool m_allowStereoMux;
    std::set<int> m_subStreamIndex;
    std::string m_muxOpts;
    bool m_interleave;

    std::vector<WriterData> m_delayedData;  // ssif interlieave
    bool m_subBlockFinished;
    bool m_mainBlockFinished;
    bool m_mvcBaseViewR;
    int64_t m_ptsOffset;
    int m_extraIsoBlocks;                // the room a split gets by default, before its parts are counted
    int m_extraIsoSpace = 0;             // --extra-iso-space, on top of the room getExtraISOBlocks works out
    bool m_extraIsoBlocksGiven = false;  // --extra-iso-space was given
    int64_t m_isoSplitBytes = 0;         // --split-size, in bytes
    int64_t m_isoSplitTicks = 0;         // --split-duration, in INTERNAL_PTS_FREQ ticks
    bool m_isoSplitChapters = false;     // --split-chapters
    std::optional<std::vector<AVChapter>> m_sourceChapters;
    bool m_bluRayMode;
    bool m_avchdMode = false;  // --avchd: m_bluRayMode is set too
    bool m_demuxMode;
    std::string m_demuxFolder;
    std::string m_finishingNote;  // setFinishingNote
    bool m_reproducibleIsoHeader = false;
    int64_t m_discSizeLimit = 0;         // --disc-size: target disc capacity in bytes (0 = guard off)
    bool m_allowOversize = false;        // --allow-oversize: warn instead of abort on an over-capacity image
    int64_t m_padRateGiven = -1;         // --bitrate, in bits a second (-1 = not given)
    int64_t m_padMaxRate = -1;           // --maxbitrate
    int64_t m_padMinRate = -1;           // --minbitrate
    bool m_noChapters = false;           // --no-chapters: the source's own chapters are not taken over
    bool m_noChaptersTyped = false;      // --no-chapters itself, not an empty --custom-chapters
    int m_layerBreakGuardMB = -1;        // --layer-break-guard: MB of zero-fill each side of the DL break (-1 = off)
    int m_layerBreakGuardBeforeMB = -1;  // --layer-break-guard-before: MB before the break on its own (-1 = derived)
    std::vector<int> m_layerBreakLbns;   // --layer-break-lbn: break sector(s); empty = BD-R/RE DL default
    bool m_innerOnly = false;            // --inner-only
    bool m_noLayerBreakGuard = false;    // --no-layer-break-guard

    /// Results of the discovery (probe) phase, indexed by stream index.
    std::vector<StreamDiscoveryData> m_discoveryData;
    bool m_discovered = false;       // m_discoveryData is filled (doMux, or early for a check)
    bool m_firstBlocksRead = false;  // every stream has read its first block (preinitMux, or early for a check)
};

#endif  // _MUXER_MANAGER_H_
