#ifndef SRT_STREAM_READER_
#define SRT_STREAM_READER_

#include <deque>
#include <memory>
#include <queue>
#include <set>
#include <string>

#include "abstractStreamReader.h"
#include "avCodecs.h"
#include "avPacket.h"
#include "fs/file.h"
#include "pgsStreamReader.h"
#include "textSubtitles.h"
#include "utf8Converter.h"

class SRTStreamReader final : public AbstractStreamReader
{
   public:
    SRTStreamReader();
    ~SRTStreamReader() override;
    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    int64_t getProcessedSize() override { return m_processedSize; }
    // A join of subtitle files ("a.srt"+"b.srt"): where each file starts in the output, in ns, the first file at 0
    // (METADemuxer gives it from the video joined from as many files). Each file's cues are moved by it.
    void setJoinStarts(std::vector<int64_t> ns) { m_joinStartsNs = std::move(ns); }
    void onJoinedFileStart() override { m_joinPending = true; }
    // A subtitle from a container that the next one follows at once (its start, in ms as its SRT text gives it): it is
    // not cleared, as the next one takes its place at that moment (see AbstractDemuxer::takeTouchingCues)
    void addTouchingCue(const int64_t ms) { m_touchingCues.insert(ms); }
    CheckStreamRez checkStream(uint8_t* buffer, int len, ContainerType containerType, int containerDataType,
                               int containerStreamIndex);
    // Kept as text the track is SRT, in Matroska S_TEXT/UTF8; otherwise it is drawn into PGS
    const CodecInfo& getCodecInfo() override { return m_keepText ? srtCodecInfo : pgsCodecInfo; }
    // keep-text: hand the subtitles over as text, each with its start and duration, instead of drawing
    // them into pictures. Only a muxer that can carry text leaves it on; the others turn it off. In a
    // demux each subtitle is handed over as an SRT record, so the file written is an SRT file.
    // Kept as text, the subtitles are not drawn and need no font: the font is loaded only when they are drawn. Loaded
    // at once, a line with keep-text and without font-name= was refused on Linux and macOS ("No font is named"), and
    // one naming a font not installed there as well.
    void setKeepText(const bool value)
    {
        m_keepText = value;
        if (!value && m_fontSet)
            m_srtRender->m_textRender->setFont(m_font);
    }
    // save-srt: while the subtitles are drawn into pictures, each one is also written to an SRT file there (#643)
    void setSaveSrt(const std::string& fileName);
    // The timeshift of the track, by which its pictures are moved: the .srt is written at their times
    void setSaveSrtShiftMs(const int64_t ms) { m_saveSrtShiftMs = ms; }
    [[nodiscard]] bool keepText() const { return m_keepText; }
    [[nodiscard]] bool hasVideoInfo() const { return m_hasVideoInfo; }
    void setStreamIndex(const int index) override
    {
        m_streamIndex = index;
        m_dstSubCodec->setStreamIndex(index);
    }
    void setDemuxMode(const bool value) override
    {
        m_demuxMode = value;
        const auto pgsReader = dynamic_cast<PGSStreamReader*>(m_dstSubCodec);
        if (pgsReader)
            pgsReader->setDemuxMode(value);
    }
    void setVideoInfo(const uint16_t width, const uint16_t height, const double fps) const
    {
        m_srtRender->setVideoInfo(width, height, fps);
        m_hasVideoInfo = width && height && fps > 0;
        const auto pgsReader = dynamic_cast<PGSStreamReader*>(m_dstSubCodec);
        if (pgsReader)
            pgsReader->setVideoInfo(0, 0, fps);
    }
    void setFont(const text_subtitles::Font& font)
    {
        m_font = font;
        m_fontSet = true;
        if (!m_keepText)
            m_srtRender->m_textRender->setFont(font);
    }
    void setAnimation(const text_subtitles::TextAnimation& animation);
    void setBottomOffset(const int offset) const { m_srtRender->setBottomOffset(offset); }

   protected:
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override
    {
        // The picture form needs a header in front of each set; text carries everything it needs
        if (m_keepText)
            return 0;
        return m_dstSubCodec->writeAdditionData(dstBuffer, dstEnd, avPacket, priorityData);
    }

   private:
    UtfConverter::SourceFormat m_srcFormat;
    // Judged UTF-8 only because the start of the file is plain ASCII, which every 8 bit code page shares with UTF-8.
    bool m_utf8Guessed = false;
    double m_inTime;
    double m_outTime;
    bool m_untilNext = false;  // the timing line has no end: shown until the next subtitle
    bool m_keepText = false;
    text_subtitles::Font m_font;  // setFont's, loaded when the subtitles are drawn
    bool m_fontSet = false;
    // save-srt: the SRT file, the subtitle waiting for the next one when it has no end, and the count written
    std::unique_ptr<File> m_saveSrt;
    std::string m_saveSrtName;
    std::deque<std::pair<std::string, std::pair<int64_t, int64_t>>> m_saveSrtPending;
    int m_saveSrtCount = 0;
    int64_t m_saveSrtShiftMs = 0;
    void saveSrtCue(const std::string& text, double inTime, double outTime, bool untilNext);
    void saveSrtWrite(const std::string& text, int64_t inMs, int64_t outMs);
    mutable bool m_hasVideoInfo = false;
    struct TextCue
    {
        std::string text;
        double inTime;
        double outTime;
        bool untilNext;
    };
    std::deque<TextCue> m_textCues;
    std::vector<uint8_t> m_textPacket;
    int m_lengthlessCues = 0;   // kept as text: subtitles left out because they have no length
    int m_invalidUtf8Cues = 0;  // kept as text: subtitles whose bytes were not all valid UTF-8
    int m_unseenCues = 0;       // drawn: subtitles with text, none of which lands inside the picture
    int m_shrunkCues = 0;       // drawn: subtitles too large for the picture, drawn at the largest size that fits
    bool m_unseenReported = false;
    // a subtitle that ends where or before it starts was said
    bool m_noLengthReported = false;
    std::set<int64_t> m_touchingCues;  // addTouchingCue
    int m_srtRecords = 0;              // demux: SRT records written so far, which numbers the next
    int64_t m_cuesRead = 0;            // subtitles read from the file
    int64_t m_skippedLines = 0;        // lines of text that are not part of any subtitle, passed over
    int64_t m_firstSkippedLine = 0;
    std::string m_firstSkippedText;
    int64_t m_counterLine = 0;  // the line of the counter of the subtitle being read
    std::string m_counterText;
    int m_doubleCounters = 0;  // counters written twice, named in a warning
    void skipNonCueLine();
    void reportSkippedLines();
    bool m_openEndWarned = false;
    int readTextPacket(AVPacket& avPacket);
    void startTextPacket(AVPacket& avPacket);
    bool prepareTextCue(TextCue& cue, int64_t nextStartMs, int64_t& inMs, int64_t& outMs);
    void appendSrtRecord(const std::string& text, int64_t inMs, int64_t outMs);
    int m_processedSize;
    int32_t m_charSize;
    AbstractStreamReader* m_dstSubCodec;
    text_subtitles::TextToPGSConverter* m_srtRender;
    bool m_lastBlock;
    int parseText(uint8_t* dataStart, size_t len);
    std::vector<uint8_t> m_tmpBuffer;
    std::deque<std::string> m_sourceText;
    std::queue<int32_t> m_origSize;
    // Lines handed to the queue since the file was opened. The line at the front of the queue is
    // therefore m_linesRead - m_sourceText.size() + 1, which is what a parse error should name.
    int64_t m_linesRead;
    std::vector<int64_t> m_joinStartsNs;                 // setJoinStarts
    bool m_joinPending = false;                          // onJoinedFileStart: the next block begins a later file
    size_t m_joinFile = 0;                               // the file of the join being read
    std::deque<std::pair<int64_t, double>> m_joinLines;  // (first line of a later file, its start in s), to come
    double m_joinShift = 0.0;                            // the start of the file the cue being read comes from, in s
    void finishLastLine();
    std::string m_renderedText;
    long m_splitterOfs;
    uint16_t m_short_R;
    uint16_t m_short_N;
    uint32_t m_long_R;
    uint32_t m_long_N;
    text_subtitles::TextAnimation m_animation;

    // the scale the last subtitle too large for the picture was drawn at
    double m_lastFitScale = 0.0;

    enum class ParseState
    {
        PARSE_FIRST_LINE,
        PARSE_TIME,
        PARSE_TEXT
    };
    ParseState m_state;
    uint8_t* renderNextMessage(uint32_t& renderedLen);
    bool readNextCue();
    void addCue(const std::string& text, double inTime, double outTime);
    void flushCueGroup();
    uint8_t* drawNext(uint32_t& renderedLen);

    // Subtitles read from a file that overlap in time are drawn together. A cue waits here until
    // one starts at or after the end of all of them (cues are expected in start order), or the
    // file ends; the group is then cut at every start and end into pieces of screen time, each
    // drawn with every subtitle visible during it. Times are on the frame grid here, as drawn.
    struct Cue
    {
        std::string text;
        double inTime;  // as read, for a cue that is drawn on its own
        double outTime;
        double inFrame;  // on the frame grid
        double outFrame;
    };
    struct Piece
    {
        std::string text;
        double inTime;
        double outTime;
        bool hide;
    };
    std::vector<Cue> m_cueGroup;
    double m_cueGroupEnd = 0;
    std::deque<Piece> m_toDraw;
    bool m_screenShown = false;  // a piece without its clear is on screen
    bool parseTime(const std::string& text);
    static std::string detectUTF8Lang(uint8_t* buffer, int len);
    bool detectSrcFormat(const uint8_t* dataStart, size_t len, int& prefixLen);
    static bool strOnlySpace(const std::string& str);
    static bool hasVisibleText(const std::string& text);
    int blankLineEndsCue() const;
    int nextCueFollows() const;
    void checkUtf8Guess(const uint8_t* line, ptrdiff_t len);
};

#endif
