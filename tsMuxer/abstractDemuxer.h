#ifndef ABSTRACT_DEMUXER_H_
#define ABSTRACT_DEMUXER_H_

#include <assert.h>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include <types/types.h>

#include "avPacket.h"
#include "vod_common.h"

class SubTrackFilter;

class MemoryBlock
{
   public:
    MemoryBlock(const MemoryBlock& other)
    {
        assert(other.m_size == 0);
        m_size = 0;
    }

    MemoryBlock() : m_size(0) {}
    void reserve(const unsigned num) { m_data.resize(num); }

    void resize(const unsigned num)
    {
        m_size = num;
        if (m_data.size() < m_size)
            m_data.resize(m_size);
    }

    void grow(const size_t num)
    {
        m_size += num;
        if (m_data.size() < m_size)
        {
            m_data.resize(FFMIN(m_size * 2, m_size + 1024LL * 1024));
        }
    }

    void append(const uint8_t* data, const size_t num)
    {
        if (num > 0)
        {
            grow(num);
            memcpy(&m_data[m_size - num], data, num);
        }
    }

    [[nodiscard]] size_t size() const { return m_size; }

    [[nodiscard]] uint8_t* data() { return m_data.empty() ? nullptr : m_data.data(); }

    [[nodiscard]] const uint8_t* data() const { return m_data.empty() ? nullptr : m_data.data(); }

    [[nodiscard]] bool isEmpty() const { return m_size == 0; }

    void clear() { m_size = 0; }

   private:
    std::vector<uint8_t> m_data;
    size_t m_size;
};

typedef MemoryBlock StreamData;
typedef std::map<int32_t, StreamData> DemuxedData;
typedef std::set<int32_t> PIDSet;
// typedef std::map<uint32_t, std::vector<uint8_t> > DemuxedData;

// Used to automatically switch to reading the next file while the current one ends.
// This class implements file-IO, which determines the name of the next file.
class FileNameIterator
{
   public:
    virtual std::string getNextName() = 0;
    // The bytes of the current file to read, when only a part of it is wanted (end -1: to the end of the file)
    virtual bool getByteRange(int64_t& first, int64_t& end) const { return false; }
    virtual ~FileNameIterator() = default;
};

struct TrackInfo
{
    int m_trackType;     // 0 - not speciffed. Autodetect is required
    std::string m_lang;  // tracl language code
    std::string m_langBCP47;
    // More the container said about the track, under the keys the muxers read: srcname, srcflags, srccolour,
    // srcdisplay. Filled for Matroska only.
    std::map<std::string, std::string>
        m_srcParams;  // the BCP 47 tag the container gave, empty when it gave none (Matroska only)
    int64_t m_delay;  // auto delay for audio
    // For stream type 0x06, which says nothing on its own: 0x59 is DVB bitmap subtitles and
    // 0x56 is EBU teletext. Zero when the container named neither.
    int m_dvbDescriptor;
    // Whether the container marked this track as its default one: -1 when it did not say, which is
    // every container that has no such flag. Only Matroska has one.
    int m_defaultFlag;
    TrackInfo() : m_trackType(0), m_delay(0), m_dvbDescriptor(0), m_defaultFlag(-1) {}
    TrackInfo(const int trackType, const char* lang, const int64_t delay, const int dvbDescriptor = 0,
              const int defaultFlag = -1)
        : m_trackType(trackType),
          m_lang(lang),
          m_delay(delay),
          m_dvbDescriptor(dvbDescriptor),
          m_defaultFlag(defaultFlag)
    {
    }
};

class AbstractDemuxer
{
   public:
    typedef std::map<int, SubTrackFilter*> PIDFilters;

    AbstractDemuxer()
    {
        m_fileBlockSize = DEFAULT_FILE_BLOCK_SIZE;
        m_timeOffset = 0;
    }
    virtual ~AbstractDemuxer();
    virtual void openFile(const std::string& streamName) = 0;

    virtual void readClose() = 0;
    virtual int64_t getDemuxedSize() = 0;

    [[nodiscard]] virtual uint64_t getDuration() { return 0; }
    virtual void setTimeOffset(const int64_t offset) { m_timeOffset = offset; }
    virtual int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
    {
        discardSize = 0;
        return 0;
    }
    virtual void terminate() {}
    [[nodiscard]] virtual int getLastReadRez() = 0;
    virtual void getTrackList(std::map<int32_t, TrackInfo>& trackList) {}
    virtual void setFileIterator(FileNameIterator*) {}

    [[nodiscard]] virtual uint32_t getFileBlockSize() { return m_fileBlockSize; }

    [[nodiscard]] virtual int64_t getTrackDelay(int32_t pid) { return 0; }
    // Where a stream starts on the timeline of a join of several files, in 90 kHz, for a stream that the first
    // file does not carry. Only the TS demuxer can tell; every other demuxer says 0.
    [[nodiscard]] virtual int64_t getTrackClipStart(int32_t pid) { return 0; }
    [[nodiscard]] virtual std::vector<AVChapter> getChapters() { return {}; }
    [[nodiscard]] virtual double getTrackFps(uint32_t trackId) { return 0.0; }
    // The rate the times of a video track's packets give, for a stream that names no rate of its own: asked only then,
    // in place of the default of 25. A transport stream answers from its PES times; the other demuxers say 0.
    // fieldCoded: the stream may code its pictures as fields, whose times may be those of its fields. Then only a
    // rate that is sure to be one of frames is given.
    [[nodiscard]] virtual double getTrackPacketFps(uint32_t trackId, bool fieldCoded) { return 0.0; }
    // Whether the picture of a video track with this number (counted from 0, in the order of decoding) is the first
    // picture of a part cut from a clip of a playlist, after which the pictures before it are not there (TSDemuxer)
    [[nodiscard]] virtual bool isPartStartPicture(int32_t pid, int64_t picture) { return false; }

    /// Return a pointer to the track's codec-private (extradata) blob and its
    /// size.  The default implementation returns nullptr/0 for demuxers that do
    /// not expose this information.
    virtual const uint8_t* getTrackCodecPrivate(int32_t pid, int& size)
    {
        size = 0;
        return nullptr;
    }

    /// Fetch a file attached to the container by name.  Only containers that
    /// carry attachments implement this; the default is that there are none.
    virtual bool getAttachment(const std::string& name, std::vector<uint8_t>& out) { return false; }

    /// What a Matroska output can carry over from a Matroska source: its title, its attachments as whole
    /// AttachedFile elements, and its tags that are about the whole file, as whole Tag elements. Empty for every
    /// other container.
    [[nodiscard]] virtual std::string getTitle() { return {}; }
    [[nodiscard]] virtual std::vector<std::vector<uint8_t>> getPassThroughAttachments() { return {}; }
    // A Matroska source's Chapters element as it stands (its body) and its SegmentUID, PrevUUID and NextUUID (16
    // bytes each, empty where it has none), for --keep-editions.
    struct SourceEditions
    {
        std::vector<uint8_t> chapters;
        std::string segmentUid, prevUid, nextUid;
    };
    [[nodiscard]] virtual SourceEditions getSourceEditions() { return {}; }
    [[nodiscard]] virtual std::vector<std::vector<uint8_t>> getGlobalTags() { return {}; }
    // A Matroska source's tags about one track, by the source's TrackUID: the Targets' other children and the
    // SimpleTag elements, as they stand. The statistics tags are left out: they count the source's bytes and frames.
    struct SourceTrackTag
    {
        std::vector<uint8_t> targetsRest;
        std::vector<uint8_t> simpleTags;
    };
    [[nodiscard]] virtual std::map<uint64_t, std::vector<SourceTrackTag>> getTrackTags() { return {}; }
    // The TrackUID of a track, 0 when the container has none
    [[nodiscard]] virtual uint64_t getTrackUid(int pid) const { return 0; }
    // Data a container stores beside the frames of a track (Matroska BlockAdditions, such as HDR10+ metadata): the
    // track's MaxBlockAdditionID and BlockAdditionMapping bodies, and the additions of the block at a time (the
    // internal clock, as the demuxer gave it), taken out once asked for. False where there are none.
    virtual bool getBlockAdditionInfo(int pid, uint64_t& maxId, std::vector<std::vector<uint8_t>>& mappings) const
    {
        return false;
    }
    virtual bool takeBlockAdditions(int pid, int64_t pts, std::vector<uint8_t>& out) { return false; }
    // Gaps in a track's times the demuxer has found, taken out once asked for: (where, how long), in ns on the clock
    // of the track's reader (AbstractStreamReader::addTimeGap)
    virtual std::vector<std::pair<int64_t, int64_t>> takeTimeGaps(int pid) { return {}; }
    // The text subtitles of a track that the next one follows at once, without a gap: their start times, in ms as the
    // SRT text the demuxer hands on gives them, taken out once asked for (SRTStreamReader::addTouchingCue)
    virtual std::vector<int64_t> takeTouchingCues(int pid) { return {}; }
    // The time the demuxer has read the file up to, on the internal clock its packets carry; -1 where it cannot say
    [[nodiscard]] virtual int64_t getDemuxedTime() const { return -1; }
    // Keep them for this track from now on; only the tracks asked for are kept, so that nothing piles up for a
    // track nobody muxes
    virtual void keepBlockAdditions(int pid) {}

    [[nodiscard]] SubTrackFilter* getPidFilter(const int pid)
    {
        const PIDFilters::const_iterator itr = m_pidFilters.find(pid);
        return itr != m_pidFilters.end() ? itr->second : nullptr;
    }
    void setPidFilter(const int pid, SubTrackFilter* pidFilter) { m_pidFilters[pid] = pidFilter; }
    [[nodiscard]] virtual bool isPidFilterSupported() const { return false; }
    [[nodiscard]] virtual int64_t getFileDurationNano() const { return 0; }
    // Where the video starts and where it ends on the file's own clock, in nanoseconds, for a container that can say
    // without reading the whole file. A join puts the next file where the video ends, not where the file's Duration
    // ends (see METADemuxer::getChapters). False when the file has no video or the container cannot say.
    virtual bool getVideoSpanNano(int64_t& startNano, int64_t& endNano) { return false; }

   protected:
    int64_t m_timeOffset;
    uint32_t m_fileBlockSize;
    // Maps PID -> SubTrackFilter. Currently supports one filter per PID; extending to
    // multiple filters per PID would be needed to handle more than one 3D subtitle track.
    PIDFilters m_pidFilters;
};

#endif
