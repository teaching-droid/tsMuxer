// ReSharper disable CppPassValueParameterByConstReference

#include "movDemuxer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

#include <fs/systemlog.h>

#include "aac.h"
#include "abstractStreamReader.h"
#include "av1.h"
#include "avPacket.h"
#include "bitStream.h"
#include "hevc.h"
#include "subTrackFilter.h"
#include "vodCoreException.h"
#include "vvc.h"

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
        for (int i = 2; i >= 0; i--)
        {
            to[i] = static_cast<char>(0x60 + (code & 0x1f));
            code >>= 5;
        }
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

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        uint8_t* dst = pkt->data;
        const uint8_t* srcEnd = buff + size;
        while (buff < srcEnd - 4)
        {
            unsigned frameSize = m_sc->sample_size;
            if (frameSize == 0)
                frameSize = m_sc->m_index[m_sc->m_indexCur++];
            if (buff + frameSize > srcEnd)
                break;
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
        for (; left > 4; ++i)
        {
            left -= m_sc->sample_size;
            if (m_sc->sample_size == 0)
            {
                if (m_sc->m_indexCur + i >= m_sc->m_index.size())
                    THROW(ERR_MOV_PARSE, "Out of index for AAC track #" << m_sc->ffindex << " at position "
                                                                        << m_demuxer->getProcessedBytes())
                left -= m_sc->m_index[m_sc->m_indexCur + i];
            }
        }
        if (left > 4)
            THROW(ERR_MOV_PARSE,
                  "Invalid AAC frame for track #" << m_sc->ffindex << " at position " << m_demuxer->getProcessedBytes())
        if (!isAAC)
            i = 0;
        return (size - left) + i * AAC_HEADER_LEN;
    }

    bool isAAC;

   private:
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
    static constexpr uint32_t MAX_NAL_SIZE = 10 * 1024 * 1024;

    MovParsedH264TrackData(MovDemuxer* demuxer, MOVStreamContext* sc) : m_sc(sc), m_demuxer(demuxer), nal_length_size(4)
    {
    }
    void setPrivData(uint8_t* buff, int size) override
    {
        spsPpsList.clear();
        if (size < 6)
            THROW(ERR_MOV_PARSE, "Invalid H.264/AVC extra data format")
        nal_length_size = (buff[4] & 0x03) + 1;
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

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        uint8_t* dst = pkt->data;
        if (!spsPpsList.empty())
        {
            for (auto& i : spsPpsList)
            {
                *dst++ = 0x0;
                *dst++ = 0x0;
                *dst++ = 0x0;
                *dst++ = 0x1;

                memcpy(dst, i.data(), i.size());
                dst += i.size();
            }
            spsPpsList.clear();
        }
        const uint8_t* end = buff + size;
        while (buff < end)
        {
            // pkt->data was sized by newBufferSize, which stops as soon as a length stops making
            // sense. This loop used to run on regardless and copy whatever it read, so one bad
            // length wrote past the end of the buffer that had been reserved for the frame. The
            // two walks have to give up in the same places.
            if (static_cast<size_t>(end - buff) < nal_length_size)
                break;
            const auto nalSize = static_cast<uint32_t>(getNalSize(buff));
            buff += nal_length_size;
            if (nalSize == 0 || nalSize > MAX_NAL_SIZE || nalSize > static_cast<size_t>(end - buff))
                break;
            *dst++ = 0x00;
            *dst++ = 0x00;
            *dst++ = 0x00;
            *dst++ = 0x01;
            memcpy(dst, buff, nalSize);
            dst += nalSize;
            buff += nalSize;
        }
    }

    unsigned newBufferSize(uint8_t* buff, const unsigned size) override
    {
        if (!buff || size == 0)
            return 0;

        if (nal_length_size < 1 || nal_length_size > 4)
        {
            LTRACE(LT_WARN, 2,
                   "MP4/MOV: invalid nal_length_size " << static_cast<int>(nal_length_size)
                                                       << ", using conservative estimate");
            return size * 2;
        }

        // The size of exactly what extractData writes: each NAL that passes, with a 4 byte start code in place of its
        // length field. It used to be the size of the whole sample plus the start code overhead, so where the walk
        // stopped early, as it does in a sample cut off by the end of the file, the packet kept the bytes behind the
        // last NAL that was written, and whatever the reused buffer held there went into the stream. Where the walk
        // reaches the end of the sample the two sizes are the same.
        const uint8_t* cur = buff;
        const uint8_t* end = buff + size;
        uint64_t nalBytes = 0;
        while (cur < end)
        {
            if (static_cast<size_t>(end - cur) < nal_length_size)
            {
                LTRACE(LT_WARN, 2,
                       "MP4/MOV: truncated NAL length field at position " << m_demuxer->getProcessedBytes()
                                                                          << ", stopping parse");
                break;
            }
            const auto nalSize = static_cast<uint32_t>(getNalSize(cur));
            cur += nal_length_size;
            if (nalSize == 0 || nalSize > MAX_NAL_SIZE)
            {
                LTRACE(LT_WARN, 2,
                       "MP4/MOV: suspicious NAL size " << nalSize << " at position " << m_demuxer->getProcessedBytes());
                break;
            }
            if (nalSize > static_cast<size_t>(end - cur))
            {
                LTRACE(LT_WARN, 2,
                       "MP4/MOV: NAL size " << nalSize << " exceeds frame boundary at position "
                                            << m_demuxer->getProcessedBytes());
                break;
            }
            cur += nalSize;
            nalBytes += 4 + static_cast<uint64_t>(nalSize);
        }
        unsigned spsPpsSize = 0;
        for (const auto& i : spsPpsList)
        {
            const auto entrySize = static_cast<unsigned>(i.size() + 4);
            if (spsPpsSize > UINT_MAX - entrySize)
                return size * 2;  // overflow protection
            spsPpsSize += entrySize;
        }

        if (nalBytes > UINT_MAX - spsPpsSize)
            return size * 2;  // overflow protection

        return static_cast<unsigned>(nalBytes) + spsPpsSize;
    }

   protected:
    MOVStreamContext* m_sc;
    MovDemuxer* m_demuxer;

    vector<vector<uint8_t>> spsPpsList;
    uint8_t nal_length_size;
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

class MovParsedAV1TrackData final : public ParsedTrackPrivData
{
   public:
    MovParsedAV1TrackData(MovDemuxer* /*demuxer*/, MOVStreamContext* /*sc*/) : m_firstExtract(true) {}

    void setPrivData(uint8_t* buff, const int size) override
    {
        // Parse AV1CodecConfigurationRecord from av1C box
        m_configOBUs = av1_extract_priv_data(buff, size);
    }

    void extractData(AVPacket* pkt, uint8_t* buff, const int size) override
    {
        // AV1 in MP4 uses low-overhead OBU format, same as MKV.
        // Convert to start-code-prefixed format for TS output.

        // Estimate output size
        size_t configSize = 0;
        if (m_firstExtract)
        {
            for (const auto& obu : m_configOBUs) configSize += obu.size();
        }
        const size_t totalSize = configSize + size * 2 + 16;

        pkt->data = new uint8_t[totalSize];
        uint8_t* dst = pkt->data;

        // Insert config OBUs before first frame
        if (m_firstExtract)
        {
            for (const auto& obu : m_configOBUs)
            {
                memcpy(dst, obu.data(), obu.size());
                dst += obu.size();
            }
            m_firstExtract = false;
        }

        // Convert each OBU
        const uint8_t* src = buff;
        const uint8_t* end = buff + size;

        while (src < end)
        {
            Av1ObuHeader obuHdr;
            const int hdrLen = obuHdr.parse(src, end);
            if (hdrLen < 0)
                break;

            if (!obuHdr.obu_has_size_field)
                break;

            int leb128Bytes = 0;
            const int64_t obuPayloadSize = decodeLeb128(src + hdrLen, end, leb128Bytes);
            if (obuPayloadSize < 0)
                break;

            const uint8_t* payload = src + hdrLen + leb128Bytes;
            if (payload + obuPayloadSize > end)
                break;

            // Start code
            *dst++ = 0x00;
            *dst++ = 0x00;
            *dst++ = 0x01;

            // OBU header with obu_has_size_field cleared
            *dst++ = src[0] & ~0x02;
            if (obuHdr.obu_extension_flag)
                *dst++ = src[1];

            // Payload with emulation prevention
            if (obuPayloadSize > 0)
            {
                const int encoded =
                    av1_add_emulation_prevention(payload, payload + obuPayloadSize, dst, totalSize - (dst - pkt->data));
                if (encoded > 0)
                    dst += encoded;
                else
                {
                    memcpy(dst, payload, obuPayloadSize);
                    dst += obuPayloadSize;
                }
            }

            src = payload + obuPayloadSize;
        }

        pkt->size = static_cast<int>(dst - pkt->data);
    }

   private:
    std::vector<std::vector<uint8_t>> m_configOBUs;
    bool m_firstExtract;
};

class MovParsedSRTTrackData final : public ParsedTrackPrivData
{
   public:
    MovParsedSRTTrackData(MovDemuxer* demuxer, MOVStreamContext* sc)
        : m_buff(), m_size(0), m_demuxer(demuxer), m_sc(sc), sttsCnt(0)
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
        return m_sc->stts_data[sttsPos].duration * 1000 / m_sc->time_scale;
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
        while (used < size)
        {
            uint32_t sampleSize = size - used;  // without a size table, what is left, as before
            if (m_sc->sample_size)
                sampleSize = m_sc->sample_size;
            else if (indexCur < m_sc->m_index.size())
                sampleSize = m_sc->m_index[indexCur++];
            sampleSize = (std::min)(sampleSize, size - used);
            const int64_t startTime = timeOffset;
            const int64_t endTime = startTime + getSttsVal();
            std::string cue = cueFor(buff + used, buff + used + sampleSize, startTime, endTime, counter + 1);
            if (!cue.empty())
            {
                if (counter == 0)
                    out += "\xEF\xBB\xBF";  // UTF-8 header
                out += cue;
                ++counter;
            }
            timeOffset = endTime;
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
            sort(tags.begin(), tags.end(), greater<>());
            for (const auto& [pos, tag] : tags) text.insert((std::min)(static_cast<size_t>(pos), text.size()), tag);
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
    int64_t m_timeOffset;
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
    readHeaders();
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
    // What follows the last box the walk read holds no media when it is only boxes such as free, skip or uuid. The
    // demux never reaches them, and left out of the count they made a complete mux read as stopped early once they
    // were over 1 % of the file (#43). A file with more media after the walk's end (a second mdat, a moof) keeps
    // saying so.
    if (!found_moof && m_headerWalkEnd > 0 && m_headerWalkEnd < m_fileSize)
        m_firstHeaderSize += trailingBytesWithoutMedia(m_headerWalkEnd);
}

// The bytes from `from` to the end of the file when the top-level boxes there hold no media, read by their headers
// alone; 0 when one of them is an mdat or a moof, or the headers do not reach the end of the file exactly.
int64_t MovDemuxer::trailingBytesWithoutMedia(const int64_t from) const
{
    File f;
    if (!f.open(m_fileName.c_str(), File::ofRead))
        return 0;
    int64_t pos = from;
    while (pos < m_fileSize)
    {
        uint8_t h[16];
        if (m_fileSize - pos < 8 || f.seek(pos, File::SeekMethod::smBegin) == -1 || f.read(h, 8) != 8)
            return 0;
        uint64_t size = static_cast<uint64_t>(h[0]) << 24 | static_cast<uint64_t>(h[1]) << 16 |
                        static_cast<uint64_t>(h[2]) << 8 | h[3];
        const uint32_t type = MKTAG(h[4], h[5], h[6], h[7]);
        if (size == 1)
        {
            if (f.read(h + 8, 8) != 8)
                return 0;
            size = 0;
            for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
        }
        else if (size == 0)
            size = static_cast<uint64_t>(m_fileSize - pos);
        if (type == MKTAG('m', 'd', 'a', 't') || type == MKTAG('m', 'o', 'o', 'f') || size < 8 ||
            size > static_cast<uint64_t>(m_fileSize - pos))
            return 0;
        pos += static_cast<int64_t>(size);
    }
    return pos == m_fileSize ? m_fileSize - from : 0;
}

// The byte size of each chunk of a track by its own tables: stsc gives the samples of each chunk, stsz the size of
// each sample. -1 where they do not say: a track with one size for every sample (raw audio counts its samples in
// units that are not bytes), or tables that run out.
static std::vector<int64_t> ownChunkBytes(const MOVStreamContext* st)
{
    std::vector<int64_t> bytes(st->chunk_offsets.size(), -1);
    if (st->sample_size != 0 || st->stsc_data.empty())
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
        for (int i = 0; i < num_tracks; ++i)
        {
            const auto st = reinterpret_cast<MOVStreamContext*>(tracks[i]);
            const std::vector<int64_t> bytes = found_moof ? std::vector<int64_t>() : ownChunkBytes(st);
            for (size_t k = 0; k < st->chunk_offsets.size(); ++k)
            {
                const int64_t j = st->chunk_offsets[k];
                if (!found_moof)
                    if (j < m_mdat_pos || j > m_mdat_pos + m_mdat_size)
                        THROW(ERR_MOV_PARSE, "Invalid chunk offset " << j)
                chunks.emplace_back(j - m_mdat_pos, i);
                if (k < bytes.size())
                    ownBytes[chunks.back()] = bytes[k];
            }
        }
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
        if (!found_moof && m_curChunk < m_chunkBytes.size() && m_chunkBytes[m_curChunk] >= 0 &&
            m_chunkBytes[m_curChunk] < chunkSize)
            chunkSize = static_cast<int>(m_chunkBytes[m_curChunk]);
        const int trackId = static_cast<int>(chunks[m_curChunk].second);
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
            openFile(nextName);
            return 0;
        }
    }

    m_lastReadRez = BufferedReader::DATA_EOF;
    return m_lastReadRez;
}

void MovDemuxer::getTrackList(std::map<int32_t, TrackInfo>& trackList)
{
    for (int i = 0; i < num_tracks; i++)
    {
        if (tracks[i]->type != IOContextTrackType::CONTROL)
        {
            trackList.insert(
                std::make_pair(i + 1, TrackInfo(tracks[i]->type == IOContextTrackType::SUBTITLE ? TRACKTYPE_SRT : 0,
                                                tracks[i]->language, 0)));
            if (const auto delay = m_codecDelayNs.find(i + 1); delay != m_codecDelayNs.end() && delay->second > 0)
                trackList[i + 1].m_srcParams["srccodecdelay"] = std::to_string(delay->second);
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
            if (m_pastEndWarnings++ == 0)
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
            seekOnFreshReader(start_pos + a.size);
        }
        else if (left > SEEK_RATHER_THAN_READ && m_fileSize > 0 && m_processedBytes + left < m_fileSize)
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

int MovDemuxer::mov_read_udta_string(MOVAtom atom)
{
    char str[1024]{}, key[4]{0}, language[4]{0};
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

    str_size = static_cast<uint16_t>(FFMIN(FFMIN((int)(sizeof(str) - 1), str_size), atom.size));
    get_buffer(reinterpret_cast<uint8_t*>(str), str_size);
    str[str_size] = 0;
    metaData[key] = str;
    return 0;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
int MovDemuxer::mov_read_cmov(MOVAtom atom) { THROW(ERR_MOV_PARSE, "Compressed MOV not supported in current version") }

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
    get_byte();  // version
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

        if (flags & 0x100)
            get_be32();  // sample_duration
        if (flags & 0x200)
            sample_size = get_be32();
        if (flags & 0x400)
            get_be32();  // sample_flags
        if (flags & 0x800)
        {
            sc->ctts_data.emplace_back();
            sc->ctts_data[sc->ctts_count].count = 1;
            sc->ctts_data[sc->ctts_count].duration = get_be32();
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
    const int result = mov_read_default(atom);
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
    }
    return result;
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

// ReSharper disable once CppMemberFunctionMayBeStatic
int MovDemuxer::mov_read_tkhd(MOVAtom atom) { return 0; }

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
    double nearest = standardRates[0];
    for (const double rate : standardRates)
        if (std::fabs(average - rate) < std::fabs(average - nearest))
            nearest = rate;
    return std::fabs(average - nearest) <= nearest * 0.001 ? nearest : average;
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
        m_firstTimecode[track] = units * 1000 / m_timescale;
    }
    m_emptyEditUnits.clear();
    // An audio track's edit list start in its own time scale, in nanoseconds, for a Matroska output's CodecDelay. It
    // was not read, so the priming came out as sound at the start of the track.
    for (const auto& [track, mediaTime] : m_editMediaTime)
    {
        if (track <= 0 || track > num_tracks || tracks[track - 1]->type != IOContextTrackType::AUDIO)
            continue;
        const auto st = reinterpret_cast<MOVStreamContext*>(tracks[track - 1]);
        if (st->time_scale > 0)
            m_codecDelayNs[track] = mediaTime * 1000000000LL / st->time_scale;
    }
    m_editMediaTime.clear();
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
            st->type = IOContextTrackType::AUDIO;
            st->parsed_priv_data = new MovParsedAudioTrackData(this, st);
            break;
        case MKTAG('t', 'x', '3', 'g'):
            st->type = IOContextTrackType::SUBTITLE;
            st->parsed_priv_data = new MovParsedSRTTrackData(this, st);
            break;
        case MKTAG('t', 'm', 'c', 'd'):
            st->type = IOContextTrackType::CONTROL;
            break;
        default:;
        }

        if (st->type == IOContextTrackType::VIDEO)
        {
            get_be16();                              // version
            get_be16();                              // revision level
            get_be32();                              // vendor
            get_be32();                              // temporal quality
            get_be32();                              // spatial quality
            get_be16();                              // width
            get_be16();                              // height
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
                    get_be32();                              // lcpm format specific flag
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

int MovDemuxer::mov_read_hdlr(MOVAtom atom)
{
    get_byte();  // version
    get_be24();  // flags

    // component type
    const unsigned ctype = get_le32();
    if (!ctype)
        isom = 1;

    get_le32();  // component subtype
    get_be32();  // component  manufacture
    get_be32();  // component flags
    get_be32();  // component flags mask

    skip_bytes(atom.size - (m_processedBytes - atom.offset));
    return 0;
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
