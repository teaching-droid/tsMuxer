// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef PROGRAM_STREAM_DEMUXER_H_
#define PROGRAM_STREAM_DEMUXER_H_

#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <vector>

#include "abstractDemuxer.h"
#include "bitmapSubtitles.h"
#include "bufferedReader.h"
#include "bufferedReaderManager.h"

class ProgramStreamDemuxer final : public AbstractDemuxer
{
   public:
    // From now on, hand the sub-picture units of stream (0x20 to 0x3F) on as they are (m_rawSubPictures)
    void setRawSubPictures(const int stream) { m_rawSubPictures.insert(stream); }
    static constexpr int MAX_PES_HEADER_SIZE = 1018;  // buffer for PES header and program stream map

    ProgramStreamDemuxer(const BufferedReaderManager& readManager);
    void openFile(const std::string& streamName) override;
    static int readPacket(AVPacket& avPacket) { return 0; }
    ~ProgramStreamDemuxer() override;
    int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo>& trackList) override;
    void readClose() override;
    int64_t getDemuxedSize() override;
    int getLastReadRez() override { return m_lastReadRez; }
    void setFileIterator(FileNameIterator* itr) override;

    int64_t getTrackDelay(const int32_t pid) override
    {
        if (m_firstPtsTime.find(pid) != m_firstPtsTime.end())
            return llround(
                static_cast<double>(m_firstPtsTime[pid] - (m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS)) /
                90.0);  // convert to ms

        return 0;
    }

    [[nodiscard]] int64_t getFileDurationNano() const override;

   private:
    uint32_t m_tmpBufferLen;
    uint8_t m_tmpBuffer[MAX_PES_HEADER_SIZE];  // TS_FRAME_SIZE
    uint32_t m_lastPesLen;
    int32_t m_lastPID;
    const BufferedReaderManager& m_readManager;
    int64_t m_dataProcessed;
    std::string m_streamName;
    int m_readerID;
    int m_lastReadRez;
    AbstractReader* m_bufferedReader;
    uint8_t m_psm_es_type[256];

    std::map<int, int64_t> m_firstPtsTime;
    int64_t m_firstVideoPTS;
    int64_t m_firstPTS;
    MemoryBlock m_lpcmWaveHeader[16];
    bool m_lpcpHeaderAdded[16];

    // ** DVD SUBTITLES. ** The sub-pictures of private stream 1, sub-streams 0x20 to 0x3F, were not read. Each
    // sub-picture unit is put together from its PES packets (its first two bytes give its size), drawn with the
    // colours of the IFO file (dvdSubtitleIdx) and handed on as PGS display sets with their own times, as a .sup file
    // holds them, for the PGS reader.
    struct SubPicture
    {
        std::unique_ptr<VobSubDecoder> decoder;
        std::vector<uint8_t> unit;  // the unit being put together
        int64_t pts = -1;           // its time, 90 kHz
        std::vector<int64_t> times;  // of an .idx file, ms
        bool timesRead = false;
        size_t done = 0;  // the units read
        bool timesRanOut = false;
    };
    std::map<int, SubPicture> m_subPictures;
    bool m_subFlushed = false;
    // The sub-picture streams handed on as they are, for a Matroska output (S_VOBSUB): each unit a record as a copied
    // Matroska block is (ParsedCopyTrackData, MatroskaCopyStreamReader), not drawn as PGS
    std::set<int> m_rawSubPictures;
    bool m_vobSubPair = false;  // an .idx/.sub pair: the times of its units are those of the .idx file
    // The time line of the video the units are placed on: the joined parts of a title, or the parts of a VOB between
    // two jumps of the clock, follow on one after the other, as the video's pictures do. A part begins with the first
    // picture shown, which is not always the first one coded, and is as long as its pictures (scanVideo).
    int64_t m_segFirst = -1;     // when the first picture of the part is shown, 90 kHz
    double m_segBase = 0;        // where the part begins on the time line
    int64_t m_segFields = 0;     // the fields its pictures show
    int64_t m_lastDts = -1;      // the decoding time of the last picture: within a part it only goes forward
    double m_frameTicks = 3003;  // from the frame rate of the sequence header
    int m_videoHeight = 0;
    bool m_progressiveSequence = false;
    int64_t m_pesPts = -1;  // the time of the PES being scanned, for the first picture that starts in it
    // the start codes of the video, across the packets: the last bytes, and the start code whose bytes are collected
    uint32_t m_scanShift = 0xffffffff;
    uint8_t m_scanCode = 0;
    uint8_t m_scanBytes[8]{};
    int m_scanHave = 0;
    int m_scanWant = 0;
    bool m_lastPesVideo = false;  // the PES cut by the end of a read is video
    void noteVideoTime(int64_t pts, int64_t dts);
    void scanVideo(const uint8_t* data, int len);
    void videoStartCode(uint8_t code, const uint8_t* b);
    void addSubPicture(int stream, const uint8_t* data, int len, int64_t pts, StreamData& out);
    void flushSubPictures(DemuxedData& demuxedData, const PIDSet& acceptedPIDs);

    [[nodiscard]] bool isVideoPID(uint32_t pid) const;
    int mpegps_psm_parse(const uint8_t* buff, const uint8_t* end);
    int processPES(uint8_t* buff, uint8_t* end, int headerLen, bool mpeg1, int& afterPesHeader);
};

// The length of a program stream file in 90 kHz ticks
int64_t getPSDuration(const char* fileName);

#endif
