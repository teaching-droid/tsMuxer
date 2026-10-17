// ReSharper disable CppPassValueParameterByConstReference

#include "movDemuxer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <fs/systemlog.h>

#include "aac.h"
#include "abstractStreamReader.h"
#include "av1.h"
#include "avPacket.h"
#include "bitStream.h"
#include "hevc.h"
#include "matroskaParser.h"
#include "nalUnits.h"
#include "subTrackFilter.h"
#include "utf8Converter.h"
#include "vodCoreException.h"
#include "vvc.h"
#include "wave.h"
#include "zlib.h"

using namespace std;

namespace
{
const char* mov_mdhd_language_map[] = {
    // see https :  // developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFChap4/qtff4.html
    "eng", "fra", "deu", "ita", "dut", "sve", "spa", "dan", "por", "nor", "heb", "jpn", "ara", "fin", "ell", "isl",
    "mlt", "tur", "hrv", "zho", "urd", "hin", "tha", "kor", "lit", "pol", "hun", "est", "lav", "smi", "fao", "fas",
    "rus", "zho", "nld", "gle", "alb", "ron", "ces", "slk", "slv", "yid", "srp", "mkd", "bul", "ukr", "bel", "uzb",
    "kaz", "aze", "aze", "arm", "geo", "ron", "kir", "tgk", "tuk", "mon", "mon", "pus", "kur", "kas", "snd", "tib",
    "nep", "san", "mar", "ben", "asm", "guj", "pa ", "ori", "mal", "kan", "tam", "tel", "sin", "bur", "khm", "lao",
    "vie", "ind", "tgl", "may", "may", "amh", "tir", "orm", "som", "swa", "kin", "run", "nya", "mlg", "epo", nullptr,
    nullptr, nullptr, nullptr, nullptr,
    /* 100 */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, "cym", "eus", "cat", "lat", "que", "grn", "aym", "crh", "uig", "dzo", "jav"};
}

static constexpr int MP4ESDescrTag = 0x03;
static constexpr int MP4DecConfigDescrTag = 0x04;
static constexpr int MP4DecSpecificDescrTag = 0x05;

// A count of a time scale in nanoseconds, without running over 64 bits for a fine scale or a late start
static int64_t ticksToNs(const int64_t ticks, const int64_t timeScale)
{
    if (timeScale <= 0)
        return 0;
    return ticks / timeScale * 1000000000LL + ticks % timeScale * 1000000000LL / timeScale;
}

#define MKTAG(a, b, c, d) ((a) | (b) << 8 | (c) << 16 | (d) << 24)

struct MOVStts
{
    uint32_t count;
    int64_t duration;
};

struct MOVDref
{
    uint32_t type;
    char* path;
};

struct MOVStsc
{
    unsigned first;
    unsigned count;
    unsigned id;
};

/*
int64_t av_gcd(int64_t a, int64_t b){
    if(b) return av_gcd(b, a%b);
    else  return a;
}
*/

int ff_mov_lang_to_iso639(unsigned code, char* to)
{
    // see http://www.geocities.com/xhelmboyx/quicktime/formats/mp4-layout.txt
    if (code > 138)
    {
        // Three letters of ISO 639-2/T, five bits each. QuickTime writes 0x7FFF for "not given", which made three DEL
        // characters (0x7F): the listing printed them, and a Matroska output wrote them as the track's language. Only
        // a to z make a language.
        char letters[3];
        for (int i = 2; i >= 0; i--)
        {
            letters[i] = static_cast<char>(0x60 + (code & 0x1f));
            code >>= 5;
        }
        for (const char c : letters)
            if (c < 'a' || c > 'z')
                return 0;
        memcpy(to, letters, 3);
        return 1;
    }
    // old fashion apple lang code
    if (code >= sizeof(mov_mdhd_language_map) / sizeof(char*))
        return 0;
    if (!mov_mdhd_language_map[code])
        return 0;
    memcpy(to, mov_mdhd_language_map[code], 4);
    return 1;
}

struct MOVStreamContext : Track
{
    MOVStreamContext()
        : m_indexCur(0),
          ffindex(0),
          next_chunk(0),
          ctts_count(0),
          fps(0),
          ctts_index(0),
          ctts_sample(0),
          sample_size(0),
          sample_count(0),
          keyframe_count(0),
          time_scale(0),
          current_sample(0),
          bytes_per_frame(0),
          samples_per_frame(0),
          pseudo_stream_id(0),
          audio_cid(0),
          width(0),
          height(0),
          bits_per_coded_sample(0),
          channels(0),
          packet_size(0),
          sample_rate(0)
    {
    }

    ~MOVStreamContext() = default;

    int containerDataType = 0;  // TRACKTYPE_VP8 etc. for the codecs nothing in their data identifies (getTrackList)
    uint32_t trackId = 0;       // tkhd track_ID
    bool isMp4v = false;        // an mp4v sample entry, whose esds says what it holds
    int64_t editMediaTime = 0;  // where the first edit of the track's edit list starts in its media (mov_read_moov)
    int64_t editDuration = 0;   // how long that edit plays, in the track's time scale (0: no edit list)
    // A fragmented file: the decode time and duration of each sample its runs (trun) give, after those of the moov,
    // and where the next one starts (tfdt, then the durations)
    std::vector<int64_t> fragDts, fragDur;
    int64_t fragNextDts = 0;
    std::string name;          // the track's name, from its hdlr, when it is not an encoder's generic one
    std::string leftOut;       // why the track cannot be read, for a meta line that names it (getTrackList)
    bool hasEditList = false;  // an elst was read for the track
    bool isOpus = false;       // an Opus sample entry: its dOps (codec_priv) gives the pre-skip
    std::map<std::string, std::string> colour;  // srccolour items (m, t, p, r, cll, fall, rx ... lmin) of the entry
    uint32_t paspH = 0, paspV = 0;              // pasp: the shape of a pixel
    bool pcmBigEndian = false;                  // an LPCM sample entry: the byte order of its samples
    bool pcmFloat = false;
    uint32_t lpcmFlags = 0;  // the format flags of a QuickTime 'lpcm' sample entry (version 2)

    vector<int64_t> chunk_offsets;
    vector<uint32_t> m_index;
    size_t m_indexCur;

    unsigned ffindex;  // the ffmpeg stream id
    int next_chunk;
    unsigned int ctts_count;
    vector<MOVStsc> stsc_data;
    double fps;

    int ctts_index;
    int ctts_sample;
    uint32_t sample_size;
    unsigned sample_count;
    unsigned keyframe_count;
    unsigned time_scale;
    // int time_rate;
    int current_sample;
    unsigned bytes_per_frame;
    unsigned samples_per_frame;
    unsigned pseudo_stream_id;  ///< -1 means demux all ids
    int audio_cid;              ///< stsd audio compression id
    int width;                  ///< tkhd width
    int height;                 ///< tkhd height
    unsigned bits_per_coded_sample;
    unsigned channels;
    int packet_size;
    int sample_rate;
    vector<uint32_t> keyframes;
    // vector<MOVDref> drefs;
    vector<MOVStts> stts_data;
    vector<MOVStts> ctts_data;
};

class MovParsedAudioTrackData final : public ParsedTrackPrivData
{
   public:
    MovParsedAudioTrackData(MovDemuxer* demuxer, MOVStreamContext* sc)
        : m_buff(), m_size(0), m_demuxer(demuxer), m_sc(sc)
    {
        isAAC = false;
    }

    /// Initialise AAC codec defaults from the stsd audio fields so that
    /// a usable ADTS header can be built even when ESDS data is absent.
    void initAACDefaults()
    {
        m_aacRaw.m_id = 1;  // MPEG-2
        m_aacRaw.m_layer = 0;
        m_aacRaw.m_rdb = 0;
        m_aacRaw.m_profile = 0;  // AAC-LC default (will be refined by readConfig)
        m_aacRaw.m_channels = static_cast<uint8_t>(m_sc->channels);
        m_aacRaw.m_sample_rate = m_sc->sample_rate;
    }

    void setPrivData(uint8_t* buff, const int size) override
    {
        m_buff = buff;
        m_size = size;
        // Use readConfig to properly parse the AudioSpecificConfig, just like
        // the Matroska path does.  This correctly sets profile, sample rate,
        // channel count and channel index in one consistent step.
        if (size >= 2)
        {
            m_aacRaw.m_id = 1;  // MPEG-2
            m_aacRaw.m_layer = 0;
            m_aacRaw.m_rdb = 0;
            m_aacRaw.readConfig(buff, size);
        }
        else
        {
            // Fallback: use stsd fields when AudioSpecificConfig is too short
            initAACDefaults();
        }
    }

    // The first frames of the track are left out: the encoder's priming of a later file of a join
    void setDropFrames(const int frames) { m_dropLeft = frames; }

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        uint8_t* dst = pkt->data;
        const uint8_t* srcEnd = buff + size;
        // Every whole frame of the chunk, however small: AAC encodes digital silence in 4 bytes a frame, and a loop
        // that stopped 4 bytes before the end left those frames out, the index lost its place and the mux stopped
        // with "Out of index". The index moves on only for a frame that is taken.
        while (buff < srcEnd)
        {
            unsigned frameSize = m_sc->sample_size;
            if (frameSize == 0)
            {
                if (m_sc->m_indexCur >= m_sc->m_index.size())
                    break;
                frameSize = m_sc->m_index[m_sc->m_indexCur];
                // An empty sample in the table has nothing to carry: it is passed over, as 2.20.0 did. Stopping at it
                // left the rest of the chunk unread and the mux stopped with "Invalid AAC frame".
                if (frameSize == 0)
                {
                    ++m_sc->m_indexCur;
                    continue;
                }
            }
            if (buff + frameSize > srcEnd)
                break;
            if (m_sc->sample_size == 0)
                ++m_sc->m_indexCur;
            if (m_dropLeft > 0)
            {
                --m_dropLeft;
                buff += frameSize;
                continue;
            }
            if (isAAC)
            {
                m_aacRaw.buildADTSHeader(dst, frameSize + AAC_HEADER_LEN);
                memcpy(dst + AAC_HEADER_LEN, buff, frameSize);
                dst += frameSize + AAC_HEADER_LEN;
            }
            else
            {
                memcpy(dst, buff, frameSize);
                dst += frameSize;
            }
            buff += frameSize;
        }
    }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        unsigned left = size;
        int i = 0;
        int empties = 0;  // empty samples among the i entries of the table: no frame, no header
        for (; left > 0; ++i)
        {
            // One size for every sample (AC-3, for instance): a chunk is read up to the next chunk, and bytes between
            // the two that are not a whole sample were taken off all the same. The unsigned count went round and the
            // loop never ended: the file could not even be listed. They are left out now, as extractData leaves them.
            if (m_sc->sample_size != 0 && m_sc->sample_size > left)
            {
                if (!m_tailWarned)
                    LTRACE(LT_WARN, 2,
                           "Warning: audio track #" << m_sc->ffindex << " of the MP4 file has " << left
                                                    << " bytes after the samples of a chunk; they are left out.");
                m_tailWarned = true;
                break;
            }
            left -= m_sc->sample_size;
            if (m_sc->sample_size == 0)
            {
                if (m_sc->m_indexCur + i >= m_sc->m_index.size() && left <= 4)
                    break;  // a few bytes after the last sample, left out as before
                if (m_sc->m_indexCur + i >= m_sc->m_index.size())
                    THROW(ERR_MOV_PARSE, "Out of index for AAC track #" << m_sc->ffindex << " at position "
                                                                        << m_demuxer->getProcessedBytes())
                // a frame that is not whole in the chunk is not taken (extractData stops at it too)
                const unsigned frameSize = m_sc->m_index[m_sc->m_indexCur + i];
                if (frameSize == 0)
                {
                    ++empties;  // passed over, as extractData passes over it
                    continue;
                }
                if (frameSize > left)
                    break;
                left -= frameSize;
            }
        }
        if (left > 4 && m_sc->sample_size == 0)
            THROW(ERR_MOV_PARSE,
                  "Invalid AAC frame for track #" << m_sc->ffindex << " at position " << m_demuxer->getProcessedBytes())
        // the frames extractData leaves out: neither their bytes nor their headers. Empty samples are no frames.
        unsigned dropped = 0;
        const int drop = (std::min)(m_dropLeft, i - empties);
        for (int k = 0, taken = 0; taken < drop && k < i; ++k)
        {
            const unsigned sz = m_sc->sample_size ? m_sc->sample_size : m_sc->m_index[m_sc->m_indexCur + k];
            if (sz == 0)
                continue;
            dropped += sz;
            ++taken;
        }
        const int frames = i;
        i -= empties;
        if (!isAAC)
            i = 0;
        const unsigned out = (size - left) - dropped + (i - (isAAC ? drop : 0)) * AAC_HEADER_LEN;
        // A chunk of nothing but dropped frames gives nothing, and extractData is then not called for it: its frames
        // are consumed here. For a track of one sample size too: its count of frames still to drop stayed where it
        // was, and every chunk after it was dropped, to the end of the part.
        if (out == 0 && drop > 0)
        {
            if (m_sc->sample_size == 0)
                m_sc->m_indexCur += frames;
            m_dropLeft -= drop;
        }
        return out;
    }

    bool isAAC;

   private:
    int m_dropLeft = 0;
    bool m_tailWarned = false;
    uint8_t* m_buff;
    int m_size;
    MovDemuxer* m_demuxer;
    MOVStreamContext* m_sc;
    AACCodec m_aacRaw;
};

class MovParsedH264TrackData : public ParsedTrackPrivData
{
   public:
    // The largest NAL this reader will believe. newBufferSize measures the buffer and extractData
    // fills it, so both have to stop at the same value or one writes more than the other reserved.
    // It was 10 MiB, which dropped every picture larger than that, as an 8K picture coded without loss (42 MiB) is.
    // It is now the limit of the video readers (MAX_TMP_BUFFER_SIZE); a NAL also has to fit in its chunk, which is
    // what keeps a damaged length from reading past the data.
    static constexpr uint32_t MAX_NAL_SIZE = 256 * 1024 * 1024;

    MovParsedH264TrackData(MovDemuxer* demuxer, MOVStreamContext* sc) : m_sc(sc), m_demuxer(demuxer), nal_length_size(4)
    {
    }
    void setPrivData(uint8_t* buff, int size) override
    {
        spsPpsList.clear();
        if (size < 6)
            THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
        nal_length_size = (buff[4] & 0x03) + 1;
        // Until a picture parameter set is found, here or in the stream (walkSample)
        m_ppsMissing = true;
        int spsCnt = buff[5] & 0x1f;
        if (spsCnt == 0)
            return;
        uint8_t* src = buff + 6;
        uint8_t* end = buff + size;
        for (; spsCnt > 0; spsCnt--)
        {
            if (src + 2 > end)
                THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
            int nalSize = (src[0] << 8) + src[1];
            src += 2;
            if (src + nalSize > end)
                THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
            if (nalSize > 0)
            {
                spsPpsList.emplace_back();
                for (int i = 0; i < nalSize; ++i, ++src) spsPpsList.rbegin()->push_back(*src);
            }
        }
        // The count of picture parameter sets was read even where the configuration ended before it: the byte after
        // the buffer decided whether the file was read or refused. A configuration that ends there has none, and the
        // stream has to carry them.
        if (src >= end)
        {
            LTRACE(LT_WARN, 2,
                   "Warning: the H.264 configuration (avcC) of an MP4 track ends before its picture parameter sets; "
                   "the stream itself has to carry them.");
            return;
        }
        int ppsCnt = *src++;
        for (; ppsCnt > 0; ppsCnt--)
        {
            if (src + 2 > end)
                THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
            int nalSize = (src[0] << 8) + src[1];
            src += 2;
            if (src + nalSize > end)
                THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
            if (nalSize > 0)
            {
                spsPpsList.emplace_back();
                for (int i = 0; i < nalSize; ++i, ++src) spsPpsList.rbegin()->push_back(*src);
                m_ppsMissing = false;
            }
        }
    }

    int getNalSize(const uint8_t* buff) const
    {
        if (nal_length_size == 1)
            return buff[0];
        if (nal_length_size == 2)
            return (buff[0] << 8) + buff[1];
        if (nal_length_size == 3)
            return (buff[0] << 16) + (buff[1] << 8) + buff[2];
        if (nal_length_size == 4)
            return (buff[0] << 24) + (buff[1] << 16) + (buff[2] << 8) + buff[3];

        THROW(ERR_MOV_PARSE, "MP4/MOV error: Unsupported H.264/AVC frame length field value " << nal_length_size)
    }

    void extractData(AVPacket* pkt, uint8_t* /*buff*/, const int /*size*/) override
    {
        if (!m_out.empty())
            memcpy(pkt->data, m_out.data(), m_out.size());
        m_sc->m_indexCur = m_indexAfter;
        spsPpsList.clear();
    }

    // The chunk, sample by sample as the track's sample sizes give them, each NAL with a 4 byte start code in place of
    // its length field; extractData copies what this builds. The chunk was walked as one run of NALs, by a second
    // walk in extractData as well: where a length made no sense, both gave up on the rest of the chunk, so damage in
    // one picture, as zero bytes filled into a file that was cut off, took every picture after it in the chunk with
    // it, hundreds of kilobytes. Each sample is walked on its own now, and the next one starts afresh. A chunk whose
    // samples the tables do not give is walked as one, as before.
    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        m_out.clear();
        if (!buff || size == 0)
            return 0;

        if (nal_length_size < 1 || nal_length_size > 4)
            LTRACE(LT_WARN, 2, "MP4/MOV: invalid nal_length_size " << static_cast<int>(nal_length_size));

        for (const auto& ps : spsPpsList)
        {
            startCode();
            m_out.insert(m_out.end(), ps.begin(), ps.end());
        }
        size_t indexCur = m_sc->m_indexCur;
        const uint8_t* p = buff;
        const uint8_t* chunkEnd = buff + size;
        while (p < chunkEnd)
        {
            auto sampleSize = static_cast<size_t>(chunkEnd - p);
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur];
            if (sampleSize == 0 || sampleSize > static_cast<size_t>(chunkEnd - p))
                sampleSize = static_cast<size_t>(chunkEnd - p);
            ++indexCur;
            // A sample whose length fields fill it exactly is whole. In the first that does not, the NALs before the
            // fault are kept, as they were. While samples go on failing, as where the sizes of the tables do not
            // match the data, nothing of them is kept: a length that happens to fit there brings no picture.
            m_nals.clear();
            const SampleWalk walk = walkSample(p, p + sampleSize, !m_faulty);
            if (walk == SampleWalk::Whole || !m_faulty)
                m_out.insert(m_out.end(), m_nals.begin(), m_nals.end());
            m_faulty = walk != SampleWalk::Whole;
            p += sampleSize;
        }
        m_indexAfter = indexCur;
        // extractData is not called for a chunk that gives nothing, and the next chunk starts after its samples all
        // the same
        if (m_out.empty())
            m_sc->m_indexCur = m_indexAfter;
        if (m_out.size() > UINT_MAX)
            THROW(ERR_MOV_PARSE, "MP4/MOV: a chunk of video too large to be read (" << m_out.size() << " bytes)")
        return static_cast<unsigned>(m_out.size());
    }

   private:
    void startCode()
    {
        m_out.push_back(0);
        m_out.push_back(0);
        m_out.push_back(0);
        m_out.push_back(1);
    }

    enum class SampleWalk
    {
        Whole,  // the length fields fill the sample exactly
        Zeros,  // from a length of 0 on, nothing but zero bytes
        Broken  // a length that does not fit
    };

    // The NALs of one sample, from its length fields, into m_nals. tell: a fault is named (the first of a run).
    SampleWalk walkSample(const uint8_t* cur, const uint8_t* end, const bool tell)
    {
        const auto put = [this](const uint8_t* nal, const uint32_t n)
        {
            m_nals.push_back(0);
            m_nals.push_back(0);
            m_nals.push_back(0);
            m_nals.push_back(1);
            m_nals.insert(m_nals.end(), nal, nal + n);
        };
        while (cur < end)
        {
            if (static_cast<size_t>(end - cur) < nal_length_size)
            {
                if (tell)
                    LTRACE(LT_WARN, 2,
                           "MP4/MOV: truncated NAL length field at position " << m_demuxer->getProcessedBytes()
                                                                              << ", stopping parse");
                return SampleWalk::Broken;
            }
            const auto nalSize = static_cast<uint32_t>(getNalSize(cur));
            cur += nal_length_size;
            // A length field of 0 is an empty NAL, which holds nothing, and is stepped over.
            if (nalSize == 0)
            {
                // Zero bytes from here to the end of the sample are no empty NALs but the data of a file that was cut
                // off and filled up with zeros. Its pictures, which hold nothing, were left out with no word but a
                // line about a truncated length field at the end of the file (#715).
                const uint8_t* zeroStart = cur - nal_length_size;
                if (end - zeroStart >= 16 && std::all_of(zeroStart, end, [](const uint8_t b) { return b == 0; }))
                {
                    static std::set<std::pair<std::string, int>> told;  // the file is opened more than once in a job
                    if (told.emplace(m_demuxer->fileName(), m_sc->ffindex).second)
                        LTRACE(LT_WARN, 2,
                               "Warning: video track #"
                                   << m_sc->ffindex << " of " << m_demuxer->fileName()
                                   << ": part of its data is nothing but zero bytes, as in a file that was cut off "
                                      "and filled up. Pictures made of zero bytes hold nothing and are left out.");
                    return SampleWalk::Zeros;
                }
                continue;
            }
            if (nalSize > MAX_NAL_SIZE)
            {
                if (tell)
                    LTRACE(LT_WARN, 2,
                           "MP4/MOV: suspicious NAL size " << nalSize << " at position "
                                                           << m_demuxer->getProcessedBytes());
                return SampleWalk::Broken;
            }
            if (nalSize > static_cast<size_t>(end - cur))
            {
                if (tell)
                    LTRACE(LT_WARN, 2,
                           "MP4/MOV: NAL size " << nalSize << " exceeds frame boundary at position "
                                                << m_demuxer->getProcessedBytes());
                return SampleWalk::Broken;
            }
            // A configuration without picture parameter sets leaves them to the stream. Where the stream has none
            // either, no picture can be decoded, and such a file was muxed into an output with no picture at all
            // that said "Mux successful". 2.20.0 refused the configuration itself.
            if (m_ppsMissing && nalSize > 0)
            {
                const int nalType = cur[0] & 0x1f;
                if (nalType == 8)
                    m_ppsMissing = false;
                else if (nalType == 5)
                    THROW(ERR_MOV_PARSE, "Track " << m_sc->ffindex << " of " << m_demuxer->fileName()
                                                  << ": the H.264 configuration (avcC) holds no picture parameter "
                                                     "sets, and the stream has none before its first IDR picture "
                                                     "either. Its pictures cannot be decoded.")
            }
            put(cur, nalSize);
            cur += nalSize;
        }
        return SampleWalk::Whole;
    }

    std::vector<uint8_t> m_out;   // the chunk as extractData hands it on (newBufferSize)
    std::vector<uint8_t> m_nals;  // the NALs of one sample (walkSample)
    size_t m_indexAfter = 0;      // the sample after the chunk
    bool m_faulty = false;        // the last sample walked was not whole

   protected:
    MOVStreamContext* m_sc;
    MovDemuxer* m_demuxer;

    vector<vector<uint8_t>> spsPpsList;
    uint8_t nal_length_size;
    bool m_ppsMissing = false;  // H.264: no picture parameter set yet, in the avcC or the stream
};

class MovParsedH265TrackData final : public MovParsedH264TrackData
{
   public:
    MovParsedH265TrackData(MovDemuxer* demuxer, MOVStreamContext* sc) : MovParsedH264TrackData(demuxer, sc) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        spsPpsList = hevc_extract_priv_data(buff, size, &nal_length_size);
    }
};

class MovParsedH266TrackData final : public MovParsedH264TrackData
{
   public:
    MovParsedH266TrackData(MovDemuxer* demuxer, MOVStreamContext* sc) : MovParsedH264TrackData(demuxer, sc) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        spsPpsList = vvc_extract_priv_data(buff, size, &nal_length_size);
    }
};

// MPEG-4 Part 2 and ProRes, copied picture for picture into a Matroska output (VideoCopyStreamReader). Each sample
// becomes a record with its display time (stts and ctts), its decode time and its key frame flag (stss), behind one
// record with the picture size and the configuration. A fragmented file is timed by its tfdt boxes and its runs.
class MovParsedVideoCopyData final : public ParsedTrackPrivData
{
   public:
    MovParsedVideoCopyData(MOVStreamContext* sc, const uint32_t fourcc) : m_sc(sc), m_fourcc(fourcc) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        if (buff && size > 0)
            m_config.assign(buff, buff + size);
    }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        times();
        m_out.clear();
        if (!m_configSent)
        {
            put("VCFG", 4);
            be(m_sc->width, 2);
            be(m_sc->height, 2);
            be(m_fourcc, 4);
            be(m_config.size(), 4);
            m_out.insert(m_out.end(), m_config.begin(), m_config.end());
        }
        size_t indexCur = m_sc->m_indexCur;
        uint32_t used = 0;
        while (used < size)
        {
            uint32_t sampleSize = size - used;
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur];
            sampleSize = (std::min)(sampleSize, size - used);
            if (sampleSize == 0)
                break;
            const size_t n = indexCur;
            // The track starts where its edit list says: with pictures reordered, its first picture is shown after
            // its first decode time, and the edit list moves it back to the start
            const int64_t dts =
                (n < m_dts.size() ? m_dts[n] : (m_dts.empty() ? 0 : m_dts.back())) - m_sc->editMediaTime;
            const int64_t pts = dts + (n < m_ctts.size() ? m_ctts[n] : 0);
            const int64_t dur = n < m_dur.size() ? m_dur[n] : 0;
            const bool key =
                m_sc->keyframes.empty() ||
                std::binary_search(m_sc->keyframes.begin(), m_sc->keyframes.end(), static_cast<uint32_t>(n + 1));
            put("VCPY", 4);
            be(static_cast<uint64_t>(internal(pts)), 8);
            be(static_cast<uint64_t>(internal(dts)), 8);
            be(static_cast<uint64_t>(internal(dur)), 8);
            m_out.push_back(key ? 1 : 0);
            be(sampleSize, 4);
            m_out.insert(m_out.end(), buff + used, buff + used + sampleSize);
            used += sampleSize;
            ++indexCur;
        }
        m_indexAfter = indexCur;
        return static_cast<unsigned>(m_out.size());
    }

    void extractData(AVPacket* pkt, uint8_t* /*buff*/, const int /*size*/) override
    {
        if (!m_out.empty())
            memcpy(pkt->data, m_out.data(), m_out.size());
        pkt->size = static_cast<int>(m_out.size());
        m_sc->m_indexCur = m_indexAfter;
        m_configSent = true;
    }

   private:
    void put(const char* s, const size_t n) { m_out.insert(m_out.end(), s, s + n); }
    void be(const uint64_t v, const int bytes)
    {
        for (int i = bytes - 1; i >= 0; --i) m_out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    // a count of the track's time scale in INTERNAL_PTS_FREQ units, without overflowing for a fine time scale
    [[nodiscard]] int64_t internal(const int64_t t) const
    {
        const int64_t ts = m_sc->time_scale ? m_sc->time_scale : 1;
        return t / ts * INTERNAL_PTS_FREQ + t % ts * INTERNAL_PTS_FREQ / ts;
    }
    // The decode time, duration and composition offset of every sample, from stts and ctts
    void times()
    {
        if (m_timesDone)
            return;
        m_timesDone = true;
        int64_t t = 0;
        for (const auto& e : m_sc->stts_data)
            for (uint32_t i = 0; i < e.count && m_dts.size() < (1u << 26); ++i)
            {
                m_dts.push_back(t);
                m_dur.push_back(e.duration);
                t += e.duration;
            }
        // the samples of the fragments, after those of the moov: their times from tfdt and the runs
        m_dts.insert(m_dts.end(), m_sc->fragDts.begin(), m_sc->fragDts.end());
        m_dur.insert(m_dur.end(), m_sc->fragDur.begin(), m_sc->fragDur.end());
        for (const auto& e : m_sc->ctts_data)
            for (uint32_t i = 0; i < e.count && m_ctts.size() < m_dts.size(); ++i)
                m_ctts.push_back(static_cast<int32_t>(static_cast<uint32_t>(e.duration)));
    }

    MOVStreamContext* m_sc;
    uint32_t m_fourcc;
    std::vector<uint8_t> m_config;
    std::vector<uint8_t> m_out;
    std::vector<int64_t> m_dts, m_dur, m_ctts;
    bool m_timesDone = false;
    bool m_configSent = false;
    size_t m_indexAfter = 0;
};

// AV1 in an MP4 file (AV1 Codec ISO Media File Format Binding, 2.4): each sample is one temporal unit, its OBUs in the
// low overhead form with their sizes, and the temporal delimiters left out. The track had no newBufferSize, so the
// MP4 reader took every chunk to give no data at all: the track was listed as "Can't detect stream type" and could not
// be muxed. Each sample is now turned into the start code form the AV1 reader takes, behind a temporal delimiter as a
// Matroska block is, the configuration OBUs of av1C in front of the first. The samples of a chunk are told apart by
// their sizes in stsz, as for audio; newBufferSize works out the bytes and extractData hands them over.
class MovParsedAV1TrackData final : public ParsedTrackPrivData
{
   public:
    MovParsedAV1TrackData(MovDemuxer* /*demuxer*/, MOVStreamContext* sc) : m_sc(sc) {}

    void setPrivData(uint8_t* buff, const int size) override { m_configOBUs = av1_extract_priv_data(buff, size); }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        m_out.clear();
        size_t indexCur = m_sc->m_indexCur;
        if (!m_configSent)
            for (const auto& obu : m_configOBUs) m_out.insert(m_out.end(), obu.begin(), obu.end());
        uint32_t used = 0;
        while (used < size)
        {
            uint32_t sampleSize = size - used;
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur++];
            sampleSize = (std::min)(sampleSize, size - used);
            if (sampleSize == 0)
                break;
            static constexpr uint8_t TD_START[4] = {0x00, 0x00, 0x01, 0x10};
            m_out.insert(m_out.end(), TD_START, TD_START + 4);
            convertSample(buff + used, buff + used + sampleSize);
            used += sampleSize;
        }
        m_indexAfter = indexCur;
        return static_cast<unsigned>(m_out.size());
    }

    void extractData(AVPacket* pkt, uint8_t* /*buff*/, const int /*size*/) override
    {
        if (!m_out.empty())
            memcpy(pkt->data, m_out.data(), m_out.size());
        pkt->size = static_cast<int>(m_out.size());
        m_sc->m_indexCur = m_indexAfter;
        m_configSent = true;
    }

   private:
    // The OBUs of one sample: a start code, the header without its size field, the payload with emulation prevention.
    // A temporal delimiter the sample carries after all is left out, as one is put in front of every sample.
    void convertSample(const uint8_t* src, const uint8_t* end)
    {
        while (src < end)
        {
            Av1ObuHeader obuHdr;
            const int hdrLen = obuHdr.parse(src, end);
            if (hdrLen < 0)
                break;
            // The last OBU of a sample may leave out its size field and run to the end of the sample (AV1 in
            // ISOBMFF, 2.4); it was dropped, and with it the picture.
            int leb128Bytes = 0;
            const int64_t payloadSize =
                obuHdr.obu_has_size_field ? decodeLeb128(src + hdrLen, end, leb128Bytes) : end - (src + hdrLen);
            const uint8_t* payload = src + hdrLen + leb128Bytes;
            if (payloadSize < 0 || payload + payloadSize > end)
                break;
            if (obuHdr.obu_type != Av1ObuType::TEMPORAL_DELIMITER)
            {
                m_out.insert(m_out.end(), {0x00, 0x00, 0x01, static_cast<uint8_t>(src[0] & ~0x02)});
                if (obuHdr.obu_extension_flag)
                    m_out.push_back(src[1]);
                if (payloadSize > 0)
                {
                    const size_t at = m_out.size();
                    m_out.resize(at + static_cast<size_t>(payloadSize) * 2 + 16);
                    const int encoded = av1_add_emulation_prevention(payload, payload + payloadSize, m_out.data() + at,
                                                                     m_out.size() - at);
                    if (encoded > 0)
                        m_out.resize(at + encoded);
                    else
                    {
                        m_out.resize(at);
                        m_out.insert(m_out.end(), payload, payload + payloadSize);
                    }
                }
            }
            src = payload + payloadSize;
        }
    }

    MOVStreamContext* m_sc;
    std::vector<std::vector<uint8_t>> m_configOBUs;
    std::vector<uint8_t> m_out;  // the start code form of the chunk newBufferSize was given
    size_t m_indexAfter = 0;
    bool m_configSent = false;
};

// One MP4 sample is one packet of these codecs, and each is handed on in the form their reader takes from Matroska
// (matroskaParser.cpp): Opus with its OpusHead in front of the first packet and a 4 byte length in front of each;
// Vorbis and ALAC with a mark and the CodecPrivate in front of the first and a 4 byte length in front of each; VP8
// and VP9 each frame behind a start code, escaped, with a stop byte. The samples of a chunk are told apart by their
// sizes in stsz. newBufferSize works out the bytes, extractData hands them over.
class MovParsedFramedTrackData final : public ParsedTrackPrivData
{
   public:
    enum class Form
    {
        Opus,
        Vorbis,
        Alac,
        Vpx
    };
    MovParsedFramedTrackData(MOVStreamContext* sc, const Form form) : m_sc(sc), m_form(form) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        m_head.clear();
        if (!buff || size <= 0)
            return;
        if (m_form == Form::Opus)
        {
            // OpusSpecificBox (Encapsulation of Opus in ISO BMFF, 4.3.2): big endian, no magic. OpusHead (RFC 7845,
            // 5.1): the magic, version 1, little endian.
            if (size < 11)
                return;
            const auto le = [this](const uint32_t v, const int bytes)
            {
                for (int i = 0; i < bytes; ++i) m_head.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            m_head.assign({'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, buff[1]});
            le(static_cast<uint32_t>(buff[2]) << 8 | buff[3], 2);  // pre-skip
            le(static_cast<uint32_t>(buff[4]) << 24 | static_cast<uint32_t>(buff[5]) << 16 |
                   static_cast<uint32_t>(buff[6]) << 8 | buff[7],
               4);                                                 // input sample rate
            le(static_cast<uint32_t>(buff[8]) << 8 | buff[9], 2);  // output gain
            m_head.insert(m_head.end(), buff + 10, buff + size);   // mapping family and table
        }
        else if (m_form == Form::Alac)
        {
            // The alac box inside the sample entry, header, version and flags included, then the 24 byte
            // ALACSpecificConfig, which is what Matroska stores as its CodecPrivate
            const uint8_t* cfg = buff;
            int n = size;
            if (n >= 12 && memcmp(buff + 4, "alac", 4) == 0)
            {
                cfg += 12;
                n -= 12;
            }
            m_head.assign(cfg, cfg + n);
        }
        else
            m_head.assign(buff, buff + size);
    }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        m_out.clear();
        size_t indexCur = m_sc->m_indexCur;
        if (!m_headSent)
        {
            if (m_form == Form::Opus)
                m_out.insert(m_out.end(), m_head.begin(), m_head.end());
            else if (m_form == Form::Vorbis || m_form == Form::Alac)
            {
                const char* mark = m_form == Form::Vorbis ? "VORBISCP" : "ALACCPRV";
                m_out.insert(m_out.end(), mark, mark + 8);
                be32(static_cast<uint32_t>(m_head.size()));
                m_out.insert(m_out.end(), m_head.begin(), m_head.end());
            }
        }
        uint32_t used = 0;
        while (used < size)
        {
            uint32_t sampleSize = size - used;
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur++];
            sampleSize = (std::min)(sampleSize, size - used);
            if (sampleSize == 0)
                break;
            const uint8_t* s = buff + used;
            if (m_form == Form::Vpx)
            {
                m_out.insert(m_out.end(), {0x00, 0x00, 0x01});
                const size_t at = m_out.size();
                m_out.resize(at + sampleSize * 2 + 4);
                const int n = NALUnit::encodeNAL(s, s + sampleSize, m_out.data() + at, sampleSize * 2 + 4);
                m_out.resize(at + (n > 0 ? n : 0));
                m_out.push_back(0x80);
            }
            else
            {
                be32(sampleSize);
                m_out.insert(m_out.end(), s, s + sampleSize);
            }
            used += sampleSize;
        }
        m_indexAfter = indexCur;
        return static_cast<unsigned>(m_out.size());
    }

    void extractData(AVPacket* pkt, uint8_t* /*buff*/, const int /*size*/) override
    {
        if (!m_out.empty())
            memcpy(pkt->data, m_out.data(), m_out.size());
        pkt->size = static_cast<int>(m_out.size());
        m_sc->m_indexCur = m_indexAfter;
        m_headSent = true;
    }

   private:
    void be32(const uint32_t v)
    {
        for (int i = 3; i >= 0; --i) m_out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    MOVStreamContext* m_sc;
    Form m_form;
    std::vector<uint8_t> m_head;  // OpusHead, or the CodecPrivate of Vorbis and ALAC
    std::vector<uint8_t> m_out;
    size_t m_indexAfter = 0;
    bool m_headSent = false;
};

// Linear PCM, as the LPCM reader takes it from Matroska: a WAVE header in front of the first chunk, then the samples
// little endian. Floating point samples are not read, as from Matroska.
class MovParsedPCMTrackData final : public ParsedTrackPrivData
{
   public:
    MovParsedPCMTrackData(MOVStreamContext* sc, const uint32_t format) : m_sc(sc), m_format(format) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        // pcmC (ISO/IEC 23003-5, 5.2): version and flags, format_flags (bit 0: little endian), PCM_sample_size
        if (m_format == MKTAG('i', 'p', 'c', 'm') && buff && size >= 6)
        {
            m_sc->pcmBigEndian = (buff[4] & 1) == 0;
            m_sc->bits_per_coded_sample = buff[5];
        }
    }

    unsigned newBufferSize(uint8_t* /*buff*/, const unsigned size) override
    {
        prepare();
        return size + static_cast<unsigned>(m_wave.size());
    }

    // The bytes of one sample of every channel, as prepare takes the format
    [[nodiscard]] unsigned bytesPerFrame() const
    {
        unsigned bits = m_sc->bits_per_coded_sample;
        if (m_format == MKTAG('i', 'n', '2', '4'))
            bits = 24;
        else if (m_format == MKTAG('i', 'n', '3', '2'))
            bits = 32;
        return m_sc->channels * (bits / 8);
    }

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        prepare();
        uint8_t* dst = pkt->data;
        if (!m_wave.isEmpty())
        {
            memcpy(dst, m_wave.data(), m_wave.size());
            dst += m_wave.size();
            m_wave.clear();
            m_waveSent = true;
        }
        if (m_sc->pcmBigEndian && m_bits > 8)
            wave_format::toLittleEndian(dst, buff, size, m_bits);
        else
            memcpy(dst, buff, size);
        pkt->size = static_cast<int>(dst - pkt->data) + size;
    }

   private:
    void prepare()
    {
        if (m_waveSent || !m_wave.isEmpty())
            return;
        m_bits = static_cast<uint16_t>(m_sc->bits_per_coded_sample);
        if (m_format == MKTAG('i', 'n', '2', '4'))
            m_bits = 24;
        else if (m_format == MKTAG('i', 'n', '3', '2'))
            m_bits = 32;
        if (m_format == MKTAG('l', 'p', 'c', 'm'))
        {
            m_sc->pcmFloat = m_sc->lpcmFlags & 1;
            m_sc->pcmBigEndian = (m_sc->lpcmFlags & 2) != 0;
        }
        if (m_sc->pcmFloat)
            THROW(ERR_MOV_PARSE, "MP4/MOV: floating point PCM is not supported")
        const auto channels = static_cast<uint16_t>(m_sc->channels);
        wave_format::buildWaveHeader(m_wave, m_sc->sample_rate, channels, channels >= 6, m_bits);
    }
    MOVStreamContext* m_sc;
    uint32_t m_format;
    uint16_t m_bits = 16;
    MemoryBlock m_wave;
    bool m_waveSent = false;
};

class MovParsedSRTTrackData final : public ParsedTrackPrivData
{
   public:
    // wvtt: the samples are WebVTT in ISO BMFF (ISO/IEC 14496-30) instead of 3GPP timed text (tx3g)
    MovParsedSRTTrackData(MovDemuxer* demuxer, MOVStreamContext* sc, const bool wvtt = false)
        : m_buff(), m_size(0), m_demuxer(demuxer), m_sc(sc), sttsCnt(0), m_wvtt(wvtt)
    {
        m_packetCnt = 0;
        sttsPos = 0;
        m_timeOffset = 0;
    }

    int64_t getSttsVal()
    {
        if (sttsCnt == 0)
        {
            sttsPos++;
            if (sttsPos >= static_cast<unsigned>(m_sc->stts_data.size()))
                THROW(ERR_MOV_PARSE, "MP4/MOV error: invalid stts index for SRT track #"
                                         << m_sc->ffindex << " at position " << m_demuxer->getProcessedBytes())

            sttsCnt = m_sc->stts_data[sttsPos].count;
        }
        sttsCnt--;
        // The text track's own time scale, from its media header. Zero ended the program on the spot
        // (a hardware divide error), even when the file was only being listed.
        if (m_sc->time_scale == 0)
            THROW(ERR_MOV_PARSE,
                  "MP4/MOV error: text track #" << m_sc->ffindex << " has a time scale of 0 in its media header (mdhd)")
        return m_sc->stts_data[sttsPos].duration;  // in the track's time scale
    }

    void setPrivData(uint8_t* buff, const int size) override
    {
        m_buff = buff;
        m_size = size;
        sttsCnt = 0;
        sttsPos = -1;
    }

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        const std::string cues = renderChunk(buff, static_cast<uint32_t>(size), true);
        memcpy(pkt->data, cues.data(), cues.size());
    }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        const std::string cues = renderChunk(buff, size, false);
        // A chunk of samples without text produces nothing, and then extractData is not called for
        // it: its samples are consumed here, or every later chunk is read with stale sizes and times
        if (cues.empty())
            renderChunk(buff, size, true);
        return static_cast<unsigned>(cues.size());
    }

   private:
    // The samples of one chunk, as SRT cues. A chunk can hold several samples, among them samples
    // without text that only mark time, and each sample has its own duration in stts. The whole
    // chunk used to be taken as ONE sample: the text of the first sample with text got the times of
    // the empty sample before it, the samples after it were lost, and their bytes were read as a
    // style box, whose length could send the reader backwards and loop for ever. The sample sizes
    // come from stsz, as for the audio above. With commit false nothing is consumed, so the size
    // pass and the copy pass see the same samples.
    std::string renderChunk(const uint8_t* buff, const uint32_t size, const bool commit)
    {
        const int64_t savedSttsPos = sttsPos;
        const int64_t savedSttsCnt = sttsCnt;
        size_t indexCur = m_sc->m_indexCur;
        int64_t timeOffset = m_timeOffset;
        int counter = m_packetCnt;
        std::string out;
        uint32_t used = 0;
        // Where the track starts against the first video track: an empty edit in front of a text track delays every
        // cue (a cue due at 5 s after an empty edit of 5 s came at 0). A text track gets no Stream
        // delay in the listing, so nothing else moves it.
        const int64_t startNs = m_demuxer->trackStartNs(static_cast<int32_t>(m_sc->ffindex));
        const int64_t startMs = (startNs >= 0 ? startNs + 500000 : startNs - 500000) / 1000000;
        while (used < size)
        {
            uint32_t sampleSize = size - used;  // without a size table, what is left, as before
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur++];
            sampleSize = (std::min)(sampleSize, size - used);
            // The times are added up in the track's own time scale and only each cue's time is rounded to
            // milliseconds: every duration was cut to whole milliseconds and then added, so the cues came earlier
            // and earlier (480 ms by the 1200th subtitle at a time scale of 600).
            const int64_t endUnits = timeOffset + getSttsVal();
            const auto toMs = [this](const int64_t units)
            { return (units * 1000 + m_sc->time_scale / 2) / m_sc->time_scale; };
            const int64_t startTime = (std::max)(int64_t{0}, toMs(timeOffset) + startMs);
            const int64_t endTime = (std::max)(int64_t{0}, toMs(endUnits) + startMs);
            std::string cue = m_wvtt
                                  ? webVttCueFor(buff + used, buff + used + sampleSize, startTime, endTime, counter + 1)
                                  : cueFor(buff + used, buff + used + sampleSize, startTime, endTime, counter + 1);
            if (!cue.empty())
            {
                if (counter == 0)
                    out += "\xEF\xBB\xBF";  // UTF-8 header
                out += cue;
                ++counter;
            }
            timeOffset = endUnits;
            used += sampleSize;
            if (sampleSize == 0 && m_sc->sample_size == 0 && indexCur >= m_sc->m_index.size())
                break;  // a zero size with no table left cannot advance
        }
        if (commit)
        {
            m_sc->m_indexCur = indexCur;
            m_timeOffset = timeOffset;
            m_packetCnt = counter;
        }
        else
        {
            sttsPos = savedSttsPos;
            sttsCnt = savedSttsCnt;
        }
        return out;
    }

    // One WebVTT sample as an SRT cue: a sample is either a vtte box (no cue for that time) or one or more vttc boxes
    // that are shown together, each with its text in a payl box. Their texts become one cue, a line each; the cue
    // settings (sttg), identifiers (iden) and comments (vtta) have no place in SRT, and the times inside a cue are
    // dropped with the other tags SRT does not have. A cue that overlaps another is split by the format into samples
    // that do not, so it comes out as more than one cue with the same text.
    static std::string webVttCueFor(const uint8_t* buff, const uint8_t* end, const int64_t startTime,
                                    const int64_t endTime, const int counter)
    {
        const auto be32 = [](const uint8_t* p)
        { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | p[2] << 8 | p[3]; };
        std::string text;
        for (const uint8_t* p = buff; end - p >= 8;)
        {
            const uint32_t len = be32(p);
            if (len < 8 || len > static_cast<uint32_t>(end - p))
                break;
            if (be32(p + 4) == 0x76747463)  // 'vttc'
                for (const uint8_t* q = p + 8; p + len - q >= 8;)
                {
                    const uint32_t clen = be32(q);
                    if (clen < 8 || clen > static_cast<uint32_t>(p + len - q))
                        break;
                    if (be32(q + 4) == 0x7061796C)  // 'payl'
                    {
                        std::string line = webVttToSrtText(std::string(reinterpret_cast<const char*>(q + 8), clen - 8));
                        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                        if (!line.empty())
                            text += (text.empty() ? "" : "\n") + line;
                    }
                    q += clen;
                }
            p += len;
        }
        if (text.empty())
            return {};
        std::string cue = int32ToStr(counter);
        cue += "\n";
        cue += floatToTime(static_cast<double>(startTime) / 1000.0, ',');
        cue += " --> ";
        cue += floatToTime(static_cast<double>(endTime) / 1000.0, ',');
        cue += "\n";
        cue += text;
        cue += "\n\n";
        return cue;
    }

    // One sample as an SRT cue with the styles it carries, or nothing for a sample without text
    static std::string cueFor(const uint8_t* buff, const uint8_t* end, const int64_t startTime, const int64_t endTime,
                              const int counter)
    {
        if (end - buff < 2)
            return {};
        const auto textLen = static_cast<size_t>(buff[0] << 8 | buff[1]);
        buff += 2;
        if (textLen == 0 || textLen > static_cast<size_t>(end - buff))
            return {};
        std::string text(reinterpret_cast<const char*>(buff), textLen);
        buff += textLen;
        std::vector<pair<int, string>> tags;
        while (end - buff >= 8)
        {
            int64_t boxLen = static_cast<int64_t>(buff[0]) << 24 | buff[1] << 16 | buff[2] << 8 | buff[3];
            const uint32_t boxType = static_cast<uint32_t>(buff[4]) << 24 | buff[5] << 16 | buff[6] << 8 | buff[7];
            const uint8_t* body = buff + 8;
            if (boxLen == 1)  // 64-bit length
            {
                if (end - buff < 16)
                    break;
                boxLen = 0;
                for (int i = 0; i < 8; i++) boxLen = boxLen << 8 | buff[8 + i];
                body = buff + 16;
            }
            // A box shorter than its own header, or longer than what is left, ends the reading
            if (boxLen < body - buff || boxLen > end - buff)
                break;
            const uint8_t* boxEnd = buff + boxLen;
            if (boxType == 0x7374796C && boxEnd - body >= 2)  // 'styl' box
            {
                const auto entryCount = static_cast<uint16_t>(body[0] << 8 | body[1]);
                const uint8_t* rec = body + 2;
                // startChar, endChar, font-ID, face style flags, font-size, text-color-rgba[4]
                for (uint16_t i = 0; i < entryCount && boxEnd - rec >= 12; ++i, rec += 12)
                {
                    const auto startChar = static_cast<uint16_t>(rec[0] << 8 | rec[1]);
                    const auto endChar = static_cast<uint16_t>(rec[2] << 8 | rec[3]);
                    const uint8_t face = rec[6];
                    if (startChar >= endChar)
                        continue;
                    std::string open;
                    std::string close;
                    if (face & 1)
                    {
                        open += "<b>";
                        close.insert(0, "</b>");
                    }
                    if (face & 2)
                    {
                        open += "<i>";
                        close.insert(0, "</i>");
                    }
                    if (face & 4)
                    {
                        open += "<u>";
                        close.insert(0, "</u>");
                    }
                    tags.insert(tags.begin(), std::make_pair(startChar, open));
                    tags.emplace_back(endChar, close);
                }
            }
            buff = boxEnd;
        }
        if (!tags.empty())
        {
            // The styl positions count characters, not bytes: with any letter outside ASCII the tags were put inside
            // a character, which broke it, and onto the wrong letters. Each position becomes the byte where its
            // character begins in the UTF-8 text.
            std::vector<size_t> charStart;
            for (size_t i = 0; i < text.size(); ++i)
                if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80)
                    charStart.push_back(i);
            charStart.push_back(text.size());
            for (auto& [pos, tag] : tags)
                pos = static_cast<int>((std::min)(static_cast<size_t>(pos), charStart.size() - 1));
            sort(tags.begin(), tags.end(), greater<>());
            for (const auto& [pos, tag] : tags) text.insert(charStart[pos], tag);
        }
        return int32ToStr(counter) + "\n" + floatToTime(static_cast<double>(startTime) / 1e3, ',') + " --> " +
               floatToTime(static_cast<double>(endTime) / 1e3, ',') + "\n" + text + "\n\n";
    }

   private:
    uint8_t* m_buff;
    int m_size;
    MovDemuxer* m_demuxer;
    MOVStreamContext* m_sc;
    int m_packetCnt;
    int64_t sttsPos;
    int64_t sttsCnt;
    int64_t m_timeOffset;  // in the track's time scale
    bool m_wvtt;
};

MovDemuxer::MovDemuxer(const BufferedReaderManager& readManager)
    : IOContextDemuxer(readManager), m_mdat_size(0), m_fileSize(0), m_timescale(0), fragment()
{
    found_moov = 0;
    found_moof = false;
    m_mdat_pos = 0;
    itunes_metadata = 0;
    moof_offset = 0;
    fileDuration = 0;
    isom = 0;
    m_curChunk = 0;
    m_firstDemux = true;
    m_readerUsed = false;
    m_fileIterator = nullptr;
    m_firstHeaderSize = 0;
}

// A reader with nothing in flight and no end of file behind it. Both calls take the reader lock,
// and deleteReader leaves a reader alone until its outstanding request has been served, so this is
// safe while the muxer is running. Reader ids are never reused.
void MovDemuxer::takeFreshReader()
{
    if (m_readerUsed)
    {
        m_bufferedReader->deleteReader(m_readerID);
        m_readerID = m_bufferedReader->createReader(TS_FRAME_SIZE);
    }
    m_readerUsed = true;
}

// A seek on the reader that has been reading the file races with the read it may still have in flight: gotoByte
// clears the end of file, the late read lands afterwards and sets it again (measured in openFile below). The header
// walk then stopped there with "moov atom not found" on a file whose moov follows a long mdat, in two or three runs of
// ten. The seek is made on a reader with nothing in flight.
void MovDemuxer::seekOnFreshReader(const int64_t pos)
{
    takeFreshReader();
    if (!m_bufferedReader->openStream(m_readerID, m_fileName.c_str()))
        THROW(ERR_FILE_NOT_FOUND, "Can't open stream " << m_fileName)
    url_fseek(pos);
}

void MovDemuxer::readClose()
{
    // Nothing used to free these. Every mp4 opened leaked its track contexts and the vectors
    // inside them, which a sanitizer put at about 13 KB for a single file. On the probe path the
    // process exits immediately afterwards so it never showed, but it leaked during a real mux
    // too, and once per file in a join.
    //
    // Cast back to what was allocated rather than deleting through Track*. Track has no virtual
    // destructor, so deleting through the base would be undefined and would leave the vectors
    // inside MOVStreamContext untouched. Only mov_read_trak fills this array and it only ever
    // puts MOVStreamContext in it.
    for (int i = 0; i < num_tracks; ++i)
    {
        delete static_cast<MOVStreamContext*>(tracks[i]);
        tracks[i] = nullptr;
    }
    num_tracks = 0;
}

void MovDemuxer::openFile(const std::string& streamName)
{
    m_fileName = streamName;
    found_moov = 0;
    // Every file of a join has its own movie header, and its own edit lists to be timed with it.
    m_timescale = 0;
    m_emptyEditUnits.clear();
    // And its own fragment defaults and media boxes: those of the file before it stayed in the lists, so a later file
    // could take its fragment defaults, or the size of a chunk, from the file before.
    trex_data.clear();
    m_mdat_data.clear();
    found_moof = false;
    m_mdat_pos = 0;
    itunes_metadata = 0;
    moof_offset = 0;
    fileDuration = 0;
    isom = 0;
    m_curChunk = 0;
    m_firstDemux = true;

    m_curPos = m_bufEnd = nullptr;
    m_processedBytes = 0;
    m_isEOF = false;

    // Free what the last file left behind BEFORE the count is cleared. The other order meant
    // readClose saw no tracks and released nothing, so reopening leaked the lot.
    readClose();

    // Every file of a join is read by a reader of its own. Reopening the stream on the reader that
    // had just finished the file before left that file's end of file behind, and the new one then
    // read as though it were already finished: the mdat was skipped only part way and the moov
    // atom after it was never reached. Whether that happened depended on where the read ahead had
    // got to, so the same command worked on one file and failed on another of a different length.
    takeFreshReader();

    if (!m_bufferedReader->openStream(m_readerID, streamName.c_str()))
        THROW(ERR_FILE_NOT_FOUND, "Can't open stream " << streamName)

    File tmpFile;
    tmpFile.open(streamName.c_str(), File::ofRead);
    tmpFile.size(&m_fileSize);
    tmpFile.close();

    m_processedBytes = 0;
    m_isEOF = false;
    m_firstTfdt.clear();
    m_firstCompositionOffset.clear();
    m_videoMediaTime.clear();
    m_trackStartNs.clear();  // each file of a join starts its tracks on its own
    readHeaders();
    if (found_moof)
        fragmentStartDelays();
    longFirstSamples();
    pictureLengths();
    if (m_mdat_pos && m_processedBytes != m_mdat_pos)
    {
        // Reading the header walked to the end of the file, because a moov atom usually sits
        // there, so this reader has a read of its own still in flight and is about to be told the
        // file is finished. Seeking it back races with that: gotoByte clears the end of file, the
        // late read lands afterwards and sets it again, and the pass that follows then stops after
        // a single block. Measured on the same file, both ways round:
        //
        //   the read finished first   gotoByte saw atQueue=0 eof=1, cleared it, 1494 frames
        //   the read was still going  gotoByte saw atQueue=1 eof=0, cleared nothing, 1207 frames
        //
        // A reader that has not been asked for anything yet has nothing in flight to come back.
        seekOnFreshReader(m_mdat_pos);
    }
    buildIndex();
    m_firstHeaderSize = m_processedBytes;
    // A moov after the mdat was read by the walk above and then stepped back over, so the demux never reaches it.
    // Left out of the count, it made a complete mux read as stopped early once the moov was over 1 % of the file
    // (#39). Only what the walk read is counted: a file whose walk ended at its first mdat may hold more media after
    // it that is never read, and the count has to keep saying so.
    if (!found_moof && m_headerWalkEnd > m_mdat_pos + m_mdat_size)
        m_firstHeaderSize += m_headerWalkEnd - (m_mdat_pos + m_mdat_size);
    // What follows both the media and the last box the walk read is never read, and it holds no media of this file:
    // buildIndex refuses a chunk that lies outside the mdat boxes, and the media is read up to the end of the last
    // chunk. Only boxes such as free, skip or uuid were counted (#43). The unused rest of a second mdat, a box after
    // it, a box whose size runs past the end of the file, an mdat no chunk uses and a moof without an mvex in the moov
    // were not, and a complete mux of such a file said it stopped before the source was finished: 98.0 percent read
    // with 2 % of free bytes after a second mdat, 95.0 percent with an unused mdat of 5 % after the moov, the output
    // the same byte for byte as that of the file without them.
    if (!found_moof && m_headerWalkEnd > 0)
    {
        const int64_t readEnd = (std::max)(m_headerWalkEnd, m_mdat_pos + m_mdat_size);
        if (readEnd < m_fileSize)
        {
            m_firstHeaderSize += m_fileSize - readEnd;
            warnUnreadBoxes(readEnd);
        }
    }
}

// Every mdat at the top level of the file, as the position and size of its contents, read by the box headers alone.
std::vector<std::pair<int64_t, int64_t>> MovDemuxer::topLevelMdats() const
{
    std::vector<std::pair<int64_t, int64_t>> out;
    File f;
    if (!f.open(m_fileName.c_str(), File::ofRead))
        return out;
    int64_t pos = 0;
    while (pos + 8 <= m_fileSize)
    {
        uint8_t h[16];
        if (f.seek(pos, File::SeekMethod::smBegin) == -1 || f.read(h, 8) != 8)
            break;
        uint64_t size = static_cast<uint64_t>(h[0]) << 24 | static_cast<uint64_t>(h[1]) << 16 |
                        static_cast<uint64_t>(h[2]) << 8 | h[3];
        int header = 8;
        if (size == 1)
        {
            if (f.read(h + 8, 8) != 8)
                break;
            size = 0;
            for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
            header = 16;
        }
        else if (size == 0)
            size = static_cast<uint64_t>(m_fileSize - pos);
        if (size < static_cast<uint64_t>(header))
            break;
        size = (std::min)(size, static_cast<uint64_t>(m_fileSize - pos));  // a box past the end ends there (#42)
        if (MKTAG(h[4], h[5], h[6], h[7]) == MKTAG('m', 'd', 'a', 't'))
            out.emplace_back(pos + header, static_cast<int64_t>(size) - header);
        pos += static_cast<int64_t>(size);
    }
    return out;
}

// The boxes from `from` on are not read. An mdat or a moof there holds no chunk the moov lists, but its bytes may be
// media all the same (an index that lost tracks, fragments without the mvex that would describe them), and a box that
// says it is longer than the rest of the file is a sign of a file cut off: a warning says so, once a file.
void MovDemuxer::warnUnreadBoxes(const int64_t from) const
{
    static std::set<std::pair<std::string, int64_t>> warned;  // a job opens a file more than once
    File f;
    if (!f.open(m_fileName.c_str(), File::ofRead))
        return;
    int64_t pos = 0;
    while (pos + 8 <= m_fileSize)
    {
        uint8_t h[16];
        if (f.seek(pos, File::SeekMethod::smBegin) == -1 || f.read(h, 8) != 8)
            return;
        uint64_t size = static_cast<uint64_t>(h[0]) << 24 | static_cast<uint64_t>(h[1]) << 16 |
                        static_cast<uint64_t>(h[2]) << 8 | h[3];
        const uint32_t type = MKTAG(h[4], h[5], h[6], h[7]);
        int header = 8;
        if (size == 1)
        {
            if (f.read(h + 8, 8) != 8)
                return;
            size = 0;
            for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
            header = 16;
        }
        else if (size == 0)
            size = static_cast<uint64_t>(m_fileSize - pos);
        if (size < static_cast<uint64_t>(header))
            return;
        if (pos >= from && (type == MKTAG('m', 'o', 'o', 'f') ||
                            (type == MKTAG('m', 'd', 'a', 't') && size > static_cast<uint64_t>(header))))
        {
            if (warned.emplace(m_fileName, pos).second)
                LTRACE(LT_WARN, 2,
                       "Warning: the MP4 box '" << std::string(reinterpret_cast<const char*>(h + 4), 4) << "' at byte "
                                                << pos << " holds no sample the moov lists; it is not read.");
            return;
        }
        if (size > static_cast<uint64_t>(m_fileSize - pos))
        {
            if (pos >= from && warned.emplace(m_fileName, pos).second)
                LTRACE(LT_WARN, 2,
                       "Warning: the MP4 box '" << std::string(reinterpret_cast<const char*>(h + 4), 4) << "' at byte "
                                                << pos << " says it is " << size << " bytes long, "
                                                << size - static_cast<uint64_t>(m_fileSize - pos)
                                                << " more than the file holds; it holds no media and is not read.");
            return;
        }
        pos += static_cast<int64_t>(size);
    }
}

// The byte size of each chunk of a track by its own tables: stsc gives the samples of each chunk, stsz the size of
// each sample. -1 where they do not say: a track with one size for every sample (raw audio counts its samples in
// units that are not bytes), or tables that run out.
static std::vector<int64_t> ownChunkBytes(const MOVStreamContext* st)
{
    std::vector<int64_t> bytes(st->chunk_offsets.size(), -1);
    // Linear PCM whose one sample size is the bytes of a sample of every channel counts its samples in bytes after
    // all. Read up to the next chunk, bytes between two chunks went out as sound: 3 bytes after one chunk of 16-bit
    // stereo shifted every later sample by a byte and a half, with no word.
    const auto pcm = dynamic_cast<const MovParsedPCMTrackData*>(st->parsed_priv_data);
    const bool pcmBytes = pcm && st->sample_size != 0 && st->sample_size == pcm->bytesPerFrame();
    if ((st->sample_size != 0 && !pcmBytes) || st->stsc_data.empty())
        return bytes;
    size_t sample = 0;
    for (size_t e = 0; e < st->stsc_data.size(); ++e)
    {
        const size_t first = st->stsc_data[e].first;  // 1-based
        const size_t last =
            e + 1 < st->stsc_data.size() ? st->stsc_data[e + 1].first : st->chunk_offsets.size() + 1;  // exclusive
        if (first == 0 || last < first)
            return bytes;
        for (size_t c = first; c < last && c <= bytes.size(); ++c)
        {
            const size_t n = st->stsc_data[e].count;
            if (pcmBytes)
            {
                bytes[c - 1] = static_cast<int64_t>(n) * st->sample_size;
                continue;
            }
            if (sample + n > st->m_index.size())
                return bytes;
            int64_t sum = 0;
            for (size_t s = sample; s < sample + n; ++s) sum += st->m_index[s];
            bytes[c - 1] = sum;
            sample += n;
        }
    }
    return bytes;
}

void MovDemuxer::buildIndex()
{
    m_curChunk = 0;
    chunks.clear();
    m_chunkBytes.clear();

    if (num_tracks == 1 && reinterpret_cast<MOVStreamContext*>(tracks[0])->chunk_offsets.empty())
    {
        chunks.emplace_back(0, 0);
    }
    else
    {
        std::map<std::pair<int64_t, int64_t>, int64_t> ownBytes;
        // A file may keep its media in more than one mdat, one for each track for instance (#388). Only the first was
        // known: a chunk in any other stopped the file with "Invalid chunk offset", and the walk does not even reach a
        // second mdat that follows a moov at the start. Every mdat at the top level is known now, a chunk has to lie
        // in one of them, and the media is read from the first mdat to the end of the last chunk; what lies between
        // the chunks (the header of the next mdat) is stepped over as before.
        const std::vector<std::pair<int64_t, int64_t>> mdats =
            found_moof ? std::vector<std::pair<int64_t, int64_t>>() : topLevelMdats();
        int64_t mediaEnd = m_mdat_pos + m_mdat_size;
        for (int i = 0; i < num_tracks; ++i)
        {
            const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
            const std::vector<int64_t> bytes = found_moof ? std::vector<int64_t>() : ownChunkBytes(st);
            for (size_t k = 0; k < st->chunk_offsets.size(); ++k)
            {
                const int64_t j = st->chunk_offsets[k];
                if (!found_moof && (j < m_mdat_pos || j > m_mdat_pos + m_mdat_size))
                {
                    const auto in = std::find_if(mdats.begin(), mdats.end(), [j](const auto& m)
                                                 { return j >= m.first && j <= m.first + m.second; });
                    if (in == mdats.end())
                        THROW(ERR_MOV_PARSE, "Invalid chunk offset " << j)
                    // where its own samples end, or its mdat where its tables do not say (-1: all samples one size)
                    const int64_t own = k < bytes.size() && bytes[k] >= 0 ? bytes[k] : in->second;
                    mediaEnd = (std::max)(mediaEnd, (std::min)(in->first + in->second, j + own));
                }
                chunks.emplace_back(j - m_mdat_pos, i);
                if (k < bytes.size())
                    ownBytes[chunks.back()] = bytes[k];
            }
        }
        if (!found_moof)
            m_mdat_size = mediaEnd - m_mdat_pos;
        sort(chunks.begin(), chunks.end());
        for (const auto& c : chunks)
        {
            const auto it = ownBytes.find(c);
            m_chunkBytes.push_back(it == ownBytes.end() ? -1 : it->second);
        }
    }
}

void MovDemuxer::readHeaders()
{
    // check MOV header
    MOVAtom atom;
    atom.size = LLONG_MAX;
    m_mdat_pos = 0;
    m_headerWalkEnd = 0;
    if (mov_read_default(atom) < 0)
        THROW(ERR_MOV_PARSE, "error reading header")
    if (!found_moov)
        THROW(ERR_MOV_PARSE, "moov atom not found")
}

int MovDemuxer::simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
{
    for (int acceptedPID : acceptedPIDs) demuxedData[acceptedPID];
    discardSize = m_firstHeaderSize;
    m_firstHeaderSize = 0;
    if (m_firstDemux)
    {
        m_firstDemux = false;
        const int64_t beforeHeadersPos = m_processedBytes;
        if (m_mdat_pos == 0)
        {
            readHeaders();
            if (m_lastReadRez == BufferedReader::DATA_EOF)
                return m_lastReadRez;
            buildIndex();
            if (m_mdat_pos && m_processedBytes != m_mdat_pos)
                seekOnFreshReader(m_mdat_pos);
        }
        discardSize += m_mdat_pos - beforeHeadersPos;
        if (!chunks.empty())
        {
            discardSize += chunks[m_curChunk].first;
            skip_bytes(chunks[m_curChunk].first);
        }
    }
    const int64_t startPos = m_processedBytes;
    while (m_processedBytes - startPos < m_fileBlockSize && m_curChunk < chunks.size())
    {
        const int64_t offset = chunks[m_curChunk].first;
        // The last chunk ends the file, fragmented or not: openFile's walk has already read every moof of a fragmented
        // file. Going back into readHeaders after it, as was done for fragmented files, read the boxes after the last
        // mdat (an mfra) a second time, rebuilt the index and took the whole file off the count, and at the end of the
        // file it returned end of stream from above the code that asks the file list for the next name.
        const int64_t next = m_curChunk < chunks.size() - 1 ? chunks[m_curChunk + 1].first : m_mdat_size;
        auto chunkSize = static_cast<int>(
            found_moof ? fragmentChunkSize(m_mdat_pos, offset, next, m_curChunk + 1 == chunks.size()) : next - offset);
        // A chunk ends where its own samples end when its track's tables say so and that comes first. The distance to
        // the next chunk of any track alone took in the data of a track whose tables were lost (a damaged box before
        // them), and the AAC reader then stopped the job with "Out of index". The bytes up to the next chunk are
        // skipped below.
        const int trackId = static_cast<int>(chunks[m_curChunk].second);
        if (!found_moof && m_curChunk < m_chunkBytes.size() && m_chunkBytes[m_curChunk] >= 0 &&
            m_chunkBytes[m_curChunk] < chunkSize)
        {
            const auto sc = reinterpret_cast<MOVStreamContext*>(tracks[trackId]);
            static std::set<std::pair<std::string, int>> told;  // the file is opened more than once in a job
            if (m_curChunk + 1 < chunks.size() && dynamic_cast<MovParsedPCMTrackData*>(sc->parsed_priv_data) &&
                told.emplace(m_fileName, trackId).second)
                LTRACE(LT_WARN, 2,
                       "Warning: audio track #" << sc->ffindex << " of the MP4 file has "
                                                << chunkSize - m_chunkBytes[m_curChunk]
                                                << " bytes after the samples of a chunk; they are left out.");
            chunkSize = static_cast<int>(m_chunkBytes[m_curChunk]);
        }
        auto filterItr = m_pidFilters.find(trackId + 1);
        if (filterItr == m_pidFilters.end() && acceptedPIDs.find(trackId + 1) == acceptedPIDs.end())
        {
            discardSize += chunkSize;
            skip_bytes(chunkSize);
        }
        else if (chunkSize)
        {
            MemoryBlock& vect = demuxedData[trackId + 1];
            const auto st = reinterpret_cast<MOVStreamContext*>(tracks[trackId]);
            const size_t oldSize = vect.size();
            if (st->parsed_priv_data)
            {
                if (static_cast<size_t>(chunkSize) > m_tmpChunkBuffer.size())
                    m_tmpChunkBuffer.resize(chunkSize);
                const unsigned readed = get_buffer(m_tmpChunkBuffer.data(), chunkSize);
                if (readed == 0)
                    break;
                // Parse what arrived, not what was asked for. get_buffer fills what it can and
                // returns the count, and the branches below all check it. This one did not, so a
                // chunk that ran past the end of the data was parsed together with whatever the
                // shared buffer still held from the chunk before it, and those bytes were read as
                // NAL lengths.
                const auto usable = static_cast<int>(readed);
                m_deliveredPacket.size =
                    static_cast<int32_t>(st->parsed_priv_data->newBufferSize(m_tmpChunkBuffer.data(), usable));
                if (m_deliveredPacket.size)
                {
                    if (filterItr != m_pidFilters.end())
                    {
                        m_filterBuffer.resize(m_deliveredPacket.size);
                        m_deliveredPacket.data = m_filterBuffer.data();
                        st->parsed_priv_data->extractData(&m_deliveredPacket, m_tmpChunkBuffer.data(), usable);
                        const int demuxed =
                            filterItr->second->demuxPacket(demuxedData, acceptedPIDs, m_deliveredPacket);
                        discardSize += static_cast<int64_t>(chunkSize) - demuxed;
                    }
                    else
                    {
                        discardSize += static_cast<int64_t>(chunkSize) - m_deliveredPacket.size;
                        vect.grow(m_deliveredPacket.size);
                        m_deliveredPacket.data = vect.data() + oldSize;
                        st->parsed_priv_data->extractData(&m_deliveredPacket, m_tmpChunkBuffer.data(), usable);
                    }
                }
                else
                {
                    discardSize += chunkSize;
                }
            }
            else
            {
                if (filterItr != m_pidFilters.end())
                {
                    m_filterBuffer.resize(chunkSize);
                    const int readed = static_cast<int>(get_buffer(m_filterBuffer.data(), chunkSize));
                    if (readed < chunkSize)
                        m_filterBuffer.grow(readed - chunkSize);
                    if (readed == 0)
                        break;
                    m_deliveredPacket.data = m_filterBuffer.data();
                    m_deliveredPacket.size = static_cast<int>(m_filterBuffer.size());
                    const int demuxed = filterItr->second->demuxPacket(demuxedData, acceptedPIDs, m_deliveredPacket);
                    discardSize += static_cast<int64_t>(chunkSize) - demuxed;
                }
                else
                {
                    vect.grow(chunkSize);
                    const int readed = static_cast<int>(get_buffer(vect.data() + oldSize, chunkSize));
                    if (readed < chunkSize)
                    {
                        vect.grow(readed - chunkSize);
                    }
                    if (readed == 0)
                        break;
                }
            }
        }
        // In a fragmented file the bytes up to the next chunk are the moof and mdat headers of the next fragment. They
        // were stepped over without being counted, which is what kept a file of many small fragments below 99 % (#39).
        if (m_curChunk < chunks.size() - 1 && next - offset > chunkSize)
        {
            discardSize += next - offset - chunkSize;
            skip_bytes(next - offset - chunkSize);
        }
        // After the last chunk the mdat may still hold bytes no sample uses, which a writer can leave there. They are
        // never read, and left out of the count they made a complete mux read as stopped early once they were over
        // 1 % of the file.
        else if (!found_moof && m_curChunk == chunks.size() - 1 && next - offset > chunkSize)
            discardSize += next - offset - chunkSize;
        m_curChunk++;
        // What follows the last chunk of a fragmented file (an mfra) holds no media when the walk read every box up to
        // the end of the file, so it counts as read. A walk that stopped short leaves it out.
        if (m_curChunk == chunks.size() && found_moof && m_headerWalkEnd >= m_fileSize && m_processedBytes < m_fileSize)
            discardSize += m_fileSize - m_processedBytes;
    }

    if (m_processedBytes > startPos)
        return 0;
    if (m_fileIterator)
    {
        const std::string nextName = m_fileIterator->getNextName();
        if (!nextName.empty())
        {
            accountJoinPart();
            ++m_joinPart;
            openFile(nextName);
            dropJoinPriming();
            return 0;
        }
    }

    m_lastReadRez = BufferedReader::DATA_EOF;
    return m_lastReadRez;
}

// The samples of a track. A track whose samples all have one size (AC-3, for instance) has no table of their sizes,
// and its samples are counted from its table of times.
static int64_t sampleCount(const MOVStreamContext* st)
{
    if (!st->m_index.empty())
        return static_cast<int64_t>(st->m_index.size());
    int64_t n = 0;
    for (const auto& e : st->stts_data) n += e.count;
    return n;
}

static int64_t usualDuration(const MOVStreamContext* st);

// Where a track of the open file starts against the first picture of its first video track, as the reader hands
// its samples on: its start on the file's timeline (m_trackStartNs), less what its edit list skips (the priming, and
// in a cut file the sound before the cut), plus the pictures before the cut that the video keeps (m_videoLeadNs).
// For the first file of a join this is what METADemuxer::addStream moves the track by.
int64_t MovDemuxer::soundOffsetNs(const int index) const
{
    const auto st = reinterpret_cast<const MOVStreamContext*>(tracks[index]);
    return trackStartFromVideoNs(index + 1) - ticksToNs(st->editMediaTime, st->time_scale) + m_videoLeadNs;
}

// How long the open file's pictures last as the video reader times them, one usual picture length each; for a file
// without video, how long the content of the audio track audioIndex lasts
int64_t MovDemuxer::partVideoNs(const int audioIndex) const
{
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<const MOVStreamContext*>(tracks[i]);
        if (st->type != IOContextTrackType::VIDEO)
            continue;
        const int64_t usual = usualDuration(st);
        if (usual > 0 && st->time_scale > 0)
            return ticksToNs(sampleCount(st) * usual, st->time_scale);
        break;
    }
    const auto st = reinterpret_cast<const MOVStreamContext*>(tracks[audioIndex]);
    const int64_t content =
        st->editDuration > 0 ? st->editDuration : sampleCount(st) * usualDuration(st) - st->editMediaTime;
    return ticksToNs(content, st->time_scale);
}

// The audio tracks of the part of a join that has just been read: the frames handed on so far, and where the
// pictures of the parts so far end, on the clock of the join's first picture. Each sample of these tracks is one
// frame (AAC, AC-3, E-AC-3, MPEG audio, DTS).
void MovDemuxer::accountJoinPart()
{
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        const auto* audio = dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data);
        const int64_t frameLen = audio ? usualDuration(st) : 0;
        if (frameLen <= 0 || st->time_scale == 0)
            continue;
        JoinAudio& j = m_joinAudio[i + 1];
        if (m_joinPart == 0)
        {
            j.timeScale = st->time_scale;
            j.frameLen = frameLen;
            j.firstOffsetNs = soundOffsetNs(i);
        }
        else if (j.timeScale != st->time_scale || j.frameLen != frameLen)
        {
            j.timeScale = 0;  // a part with another sample rate or frame length: not kept in step
            continue;
        }
        j.frames += sampleCount(st) - j.dropped;
        j.videoEndNs += partVideoNs(i);
    }
}

// ** A LATER FILE OF A JOIN. ** The sound of a join runs on from file to file, frame after frame, and the pictures
// of each file follow those of the file before. Each file's sound begins with the encoder's priming, which its edit
// list starts after, and ends with the rest of its last frame; a cut file's sound begins with the sound before the
// cut as well, and its video keeps the pictures before the cut. The frames of each later file were left out as far
// as its edit list starts, the pre-roll the video keeps with them, so the sound of each later part of a join of cut
// files came 496 ms earlier than the one before; a fragmented file, or one without edit lists, starts its
// tracks with a long first sample or with the first composition offset, which were not counted, and the sound fell
// 37 ms further behind at every joint. As many whole frames are left out at the start of each later file
// as bring its sound nearest to where the file plays it against its own first picture, placed where the pictures
// of the files before end. The sound then stays within half a frame of the picture however many files are joined;
// to the sample it cannot be without decoding the sound. A file whose sound ends before its picture leaves a gap
// that frames cannot fill: the sound of the files after it comes early, with a warning.
void MovDemuxer::dropJoinPriming()
{
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        auto* audio = dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data);
        const int64_t frameLen = audio ? usualDuration(st) : 0;
        if (frameLen <= 0 || st->time_scale == 0)
            continue;
        const auto j = m_joinAudio.find(i + 1);
        const int64_t frames = sampleCount(st);
        int64_t drop = st->editMediaTime / frameLen;  // the priming, whole frames
        if (j != m_joinAudio.end() && j->second.timeScale == st->time_scale && j->second.frameLen == frameLen)
        {
            // where the next frame of the join plays, and where this file's first frame should
            const int64_t soundNs = j->second.firstOffsetNs + ticksToNs(j->second.frames * frameLen, st->time_scale);
            const int64_t dueNs = j->second.videoEndNs + soundOffsetNs(i);
            const int64_t frameNs = ticksToNs(frameLen, st->time_scale);
            const int64_t lateNs = soundNs - dueNs;
            drop = lateNs >= 0 && frameNs > 0 ? (lateNs + frameNs / 2) / frameNs : 0;
            if (lateNs < -frameNs / 2)
                LTRACE(LT_WARN, 2,
                       "Warning: from part " << m_joinPart + 1 << " of the join on, the sound of track " << i + 1
                                             << " comes " << (-lateNs + 500000) / 1000000
                                             << " ms early: a part before it has less sound than picture, and "
                                                "frames cannot fill the gap.");
        }
        drop = (std::min)(drop, frames);
        if (j != m_joinAudio.end())
            j->second.dropped = static_cast<int>(drop);
        if (drop <= 0)
            continue;
        audio->setDropFrames(static_cast<int>(drop));
        LTRACE(LT_INFO, 2,
               "Part " << m_joinPart + 1 << " of the join, track " << i + 1 << ": its first " << drop
                       << " audio frame(s) are left out at the joint, to keep the sound with the picture.");
    }
}

void MovDemuxer::getTrackList(std::map<int32_t, TrackInfo>& trackList)
{
    for (int i = 0; i < num_tracks; i++)
    {
        const auto mst = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        if (mst->trackId &&
            std::find(m_chapterTrackIds.begin(), m_chapterTrackIds.end(), mst->trackId) != m_chapterTrackIds.end())
            continue;  // chapter titles (tref chap), read as chapters
        if (tracks[i]->type != IOContextTrackType::CONTROL)
        {
            trackList.insert(
                std::make_pair(i + 1, TrackInfo(tracks[i]->type == IOContextTrackType::SUBTITLE
                                                    ? TRACKTYPE_SRT
                                                    : reinterpret_cast<MOVStreamContext*>(tracks[i])->containerDataType,
                                                tracks[i]->language, 0)));
            if (const auto delay = m_codecDelayNs.find(i + 1); delay != m_codecDelayNs.end() && delay->second > 0)
                trackList[i + 1].m_srcParams["srccodecdelay"] = std::to_string(delay->second);
            if (tracks[i]->type == IOContextTrackType::AUDIO && m_videoLeadNs > 0)
                trackList[i + 1].m_srcParams["srcvideolead"] = std::to_string(m_videoLeadNs);
            // Where the track starts against the first video track (the listing's Stream delay), for the mux
            // (METADemuxer::addStream). A text track's times are moved by its reader (MovParsedSRTTrackData).
            if (tracks[i]->type == IOContextTrackType::AUDIO || tracks[i]->type == IOContextTrackType::VIDEO)
                if (const int64_t start = trackStartFromVideoNs(i + 1); start != 0)
                    trackList[i + 1].m_srcParams["srcstartdelay"] = std::to_string(start);
            // The samples after the end of the edit list, which only fill the last frame of an Opus or Vorbis track
            // (srcendpadding, as a Matroska source's DiscardPadding gives them). They were not passed on, and a
            // Matroska output played them: 648 samples of Opus, 256 of Vorbis, and a join of Opus files
            // from MP4 played each file's.
            if ((mst->isOpus || mst->containerDataType == TRACKTYPE_VORBIS) && mst->editDuration > 0)
            {
                int64_t media = 0;
                for (const auto& e : mst->stts_data) media += static_cast<int64_t>(e.count) * e.duration;
                for (const int64_t d : mst->fragDur) media += d;
                if (const int64_t pad = media - mst->editMediaTime - mst->editDuration; pad > 0)
                    trackList[i + 1].m_srcParams["srcendpadding"] = std::to_string(ticksToNs(pad, mst->time_scale));
            }
            if (mst->isOpus && mst->codec_priv && mst->codec_priv_size >= 4)
            {
                const auto* p = reinterpret_cast<const uint8_t*>(mst->codec_priv);
                const int64_t preSkip = static_cast<int64_t>(p[2]) << 8 | p[3];  // dOps: big endian, 48 kHz
                trackList[i + 1].m_srcParams["srcopusskip"] = std::to_string(preSkip * 1000000000LL / 48000);
            }
            if (!mst->name.empty())
                trackList[i + 1].m_srcParams["srcname"] = mst->name;
            if (!mst->leftOut.empty())
                trackList[i + 1].m_srcParams["srcleftout"] = mst->leftOut;
            std::string colour;
            for (const auto& [key, value] : mst->colour) colour += (colour.empty() ? "" : ";") + key + "=" + value;
            if (!colour.empty())
                trackList[i + 1].m_srcParams["srccolour"] = colour;
            // A pixel that is not square: the size to show the picture at, as a Matroska source gives it
            if (mst->paspH && mst->paspV && mst->paspH != mst->paspV && mst->width > 0 && mst->height > 0)
                trackList[i + 1].m_srcParams["srcdisplay"] =
                    std::to_string((static_cast<uint64_t>(mst->width) * mst->paspH + mst->paspV / 2) / mst->paspV) +
                    "x" + std::to_string(mst->height);
        }
    }
}

const uint8_t* MovDemuxer::getTrackCodecPrivate(const int32_t pid, int& size)
{
    // MOV/MP4 track PIDs are 1-based (see getTrackList)
    if (pid <= 0 || pid > num_tracks)
    {
        size = 0;
        return nullptr;
    }
    const Track* track = tracks[pid - 1];
    size = track->codec_priv_size;
    return track->codec_priv;
}

int MovDemuxer::ParseTableEntry(MOVAtom atom)
{
    switch (atom.type)
    {
    case MKTAG('a', 'v', 's', 's'):
        return mov_read_extradata(atom);
    case MKTAG('c', 'm', 'o', 'v'):
        return mov_read_cmov(atom);
    case MKTAG('c', 'o', '6', '4'):
        return mov_read_stco(atom);
    case MKTAG('c', 't', 't', 's'):
        return mov_read_ctts(atom);
    case MKTAG('d', 'i', 'n', 'f'):
    case MKTAG('e', 'd', 't', 's'):
    case MKTAG('m', 'd', 'i', 'a'):
    case MKTAG('m', 'i', 'n', 'f'):
    case MKTAG('m', 'v', 'e', 'x'):
    case MKTAG('s', 't', 'b', 'l'):
    case MKTAG('t', 'r', 'a', 'f'):
    case MKTAG('u', 'd', 't', 'a'):
        return mov_read_default(atom);
    case MKTAG('d', 'r', 'e', 'f'):
        return mov_read_dref(atom);
    case MKTAG('e', 'l', 's', 't'):
        return mov_read_elst(atom);
    case MKTAG('e', 's', 'd', 's'):
        return mov_read_esds(atom);
    case MKTAG('a', 'l', 'a', 'c'):
    case MKTAG('f', 'i', 'e', 'l'):
    case MKTAG('j', 'p', '2', 'h'):
        return mov_read_extradata(atom);
    case MKTAG('f', 't', 'y', 'p'):
        return mov_read_ftyp(atom);
    case MKTAG('a', 'v', '1', 'C'):
    case MKTAG('a', 'v', 'c', 'C'):
    case MKTAG('g', 'l', 'b', 'l'):
    case MKTAG('m', 'v', 'c', 'C'):
    case MKTAG('h', 'v', 'c', 'C'):
        return mov_read_glbl(atom);
    case MKTAG('v', 'v', 'c', 'C'):
        return mov_read_vvcc(atom);
    case MKTAG('m', 'e', 't', 'a'):
        return mov_read_meta(atom);
    case MKTAG('i', 'l', 's', 't'):
        return mov_read_ilst(atom);
    case MKTAG('c', 'o', 'v', 'r'):
        return m_inIlst ? mov_read_covr(atom) : 0;
    case MKTAG('c', 'h', 'p', 'l'):
        return mov_read_chpl(atom);
    case MKTAG('t', 'r', 'e', 'f'):
        return mov_read_default(atom);
    case MKTAG('c', 'h', 'a', 'p'):
        return mov_read_tref_chap(atom);
    case MKTAG('t', 'f', 'd', 't'):
        return mov_read_tfdt(atom);
    case MKTAG('c', 'o', 'l', 'r'):
    case MKTAG('m', 'd', 'c', 'v'):
    case MKTAG('c', 'l', 'l', 'i'):
    case MKTAG('C', 'o', 'L', 'L'):
    case MKTAG('p', 'a', 's', 'p'):
    case MKTAG('v', 'p', 'c', 'C'):
        return mov_read_colour(atom);
    case MKTAG('d', 'O', 'p', 's'):  // OpusSpecificBox
    case MKTAG('p', 'c', 'm', 'C'):  // PCMConfigurationBox, ISO/IEC 23003-5
        return mov_read_glbl(atom);
    case MKTAG('e', 'n', 'd', 'a'):  // QuickTime: the byte order of in24 and in32
    {
        if (num_tracks < 1 || atom.size < 2)
            return 0;
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
        st->pcmBigEndian = get_be16() == 0;
        return 0;
    }
    case MKTAG('h', 'd', 'l', 'r'):
        return mov_read_hdlr(atom);
    case MKTAG('m', 'd', 'a', 't'):
        return mov_read_mdat(atom);
    case MKTAG('m', 'd', 'h', 'd'):
        return mov_read_mdhd(atom);
    case MKTAG('m', 'o', 'o', 'f'):
        return mov_read_moof(atom);
    case MKTAG('m', 'o', 'o', 'v'):
        return mov_read_moov(atom);
    case MKTAG('m', 'v', 'h', 'd'):
        return mov_read_mvhd(atom);
    case MKTAG('s', 't', 'c', 'o'):
        return mov_read_stco(atom);
    case MKTAG('s', 't', 's', 'c'):
        return mov_read_stsc(atom);
    case MKTAG('s', 't', 's', 'd'):
        return mov_read_stsd(atom);
    case MKTAG('s', 't', 's', 's'):
        return mov_read_stss(atom);
    case MKTAG('s', 't', 's', 'z'):
        return mov_read_stsz(atom);
    case MKTAG('s', 't', 't', 's'):
        return mov_read_stts(atom);
    case MKTAG('t', 'k', 'h', 'd'):
        return mov_read_tkhd(atom);
    case MKTAG('t', 'f', 'h', 'd'):
        return mov_read_tfhd(atom);
    case MKTAG('t', 'r', 'a', 'k'):
        return mov_read_trak(atom);
    case MKTAG('t', 'r', 'e', 'x'):
        return mov_read_trex(atom);
    case MKTAG('t', 'r', 'k', 'n'):
        return mov_read_trkn(atom);
    case MKTAG('t', 'r', 'u', 'n'):
        return mov_read_trun(atom);
    case MKTAG('w', 'a', 'v', 'e'):
        return mov_read_wave(atom);
    case MKTAG('w', 'i', 'd', 'e'):
        return mov_read_wide(atom);
    default:
        break;
    }

    // Apple QuickTime tags
    if ((atom.type & 0xff) == 0xa9)
        return mov_read_udta_string(atom);

    return 0;
}

int MovDemuxer::mov_read_default(MOVAtom atom)
{
    int64_t total_size = 0;
    MOVAtom a;
    int err = 0;

    a.offset = atom.offset;
    if (atom.size < 0)
        atom.size = LLONG_MAX;

    while (total_size + 8 < atom.size && !m_isEOF && !err)
    {
        a.size = atom.size;
        a.type = 0;
        if (atom.size >= 8)
        {
            a.size = get_be32();
            a.type = get_le32();
        }
        total_size += 8;
        a.offset += 8;
        if (a.size == 1)
        {  // 64 bit extended size
            a.size = get_be64() - 8;
            a.offset += 8;
            total_size += 8;
        }
        // A size of 0 is a box that runs to the end of the file (ISO/IEC 14496-12, 4.2). At the top level the
        // parent's size is "unknown" (LLONG_MAX), so it ran to there instead: the walk read past the end of the file,
        // the reader was left at its end and the demux stopped part way through the media, and that box's end, far
        // past the file, counted as read, so the stopped mux said it was complete (#42). The same for a size that
        // reaches past the end. Both end where the file ends now.
        const int64_t toFileEnd = m_fileSize > 0 ? m_fileSize - m_processedBytes + 8 : LLONG_MAX;
        if (a.size == 0)
        {
            a.size = (std::min)(atom.size - total_size + 8, toFileEnd);
            if (a.size <= 8)
                break;
        }
        else if (a.size > toFileEnd)
        {
            // A job opens a file more than once (its tracks are asked for, then read): the warning comes once a box
            static std::set<std::pair<std::string, int64_t>> warned;
            if (m_pastEndWarnings++ == 0 && warned.emplace(m_fileName, m_processedBytes - 8).second)
                LTRACE(LT_WARN, 2,
                       "Warning: the MP4 box '" << std::string(reinterpret_cast<const char*>(&a.type), 4)
                                                << "' at byte " << m_processedBytes - 8 << " says it is " << a.size
                                                << " bytes long, " << a.size - toFileEnd
                                                << " more than the file holds; it is read up to the end of the file.");
            a.size = toFileEnd;
        }
        a.size -= 8;
        if (a.size < 0)
            break;
        a.size = FFMIN(a.size, atom.size - total_size);

        const int64_t start_pos = m_processedBytes;
        err = ParseTableEntry(a);
        const int64_t left = a.size - m_processedBytes + start_pos;

        // A plain file is read up to its first mdat once its moov is known. A moov with an mvex box announces movie
        // fragments, and those come AFTER the first mdat when the moov describes the first fragment itself (what an
        // encoder writes for fragments without an empty moov): stopping there left every moof unread, and only the
        // first fragment was muxed, 25 of 250 pictures. So with an mvex the walk goes on to the end of the file, as it
        // does for a file whose moov describes no samples.
        if ((!found_moof && m_mdat_pos && found_moov && trex_data.empty()) ||
            (found_moof && m_processedBytes + left >= m_fileSize))
        {
            m_headerWalkEnd = start_pos + a.size;
            return 0;
        }

        // A movie whose index (moov) follows its media data (mdat) had every byte of the media read just to step
        // over it, and the mux then read it all again: minutes for a large file on a network drive (#718, #733,
        // #779). A long step inside the file is a seek now. A short one, and one that would end past the end of
        // the file, are read as before.
        static constexpr int64_t SEEK_RATHER_THAN_READ = 4 * 1024 * 1024;
        // A box whose parser read more than the box holds (a size written too small) left a negative rest, which
        // the skip took as a huge unsigned count: it read to the end of the file, and every box after it, the
        // later tracks included, was lost. The walk goes back to where the box says it ends.
        if (left < 0)
        {
            if (m_overreadWarnings++ == 0)
                LTRACE(LT_WARN, 2,
                       "Warning: the MP4 box '" << std::string(reinterpret_cast<const char*>(&a.type), 4)
                                                << "' at byte " << start_pos - 8 << " holds less than its contents; "
                                                << "reading goes on where the box says it ends.");
            if (m_memoryOnly)  // a compressed header: back to where the box ends, in memory
            {
                m_curPos -= m_processedBytes - (start_pos + a.size);
                m_processedBytes = start_pos + a.size;
            }
            else
                seekOnFreshReader(start_pos + a.size);
        }
        else if (left > SEEK_RATHER_THAN_READ && !m_memoryOnly && m_fileSize > 0 &&
                 m_processedBytes + left < m_fileSize)
            seekOnFreshReader(m_processedBytes + left);
        else
            skip_bytes(left);

        a.offset += a.size;
        total_size += a.size;
    }

    if (!err && total_size < atom.size && atom.size < 0x7ffff)
        skip_bytes(atom.size - total_size);

    return err;
}

// The length of the first n bytes of s without a UTF-8 character at their end that lacks its last bytes: a string cut
// to a count of bytes ends inside a character as often as not, and the half character went out as invalid UTF-8.
static size_t wholeUtf8(const char* s, const size_t n)
{
    size_t i = n;
    int follow = 0;  // continuation bytes at the end
    while (i > 0 && follow < 3 && (static_cast<uint8_t>(s[i - 1]) & 0xC0) == 0x80)
    {
        --i;
        ++follow;
    }
    if (i == 0)
        return n;
    const auto lead = static_cast<uint8_t>(s[i - 1]);
    const int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    return lead >= 0xC0 && need > follow ? i - 1 : n;
}

int MovDemuxer::mov_read_udta_string(MOVAtom atom)
{
    // A string is read whole up to 64 KB: 1023 bytes were kept, which cut a long title, in the middle of a character
    // as often as not.
    std::vector<char> str(65536);
    char key[4]{0}, language[4]{0};
    unsigned str_size;

    if (itunes_metadata)
    {
        const unsigned data_size = get_be32();
        const unsigned tag = get_le32();
        if (tag == MKTAG('d', 'a', 't', 'a'))
        {
            get_be32();  // type
            get_be32();  // unknown
            str_size = data_size - 16;
            atom.size -= 16;
        }
        else
            return 0;
    }
    else
    {
        str_size = get_be16();  // string length
        ff_mov_lang_to_iso639(get_be16(), language);
        atom.size -= 4;
    }
    if (atom.size < 0)
        return -1;

    key[0] = static_cast<char>(atom.type >> 8);
    key[1] = static_cast<char>(atom.type >> 16);
    key[2] = static_cast<char>(atom.type >> 24);

    const bool cut = str_size > str.size() - 1;
    str_size = static_cast<unsigned>(FFMIN(FFMIN(static_cast<int64_t>(str.size() - 1), str_size), atom.size));
    get_buffer(reinterpret_cast<uint8_t*>(str.data()), str_size);
    str[cut ? wholeUtf8(str.data(), str_size) : str_size] = 0;
    // The title of a track (trak/udta) is that track's name when its handler gave none, not the title of the file
    if (m_inTrak)
    {
        if (num_tracks > 0 && strcmp(key, "nam") == 0 && str[0])
            if (const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]); st->name.empty())
                st->name = str.data();
        return 0;
    }
    metaData[key] = str.data();
    return 0;
}

// A compressed movie header (QuickTime): cmov holds dcom, the method, and cmvd, the size of the header and the header
// compressed. Only zlib was ever used. It was refused; the header is now unpacked and read from memory as an ordinary
// moov, which leaves the file where the cmov box ends.
int MovDemuxer::mov_read_cmov(MOVAtom atom)
{
    const int64_t end = m_processedBytes + atom.size;
    uint32_t method = 0;
    std::vector<uint8_t> packed;
    uint32_t unpackedSize = 0;
    while (m_processedBytes + 8 <= end)
    {
        const uint32_t size = get_be32();
        const uint32_t type = get_le32();
        if (size < 8 || m_processedBytes - 8 + size > end)
            break;
        if (type == MKTAG('d', 'c', 'o', 'm') && size >= 12)
        {
            method = get_le32();
            skip_bytes(size - 12);
        }
        else if (type == MKTAG('c', 'm', 'v', 'd') && size >= 12)
        {
            unpackedSize = get_be32();
            packed.resize(size - 12);
            get_buffer(packed.data(), static_cast<unsigned>(packed.size()));
        }
        else
            skip_bytes(size - 8);
    }
    skip_bytes(end - m_processedBytes);
    if (method != MKTAG('z', 'l', 'i', 'b'))
        THROW(ERR_MOV_PARSE,
              "MP4/MOV error: the movie header is compressed with a method other than zlib, which "
              "tsMuxeR cannot unpack")
    if (unpackedSize < 8 || unpackedSize > (256u << 20))
        THROW(ERR_MOV_PARSE, "MP4/MOV error: the compressed movie header gives a size of " << unpackedSize)
    std::vector<uint8_t> moov(unpackedSize);
    uLongf outLen = unpackedSize;
    if (uncompress(moov.data(), &outLen, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
        outLen != unpackedSize)
        THROW(ERR_MOV_PARSE, "MP4/MOV error: the compressed movie header could not be unpacked")
    const uint32_t boxSize = static_cast<uint32_t>(moov[0]) << 24 | moov[1] << 16 | moov[2] << 8 | moov[3];
    if (memcmp(moov.data() + 4, "moov", 4) != 0 || boxSize < 8 || boxSize > unpackedSize)
        THROW(ERR_MOV_PARSE, "MP4/MOV error: the unpacked movie header is not a moov box")

    // Read it from memory, with offsets counted from its start, then go back to the file where the cmov box ended
    uint8_t* const savedCur = m_curPos;
    uint8_t* const savedEnd = m_bufEnd;
    const int64_t savedProcessed = m_processedBytes;
    const bool savedEof = m_isEOF;
    const int64_t savedFileSize = m_fileSize;
    m_curPos = moov.data() + 8;
    m_bufEnd = moov.data() + boxSize;
    m_processedBytes = 8;
    m_fileSize = (std::max)(m_fileSize, static_cast<int64_t>(boxSize));
    m_memoryOnly = true;
    MOVAtom inner;
    inner.type = MKTAG('m', 'o', 'o', 'v');
    inner.offset = 8;
    inner.size = boxSize - 8;
    const int rez = mov_read_default(inner);
    m_memoryOnly = false;
    m_curPos = savedCur;
    m_bufEnd = savedEnd;
    m_processedBytes = savedProcessed;
    m_isEOF = savedEof;
    m_fileSize = savedFileSize;
    return rez;
}

int MovDemuxer::mov_read_wide(MOVAtom atom)
{
    if (atom.size < 8)
        return 0;  // continue
    if (get_be32() != 0)
    {  // 0 sized mdat atom... use the 'wide' atom size
        skip_bytes(atom.size - 4);
        return 0;
    }
    atom.type = get_le32();
    atom.offset += 8;
    atom.size -= 8;
    if (atom.type != MKTAG('m', 'd', 'a', 't'))
    {
        skip_bytes(atom.size);
        return 0;
    }
    const int err = mov_read_mdat(atom);
    return err;
}

// In a fragmented file every track run of every fragment is a chunk, and every mdat holds the runs of its fragment.
// The two lists used to be paired by position, chunk n with mdat n, which holds only while each fragment carries one
// run. With two tracks a fragment carries two runs and one mdat: the first track's chunk was given the whole mdat,
// the other track's samples included, the second was given the size of the next fragment's mdat, and near the end the
// list of mdats ran out before the chunks did and was read past its end. A chunk now belongs to the mdat that holds
// its first byte and ends where the next chunk begins or its mdat ends, whichever comes first. A file with one run per
// fragment gets exactly the sizes it got before. A chunk that no mdat holds is given nothing.
int MovDemuxer::fragmentChunkSize(const int64_t mdatPos, const int64_t offset, const int64_t next,
                                  const bool last) const
{
    const int64_t at = mdatPos + offset;
    // The mdats in file order: the last one that starts at or before the chunk.
    auto mdat = std::upper_bound(m_mdat_data.begin(), m_mdat_data.end(), at,
                                 [](const int64_t pos, const std::pair<int64_t, int64_t>& m) { return pos < m.first; });
    if (mdat == m_mdat_data.begin())
        return 0;
    --mdat;
    int64_t end = mdat->first + mdat->second;
    if (!last && mdatPos + next < end)
        end = mdatPos + next;
    if (end <= at || end - at > INT_MAX)
        return 0;
    return static_cast<int>(end - at);
}

// this atom contains actual media data
int MovDemuxer::mov_read_mdat(MOVAtom atom)
{
    if (atom.size == 0)  // wrong one (MP4)
        return 0;
    if (m_mdat_pos == 0)
    {
        m_mdat_pos = m_processedBytes;
        m_mdat_size = atom.size;
    }
    m_mdat_data.emplace_back(m_processedBytes, atom.size);
    return 0;  // now go for moov
}

int MovDemuxer::mov_read_trun(MOVAtom atom)
{
    MOVFragment* frag = &fragment;
    unsigned data_offset = 0;

    if (frag->track_id <= 0 || frag->track_id > num_tracks)
        return -1;
    Track* st = tracks[frag->track_id - 1];
    const auto sc = reinterpret_cast<MOVStreamContext*>(st);
    if (sc->pseudo_stream_id + 1 != frag->stsd_id)
        return 0;
    const int version = get_byte();  // 1: the composition offsets are signed
    const unsigned flags = get_be24();
    const unsigned entries = get_be32();
    if (flags & 0x001)
        data_offset = get_be32();
    if (flags & 0x004)
        get_be32();  // first_sample_flags
    int64_t offset = frag->base_data_offset + data_offset;
    sc->chunk_offsets.push_back(offset);
    for (size_t i = 0; i < entries; i++)
    {
        unsigned sample_size = frag->size;

        const unsigned duration = flags & 0x100 ? get_be32() : frag->duration;  // sample_duration
        sc->fragDts.push_back(sc->fragNextDts);
        sc->fragDur.push_back(duration);
        sc->fragNextDts += duration;
        if (flags & 0x200)
            sample_size = get_be32();
        if (flags & 0x400)
            get_be32();  // sample_flags
        if (flags & 0x800)
        {
            sc->ctts_data.emplace_back();
            sc->ctts_data[sc->ctts_count].count = 1;
            sc->ctts_data[sc->ctts_count].duration = get_be32();
            if (i == 0 && m_firstTfdt.find(frag->track_id) != m_firstTfdt.end() &&
                m_firstCompositionOffset.find(frag->track_id) == m_firstCompositionOffset.end())
                m_firstCompositionOffset[frag->track_id] =
                    version == 0 ? static_cast<int64_t>(static_cast<uint32_t>(sc->ctts_data[sc->ctts_count].duration))
                                 : static_cast<int64_t>(static_cast<int32_t>(
                                       static_cast<uint32_t>(sc->ctts_data[sc->ctts_count].duration)));
            sc->ctts_count++;
        }

        // assert(sample_duration % sc->time_rate == 0);
        // The size of each sample, as stsz gives it for an ordinary file: the audio and text readers cut a chunk into
        // its samples by it. A fragmented file's stsz is empty, so its AAC had no sizes to go by and was not read.
        sc->m_index.push_back(sample_size);
        offset += sample_size;
    }
    frag->moof_offset = offset;
    return 0;
}

int MovDemuxer::mov_read_trkn(MOVAtom atom)
{
    get_be32();  // type
    get_be32();  // unknown
    metaData["track"] = int32uToStr(get_be32());
    return 0;
}

int MovDemuxer::mov_read_trex(MOVAtom atom)
{
    trex_data.emplace_back();
    MOVTrackExt& trex = trex_data[trex_data.size() - 1];
    get_byte();  // version
    get_be24();  // flags
    trex.track_id = static_cast<int>(get_be32());
    trex.stsd_id = get_be32();
    trex.duration = get_be32();
    trex.size = get_be32();
    trex.flags = get_be32();
    return 0;
}

int MovDemuxer::mov_read_trak(MOVAtom atom)
{
    // Check before allocating. The other order leaked the context every time the limit was hit,
    // and said less than the Matroska path does about what went wrong.
    if (num_tracks >= MAX_STREAMS)
        THROW(ERR_COMMON, "Too many tracks. Max supported tracks count: " << MAX_STREAMS)
    const auto sc = new MOVStreamContext();
    Track* st = tracks[num_tracks] = sc;
    num_tracks++;
    st->type = IOContextTrackType::DATA;
    sc->ffindex = num_tracks;  // st->index;
    m_inTrak = true;
    const int result = mov_read_default(atom);
    m_inTrak = false;
    // Every subtitle time of a text track is a count of its media header's time scale, so a text track whose scale
    // is 0, or that has no media header, cannot be timed. It refused the whole file, even when the track was left
    // out of the mux and when the file was only listed, and the arm64 build, where a division by 0 does not stop
    // the program, read such files. The track is left out now, on every build, and the other tracks are read as
    // usual. Checked once the whole track is read, as the boxes inside it may come in any order.
    if (st->type == IOContextTrackType::SUBTITLE && sc->time_scale == 0 && sc->parsed_priv_data)
    {
        LTRACE(LT_WARN, 2,
               "Warning: text track #" << sc->ffindex
                                       << " has no time scale in its media header (mdhd), so its subtitles cannot "
                                          "be timed. The track is left out.");
        delete sc->parsed_priv_data;
        sc->parsed_priv_data = nullptr;
        st->type = IOContextTrackType::DATA;
        sc->leftOut = "it is a text track with no time scale in its media header, so its subtitles cannot be timed";
    }
    return result;
}

// tfdt: where the track fragment starts on its track's timeline, in the track's time scale. Only the first of each
// track is kept: it is where the track starts.
int MovDemuxer::mov_read_tfdt(MOVAtom atom)
{
    if (atom.size < 8)
        return 0;
    const int version = get_byte();
    get_be24();  // flags
    if (version == 1 && atom.size < 12)
        return 0;
    const int64_t baseMediaDecodeTime =
        version == 1 ? static_cast<int64_t>(get_be64()) : static_cast<int64_t>(get_be32());
    // A track whose samples begin in the moov (a file whose moov describes its first fragment) starts there, as an
    // ordinary file does; its first tfdt is that of its second fragment.
    if (fragment.track_id > 0 && fragment.track_id <= num_tracks &&
        m_firstTfdt.find(fragment.track_id) == m_firstTfdt.end() &&
        reinterpret_cast<MOVStreamContext*>(tracks[fragment.track_id - 1])->chunk_offsets.empty())
        m_firstTfdt[fragment.track_id] = baseMediaDecodeTime;
    if (fragment.track_id > 0 && fragment.track_id <= num_tracks)
        reinterpret_cast<MOVStreamContext*>(tracks[fragment.track_id - 1])->fragNextDts = baseMediaDecodeTime;
    return 0;
}

// The start of each track of a fragmented file, for the delays the listing gives (getTrackDelay). A fragmented file
// says where each track starts in its tfdt boxes rather than in an edit list. They were not read, so a track that
// starts later than the others (audio that begins half a second after the picture, as a recording cut from a live
// stream has it) was muxed as starting with them, and its sound was early by that much. A track starts where its
// first sample is shown: its first tfdt, plus the composition offset of that sample, less the start of a video edit
// list (an audio edit list start is the priming, which is taken off on its own).
void MovDemuxer::fragmentStartDelays()
{
    for (const auto& [track, tfdt] : m_firstTfdt)
    {
        if (track <= 0 || track > num_tracks)
            continue;
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[track - 1]);
        if (st->time_scale == 0)
            continue;
        int64_t start = tfdt;
        if (const auto ct = m_firstCompositionOffset.find(track); ct != m_firstCompositionOffset.end())
            start += ct->second;
        if (const auto mt = m_videoMediaTime.find(track); mt != m_videoMediaTime.end())
            start -= mt->second;
        m_trackStartNs[track] += ticksToNs(start, st->time_scale);
    }
    m_firstTfdt.clear();
    m_firstCompositionOffset.clear();
}

// The durations of a track's first samples, at most n: those of its table of times, then those of its fragments
static std::vector<int64_t> firstDurations(const MOVStreamContext* st, const size_t n)
{
    std::vector<int64_t> d;
    for (const auto& e : st->stts_data)
        for (uint32_t i = 0; i < e.count && d.size() < n; ++i) d.push_back(e.duration);
    for (size_t i = 0; i < st->fragDur.size() && d.size() < n; ++i) d.push_back(st->fragDur[i]);
    return d;
}

// The length most of a track's samples have, from its first ones, the first and the last left out (the first may
// hold a start offset, the last is often only what was left); 0 when there are too few to tell
static int64_t usualDuration(const MOVStreamContext* st)
{
    const std::vector<int64_t> d = firstDurations(st, 64);
    std::map<int64_t, int> counts;
    for (size_t i = 1; i + 1 < d.size(); ++i)
        if (d[i] > 0)
            ++counts[d[i]];
    if (counts.empty())
        return d.size() == 2 && d[1] > 0 ? d[1] : 0;
    return std::max_element(counts.begin(), counts.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; })
        ->first;
}

// ** A START OFFSET WRITTEN AS A LONG FIRST SAMPLE. ** Where a writer does not use an edit list (a fragmented file,
// or ffmpeg with -use_editlist 0), it puts a track that starts later than the others at time 0 all the same, and
// makes its first sample longer by the difference: an AAC track whose sound starts 0.5 s after the picture has a
// first sample of 27840 at 48000 instead of 1024. The readers time a track by its usual sample length, so the start
// was lost: the sound came 500 ms early, the picture 520 ms early, and the priming of AAC that
// such a file writes the same way played in front of the picture (+21.4 ms). What the first sample lasts
// beyond the usual length is where the rest of the track starts, and so where the track starts.
void MovDemuxer::longFirstSamples()
{
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        if (st->hasEditList || st->time_scale == 0 ||
            (st->type != IOContextTrackType::AUDIO && st->type != IOContextTrackType::VIDEO))
            continue;
        const std::vector<int64_t> d = firstDurations(st, 1);
        const int64_t usual = usualDuration(st);
        if (d.empty() || usual <= 0 || d[0] <= usual)
            continue;
        m_trackStartNs[i + 1] += ticksToNs(d[0] - usual, st->time_scale);
    }
}

// A track's start against the first video track's (itself when there is none), in ns
int64_t MovDemuxer::trackStartFromVideoNs(const int32_t pid) const
{
    int64_t videoStart = 0;
    for (int i = 0; i < num_tracks; i++)
        if (tracks[i]->type == IOContextTrackType::VIDEO)
        {
            if (const auto it = m_trackStartNs.find(i + 1); it != m_trackStartNs.end())
                videoStart = it->second;
            break;
        }
    const auto it = m_trackStartNs.find(pid);
    return (it != m_trackStartNs.end() ? it->second : 0) - videoStart;
}

int MovDemuxer::mov_read_tfhd(MOVAtom atom)
{
    MOVFragment* frag = &fragment;
    const MOVTrackExt* trex = nullptr;

    get_byte();  // version
    const int flags = get_be24();

    const int track_id = static_cast<int>(get_be32());
    if (track_id <= 0 || track_id > num_tracks)
        return -1;
    frag->track_id = track_id;
    for (const auto& i : trex_data)
        if (i.track_id == frag->track_id)
        {
            trex = &i;
            break;
        }
    if (!trex)
        THROW(ERR_COMMON, "could not find corresponding trex")

    // Where this track fragment's data offsets count from: the offset the header gives, else the start of the moof
    // when the header says so (default-base-is-moof, which CMAF and DASH fragments use), else the end of the data of
    // the track fragment before it, which mov_read_trun leaves in moof_offset. That last value used to be taken in
    // the second case as well, so the second track of such a fragment was read from the wrong place: its offsets
    // counted from the end of the first track's data instead of from the moof.
    if (flags & 0x01)
        frag->base_data_offset = get_be64();
    else if (flags & 0x020000)
        frag->base_data_offset = frag->moof_start;
    else
        frag->base_data_offset = frag->moof_offset;
    if (flags & 0x02)
        frag->stsd_id = get_be32();
    else
        frag->stsd_id = trex->stsd_id;

    frag->duration = flags & 0x08 ? get_be32() : trex->duration;
    frag->size = flags & 0x10 ? get_be32() : trex->size;
    frag->flags = flags & 0x20 ? get_be32() : trex->flags;
    return 0;
}

int MovDemuxer::mov_read_tkhd(MOVAtom atom)
{
    if (num_tracks < 1 || atom.size < 24)
        return 0;
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    const int version = get_byte();
    get_be24();  // flags
    if (version == 1)
    {
        get_be64();  // creation time
        get_be64();  // modification time
    }
    else
    {
        get_be32();
        get_be32();
    }
    st->trackId = get_be32();
    skip_bytes(atom.size - (m_processedBytes - atom.offset));
    return 0;
}

int MovDemuxer::mov_read_ctts(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_byte();  // version
    get_be24();  // flags
    const unsigned entries = get_be32();
    st->ctts_data.resize(entries);
    st->ctts_data.shrink_to_fit();
    st->ctts_count = 0;
    for (unsigned i = 0; i < entries; i++)
    {
        st->ctts_data[i].count = get_be32();
        st->ctts_data[i].duration = get_be32();
        // st->time_rate= av_gcd(st->time_rate, abs(st->ctts_data[i].duration));
    }
    return 0;
}

// A track's frame rate from its whole table of sample durations: the average over every sample but the last, whose
// duration is often only what was left. A time scale that cannot express the rate makes the durations alternate, as
// 528 and 544 at 16000 for 29.97 or 3753 and 3754 at 90 kHz for 23.976, and only the average gives the rate. An
// average within 0.1 % of a standard rate is that rate: a few samples one tick off, as timestamps rounded to the time
// scale leave them, must not turn 59.94 into 59.9412, which a disc cannot carry. 0 when the table is empty.
// The NEAREST standard rate is taken. The first one within 0.1 % used to win, and 24 lies exactly 0.1 % above 23.976
// (30 above 29.97, and so on), so a true 24 fps track came out as 23.976 or 24 by the rounding of the average alone:
// 0.1 % slower, 3.6 s an hour behind the sound, at the usual time scales 12288 and 90000.
static double fpsFromStts(const std::vector<MOVStts>& stts, const int64_t timeScale)
{
    uint64_t total = 0;
    for (const MOVStts& e : stts) total += e.count;
    uint64_t samples = 0;
    double ticks = 0.0;
    for (size_t i = 0; i < stts.size(); ++i)
    {
        uint64_t count = stts[i].count;
        if (i + 1 == stts.size() && count > 0 && total > 2)
            count--;
        if (stts[i].duration <= 0 || count == 0)
            continue;
        samples += count;
        ticks += static_cast<double>(stts[i].duration) * static_cast<double>(count);
    }
    if (samples == 0 || ticks <= 0.0 || timeScale <= 0)
        return 0.0;
    const double average = static_cast<double>(timeScale) * static_cast<double>(samples) / ticks;
    static constexpr double standardRates[] = {
        24000.0 / 1001, 24.0, 25.0,  30000.0 / 1001,  30.0, 48000.0 / 1001, 48.0, 50.0,
        60000.0 / 1001, 60.0, 100.0, 120000.0 / 1001, 120.0};
    const auto snap = [&](const double rate)
    {
        double nearest = standardRates[0];
        for (const double r : standardRates)
            if (std::fabs(rate - r) < std::fabs(rate - nearest))
                nearest = r;
        return std::fabs(rate - nearest) <= nearest * 0.001 ? nearest : 0.0;
    };
    if (const double standard = snap(average); standard > 0)
        return standard;
    // Not one rate on average, as with one picture held for 2 s in a 25 fps file: the average, 20.83, retimed every
    // picture, and the pictures drifted from the sound by up to 1.9 s. The duration most pictures have gives the rate
    // when it is a standard one; the pictures of other lengths are warned about (MovDemuxer::pictureLengths). Only
    // when the others are LONGER (stills, pauses): with a burst of shorter pictures, 100 at 50 fps in a 25 fps file,
    // the common rate put every picture after the burst 2 s late, to the end of the file; the average, as
    // 2.20.0 took it, keeps the end in place.
    std::map<int64_t, uint64_t> byDuration;
    for (size_t i = 0; i < stts.size(); ++i)
        if (stts[i].duration > 0 && (i + 1 < stts.size() || stts[i].count > 1))  // the last picture alone left out
            byDuration[stts[i].duration] += stts[i].count;
    const auto mostCommon = std::max_element(byDuration.begin(), byDuration.end(),
                                             [](const auto& a, const auto& b) { return a.second < b.second; });
    if (mostCommon != byDuration.end() && mostCommon->second * 2 > total && mostCommon == byDuration.begin())
        if (const double standard = snap(static_cast<double>(timeScale) / static_cast<double>(mostCommon->first));
            standard > 0)
            return standard;
    return average;
}

int MovDemuxer::mov_read_stts(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_byte();  // version
    get_be24();  // flags
    const unsigned entries = get_be32();
    st->stts_data.resize(entries);
    for (unsigned i = 0; i < entries; i++)
    {
        st->stts_data[i].count = get_be32();
        st->stts_data[i].duration = get_be32();
    }
    // The rate the track is muxed at when the meta line gives no fps=. It used to be the FIRST entry alone, which is
    // not the rate wherever the durations alternate: the first entry of a 90 kHz 23.976 track gave 23.9808 or
    // 23.9744, and a 29.97 track at 16000 gave 30.303, and every picture was retimed by it, 80 s over two hours.
    st->fps = fpsFromStts(st->stts_data, st->time_scale);
    return 0;
}

// ** VIDEO IS TIMED AT ONE RATE. ** A picture much longer than one frame time, a still or a pause, is shown for one
// frame time, and the pictures after it come early against the sound; pictures much shorter, a burst at a higher
// rate, make the pictures after them late. The warning was given for longer pictures only, not for a fragmented
// file (whose moov has no table of times, so its runs were not looked at), and once for each reading
// of the moov: twice for one file, five times for a join of two. A fragmented track now takes
// its rate from its runs as a plain one does from its table, and the warning comes once for each file and track.
// The first picture of a track without an edit list is left out: a long one is a start offset (longFirstSamples).
void MovDemuxer::pictureLengths()
{
    static std::mutex warnedMtx;
    static std::set<std::string> warned;
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        if (st->type != IOContextTrackType::VIDEO || st->time_scale == 0)
            continue;
        std::vector<MOVStts> runs = st->stts_data;
        if (runs.empty())
        {
            for (const int64_t d : st->fragDur)
                if (!runs.empty() && runs.back().duration == d)
                    ++runs.back().count;
                else
                    runs.push_back({1, d});
            if (st->fps == 0)
                st->fps = fpsFromStts(runs, st->time_scale);
        }
        if (st->fps <= 0)
            continue;
        uint64_t total = 0;
        for (const auto& e : runs) total += e.count;
        int64_t longest = 0, shortest = INT64_MAX;
        uint64_t first = 0;  // the place of the first picture of the run
        for (const auto& e : runs)
        {
            uint64_t from = first, to = first + e.count;  // the pictures of the run that count
            if (from == 0 && !st->hasEditList)
                from = 1;
            if (to == total)
                to = total - 1;
            if (to > from)
            {
                longest = (std::max)(longest, e.duration);
                shortest = (std::min)(shortest, e.duration);
            }
            first += e.count;
        }
        const double frameTicks = static_cast<double>(st->time_scale) / st->fps;
        if (shortest == INT64_MAX ||
            (static_cast<double>(longest) <= 1.25 * frameTicks && static_cast<double>(shortest) >= 0.8 * frameTicks))
            continue;
        {
            const std::lock_guard<std::mutex> lock(warnedMtx);
            if (!warned.insert(m_fileName + "|" + std::to_string(i + 1)).second)
                continue;
        }
        LTRACE(LT_WARN, 2,
               "Warning: video track " << i + 1 << " of " << m_fileName << " has pictures of different lengths, from "
                                       << shortest * 1000 / st->time_scale << " to " << longest * 1000 / st->time_scale
                                       << " ms. Its pictures are timed at " << doubleToStr(st->fps, 3)
                                       << " fps, so where their lengths differ from that, the video does not keep in "
                                          "step with the sound.");
    }
}

int MovDemuxer::mov_read_stsz(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_byte();  // version
    get_be24();  // flags
    st->sample_size = get_be32();
    const unsigned int entries = get_be32();
    if (st->sample_size)
        return 0;
    if (entries >= UINT_MAX / sizeof(int))
        return -1;
    for (size_t i = 0; i < entries; i++) st->m_index.push_back(get_be32());
    return 0;
}

int MovDemuxer::mov_read_stss(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_byte();  // version
    get_be24();  // flags

    const unsigned int entries = get_be32();
    if (st->sample_size)
        return 0;
    if (entries >= UINT_MAX / sizeof(int))
        return -1;
    for (size_t i = 0; i < entries; i++) st->keyframes.push_back(get_be32());
    return 0;
}

int MovDemuxer::mov_read_extradata(MOVAtom atom)
{
    if (num_tracks < 1)  // will happen with jp2 files
        return 0;
    Track* st = tracks[num_tracks - 1];

    // ISO 14496-12: use extended (16-byte) box header when the total box size
    // (header + payload) exceeds what the compact 4-byte size field can hold.
    const bool useExtendedSize = (static_cast<uint64_t>(atom.size) + 8 > UINT32_MAX);
    const int headerSize = useExtendedSize ? 16 : 8;

    const int64_t newSize = st->codec_priv_size + atom.size + headerSize;
    if (newSize > INT_MAX || atom.size < 0)
        return -1;

    const int64_t oldSize = st->codec_priv_size;
    const auto tmp = new uint8_t[oldSize];
    if (oldSize > 0)
        memcpy(tmp, st->codec_priv, oldSize);
    delete[] st->codec_priv;
    st->codec_priv = new uint8_t[newSize];
    memcpy(st->codec_priv, tmp, oldSize);
    delete[] tmp;
    uint8_t* buf = st->codec_priv + oldSize;

    if (useExtendedSize)
    {
        // Extended size box: [size=1][type][64-bit extended size][data]
        AV_WB32(buf, 1);
        AV_WB32(buf + 4, my_htonl(atom.type));
        AV_WB64(buf + 8, static_cast<uint64_t>(atom.size) + 16);
    }
    else
    {
        // Compact box: [32-bit size][type][data]
        AV_WB32(buf, static_cast<uint32_t>(atom.size + headerSize));
        AV_WB32(buf + 4, my_htonl(atom.type));
    }
    get_buffer(buf + headerSize, static_cast<int>(atom.size));
    st->codec_priv_size = static_cast<int>(newSize);
    if (st->parsed_priv_data)
        st->parsed_priv_data->setPrivData(st->codec_priv, st->codec_priv_size);
    return 0;
}

int MovDemuxer::mov_read_moov(MOVAtom atom)
{
    if (mov_read_default(atom) < 0)
        return -1;
    found_moov = 1;
    // The start delays of the edit lists, now that the movie header has been read wherever it stood (mov_read_elst).
    for (const auto& [track, units] : m_emptyEditUnits)
    {
        if (m_timescale == 0)
            THROW(ERR_MOV_PARSE, "MP4/MOV error: an edit list (elst) delays track #"
                                     << track << ", but the movie has no movie header (mvhd) to time it with")
        m_trackStartNs[track] = ticksToNs(units, m_timescale);
    }
    m_emptyEditUnits.clear();
    // An audio track's edit list start in its own time scale, in nanoseconds, for a Matroska output's CodecDelay. It
    // was not read, so the priming came out as sound at the start of the track.
    for (const auto& [track, mediaTime] : m_editMediaTime)
    {
        if (track > 0 && track <= num_tracks)
            reinterpret_cast<MOVStreamContext*>(tracks[track - 1])->editMediaTime = mediaTime;
        if (track > 0 && track <= num_tracks && tracks[track - 1]->type == IOContextTrackType::VIDEO)
            m_videoMediaTime[track] = mediaTime;
        if (track <= 0 || track > num_tracks || tracks[track - 1]->type != IOContextTrackType::AUDIO)
            continue;
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[track - 1]);
        if (st->time_scale > 0)
            m_codecDelayNs[track] = mediaTime * 1000000000LL / st->time_scale;
    }
    m_editMediaTime.clear();
    // ** THE PRE-ROLL OF A CUT FILE'S VIDEO. ** A file cut without re-encoding starts its video at the key frame before
    // the cut, and its edit lists start every track at the cut: the video's edit list skips the pictures before it, and
    // the sound's edit list skips the sound before it and the encoder's priming. The video reader times the pictures
    // from the first one in the file, so it keeps those pictures, and moving the sound back by its whole edit list
    // start put it that far early against them (500 ms for a cut 0.5 s after a key frame). What the video's edit list
    // skips beyond its own first composition offset (which the video reader's timing already accounts for) is that
    // pre-roll: the sound is moved back by its edit list start less the pre-roll.
    m_videoLeadNs = 0;
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        if (st->type != IOContextTrackType::VIDEO || st->editMediaTime <= 0 || st->time_scale <= 0)
            continue;
        // signed, as below: read unsigned, a first offset of -1024 (a cut file written with negative composition
        // offsets) left no pre-roll, and the sound came 500 ms early
        const int64_t firstOffset =
            st->ctts_data.empty()
                ? 0
                : static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(st->ctts_data[0].duration)));
        const int64_t lead = st->editMediaTime - firstOffset;
        if (lead > 0)
            m_videoLeadNs = lead * 1000000000LL / st->time_scale;
        break;  // the first video track times the file
    }
    for (const auto& [track, duration] : m_editDuration)
        if (track > 0 && track <= num_tracks && m_timescale > 0)
        {
            const auto st = reinterpret_cast<MOVStreamContext*>(tracks[track - 1]);
            st->editDuration = duration * st->time_scale / m_timescale;
        }
    m_editDuration.clear();
    // A track with no edit list is shown from the composition time of its first sample (ISO/IEC 14496-12, 8.6.6): a
    // video track whose pictures are reordered starts that much after a track that has no reordering. That was not
    // counted, so the sound of such a file, 80 to 120 ms in the files measured, came early. A track whose edit list
    // starts it is timed by that list, as before; a fragmented track's samples are timed in fragmentStartDelays.
    for (int i = 0; i < num_tracks; ++i)
    {
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
        if (st->hasEditList || st->ctts_data.empty() || st->time_scale == 0 || st->chunk_offsets.empty())
            continue;
        const auto first = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(st->ctts_data[0].duration)));
        if (first != 0)
            m_trackStartNs[i + 1] = ticksToNs(first, st->time_scale);
    }
    return 0;
}

int MovDemuxer::mov_read_moof(MOVAtom atom)
{
    MOVFragment* frag = &fragment;
    found_moof = true;
    frag->moof_offset = m_processedBytes - 8;
    frag->moof_start = frag->moof_offset;
    return mov_read_default(atom);
}

int MovDemuxer::mov_read_mvhd(MOVAtom atom)
{
    const int version = get_byte();  // version
    get_be24();                      // flags
    if (version == 1)
    {
        get_be64();
        get_be64();
    }
    else
    {
        get_be32();  // creation time
        get_be32();  // modification time
    }
    m_timescale = get_be32();                                           // time scale
    const int64_t duration = (version == 1) ? get_be64() : get_be32();  // duration
    // Every movie time is divided by this, and a zero ended the program on the spot (a hardware
    // divide error that no catch block sees), with no word about the file.
    if (m_timescale == 0)
        THROW(ERR_MOV_PARSE, "MP4/MOV error: the movie header (mvhd) gives a time scale of 0")
    // The longest of the movie's and the tracks' durations, whichever header comes first: the movie header may come
    // after the tracks, and then it used to replace a longer track duration.
    fileDuration = FFMAX(fileDuration, duration * 1000000000ll / m_timescale);
    get_be32();      // preferred scale
    get_be16();      // preferred volume
    skip_bytes(10);  // reserved
    skip_bytes(36);  // display matrix
    get_be32();      // preview time
    get_be32();      // preview duration
    get_be32();      // poster time
    get_be32();      // selection time
    get_be32();      // selection duration
    get_be32();      // current time
    get_be32();      // next track ID
    return 0;
}

int64_t MovDemuxer::getFileDurationNano() const { return fileDuration; }

int MovDemuxer::mov_read_mdhd(MOVAtom atom)
{
    if (num_tracks == 0)
        return -1;
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    const int version = get_byte();
    if (version > 1)
        return -1;  // unsupported

    get_be24();  // flags
    if (version == 1)
    {
        get_be64();
        get_be64();
    }
    else
    {
        get_be32();  // creation time
        get_be32();  // modification time
    }
    st->time_scale = get_be32();  // time_scale

    const int64_t duration = version == 1 ? get_be64() : get_be32();
    if (st->time_scale > 0)  // a zero scale gives an infinite duration, which no integer holds
        fileDuration = FFMAX(fileDuration, (int64_t)((double)duration / st->time_scale * 1000000000));

    const unsigned lang = get_be16();  // language
    ff_mov_lang_to_iso639(lang, st->language);
    get_be16();  // quality

    return 0;
}

int MovDemuxer::mov_read_stsd(MOVAtom atom)
{
    if (num_tracks == 0)
        return -1;
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);

    get_byte();  // version
    get_be24();  // flags

    const unsigned entries = get_be32();

    for (unsigned pseudo_stream_id = 0; pseudo_stream_id < entries; pseudo_stream_id++)
    {
        // Parsing Sample description table
        // enum CodecID id;
        MOVAtom a;
        const int64_t start_pos = m_processedBytes;
        const unsigned size = get_be32();    // size
        const uint32_t format = get_le32();  // data format

        get_be32();  // reserved
        get_be16();  // reserved
        get_be16();  // dref_id

        st->pseudo_stream_id = pseudo_stream_id;
        st->type = IOContextTrackType::DATA;
        switch (format)
        {
        case MKTAG('a', 'v', 'c', '1'):
        case MKTAG('a', 'v', 'c', '3'):
        case MKTAG('d', 'v', 'a', 'v'):
        case MKTAG('d', 'v', 'a', '1'):
            st->type = IOContextTrackType::VIDEO;
            st->parsed_priv_data = new MovParsedH264TrackData(this, st);
            break;
        case MKTAG('h', 'v', 'c', '1'):
        case MKTAG('h', 'e', 'v', '1'):
        case MKTAG('d', 'v', 'h', 'e'):
        case MKTAG('d', 'v', 'h', '1'):
            st->parsed_priv_data = new MovParsedH265TrackData(this, st);
            st->type = IOContextTrackType::VIDEO;
            break;
        // VVC (ISO/IEC 14496-15, 11): its track was not recognised and was listed as Opus with a sample rate of 0, and
        // a mux of it gave no video
        case MKTAG('v', 'v', 'c', '1'):
        case MKTAG('v', 'v', 'i', '1'):
            st->parsed_priv_data = new MovParsedH266TrackData(this, st);
            st->type = IOContextTrackType::VIDEO;
            break;
        case MKTAG('a', 'v', '0', '1'):
            st->parsed_priv_data = new MovParsedAV1TrackData(this, st);
            st->type = IOContextTrackType::VIDEO;
            break;
        case MKTAG('m', 'p', '4', 'a'):
        {
            st->type = IOContextTrackType::AUDIO;
            auto* audioData = new MovParsedAudioTrackData(this, st);
            audioData->isAAC = true;  // mp4a is AAC; ESDS parsing will refine params
            st->parsed_priv_data = audioData;
            break;
        }
        case MKTAG('a', 'c', '-', '3'):
        // E-AC-3 was read as raw bytes without a sample entry of its own, and so its frames could not be counted:
        // the join of MP4 files kept its priming at every joint
        case MKTAG('e', 'c', '-', '3'):
            st->type = IOContextTrackType::AUDIO;
            st->parsed_priv_data = new MovParsedAudioTrackData(this, st);
            break;
        // Codecs whose samples carry nothing a reader could find them by: each sample is handed on in the form the
        // reader of that codec takes from a Matroska file (MovParsedFramedTrackData). They were not recognised.
        case MKTAG('O', 'p', 'u', 's'):
            st->type = IOContextTrackType::AUDIO;
            st->isOpus = true;
            st->parsed_priv_data = new MovParsedFramedTrackData(st, MovParsedFramedTrackData::Form::Opus);
            break;
        case MKTAG('a', 'l', 'a', 'c'):
            st->type = IOContextTrackType::AUDIO;
            st->containerDataType = TRACKTYPE_ALAC;
            st->parsed_priv_data = new MovParsedFramedTrackData(st, MovParsedFramedTrackData::Form::Alac);
            break;
        case MKTAG('v', 'p', '0', '8'):
        case MKTAG('v', 'p', '0', '9'):
            st->type = IOContextTrackType::VIDEO;
            st->containerDataType = format == MKTAG('v', 'p', '0', '8') ? TRACKTYPE_VP8 : TRACKTYPE_VP9;
            st->parsed_priv_data = new MovParsedFramedTrackData(st, MovParsedFramedTrackData::Form::Vpx);
            break;
        // Linear PCM: QuickTime's sowt (16 bit little endian), twos (16 bit big endian), in24 and in32 (big endian
        // unless an enda box says otherwise) and lpcm (its flags say), and ISO/IEC 23003-5's ipcm (its pcmC box says)
        case MKTAG('s', 'o', 'w', 't'):
        case MKTAG('t', 'w', 'o', 's'):
        case MKTAG('i', 'n', '2', '4'):
        case MKTAG('i', 'n', '3', '2'):
        case MKTAG('l', 'p', 'c', 'm'):
        case MKTAG('i', 'p', 'c', 'm'):
            st->type = IOContextTrackType::AUDIO;
            st->pcmBigEndian = format != MKTAG('s', 'o', 'w', 't');
            st->parsed_priv_data = new MovParsedPCMTrackData(st, format);
            break;
        case MKTAG('t', 'x', '3', 'g'):
            st->type = IOContextTrackType::SUBTITLE;
            st->parsed_priv_data = new MovParsedSRTTrackData(this, st);
            break;
        case MKTAG('a', 'p', '4', 'x'):  // ProRes, copied into a Matroska output only (VideoCopyStreamReader)
        case MKTAG('a', 'p', '4', 'h'):
        case MKTAG('a', 'p', 'c', 'h'):
        case MKTAG('a', 'p', 'c', 'n'):
        case MKTAG('a', 'p', 'c', 's'):
        case MKTAG('a', 'p', 'c', 'o'):
            st->type = IOContextTrackType::VIDEO;
            st->containerDataType = TRACKTYPE_PRORES;
            st->parsed_priv_data = new MovParsedVideoCopyData(
                st, static_cast<uint32_t>(format & 0xFF) << 24 | static_cast<uint32_t>(format >> 8 & 0xFF) << 16 |
                        static_cast<uint32_t>(format >> 16 & 0xFF) << 8 | static_cast<uint32_t>(format >> 24));
            break;
        case MKTAG('m', 'p', '4', 'v'):
            // MPEG-4 Part 2, or MPEG-1 or MPEG-2 video found by its start codes as before: its esds says which
            st->type = IOContextTrackType::VIDEO;
            st->isMp4v = true;
            break;
        case MKTAG('w', 'v', 't', 't'):  // WebVTT, ISO/IEC 14496-30
            st->type = IOContextTrackType::SUBTITLE;
            st->parsed_priv_data = new MovParsedSRTTrackData(this, st, true);
            break;
        case MKTAG('t', 'm', 'c', 'd'):
            st->type = IOContextTrackType::CONTROL;
            break;
        default:;
        }

        if (st->type == IOContextTrackType::VIDEO)
        {
            get_be16();  // version
            get_be16();  // revision level
            get_be32();  // vendor
            get_be32();  // temporal quality
            get_be32();  // spatial quality
            st->width = get_be16();
            st->height = get_be16();
            get_be32();                              // horiz resolution
            get_be32();                              // vert resolution
            get_be32();                              // data size, always 0
            get_be16();                              // frames per samples
            skip_bytes(32);                          // codec name, pascal string
            st->bits_per_coded_sample = get_be16();  // depth
            get_be16();                              // colortable id
        }
        else if (st->type == IOContextTrackType::AUDIO)
        {
            const int version = get_be16();
            get_be16();                              // revision level
            get_be32();                              // vendor
            st->channels = get_be16();               // channel count
            st->bits_per_coded_sample = get_be16();  // sample size
            st->audio_cid = get_be16();
            st->packet_size = get_be16();  // packet size = 0
            st->sample_rate = static_cast<int>(get_be32() >> 16);
            // Read QT version 1 fields. In version 0 these do not exist.
            if (!isom)
            {
                if (version == 1)
                {
                    st->samples_per_frame = get_be32();
                    get_be32();  // bytes per packet
                    st->bytes_per_frame = get_be32();
                    get_be32();  // bytes per sample
                }
                else if (version == 2)
                {
                    get_be32();                                                  // sizeof struct only
                    st->sample_rate = static_cast<int>(av_int2dbl(get_be64()));  // float 64
                    st->channels = get_be32();
                    get_be32();                              // always 0x7F000000
                    st->bits_per_coded_sample = get_be32();  // bits per channel if sound is uncompressed
                    st->lpcmFlags = get_be32();              // lpcm format specific flags
                    st->bytes_per_frame = get_be32();        // bytes per audio packet if constant
                    st->samples_per_frame = get_be32();      // lpcm frames per audio packet if constant
                    // if (format == MKTAG('l','p','c','m'))
                    //    st->codec->codec_id = mov_get_lpcm_codec_id(st->codec->bits_per_coded_sample, flags);
                }
            }
            /*
switch (st->codec->codec_id) {
case CODEC_ID_PCM_S8:
case CODEC_ID_PCM_U8:
    if (st->codec->bits_per_coded_sample == 16)
        st->codec->codec_id = CODEC_ID_PCM_S16BE;
    break;
case CODEC_ID_PCM_S16LE:
case CODEC_ID_PCM_S16BE:
    if (st->codec->bits_per_coded_sample == 8)
        st->codec->codec_id = CODEC_ID_PCM_S8;
    else if (st->codec->bits_per_coded_sample == 24)
        st->codec->codec_id =
            st->codec->codec_id == CODEC_ID_PCM_S16BE ?
            CODEC_ID_PCM_S24BE : CODEC_ID_PCM_S24LE;
    break;
// set values for old format before stsd version 1 appeared
case CODEC_ID_MACE3:
    sc->samples_per_frame = 6;
    sc->bytes_per_frame = 2*st->codec->channels;
    break;
case CODEC_ID_MACE6:
    sc->samples_per_frame = 6;
    sc->bytes_per_frame = 1*st->codec->channels;
    break;
case CODEC_ID_ADPCM_IMA_QT:
    sc->samples_per_frame = 64;
    sc->bytes_per_frame = 34*st->codec->channels;
    break;
case CODEC_ID_GSM:
    sc->samples_per_frame = 160;
    sc->bytes_per_frame = 33;
    break;
default:
    break;
}

bits_per_sample = av_get_bits_per_sample(st->codec->codec_id);
if (bits_per_sample) {
    st->codec->bits_per_coded_sample = bits_per_sample;
    sc->sample_size = (bits_per_sample >> 3) * st->codec->channels;
}
            */
        }
        else if (st->type == IOContextTrackType::SUBTITLE)
        {
            const MOVAtom fake_atom(0, 0, size - (m_processedBytes - start_pos));
            mov_read_glbl(fake_atom);
        }
        else
        {
            // other codec type, just skip (rtp, mp4s, tmcd ...)
            skip_bytes(size - (m_processedBytes - start_pos));
        }

        // For mp4a tracks, seed AAC codec defaults from stsd fields so that
        // a usable ADTS header can be built even if no ESDS atom is found.
        // ESDS parsing (via sub-atoms below) will refine these if present.
        if (st->parsed_priv_data)
        {
            auto* audioData = dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data);
            if (audioData && audioData->isAAC)
                audioData->initAACDefaults();
        }

        // this will read extra atoms at the end (wave, alac, damr, avcC, SMI ...)
        a.size = size - (m_processedBytes - start_pos);
        if (a.size > atom.size)
        {
            THROW(ERR_MOV_PARSE, "MP4/MOV error: Invalid a.size in mov_read_stsd")
        }
        if (a.size > 8)
        {
            if (mov_read_default(a) < 0)
                return -1;
        }
        else if (a.size > 0)
            skip_bytes(a.size);
    }
    return 0;
}

int MovDemuxer::mov_read_stco(MOVAtom atom)
{
    const auto sc = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);

    get_byte();  // version
    get_be24();  // flags

    const unsigned entries = get_be32();

    if (entries >= UINT_MAX / sizeof(int64_t))
        return -1;

    // sc->chunk_count = entries;

    if (atom.type == MKTAG('s', 't', 'c', 'o'))
        for (unsigned i = 0; i < entries; i++) sc->chunk_offsets.push_back(get_be32());
    else if (atom.type == MKTAG('c', 'o', '6', '4'))
        for (unsigned i = 0; i < entries; i++) sc->chunk_offsets.push_back(get_be64());
    else
        return -1;

    return 0;
}

int MovDemuxer::mov_read_glbl(MOVAtom atom)
{
    if (static_cast<uint64_t>(atom.size) > (1 << 30))
        return -1;
    Track* st = tracks[num_tracks - 1];
    delete[] st->codec_priv;
    st->codec_priv = new unsigned char[atom.size];
    st->codec_priv_size = static_cast<int>(atom.size);
    get_buffer(st->codec_priv, static_cast<int>(atom.size));
    if (st->parsed_priv_data)
        st->parsed_priv_data->setPrivData(st->codec_priv, st->codec_priv_size);
    return 0;
}

// The VVC configuration box is a full box: a version and flags come before the VvcDecoderConfigurationRecord (ISO/IEC
// 14496-15, 11.2.4.2), which an avcC or hvcC does not have
int MovDemuxer::mov_read_vvcc(MOVAtom atom)
{
    if (atom.size < 4)
        return -1;
    get_be32();  // version and flags
    atom.size -= 4;
    return mov_read_glbl(atom);
}

int MovDemuxer::mov_read_hdlr(MOVAtom atom)
{
    get_byte();  // version
    get_be24();  // flags

    // component type
    const unsigned ctype = get_le32();
    if (!ctype)
        isom = 1;

    const uint32_t subtype = get_le32();  // component subtype, the handler type
    get_be32();                           // component  manufacture
    get_be32();                           // component flags
    get_be32();                           // component flags mask

    // The name of a media track. ISO/IEC 14496-12 has it as a string ending in a zero byte, QuickTime as a counted
    // one; many encoders write a generic one there, which names nothing and is left out.
    const int64_t left = atom.size - (m_processedBytes - atom.offset);
    if (num_tracks > 0 && left > 0 && left < 1024 &&
        (subtype == MKTAG('v', 'i', 'd', 'e') || subtype == MKTAG('s', 'o', 'u', 'n') ||
         subtype == MKTAG('s', 'u', 'b', 't') || subtype == MKTAG('t', 'e', 'x', 't') ||
         subtype == MKTAG('s', 'b', 't', 'l')))
    {
        std::string raw(static_cast<size_t>(left), '\0');
        get_buffer(reinterpret_cast<uint8_t*>(raw.data()), static_cast<int>(left));
        std::string name = static_cast<uint8_t>(raw[0]) == raw.size() - 1 ? raw.substr(1) : raw;
        name = name.substr(0, name.find('\0'));
        while (!name.empty() && static_cast<unsigned char>(name.back()) <= ' ') name.pop_back();
        const bool generic = name.empty() || strEndWith(name, "Handler") || strStartWith(name, "Core Media") ||
                             strStartWith(name, "ISO Media");
        if (!generic)
            reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1])->name = name;
        return 0;
    }
    skip_bytes(left);
    return 0;
}

// An ISO meta box: a full box holding a handler and, for iTunes style metadata, an ilst. QuickTime's own meta box,
// with no version and flags, holds keys this reader does not use and is stepped over.
int MovDemuxer::mov_read_meta(MOVAtom atom)
{
    if (atom.size < 4)
        return 0;
    if (get_be32() != 0)
    {
        skip_bytes(atom.size - 4);
        return 0;
    }
    MOVAtom inner = atom;
    inner.offset += 4;
    inner.size -= 4;
    // Its handler (mdir) has a component type of 0, which mov_read_hdlr takes for an ISO file; a QuickTime file
    // holding an ISO meta box stays a QuickTime file
    const int wasIsom = isom;
    const int rez = mov_read_default(inner);
    isom = wasIsom;
    return rez;
}

int MovDemuxer::mov_read_ilst(MOVAtom atom)
{
    m_inIlst = true;
    itunes_metadata = 1;
    const int rez = mov_read_default(atom);
    itunes_metadata = 0;
    m_inIlst = false;
    return rez;
}

// ilst covr: a data box (size, 'data', type, locale) with the picture
int MovDemuxer::mov_read_covr(MOVAtom atom)
{
    if (atom.size < 16 || atom.size > (64 << 20))
    {
        skip_bytes(atom.size);
        return 0;
    }
    const uint32_t size = get_be32();
    const uint32_t tag = get_le32();
    if (tag != MKTAG('d', 'a', 't', 'a') || size < 16 || size > atom.size)
    {
        skip_bytes(atom.size - 8);
        return 0;
    }
    m_coverType = get_be32();
    get_be32();  // locale
    m_cover.resize(size - 16);
    get_buffer(m_cover.data(), static_cast<int>(m_cover.size()));
    skip_bytes(atom.size - size);
    return 0;
}

// Nero chapters (udta/chpl): version, flags, for version 1 four more bytes, a count, then for each chapter its start in
// units of 100 ns and its title, counted
int MovDemuxer::mov_read_chpl(MOVAtom atom)
{
    const int64_t end = m_processedBytes + atom.size;
    if (atom.size < 5)
    {
        skip_bytes(atom.size);
        return 0;
    }
    const int version = get_byte();
    get_be24();
    if (version == 1)
        get_be32();
    const int count = get_byte();
    for (int i = 0; i < count && m_processedBytes + 9 <= end; ++i)
    {
        const int64_t start = static_cast<int64_t>(get_be64());
        const int len = get_byte();
        if (m_processedBytes + len > end)
            break;
        std::string title(static_cast<size_t>(len), '\0');
        get_buffer(reinterpret_cast<uint8_t*>(title.data()), len);
        // A title holds at most 255 bytes, and a writer cuts a longer one there, inside a character or not
        title.resize(wholeUtf8(title.data(), title.size()));
        m_chplChapters.emplace_back(start * 100, title);
    }
    skip_bytes(end - m_processedBytes);
    return 0;
}

// tref/chap: the tracks that hold this track's chapters as text samples. They are chapter titles, not a subtitle track.
int MovDemuxer::mov_read_tref_chap(MOVAtom atom)
{
    for (int64_t i = 0; i + 4 <= atom.size; i += 4) m_chapterTrackIds.push_back(get_be32());
    skip_bytes(atom.size % 4);
    return 0;
}

// What a video sample entry says about its colours and its pixels, for a Matroska output: the muxer takes it where the
// stream itself says nothing (srccolour, srcdisplay, as from a Matroska source).
//   colr nclx (ISO/IEC 14496-12): primaries, transfer and matrix (16 bits each), then the full range flag
//   colr nclc (QuickTime): the same without the flag; an ICC profile (prof, rICC) is not read
//   mdcv: three display primaries (x, y in units of 0.00002), the white point, the most and the least luminance
//         (units of 0.0001 cd/m2), as in the mastering display SEI
//   clli, and QuickTime's CoLL with a version and flags first: the most and the average light level
//   pasp: the horizontal and vertical spacing of the pixels
//   vpcC (VP codec configuration, version 1): profile, level, bit depth, chroma subsampling and the full range flag,
//         then primaries, transfer and matrix (8 bits each); a VP9 stream itself carries no primaries or transfer
int MovDemuxer::mov_read_colour(MOVAtom atom)
{
    if (num_tracks < 1)
        return 0;
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    if (st->type != IOContextTrackType::VIDEO)
        return 0;
    auto& c = st->colour;
    const auto put = [&c](const char* key, const double value)
    {
        std::ostringstream text;  // six significant digits
        text << value;
        c[key] = text.str();
    };
    switch (atom.type)
    {
    case MKTAG('c', 'o', 'l', 'r'):
    {
        if (atom.size < 10)
            return 0;
        const uint32_t type = get_le32();
        if (type != MKTAG('n', 'c', 'l', 'x') && type != MKTAG('n', 'c', 'l', 'c'))
            return 0;
        const int primaries = get_be16();
        const int transfer = get_be16();
        const int matrix = get_be16();
        // 2 (unspecified) says nothing, and Matroska takes the same numbers (ISO/IEC 23091-2)
        if (primaries != 2)
            put("p", primaries);
        if (transfer != 2)
            put("t", transfer);
        if (matrix != 2)
            put("m", matrix);
        if (type == MKTAG('n', 'c', 'l', 'x') && atom.size >= 11)
            put("r", get_byte() & 0x80 ? 2 : 1);  // Matroska: 1 limited (broadcast), 2 full
        return 0;
    }
    case MKTAG('m', 'd', 'c', 'v'):
    {
        if (atom.size < 24)
            return 0;
        std::pair<double, double> xy[3];
        for (auto& [x, y] : xy)
        {
            x = get_be16() / 50000.0;
            y = get_be16() / 50000.0;
        }
        const double wx = get_be16() / 50000.0;
        const double wy = get_be16() / 50000.0;
        const double lmax = get_be32() / 10000.0;
        const double lmin = get_be32() / 10000.0;
        if (lmax <= 0)
            return 0;
        // The order of the three primaries is the writer's (the SEI suggests green, blue, red); told apart by where
        // they lie: red has the largest x, green the largest y of the other two
        int r = 0;
        for (int i = 1; i < 3; ++i)
            if (xy[i].first > xy[r].first)
                r = i;
        int g = r == 0 ? 1 : 0;
        for (int i = 0; i < 3; ++i)
            if (i != r && xy[i].second > xy[g].second)
                g = i;
        const int b = 3 - r - g;
        put("rx", xy[r].first);
        put("ry", xy[r].second);
        put("gx", xy[g].first);
        put("gy", xy[g].second);
        put("bx", xy[b].first);
        put("by", xy[b].second);
        put("wx", wx);
        put("wy", wy);
        put("lmax", lmax);
        put("lmin", lmin);
        return 0;
    }
    case MKTAG('C', 'o', 'L', 'L'):
    case MKTAG('c', 'l', 'l', 'i'):
    {
        if (atom.type == MKTAG('C', 'o', 'L', 'L'))
        {
            if (atom.size < 8)
                return 0;
            get_be32();  // version, flags
        }
        else if (atom.size < 4)
            return 0;
        const int cll = get_be16();
        const int fall = get_be16();
        if (cll)
            put("cll", cll);
        if (fall)
            put("fall", fall);
        return 0;
    }
    case MKTAG('p', 'a', 's', 'p'):
        if (atom.size < 8)
            return 0;
        st->paspH = get_be32();
        st->paspV = get_be32();
        return 0;
    case MKTAG('v', 'p', 'c', 'C'):
    {
        if (atom.size < 10 || get_byte() != 1)
            return 0;
        get_be24();  // flags
        get_byte();  // profile
        get_byte();  // level
        const int depthChromaRange = get_byte();
        const int primaries = get_byte();
        const int transfer = get_byte();
        const int matrix = get_byte();
        if (c.find("p") == c.end() && primaries != 2)  // a colr box says it too, and says more (the range)
            put("p", primaries);
        if (c.find("t") == c.end() && transfer != 2)
            put("t", transfer);
        if (c.find("m") == c.end() && matrix != 2)
            put("m", matrix);
        if (c.find("r") == c.end())
            put("r", depthChromaRange & 1 ? 2 : 1);
        return 0;
    }
    default:
        return 0;
    }
}

std::string MovDemuxer::getTitle()
{
    const auto it = metaData.find("nam");
    return it != metaData.end() ? it->second : std::string();
}

std::vector<AVChapter> MovDemuxer::getChapters()
{
    // Nero chapters count in one byte, so a writer that writes both lists cuts them at 255 and the chapter track holds
    // them all: of 300 chapters 255 were kept. The longer list is taken.
    //
    // At equal counts the Nero list was taken, also where its titles were cut at 255 bytes and the chapter track holds
    // them whole. The chapter track is taken now where it holds the same chapters: each Nero title is the start of the
    // chapter track's title at the same place. The times stay those of the Nero list for the chapters it holds: a
    // chapter track has no gap before its first sample, and a writer stretches that sample back to 0, so a first
    // chapter at 5 s came out at 0 once the chapter track was the longer list.
    std::vector<AVChapter> out = m_chplChapters;
    std::vector<AVChapter> track = chapterTrackChapters();
    const auto sameChapter = [](const AVChapter& nero, const AVChapter& t)
    { return t.cTitle.compare(0, nero.cTitle.size(), nero.cTitle) == 0; };
    if (!track.empty() &&
        (track.size() > out.size() ||
         (track.size() == out.size() && std::equal(out.begin(), out.end(), track.begin(), sameChapter))))
    {
        for (size_t i = 0; i < out.size(); ++i)
            if (sameChapter(out[i], track[i]))
                track[i].start = out[i].start;
        out = std::move(track);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The chapters of a QuickTime chapter track (tref chap), for a file without Nero chapters: each sample is one chapter,
// starting at the sample's time, its title a 16-bit length and the text (QuickTime text and 3GPP timed text alike).
// Read by the sample tables with a reader of its own, as the demux may be under way.
std::vector<AVChapter> MovDemuxer::chapterTrackChapters() const
{
    const MOVStreamContext* sc = nullptr;
    for (int i = 0; i < num_tracks && !sc; ++i)
    {
        const auto st = reinterpret_cast<const MOVStreamContext*>(tracks[i]);
        if (st->trackId &&
            std::find(m_chapterTrackIds.begin(), m_chapterTrackIds.end(), st->trackId) != m_chapterTrackIds.end())
            sc = st;
    }
    if (!sc || sc->time_scale == 0 || sc->chunk_offsets.empty() || sc->stsc_data.empty())
        return {};
    File f;
    if (!f.open(m_fileName.c_str(), File::ofRead))
        return {};
    std::vector<AVChapter> out;
    size_t sample = 0;
    size_t sttsIndex = 0;
    uint32_t sttsUsed = 0;
    int64_t time = 0;
    size_t samples = sc->m_index.size();  // a size for every sample, or for all of them, and the count from stts
    if (sc->sample_size)
    {
        samples = 0;
        for (const auto& e : sc->stts_data) samples += e.count;
    }
    for (size_t chunk = 0; chunk < sc->chunk_offsets.size() && sample < samples && out.size() < 10000; ++chunk)
    {
        size_t entry = 0;  // the stsc entry this chunk falls under (chunks count from 1)
        while (entry + 1 < sc->stsc_data.size() && sc->stsc_data[entry + 1].first <= chunk + 1) ++entry;
        int64_t pos = sc->chunk_offsets[chunk];
        for (unsigned n = 0; n < sc->stsc_data[entry].count && sample < samples; ++n, ++sample)
        {
            const uint32_t size = sc->sample_size ? sc->sample_size : sc->m_index[sample];
            uint8_t buf[2 + 1024];
            const auto want = static_cast<int>((std::min)(size, static_cast<uint32_t>(sizeof(buf))));
            std::string title;
            if (size >= 2 && f.seek(pos, File::SeekMethod::smBegin) != -1 && f.read(buf, want) == want)
            {
                const int len = (std::min)(buf[0] << 8 | buf[1], want - 2);
                // a title over 1024 bytes is cut to a whole character
                const bool cut = (buf[0] << 8 | buf[1]) > len;
                if (len >= 2 && buf[2] == 0xFE && buf[3] == 0xFF)  // UTF-16, big endian by its byte order mark
                    title = UtfConverter::toUtf8(buf + 4, static_cast<size_t>(len - 2) & ~size_t{1},
                                                 UtfConverter::SourceFormat::sfUTF16be);
                else
                    title.assign(reinterpret_cast<const char*>(buf + 2),
                                 cut ? wholeUtf8(reinterpret_cast<const char*>(buf + 2), static_cast<size_t>(len))
                                     : static_cast<size_t>(len));
            }
            out.emplace_back(time * 1000000000LL / sc->time_scale, title);
            pos += size;
            while (sttsIndex < sc->stts_data.size() && sttsUsed >= sc->stts_data[sttsIndex].count)
            {
                ++sttsIndex;
                sttsUsed = 0;
            }
            if (sttsIndex < sc->stts_data.size())
            {
                time += sc->stts_data[sttsIndex].duration;
                ++sttsUsed;
            }
        }
    }
    return out;
}

// The cover picture of an ilst, as a Matroska AttachedFile element (RFC 9559, 5.1.6), the form a Matroska output takes
// the attachments of its sources in
std::vector<std::vector<uint8_t>> MovDemuxer::getPassThroughAttachments()
{
    if (m_cover.empty())
        return {};
    // By the data type of the covr, or by the picture's own first bytes where the type says nothing
    const bool png = m_coverType == 14 || (m_cover.size() > 4 && memcmp(m_cover.data(), "\x89PNG", 4) == 0);
    const bool bmp = !png && (m_coverType == 27 || (m_cover.size() > 2 && memcmp(m_cover.data(), "BM", 2) == 0));
    const auto element = [](std::vector<uint8_t>& out, const uint32_t id, const uint8_t* data, const size_t size)
    {
        for (int i = id > 0xFFFFFF ? 3 : id > 0xFFFF ? 2 : id > 0xFF ? 1 : 0; i >= 0; --i)
            out.push_back(static_cast<uint8_t>(id >> (8 * i)));
        out.push_back(0x01);  // an eight byte size
        for (int i = 6; i >= 0; --i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (8 * i)));
        out.insert(out.end(), data, data + size);
    };
    std::vector<uint8_t> body;
    const std::string name = png ? "cover.png" : bmp ? "cover.bmp" : "cover.jpg";
    const std::string mime = png ? "image/png" : bmp ? "image/bmp" : "image/jpeg";
    element(body, 0x466E, reinterpret_cast<const uint8_t*>(name.data()), name.size());  // FileName
    element(body, 0x4660, reinterpret_cast<const uint8_t*>(mime.data()), mime.size());  // FileMediaType
    element(body, 0x465C, m_cover.data(), m_cover.size());                              // FileData
    uint64_t uid = 1469598103934665603ULL;  // FNV-1a of the picture: the same file gives the same UID
    for (const uint8_t c : m_cover) uid = (uid ^ c) * 1099511628211ULL;
    uint8_t uidBytes[8];
    for (int i = 0; i < 8; ++i) uidBytes[i] = static_cast<uint8_t>(uid >> (8 * (7 - i)));
    element(body, 0x46AE, uidBytes, 8);  // FileUID
    std::vector<uint8_t> attached;
    element(attached, 0x61A7, body.data(), body.size());  // AttachedFile
    return {attached};
}

int MovDemuxer::mov_read_ftyp(MOVAtom atom)
{
    const uint32_t type = get_le32();
    if (type != MKTAG('q', 't', ' ', ' '))
        isom = 1;
    get_be32();  // minor version
    skip_bytes(atom.size - 8);
    return 0;
}

int MovDemuxer::mp4_read_descr(int* tag)
{
    *tag = get_byte();
    int len = 0;
    int count = 4;
    while (count--)
    {
        const int c = get_byte();
        len = (len << 7) | (c & 0x7f);
        if (!(c & 0x80))
            break;
    }
    return len;
}

int MovDemuxer::mov_read_esds(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_be32();  // version + flags
    int tag;
    mp4_read_descr(&tag);  // len
    get_be16();            // ID
    if (tag == MP4ESDescrTag)
        get_byte();        // priority
    mp4_read_descr(&tag);  // len
    if (tag == MP4DecConfigDescrTag)
    {
        const uint8_t object_type_id = get_byte();
        // The object type says what the track holds (ISO/IEC 14496-1, 7.2.6.6.2, and the MP4 registration
        // authority's list). Only AAC carries the decoder specific information below, and the track was left marked as
        // AAC whenever that was missing: MP3, MP2 and DTS in an mp4a sample entry got an ADTS header on every frame,
        // were listed as AAC and muxed to nothing. Anything that is not AAC goes on as it is stored, framed by its own
        // headers, and the codec readers find it by those.
        if (auto* audioData = dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data))
            audioData->isAAC =
                object_type_id == 0x40 || object_type_id == 0x66 || object_type_id == 0x67 || object_type_id == 0x68;
        get_byte();  // stream type
        get_be24();  // buffer size db
        get_be32();  // max bitrate
        get_be32();  // avg bitrate
        const int len = mp4_read_descr(&tag);
        if (tag == MP4DecSpecificDescrTag)
        {
            if (static_cast<uint64_t>(len) > (1 << 30) || static_cast<uint64_t>(len) < 2)
                return -1;
            st->codec_priv = new unsigned char[len];
            st->codec_priv_size = len;
            get_buffer(st->codec_priv, len);
            // MPEG-4 Part 2 (Visual, object type 0x20): no reader of tsMuxeR understands it, and it is copied into a
            // Matroska output as it is, its VOS and VOL headers, the decoder specific information, in front
            if (object_type_id == 0x20 && st->isMp4v && !st->parsed_priv_data)
            {
                st->containerDataType = TRACKTYPE_MP4V;
                st->parsed_priv_data = new MovParsedVideoCopyData(st, 0x6D703476);  // 'mp4v'
                st->parsed_priv_data->setPrivData(st->codec_priv, st->codec_priv_size);
                return 0;
            }
            // Vorbis, object type 0xDD: its three headers, Xiph laced as in a Matroska CodecPrivate, are the decoder
            // specific information
            if (object_type_id == 0xDD && dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data))
            {
                delete st->parsed_priv_data;
                st->containerDataType = TRACKTYPE_VORBIS;
                st->parsed_priv_data = new MovParsedFramedTrackData(st, MovParsedFramedTrackData::Form::Vorbis);
                st->parsed_priv_data->setPrivData(st->codec_priv, st->codec_priv_size);
                return 0;
            }
            if (st->parsed_priv_data)
            {
                // object_type_id 0x40 = Audio ISO/IEC 14496-3 (AAC)
                // 0x66/0x67/0x68 = MPEG-2 AAC profiles
                const bool isAAC = (object_type_id == 0x40 || object_type_id == 0x66 || object_type_id == 0x67 ||
                                    object_type_id == 0x68);
                auto* audioData = dynamic_cast<MovParsedAudioTrackData*>(st->parsed_priv_data);
                if (audioData)
                {
                    audioData->isAAC = isAAC;
                    // setPrivData uses readConfig() to properly parse the
                    // AudioSpecificConfig (profile, sample rate, channels).
                    st->parsed_priv_data->setPrivData(st->codec_priv, st->codec_priv_size);
                }
            }
        }
    }
    return 0;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
int MovDemuxer::mov_read_dref(MOVAtom atom)
{
    /*
    MOVStreamContext* st = reinterpret_cast<MOVStreamContext*> tracks[num_tracks-1];
get_be32(); // version + flags
int entries = get_be32();
    st->drefs.resize(entries);

for (int i = 0; i < entries; i++) {
    uint32_t size = get_be32();
    int64_t next = m_processedBytes + size - 4;
    MOVDref *dref = &(st->drefs[i]);
    dref->type = get_le32();
            get_be32(); // version + flags
    if (dref->type == MKTAG('a','l','i','s') && size > 150) {
        int volume_len = get_byte();
            }
    }
    */
    return 0;
}
int MovDemuxer::mov_read_stsc(MOVAtom atom)
{
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1]);
    get_byte();  // version
    get_be24();  // flags

    const unsigned entries = get_be32();
    st->stsc_data.resize(entries);

    for (unsigned i = 0; i < entries; i++)
    {
        st->stsc_data[i].first = get_be32();
        st->stsc_data[i].count = get_be32();
        st->stsc_data[i].id = get_be32();
    }
    return 0;
}

int MovDemuxer::mov_read_wave(MOVAtom atom)
{
    if (static_cast<uint64_t>(atom.size) > (1 << 30))
        return -1;
    /*
if (st->codec->codec_id == CODEC_ID_QDM2) {
    // pass all frma atom to codec, needed at least for QDM2
    av_free(st->codec->extradata);
    st->codec->extradata = av_mallocz(atom.size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!st->codec->extradata)
        return AVERROR(ENOMEM);
    st->codec->extradata_size = atom.size;
    get_buffer(pb, st->codec->extradata, atom.size);
} else
    */
    if (atom.size > 8)
    {  // to read frma, esds atoms
        if (mov_read_default(atom) < 0)
            return -1;
    }
    else
        skip_bytes(atom.size);
    return 0;
}

// The edit list of a track. Each entry is a duration, a media time and a media rate: 20 bytes in version 1, 12 in
// version 0. The rate was read once after the list instead of with each entry, so every entry after the first was read
// 4 bytes out of step, and the count was taken as written, so a count larger than the box made the reading run on
// through what follows it. The entries are now read whole, and no more of them than the box holds.
//
// An empty edit (media time -1) at the start delays the track. The delay is the sum of the empty edits before the
// first edit that shows media; it used to be the last empty edit anywhere in the list. It is in movie time, so it is
// kept in movie units here and converted once the whole movie box is read, with this file's own movie header: the
// header may come after the tracks, which the format allows, and in a join the next file's header was not read yet.
int MovDemuxer::mov_read_elst(MOVAtom atom)
{
    if (num_tracks > 0)
        reinterpret_cast<MOVStreamContext*>(tracks[num_tracks - 1])->hasEditList = true;
    const int version = get_byte();
    get_be24();                              // flags
    const unsigned edit_count = get_be32();  // entries

    const int64_t entrySize = version == 1 ? 20 : 12;
    const int64_t fits = atom.size > 8 ? (atom.size - 8) / entrySize : 0;
    unsigned count = edit_count;
    if (count > fits)
    {
        LTRACE(LT_WARN, 2,
               "Warning: the edit list (elst) of track #" << num_tracks << " gives " << edit_count
                                                          << " entries, but its box holds only " << fits
                                                          << ". Only those are read.");
        count = static_cast<unsigned>(fits);
    }
    int64_t leadingEmpty = 0;
    bool leading = true;
    for (unsigned i = 0; i < count; i++)
    {
        int64_t duration = 0;
        int64_t mediaTime = 0;
        bool empty = false;
        if (version == 1)
        {
            duration = get_be64();
            mediaTime = static_cast<int64_t>(get_be64());
            empty = mediaTime == -1;
        }
        else
        {
            duration = get_be32();
            const uint32_t raw = get_be32();
            empty = raw == UINT_MAX;
            mediaTime = empty ? -1 : static_cast<int64_t>(raw);
        }
        get_be32();  // media rate
        if (leading && empty)
            leadingEmpty += duration;
        else
        {
            // The first edit that plays something: where in the media it starts. For AAC that is the priming the
            // encoder put in front, 1024 or 2112 samples, which a player skips.
            if (leading && mediaTime > 0)
                m_editMediaTime[num_tracks] = mediaTime;
            if (leading)
                m_editDuration[num_tracks] = duration;
            leading = false;
        }
    }
    if (leadingEmpty > 0)
        m_emptyEditUnits[num_tracks] = leadingEmpty;
    return 0;
}

double MovDemuxer::getTrackFps(const uint32_t trackId)
{
    // Bounds checked, as MatroskaDemuxer::getTrackFps is, and as getTrackCodecPrivate a few
    // hundred lines above already is. A subTrack stream is identified by a SUB-PID,
    // (pid << 16) + subTrack, so an unchecked tracks[trackId - 1] reads tens of thousands of
    // entries past the end and takes the process with it.
    //
    // That is reachable without any of this being unusual: an mp4 whose video track holds a
    // merged Dolby Vision picture, read back with subTrack= and no fps= on the line, crashed
    // 2.18.4 and 2.18.5 with an access violation.
    if (trackId == 0 || static_cast<int>(trackId) > num_tracks)
        return 0.0;
    const auto st = reinterpret_cast<MOVStreamContext*>(tracks[trackId - 1]);
    return st->fps;
}

void MovDemuxer::setFileIterator(FileNameIterator* itr) { m_fileIterator = itr; }
