#include "matroskaDemuxer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

#include <fs/directory.h>
#include <fs/systemlog.h>
#include <types/types.h>

#include "abstractDemuxer.h"
#include "avPacket.h"
#include "bufferedFileReader.h"
#include "metaDemuxer.h"
#include "nalUnits.h"
#include "subTrackFilter.h"
#include "vodCoreException.h"

extern "C"
{
#include "zlib.h"
}

typedef uint64_t offset_t;

static constexpr int64_t AV_NOPTS_VALUE = AV_NOPTS_VALUE_INTERNAL;

static constexpr int PKT_FLAG_KEY = 1;
static constexpr int AVERROR_INVALIDDATA = -1;

static constexpr int COMPRESSION_STRIP_HEADERS = 3;
static constexpr int COMPRESSION_ZLIB = 0;

#define AV_RL32(x) ((x)[3] << 24 | (x)[2] << 16 | (x)[1] << 8 | (x)[0])

#define MAX(a, b) ((a) > (b) ? (a) : (b))

// An element that cannot be inside a Cluster: a top level element of the Segment, or the start of the next file of a
// join. It ends the Cluster before it when that Cluster's size is unknown (RFC 8794, 6.2: a live writer leaves it so)
// or runs past where its data really ends.
static bool endsCluster(const uint32_t id)
{
    switch (id)
    {
    case MATROSKA_ID_CLUSTER:
    case MATROSKA_ID_CUES:
    case MATROSKA_ID_TAGS:
    case MATROSKA_ID_CHAPTERS:
    case MATROSKA_ID_ATTACHMENTS:
    case MATROSKA_ID_SEEKHEAD:
    case MATROSKA_ID_INFO:
    case MATROSKA_ID_TRACKS:
    case MATROSKA_ID_SEGMENT:
    case EBML_ID_HEADER:
        return true;
    default:
        return false;
    }
}

static constexpr int MAX_TRACK_SIZE =
    (MAX(MAX(sizeof(MatroskaVideoTrack), sizeof(MatroskaAudioTrack)), sizeof(MatroskaSubtitleTrack)));

int MatroskaDemuxer::matroska_parse_index()
{
    int res = 0;
    uint32_t id;
    MatroskaDemuxIndex idx{};

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* one single index entry ('point') */
        case MATROSKA_ID_POINTENTRY:
            if ((res = ebml_read_master(&id)) < 0)
                break;

            /* in the end, we hope to fill one entry with a
             * timestamp, a file position and a tracknum */
            idx.pos = ULLONG_MAX;
            idx.time = ULLONG_MAX;
            idx.track = -1;

            while (res == 0)
            {
                if ((id = ebml_peek_id(&level_up)) == 0)
                {
                    res = -BufferedReader::DATA_EOF;
                    break;
                }
                if (level_up)
                {
                    level_up--;
                    break;
                }

                switch (id)
                {
                /* one single index entry ('point') */
                case MATROSKA_ID_CUETIME:
                {
                    int64_t time;
                    if ((res = ebml_read_uint(&id, &time)) < 0)
                        break;
                    idx.time = time * time_scale;
                    break;
                }

                /* position in the file + track to which it
                 * belongs */
                case MATROSKA_ID_CUETRACKPOSITION:
                    if ((res = ebml_read_master(&id)) < 0)
                        break;

                    while (res == 0)
                    {
                        if ((id = ebml_peek_id(&level_up)) == 0)
                        {
                            res = -BufferedReader::DATA_EOF;
                            break;
                        }
                        if (level_up)
                        {
                            level_up--;
                            break;
                        }

                        switch (id)
                        {
                        /* track number */
                        case MATROSKA_ID_CUETRACK:
                        {
                            int64_t num;
                            if ((res = ebml_read_uint(&id, &num)) < 0)
                                break;
                            idx.track = static_cast<int16_t>(num);
                            break;
                        }

                        /* position in file */
                        case MATROSKA_ID_CUECLUSTERPOSITION:
                        {
                            int64_t num;
                            if ((res = ebml_read_uint(&id, &num)) < 0)
                                break;
                            idx.pos = num + segment_start;
                            break;
                        }
                        case EBML_ID_VOID:
                        case EBML_ID_CRC32:
                            res = ebml_read_skip();
                            break;
                        default:
                            res = ebml_read_skip();
                            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in CuesTrackPositions");
                        }

                        if (level_up)
                        {
                            level_up--;
                            break;
                        }
                    }
                    break;
                case EBML_ID_VOID:
                case EBML_ID_CRC32:
                    res = ebml_read_skip();
                    break;
                default:
                    res = ebml_read_skip();
                    LTRACE(LT_INFO, 0, "Unknown entry " << id << " in cuespoint index");
                }

                if (level_up)
                {
                    level_up--;
                    break;
                }
            }

            /* so let's see if we got what we wanted */
            if (idx.pos != ULLONG_MAX && idx.time != ULLONG_MAX && idx.track != -1)
            {
                indexes.push_back(idx);
            }
            break;
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in cues header");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    return res;
}

MatroskaDemuxer::MatroskaDemuxer(const BufferedReaderManager& readManager)
    : IOContextDemuxer(readManager), levels(), m_title(), created(0), fileDuration(0)
{
    m_lastDeliveryPacket = nullptr;
    num_levels = 0;
    level_up = 0;
    peek_id = 0;
    done = false;
    num_streams = 0;
    segment_start = 0;
    time_scale = 0;
    m_firstTimecode.clear();
    index_parsed = false;
    metadata_parsed = false;
    attachments_parsed = false;
    m_attachments.clear();
    writing_app = nullptr;
    muxing_app = nullptr;
}

int MatroskaDemuxer::ebml_read_ascii(uint32_t* id, char** str)
{
    int res;
    int64_t rlength;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&rlength)) < 0)
        return res;
    const int size = static_cast<int>(rlength);

    // ebml strings are usually not 0-terminated, so we allocate one
    // byte more, read the string and nullptr-terminate it ourselves.
    *str = new char[size + 1];
    if (size < 0 || *str == nullptr)
    {
        THROW(ERR_MATROSKA_PARSE, "Memory allocation failed")
    }
    if (static_cast<int32_t>(get_buffer(reinterpret_cast<uint8_t*>(*str), size)) != size)
    {
        const offset_t pos = m_processedBytes;
        THROW(ERR_MATROSKA_PARSE, "Read error at pos. " << pos)
    }
    (*str)[size] = '\0';
    return 0;
}

int MatroskaDemuxer::ebml_read_header(char** doctype, int* version)
{
    uint32_t id;
    int levelUp, res = 0;

    /* default init */
    if (doctype)
        *doctype = nullptr;
    if (version)
        *version = 1;

    if ((id = ebml_peek_id(&levelUp)) == 0 || levelUp != 0 || id != EBML_ID_HEADER)
    {
        THROW(ERR_MATROSKA_PARSE, "This is not an EBML file (id=" << id << "/" << EBML_ID_HEADER)
    }
    if ((res = ebml_read_master(&id)) < 0)
        return res;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&levelUp)) == 0)
            return -BufferedReader::DATA_EOF;

        /* end-of-header */
        if (levelUp)
            break;

        switch (id)
        {
        /* is our read version uptodate? */
        case EBML_ID_EBMLREADVERSION:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                return res;
            if (num > EBML_VERSION)
            {
                THROW(ERR_MATROSKA_PARSE, "EBML version " << num << " > " << EBML_VERSION << " is not supported")
            }
            break;
        }

        /* we only handle 8 byte lengths at max */
        case EBML_ID_EBMLMAXSIZELENGTH:
        {
            int64_t num;

            if ((res = ebml_read_uint(&id, &num)) < 0)
                return res;
            if (num > 8)
            {
                THROW(ERR_MATROSKA_PARSE, "Integers of size " << num << " (> 8) not supported")
            }
            break;
        }

        /* we handle 4 byte IDs at max */
        case EBML_ID_EBMLMAXIDLENGTH:
        {
            int64_t num;

            if ((res = ebml_read_uint(&id, &num)) < 0)
                return res;
            if (num > 8)
            {
                THROW(ERR_MATROSKA_PARSE, "IDs of size " << num << " (> 8) not supported")
            }
            break;
        }

        case EBML_ID_DOCTYPE:
        {
            char* text;

            if ((res = ebml_read_ascii(&id, &text)) < 0)
                return res;
            if (doctype)
            {
                *doctype = text;
            }
            break;
        }

        case EBML_ID_DOCTYPEREADVERSION:
        {
            int64_t num;

            if ((res = ebml_read_uint(&id, &num)) < 0)
                return res;
            if (version)
                *version = static_cast<int>(num);
            break;
        }
        /* we ignore these four, as they don't tell us anything we care about */
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
        case EBML_ID_EBMLVERSION:
        case EBML_ID_DOCTYPEVERSION:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown data type " << id << " in EBML header");
        }
    }

    return 0;
}

void MatroskaDemuxer::matroska_queue_packet(AVPacket* pkt) { packets.push(pkt); }

int MatroskaDemuxer::rv_offset(const uint8_t* data, const int slice, const int slices)
{
    const int offset = 8 * slice + 4;
    return AV_RL32(data + offset) + 8 * slices;
}

/* Read signed/unsigned "EBML" numbers.
 * Return: number of bytes processed, < 0 on error.
 * XXX: use ebml_read_num(). */
int MatroskaDemuxer::matroska_find_track_by_num(const int64_t num) const
{
    for (int i = 0; i < num_tracks; i++)
    {
        if (tracks[i]->num == num)
            return i;
    }
    return -1;
}

int MatroskaDemuxer::matroska_ebmlnum_uint(const uint8_t* data, const int32_t size, uint64_t* num)
{
    int read = 1, n = 1, num_ffs = 0;
    uint64_t len_mask = 0x80;

    if (size <= 0)
        return AVERROR_INVALIDDATA;

    uint64_t total = data[0];
    while (read <= 8 && !(total & len_mask))
    {
        read++;
        len_mask >>= 1;
    }
    if (read > 8)
        return AVERROR_INVALIDDATA;

    if ((total &= (len_mask - 1)) == len_mask - 1)
        num_ffs++;
    if (size < read)
        return AVERROR_INVALIDDATA;
    while (n < read)
    {
        if (data[n] == 0xff)
            num_ffs++;
        total = (total << 8) | data[n];
        n++;
    }

    if (read == num_ffs)
        *num = ULLONG_MAX;
    else
        *num = total;

    return read;
}

int MatroskaDemuxer::matroska_ebmlnum_sint(const uint8_t* data, const int32_t size, int64_t* num)
{
    uint64_t unum;
    int res;

    /* read as unsigned number first */
    if ((res = matroska_ebmlnum_uint(data, size, &unum)) < 0)
        return res;

    /* make signed (weird way) */
    if (unum == ULLONG_MAX)
        *num = LLONG_MAX;
    else
        *num = static_cast<int64_t>(unum - ((1 << ((7 * res) - 1)) - 1));

    return res;
}

int MatroskaDemuxer::matroska_deliver_packet(AVPacket*& avPacket)
{
    if (!packets.empty())
    {
        avPacket = packets.front();
        packets.pop();
        return 0;
    }

    return -1;
}

int MatroskaDemuxer::ebml_read_sint(uint32_t* id, int64_t* num)
{
    unsigned n = 1;
    int negative = 0, res;
    int64_t size;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&size)) < 0)
        return res;
    if (size < 1 || size > 8)
    {
        const offset_t pos = m_processedBytes;
        THROW(ERR_MATROSKA_PARSE, "Invalid sint element size " << size << " at position " << pos)
    }
    if ((*num = get_byte()) & 0x80)
    {
        negative = 1;
        *num &= ~0x80;
    }
    while (n++ < size) *num = *num << 8 | get_byte();

    /* make signed */
    if (negative)
        *num = *num - (1LL << (8 * size - 1));

    return 0;
}

void MatroskaDemuxer::decompressData(uint8_t* data, const int size)
{
    z_stream zstream = {};

    int err = inflateInit(&zstream);
    if (err != Z_OK)
        return;

    zstream.avail_in = size;
    zstream.next_in = data;
    int curSize = size;
    do
    {
        curSize *= 3;
        m_tmpBuffer.resize(curSize);

        zstream.avail_out = static_cast<unsigned>(m_tmpBuffer.size() - zstream.total_out);
        zstream.next_out = m_tmpBuffer.data() + zstream.total_out;
        err = inflate(&zstream, Z_NO_FLUSH);
    } while (err == Z_OK && curSize < 10000000);

    m_tmpBuffer.resize(zstream.total_out);
    inflateEnd(&zstream);
    if (err != Z_STREAM_END)
        m_tmpBuffer.clear();
}

int MatroskaDemuxer::matroska_parse_block(uint8_t* data, int size, const int64_t pos, const int64_t cluster_time,
                                          const int64_t duration, int is_keyframe, int is_bframe)
{
    int res = 0;
    // AVStream *st;
    const uint8_t* origdata = data;
    int* lace_size = nullptr;
    int n, laces = 0;
    uint64_t num;

    /* first byte(s): tracknum */
    if ((n = matroska_ebmlnum_uint(data, size, &num)) < 0)
    {
        LTRACE(LT_ERROR, 0, "EBML block data error");
        delete[] origdata;
        return res;
    }
    data += n;
    size -= n;
    auto snum = static_cast<int64_t>(num);

    /* fetch track from num */
    const int track = matroska_find_track_by_num(snum);
    if (size <= 3 || track < 0 || track >= num_tracks)
    {
        LTRACE(LT_INFO, 0, "Invalid stream " << track << " or size " << size);
        delete[] origdata;
        return res;
    }
    if (tracks[track]->stream_index < 0)
        return res;
    /* block_time (relative to cluster time) */
    const auto block_time = static_cast<int16_t>(AV_RB16(data));
    data += 2;
    const int flags = *data++;
    size -= 3;
    if (is_keyframe == -1)
        is_keyframe = flags & 0x80 ? PKT_FLAG_KEY : 0;

    switch ((flags & 0x06) >> 1)
    {
    case 0x0: /* no lacing */
        laces = 1;
        lace_size = reinterpret_cast<int32_t*>(new uint8_t[sizeof(int)]);
        lace_size[0] = size;
        break;

    // see https://www.matroska.org/technical/notes.html
    // for explanations on types of lacing
    case 0x1: /* xiph lacing */
    case 0x2: /* fixed-size lacing */
    case 0x3: /* EBML lacing */
        if (size == 0)
        {
            res = -1;
            break;
        }
        // laces = number of laced frames
        laces = (*data) + 1;
        data += 1;
        size -= 1;
        lace_size = reinterpret_cast<int32_t*>(new uint8_t[laces * sizeof(int)]);
        memset(lace_size, 0, static_cast<size_t>(laces) * sizeof(int));

        switch ((flags & 0x06) >> 1)
        {
        case 0x1: /* xiph lacing */
        {
            int32_t total = 0;
            for (n = 0; res == 0 && n < laces - 1; n++)
            {
                while (true)
                {
                    if (size == 0)
                    {
                        res = -1;
                        break;
                    }
                    const uint8_t temp = *data;
                    lace_size[n] += temp;
                    data += 1;
                    size -= 1;
                    if (temp != 0xff)
                        break;
                }
                total += lace_size[n];
            }
            if (total > size)
            {
                LTRACE(LT_INFO, 0, "EBML block data error");
                break;
            }

            // if more  than one frame in the lace,
            // size of last frame is remaining size
            if (laces > 1)
                lace_size[n] = size - total;
            break;
        }

        case 0x2: /* fixed-size lacing */
            for (n = 0; n < laces; n++) lace_size[n] = size / laces;
            break;

        case 0x3: /* EBML lacing */
        {
            n = matroska_ebmlnum_uint(data, size, &num);
            if (n < 0)
            {
                LTRACE(LT_INFO, 0, "EBML block data error");
                break;
            }
            data += n;
            size -= n;
            int32_t total = lace_size[0] = static_cast<int32_t>(num);

            for (n = 1; res == 0 && n < laces - 1; n++)
            {
                const int r = matroska_ebmlnum_sint(data, size, &snum);
                if (r < 0 || r > size)
                {
                    LTRACE(LT_INFO, 0, "EBML block data error");
                    break;
                }
                data += r;
                size -= r;

                lace_size[n] = lace_size[n - 1] + static_cast<int32_t>(snum);
                total += lace_size[n];
            }
            // if more  than one frame in the lace,
            // size of last frame is remaining size
            if (laces > 1)
                lace_size[n] = size - total;

            // check that all read frame sizes are > 0
            for (n = 0; res == 0 && n < laces; n++)
            {
                if (lace_size[n] < 0)
                {
                    LTRACE(LT_INFO, 0, "EBML block data error");
                    break;
                }
            }
            break;
        }
        default:;
        }
        break;
    default:;
    }

    if (res == 0)
    {
        const int real_v = tracks[track]->flags & MATROSKA_TRACK_REAL_V;
        int64_t timecode = AV_NOPTS_VALUE;

        if (cluster_time != -1 && (block_time >= 0 || cluster_time >= -block_time))
            timecode = cluster_time + block_time;
        if (timecode != AV_NOPTS_VALUE && tracks[track]->timestampScale > 0 && tracks[track]->timestampScale != 1.0)
            timecode = llround(static_cast<double>(timecode) * tracks[track]->timestampScale);
        // A play timeline keeps each part's stretch only (keepBlock), a laced block frame by frame: an audio block
        // holds several frames, and a part may end inside it. Each frame is at its own time, a DefaultDuration after
        // the one before. What is kept moves to where the part plays, which matters for the tracks whose times come
        // from the container, the subtitles.
        const bool timeline = m_joinPart < m_joinRanges.size() && m_joinRanges[m_joinPart].startNs >= 0;
        const int64_t blockNs =
            timecode == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : timecode * static_cast<int64_t>(time_scale);
        const bool isVideo = tracks[track]->type == IOContextTrackType::VIDEO;
        const bool keepVideo = !timeline || !isVideo || keepBlock(tracks[track], blockNs, is_keyframe != 0);
        const int64_t timelineShiftNs =
            timeline ? m_joinRanges[m_joinPart].offsetNs - m_joinRanges[m_joinPart].startNs : m_joinShiftNs;
        if (!timeline && timecode != AV_NOPTS_VALUE &&
            m_firstTimecode.find(tracks[track]->num) == m_firstTimecode.end())
            m_firstTimecode[tracks[track]->num] = timecode;

        for (n = 0; n < laces; n++)
        {
            if (timeline)
            {
                const int64_t laceNs = blockNs == AV_NOPTS_VALUE
                                           ? AV_NOPTS_VALUE
                                           : blockNs + n * static_cast<int64_t>(tracks[track]->default_duration);
                if (isVideo ? !keepVideo : !keepBlock(tracks[track], laceNs, false))
                {
                    data += lace_size[n];
                    size -= lace_size[n];
                    continue;
                }
                if (timecode != AV_NOPTS_VALUE && m_firstTimecode.find(tracks[track]->num) == m_firstTimecode.end())
                    m_firstTimecode[tracks[track]->num] = timecode;
            }
            int slices = 1;

            if (real_v)
            {
                slices = *data++ + 1;
                size--;
                lace_size[n]--;
            }

            for (int slice = 0; slice < slices; slice++)
            {
                int slice_size, slice_offset = 0;
                if (real_v)
                    slice_offset = rv_offset(data, slice, slices);
                if (slice + 1 == slices)
                    slice_size = lace_size[n] - slice_offset;
                else
                    slice_size = rv_offset(data, slice + 1, slices) - slice_offset;

                auto* pkt = new AVPacket();
                pkt->data = nullptr;
                pkt->size = 0;
                // A block whose timestamp could not be worked out above leaves timecode at the
                // "no timestamp" sentinel. Multiplying that produced a number that is not the
                // sentinel any more and, for the common case, lands close enough to zero to be
                // taken for a real timestamp at the very start of the stream. Anything ordering
                // pictures by timestamp then sorts that picture to the front, silently. Carry the
                // sentinel through instead so it can be recognised.
                //
                // Block times and durations are counted in the file's TimestampScale, in nanoseconds. It
                // is a millisecond unless the file says otherwise, and was taken to be one always: a file
                // written at 100 us had every subtitle ten times too late. Through nanoseconds the result
                // for a millisecond is exactly what it was, and ten hours still fit in 64 bits.
                const auto toInternal = [this](const int64_t t)
                { return t * static_cast<int64_t>(time_scale) * (INTERNAL_PTS_FREQ / 1000000) / 1000; };
                pkt->pts = timecode == AV_NOPTS_VALUE
                               ? AV_NOPTS_VALUE
                               : toInternal(timecode) + timelineShiftNs * (INTERNAL_PTS_FREQ / 1000000) / 1000;
                pkt->pos = pos;
                const double trackScale = tracks[track]->timestampScale > 0 ? tracks[track]->timestampScale : 1.0;
                pkt->duration =
                    duration == AV_NOPTS_VALUE
                        ? AV_NOPTS_VALUE
                        : toInternal(trackScale == 1.0 ? duration
                                                       : llround(static_cast<double>(duration) * trackScale));

                pkt->stream_index = track + 1;  // tracks[track]->stream_index;

                int offset = 0;
                uint8_t* curPtr = data + slice_offset;
                int curPtr_size = size - slice_offset;
                m_tmpBuffer.clear();
                if (tracks[track]->encodingAlgo == COMPRESSION_STRIP_HEADERS)
                {
                    // The bytes the writer stripped from the front of every frame (an AC-3 or DTS sync word, for
                    // instance) are put back in front of it. They were written into the bytes before the frame in
                    // place, which was allowed only with room left by a RealVideo slice table, so for every other
                    // track every block was dropped and the track came out empty. The frame is put together in a
                    // buffer of its own now.
                    offset = static_cast<int>(tracks[track]->encodingAlgoPriv.size());
                    if (offset)
                    {
                        if (slice_size < 0 || slice_size > curPtr_size)
                        {
                            LTRACE(LT_ERROR, 0, "invalid slice size");
                            delete pkt;
                            delete[] origdata;
                            delete[] lace_size;
                            return res;
                        }
                        m_tmpBuffer.append(tracks[track]->encodingAlgoPriv.data(), offset);
                        m_tmpBuffer.append(curPtr, slice_size);
                        curPtr = m_tmpBuffer.data();
                        curPtr_size = static_cast<int>(m_tmpBuffer.size());
                    }
                }
                else if (tracks[track]->encodingAlgo == COMPRESSION_ZLIB)
                {
                    decompressData(curPtr, slice_size);
                    curPtr = m_tmpBuffer.data();
                    curPtr_size = slice_size = static_cast<int>(m_tmpBuffer.size());
                }

                if (curPtr_size < 0 || slice_size + offset < 0 || curPtr_size < slice_size + offset)
                {
                    LTRACE(LT_ERROR, 0, "invalid slice size");
                    delete pkt;
                    delete[] origdata;
                    delete[] lace_size;
                    return res;
                }

                // The BlockAdditions of a video or audio track a Matroska output keeps them for, by the block's time
                if (n == 0 && !m_pendingAdditions.empty() && m_keepAdditionPids.count(track + 1))
                    m_blockAddLog[track + 1][pkt->pts] = m_pendingAdditions;
                if (tracks[track]->parsed_priv_data != nullptr)
                {
                    if (n == 0 && !m_pendingAdditions.empty())
                        tracks[track]->parsed_priv_data->setBlockAdditions(m_pendingAdditions.data(),
                                                                           static_cast<int>(m_pendingAdditions.size()));
                    tracks[track]->parsed_priv_data->extractData(pkt, curPtr, slice_size + offset);
                }
                else if (slice_size + offset > 0)
                {
                    pkt->data = new uint8_t[slice_size + offset];
                    pkt->size = slice_size + offset;
                    memcpy(pkt->data, curPtr, slice_size + offset);
                }

                if (n == 0)
                    pkt->flags = is_keyframe;

                matroska_queue_packet(pkt);

                // the next lace is a duration later; a block with no duration (AV_NOPTS_VALUE, which is not 0)
                // leaves it without a time, where adding the marker gave a time near the most negative one
                if (timecode != AV_NOPTS_VALUE)
                    timecode = duration && duration != AV_NOPTS_VALUE ? timecode + duration : AV_NOPTS_VALUE;
            }
            data += lace_size[n];
            size -= lace_size[n];
        }
    }

    delete[] lace_size;
    delete[] origdata;
    return res;
}

int MatroskaDemuxer::matroska_parse_blockgroup(const int64_t cluster_time)
{
    int res = 0;
    uint32_t id;
    int is_bframe = 0;
    int is_keyframe = PKT_FLAG_KEY;
    const size_t last_num_packets = packets.size();
    int64_t duration = AV_NOPTS_VALUE;
    uint8_t* data = nullptr;
    int size = 0;
    int64_t pos = 0;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* one block inside the group. Note, block parsing is one
         * of the harder things, so this code is a bit complicated.
         * See http://www.matroska.org/ for documentation. */
        case MATROSKA_ID_BLOCK:
        {
            pos = m_processedBytes;
            res = ebml_read_binary(&id, &data, &size);
            break;
        }

        case MATROSKA_ID_BLOCKDURATION:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            duration = num;
            break;
        }

        case MATROSKA_ID_BLOCKREFERENCE:
        {
            int64_t num;
            /* We've found a reference, so not even the first frame in
             * the lace is a key frame. */
            is_keyframe = 0;
            if (last_num_packets != packets.size())
                packets.back()->flags = 0;
            if ((res = ebml_read_sint(&id, &num)) < 0)
                break;
            if (num > 0)
                is_bframe = 1;
            break;
        }
        case MATROSKA_ID_BLOCKADDITIONS:
        {
            // Kept for a track copied as it is; every other reader has no use for them and gets none
            uint8_t* body = nullptr;
            int bodySize = 0;
            if ((res = ebml_read_binary(&id, &body, &bodySize)) < 0)
                break;
            if (body && bodySize > 0)
                m_pendingAdditions.assign(body, body + bodySize);
            delete[] body;
            break;
        }
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in blockgroup data");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    if (res)
    {
        // A group that is the last thing in the file: the look for its next child finds the end of
        // the file instead. The Block it holds has been read in full and is still parsed, or the last
        // block of the file was lost, which for a subtitle track is its last subtitle.
        if (res == -BufferedReader::DATA_EOF && data != nullptr)
        {
            const int blockRes = matroska_parse_block(data, size, pos, cluster_time, duration, is_keyframe, is_bframe);
            m_pendingAdditions.clear();
            return blockRes ? blockRes : res;
        }
        // matroska_parse_block owns and frees the block; on any other early return it was leaked
        delete[] data;
        m_pendingAdditions.clear();
        return res;
    }

    if (data != nullptr)
        res = matroska_parse_block(data, size, pos, cluster_time, duration, is_keyframe, is_bframe);
    m_pendingAdditions.clear();

    return res;
}

/* Read the next element as an unsigned int.
 * 0 is success, < 0 is failure. */
int MatroskaDemuxer::ebml_read_uint(uint32_t* id, int64_t* num)
{
    unsigned n = 0;
    int res;
    int64_t rlength;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&rlength)) < 0)
        return res;
    const uint64_t size = rlength;
    if (size < 1 || size > 8)
    {
        THROW(ERR_MATROSKA_PARSE, "Invalid uint element size " << size << " at position " << m_processedBytes)
    }

    /* big-endian ordening; build up number */
    *num = 0;
    while (n++ < size) *num = *num << 8 | get_byte();
    return 0;
}

/* Read: the element content data ID.
 * Return: the number of bytes read or < 0 on error. */
int MatroskaDemuxer::ebml_read_element_id(uint32_t* id, int* levelUp)
{
    int read;
    int64_t total;

    /* if we re-call this, use our cached ID */
    if (peek_id != 0)
    {
        if (levelUp)
            *levelUp = 0;
        *id = peek_id;
        return 0;
    }

    /* read out the "EBML number", include tag in ID */
    if ((read = ebml_read_num(4, &total)) < 0)
        return read;
    *id = peek_id = (static_cast<int>(total) | (1 << (read * 7)));

    /* level tracking */
    if (levelUp)
        *levelUp = ebml_read_element_level_up();

    return read;
}

/* Skip the next element.
 * 0 is success, -1 is failure. */
int MatroskaDemuxer::ebml_read_skip()
{
    uint32_t id;
    int64_t length;
    int res;

    if ((res = ebml_read_element_id(&id, nullptr)) < 0 || (res = ebml_read_element_length(&length)) < 0)
        return res;
    skip_bytes(length);
    return 0;
}

/* Read the next element, but only the header. The contents
 * are supposed to be sub-elements which can be read separately.
 * 0 is success, < 0 is failure. */
int MatroskaDemuxer::ebml_read_master(uint32_t* id)
{
    int64_t length;
    int res;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&length)) < 0)
        return res;
    const bool unknownSize = res >= 1 && res <= 8 && length == (static_cast<int64_t>(1) << (7 * res)) - 1;

    /* protect... (Heaven forbids that the '>' is true) */
    if (num_levels >= EBML_MAX_DEPTH)
    {
        THROW(ERR_MATROSKA_PARSE, "File moves beyond max. allowed depth (" << EBML_MAX_DEPTH << ")")
    }

    /* remember level */
    MatroskaLevel* level = &levels[num_levels++];
    level->start = m_processedBytes;
    level->length = length;
    level->unknownSize = unknownSize;
    return 0;
}

/* Read: element content length.
 * Return: the number of bytes read or < 0 on error. */

int MatroskaDemuxer::ebml_read_element_length(int64_t* length)
{
    /* clear cache since we're now beyond that data point */
    peek_id = 0;

    /* read out the "EBML number", include tag in ID */
    return ebml_read_num(8, length);
}

int MatroskaDemuxer::ebml_read_binary(uint32_t* id, uint8_t** binary, int* size)
{
    int64_t rlength;
    int res;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&rlength)) < 0)
        return res;
    *size = static_cast<int>(rlength);

    *binary = new uint8_t[*size];
    if (!(*binary))
    {
        THROW(ERR_COMMON_MEMORY, "Memory allocation error")
    }

    if (static_cast<int>(get_buffer(*binary, *size)) != *size)
    {
        THROW(ERR_MATROSKA_PARSE, "Matroska parser: read error at pos " << m_processedBytes)
    }
    return 0;
}

int MatroskaDemuxer::matroska_parse_cluster()
{
    int res = 0;
    uint32_t id;
    int64_t cluster_time = 0;
    uint8_t* data;
    int64_t pos;
    int size;
    const int clusterLevels = num_levels;  // the Cluster's own level is the last of them

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        // A Cluster of unknown size, as a live writer leaves every one, ends where the next Cluster or another top
        // level element begins, and so does one whose size runs past where it really ends. Its level was kept open:
        // each Cluster of a live file took one more of the 16 levels, and the 17th refused the file ("beyond max.
        // allowed depth"); and in a join, "a"+"b", the next file's EBML header and Segment fell inside the first
        // file's last Cluster, were stepped over as unknown entries, and the whole second file was lost while the mux
        // said it was successful. The Cluster's level is closed here and the element is left for the top level.
        if (endsCluster(id))
        {
            if (clusterLevels > 0 && num_levels >= clusterLevels)
            {
                // Said once per file: the file is opened for the track list and again for the mux.
                static std::set<std::string> reportedPast;
                const std::string& file = m_joinPart < m_joinFiles.size() ? m_joinFiles[m_joinPart] : m_sourceName;
                if (!levels[clusterLevels - 1].unknownSize && reportedPast.insert(file).second)
                    LTRACE(LT_WARN, 2,
                           "Warning: a Cluster of " << file
                                                    << " gives a size that runs past its data. It is read up to where "
                                                       "the next element begins.");
                num_levels = clusterLevels - 1;
            }
            return 0;
        }

        switch (id)
        {
        /* cluster timecode */
        case MATROSKA_ID_CLUSTERTIMECODE:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            cluster_time = num;
            break;
        }

        /* a group of blocks inside a cluster */
        case MATROSKA_ID_BLOCKGROUP:
            if ((res = ebml_read_master(&id)) < 0)
                break;
            res = matroska_parse_blockgroup(cluster_time);
            break;

        case MATROSKA_ID_SIMPLEBLOCK:
            pos = m_processedBytes;
            res = ebml_read_binary(&id, &data, &size);
            if (res == 0)
                res = matroska_parse_block(data, size, pos, cluster_time, AV_NOPTS_VALUE, -1, 0);
            break;

        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;
        default:
            LTRACE(LT_WARN, 0, "Unknown entry " << id << " in cluster data");
            res = ebml_read_skip();
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    return res;
}

int MatroskaDemuxer::ebml_read_element_level_up()
{
    // offset_t pos = url_ftell(pb);
    int num = 0;

    while (num_levels > 0)
    {
        const MatroskaLevel* level = &levels[num_levels - 1];

        if (m_processedBytes >= level->start + level->length)
        {
            num_levels--;
            num++;
        }
        else
        {
            break;
        }
    }

    return num;
}

void MatroskaDemuxer::openFile(const std::string& streamName)
{
    readClose();
    if (!m_bufferedReader->openStream(m_readerID, streamName.c_str()))
        THROW(ERR_FILE_NOT_FOUND, "Can't open stream " << streamName)
    m_curPos = m_bufEnd = nullptr;
    m_processedBytes = 0;
    m_isEOF = false;

    num_levels = 0;
    level_up = 0;
    peek_id = 0;
    done = false;
    num_streams = 0;

    segment_start = 0;
    time_scale = 0;
    m_firstTimecode.clear();
    index_parsed = false;
    metadata_parsed = false;
    attachments_parsed = false;
    m_attachments.clear();
    m_sourceName = streamName;

    writing_app = nullptr;
    muxing_app = nullptr;
    num_tracks = 0;
    m_joinPart = 0;
    matroska_read_header();
    // Separately, on its own handle, so nothing about the streaming read is disturbed.
    loadAttachments(streamName);
    loadChapters(streamName);
    loadTrackExtras(streamName);
    applyStartCodeRule();
}

// A file this muxer wrote records how its source framed its start codes, because Matroska stores
// these codecs length prefixed and holds none. Without that record every stream rebuilt from such a
// file comes out four byte, whatever the source did, and never matches it. Any other file carries no
// manifest, so nothing changes for it.
//
// This is the whole track, as it is read for an ordinary mux. The split path applies the same rule
// again on its own side, from the same line, so the two agree.
void MatroskaDemuxer::applyStartCodeRule()
{
    std::vector<uint8_t> manifestBytes;
    if (!getAttachment(MANIFEST_ATTACHMENT_NAME, manifestBytes))
        return;

    const std::string manifest(manifestBytes.begin(), manifestBytes.end());
    const std::string rule = dvManifestValue(manifest, "start-code");
    if (rule.empty())
        return;

    // Each video track's own rule when the file gives one per track, found by the track's TrackUID: a tool that rewrites
    // the file may number its tracks anew (mkvmerge does, with --track-order or a track left out), and a rule found by
    // number went to another track. A file from before, or one whose tracks were given new UIDs, has no line for its
    // tracks, and the line for the whole file goes to every track as it did (see MatroskaMuxer::writeAttachments).
    const auto ownRule = [&](const Track* t) {
        return dvManifestValue(manifest, "start-code-track-uid-" + std::to_string(static_cast<uint64_t>(t->uid)));
    };
    bool perTrack = false;
    for (int i = 0; i < num_tracks && !perTrack; i++)
        perTrack = tracks[i] != nullptr && !ownRule(tracks[i]).empty();

    int applied = 0;
    for (int i = 0; i < num_tracks; i++)
    {
        if (tracks[i] != nullptr && tracks[i]->parsed_priv_data != nullptr)
        {
            const std::string own = perTrack ? ownRule(tracks[i]) : rule;
            if (own.empty())
                continue;
            tracks[i]->parsed_priv_data->setStartCodeRule(own);
            applied++;
        }
    }

    // Said once per file, not once per open. A file is opened and thrown away during track
    // detection before the mux opens it again, so the plain message appears twice for one input and
    // reads like something happening twice.
    static std::set<std::string> reported;
    if (applied > 0 && reported.insert(m_attachmentSource + "|" + rule).second)
        LTRACE(
            LT_INFO, 2,
            "This file records how its source framed the video (" << rule << "), so it is framed the same way again.");
}

void MatroskaDemuxer::setFileIterator(FileNameIterator* itr)
{
    IOContextDemuxer::setFileIterator(itr);  // the reader moves on to the next file through it
    const auto* join = dynamic_cast<const FileListIterator*>(itr);
    m_joinFiles = join != nullptr ? join->getFiles() : std::vector<std::string>();
    m_joinRanges = join != nullptr ? join->getRanges() : std::vector<JoinRange>();
    m_partVideoOn = m_partVideoDone = false;
}

// A block of a play timeline's part is kept when it lies in the part's stretch. The video is cut in the order the
// blocks are stored, from the key frame the stretch starts at up to the key frame it ends at, so that no picture is
// kept without the pictures it is predicted from. The other tracks are cut at the same times.
bool MatroskaDemuxer::keepBlock(const MatroskaTrack* track, const int64_t ns, const bool keyFrame)
{
    if (m_joinPart >= m_joinRanges.size() || m_joinRanges[m_joinPart].startNs < 0)
        return true;
    const JoinRange& range = m_joinRanges[m_joinPart];
    if (ns == AV_NOPTS_VALUE)
        return false;
    if (track->type == IOContextTrackType::VIDEO)
    {
        if (!m_partVideoOn && !m_partVideoDone && keyFrame && ns >= range.startNs)
            m_partVideoOn = true;
        if (m_partVideoOn && keyFrame && range.endNs != INT64_MAX && ns >= range.endNs)
        {
            m_partVideoOn = false;
            m_partVideoDone = true;
        }
        return m_partVideoOn;
    }
    return ns >= range.startNs && ns < range.endNs;
}

// The stream of a joined input, "a"+"b", has just reached the header of its next file, and the tracks were read
// again from it. openFile applies the record of the framing to the first file only, so every part after it lost
// that record and was framed four byte whatever its own record said. Each part's own record now applies, read from
// that part, and a part without one is framed as a file without one always was.
void MatroskaDemuxer::nextJoinPart()
{
    m_partVideoOn = m_partVideoDone = false;
    if (++m_joinPart >= m_joinFiles.size())
        return;
    // A plain join puts each file where the video of the one before ends (see METADemuxer::getChapters). The video
    // and the audio follow by counting their frames; the tracks whose times come from the container, the subtitles,
    // started again at the next file's own clock, so every subtitle of a second file came where the first one's
    // were. Their times move by the same amount as the file's chapters.
    if (m_joinRanges.empty())
    {
        const auto span = [this](const std::string& file, int64_t& start, int64_t& end)
        {
            MatroskaDemuxer part(m_readManager);
            part.openFile(file);
            if (!part.getVideoSpanNano(start, end))
            {
                start = 0;
                end = part.getFileDurationNano();
            }
        };
        int64_t prevStart = 0, prevEnd = 0, start = 0, end = 0;
        span(m_joinFiles[m_joinPart - 1], prevStart, prevEnd);
        span(m_joinFiles[m_joinPart], start, end);
        if (prevEnd > 0)
            m_joinShiftNs += prevEnd - start;
    }
    m_attachments.clear();
    loadAttachments(m_joinFiles[m_joinPart]);
    applyStartCodeRule();
}

// A track is a block of characters that a Track was never constructed in, so ~Track never runs
// and everything the type owns is left behind. The mp4 demuxer had the same shape and its half
// was fixed for 2.18.11; this one was not. Measured on one small Matroska file, the codec
// private data, the codec id and the parsed private data object all survived the demuxer.
//
// Calling ~Track() would be undefined here, because nothing was ever constructed in these
// bytes. So what the type owns is released by hand, in the same order the destructor would
// have used, and the vector is emptied by swapping a fresh one over it.
void MatroskaDemuxer::freeTrack(MatroskaTrack* track)
{
    if (track == nullptr)
        return;
    delete[] track->name;
    delete[] track->codec_id;
    delete[] track->codec_name;
    delete[] track->codec_priv;
    delete track->parsed_priv_data;
    std::vector<uint8_t>().swap(track->encodingAlgoPriv);
    delete[] reinterpret_cast<char*>(track);
}

void MatroskaDemuxer::readClose()
{
    delete[] writing_app;
    delete[] muxing_app;
    while (!packets.empty())
    {
        const AVPacket* pkt = packets.front();
        delete[] pkt->data;
        delete pkt;
        packets.pop();
    }
    for (int i = 0; i < num_tracks; i++) freeTrack(tracks[i]);
}

// --------------------------- refactored from ffmpeg matroska decoder -----------------------

int MatroskaDemuxer::ebml_read_num(const int max_size, int64_t* number)
{
    // ByteIOContext *pb = &matroska->ctx->pb;
    int len_mask = 0x80, read = 1, n = 1;
    int64_t total;

    /* the first byte tells us the length in bytes - get_byte() can normally
     * return 0, but since that's not a valid first ebmlID byte, we can
     * use it safely here to catch EOS. */
    if ((total = get_byte()) == 0)
    {
        /* we might encounter EOS here */
        if (!m_isEOF)
            THROW(ERR_MATROSKA_PARSE,
                  "Matroska parse error: Invalid EBML number size " << total << " at pos " << m_processedBytes - 1)
        return -BufferedReader::DATA_EOF;
    }

    /* get the length of the EBML number */
    while (read <= max_size && !(total & len_mask))
    {
        read++;
        len_mask >>= 1;
    }
    if (read > max_size)
    {
        const offset_t pos = m_processedBytes - 1;
        THROW(ERR_MATROSKA_PARSE, "Matroska parse error: Invalid EBML number size " << total << " at pos " << pos)
    }

    /* read out length */
    total &= ~len_mask;
    while (n++ < read) total = total << 8 | get_byte();
    *number = total;
    return read;
}

uint32_t MatroskaDemuxer::ebml_peek_id(int* levelUp)
{
    uint32_t id;
    return ebml_read_element_id(&id, levelUp) < 0 ? 0 : id;
}

int MatroskaDemuxer::readPacket(AVPacket& avPacket)
{
    uint32_t id;
    if (m_lastDeliveryPacket)
    {
        delete[] m_lastDeliveryPacket->data;
        delete m_lastDeliveryPacket;
        m_lastDeliveryPacket = nullptr;
    }

    // Read stream until we have a packet queued.
    AVPacket* newPacket = nullptr;
    while (matroska_deliver_packet(newPacket) != 0)
    {
        // Have we already reached the end? Then what the converters still hold, once, before the end is reported.
        if (done)
        {
            if (!m_tracksFinished)
            {
                m_tracksFinished = true;
                if (queueFinalPackets())
                    continue;
            }
            return BufferedReader::DATA_EOF;
        }

        int res = 0;
        while (res == 0)
        {
            if ((id = ebml_peek_id(&level_up)) == 0)
            {
                done = true;
                break;
            }
            if (level_up)
            {
                level_up--;
                break;
            }

            switch (id)
            {
            case MATROSKA_ID_CLUSTER:
                if ((res = ebml_read_master(&id)) < 0)
                    break;
                if ((res = matroska_parse_cluster()) == 0)
                    res = 1;  // Parsed one cluster, let's get out.
                break;
            case EBML_ID_HEADER:
                // The next file of a join: what the tracks of this one still hold comes first. Nothing of this file
                // is open any more, also not a Segment of unknown size or one whose size runs past its end, whose
                // level stayed open under the next file's Segment.
                num_levels = 0;
                level_up = 0;
                queueFinalPackets();
                matroska_read_header();
                nextJoinPart();
                m_tracksFinished = false;
                break;
            default:
                res = ebml_read_skip();
            }

            if (level_up)
            {
                level_up--;
                break;
            }
        }

        if (res == -1)
            done = true;
    }
    if (newPacket)
    {
        memcpy(&avPacket, newPacket, sizeof(AVPacket));
    }
    else
        avPacket = AVPacket();
    m_lastDeliveryPacket = newPacket;
    return 0;
}

// One more packet for each track whose converter still holds something at the end of its file: the end of the last
// DVD subtitle picture, which is held until the next picture says whether it comes first.
bool MatroskaDemuxer::queueFinalPackets()
{
    bool queued = false;
    for (int i = 0; i < num_tracks; i++)
    {
        if (tracks[i]->parsed_priv_data == nullptr || tracks[i]->stream_index < 0)
            continue;
        auto* pkt = new AVPacket();
        pkt->data = nullptr;
        pkt->size = 0;
        pkt->pts = AV_NOPTS_VALUE;
        pkt->dts = AV_NOPTS_VALUE;
        pkt->duration = AV_NOPTS_VALUE;
        pkt->stream_index = i + 1;
        pkt->flags = 0;
        if (tracks[i]->parsed_priv_data->finish(pkt) && pkt->size > 0)
        {
            matroska_queue_packet(pkt);
            queued = true;
        }
        else
        {
            delete[] pkt->data;
            delete pkt;
        }
    }
    return queued;
}

int MatroskaDemuxer::matroska_read_header()
{
    for (int i = 0; i < num_tracks; i++) freeTrack(tracks[i]);
    num_tracks = 0;

    // MatroskaDemuxContext *matroska = s->priv_data;
    char* doctype = nullptr;
    int version, last_level, res;
    uint32_t id;

    // matroska->ctx = s;

    /* First read the EBML header. */
    if ((res = ebml_read_header(&doctype, &version)) < 0)
        return res;
    // ebml_read_header hands back a string it allocated, and it was never released on any path,
    // including the two that throw just below. Nine bytes on every Matroska file opened.
    const std::unique_ptr<char[]> doctypeOwner(doctype);
    // WebM is Matroska with a narrower set of codecs and its own DocType. It was refused outright, although the same
    // file with the DocType "matroska" reads.
    if ((doctype == nullptr) || (strcmp(doctype, "matroska") != 0 && strcmp(doctype, "webm") != 0))
    {
        THROW(ERR_MATROSKA_PARSE, "Wrong EBML doctype ('" << (doctype ? doctype : "(none)") << "' != 'matroska').")
    }
    if (version > 2)
    {
        THROW(ERR_MATROSKA_PARSE, "Matroska demuxer version 2 too old for file version " << version)
    }

    /* The next thing is a segment. */
    while (true)
    {
        if ((id = ebml_peek_id(&last_level)) == 0)
            return -BufferedReader::DATA_EOF;
        if (id == MATROSKA_ID_SEGMENT)
            break;
        if ((res = ebml_read_skip()) < 0)
            return res;
    }

    /* We now have a Matroska segment.
     * Seeks are from the beginning of the segment,
     * after the segment ID/length. */
    if ((res = ebml_read_master(&id)) < 0)
        return res;
    segment_start = m_processedBytes;

    time_scale = 1000000;
    /* we've found our segment, start reading the different contents in here */
    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* stream info */
        case MATROSKA_ID_INFO:
        {
            if ((res = ebml_read_master(&id)) < 0)
                break;
            res = matroska_parse_info();
            break;
        }

        /* track info headers */
        case MATROSKA_ID_TRACKS:
        {
            if ((res = ebml_read_master(&id)) < 0)
                break;
            res = matroska_parse_tracks();
            break;
        }

        case MATROSKA_ID_CHAPTERS:
            // Read by loadChapters, which finds them wherever they are, also after the clusters.
            res = ebml_read_skip();
            break;

        /* stream index */
        case MATROSKA_ID_CUES:
        {
            if (!index_parsed)
            {
                if ((res = ebml_read_master(&id)) < 0)
                    break;
                res = matroska_parse_index();
            }
            else
                res = ebml_read_skip();
            break;
        }

        /* metadata */
        case MATROSKA_ID_TAGS:
        {
            if (!metadata_parsed)
            {
                if ((res = ebml_read_master(&id)) < 0)
                    break;
                res = matroska_parse_metadata();
            }
            else
                res = ebml_read_skip();
            break;
        }

        /* file index (if seekable, seek to Cues/Tags to parse it) */
        case MATROSKA_ID_SEEKHEAD:
        {
            ebml_read_skip();
            break;
        }

        /* attachments are read separately, see loadAttachments */
        case MATROSKA_ID_ATTACHMENTS:
        {
            ebml_read_skip();
            break;
        }

        case MATROSKA_ID_CLUSTER:
        {
            /* Do not read the master - this will be done in the next
             * call to matroska_read_packet. */
            res = 1;
            break;
        }

        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;

        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown matroska file header ID " << id);
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    if (num_tracks == 0 && ebml_peek_id(nullptr) == MATROSKA_ID_CLUSTER)
        readTracksAfterClusters();

    /* Have we found a cluster? */
    if (ebml_peek_id(nullptr) == MATROSKA_ID_CLUSTER)
    {
        for (int i = 0; i < num_tracks; i++)
        {
            MatroskaTrack* track = tracks[i];
            track->stream_index = -1;
            if (track->codec_id == nullptr)
                continue;

            track->stream_index = num_streams++;

            if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AVC_FOURCC) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedH264TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_HEVC_FOURCC) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedH265TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VVC_FOURCC) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedH266TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AV1) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedAV1TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VIDEO_VFW_FOURCC) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedVC1TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VIDEO_VC1) && (track->codec_priv != nullptr))
            {
                track->parsed_priv_data = new ParsedVC1TrackData(track->codec_priv, track->codec_priv_size, true);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_AC3))
            {
                track->parsed_priv_data = new ParsedAC3TrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_AAC))
            {
                track->parsed_priv_data = new ParsedAACTrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_PCM_BIG) ||
                     !strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_PCM_LIT) ||
                     !strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_ACM))
            {
                track->parsed_priv_data = new ParsedLPCMTrackData(track);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_FLAC))
            {
                track->parsed_priv_data = new ParsedFLACTrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_OPUS))
            {
                track->parsed_priv_data = new ParsedOpusTrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_VORBIS))
            {
                track->parsed_priv_data =
                    new ParsedMarkedTrackData("VORBISCP", track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_ALAC))
            {
                track->parsed_priv_data =
                    new ParsedMarkedTrackData("ALACCPRV", track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VP8) || !strcmp(track->codec_id, MATROSKA_CODEC_ID_VP9))
            {
                track->parsed_priv_data = new ParsedVpxTrackData();
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SRT) ||
                     !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_ASCII))
            {
                track->parsed_priv_data = new ParsedSRTTrackData(track->codec_priv, track->codec_priv_size);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_ASS) ||
                     !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_SSA))
            {
                track->parsed_priv_data =
                    new ParsedSRTTrackData(track->codec_priv, track->codec_priv_size, ParsedSRTTrackData::Form::Ass);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_WEBVTT) || isWebmVttText(track->codec_id))
            {
                track->parsed_priv_data = new ParsedSRTTrackData(track->codec_priv, track->codec_priv_size,
                                                                 isWebmVttText(track->codec_id)
                                                                     ? ParsedSRTTrackData::Form::WebmVtt
                                                                     : ParsedSRTTrackData::Form::WebVtt);
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_PGS))
            {
                track->parsed_priv_data = new ParsedPGTrackData();
            }
            else if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_VOBSUB) ||
                     !strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_DVB))
            {
                // DVD and DVB subtitles were not read at all. Their pictures become PGS, timed to the frame rate of
                // the file's first video track.
                double videoFps = 0;
                for (int j = 0; j < num_tracks && videoFps == 0; j++)
                    if (tracks[j]->type == IOContextTrackType::VIDEO && tracks[j]->default_duration > 0)
                        videoFps = 1e9 / static_cast<double>(tracks[j]->default_duration);
                if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_VOBSUB))
                    track->parsed_priv_data = new ParsedBitmapSubTrackData(
                        new VobSubDecoder(track->codec_priv, track->codec_priv_size, videoFps));
                else
                    track->parsed_priv_data = new ParsedBitmapSubTrackData(
                        new DvbSubDecoder(track->codec_priv, track->codec_priv_size, videoFps));
            }
        }
        // The BlockAdditionMapping bodies by pid, the number getTrackList and the meta use (index + 1)
        m_addMappings.clear();
        for (int i = 0; i < num_tracks; i++)
            if (const auto it = m_addMappingsByTrack.find(tracks[i]); it != m_addMappingsByTrack.end())
                m_addMappings[i + 1] = it->second;
        m_addMappingsByTrack.clear();
        m_maxAddIds.clear();
        for (int i = 0; i < num_tracks; i++)
            if (const auto it = m_maxAddIdsByTrack.find(tracks[i]); it != m_maxAddIdsByTrack.end())
                m_maxAddIds[i + 1] = it->second;
        m_maxAddIdsByTrack.clear();
        // A track to be copied stays copied in every later file of a join, whose header is read again, and so does a
        // track drawn with libass
        applyTrackCopy();
        applyAssDrawing();
        res = 0;
    }

    return res;
}

bool MatroskaDemuxer::getCopySource(const int pid, CopySource& out) const
{
    if (pid < 1 || pid > num_tracks || tracks[pid - 1] == nullptr || tracks[pid - 1]->codec_id == nullptr)
        return false;
    const MatroskaTrack* track = tracks[pid - 1];
    out.codecId = track->codec_id;
    out.codecPrivate.clear();
    if (track->codec_priv && track->codec_priv_size > 0)
        out.codecPrivate.assign(track->codec_priv, track->codec_priv + track->codec_priv_size);
    out.addMappings.clear();
    if (const auto it = m_addMappings.find(pid); it != m_addMappings.end())
        out.addMappings = it->second;
    const auto maxIt = m_maxAddIds.find(pid);
    out.maxBlockAddId = maxIt != m_maxAddIds.end() ? maxIt->second : 0;
    return true;
}

bool MatroskaDemuxer::getBlockAdditionInfo(const int pid, uint64_t& maxId,
                                           std::vector<std::vector<uint8_t>>& mappings) const
{
    const auto maxIt = m_maxAddIds.find(pid);
    if (maxIt == m_maxAddIds.end() || maxIt->second == 0)
        return false;
    maxId = maxIt->second;
    const auto mapIt = m_addMappings.find(pid);
    mappings = mapIt != m_addMappings.end() ? mapIt->second : std::vector<std::vector<uint8_t>>();
    return true;
}

// The additions of the block nearest to pts, within 8 ms: the readers of a video track give its frames the times they
// work out themselves, a fraction of a millisecond from the container's, which are whole milliseconds. Those before
// it by more than 10 s were never asked for (a cut, a frame the reader left out) and go.
bool MatroskaDemuxer::takeBlockAdditions(const int pid, const int64_t pts, std::vector<uint8_t>& out)
{
    const auto logIt = m_blockAddLog.find(pid);
    if (logIt == m_blockAddLog.end() || logIt->second.empty())
        return false;
    auto& log = logIt->second;
    static constexpr int64_t TOLERANCE = INTERNAL_PTS_PER_MS * 8;
    auto best = log.end();
    for (auto it = log.lower_bound(pts - TOLERANCE); it != log.end() && it->first <= pts + TOLERANCE; ++it)
        if (best == log.end() || std::llabs(it->first - pts) < std::llabs(best->first - pts))
            best = it;
    bool found = false;
    if (best != log.end())
    {
        out = std::move(best->second);
        log.erase(best);
        found = true;
    }
    log.erase(log.begin(), log.lower_bound(pts - 10 * INTERNAL_PTS_FREQ));
    return found;
}

void MatroskaDemuxer::setTrackCopy(const int pid)
{
    m_copyPids.insert(pid);
    applyTrackCopy();
}

void MatroskaDemuxer::applyTrackCopy()
{
    for (const int pid : m_copyPids)
    {
        if (pid < 1 || pid > num_tracks || tracks[pid - 1] == nullptr)
            continue;
        MatroskaTrack* track = tracks[pid - 1];
        if (dynamic_cast<ParsedCopyTrackData*>(track->parsed_priv_data))
            continue;
        delete track->parsed_priv_data;
        track->parsed_priv_data = new ParsedCopyTrackData();
        // The header of the track (CodecPrivate: the styles of ASS and SSA, the palette of VobSub) is written once,
        // from the first file. A later file of a join with another header had its subtitles kept with the first
        // file's without a word: the Default style of the second file, red in its own file, became the first file's
        // white one.
        std::vector<uint8_t> priv;
        if (track->codec_priv && track->codec_priv_size > 0)
            priv.assign(track->codec_priv, track->codec_priv + track->codec_priv_size);
        if (const auto [first, isNew] = m_copyPrivates.try_emplace(pid, priv); !isNew && first->second != priv)
            LTRACE(LT_WARN, 2,
                   "Warning: track " << pid
                                     << ": a later file of the join has another header (CodecPrivate, which holds the "
                                        "styles of ASS and SSA and the palette of DVD subtitles) than the first file. "
                                        "Only the first file's is written, so the later file's subtitles may look "
                                        "otherwise.");
    }
}

void MatroskaDemuxer::setAssDrawing(const int pid, const ass_render::Drawing& drawing)
{
    m_assPids[pid] = drawing;
    applyAssDrawing();
}

void MatroskaDemuxer::applyAssDrawing()
{
    if (m_assPids.empty())
        return;
    std::vector<ass_render::Font> fonts;
    bool fontsRead = false;
    for (const auto& [pid, drawing] : m_assPids)
    {
        if (pid < 1 || pid > num_tracks || tracks[pid - 1] == nullptr)
            continue;
        MatroskaTrack* track = tracks[pid - 1];
        if (dynamic_cast<ParsedBitmapSubTrackData*>(track->parsed_priv_data))
            continue;
        // the fonts attached to the file, read once for all its ASS tracks
        if (!fontsRead)
        {
            fonts = ass_render::fontsFromAttachments(getPassThroughAttachments());
            fontsRead = true;
            // a fonts-dir that is not a folder adds no font, and the meta reader has warned about it already
            std::error_code ec;
            const bool fontsDirUsed =
                !drawing.fontsDir.empty() &&
                std::filesystem::is_directory(std::u8string(drawing.fontsDir.begin(), drawing.fontsDir.end()), ec);
            LTRACE(LT_INFO, 2,
                   "ASS subtitles are drawn with libass, with " << fonts.size() << " font(s) attached to the file"
                                                                << (fontsDirUsed ? ", the fonts in " : "")
                                                                << (fontsDirUsed ? drawing.fontsDir : "")
                                                                << " and the system's fonts.");
        }
        delete track->parsed_priv_data;
        track->parsed_priv_data =
            new ParsedBitmapSubTrackData(new AssDecoder(track->codec_priv, track->codec_priv_size, drawing, fonts));
    }
}

/* From here on, it's all XML-style DTD stuff... Needs no comments. */

int MatroskaDemuxer::matroska_parse_info()
{
    int res = 0;
    uint32_t id;

    // av_log(matroska->ctx, AV_LOG_DEBUG, "Parsing info...\n");

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* cluster timecode */
        case MATROSKA_ID_TIMECODESCALE:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            time_scale = num;
            break;
        }

        case MATROSKA_ID_DURATION:
        {
            double num;
            if ((res = ebml_read_float(&id, &num)) < 0)
                break;
            fileDuration = static_cast<int64_t>(num * static_cast<double>(time_scale));
            break;
        }

        case MATROSKA_ID_TITLE:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            strncpy(m_title, text, sizeof(m_title) - 1);
            delete[] text;
            break;
        }

        case MATROSKA_ID_WRITINGAPP:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            writing_app = text;
            break;
        }

        case MATROSKA_ID_MUXINGAPP:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            muxing_app = text;
            break;
        }

        case MATROSKA_ID_DATEUTC:
        {
            int64_t time;
            if ((res = ebml_read_date(&id, &time)) < 0)
                break;
            created = time;
            break;
        }
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;

        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in info header");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    return res;
}

/* Read the next element as a date (nanoseconds since 1/1/2000).
 * 0 is success, < 0 is failure. */
int MatroskaDemuxer::ebml_read_date(uint32_t* id, int64_t* date) { return ebml_read_sint(id, date); }

/* Read the next element as a float.
 * 0 is success, < 0 is failure. */
int MatroskaDemuxer::ebml_read_float(uint32_t* id, double* num)
{
    int res;
    int64_t size;

    if ((res = ebml_read_element_id(id, nullptr)) < 0 || (res = ebml_read_element_length(&size)) < 0)
        return res;

    if (size == 4)
    {
        *num = av_int2flt(get_be32());
    }
    else if (size == 8)
    {
        *num = av_int2dbl(get_be64());
    }
    else
    {
        const offset_t pos = m_processedBytes;
        THROW(ERR_MATROSKA_PARSE, "Invalid float element size " << size << " at position " << pos)
    }
    return 0;
}

/* Read the next element as a UTF-8 string.
 * 0 is success, < 0 is failure. */
int MatroskaDemuxer::ebml_read_utf8(uint32_t* id, char** str) { return ebml_read_ascii(id, str); }

int MatroskaDemuxer::matroska_parse_metadata()
{
    int res = 0;
    uint32_t id;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in metadata header");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    return res;
}

// ─── attachments ─────────────────────────────────────────────────────────────
//
// Read on a file handle of their own, deliberately, and NOT through the demuxer's own reader.
//
// Attachments can sit either side of the clusters. This project's muxer writes them after, because
// their contents are not known until the mux has finished, so the streaming header parse below
// stops before ever reaching them. The obvious answer is to seek there and seek back, and that was
// tried: it disturbs the reader's position, its cached element id and its buffer, and the damage
// surfaces far away, deep inside the clusters, as a parse error. A second handle cannot do that to
// anything, and the cost is a handful of seeks over element headers.
namespace
{
struct EbmlScan
{
    File& f;
    int64_t pos = 0;
    bool sizeUnknown = false;  // the size of the last element read has all its value bits set (RFC 8794, 6.2)

    explicit EbmlScan(File& file) : f(file) {}

    bool byteAt(const int64_t at, uint8_t& out)
    {
        f.seek(at);
        return f.read(&out, 1) == 1;
    }

    // An element id keeps its marker bit; a size does not. Both are variable length, the length
    // given by the position of the top set bit of the first byte.
    bool readNumber(int64_t& value, const bool keepMarker, int& width)
    {
        uint8_t first;
        if (!byteAt(pos, first) || first == 0)
            return false;
        width = 1;
        uint8_t mask = 0x80;
        while (!(first & mask))
        {
            mask >>= 1;
            width++;
            if (width > 8)
                return false;
        }
        std::vector<uint8_t> raw(width);
        f.seek(pos);
        if (f.read(raw.data(), width) != width)
            return false;
        value = keepMarker ? raw[0] : (raw[0] & (mask - 1));
        for (int i = 1; i < width; ++i) value = (value << 8) | raw[i];
        pos += width;
        return true;
    }

    bool readElement(uint32_t& id, int64_t& size, int64_t& bodyPos)
    {
        int width = 0;
        int64_t rawId = 0;
        if (!readNumber(rawId, true, width))
            return false;
        id = static_cast<uint32_t>(rawId);
        if (!readNumber(size, false, width))
            return false;
        sizeUnknown = size == (static_cast<int64_t>(1) << (7 * width)) - 1;
        bodyPos = pos;
        return true;
    }

    // An unsigned integer element's body, big endian, at most eight bytes.
    bool readUInt(const int64_t at, const int64_t width, uint64_t& out)
    {
        uint8_t raw[8] = {0};
        if (width < 0 || width > 8)
            return false;
        f.seek(at);
        if (width > 0 && f.read(raw, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return false;
        out = 0;
        for (int64_t i = 0; i < width; ++i) out = (out << 8) | raw[i];
        return true;
    }
};

// One AttachedFile: its name, and where its payload lives.
bool scanAttachedFile(EbmlScan& scan, const int64_t end, std::string& name, int64_t& dataPos, int64_t& dataSize)
{
    while (scan.pos < end)
    {
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (!scan.readElement(id, size, body))
            return false;
        if (id == MATROSKA_ID_FILENAME)
        {
            std::vector<char> text(static_cast<size_t>(size) + 1, 0);
            scan.f.seek(body);
            if (scan.f.read(text.data(), static_cast<uint32_t>(size)) != static_cast<int>(size))
                return false;
            name.assign(text.data(), static_cast<size_t>(size));
        }
        else if (id == MATROSKA_ID_FILEDATA)
        {
            dataPos = body;
            dataSize = size;
        }
        scan.pos = body + size;
    }
    return true;
}

// The same reading over bytes already in memory. The Cues are read this way, in one go: walked element by element on
// the file, the index of a long film costs tens of thousands of reads.
struct EbmlMem
{
    const uint8_t* p;
    const uint8_t* end;

    bool number(int64_t& value, const bool keepMarker)
    {
        if (p >= end || *p == 0)
            return false;
        int width = 1;
        uint8_t mask = 0x80;
        while (!(*p & mask))
        {
            mask >>= 1;
            width++;
        }
        if (end - p < width)
            return false;
        value = keepMarker ? *p : (*p & (mask - 1));
        for (int i = 1; i < width; ++i) value = (value << 8) | p[i];
        p += width;
        return true;
    }

    // One element whose body is all in memory. On success p stands at its body, and the caller steps over it.
    bool element(uint32_t& id, int64_t& size)
    {
        int64_t rawId = 0;
        if (!number(rawId, true) || !number(size, false) || size > end - p)
            return false;
        id = static_cast<uint32_t>(rawId);
        return true;
    }

    [[nodiscard]] uint64_t uintBody(const int64_t size) const
    {
        uint64_t value = 0;
        for (int64_t i = 0; i < size && i < 8; ++i) value = (value << 8) | p[i];
        return value;
    }
};

// Where one SeekHead says the Cues are, and a further SeekHead if it names one, as offsets from the start of the
// segment's data. What it does not name is left as it was.
void readSeekPositions(EbmlScan& scan, const int64_t body, const int64_t size, int64_t& cues, int64_t& nextSeekHead)
{
    scan.pos = body;
    while (scan.pos < body + size)
    {
        uint32_t id = 0;
        int64_t entrySize = 0, entry = 0;
        if (!scan.readElement(id, entrySize, entry))
            return;
        uint64_t seekId = 0, seekPos = 0;
        bool haveId = false, havePos = false;
        while (id == MATROSKA_ID_SEEKENTRY && scan.pos < entry + entrySize)
        {
            uint32_t fieldId = 0;
            int64_t fieldSize = 0, field = 0;
            if (!scan.readElement(fieldId, fieldSize, field))
                return;
            if (fieldId == MATROSKA_ID_SEEKID)
                haveId = scan.readUInt(field, fieldSize, seekId);
            else if (fieldId == MATROSKA_ID_SEEKPOSITION)
                havePos = scan.readUInt(field, fieldSize, seekPos);
            scan.pos = field + fieldSize;
        }
        if (haveId && havePos && seekId == MATROSKA_ID_CUES)
            cues = static_cast<int64_t>(seekPos);
        else if (haveId && havePos && seekId == MATROSKA_ID_SEEKHEAD)
            nextSeekHead = static_cast<int64_t>(seekPos);
        scan.pos = entry + entrySize;
    }
}

// The blocks of one track, from the cluster at `from` to the end of the segment or for at most `maxClusters`
// clusters, read through their headers alone: no picture is read. onBlock gets the timestamp and the BlockDuration in
// the file's own units (-1 where the block has none), whether it is a keyframe, and how many frames are laced into it,
// and returns false to stop. What is not a cluster is stepped over. The walk ends quietly where the file stops making
// sense. A cluster of unknown size, as a live writer leaves every one, ends where an element begins that cannot be in
// it; the walk stopped at the first one, and a track without DefaultDuration in such a file was taken to be 25 fps.
template <typename OnBlock>
void walkTrackBlocks(EbmlScan& scan, const int64_t from, const int64_t segmentEnd, int maxClusters, const int64_t track,
                     OnBlock onBlock)
{
    scan.pos = from;
    while (scan.pos < segmentEnd)
    {
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (!scan.readElement(id, size, body))
            return;
        const bool openCluster = id == MATROSKA_ID_CLUSTER && scan.sizeUnknown;
        if ((!openCluster && size > segmentEnd - body) || (id == MATROSKA_ID_CLUSTER && maxClusters-- <= 0))
            return;
        const int64_t elementEnd = openCluster ? segmentEnd : body + size;
        uint64_t clusterTime = 0;
        while (id == MATROSKA_ID_CLUSTER && scan.pos < elementEnd)
        {
            const int64_t childAt = scan.pos;
            uint32_t child = 0;
            int64_t childSize = 0, childBody = 0;
            if (!scan.readElement(child, childSize, childBody))
                return;
            if (openCluster && endsCluster(child))
            {
                scan.pos = childAt;
                break;
            }
            if (childSize > elementEnd - childBody)
                return;
            const int64_t childEnd = childBody + childSize;
            int64_t blockAt = child == MATROSKA_ID_SIMPLEBLOCK ? childBody : -1;
            int64_t duration = -1;
            bool key = true;  // a BlockGroup without a ReferenceBlock is a keyframe
            if (child == MATROSKA_ID_CLUSTERTIMECODE && !scan.readUInt(childBody, childSize, clusterTime))
                return;
            while (child == MATROSKA_ID_BLOCKGROUP && scan.pos < childEnd)
            {
                uint32_t field = 0;
                int64_t fieldSize = 0, fieldBody = 0;
                uint64_t value = 0;
                if (!scan.readElement(field, fieldSize, fieldBody))
                    return;
                if (field == MATROSKA_ID_BLOCK)
                    blockAt = fieldBody;
                else if (field == MATROSKA_ID_BLOCKDURATION && scan.readUInt(fieldBody, fieldSize, value))
                    duration = static_cast<int64_t>(value);
                else if (field == MATROSKA_ID_BLOCKREFERENCE)
                    key = false;
                scan.pos = fieldBody + fieldSize;
            }
            if (blockAt >= 0)
            {
                // The block's own header: track number, timestamp against the cluster's, flags, lace count.
                int64_t blockTrack = 0;
                int width = 0;
                uint8_t head[4] = {0};
                scan.pos = blockAt;
                if (!scan.readNumber(blockTrack, false, width))
                    return;
                if (blockTrack == track)
                {
                    scan.f.seek(scan.pos);
                    if (scan.f.read(head, 4) < 3)
                        return;
                    const auto relative = static_cast<int16_t>(head[0] << 8 | head[1]);
                    if (child == MATROSKA_ID_SIMPLEBLOCK)
                        key = (head[2] & 0x80) != 0;
                    const int laces = (head[2] & 0x06) ? head[3] + 1 : 1;
                    if (!onBlock(static_cast<int64_t>(clusterTime) + relative, duration, key, laces))
                        return;
                }
            }
            scan.pos = childEnd;
        }
        if (!openCluster)
            scan.pos = elementEnd;
    }
}
}  // namespace

// Find the attachments WITHOUT reading the media.
//
// This runs on every Matroska file that is opened, so what it costs is what every file costs
// before its track list appears. It used to walk the top level from front to back, stepping over
// each cluster by its declared size. That is correct, and on a long file it is thousands of seeks:
// a 36.6 GB file here holds 7,293 clusters, so about 29,000 seeks and reads to answer a question
// the format already answers. On a local SSD that is under a second. On an external or network
// drive it is minutes, and it looks like the program has hung.
//
// The SeekHead at the front of the segment says where the top level elements are, and this muxer
// writes one deliberately so that what follows the clusters can be found (see writeFrontSeekHead).
// mkvmerge writes one too. So read the index and go straight there.
//
// If a SeekHead exists and does not mention attachments, the file has none: an index that lists
// Info, Tracks and Cues but no Attachments is not silent about them, it is saying there are none.
// Only a file with no SeekHead at all falls back to the old walk.
// Where a top-level element of the segment starts: in the head of the segment, before the first cluster, or where a
// SeekHead there says it is, following one SeekHead that points to another. False when the file does not say. RFC 9559
// (6.1) lets the Chapters, Tags and Tracks follow the clusters as long as a SeekHead before them says so; a chapter
// editor puts the Chapters there when the head has no room for them.
static bool findTopLevelElement(EbmlScan& scan, const int64_t fileSize, const uint32_t wanted, int64_t& bodyPos,
                                int64_t& size, int64_t* segmentDataOut = nullptr)
{
    const auto readUInt = [&](const int64_t at, const int64_t width, uint64_t& out) -> bool
    {
        if (width <= 0 || width > 8)
            return false;
        uint8_t buf[8] = {0};
        scan.f.seek(at);
        if (scan.f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return false;
        out = 0;
        for (int i = 0; i < static_cast<int>(width); ++i) out = (out << 8) | buf[i];
        return true;
    };
    // The positions one SeekHead gives for the wanted element and for a further SeekHead, 0 where it names neither.
    const auto readSeekHead = [&](const int64_t body, const int64_t len, int64_t& wantedAt, int64_t& nextSeekHead)
    {
        const int64_t end = body + len;
        scan.pos = body;
        while (scan.pos < end)
        {
            uint32_t id = 0;
            int64_t sz = 0, b = 0;
            if (!scan.readElement(id, sz, b))
                break;
            if (id == MATROSKA_ID_SEEKENTRY)
            {
                uint64_t seekId = 0, seekPos = 0;
                bool haveId = false, havePos = false;
                scan.pos = b;
                while (scan.pos < b + sz)
                {
                    uint32_t eid = 0;
                    int64_t esz = 0, eb = 0;
                    if (!scan.readElement(eid, esz, eb))
                        break;
                    if (eid == MATROSKA_ID_SEEKID)
                        haveId = readUInt(eb, esz, seekId);
                    else if (eid == MATROSKA_ID_SEEKPOSITION)
                        havePos = readUInt(eb, esz, seekPos);
                    scan.pos = eb + esz;
                }
                if (haveId && havePos && seekId == wanted && wantedAt == 0)
                    wantedAt = static_cast<int64_t>(seekPos);
                else if (haveId && havePos && seekId == MATROSKA_ID_SEEKHEAD)
                    nextSeekHead = static_cast<int64_t>(seekPos);
            }
            scan.pos = b + sz;
        }
    };

    int64_t segmentData = -1;
    int64_t segmentEnd = fileSize;
    scan.pos = 0;
    while (scan.pos < fileSize)
    {
        uint32_t id = 0;
        int64_t len = 0, body = 0;
        if (!scan.readElement(id, len, body))
            return false;
        if (id == MATROSKA_ID_SEGMENT)
        {
            segmentData = body;
            segmentEnd = (len > 0 && body + len <= fileSize) ? body + len : fileSize;
            break;
        }
        scan.pos = body + len;
    }
    if (segmentData < 0)
        return false;
    if (segmentDataOut)
        *segmentDataOut = segmentData;

    int64_t wantedAt = 0;
    int64_t nextSeekHead = 0;
    scan.pos = segmentData;
    while (scan.pos < segmentEnd)
    {
        uint32_t id = 0;
        int64_t len = 0, body = 0;
        if (!scan.readElement(id, len, body) || id == MATROSKA_ID_CLUSTER)
            break;
        if (id == wanted)
        {
            bodyPos = body;
            size = len;
            return true;
        }
        if (id == MATROSKA_ID_SEEKHEAD)
        {
            const int64_t resume = body + len;
            readSeekHead(body, len, wantedAt, nextSeekHead);
            scan.pos = resume;
            continue;
        }
        scan.pos = body + len;
    }
    if (wantedAt == 0 && nextSeekHead > 0)
    {
        scan.pos = segmentData + nextSeekHead;
        uint32_t id = 0;
        int64_t len = 0, body = 0;
        if (scan.readElement(id, len, body) && id == MATROSKA_ID_SEEKHEAD)
        {
            int64_t ignored = 0;
            readSeekHead(body, len, wantedAt, ignored);
        }
    }
    if (wantedAt == 0)
        return false;
    scan.pos = segmentData + wantedAt;
    uint32_t id = 0;
    return scan.readElement(id, size, bodyPos) && id == wanted && bodyPos + size <= fileSize;
}

// RFC 9559 (6.1) lets the Tracks follow the clusters when a SeekHead before them says where they are, and a current
// ffmpeg writes them there when the codec data did not fit the room it kept for them in the head. The header was read
// up to the first cluster only, so such a file had no tracks at all: the listing showed none, and a mux of its track
// wrote an empty output and called it successful. The Tracks element is found through the SeekHead now and read from
// memory, as if the reader stood there, and the reader then goes on at the first cluster where it was.
void MatroskaDemuxer::readTracksAfterClusters()
{
    File f;
    if (!f.open(m_sourceName.c_str(), File::ofRead))
        return;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_TRACKS, body, size) || size <= 0 || size > (16 << 20))
        return;
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seek(body);
    if (f.read(data.data(), static_cast<uint32_t>(size)) != static_cast<int>(size))
        return;

    uint8_t* const savedCur = m_curPos;
    uint8_t* const savedEnd = m_bufEnd;
    const int64_t savedProcessed = m_processedBytes;
    const bool savedEof = m_isEOF;
    const int savedLevels = num_levels;
    const int savedLevelUp = level_up;
    const int savedPeek = peek_id;
    m_curPos = data.data();
    m_bufEnd = data.data() + size;
    m_processedBytes = body;  // file offsets, so the levels around it still hold
    m_memoryOnly = true;
    peek_id = 0;
    level_up = 0;
    if (num_levels < EBML_MAX_DEPTH)
    {
        MatroskaLevel* level = &levels[num_levels++];
        level->start = body;
        level->length = size;
        level->unknownSize = false;
        matroska_parse_tracks();
    }
    m_memoryOnly = false;
    m_curPos = savedCur;
    m_bufEnd = savedEnd;
    m_processedBytes = savedProcessed;
    m_isEOF = savedEof;
    num_levels = savedLevels;
    level_up = savedLevelUp;
    peek_id = savedPeek;
    if (num_tracks > 0)
        LTRACE(LT_INFO, 2, "Matroska: the track list of " << m_sourceName << " follows the clusters, and is read there.");
}

// The chapters of a Matroska file, read wherever the Chapters element is. They used to be read only while the header
// was walked, which stops at the first cluster, so chapters after the clusters were lost. And of what was read:
//   - the FIRST edition was taken, where RFC 9559 (20.1.2) makes the first one marked EditionFlagDefault the one to use
//   - a hidden chapter was taken as a visible one (20.2.5: each chapter's own flag says, nested ones included)
//   - a nested chapter was dropped (20.2.3)
//   - of a chapter's names in several languages the LAST was kept. Now the first is the title, and all are kept
// --------------------------- the play timeline: ordered editions and linked segments ---------------------------
//
// An edition with EditionFlagOrdered set is a timeline of its own: its chapters are played one after the other, each
// from its ChapterTimeStart to its ChapterTimeEnd, and a chapter with a ChapterSegmentUUID takes that stretch from
// another segment, a file of its own in the same folder (RFC 9559, 17.2 and 20.1.3). A player plays the default
// edition that way. A Matroska input was read from start to end whatever its edition said, so a file whose ordered
// edition plays 0 to 2 s and 5 to 8 s came out ten seconds long, and the parts kept in other files were not there.
//
// The parts are read as a join of the files they come from, a file as often as the timeline uses it, and the
// demuxer keeps each part's stretch only (MatroskaDemuxer::keepBlock). A stretch cannot start or end anywhere: the
// video can only be cut before a key frame. Each end of a part is moved to the key frame of the file's first video
// track that is nearest to it, and every other track is cut at the same times, so the tracks stay together.
namespace
{
constexpr uint32_t MATROSKA_ID_PREVUUID = 0x3CB923;
constexpr uint32_t MATROSKA_ID_NEXTUUID = 0x3EB923;
constexpr uint16_t MATROSKA_ID_CHAPTERFLAGENABLED_ = 0x4598;
constexpr uint16_t MATROSKA_ID_CHAPTERSEGMENTUUID = 0x6E67;
constexpr uint16_t MATROSKA_ID_CHAPTERSEGMENTEDITIONUID = 0x6EBC;

struct SegmentFacts
{
    struct Atom
    {
        int64_t start = -1;
        int64_t end = -1;
        bool enabled = true;
        std::string segmentUuid;
        uint64_t segmentEdition = 0;
        std::string name;
    };
    struct Edition
    {
        uint64_t uid = 0;
        bool isDefault = false;
        bool ordered = false;
        std::vector<Atom> atoms;  // the top-level chapters, in their order
    };
    std::string uuid, prevUuid, nextUuid;  // 16 bytes each, empty where the file has none
    uint64_t timestampScale = 1000000;
    int64_t durationNs = -1;
    int64_t videoTrack = -1;
    int64_t segmentData = -1, segmentEnd = 0, firstCluster = -1;
    std::vector<Edition> editions;

    [[nodiscard]] const Edition* defaultEdition() const
    {
        for (const Edition& e : editions)
            if (e.isDefault)
                return &e;
        return editions.empty() ? nullptr : &editions.front();
    }
    [[nodiscard]] const Edition* edition(const uint64_t uid) const
    {
        for (const Edition& e : editions)
            if (e.uid == uid)
                return &e;
        return nullptr;
    }
};

// What the timeline needs of one file, read from its head without the media.
bool readSegmentFacts(const std::string& path, SegmentFacts& facts)
{
    File f;
    if (!f.open(path.c_str(), File::ofRead))
        return false;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    const auto children = [&](const int64_t at, const int64_t len)
    {
        std::vector<std::tuple<uint32_t, int64_t, int64_t>> out;
        scan.pos = at;
        while (scan.pos < at + len)
        {
            uint32_t id = 0;
            int64_t size = 0, body = 0;
            if (!scan.readElement(id, size, body) || size > at + len - body)
                break;
            out.emplace_back(id, body, size);
            scan.pos = body + size;
        }
        return out;
    };
    const auto bytes = [&](const int64_t at, const int64_t len)
    {
        std::string out(static_cast<size_t>((std::max)(int64_t{0}, (std::min)(len, int64_t{1024}))), '\0');
        scan.f.seek(at);
        if (!out.empty() && scan.f.read(out.data(), static_cast<uint32_t>(out.size())) != static_cast<int>(out.size()))
            out.clear();
        return out;
    };
    const auto uint = [&](const int64_t at, const int64_t len)
    {
        uint64_t v = 0;
        return scan.readUInt(at, len, v) ? v : 0;
    };

    int64_t body = 0, size = 0;
    if (findTopLevelElement(scan, fileSize, MATROSKA_ID_INFO, body, size, &facts.segmentData))
        for (const auto& [id, b, len] : children(body, size))
        {
            if (id == MATROSKA_ID_TIMECODESCALE)
                facts.timestampScale = uint(b, len);
            else if (id == MATROSKA_ID_DURATION && (len == 4 || len == 8))
            {
                const std::string raw = bytes(b, len);
                if (raw.size() == static_cast<size_t>(len))
                {
                    uint64_t v = 0;
                    for (const char c : raw) v = v << 8 | static_cast<uint8_t>(c);
                    double d = 0;
                    if (len == 4)
                    {
                        const auto v32 = static_cast<uint32_t>(v);
                        float fl = 0;
                        memcpy(&fl, &v32, 4);
                        d = fl;
                    }
                    else
                        memcpy(&d, &v, 8);
                    facts.durationNs = static_cast<int64_t>(d * static_cast<double>(facts.timestampScale));
                }
            }
            else if (id == MATROSKA_ID_SEGMENTUID && len == 16)
                facts.uuid = bytes(b, len);
            else if (id == MATROSKA_ID_PREVUUID && len == 16)
                facts.prevUuid = bytes(b, len);
            else if (id == MATROSKA_ID_NEXTUUID && len == 16)
                facts.nextUuid = bytes(b, len);
        }
    if (facts.timestampScale == 0)
        facts.timestampScale = 1000000;
    if (findTopLevelElement(scan, fileSize, MATROSKA_ID_TRACKS, body, size))
        for (const auto& [id, b, len] : children(body, size))
        {
            if (id != MATROSKA_ID_TRACKENTRY || facts.videoTrack >= 0)
                continue;
            int64_t number = -1, type = 0;
            for (const auto& [tid, tb, tlen] : children(b, len))
            {
                if (tid == MATROSKA_ID_TRACKNUMBER)
                    number = static_cast<int64_t>(uint(tb, tlen));
                else if (tid == MATROSKA_ID_TRACKTYPE)
                    type = static_cast<int64_t>(uint(tb, tlen));
            }
            if (type == 1)
                facts.videoTrack = number;
        }
    if (findTopLevelElement(scan, fileSize, MATROSKA_ID_CHAPTERS, body, size))
        for (const auto& [id, b, len] : children(body, size))
        {
            if (id != MATROSKA_ID_EDITIONENTRY)
                continue;
            SegmentFacts::Edition edition;
            for (const auto& [eid, eb, elen] : children(b, len))
            {
                if (eid == MATROSKA_ID_EDITIONUID)
                    edition.uid = uint(eb, elen);
                else if (eid == MATROSKA_ID_EDITIONFLAGDEFAULT)
                    edition.isDefault = uint(eb, elen) != 0;
                else if (eid == MATROSKA_ID_EDITIONFLAGORDERED)
                    edition.ordered = uint(eb, elen) != 0;
                else if (eid == MATROSKA_ID_CHAPTERATOM)
                {
                    SegmentFacts::Atom atom;
                    for (const auto& [aid, ab, alen] : children(eb, elen))
                    {
                        if (aid == MATROSKA_ID_CHAPTERTIMESTART)
                            atom.start = static_cast<int64_t>(uint(ab, alen));
                        else if (aid == MATROSKA_ID_CHAPTERTIMEEND)
                            atom.end = static_cast<int64_t>(uint(ab, alen));
                        else if (aid == MATROSKA_ID_CHAPTERFLAGENABLED_)
                            atom.enabled = uint(ab, alen) != 0;
                        else if (aid == MATROSKA_ID_CHAPTERSEGMENTUUID && alen == 16)
                            atom.segmentUuid = bytes(ab, alen);
                        else if (aid == MATROSKA_ID_CHAPTERSEGMENTEDITIONUID)
                            atom.segmentEdition = uint(ab, alen);
                        else if (aid == MATROSKA_ID_CHAPTERDISPLAY && atom.name.empty())
                            for (const auto& [did, db, dlen] : children(ab, alen))
                                if (did == MATROSKA_ID_CHAPSTRING)
                                {
                                    atom.name = bytes(db, dlen);
                                    atom.name.resize(strlen(atom.name.c_str()));
                                }
                    }
                    edition.atoms.push_back(atom);
                }
            }
            facts.editions.push_back(edition);
        }

    // where the clusters begin, for the key frames
    facts.segmentData = -1;
    scan.pos = 0;
    while (scan.pos < fileSize)
    {
        uint32_t id = 0;
        int64_t len = 0, b = 0;
        if (!scan.readElement(id, len, b))
            break;
        if (id == MATROSKA_ID_SEGMENT)
        {
            facts.segmentData = b;
            facts.segmentEnd = (len > 0 && len <= fileSize - b) ? b + len : fileSize;
            break;
        }
        scan.pos = b + len;
    }
    if (facts.segmentData >= 0)
    {
        scan.pos = facts.segmentData;
        while (scan.pos < facts.segmentEnd)
        {
            const int64_t at = scan.pos;
            uint32_t id = 0;
            int64_t len = 0, b = 0;
            if (!scan.readElement(id, len, b))
                break;
            if (id == MATROSKA_ID_CLUSTER)
            {
                facts.firstCluster = at;
                break;
            }
            scan.pos = b + len;
        }
    }
    return true;
}

// The times of the key frames of a file's first video track, in nanoseconds, read from the block headers alone.
std::vector<int64_t> keyFrameTimesNs(const std::string& path, const SegmentFacts& facts)
{
    std::vector<int64_t> times;
    File f;
    if (facts.videoTrack < 0 || facts.firstCluster < 0 || !f.open(path.c_str(), File::ofRead))
        return times;
    EbmlScan scan(f);
    walkTrackBlocks(scan, facts.firstCluster, facts.segmentEnd, INT_MAX, facts.videoTrack,
                    [&](const int64_t time, int64_t, const bool key, int)
                    {
                        if (key)
                            times.push_back(time * static_cast<int64_t>(facts.timestampScale));
                        return true;
                    });
    std::sort(times.begin(), times.end());
    return times;
}

std::string hexUuid(const std::string& uuid)
{
    static const char* DIGITS = "0123456789abcdef";
    std::string out;
    for (const char c : uuid)
    {
        out += DIGITS[static_cast<uint8_t>(c) >> 4];
        out += DIGITS[static_cast<uint8_t>(c) & 0x0F];
    }
    return out;
}
}  // namespace

AbstractDemuxer::SourceEditions MatroskaDemuxer::getSourceEditions()
{
    SourceEditions out;
    SegmentFacts facts;
    if (readSegmentFacts(m_sourceName, facts))
    {
        out.segmentUid = facts.uuid;
        out.prevUid = facts.prevUuid;
        out.nextUid = facts.nextUuid;
    }
    File f;
    if (!f.open(m_sourceName.c_str(), File::ofRead))
        return out;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (findTopLevelElement(scan, fileSize, MATROSKA_ID_CHAPTERS, body, size) && size > 0 && size < (64 << 20))
    {
        out.chapters.resize(static_cast<size_t>(size));
        f.seek(body);
        if (f.read(out.chapters.data(), static_cast<uint32_t>(size)) != static_cast<int>(size))
            out.chapters.clear();
    }
    return out;
}

std::vector<MatroskaPlayPart> MatroskaDemuxer::playTimeline(const std::string& file, std::string& how)
{
    std::vector<MatroskaPlayPart> parts;
    SegmentFacts first;
    if (!readSegmentFacts(file, first))
        return parts;
    const SegmentFacts::Edition* edition = first.defaultEdition();
    const bool ordered = edition && edition->ordered && !edition->atoms.empty();
    // Hard linking (17.1): only from the first file of the chain, and an ordered edition takes precedence.
    const bool linked = !ordered && !first.nextUuid.empty() && first.prevUuid.empty();
    if (!ordered && !linked)
        return parts;

    // The other segments of the folder, by their SegmentUUID.
    std::map<std::string, std::string> byUuid;
    std::map<std::string, SegmentFacts> facts;
    facts[file] = first;
    {
        std::string dir = extractFileDir(file);
        if (dir.empty())
            dir = ".";
        while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        std::vector<std::string> names;
        for (const char* mask : {"*.mkv", "*.mka", "*.mks", "*.mk3d", "*.webm"}) findFiles(dir, mask, &names, true);
        for (const std::string& name : names)
        {
            SegmentFacts f;
            if (readSegmentFacts(name, f) && !f.uuid.empty())
            {
                byUuid.emplace(f.uuid, name);
                facts.emplace(name, f);
            }
        }
        if (!first.uuid.empty())
            byUuid[first.uuid] = file;
    }

    std::ostringstream text;
    if (linked)
    {
        // The chain, by NextUUID, and where a file has none, by the file whose PrevUUID names it.
        std::set<std::string> seen;
        std::string current = file;
        while (!current.empty() && seen.insert(current).second)
        {
            parts.push_back({current, JoinRange()});
            const SegmentFacts& here = facts[current];
            std::string next;
            if (!here.nextUuid.empty())
            {
                const auto found = byUuid.find(here.nextUuid);
                if (found != byUuid.end())
                    next = found->second;
                else
                    LTRACE(LT_WARN, 2,
                           "Warning: " << current << " is linked to a next segment (" << hexUuid(here.nextUuid)
                                       << ") that is not in its folder. The linked files are read up to it.");
            }
            else
                for (const auto& [name, f] : facts)
                    if (!here.uuid.empty() && f.prevUuid == here.uuid && name != current)
                        next = name;
            current = next;
        }
        if (parts.size() < 2)
            return {};
        text << "it is the first of " << parts.size() << " linked files, which are joined in their order";
        how = text.str();
        return parts;
    }

    // The ordered edition, a chapter from another segment as the chapter says, and a linked edition (17.2.2) as that
    // edition plays, one level deep.
    struct Wanted
    {
        std::string file;
        int64_t start, end;
        std::string name;
    };
    std::vector<Wanted> wanted;
    std::function<void(const std::string&, const SegmentFacts::Edition&, int)> expand =
        [&](const std::string& path, const SegmentFacts::Edition& ed, const int depth)
    {
        for (const SegmentFacts::Atom& atom : ed.atoms)
        {
            if (!atom.enabled)
                continue;
            std::string source = path;
            if (!atom.segmentUuid.empty() && atom.segmentUuid != facts[path].uuid)
            {
                const auto found = byUuid.find(atom.segmentUuid);
                if (found == byUuid.end())
                {
                    LTRACE(LT_WARN, 2,
                           "Warning: the chapter \"" << atom.name << "\" plays a segment (" << hexUuid(atom.segmentUuid)
                                                     << ") that is not in the folder of " << path
                                                     << ", so it is left out.");
                    continue;
                }
                source = found->second;
                if (atom.segmentEdition != 0 && depth < 3)
                {
                    const SegmentFacts& linkedFacts = facts[source];
                    if (const SegmentFacts::Edition* linkedEdition = linkedFacts.edition(atom.segmentEdition))
                    {
                        if (linkedEdition->ordered)
                            expand(source, *linkedEdition, depth + 1);
                        else
                            wanted.push_back({source, 0, INT64_MAX, atom.name});
                        continue;
                    }
                }
            }
            if (atom.start < 0)
                continue;
            wanted.push_back({source, atom.start, atom.end > atom.start ? atom.end : INT64_MAX, atom.name});
        }
    };
    expand(file, *edition, 0);

    // Each end to the nearest key frame; the parts placed one after the other.
    std::map<std::string, std::vector<int64_t>> keys;
    const auto nearestKey = [&](const std::string& path, const int64_t t)
    {
        auto& times = keys[path];
        if (times.empty())
            times = keyFrameTimesNs(path, facts[path]);
        if (times.empty() || t == INT64_MAX)
            return t;
        const auto after = std::lower_bound(times.begin(), times.end(), t);
        if (after == times.begin())
            return *after;
        if (after == times.end())
        {
            // past the last key frame: the end of the file for an end, the last key frame for a start
            return t;
        }
        const int64_t before = *(after - 1);
        return t - before <= *after - t ? before : *after;
    };
    int64_t offset = 0;
    for (const Wanted& w : wanted)
    {
        const SegmentFacts& f = facts[w.file];
        int64_t start = nearestKey(w.file, w.start);
        int64_t end = nearestKey(w.file, w.end);
        if (f.durationNs > 0 && end >= f.durationNs)
            end = INT64_MAX;
        if (start == INT64_MAX || (end != INT64_MAX && end <= start))
            continue;
        const int64_t length = end != INT64_MAX ? end - start : (f.durationNs > start ? f.durationNs - start : 0);
        JoinRange range;
        range.startNs = start;
        range.endNs = end;
        range.offsetNs = offset;
        range.name = w.name;
        parts.push_back({w.file, range});
        offset += length;
    }
    if (parts.empty())
        return parts;
    std::set<std::string> files;
    for (const auto& p : parts) files.insert(p.file);
    text << "its default edition is ordered: " << parts.size() << (parts.size() == 1 ? " part" : " parts");
    if (files.size() > 1)
        text << " from " << files.size() << " files";
    text << ", " << floatToTime(static_cast<double>(offset) / 1e9) << " in all";
    how = text.str();
    return parts;
}

void MatroskaDemuxer::loadChapters(const std::string& fileName)
{
    File f;
    if (!f.open(fileName.c_str(), File::ofRead))
        return;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_CHAPTERS, body, size))
        return;

    const auto readUInt = [&](const int64_t at, const int64_t width) -> uint64_t
    {
        uint64_t out = 0;
        if (width <= 0 || width > 8)
            return 0;
        uint8_t buf[8] = {0};
        scan.f.seek(at);
        if (scan.f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return 0;
        for (int i = 0; i < static_cast<int>(width); ++i) out = (out << 8) | buf[i];
        return out;
    };
    const auto readString = [&](const int64_t at, const int64_t width) -> std::string
    {
        if (width <= 0 || width > 64 * 1024)
            return {};
        std::vector<char> buf(static_cast<size_t>(width) + 1, 0);
        scan.f.seek(at);
        if (scan.f.read(buf.data(), static_cast<uint32_t>(width)) != static_cast<int>(width))
            return {};
        return std::string(buf.data());  // a string element may end in zero bytes
    };

    // The editions, and the one to use.
    int64_t chosenBody = -1, chosenSize = 0;
    int64_t firstBody = -1, firstSize = 0;
    scan.pos = body;
    while (scan.pos < body + size)
    {
        uint32_t id = 0;
        int64_t len = 0, b = 0;
        if (!scan.readElement(id, len, b))
            break;
        if (id == MATROSKA_ID_EDITIONENTRY)
        {
            if (firstBody < 0)
            {
                firstBody = b;
                firstSize = len;
            }
            bool isDefault = false;
            scan.pos = b;
            while (scan.pos < b + len)
            {
                uint32_t eid = 0;
                int64_t esz = 0, eb = 0;
                if (!scan.readElement(eid, esz, eb))
                    break;
                if (eid == MATROSKA_ID_EDITIONFLAGDEFAULT)
                    isDefault = readUInt(eb, esz) != 0;
                scan.pos = eb + esz;
            }
            if (isDefault && chosenBody < 0)
            {
                chosenBody = b;
                chosenSize = len;
            }
        }
        scan.pos = b + len;
    }
    if (chosenBody < 0)
    {
        chosenBody = firstBody;
        chosenSize = firstSize;
    }
    if (chosenBody < 0)
        return;

    std::map<uint64_t, AVChapter> found;
    uint64_t noUid = 1ULL << 63;  // a key for a chapter without a ChapterUID, or with one already used
    // One ChapterAtom, and the ones nested in it.
    std::function<void(int64_t, int64_t)> readAtom = [&](const int64_t atomBody, const int64_t atomSize)
    {
        uint64_t uid = 0;
        int64_t start = -1;
        int64_t end = -1;
        bool hidden = false;
        bool enabled = true;
        std::vector<AVChapter::Name> names;
        std::vector<std::pair<int64_t, int64_t>> nested;
        scan.pos = atomBody;
        while (scan.pos < atomBody + atomSize)
        {
            uint32_t id = 0;
            int64_t len = 0, b = 0;
            if (!scan.readElement(id, len, b))
                break;
            if (id == MATROSKA_ID_CHAPTERUID)
                uid = readUInt(b, len);
            else if (id == MATROSKA_ID_CHAPTERTIMESTART)
                start = static_cast<int64_t>(readUInt(b, len));
            else if (id == MATROSKA_ID_CHAPTERTIMEEND)
                end = static_cast<int64_t>(readUInt(b, len));
            else if (id == MATROSKA_ID_CHAPTERFLAGHIDDEN)
                hidden = readUInt(b, len) != 0;
            else if (id == MATROSKA_ID_CHAPTERFLAGENABLED)
                enabled = readUInt(b, len) != 0;
            else if (id == MATROSKA_ID_CHAPTERATOM)
                nested.emplace_back(b, len);
            else if (id == MATROSKA_ID_CHAPTERDISPLAY)
            {
                AVChapter::Name name;
                scan.pos = b;
                while (scan.pos < b + len)
                {
                    uint32_t did = 0;
                    int64_t dlen = 0, db = 0;
                    if (!scan.readElement(did, dlen, db))
                        break;
                    if (did == MATROSKA_ID_CHAPSTRING)
                        name.title = readString(db, dlen);
                    else if (did == MATROSKA_ID_CHAPTERLANGUAGE && name.language.empty())
                        name.language = readString(db, dlen);  // the first of several: a title has one language
                    else if (did == MATROSKA_ID_CHAPLANGUAGEBCP47 && name.languageBCP47.empty())
                        name.languageBCP47 = readString(db, dlen);
                    scan.pos = db + dlen;
                }
                // As for a track, LanguageBCP47 rules where it is there (RFC 9559, 5.1.7.1.4.12).
                if (!name.languageBCP47.empty())
                    if (const std::string code = iso639FromBcp47(name.languageBCP47); !code.empty())
                        name.language = code;
                names.push_back(name);
            }
            scan.pos = b + len;
        }
        // A disabled chapter (ChapterFlagEnabled 0) is one a player skips, as it skips a
        // hidden one: it was taken as a chapter like any other, a mark on a disc and an enabled chapter in Matroska.
        if (start >= 0 && !hidden && enabled)
        {
            AVChapter chapter(start, names.empty() ? "" : names.front().title,
                              names.empty() ? "" : names.front().language);
            chapter.names = names;
            if (end > start)
                chapter.end = end;
            found[uid != 0 && found.find(uid) == found.end() ? uid : noUid++] = chapter;
        }
        for (const auto& [b, len] : nested) readAtom(b, len);
    };

    scan.pos = chosenBody;
    while (scan.pos < chosenBody + chosenSize)
    {
        uint32_t id = 0;
        int64_t len = 0, b = 0;
        if (!scan.readElement(id, len, b))
            break;
        if (id == MATROSKA_ID_CHAPTERATOM)
            readAtom(b, len);
        scan.pos = b + len;
    }
    chapters = std::move(found);
}

// What a Matroska source says about a track beyond what the stream itself carries, for the outputs that can keep it:
// its flags, its colour description and mastering metadata, and the size to show it at. All of it was parsed and
// dropped, or not parsed, so a remux lost the forced flag of a subtitle, the HDR metadata that lives only in the
// container (common in files from other muxers), and a display aspect ratio given by the container. Read on a handle
// of its own, keyed by track number, as strings the muxers take or leave:
//   srcflags    forced=1,hearing=1,visual=1,descriptions=1,original=1,commentary=1 (the flags present, with their
//   value) srccolour   m=,r=,t=,p=,cll=,fall= and rx= ry= gx= gy= bx= by= wx= wy= lmax= lmin= (the elements present)
//   srcdisplay  WIDTHxHEIGHT, when the file gives a display size: in pixels, or as a shape at the picture's height
void MatroskaDemuxer::loadTrackExtras(const std::string& fileName)
{
    m_trackExtras.clear();
    File f;
    if (!f.open(fileName.c_str(), File::ofRead))
        return;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_TRACKS, body, size))
        return;

    const auto readUInt = [&](const int64_t at, const int64_t width) -> uint64_t
    {
        uint64_t out = 0;
        if (width <= 0 || width > 8)
            return 0;
        uint8_t buf[8] = {0};
        scan.f.seek(at);
        if (scan.f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return 0;
        for (int i = 0; i < static_cast<int>(width); ++i) out = (out << 8) | buf[i];
        return out;
    };
    const auto readFloat = [&](const int64_t at, const int64_t width) -> double
    {
        uint8_t buf[8] = {0};
        if (width != 4 && width != 8)
            return 0;
        scan.f.seek(at);
        if (scan.f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return 0;
        uint64_t bits = 0;
        for (int i = 0; i < static_cast<int>(width); ++i) bits = (bits << 8) | buf[i];
        if (width == 4)
        {
            const auto b32 = static_cast<uint32_t>(bits);
            float v;
            memcpy(&v, &b32, 4);
            return v;
        }
        double v;
        memcpy(&v, &bits, 8);
        return v;
    };
    const auto toText = [](const double v)
    {
        std::ostringstream s;
        s.precision(10);
        s << v;
        return s.str();
    };
    // The elements of one master, as (id, body, size).
    const auto elementsOf = [&](const int64_t at, const int64_t len)
    {
        std::vector<std::tuple<uint32_t, int64_t, int64_t>> out;
        scan.pos = at;
        while (scan.pos < at + len)
        {
            uint32_t id = 0;
            int64_t sz = 0, b = 0;
            if (!scan.readElement(id, sz, b) || b + sz > at + len)
                break;
            out.emplace_back(id, b, sz);
            scan.pos = b + sz;
        }
        return out;
    };

    // The DiscardPadding of each track's LAST block, in nanoseconds: the samples at the end of an Opus track that
    // are only there to fill its last frame (RFC 9559, 5.1.3.5.7). Taken from the clusters from the last cue point
    // on, so only the end of the file is read; a file without Cues from the clusters of its last 16 MB. A remux dropped
    // it, and the track played that much longer than its source, 13.5 ms for a usual encoder.
    std::map<int64_t, int64_t> endPadding;
    {
        int64_t cuesBody = 0, cuesSize = 0, segmentData = 0;
        int64_t lastCluster = -1;
        if (findTopLevelElement(scan, fileSize, MATROSKA_ID_CUES, cuesBody, cuesSize, &segmentData))
            for (const auto& [pid, pb, plen] : elementsOf(cuesBody, cuesSize))
                for (const auto& [tid, tb, tlen] : elementsOf(pb, plen))
                    if (tid == MATROSKA_ID_CUETRACKPOSITION)
                        for (const auto& [kid, kb, klen] : elementsOf(tb, tlen))
                            if (kid == MATROSKA_ID_CUECLUSTERPOSITION)
                                lastCluster = std::max(lastCluster, static_cast<int64_t>(readUInt(kb, klen)));
        // A file without Cues, as a file written live has: the first cluster of its last 16 MB, found by its id and the
        // timestamp that opens every cluster.
        int64_t walkFrom = lastCluster >= 0 ? segmentData + lastCluster : -1;
        if (walkFrom < 0 && fileSize > 64)
        {
            const int64_t tail = (std::max)(int64_t{0}, fileSize - (int64_t{16} << 20));
            std::vector<uint8_t> buf(static_cast<size_t>(fileSize - tail));
            scan.f.seek(tail);
            if (scan.f.read(buf.data(), static_cast<uint32_t>(buf.size())) == static_cast<int>(buf.size()))
                for (size_t i = 0; i + 12 < buf.size() && walkFrom < 0; ++i)
                {
                    if (buf[i] != 0x1F || buf[i + 1] != 0x43 || buf[i + 2] != 0xB6 || buf[i + 3] != 0x75)
                        continue;
                    uint32_t id = 0;
                    int64_t len = 0, b = 0;
                    scan.pos = tail + static_cast<int64_t>(i);
                    uint32_t first = 0;
                    int64_t firstLen = 0, firstBody = 0;
                    if (scan.readElement(id, len, b) && scan.readElement(first, firstLen, firstBody) &&
                        first == MATROSKA_ID_CLUSTERTIMECODE && firstLen > 0 && firstLen <= 8)
                        walkFrom = tail + static_cast<int64_t>(i);
                }
        }
        // Where a cluster ends. A cluster written live has no size: it ends where the next top-level element begins.
        const auto clusterEnd = [&](const int64_t body, const int64_t len)
        {
            if (body + len <= fileSize)
                return body + len;
            scan.pos = body;
            while (scan.pos < fileSize)
            {
                const int64_t at = scan.pos;
                uint32_t id = 0;
                int64_t sz = 0, b = 0;
                if (!scan.readElement(id, sz, b))
                    return at;
                if (id == MATROSKA_ID_CLUSTER || id == MATROSKA_ID_CUES || id == MATROSKA_ID_TAGS ||
                    id == MATROSKA_ID_CHAPTERS || id == MATROSKA_ID_ATTACHMENTS || id == MATROSKA_ID_SEEKHEAD ||
                    b + sz > fileSize)
                    return at;
                scan.pos = b + sz;
            }
            return fileSize;
        };
        if (walkFrom >= 0)
        {
            scan.pos = walkFrom;
            while (scan.pos < fileSize)
            {
                uint32_t id = 0;
                int64_t len = 0, b = 0;
                if (!scan.readElement(id, len, b))
                    break;
                if (id != MATROSKA_ID_CLUSTER && b + len > fileSize)
                    break;
                const int64_t next = id == MATROSKA_ID_CLUSTER ? clusterEnd(b, len) : b + len;
                if (id == MATROSKA_ID_CLUSTER)
                    for (const auto& [cid, cb, clen] : elementsOf(b, next - b))
                    {
                        if (cid != MATROSKA_ID_SIMPLEBLOCK && cid != MATROSKA_ID_BLOCKGROUP)
                            continue;
                        int64_t blockAt = cb, padding = 0;
                        if (cid == MATROSKA_ID_BLOCKGROUP)
                            for (const auto& [gid, gb, glen] : elementsOf(cb, clen))
                            {
                                if (gid == MATROSKA_ID_BLOCK)
                                    blockAt = gb;
                                else if (gid == MATROSKA_ID_DISCARDPADDING && glen > 0 && glen <= 8)
                                {
                                    // a signed number: negative pads the start, which is not what this is about
                                    const uint64_t raw = readUInt(gb, glen);
                                    const bool negative = raw >> (glen * 8 - 1) & 1;
                                    padding = negative ? -1 : static_cast<int64_t>(raw);
                                }
                            }
                        scan.pos = blockAt;
                        int64_t trackNumber = 0;
                        int width = 0;
                        if (scan.readNumber(trackNumber, false, width))
                            endPadding[trackNumber] = padding;  // the last block of the track decides
                    }
                scan.pos = next;
            }
        }
    }

    for (const auto& [entryId, entryBody, entrySize] : elementsOf(body, size))
    {
        if (entryId != MATROSKA_ID_TRACKENTRY)
            continue;
        int64_t number = -1;
        std::string flags;
        std::string colour;
        std::string display;
        std::string crop, fieldOrder, projection;
        bool rightEyeFirst = false;
        bool encrypted = false;
        for (const auto& [id, b, len] : elementsOf(entryBody, entrySize))
        {
            // ContentEncodings > ContentEncoding > ContentEncryption, or a ContentEncodingType of 1 (encryption)
            if (id == MATROSKA_ID_TRACKCONTENTENCODINGS)
                for (const auto& [eid, eb, elen] : elementsOf(b, len))
                    if (eid == MATROSKA_ID_TRACKCONTENTENCODING)
                        for (const auto& [cid, cb, clen] : elementsOf(eb, elen))
                            if (cid == 0x5035 || (cid == 0x5033 && readUInt(cb, clen) == 1))
                                encrypted = true;
            static const std::pair<uint32_t, const char*> FLAGS[] = {{0x55AA, "forced"},   {0x55AB, "hearing"},
                                                                     {0x55AC, "visual"},   {0x55AD, "descriptions"},
                                                                     {0x55AE, "original"}, {0x55AF, "commentary"}};
            if (id == MATROSKA_ID_TRACKNUMBER)
                number = static_cast<int64_t>(readUInt(b, len));
            for (const auto& [flagId, flagName] : FLAGS)
                if (id == flagId)
                    flags += std::string(flags.empty() ? "" : ",") + flagName + "=" + (readUInt(b, len) ? "1" : "0");
            if (id != MATROSKA_ID_TRACKVIDEO)
                continue;
            uint64_t pixelWidth = 0, pixelHeight = 0, displayWidth = 0, displayHeight = 0, displayUnit = 0;
            uint64_t cropTop = 0, cropBottom = 0, cropLeft = 0, cropRight = 0;
            for (const auto& [vid, vb, vlen] : elementsOf(b, len))
            {
                // The crop, the field order and the projection (which holds the rotation), carried over to a
                // Matroska output; they were read past (RFC 9559, 5.1.4.1.28).
                if (vid == MATROSKA_ID_VIDEOPIXELCROPTOP)
                    cropTop = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOPIXELCROPBOTTOM)
                    cropBottom = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOPIXELCROPLEFT)
                    cropLeft = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOPIXELCROPRIGHT)
                    cropRight = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOFIELDORDER)
                    fieldOrder = std::to_string(readUInt(vb, vlen));
                // A 3D track that says its base view is the right eye: a remux wrote it as left first, which swaps
                // the eyes, because only the old stereo element was read
                else if (vid == MATROSKA_ID_VIDEOSTEREOMODE_CURRENT &&
                         readUInt(vb, vlen) == MATROSKA_STEREO_BOTH_EYES_RIGHT_FIRST)
                    rightEyeFirst = true;
                else if (vid == MATROSKA_ID_VIDEOPROJECTION && vlen > 0 && vlen <= 256)
                {
                    // the whole element body as it stands, as hex
                    std::vector<uint8_t> raw(static_cast<size_t>(vlen));
                    scan.f.seek(vb);
                    if (scan.f.read(raw.data(), static_cast<uint32_t>(vlen)) == static_cast<int>(vlen))
                    {
                        static const char* HEX = "0123456789abcdef";
                        for (const uint8_t c : raw)
                        {
                            projection += HEX[c >> 4];
                            projection += HEX[c & 15];
                        }
                    }
                }
                if (vid == MATROSKA_ID_VIDEOPIXELWIDTH)
                    pixelWidth = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOPIXELHEIGHT)
                    pixelHeight = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEODISPLAYWIDTH)
                    displayWidth = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEODISPLAYHEIGHT)
                    displayHeight = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEODISPLAYUNIT)
                    displayUnit = readUInt(vb, vlen);
                else if (vid == MATROSKA_ID_VIDEOCOLOUR)
                {
                    static const std::pair<uint32_t, const char*> UINTS[] = {
                        {MATROSKA_ID_COLOURMATRIXCOEFF, "m"},       {MATROSKA_ID_COLOURRANGE, "r"},
                        {MATROSKA_ID_COLOURTRANSFERCHARACTER, "t"}, {MATROSKA_ID_COLOURPRIMARIES, "p"},
                        {MATROSKA_ID_COLOURMAXCLL, "cll"},          {MATROSKA_ID_COLOURMAXFALL, "fall"}};
                    static const std::pair<uint32_t, const char*> FLOATS[] = {
                        {MATROSKA_ID_PRIMARYRCHROMATICITYX, "rx"},   {MATROSKA_ID_PRIMARYRCHROMATICITYY, "ry"},
                        {MATROSKA_ID_PRIMARYGCHROMATICITYX, "gx"},   {MATROSKA_ID_PRIMARYGCHROMATICITYY, "gy"},
                        {MATROSKA_ID_PRIMARYBCHROMATICITYX, "bx"},   {MATROSKA_ID_PRIMARYBCHROMATICITYY, "by"},
                        {MATROSKA_ID_WHITEPOINTCHROMATICITYX, "wx"}, {MATROSKA_ID_WHITEPOINTCHROMATICITYY, "wy"},
                        {MATROSKA_ID_LUMINANCEMAX, "lmax"},          {MATROSKA_ID_LUMINANCEMIN, "lmin"}};
                    for (const auto& [cid, cb, clen] : elementsOf(vb, vlen))
                    {
                        for (const auto& [uid, key] : UINTS)
                            if (cid == uid)
                                colour += std::string(colour.empty() ? "" : ";") + key + "=" +
                                          std::to_string(readUInt(cb, clen));
                        if (cid != MATROSKA_ID_MASTERINGMETADATA)
                            continue;
                        for (const auto& [mid, mb, mlen] : elementsOf(cb, clen))
                            for (const auto& [fid, key] : FLOATS)
                                if (mid == fid)
                                    colour += std::string(colour.empty() ? "" : ";") + key + "=" +
                                              toText(readFloat(mb, mlen));
                    }
                }
            }
            // Also when it equals the picture's size. That is a file saying "square pixels" over a stream that says
            // otherwise, and in Matroska the file's display size is the one shown: it was left out, and the output
            // then showed the stream's shape, a 720x576 file made to be shown at 5:4 came out 16:9.
            if (displayUnit == 0 && displayWidth && displayHeight)
                display = std::to_string(displayWidth) + "x" + std::to_string(displayHeight);
            // In centimetres, inches or as an aspect ratio (DisplayUnit 1, 2 and 3), the two numbers give the shape
            // only, and a current ffmpeg writes the ratio. It was left out, so such a file's shape was lost the same
            // way. It goes on as the display size at the picture's height.
            else if (displayUnit >= 1 && displayUnit <= 3 && displayWidth && displayHeight && pixelHeight)
                display = std::to_string((pixelHeight * displayWidth + displayHeight / 2) / displayHeight) + "x" +
                          std::to_string(pixelHeight);
            if (cropTop || cropBottom || cropLeft || cropRight)
                crop = std::to_string(cropTop) + "," + std::to_string(cropBottom) + "," + std::to_string(cropLeft) +
                       "," + std::to_string(cropRight);
        }
        if (number < 0)
            continue;
        auto& extras = m_trackExtras[number];
        if (encrypted)
            extras["encrypted"] = "1";
        if (const auto pad = endPadding.find(number); pad != endPadding.end() && pad->second > 0)
            extras["srcendpadding"] = std::to_string(pad->second);
        if (!flags.empty())
            extras["srcflags"] = flags;
        if (!colour.empty())
            extras["srccolour"] = colour;
        if (!display.empty())
            extras["srcdisplay"] = display;
        if (!crop.empty())
            extras["srccrop"] = crop;
        if (!fieldOrder.empty())
            extras["srcfieldorder"] = fieldOrder;
        if (!projection.empty())
            extras["srcprojection"] = projection;
        if (rightEyeFirst)
            extras["srcrighteye"] = "1";
    }
}

// The source's attachments, each a whole AttachedFile element as it stands in the file, for a Matroska output to
// carry: cover art, the fonts of styled subtitles and the like, which a remux used to drop. tsMuxeR's own Dolby Vision
// files are left out: a profile 8.1 output writes them again from the stream, and anything else has no use for them.
std::vector<std::vector<uint8_t>> MatroskaDemuxer::getPassThroughAttachments()
{
    std::vector<std::vector<uint8_t>> out;
    if (m_sourceName.empty())
        return out;
    File f;
    if (!f.open(m_sourceName.c_str(), File::ofRead))
        return out;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_ATTACHMENTS, body, size))
        return out;
    scan.pos = body;
    while (scan.pos < body + size)
    {
        const int64_t start = scan.pos;
        uint32_t id = 0;
        int64_t len = 0, b = 0;
        if (!scan.readElement(id, len, b) || b + len > body + size)
            break;
        scan.pos = b + len;
        if (id != MATROSKA_ID_ATTACHEDFILE)
            continue;
        std::string name;
        int64_t dataPos = 0, dataSize = 0;
        const int64_t resume = scan.pos;
        scan.pos = b;
        scanAttachedFile(scan, b + len, name, dataPos, dataSize);
        scan.pos = resume;
        if (name == MANIFEST_ATTACHMENT_NAME || name == "dv-original-rpu.bin" || name == "dv-original-rpu-pts.bin")
            continue;
        std::vector<uint8_t> raw(static_cast<size_t>(b + len - start));
        f.seek(start);
        if (f.read(raw.data(), static_cast<uint32_t>(raw.size())) == static_cast<int>(raw.size()))
            out.push_back(std::move(raw));
        scan.pos = resume;
    }
    return out;
}

// The source's tags that are about the whole file (a Targets with no track, edition, chapter or attachment named):
// the title, the release date and the like, each a whole Tag element. A tag about a track is left behind, because the
// output's tracks have other UIDs, and statistics such as a track's bit rate would be wrong for the output anyway.
std::vector<std::vector<uint8_t>> MatroskaDemuxer::getGlobalTags()
{
    std::vector<std::vector<uint8_t>> out;
    if (m_sourceName.empty())
        return out;
    File f;
    if (!f.open(m_sourceName.c_str(), File::ofRead))
        return out;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_TAGS, body, size))
        return out;
    const auto nonZero = [&](const int64_t at, const int64_t width)
    {
        uint8_t buf[8] = {0};
        if (width <= 0 || width > 8)
            return false;
        f.seek(at);
        if (f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return false;
        for (int i = 0; i < static_cast<int>(width); ++i)
            if (buf[i])
                return true;
        return false;
    };
    scan.pos = body;
    while (scan.pos < body + size)
    {
        const int64_t start = scan.pos;
        uint32_t id = 0;
        int64_t len = 0, b = 0;
        if (!scan.readElement(id, len, b) || b + len > body + size)
            break;
        const int64_t resume = b + len;
        if (id == MATROSKA_ID_TAG)
        {
            bool aboutPart = false;
            scan.pos = b;
            while (scan.pos < b + len)
            {
                uint32_t tid = 0;
                int64_t tlen = 0, tb = 0;
                if (!scan.readElement(tid, tlen, tb))
                    break;
                if (tid == MATROSKA_ID_TAGTARGETS)
                {
                    scan.pos = tb;
                    while (scan.pos < tb + tlen)
                    {
                        uint32_t uid = 0;
                        int64_t ulen = 0, ub = 0;
                        if (!scan.readElement(uid, ulen, ub))
                            break;
                        if ((uid == 0x63C5 || uid == 0x63C9 || uid == 0x63C4 || uid == 0x63C6) && nonZero(ub, ulen))
                            aboutPart = true;  // TagTrackUID, TagEditionUID, TagChapterUID, TagAttachmentUID
                        scan.pos = ub + ulen;
                    }
                }
                scan.pos = tb + tlen;
            }
            if (!aboutPart)
            {
                std::vector<uint8_t> raw(static_cast<size_t>(resume - start));
                f.seek(start);
                if (f.read(raw.data(), static_cast<uint32_t>(raw.size())) == static_cast<int>(raw.size()))
                    out.push_back(std::move(raw));
            }
        }
        scan.pos = resume;
    }
    return out;
}

// The tags about one track, read from the Tags element. Kept apart from getGlobalTags because a Matroska output gives
// its tracks new UIDs: the muxer writes each of these again with the UID of the track it now belongs to.
std::map<uint64_t, std::vector<AbstractDemuxer::SourceTrackTag>> MatroskaDemuxer::getTrackTags()
{
    std::map<uint64_t, std::vector<SourceTrackTag>> out;
    if (m_sourceName.empty())
        return out;
    File f;
    if (!f.open(m_sourceName.c_str(), File::ofRead))
        return out;
    const int64_t fileSize = f.size();
    EbmlScan scan(f);
    int64_t body = 0, size = 0;
    if (!findTopLevelElement(scan, fileSize, MATROSKA_ID_TAGS, body, size) || size <= 0 || size > (64 << 20))
        return out;
    std::vector<uint8_t> d(static_cast<size_t>(size));
    f.seek(body);
    if (f.read(d.data(), static_cast<uint32_t>(d.size())) != static_cast<int>(d.size()))
        return out;

    // One element at i: its id (marker kept), where its body starts and where it ends; false at the end or on damage
    const auto element = [&d](size_t& i, const size_t end, uint32_t& id, size_t& b, size_t& e)
    {
        const auto number = [&d, end](size_t& at, const bool keep, uint64_t& v)
        {
            if (at >= end || d[at] == 0)
                return false;
            int width = 1;
            uint8_t mask = 0x80;
            while (!(d[at] & mask))
            {
                mask >>= 1;
                ++width;
            }
            if (at + width > end)
                return false;
            v = keep ? d[at] : d[at] & (mask - 1);
            for (int k = 1; k < width; ++k) v = v << 8 | d[at + k];
            at += width;
            return true;
        };
        uint64_t idv = 0, len = 0;
        size_t at = i;
        if (!number(at, true, idv) || !number(at, false, len) || len > end - at)
            return false;
        id = static_cast<uint32_t>(idv);
        b = at;
        e = at + static_cast<size_t>(len);
        i = e;
        return true;
    };
    const auto uintOf = [&d](const size_t b, const size_t e)
    {
        uint64_t v = 0;
        for (size_t k = b; k < e && k < b + 8; ++k) v = v << 8 | d[k];
        return v;
    };

    size_t i = 0;
    uint32_t id = 0;
    size_t b = 0, e = 0;
    while (element(i, d.size(), id, b, e))
    {
        if (id != MATROSKA_ID_TAG)
            continue;
        std::vector<uint64_t> trackUids;
        bool aboutOther = false;
        SourceTrackTag tag;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> simple;  // name, the whole element
        std::string statistics;
        size_t j = b;
        uint32_t cid = 0;
        size_t cb = 0, ce = 0;
        while (element(j, e, cid, cb, ce))
        {
            if (cid == MATROSKA_ID_TAGTARGETS)
            {
                size_t k = cb;
                uint32_t tid = 0;
                size_t tb = 0, te = 0;
                size_t tstart = k;
                while (element(k, ce, tid, tb, te))
                {
                    const uint64_t value = uintOf(tb, te);
                    if (tid == MATROSKA_ID_TAGTARGETS_TRACKUID)
                    {
                        if (value)
                            trackUids.push_back(value);
                    }
                    else
                    {
                        if ((tid == 0x63C9 || tid == 0x63C4 || tid == 0x63C6) && value)
                            aboutOther = true;  // TagEditionUID, TagChapterUID, TagAttachmentUID
                        tag.targetsRest.insert(tag.targetsRest.end(), d.begin() + static_cast<std::ptrdiff_t>(tstart),
                                               d.begin() + static_cast<std::ptrdiff_t>(te));
                    }
                    tstart = k;
                }
            }
            else if (cid == MATROSKA_ID_SIMPLETAG)
            {
                std::string name, value;
                size_t k = cb;
                uint32_t sid = 0;
                size_t sb = 0, se = 0;
                while (element(k, ce, sid, sb, se))
                    if (sid == MATROSKA_ID_TAGNAME)
                        name.assign(d.begin() + static_cast<std::ptrdiff_t>(sb),
                                    d.begin() + static_cast<std::ptrdiff_t>(se));
                    else if (sid == MATROSKA_ID_TAGSTRING)
                        value.assign(d.begin() + static_cast<std::ptrdiff_t>(sb),
                                     d.begin() + static_cast<std::ptrdiff_t>(se));
                if (name == "_STATISTICS_TAGS")
                    statistics = " " + value + " ";
            }
        }
        if (trackUids.empty() || aboutOther)
            continue;
        // The SimpleTags again, now that the list of statistics tags is known, each as it stands
        j = b;
        size_t start = j;
        while (element(j, e, cid, cb, ce))
        {
            if (cid == MATROSKA_ID_SIMPLETAG)
            {
                std::string name;
                size_t k = cb;
                uint32_t sid = 0;
                size_t sb = 0, se = 0;
                while (element(k, ce, sid, sb, se))
                    if (sid == MATROSKA_ID_TAGNAME)
                        name.assign(d.begin() + static_cast<std::ptrdiff_t>(sb),
                                    d.begin() + static_cast<std::ptrdiff_t>(se));
                // Also the statistics a writer leaves without naming them in _STATISTICS_TAGS, as ffmpeg writes a
                // DURATION for each track: kept, it gave the source's length to a cut output.
                const bool isStatistics = name.rfind("_STATISTICS_", 0) == 0 ||
                                          (!name.empty() && statistics.find(" " + name + " ") != std::string::npos) ||
                                          name == "DURATION" || name == "BPS" || name == "NUMBER_OF_FRAMES" ||
                                          name == "NUMBER_OF_BYTES";
                if (!isStatistics)
                    tag.simpleTags.insert(tag.simpleTags.end(), d.begin() + static_cast<std::ptrdiff_t>(start),
                                          d.begin() + static_cast<std::ptrdiff_t>(ce));
            }
            start = j;
        }
        if (tag.simpleTags.empty())
            continue;
        for (const uint64_t uid : trackUids) out[uid].push_back(tag);
    }
    return out;
}

void MatroskaDemuxer::loadAttachments(const std::string& fileName)
{
    File f;
    if (!f.open(fileName.c_str(), File::ofRead))
        return;

    const int64_t fileSize = f.size();
    EbmlScan scan(f);

    // Every AttachedFile inside one Attachments element.
    auto readAttachments = [&](const int64_t body, const int64_t size)
    {
        const int64_t attEnd = body + size;
        scan.pos = body;
        while (scan.pos < attEnd)
        {
            uint32_t fileId = 0;
            int64_t fileSizeField = 0, fileBody = 0;
            if (!scan.readElement(fileId, fileSizeField, fileBody))
                break;
            if (fileId == MATROSKA_ID_ATTACHEDFILE)
            {
                MatroskaAttachment att;
                const int64_t save = scan.pos;
                scan.pos = fileBody;
                if (scanAttachedFile(scan, fileBody + fileSizeField, att.name, att.dataPos, att.dataSize) &&
                    !att.name.empty() && att.dataSize > 0)
                    m_attachments.push_back(att);
                scan.pos = save;
            }
            scan.pos = fileBody + fileSizeField;
        }
    };

    // A fixed width big endian number out of an element's body.
    auto readUInt = [&](const int64_t at, const int64_t width, uint64_t& out) -> bool
    {
        if (width <= 0 || width > 8)
            return false;
        uint8_t buf[8] = {0};
        scan.f.seek(at);
        if (scan.f.read(buf, static_cast<uint32_t>(width)) != static_cast<int>(width))
            return false;
        out = 0;
        for (int i = 0; i < static_cast<int>(width); ++i) out = (out << 8) | buf[i];
        return true;
    };

    // Read one SeekHead. Returns where it says the attachments are, as an offset from the start of
    // the segment's data, or 0 if it does not name them. A writer may keep the front index small and
    // point at a fuller one elsewhere, so a SeekHead naming another SeekHead is reported separately.
    auto readSeekHead = [&](const int64_t body, const int64_t size, int64_t& nextSeekHead) -> int64_t
    {
        int64_t attAt = 0;
        nextSeekHead = 0;
        const int64_t end = body + size;
        scan.pos = body;
        while (scan.pos < end)
        {
            uint32_t id = 0;
            int64_t sz = 0, b = 0;
            if (!scan.readElement(id, sz, b))
                break;
            if (id == MATROSKA_ID_SEEKENTRY)
            {
                uint64_t seekId = 0, seekPos = 0;
                bool haveId = false, havePos = false;
                const int64_t entryEnd = b + sz;
                const int64_t resume = b + sz;
                scan.pos = b;
                while (scan.pos < entryEnd)
                {
                    uint32_t eid = 0;
                    int64_t esz = 0, eb = 0;
                    if (!scan.readElement(eid, esz, eb))
                        break;
                    // A SeekID carries the element id with its marker bit, which is how the
                    // MATROSKA_ID_ constants are written, so the two compare directly.
                    if (eid == MATROSKA_ID_SEEKID)
                        haveId = readUInt(eb, esz, seekId);
                    else if (eid == MATROSKA_ID_SEEKPOSITION)
                        havePos = readUInt(eb, esz, seekPos);
                    scan.pos = eb + esz;
                }
                if (haveId && havePos)
                {
                    if (seekId == MATROSKA_ID_ATTACHMENTS)
                        attAt = static_cast<int64_t>(seekPos);
                    else if (seekId == MATROSKA_ID_SEEKHEAD)
                        nextSeekHead = static_cast<int64_t>(seekPos);
                }
                scan.pos = resume;
                continue;
            }
            scan.pos = b + sz;
        }
        return attAt;
    };

    // Where the segment's data begins. Every SeekPosition is measured from there.
    int64_t segmentData = -1;
    int64_t segmentEnd = fileSize;
    while (scan.pos < fileSize)
    {
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (!scan.readElement(id, size, body))
            break;
        if (id == MATROSKA_ID_SEGMENT)
        {
            segmentData = body;
            segmentEnd = (size > 0 && body + size <= fileSize) ? body + size : fileSize;
            break;
        }
        scan.pos = body + size;
    }

    bool sawSeekHead = false;
    int64_t attAt = 0;

    if (segmentData >= 0)
    {
        // Only the head of the segment, as far as the first cluster. Everything that says where
        // things are lives before the media, so this stops the moment the media starts.
        int64_t nextSeekHead = 0;
        scan.pos = segmentData;
        while (scan.pos < segmentEnd)
        {
            uint32_t id = 0;
            int64_t size = 0, body = 0;
            if (!scan.readElement(id, size, body))
                break;
            if (id == MATROSKA_ID_CLUSTER)
                break;
            if (id == MATROSKA_ID_SEEKHEAD)
            {
                sawSeekHead = true;
                int64_t nested = 0;
                const int64_t at = readSeekHead(body, size, nested);
                if (at > 0)
                    attAt = at;
                if (nested > 0)
                    nextSeekHead = nested;
            }
            else if (id == MATROSKA_ID_ATTACHMENTS)
            {
                // Written before the media, so it is already here.
                readAttachments(body, size);
                f.close();
                attachments_parsed = true;
                m_attachmentSource = fileName;
                return;
            }
            scan.pos = body + size;
        }

        // One level of "the index you want is over there".
        if (attAt == 0 && nextSeekHead > 0)
        {
            scan.pos = segmentData + nextSeekHead;
            uint32_t id = 0;
            int64_t size = 0, body = 0;
            if (scan.readElement(id, size, body) && id == MATROSKA_ID_SEEKHEAD)
            {
                int64_t ignored = 0;
                attAt = readSeekHead(body, size, ignored);
            }
        }
    }

    if (attAt > 0)
    {
        scan.pos = segmentData + attAt;
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (scan.readElement(id, size, body) && id == MATROSKA_ID_ATTACHMENTS)
            readAttachments(body, size);
    }
    else if (!sawSeekHead)
    {
        // No index at all. Nothing for it but the old walk, stepping over clusters by their
        // declared size. Rare, and the only case that still pays for it.
        int64_t segEnd = segmentEnd;
        bool inSegment = false;
        scan.pos = 0;
        while (scan.pos < fileSize)
        {
            uint32_t id = 0;
            int64_t size = 0, body = 0;
            if (!scan.readElement(id, size, body))
                break;

            if (id == MATROSKA_ID_SEGMENT)
            {
                inSegment = true;
                segEnd = (size > 0 && body + size <= fileSize) ? body + size : fileSize;
                scan.pos = body;
                continue;
            }

            if (id == MATROSKA_ID_ATTACHMENTS)
            {
                readAttachments(body, size);
                scan.pos = body + size;
                continue;
            }

            scan.pos = body + size;
            if (inSegment && scan.pos >= segEnd)
                break;
        }
    }

    f.close();
    attachments_parsed = true;
    m_attachmentSource = fileName;
}

// --------------------------- where the video starts and ends ---------------------------
//
// A join ("a.mkv"+"b.mkv") puts the next file where the VIDEO of the file before it ends. The video reader counts
// frames, so the next file's first picture follows the last picture of the one before, whatever else that file
// carries. Its Duration is where its longest track ends, and on a DVD rip that is often a subtitle: on one joined
// here, the second part's pictures sit 1.03 s before the place its Duration gives, measured frame by frame on the
// output. METADemuxer::getChapters places the chapters of each part from these two numbers, and the listing prints
// them for the window, which does the same.
//
// Neither needs the media read. The start is the earliest timestamp among the first group of pictures, which is not
// always the first block: an open group begins with an I-frame that is shown after the B-frames stored behind it.
// The end is the latest time a picture is still shown. The Cues say where the cluster of the last keyframe is, and
// only the clusters from there on are read, a few hundred KB on the files measured. A file without Cues gives false,
// and the callers keep to its Duration, as before.
//
// On a handle of its own, for the reason given for the attachments above.
bool MatroskaDemuxer::getVideoSpanNano(int64_t& startNano, int64_t& endNano)
{
    static constexpr int64_t MAX_CUES_SIZE = 64 * 1024 * 1024;
    static constexpr int FIRST_GROUP_BLOCKS = 64;
    // The first group is in the first cluster or two. The bound keeps a track that hardly ever has a picture from
    // making this walk the whole file.
    static constexpr int FIRST_GROUP_CLUSTERS = 16;

    const MatroskaTrack* video = nullptr;
    for (int i = 0; i < num_tracks && video == nullptr; i++)
        if (tracks[i]->type == MatroskaTrackType::VIDEO)
            video = tracks[i];
    File f;
    if (video == nullptr || m_sourceName.empty() || !f.open(m_sourceName.c_str(), File::ofRead))
        return false;
    const int64_t fileSize = f.size();
    const auto scale = static_cast<int64_t>(time_scale > 0 ? time_scale : 1000000);  // nanoseconds per unit
    EbmlScan scan(f);

    // The segment, and in its head where the Cues are and where the first cluster begins.
    int64_t segmentData = -1;
    int64_t segmentEnd = fileSize;
    while (segmentData < 0 && scan.pos < fileSize)
    {
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (!scan.readElement(id, size, body))
            return false;
        if (id == MATROSKA_ID_SEGMENT)
        {
            segmentData = body;
            segmentEnd = (size > 0 && size <= fileSize - body) ? body + size : fileSize;
        }
        scan.pos = body + size;
    }
    int64_t cues = -1, nextSeekHead = -1, firstCluster = -1;
    scan.pos = segmentData;
    while (segmentData >= 0 && scan.pos < segmentEnd)
    {
        const int64_t at = scan.pos;
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (!scan.readElement(id, size, body))
            break;
        if (id == MATROSKA_ID_CLUSTER)
        {
            firstCluster = at;
            break;
        }
        if (id == MATROSKA_ID_CUES)
            cues = at - segmentData;
        else if (id == MATROSKA_ID_SEEKHEAD)
            readSeekPositions(scan, body, size, cues, nextSeekHead);
        scan.pos = body + size;
    }
    if (cues < 0 && nextSeekHead > 0)  // one level of "the index you want is over there", as for the attachments
    {
        scan.pos = segmentData + nextSeekHead;
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        int64_t ignored = -1;
        if (scan.readElement(id, size, body) && id == MATROSKA_ID_SEEKHEAD)
            readSeekPositions(scan, body, size, cues, ignored);
    }
    if (firstCluster < 0 || cues < 0)
        return false;

    // The start: the earliest picture of the first group, up to the second keyframe.
    int64_t start = 0;
    int blocks = 0, keyframes = 0;
    walkTrackBlocks(scan, firstCluster, segmentEnd, FIRST_GROUP_CLUSTERS, video->num,
                    [&](const int64_t time, int64_t, const bool key, int)
                    {
                        if (key && ++keyframes == 2)
                            return false;
                        if (blocks == 0 || time * scale < start)
                            start = time * scale;
                        return ++blocks < FIRST_GROUP_BLOCKS;
                    });

    // The cluster of the last keyframe the Cues give for the video track.
    std::vector<uint8_t> index;
    {
        scan.pos = segmentData + cues;
        uint32_t id = 0;
        int64_t size = 0, body = 0;
        if (blocks == 0 || !scan.readElement(id, size, body) || id != MATROSKA_ID_CUES || size <= 0 ||
            size > MAX_CUES_SIZE || size > segmentEnd - body)
            return false;
        index.resize(static_cast<size_t>(size));
        f.seek(body);
        if (f.read(index.data(), static_cast<uint32_t>(size)) != static_cast<int>(size))
            return false;
    }
    int64_t lastKeyCluster = -1;
    EbmlMem cuesBody{index.data(), index.data() + index.size()};
    uint32_t pointId = 0;
    int64_t pointSize = 0;
    while (cuesBody.element(pointId, pointSize))
    {
        EbmlMem point{cuesBody.p, cuesBody.p + pointSize};
        cuesBody.p += pointSize;
        uint32_t positionsId = 0;
        int64_t positionsSize = 0;
        while (pointId == MATROSKA_ID_POINTENTRY && point.element(positionsId, positionsSize))
        {
            EbmlMem positions{point.p, point.p + positionsSize};
            point.p += positionsSize;
            int64_t track = -1, cluster = -1;
            uint32_t fieldId = 0;
            int64_t fieldSize = 0;
            while (positionsId == MATROSKA_ID_CUETRACKPOSITION && positions.element(fieldId, fieldSize))
            {
                if (fieldId == MATROSKA_ID_CUETRACK)
                    track = static_cast<int64_t>(positions.uintBody(fieldSize));
                else if (fieldId == MATROSKA_ID_CUECLUSTERPOSITION)
                    cluster = static_cast<int64_t>(positions.uintBody(fieldSize));
                positions.p += fieldSize;
            }
            if (track == video->num && cluster > lastKeyCluster)
                lastKeyCluster = cluster;
        }
    }
    if (lastKeyCluster < 0)
        return false;

    // The end: from that cluster on, the latest time a picture is still shown. A block without a BlockDuration lasts
    // the track's DefaultDuration; where there is neither, the shortest step between two pictures stands in.
    std::vector<std::pair<int64_t, int64_t>> pictures;  // timestamp and duration in nanoseconds, -1 when not known
    walkTrackBlocks(scan, segmentData + lastKeyCluster, segmentEnd, INT_MAX, video->num,
                    [&](const int64_t time, const int64_t duration, bool, const int laces)
                    {
                        int64_t lasts = -1;
                        if (duration >= 0)
                            lasts = duration * scale;
                        else if (video->default_duration > 0)
                            lasts = static_cast<int64_t>(video->default_duration) * laces;
                        pictures.emplace_back(time * scale, lasts);
                        return true;
                    });
    if (pictures.empty())
        return false;
    std::vector<int64_t> times;
    for (const auto& picture : pictures) times.push_back(picture.first);
    std::sort(times.begin(), times.end());
    int64_t step = 0;
    for (size_t i = 1; i < times.size(); ++i)
        if (times[i] > times[i - 1] && (step == 0 || times[i] - times[i - 1] < step))
            step = times[i] - times[i - 1];
    int64_t end = 0;
    for (const auto& [time, lasts] : pictures)
        if (time + (lasts >= 0 ? lasts : step) > end)
            end = time + (lasts >= 0 ? lasts : step);
    if (end <= start)
        return false;
    startNano = start > 0 ? start : 0;
    endNano = end;
    return true;
}

/* Seek to a given offset.
 * 0 is success, -1 is failure. */
int MatroskaDemuxer::ebml_read_seek(const int64_t offset)
{
    /* clear ID cache, if any */
    peek_id = 0;
    return (url_fseek(offset)) ? 0 : -1;
}

int MatroskaDemuxer::matroska_parse_tracks()
{
    int res = 0;
    uint32_t id;

    // av_log(matroska->ctx, AV_LOG_DEBUG, "parsing tracks...\n");

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* one track within the "all-tracks" header */
        case MATROSKA_ID_TRACKENTRY:
            res = matroska_add_stream();
            break;
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown entry " << id << " in track header");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    return res;
}

int MatroskaDemuxer::readEncodingCompression(MatroskaTrack* track)
{
    int res;
    uint32_t id;

    if ((res = ebml_read_master(&id)) < 0)
        return res;

    track->encodingAlgo = 0;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            break;
        }
        if (level_up > 0)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        case MATROSKA_ID_ENCODINGCOMPALGO:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            track->encodingAlgo = static_cast<int32_t>(num);
            break;
        }
        case MATROSKA_ID_ENCODINGCOMPSETTINGS:
        {
            uint8_t* data;
            int size;
            if ((res = ebml_read_binary(&id, &data, &size)) < 0)
                break;
            if (size > 0)
            {
                track->encodingAlgoPriv.resize(size);
                memcpy(track->encodingAlgoPriv.data(), data, size);
            }
            break;
        }
        default:
            res = ebml_read_skip();
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }
    return 0;
}

int MatroskaDemuxer::readTrackEncoding(MatroskaTrack* track)
{
    int res;
    uint32_t id;

    if ((res = ebml_read_master(&id)) < 0)
        return res;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            break;
        }
        if (level_up > 0)
        {
            level_up--;
            break;
        }

        res = id == MATROSKA_ID_ENCODINGCOMPRESSION ? readEncodingCompression(track) : ebml_read_skip();

        if (level_up)
        {
            level_up--;
            break;
        }
    }
    return 0;
}

int MatroskaDemuxer::readTrackEncodings(MatroskaTrack* track)
{
    int res;
    uint32_t id;

    if ((res = ebml_read_master(&id)) < 0)
        return res;

    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            break;
        }
        if (level_up > 0)
        {
            level_up--;
            break;
        }

        res = id == MATROSKA_ID_TRACKCONTENTENCODING ? readTrackEncoding(track) : ebml_read_skip();

        if (level_up)
        {
            level_up--;
            break;
        }
    }
    return 0;
}

int MatroskaDemuxer::matroska_add_stream()
{
    int res = 0;
    uint32_t id;

    if (num_tracks >= MAX_STREAMS)
        THROW(ERR_COMMON, "Too many tracks. Max supported tracks count: " << MAX_STREAMS)

    /* Allocate a generic track. As soon as we know its type we'll realloc. */
    auto* track = reinterpret_cast<MatroskaTrack*>(new char[MAX_TRACK_SIZE]{});
    track->encodingAlgo = -1;
    strcpy(track->language, "eng");
    // FlagDefault's default VALUE in Matroska is 1, so a track that does not carry the element is
    // the default one, and mkvmerge leaves it out exactly when it is true. The track is raw zeroed
    // memory here, so without this an absent element read as "not the default", which is backwards.
    // FlagEnabled defaults to 1 as well; nothing reads that one, so it is left alone.
    track->flags |= MATROSKA_TRACK_DEFAULT;

    /* start with the master */
    if ((res = ebml_read_master(&id)) < 0)
    {
        freeTrack(track);
        return res;
    }

    /* try reading the trackentry headers */
    while (res == 0)
    {
        if ((id = ebml_peek_id(&level_up)) == 0)
        {
            res = -BufferedReader::DATA_EOF;
            break;
        }
        if (level_up > 0)
        {
            level_up--;
            break;
        }

        switch (id)
        {
        /* track number (unique stream ID) */
        case MATROSKA_ID_TRACKNUMBER:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            track->num = num;
            break;
        }

        /* track UID (unique identifier) */
        case MATROSKA_ID_TRACKUID:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            track->uid = num;
            break;
        }

        /* track type (video, audio, combined, subtitle, etc.) */
        case MATROSKA_ID_TRACKTYPE:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            if (track->type != IOContextTrackType::UNDEFINED && static_cast<int64_t>(track->type) != num)
            {
                LTRACE(LT_INFO, 0, "More than one tracktype in an entry - skip");
                break;
            }
            track->type = static_cast<MatroskaTrackType>(num);

            switch (track->type)
            {
            case IOContextTrackType::VIDEO:
            case IOContextTrackType::AUDIO:
            case IOContextTrackType::SUBTITLE:
                break;
            case IOContextTrackType::COMPLEX:
            case IOContextTrackType::LOGO:
            case IOContextTrackType::CONTROL:
            default:
                LTRACE(LT_INFO, 0, "Unknown or unsupported track type " << (uint64_t)track->type);
                track->type = static_cast<MatroskaTrackType>(0);
            }
            tracks[num_tracks++] = track;
            break;
        }

        /* tracktype specific stuff for video */
        case MATROSKA_ID_TRACKVIDEO:
        {
            MatroskaVideoTrack* videotrack;
            if (track->type == IOContextTrackType::UNDEFINED)
                track->type = IOContextTrackType::VIDEO;
            if (track->type != IOContextTrackType::VIDEO)
            {
                LTRACE(LT_INFO, 0, "video data in non-video track - ignoring");
                res = AVERROR_INVALIDDATA;
                break;
            }
            if ((res = ebml_read_master(&id)) < 0)
                break;
            videotrack = reinterpret_cast<MatroskaVideoTrack*>(track);

            while (res == 0)
            {
                if ((id = ebml_peek_id(&level_up)) == 0)
                {
                    res = -BufferedReader::DATA_EOF;
                    break;
                }
                if (level_up > 0)
                {
                    level_up--;
                    break;
                }

                switch (id)
                {
                // MATROSKA_ID_TRACKDEFAULTDURATION belongs one level up (in the track entry),
                // but some muxers incorrectly place it inside the video settings element.
                // Handle it here as well for compatibility with such files.
                case MATROSKA_ID_TRACKDEFAULTDURATION:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    track->default_duration = num;
                    break;
                }

                /* video framerate */
                case MATROSKA_ID_VIDEOFRAMERATE:
                {
                    double num;
                    if ((res = ebml_read_float(&id, &num)) < 0)
                        break;
                    if (!track->default_duration)
                        track->default_duration = static_cast<uint64_t>(1000000000 / num);
                    break;
                }

                /* width of the size to display the video at */
                case MATROSKA_ID_VIDEODISPLAYWIDTH:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    videotrack->display_width = static_cast<int>(num);
                    break;
                }

                /* height of the size to display the video at */
                case MATROSKA_ID_VIDEODISPLAYHEIGHT:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    videotrack->display_height = static_cast<int>(num);
                    break;
                }

                /* width of the video in the file */
                case MATROSKA_ID_VIDEOPIXELWIDTH:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    videotrack->pixel_width = static_cast<int>(num);
                    break;
                }

                /* height of the video in the file */
                case MATROSKA_ID_VIDEOPIXELHEIGHT:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    videotrack->pixel_height = static_cast<int>(num);
                    break;
                }

                /* whether the video is interlaced */
                case MATROSKA_ID_VIDEOFLAGINTERLACED:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    if (num)
                        track->flags |= MATROSKA_VIDEOTRACK_INTERLACED;
                    else
                        track->flags &= ~MATROSKA_VIDEOTRACK_INTERLACED;
                    break;
                }

                /* stereo mode (whether the video has two streams,
                 * where one is for the left eye and the other for
                 * the right eye, which creates a 3D-like
                 * effect) */
                case MATROSKA_ID_VIDEOSTEREOMODE:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;

                    if (num != static_cast<uint64_t>(MatroskaEyeMode::MONO) &&
                        num != static_cast<uint64_t>(MatroskaEyeMode::LEFT) &&
                        num != static_cast<uint64_t>(MatroskaEyeMode::RIGHT) &&
                        num != static_cast<uint64_t>(MatroskaEyeMode::BOTH))
                    {
                        LTRACE(LT_INFO, 0, "Ignoring unknown eye mode " << (uint32_t)num);
                        break;
                    }
                    videotrack->eye_mode = static_cast<MatroskaEyeMode>(num);
                    break;
                }

                /* aspect ratio behaviour */
                case MATROSKA_ID_VIDEOASPECTRATIO:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    if (num != static_cast<uint64_t>(MatroskaAspectRatioMode::FREE) &&
                        num != static_cast<uint64_t>(MatroskaAspectRatioMode::KEEP) &&
                        num != static_cast<uint64_t>(MatroskaAspectRatioMode::FIXED))
                    {
                        LTRACE(LT_INFO, 0, "Ignoring unknown aspect ratio " << (uint32_t)num);
                        break;
                    }
                    videotrack->ar_mode = static_cast<MatroskaAspectRatioMode>(num);
                    break;
                }

                /* colourspace (only matters for raw video)
                 * fourcc */
                case MATROSKA_ID_VIDEOCOLOURSPACE:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    videotrack->fourcc = static_cast<uint32_t>(num);
                    break;
                }
                case EBML_ID_VOID:
                case EBML_ID_CRC32:
                    res = ebml_read_skip();
                    break;
                default:
                    res = ebml_read_skip();
                    LTRACE(LT_INFO, 0, "Unknown video track header entry " << id << " - ignoring\n");
                }

                if (level_up)
                {
                    level_up--;
                    break;
                }
            }
            break;
        }

        /* tracktype specific stuff for audio */
        case MATROSKA_ID_TRACKAUDIO:
        {
            MatroskaAudioTrack* audiotrack;
            if (track->type == IOContextTrackType::UNDEFINED)
                track->type = IOContextTrackType::AUDIO;
            if (track->type != IOContextTrackType::AUDIO)
            {
                LTRACE(LT_INFO, 0, "audio data in non-audio track - ignoring");
                res = AVERROR_INVALIDDATA;
                break;
            }
            if ((res = ebml_read_master(&id)) < 0)
                break;
            audiotrack = reinterpret_cast<MatroskaAudioTrack*>(track);
            audiotrack->channels = 1;
            audiotrack->samplerate = 8000;

            while (res == 0)
            {
                if ((id = ebml_peek_id(&level_up)) == 0)
                {
                    res = -BufferedReader::DATA_EOF;
                    break;
                }
                if (level_up > 0)
                {
                    level_up--;
                    break;
                }

                switch (id)
                {
                /* samplerate */
                case MATROSKA_ID_AUDIOSAMPLINGFREQ:
                {
                    double num;
                    if ((res = ebml_read_float(&id, &num)) < 0)
                        break;
                    audiotrack->internal_samplerate = audiotrack->samplerate = static_cast<int>(num);
                    break;
                }
                case MATROSKA_ID_AUDIOOUTSAMPLINGFREQ:
                {
                    double num;
                    if ((res = ebml_read_float(&id, &num)) < 0)
                        break;
                    audiotrack->samplerate = static_cast<int>(num);
                    break;
                }

                    /* bitdepth */
                case MATROSKA_ID_AUDIOBITDEPTH:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    audiotrack->bitdepth = static_cast<uint16_t>(num);
                    break;
                }

                    /* channels */
                case MATROSKA_ID_AUDIOCHANNELS:
                {
                    int64_t num;
                    if ((res = ebml_read_uint(&id, &num)) < 0)
                        break;
                    audiotrack->channels = static_cast<uint16_t>(num);
                    break;
                }
                case EBML_ID_VOID:
                case EBML_ID_CRC32:
                    res = ebml_read_skip();
                    break;
                default:
                    res = ebml_read_skip();
                    LTRACE(LT_INFO, 0, "Unknown audio track header entry " << id << " - ignoring\n");
                }

                if (level_up)
                {
                    level_up--;
                    break;
                }
            }
            break;
        }

            /* codec identifier */
        case MATROSKA_ID_CODECID:
        {
            char* text;
            if ((res = ebml_read_ascii(&id, &text)) < 0)
                break;
            track->codec_id = text;
            break;
        }

            /* codec private data */
        case MATROSKA_ID_CODECPRIVATE:
        {
            uint8_t* data;
            int size;
            if ((res = ebml_read_binary(&id, &data, &size) < 0))
                break;
            track->codec_priv = data;
            track->codec_priv_size = size;
            break;
        }

            /* name of the codec */
        case MATROSKA_ID_CODECNAME:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            track->codec_name = text;
            break;
        }

            /* name of this track */
        case MATROSKA_ID_TRACKNAME:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            track->name = text;
            break;
        }

            /* language (matters for audio/subtitles, mostly) */
        case MATROSKA_ID_TRACKLANGUAGE:
        {
            char *text, *end;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            if ((end = strchr(text, '-')))
                *end = '\0';
            if (strlen(text) == 3)
            {
                strcpy(track->language, text);
                track->hasLanguageElement = true;
            }
            delete[] text;
            break;
        }

        case MATROSKA_ID_TRACKLANGUAGEBCP47:
        {
            char* text;
            if ((res = ebml_read_utf8(&id, &text)) < 0)
                break;
            const size_t len = strlen(text);
            // Said once per file and tag, as the start code rule is (applyStartCodeRule): the file is opened for the
            // track list and again for the mux, and the warning came twice.
            static std::set<std::string> reportedTooLong;
            if (len > 0 && len < sizeof(track->languageBCP47))
                memcpy(track->languageBCP47, text, len + 1);
            else if (len > 0 && reportedTooLong.insert(m_sourceName + '\n' + text).second)
                LTRACE(LT_WARN, 2,
                       "Warning: a track's LanguageBCP47 of " << len << " characters is too long and is left out.");
            delete[] text;
            break;
        }

            /* whether this is actually used */
        case MATROSKA_ID_TRACKFLAGENABLED:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            if (num)
                track->flags |= MATROSKA_TRACK_ENABLED;
            else
                track->flags &= ~MATROSKA_TRACK_ENABLED;
            break;
        }

            /* whether it's the default for this track type */
        case MATROSKA_ID_TRACKFLAGDEFAULT:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            if (num)
                track->flags |= MATROSKA_TRACK_DEFAULT;
            else
                track->flags &= ~MATROSKA_TRACK_DEFAULT;
            break;
        }

            /* lacing (like MPEG, where blocks don't end/start on frame
             * boundaries) */
        case MATROSKA_ID_TRACKFLAGLACING:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            if (num)
                track->flags |= MATROSKA_TRACK_LACING;
            else
                track->flags &= ~MATROSKA_TRACK_LACING;
            break;
        }

            /* default length (in time) of one data block in this track */
        case MATROSKA_ID_TRACKDEFAULTDURATION:
        {
            int64_t num;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            track->default_duration = num;
            break;
        }

        case MATROSKA_ID_TRACKCONTENTENCODINGS:
            readTrackEncodings(track);
            break;

            // The track's times are its block times times this (RFC 9559, 5.1.4.1.15, where it is deprecated, but
            // files written with it exist). It was skipped, so such a track's subtitles came at their unscaled times.
        case MATROSKA_ID_TRACKTIMESTAMPSCALE:
        {
            double num;
            if ((res = ebml_read_float(&id, &num)) < 0)
                break;
            if (num > 0 && std::isfinite(num))
                track->timestampScale = num;
            break;
        }
            /* we ignore these because they're nothing useful. */
        case MATROSKA_ID_MAXBLOCKADDITIONID:
        {
            int64_t num = 0;
            if ((res = ebml_read_uint(&id, &num)) < 0)
                break;
            m_maxAddIdsByTrack[track] = static_cast<uint64_t>(num);
            break;
        }
        case MATROSKA_ID_BLOCKADDITIONMAPPING:
        {
            // Kept as it stands, for a track copied as it is into a Matroska output
            uint8_t* body = nullptr;
            int bodySize = 0;
            if ((res = ebml_read_binary(&id, &body, &bodySize)) < 0)
                break;
            if (body && bodySize > 0)
                m_addMappingsByTrack[track].emplace_back(body, body + bodySize);
            delete[] body;
            break;
        }
        case EBML_ID_VOID:
        case EBML_ID_CRC32:
        case MATROSKA_ID_CODECINFOURL:
        case MATROSKA_ID_CODECDOWNLOADURL:
        case MATROSKA_ID_TRACKMINCACHE:
        case MATROSKA_ID_TRACKMAXCACHE:
            res = ebml_read_skip();
            break;
        default:
            res = ebml_read_skip();
            LTRACE(LT_INFO, 0, "Unknown track header entry " << id << " - ignoring");
        }

        if (level_up)
        {
            level_up--;
            break;
        }
    }

    // RFC 9559 (5.1.4.1.19-20): where a track has LanguageBCP47, its Language MUST be ignored. It was the other way
    // round, LanguageBCP47 was never read, so pt-BR came through as plain Portuguese only by luck of the Language
    // element beside it, and a track with LanguageBCP47 alone came through as English, the Language default. The
    // three-letter code every output needs is now taken from LanguageBCP47. Asked for the codes as the source has them
    // (--lang-codes=source), a Language element that names the same language keeps its own spelling.
    if (track->languageBCP47[0])
    {
        const std::string fromTag = iso639FromBcp47(track->languageBCP47);
        if (!fromTag.empty() && !(getLangCodeStyle() == LangCodeStyle::Source && track->hasLanguageElement &&
                                  toIso639_2T(track->language) == fromTag))
            memcpy(track->language, fromTag.c_str(), 4);
    }

    return res;
}

// ------------- need to be implemented --------------

int MatroskaDemuxer::simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
{
    for (int acceptedPID : acceptedPIDs) demuxedData[acceptedPID];

    AVPacket packet;
    uint32_t demuxedSize = 0;
    discardSize = 0;
    while (demuxedSize < m_fileBlockSize)
    {
        const int readRez = readPacket(packet);
        if (readRez == BufferedReader::DATA_EOF)
        {
            m_demuxEnded = true;
            discardSize = m_processedBytes - m_lastProcessedBytes - demuxedSize;
            m_lastProcessedBytes = m_processedBytes;
            return readRez;
        }

        auto itr = m_pidFilters.find(packet.stream_index);
        if (itr != m_pidFilters.end())
        {
            demuxedSize += itr->second->demuxPacket(demuxedData, acceptedPIDs, packet);
        }
        else
        {
            if (acceptedPIDs.find(packet.stream_index) != acceptedPIDs.end())
            {
                MemoryBlock& vect = demuxedData[packet.stream_index];
                if (packet.size > 0)
                    vect.append(packet.data, packet.size);
                demuxedSize += packet.size;
            }
        }
    }
    discardSize = m_processedBytes - m_lastProcessedBytes - demuxedSize;
    m_lastProcessedBytes = m_processedBytes;
    return 0;
}

// Read one attached file's bytes now. The caller position is saved and restored, including the
// cached element id, because unlike the header parse this can be called at any point.
bool MatroskaDemuxer::getAttachment(const std::string& name, std::vector<uint8_t>& out)
{
    const MatroskaAttachment* found = nullptr;
    for (const auto& att : m_attachments)
        if (att.name == name)
            found = &att;
    if (found == nullptr || found->dataSize <= 0)
        return false;

    // Again on a handle of its own. The demuxer's reader is left exactly where it was.
    File f;
    if (!f.open(m_attachmentSource.c_str(), File::ofRead))
        return false;
    out.resize(static_cast<size_t>(found->dataSize));
    f.seek(found->dataPos);
    const bool ok = f.read(out.data(), static_cast<uint32_t>(found->dataSize)) == static_cast<int>(found->dataSize);
    f.close();
    if (!ok)
        out.clear();
    return ok;
}

void MatroskaDemuxer::getTrackList(std::map<int32_t, TrackInfo>& trackList)
{
    for (int i = 0; i < num_tracks; i++)
    {
        trackList[i + 1] = TrackInfo(getTrackType(tracks[i]), tracks[i]->language, 0, 0,
                                     (tracks[i]->flags & MATROSKA_TRACK_DEFAULT) ? 1 : 0);
        trackList[i + 1].m_langBCP47 = tracks[i]->languageBCP47;
        if (const auto extras = m_trackExtras.find(tracks[i]->num); extras != m_trackExtras.end())
            trackList[i + 1].m_srcParams = extras->second;
        if (tracks[i]->name && tracks[i]->name[0])
            trackList[i + 1].m_srcParams["srcname"] = tracks[i]->name;
        if (tracks[i]->codec_id && tracks[i]->codec_id[0])
            trackList[i + 1].m_srcParams["srccodecid"] = tracks[i]->codec_id;
        // A VP9 track's CodecPrivate (profile, level, bit depth, chroma subsampling), which the stream says only in
        // part, for a Matroska output to keep (srcvpxprivate, in hex).
        if (tracks[i]->codec_id && !strcmp(tracks[i]->codec_id, MATROSKA_CODEC_ID_VP9) && tracks[i]->codec_priv &&
            tracks[i]->codec_priv_size > 0)
        {
            static const char* HEX = "0123456789abcdef";
            std::string hex;
            for (int k = 0; k < tracks[i]->codec_priv_size; ++k)
            {
                hex += HEX[tracks[i]->codec_priv[k] >> 4];
                hex += HEX[tracks[i]->codec_priv[k] & 15];
            }
            trackList[i + 1].m_srcParams["srcvpxprivate"] = hex;
        }
        // DVD or DVB subtitles in a file without video: the frame rate their display sets carry is a guess, which
        // fps= replaces rather than converts from.
        if (const auto* bitmapSub = dynamic_cast<ParsedBitmapSubTrackData*>(tracks[i]->parsed_priv_data);
            bitmapSub && bitmapSub->rateGuessed())
            trackList[i + 1].m_srcParams["srcpgsrate"] = "guessed";
    }
}

double MatroskaDemuxer::getTrackFps(const uint32_t trackId)
{
    if (trackId == 0 || static_cast<int>(trackId) > num_tracks)
        return 0.0;
    const MatroskaTrack* track = tracks[trackId - 1];
    if (track->default_duration > 0)
        return 1000000000.0 / static_cast<double>(track->default_duration);

    // A video track without DefaultDuration, as many WebM files have: the frame rate was taken to be 25 and every
    // picture retimed to it. The mean spacing of its first pictures says it instead: their times sorted, since pictures
    // are stored in decoding order, and taken from first to last, so that times rounded to the millisecond cancel out.
    // A track whose rate varies gets its mean rate.
    if (track->type != MatroskaTrackType::VIDEO || m_sourceName.empty())
        return 0.0;
    if (const auto known = m_estimatedFps.find(trackId); known != m_estimatedFps.end())
        return known->second;
    static constexpr size_t PICTURES = 240;
    double fps = 0.0;
    SegmentFacts facts;
    File f;
    if (readSegmentFacts(m_sourceName, facts) && facts.firstCluster > 0 && f.open(m_sourceName.c_str(), File::ofRead))
    {
        EbmlScan scan(f);
        std::vector<int64_t> times;
        walkTrackBlocks(scan, facts.firstCluster, facts.segmentEnd, 256, track->num,
                        [&](const int64_t time, int64_t, bool, int)
                        {
                            times.push_back(time);
                            return times.size() < PICTURES;
                        });
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        if (times.size() >= 8 && times.back() > times.front())
        {
            const double spacingNs = static_cast<double>(times.back() - times.front()) *
                                     static_cast<double>(facts.timestampScale) / static_cast<double>(times.size() - 1);
            fps = 1e9 / spacingNs;
            // The rate the times stand for: a standard one within 0.04 % (240 pictures rounded to the millisecond are
            // out by about 0.013 % at most; 29.97 and 30 are 0.1 % apart).
            for (const double standard :
                 {12.5, 15000.0 / 1001, 15.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 30.0, 48000.0 / 1001, 48.0,
                  50.0, 60000.0 / 1001, 60.0, 100.0, 120000.0 / 1001, 120.0})
                if (std::fabs(fps - standard) < standard * 0.0004)
                    fps = standard;
            LTRACE(LT_INFO, 2,
                   "Track " << trackId << " gives no frame rate (DefaultDuration). Taken from the times of its first "
                            << times.size() << " pictures: " << fps << " fps. fps= on the track's line sets another.");
        }
    }
    m_estimatedFps[trackId] = fps;
    return fps;
}

const uint8_t* MatroskaDemuxer::getTrackCodecPrivate(const int32_t pid, int& size)
{
    // MKV track IDs are 1-based in our mapping
    if (pid <= 0 || pid > num_tracks)
    {
        size = 0;
        return nullptr;
    }
    const MatroskaTrack* track = tracks[pid - 1];
    size = track->codec_priv_size;
    return track->codec_priv;
}

int MatroskaDemuxer::getTrackType(const MatroskaTrack* track)
{
    if (track->codec_id == nullptr)
        return 0;

    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SRT) || !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_ASCII) ||
        !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_ASS) || !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_SSA) ||
        !strcmp(track->codec_id, MATROSKA_CODEC_ID_TEXT_WEBVTT) || isWebmVttText(track->codec_id))
        return TRACKTYPE_SRT;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_PCM_BIG))
        return TRACKTYPE_WAV;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_PCM_LIT))
        return TRACKTYPE_WAV;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_ACM))
        return TRACKTYPE_WAV;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_PGS) ||
        !strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_VOBSUB) ||
        !strcmp(track->codec_id, MATROSKA_CODEC_ID_SUBTITLE_DVB))
        return TRACKTYPE_PGS;

    // The video codecs are named as well, so that detection asks that reader first: MPEG-2 video from a DVD was taken
    // for FLAC (#27). Detection still checks the data, so a wrong name changes nothing.
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VIDEO_MPEG2))
        return static_cast<int>(StreamType::VIDEO_MPEG2);
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AVC_FOURCC))
        return static_cast<int>(StreamType::VIDEO_H264);
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_HEVC_FOURCC))
        return static_cast<int>(StreamType::VIDEO_H265);
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VVC_FOURCC))
        return static_cast<int>(StreamType::VIDEO_H266);
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VP8))
        return TRACKTYPE_VP8;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_VP9))
        return TRACKTYPE_VP9;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_VORBIS))
        return TRACKTYPE_VORBIS;
    if (!strcmp(track->codec_id, MATROSKA_CODEC_ID_AUDIO_ALAC))
        return TRACKTYPE_ALAC;

    return 0;
}

std::vector<AVChapter> MatroskaDemuxer::getChapters()
{
    std::vector<AVChapter> rez;
    rez.reserve(chapters.size());
    for (const auto& [index, avChapter] : chapters) rez.push_back(avChapter);
    std::sort(rez.begin(), rez.end());
    return rez;
}
