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

    // Dual-layer guard band: MB of zero-fill on each side of the BD-R DL layer break (-1 = off),
    // plus an optional break-sector override for testing / non-standard geometries.
    [[nodiscard]] int getLayerBreakGuardMB() const { return m_layerBreakGuardMB; }
    [[nodiscard]] int getLayerBreakGuardBeforeMB() const { return m_layerBreakGuardBeforeMB; }
    [[nodiscard]] const std::vector<int>& getLayerBreakLbns() const { return m_layerBreakLbns; }

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
    std::unique_ptr<AbstractMuxer> createMuxer();
    void asyncWriteBlock(const WriterData& data) const;
    void checkTrackList(const std::vector<StreamInfo>& ci) const;
    void checkMatroskaOnlyCodecs(const std::vector<StreamInfo>& ci, bool notMatroska) const;
    void checkOtherTracks(const std::vector<StreamInfo>& ci) const;

   public:
    // The codecs only a Matroska output carries, checked before anything is written for an output that is not one: a
    // demux made its output folder first, and a disc its folder tree, and a refused track left them there empty
    [[nodiscard]] bool keepEditions() const { return m_keepEditions; }
    void checkTracksBeforeOutput() const { checkMatroskaOnlyCodecs(m_metaDemuxer.getStreamInfo(), true); }

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
    int m_extraIsoBlocks;
    bool m_extraIsoBlocksGiven = false;  // --extra-iso-space was given, and is taken as it is
    int64_t m_isoSplitBytes = 0;         // --split-size, in bytes
    int64_t m_isoSplitSeconds = 0;       // --split-duration, in seconds
    bool m_isoSplitChapters = false;     // --split-chapters
    std::optional<std::vector<AVChapter>> m_sourceChapters;
    bool m_bluRayMode;
    bool m_demuxMode;
    bool m_reproducibleIsoHeader = false;
    int64_t m_discSizeLimit = 0;         // --disc-size: target disc capacity in bytes (0 = guard off)
    bool m_allowOversize = false;        // --allow-oversize: warn instead of abort on an over-capacity image
    bool m_noChapters = false;           // --no-chapters: the source's own chapters are not taken over
    int m_layerBreakGuardMB = -1;        // --layer-break-guard: MB of zero-fill each side of the DL break (-1 = off)
    int m_layerBreakGuardBeforeMB = -1;  // --layer-break-guard-before: MB before the break on its own (-1 = derived)
    std::vector<int> m_layerBreakLbns;   // --layer-break-lbn: break sector(s); empty = BD-R/RE DL default

    /// Results of the discovery (probe) phase, indexed by stream index.
    std::vector<StreamDiscoveryData> m_discoveryData;
};

#endif  // _MUXER_MANAGER_H_
