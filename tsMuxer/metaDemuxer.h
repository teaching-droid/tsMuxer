#ifndef META_DEMUXER_H_
#define META_DEMUXER_H_

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "abstractDemuxer.h"
#include "abstractStreamReader.h"
#include "assRender.h"
#include "avPacket.h"
#include "bufferedReaderManager.h"
#include "streamDiscoveryData.h"
#include "vodCoreException.h"

// META file demuxer

struct StreamInfo
{
    AbstractReader* m_dataReader;
    AbstractStreamReader* m_streamReader;
    StreamInfo(AbstractReader* dataReader, AbstractStreamReader* streamReader, const std::string& streamName,
               const std::string& fullStreamName, int pid, bool isSubStream = false)
        : m_data(nullptr)
    {
        m_streamName = streamName;
        m_fullStreamName = fullStreamName;
        m_dataReader = dataReader;
        m_readerID = dataReader->createReader(streamReader->getTmpBufferSize());
        if (!dataReader->openStream(m_readerID, m_streamName.c_str(), pid, &streamReader->getCodecInfo()))
            THROW(ERR_CANT_OPEN_STREAM, "Can't open stream: " << m_streamName)
        m_streamReader = streamReader;
        m_pid = pid;
        m_readCnt = 0;
        m_lastAVRez = AbstractStreamReader::NEED_MORE_DATA;
        m_lastDTS = 0;
        lastReadRez = 0;
        m_flushed = false;
        m_timeShift = 0;
        m_isEOF = false;
        m_notificated = true;
        m_asyncMode = true;
        m_blockSize = 0;
        m_isSubStream = isSubStream;
        m_mergeAc3ReaderId = -1;
        m_mergeAc3DataReader = nullptr;
        m_clipStartTaken = false;
    }

    int read();

    int m_mergeAc3ReaderId;
    AbstractReader* m_mergeAc3DataReader;

    int m_lastAVRez;
    int64_t m_readCnt;
    bool m_notificated;
    int m_readerID;
    uint32_t m_blockSize;
    int64_t m_lastDTS;
    uint8_t* m_data;
    std::string m_streamName;
    std::string m_fullStreamName;
    std::vector<std::string>
        m_sourceFiles;  // every file read for this track: the clips of a playlist, each joined file
    int lastReadRez;
    int m_pid;
    bool m_flushed;
    int64_t m_timeShift;
    uint64_t m_srcTrackUid = 0;  // the source's TrackUID, for the tags about this track (getTrackTags)
    std::string m_lang;
    std::string m_langBCP47;  // the source container's BCP 47 tag, when it had one
    std::string m_codec;
    std::map<std::string, std::string> m_addParams;
    bool m_isEOF;
    bool m_asyncMode;
    bool m_isSubStream;
    bool m_clipStartTaken;  // the start of the clip in which this stream begins has been added to m_timeShift
    // The gaps a join put in this stream's times (takeTimeGaps), in the internal clock, the packet after each not read
    // yet; and that packet, held back until the other streams have come up to its time (METADemuxer::readPacket)
    std::vector<int64_t> m_joinGaps;
    bool m_held = false;
    AVPacket m_heldPacket;
};

enum class DemuxerReadPolicy
{
    drpReadSequence,
    drpFragmented
};

class METADemuxer;

class ContainerToReaderWrapper final : public AbstractReader
{
   public:
    struct DemuxerData
    {
        std::map<int32_t, DemuxerReadPolicy> m_pids;
        PIDSet m_pidSet;  // same as pids
        AbstractDemuxer* m_demuxer;
        std::string m_streamName;
        DemuxedData demuxedData;
        FileNameIterator* m_iterator;
        std::map<uint32_t, uint32_t> lastReadCnt;
        std::map<uint32_t, uint32_t> lastReadRez;
        DemuxerData()
        {
            m_demuxer = nullptr;
            m_firstRead = true;
            m_iterator = nullptr;
            m_allFragmented = true;
        }
        bool m_firstRead;
        bool m_allFragmented;  // // container reader does not contain any sequence track reader(s)
    };

    struct ReaderInfo
    {
        ReaderInfo(DemuxerData& demuxerData, const int pid) : m_demuxerData(demuxerData), m_pid(pid) {}

        DemuxerData& m_demuxerData;
        int m_pid;
        int64_t m_readAheadTo = INT64_MIN;  // setReadAhead
        bool m_gotData = false;             // a block of this track has been handed on (readBlock)
    };

    ContainerToReaderWrapper(const METADemuxer& owner, const BufferedReaderManager& readManager)
        : m_readBuffOffset(0), m_readManager(readManager), m_owner(owner)
    {
        const auto& brm = const_cast<BufferedReaderManager&>(readManager);

        m_blockSize = brm.getBlockSize();
        m_allocSize = brm.getAllocSize();
        m_prereadThreshold = brm.getPreReadThreshold();

        m_readerCnt = 0;
        m_discardedSize = 0;
        m_terminated = false;
    }
    uint8_t* readBlock(int readerID, uint32_t& readCnt, int& rez, bool* firstBlockVar = nullptr) override;
    bool seek(int readerID, int64_t offset) override { return false; }
    bool incSeek(int readerID, int64_t offset) override { return false; }
    void notify(int readerID, uint32_t dataReaded) override {}
    int createReader(int readBuffOffset = 0) override;
    void deleteReader(int readerID) override;
    bool openStream(int readerID, const char* streamName, int pid = 0, const CodecInfo* codecInfo = nullptr) override;
    void setFileIterator(const char* streamName, FileNameIterator* itr);
    void resetDelayedMark() const;
    [[nodiscard]] int64_t getDiscardedSize() const { return m_discardedSize; }

    bool gotoByte(int readerID, int64_t seekDist) override { return false; }
    void terminate();
    std::map<std::string, DemuxerData> m_demuxers;
    // ASS and SSA tracks drawn with libass, by file and pid: handed to the Matroska reader when the track is opened
    std::map<std::pair<std::string, int>, ass_render::Drawing> m_assDrawing;

    /// Retrieve the track's codec-private blob via the underlying demuxer.
    /// @param readerID  The reader ID used when the stream was registered.
    /// @param[out] size  Receives the codec-private size.
    /// @returns  Pointer to the blob, or nullptr if unavailable.
    const uint8_t* getTrackCodecPrivate(uint32_t readerID, int& size);
    // Where the stream begins on the timeline of a join, in 90 kHz (see AbstractDemuxer::getTrackClipStart).
    int64_t getTrackClipStart(uint32_t readerID);
    // The gaps the demuxer found in the stream's times (see AbstractDemuxer::takeTimeGaps).
    std::vector<std::pair<int64_t, int64_t>> takeTimeGaps(uint32_t readerID);
    // The text subtitles the next one follows at once (see AbstractDemuxer::takeTouchingCues).
    std::vector<int64_t> takeTouchingCues(uint32_t readerID);
    // A track whose packets are sparse (subtitles) and due before the others of its file, by a negative timeshift:
    // while it has none, its file is read on up to this time (the internal clock of the demuxer's packets), so that
    // they are there when due.
    void setReadAhead(uint32_t readerID, int64_t until);

   private:
    int64_t m_discardedSize;

    int32_t m_readerCnt;
    size_t m_readBuffOffset;
    const BufferedReaderManager& m_readManager;
    std::map<uint32_t, ReaderInfo> m_readerInfo;
    const METADemuxer& m_owner;
    bool m_terminated;
};

typedef std::map<std::string, MPLSParser> MPLSCache;

struct DetectStreamRez
{
    DetectStreamRez() : fileDurationNano(0) {}

    AVChapters chapters;
    std::vector<CheckStreamRez> streams;
    int64_t fileDurationNano;
    // Where the video starts and ends on the file's own clock (AbstractDemuxer::getVideoSpanNano); the end is 0 when
    // the container could not say.
    int64_t videoStartNano = 0;
    int64_t videoEndNano = 0;
    // A Matroska file's WritingApp, empty for other files. The window takes it to tell a file tsMuxeR wrote.
    std::string writingApp;
};

// ISO 639-2 gives twenty languages two codes, a bibliographic one and a terminological one,
// German being "ger" and "deu". eac3to reports the first, tsMuxeR has reported the second, and a
// disc may carry either. Which one is REPORTED is the user's to choose; what gets written is not
// affected, because a lang= in a meta file reaches the disc exactly as typed.
//
// The table in metaDemuxer.cpp holds twenty-four rows, not twenty. Four of them are deprecated and
// are accepted on the way IN only, so that an older disc still reads: jaw, mol, scc and scr. Those
// four must never be produced, which is why each row carries a flag saying whether it is current.
enum class LangCodeStyle
{
    Source,          // whatever the source says, converted neither way
    Terminological,  // ell, deu, nld. The default, and what tsMuxeR has always done
    Bibliographic    // gre, ger, dut. What eac3to shows
};

void setLangCodeStyle(LangCodeStyle style);
LangCodeStyle getLangCodeStyle();

std::string toIso639_2T(const std::string& lang);
std::string toIso639_2B(const std::string& lang);
std::string applyLangCodeStyle(const std::string& lang);
// Three letters, as a language code of a disc is: a playlist whose field holds spaces or other bytes names no language
bool isLanguageCode(const std::string& lang);
// The ISO 639-2 code (terminological form) of a BCP 47 language tag, from its primary subtag; empty when it has none.
std::string iso639FromBcp47(const std::string& tag);

// ** THE DEFAULT OF --playlist-clips, IN THIS ONE PLACE. ** true: a Blu-ray playlist that plays only a part of a clip
// gets only that part (--playlist-clips=played). false: every clip of a playlist is muxed whole, from its first picture
// to its last, whatever part of it the playlist plays (--playlist-clips=whole).
constexpr bool PLAYLIST_CLIPS_PLAYED_BY_DEFAULT = true;

// The kind of track a container declares: a transport stream by its stream type, a Matroska file by the first letter
// of the codec ID (A_ sound, V_ video). Detection asks the probes of that kind first (METADemuxer::detectTrackReader).
enum class DeclaredKind
{
    None,  // nothing declared, or a type that names no codec: every probe in the order they always ran
    Sound,
    Video
};

class METADemuxer final : public AbstractDemuxer
{
   public:
    METADemuxer(const BufferedReaderManager& readManager);
    ~METADemuxer() override;
    int readPacket(AVPacket& avPacket);
    void readClose() override;

    // Chapters of the first source container that has any. DetectStreamRez::chapters holds the
    // same information but only on the listing path, so the mux path had no way to reach it.
    [[nodiscard]] std::vector<AVChapter> getChapters() override;
    [[nodiscard]] std::string getTitle() override;
    [[nodiscard]] std::vector<std::vector<uint8_t>> getPassThroughAttachments() override;
    // --keep-editions: a Matroska source is read as it is stored, not as its ordered edition plays it, and its
    // chapters and segment identity go to the output unchanged.
    void setKeepEditions(const bool value) { m_keepEditions = value; }
    // --playlist-clips: true when a clip of a playlist is cut to the part its play item plays (cutPlayItems)
    void setPlaylistClipsPlayed(const bool value) { m_playlistClipsPlayed = value; }
    // The output is Matroska (1) or WebM (2), known before the meta is read: a subtitle of a Matroska source is then
    // copied in its own form (MatroskaCopyStreamReader) instead of being converted.
    void setMatroskaOutput(const int kind) { m_matroskaOutput = kind; }
    // The output is a demux: ASS, SSA and WebVTT subtitles kept as text are written in their own form
    void setDemuxOutput(const bool demux) { m_demuxOutput = demux; }
    // A second pass over the sources of a mux, before it (MuxerManager::findCutKeyPictures): read as the mux reads
    // them, with the other's settings, but its readers give no advice, open no file to write (save-srt) and print no
    // progress
    void setQuiet(const bool value) { m_quiet = value; }
    void takeSettingsOf(const METADemuxer& other)
    {
        m_keepEditions = other.m_keepEditions;
        m_playlistClipsPlayed = other.m_playlistClipsPlayed;
        m_matroskaOutput = other.m_matroskaOutput;
        m_demuxOutput = other.m_demuxOutput;
    }
    [[nodiscard]] SourceEditions getSourceEditions() override;
    // The Chapters element of the one Matroska file the output takes its chapters from, as it stands; empty when
    // they come from anything else: a playlist, a join, an ordered edition's timeline, another container.
    [[nodiscard]] std::vector<uint8_t> getPlainMatroskaChapters();
    [[nodiscard]] std::vector<std::vector<uint8_t>> getGlobalTags() override;
    // The tags each source had about one of its tracks, by the stream index of that track here
    [[nodiscard]] std::map<int, std::vector<SourceTrackTag>> getTrackTagsByStream();
    // The BlockAdditions of the source tracks that carry them, by stream index (see AbstractDemuxer)
    [[nodiscard]] std::map<int, std::pair<uint64_t, std::vector<std::vector<uint8_t>>>> getBlockAdditionInfoByStream();
    bool takeBlockAdditions(int streamIndex, int64_t pts, std::vector<uint8_t>& out) override;
    // How long the sources say they run, without reading them: a playlist by its play items, a container by the
    // length it states. A join counts its first file only and an elementary stream states nothing, so this can fall
    // short or be 0. It is used to reckon how many parts a --split-duration makes, and in the checks of the clock's
    // range and of the chapter count. With sumJoins, for the size of a constant bitrate disc, which may not fall short
    // for a join, a join of files counts as the sum of their lengths, each file opened again for it.
    [[nodiscard]] int64_t statedDurationNano(bool sumJoins = false);
    int64_t getDemuxedSize() override;
    int addStream(const std::string& codec, const std::string& codecStreamName,
                  const std::map<std::string, std::string>& addParams);
    void openFile(const std::string& streamName) override;
    [[nodiscard]] const std::vector<StreamInfo>& getStreamInfo() const { return m_codecInfo; }
    // laterFiles: the clips that follow fileName in a playlist, read for a frame rate when fileName is too short
    static DetectStreamRez DetectStreamReader(const BufferedReaderManager& readManager, const std::string& fileName,
                                              bool calcDuration, const std::vector<std::string>& laterFiles = {});
    std::vector<StreamInfo>& getCodecInfo() { return m_codecInfo; }

    /// Run a self-contained discovery pass: for every track in m_codecInfo,
    /// open a temporary demuxer, probe the first few frames, and return a
    /// vector of StreamDiscoveryData (indexed the same as m_codecInfo).
    /// All temporary I/O is closed before the function returns.
    std::vector<StreamDiscoveryData> discoverStreams() const;

    int getLastReadRez() override { return m_lastReadRez; }
    [[nodiscard]] int64_t totalSize() const { return m_totalSize; }
    // The STREAM folder of the disc a playlist belongs to, with a separator at its end; empty when there is none.
    static std::string playlistStreamDir(const std::string& mplsFileName);
    static std::string mplsTrackToFullName(const std::string& mplsFileName, const std::string& mplsNum);
    static std::string mplsTrackToSSIFName(const std::string& mplsFileName, const std::string& mplsNum);
    // --playlist-clips=played (played true): the play items of a playlist that plays only parts of its clips are cut
    // to those parts; the listing does it as the mux does by default, so that the lengths and marks it gives are the
    // ones of the output
    static void cutPlayItems(MPLSParser& mpls, const std::string& mplsFileName, bool played);
    bool m_HevcFound;
    bool m_MvcFound;  // a 3D (MVC) track is in the mux

   private:
    std::vector<FileListIterator*> m_iterators;
    int m_lastReadRez;
    ContainerToReaderWrapper m_containerReader;
    // Subtitle tracks with a negative timeshift, read by a demuxer of their own (see addStream)
    ContainerToReaderWrapper m_shiftedSubReader;
    int m_lastProgressY;
    std::chrono::steady_clock::time_point m_lastReportTime;
    std::set<std::string> m_timelinesReported;  // the Matroska files whose play timeline was said in the log
    bool m_keepEditions = false;                // --keep-editions (setKeepEditions)
    bool m_playlistClipsPlayed = PLAYLIST_CLIPS_PLAYED_BY_DEFAULT;  // --playlist-clips (setPlaylistClipsPlayed)
    int m_matroskaOutput = 0;                                       // setMatroskaOutput: 0 other, 1 Matroska, 2 WebM
    bool m_demuxOutput = false;                                     // setDemuxOutput
    bool m_quiet = false;                                           // setQuiet
    std::set<std::string> m_pipPlacingReported;  // the playlists whose own placing of a picture-in-picture was said
    double m_lastProgress = 0.0;                                    // the last value printed
    int64_t m_totalSize;
    bool m_flushDataMode;
    int64_t m_muxPos = 0;  // the time of the packet given out last, for setReadAhead
    const BufferedReaderManager& m_readManager;
    std::string m_streamName;
    std::vector<StreamInfo> m_codecInfo;

    // MPLSPlayItemsMap m_mplsPlayItemsMap;
    // MPLSPlayItemsMap m_mplsStreamMap;
    MPLSCache m_mplsStreamMap;
    std::set<std::string> m_processedTracks;
    std::set<std::string> m_processedLines;  // the files and track of every line read, to tell a repeated line

    friend class ContainerToReaderWrapper;

    void placeJoinedSubtitles();
    // How long an elementary video stream lasts as its reader times it, in ns; -1 where it cannot be read
    static int64_t esVideoLengthNs(const std::string& codec, const std::map<std::string, std::string>& addParams,
                                   const std::string& fileName);

    // quiet suppresses advice meant for the user. discoverStreams builds a reader purely to look
    // at a file and throws it away, so anything said there is said a second time by the real pass.
    static AbstractStreamReader* createCodec(const std::string& codecName,
                                             const std::map<std::string, std::string>& addParams,
                                             const std::string& codecStreamName,
                                             const std::vector<MPLSPlayItem>& mplsInfo, bool quiet = false);
    inline void updateReport(bool checkTime);
    void lineBack();
    static CheckStreamRez detectTrackReader(uint8_t* tmpBuffer, int len,
                                            AbstractStreamReader::ContainerType containerType, int containerDataType,
                                            int containerStreamIndex, DeclaredKind declared = DeclaredKind::None);
    static std::string findBluRayFile(const std::string& streamDir, const std::string& requestDir,
                                      const std::string& requestFile);
    std::vector<MPLSParser> getMplsInfo(const std::string& mplsFileName);

   public:
    // A playlist read as a source says its 3D base view is the right eye, or a Matroska source's 3D track does
    // (StereoMode 14, srcrighteye)
    [[nodiscard]] bool mplsBaseViewRight() const
    {
        for (const auto& [name, parser] : m_mplsStreamMap)
            if (parser.mvc_base_view_r)
                return true;
        for (const auto& si : m_codecInfo)
            if (const auto it = si.m_addParams.find("srcrighteye"); it != si.m_addParams.end() && it->second == "1")
                return true;
        return false;
    }

   private:
    int addPGSubStream(const std::string& codec, const std::string& _codecStreamName,
                       const std::map<std::string, std::string>& addParams, const MPLSStreamInfo* subStream);
    static void addTrack(std::vector<CheckStreamRez>& rez, CheckStreamRez trackRez);
    static std::vector<MPLSPlayItem> mergePlayItems(const std::vector<MPLSParser>& mplsInfoList);
};

#endif
