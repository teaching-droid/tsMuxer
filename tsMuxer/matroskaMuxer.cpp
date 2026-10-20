#include "matroskaMuxer.h"

#include <fs/directory.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>

extern "C"
{
#include "zlib.h"
}

#include "aac.h"
#include "aacStreamReader.h"
#include "ac3StreamReader.h"
#include "alacStreamReader.h"
#include "av1.h"
#include "av1StreamReader.h"
#include "avCodecs.h"
#include "doviLib.h"
#include "dtsStreamReader.h"
#include "flacStreamReader.h"
#include "h264StreamReader.h"
#include "hevc.h"
#include "hevcStreamReader.h"
#include "lpcmStreamReader.h"
#include "matroskaParser.h"
#include "mkvCopyStreamReader.h"
#include "mlpStreamReader.h"
#include "mpeg2StreamReader.h"
#include "mpegAudioStreamReader.h"
#include "muxerManager.h"
#include "nalUnits.h"
#include "opusStreamReader.h"
#include "simplePacketizerReader.h"
#include "trueHDAC3MergeReader.h"
#include "tsMuxer.h"  // HDR10_metadata, filled from the HEVC SEI while parsing
#include "vc1StreamReader.h"
#include "videoCopyStreamReader.h"
#include "vodCoreException.h"
#include "vorbisStreamReader.h"
#include "vpxStreamReader.h"
#include "vvc.h"
#include "vvcStreamReader.h"

// The Matroska TrackType values this muxer writes, those that are asked about away from where the tracks are built.
static constexpr uint8_t MKV_TRACK_TYPE_VIDEO = 1;
static constexpr uint8_t MKV_TRACK_TYPE_AUDIO = 2;
static constexpr uint8_t MKV_TRACK_TYPE_SUBTITLE = 17;
static constexpr uint8_t PGS_SEGMENT_PCS = 0x16;  // presentation composition, opens a display set
static constexpr uint8_t PGS_SEGMENT_END = 0x80;  // closes it

// Key for the AC-3 core companion of a TrueHD track (see refreshTrackProperties). Packets are
// routed by real stream index, so the companion is keyed clear of every real one.
static constexpr int AC3_CORE_STREAM_BASE = 0x40000000;

// Said in two places, so it lives in one: openDstFile refuses before the destination is touched,
// and refreshTrackProperties keeps the same refusal as a backstop for a pair that looked foldable
// and turned out not to be.
static constexpr char DV_PROFILE81_NEEDS_DUAL_LAYER[] =
    "--dv-profile=8.1 converts the Dolby Vision metadata of a DUAL LAYER source, and this mux has no "
    "dual layer Dolby Vision track to convert. A single layer file already carries its picture and its "
    "RPU in one track, so there is nothing to fold and nothing to convert: leave --dv-profile out and "
    "the file is written as it is. A dual layer disc needs BOTH of its video streams listed, the base "
    "layer and the enhancement layer.";

using namespace std;

// INTERNAL_PTS_PER_MS, the divisor from the internal PTS frequency to milliseconds, now lives in
// vod_common.h: the Dolby Vision splitter has to convert the same way round on the way back.

// A time in the internal clock as whole milliseconds, the nearest one, a half going up. The block times were cut
// down to the millisecond below, and a reader can only take a time as written (RFC 9559, 11): of 312 pictures at
// 23.976 fps, 143 were stored more than half a millisecond early. A time before the start goes to the nearest
// millisecond as well, not towards zero.
static int64_t internalToRoundedMs(const int64_t value)
{
    const int64_t shifted = value + INTERNAL_PTS_PER_MS / 2;
    return shifted >= 0 ? shifted / INTERNAL_PTS_PER_MS : -((-shifted + INTERNAL_PTS_PER_MS - 1) / INTERNAL_PTS_PER_MS);
}

// ═══════════════════════════════ EBML Writing Utilities ═══════════════════════════════

int ebml_id_size(uint32_t id)
{
    if (id < 0x80)
        return 0;  // invalid – IDs always have the VINT marker bit
    if (id <= 0xFF)
        return 1;
    if (id <= 0xFFFF)
        return 2;
    if (id <= 0xFFFFFF)
        return 3;
    return 4;
}

int ebml_write_id(uint8_t* dst, const uint32_t id)
{
    const int len = ebml_id_size(id);
    for (int i = len - 1; i >= 0; --i) dst[len - 1 - i] = static_cast<uint8_t>((id >> (i * 8)) & 0xFF);
    return len;
}

int ebml_size_size(uint64_t size)
{
    if (size < 0x7F)
        return 1;
    if (size < 0x3FFF)
        return 2;
    if (size < 0x1FFFFF)
        return 3;
    if (size < 0x0FFFFFFF)
        return 4;
    if (size < 0x07FFFFFFFF)
        return 5;
    if (size < 0x03FFFFFFFFFF)
        return 6;
    if (size < 0x01FFFFFFFFFFFF)
        return 7;
    return 8;
}

int ebml_write_size(uint8_t* dst, uint64_t size) { return ebml_write_size_fixed(dst, size, ebml_size_size(size)); }

int ebml_write_size_fixed(uint8_t* dst, uint64_t size, const int bytes)
{
    // The leading byte has the VINT_MARKER at position (8 - bytes) from MSB
    for (int i = bytes - 1; i >= 0; --i) dst[bytes - 1 - i] = static_cast<uint8_t>((size >> (i * 8)) & 0xFF);
    dst[0] |= static_cast<uint8_t>(1 << (8 - bytes));  // set VINT_MARKER
    return bytes;
}

int ebml_write_unknown_size(uint8_t* dst, const int bytes)
{
    // For an n-byte VINT "unknown size", the first byte has the VINT_MARKER
    // in bit (8-n) and all data bits set to 1.  Remaining bytes are all 0xFF.
    // e.g. 1-byte: 0xFF, 2-byte: 0x7F FF, 8-byte: 0x01 FF FF FF FF FF FF FF
    dst[0] = static_cast<uint8_t>(0xFF >> (bytes - 1));
    for (int i = 1; i < bytes; i++) dst[i] = 0xFF;
    return bytes;
}

// Return the minimum number of bytes needed to store a uint value
static int uint_size(uint64_t value)
{
    if (value == 0)
        return 1;
    int bytes = 0;
    while (value > 0)
    {
        value >>= 8;
        bytes++;
    }
    return bytes;
}

static int sint_size(int64_t value)
{
    if (value >= -128 && value <= 127)
        return 1;
    if (value >= -32768 && value <= 32767)
        return 2;
    if (value >= -8388608 && value <= 8388607)
        return 3;
    if (value >= -2147483648LL && value <= 2147483647LL)
        return 4;
    return 8;
}

int ebml_write_uint(uint8_t* dst, const uint32_t id, uint64_t value)
{
    int pos = ebml_write_id(dst, id);
    const int valSize = uint_size(value);
    pos += ebml_write_size(dst + pos, valSize);
    for (int i = valSize - 1; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_sint(uint8_t* dst, const uint32_t id, int64_t value)
{
    int pos = ebml_write_id(dst, id);
    const int valSize = sint_size(value);
    pos += ebml_write_size(dst + pos, valSize);
    for (int i = valSize - 1; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_float(uint8_t* dst, const uint32_t id, double value)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, 8);
    // Write 64-bit IEEE 754 big-endian
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 7; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_string(uint8_t* dst, const uint32_t id, const std::string& value)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, value.size());
    memcpy(dst + pos, value.data(), value.size());
    return pos + static_cast<int>(value.size());
}

int ebml_write_binary(uint8_t* dst, const uint32_t id, const uint8_t* data, int len)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, len);
    memcpy(dst + pos, data, len);
    return pos + len;
}

int ebml_write_master_open(uint8_t* dst, const uint32_t id, uint64_t contentSize)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, contentSize);
    return pos;
}

// ═══════════════════════════════ Matroska Muxer ══════════════════════════════════════

MatroskaMuxer::MatroskaMuxer(MuxerManager* owner)
    : AbstractMuxer(owner),
      m_nextTrackNumber(1),
      m_segmentStartPos(0),
      m_segmentSizePos(0),
      m_clusterTimecodeMs(0),
      m_clusterStartFilePos(0),
      m_clusterOpen(false),
      m_clusterDataSize(0),
      m_segmentInfoPos(0),
      m_tracksPos(0),
      m_cuesPos(0),
      m_firstTimecode(0),
      m_firstTimecodeSet(false),
      m_durationValueFilePos(0),
      m_headerWritten(false),
      m_destinationOpen(false)
{
}

MatroskaMuxer::~MatroskaMuxer() = default;

// ──────────────── Codec name mapping ──────────────────────────────────────────

std::string MatroskaMuxer::codecNameToMatroskaID(const std::string& codecName, int codecID)
{
    // Video
    if (codecName == "V_MPEG4/ISO/AVC")
        return MATROSKA_CODEC_ID_AVC_FOURCC;
    if (codecName == "V_MPEGH/ISO/HEVC")
        return MATROSKA_CODEC_ID_HEVC_FOURCC;
    if (codecName == "V_MPEGI/ISO/VVC")
        return MATROSKA_CODEC_ID_VVC_FOURCC;
    if (codecName == "V_AV1")
        return MATROSKA_CODEC_ID_AV1;
    if (codecName == "V_MS/VFW/WVC1")
        return MATROSKA_CODEC_ID_VIDEO_VFW_FOURCC;
    if (codecName == "V_MPEG-2")
        return MATROSKA_CODEC_ID_VIDEO_MPEG2;

    // Audio
    if (codecName == "A_AC3")
    {
        if (codecID == CODEC_A_EAC3)
            return MATROSKA_CODEC_ID_AUDIO_EAC3;
        if (codecID == CODEC_A_HDAC3)
            return MATROSKA_CODEC_ID_AUDIO_TRUEHD;
        return MATROSKA_CODEC_ID_AUDIO_AC3;
    }
    if (codecName == "A_AAC")
        return MATROSKA_CODEC_ID_AUDIO_AAC;
    if (codecName == "A_DTS")
        return MATROSKA_CODEC_ID_AUDIO_DTS;
    if (codecName == "A_LPCM")
        return MATROSKA_CODEC_ID_AUDIO_PCM_LIT;
    if (codecName == "A_MLP")
        return MATROSKA_CODEC_ID_AUDIO_TRUEHD;
    if (codecName == "A_MP3")
        return MATROSKA_CODEC_ID_AUDIO_MPEG_L3;
    if (codecName == "A_FLAC")
        return MATROSKA_CODEC_ID_AUDIO_FLAC;
    if (codecName == "A_OPUS")
        return MATROSKA_CODEC_ID_AUDIO_OPUS;

    // Subtitles
    if (codecName == "S_TEXT/UTF8")
        return MATROSKA_CODEC_ID_SRT;
    if (codecName == "S_HDMV/PGS")
        return MATROSKA_CODEC_ID_SUBTITLE_PGS;
    if (codecName == "S_SUP")
        return MATROSKA_CODEC_ID_SUBTITLE_PGS;

    return codecName;
}

// ──────────────── intAddStream ───────────────────────────────────────────────

void MatroskaMuxer::intAddStream(const std::string& /*streamName*/, const std::string& codecName, int streamIndex,
                                 const std::map<std::string, std::string>& params, AbstractStreamReader* codecReader)
{
    MkvTrackInfo track;
    track.streamIndex = streamIndex;
    track.trackNumber = m_nextTrackNumber++;
    track.codecReader = codecReader;
    track.codecID = codecReader->getCodecInfo().codecID;

    // The CodecID must describe what is IN the track, not what the meta line asked for. This used the
    // META's name, and a text subtitle is drawn into PGS pictures before it is written, so a track
    // said S_TEXT/UTF8 and held PGS segments: its blocks begin 0x16, 0x17, 0x14, which are PGS
    // segment types and are not UTF-8 at all. Nothing could read it as text and the text was not
    // recoverable from the file. The check in writeTracks cannot catch this, because both sides of
    // its comparison come from the same reader.
    //
    // The name the reader gives is in the same form as a meta name, and the codec id goes with it, so
    // E-AC3 and TrueHD, which both call themselves A_AC3, still come out right.
    const std::string metaCodecID = codecNameToMatroskaID(codecName, track.codecID);
    track.matroskaCodecID = codecNameToMatroskaID(codecReader->getCodecInfo().programName, track.codecID);
    // A subtitle copied from its Matroska source keeps the source's CodecID, whatever the meta line called it
    if (const auto* copy = dynamic_cast<const MatroskaCopyStreamReader*>(codecReader))
    {
        track.matroskaCodecID = copy->sourceCodecId();
        track.copyAddMappings = copy->addMappings();
        track.copyMaxBlockAddId = copy->maxBlockAddId();
    }
    if (track.codecID != CODEC_S_MKVCOPY && track.matroskaCodecID != metaCodecID)
        LTRACE(LT_WARN, 2,
               "Warning: the meta asks for "
                   << codecName << " but the data is " << codecReader->getCodecInfo().programName
                   << ", so the track is written "
                      "as "
                   << track.matroskaCodecID
                   << ". A text subtitle is drawn into pictures before it is written, so the "
                      "text itself is not kept in the file, unless the track asks for keep-text.");

    // Same parameter map the TS muxer reads the language from (tsMuxer.cpp). An explicit lang=
    // wins; "srclang" is the language muxerManager carried over from the source container.
    if (const auto itr = params.find("lang"); itr != params.end())
        track.language = itr->second;
    else if (const auto src = params.find("srclang"); src != params.end())
        track.language = src->second;

    // Only beside a language taken from the source: a lang= in the meta replaces the source's language, tag and all.
    // A lang= given as a BCP 47 tag (langbcp47) is written as that tag.
    if (const auto tag = params.find("langbcp47"); tag != params.end())
        track.languageBCP47 = tag->second;
    else if (params.find("lang") == params.end())
        if (const auto tag = params.find("srclangbcp47"); tag != params.end())
            track.languageBCP47 = tag->second;

    // A name with a comma in it has to be quoted in the meta, and the quotes stayed in the name.
    if (const auto nm = params.find("track-name"); nm != params.end())
        track.name = unquoteStr(nm->second);
    else if (const auto src = params.find("srcname"); src != params.end())
        track.name = src->second;  // the source's own name, which a remux used to drop

    // The source's track flags (srcflags: forced=1,hearing=0,...), which a remux used to drop.
    if (const auto src = params.find("srcflags"); src != params.end())
    {
        static const std::pair<const char*, uint32_t> NAMES[] = {
            {"forced", MATROSKA_ID_TRACKFLAGFORCED},         {"hearing", MATROSKA_ID_TRACKFLAGHEARINGIMPAIRED},
            {"visual", MATROSKA_ID_TRACKFLAGVISUALIMPAIRED}, {"descriptions", MATROSKA_ID_TRACKFLAGTEXTDESCRIPTIONS},
            {"original", MATROSKA_ID_TRACKFLAGORIGINAL},     {"commentary", MATROSKA_ID_TRACKFLAGCOMMENTARY}};
        for (const std::string& item : splitStr(src->second.c_str(), ','))
        {
            const size_t eq = item.find('=');
            for (const auto& [flagName, flagId] : NAMES)
                if (eq != std::string::npos && item.compare(0, eq, flagName) == 0)
                    track.flags[flagId] = item.substr(eq + 1) == "1" ? 1 : 0;
        }
    }
    // The same flags from the meta, which win over the source's: the name alone or =1 sets one, =0 clears it.
    {
        static const std::pair<const char*, uint32_t> OPTIONS[] = {
            {"forced", MATROSKA_ID_TRACKFLAGFORCED},
            {"hearing-impaired", MATROSKA_ID_TRACKFLAGHEARINGIMPAIRED},
            {"visual-impaired", MATROSKA_ID_TRACKFLAGVISUALIMPAIRED},
            {"text-descriptions", MATROSKA_ID_TRACKFLAGTEXTDESCRIPTIONS},
            {"original", MATROSKA_ID_TRACKFLAGORIGINAL},
            {"commentary", MATROSKA_ID_TRACKFLAGCOMMENTARY}};
        for (const auto& [option, flagId] : OPTIONS)
            if (const auto it = params.find(option); it != params.end())
                track.flags[flagId] = it->second.empty() || !defaultParamMeansNo(it->second) ? 1 : 0;
    }
    // Container colour and mastering metadata (srccolour: m=9;t=16;...), for when the stream carries none.
    if (const auto src = params.find("srccolour"); src != params.end())
        for (const std::string& item : splitStr(src->second.c_str(), ';'))
            if (const size_t eq = item.find('='); eq != std::string::npos)
                track.srcColour[item.substr(0, eq)] = strtod(item.c_str() + eq + 1, nullptr);
    // The samples at the end of an Opus track that only fill its last frame (srcendpadding, in nanoseconds).
    if (const auto src = params.find("srcendpadding");
        src != params.end() && (track.codecID == CODEC_A_OPUS || track.codecID == CODEC_A_VORBIS))
        track.endPaddingNs = strtoll(src->second.c_str(), nullptr, 10);
    // The crop, the field order and the projection of a Matroska source (srccrop: top,bottom,left,right;
    // srcfieldorder; srcprojection: the element's body in hex).
    if (const auto src = params.find("srccrop"); src != params.end())
    {
        const std::vector<std::string> parts = splitStr(src->second.c_str(), ',');
        for (size_t i = 0; i < 4 && i < parts.size(); ++i) track.srcCrop[i] = strtoull(parts[i].c_str(), nullptr, 10);
    }
    if (const auto src = params.find("srcfieldorder"); src != params.end())
        track.srcFieldOrder = static_cast<int>(strtol(src->second.c_str(), nullptr, 10));
    if (const auto src = params.find("srcprojection"); src != params.end())
        for (size_t i = 0; i + 1 < src->second.size(); i += 2)
            track.srcProjection.push_back(static_cast<uint8_t>(strtoul(src->second.substr(i, 2).c_str(), nullptr, 16)));
    if (const auto src = params.find("srcvpxprivate"); src != params.end() && track.codecID == CODEC_V_VP9)
        for (size_t i = 0; i + 1 < src->second.size(); i += 2)
            track.srcVpxPrivate.push_back(static_cast<uint8_t>(strtoul(src->second.substr(i, 2).c_str(), nullptr, 16)));
    // The priming at the start of an AAC track, from an MP4 source's edit list (srccodecdelay, in nanoseconds).
    if (const auto src = params.find("srccodecdelay"); src != params.end() && track.codecID == CODEC_A_AAC)
        track.srcCodecDelayNs = strtoull(src->second.c_str(), nullptr, 10);
    // The size the container said to show the picture at (srcdisplay: 640x480). It says more than the stream's own
    // aspect ratio, which for square pixels is just the picture's shape; an ar= in the meta says more than both.
    if (const auto src = params.find("srcdisplay"); src != params.end() && params.find("ar") == params.end())
    {
        const size_t x = src->second.find('x');
        if (x != std::string::npos)
        {
            track.srcDisplayWidth = static_cast<unsigned>(strtoul(src->second.c_str(), nullptr, 10));
            track.srcDisplayHeight = static_cast<unsigned>(strtoul(src->second.c_str() + x + 1, nullptr, 10));
        }
    }
    if (const auto src = params.find("srcstereomode"); src != params.end())
        track.srcStereoMode = static_cast<unsigned>(strtoul(src->second.c_str(), nullptr, 10));
    // "default" already exists and is documented for Blu-ray; it just never reached Matroska.
    // Its VALUE is read now. Testing only for presence meant default=0 marked the track as THE
    // default, and it also meant "no default for this type" could not be said at all, because a
    // type with nothing marked falls back to its first track.
    const auto defaultParam = params.find("default");
    track.defaultExplicit = defaultParam != params.end();
    track.markedDefault = track.defaultExplicit && !defaultParamMeansNo(defaultParam->second);
    // default=forced, also set from a subtitle file named "forced", is the forced track. Matroska has a flag for that,
    // FlagForced, and it was not written: the track came out as the default one and nothing more.
    if (track.defaultExplicit && defaultParamKind(defaultParam->second) == 2)
        track.flags[MATROSKA_ID_TRACKFLAGFORCED] = 1;
    if (!track.defaultExplicit)
    {
        // Nothing said in the meta, so the source container's own flag stands in. METADemuxer puts
        // it here for a Matroska source, where it was previously parsed and discarded: a remux
        // replaced the source's choice with the first track of each type.
        if (const auto srcDefault = params.find("srcdefault"); srcDefault != params.end())
        {
            track.defaultExplicit = true;
            track.markedDefault = srcDefault->second == "1";
        }
    }
    track.dropAc3Core = params.find("drop-ac3-core") != params.end();

    // Generate a random UID
    static std::mt19937_64 rng(std::random_device{}());
    // The top bit set: still random in 63 bits, never zero, and always written in 8 bytes. At its
    // shortest, one UID in 256 took 7, and every byte of the file after it moved, so two runs of
    // the same mux did not even agree on where the clusters were.
    track.trackUID = rng() | 1ull << 63;

    // Determine track type & properties
    if (codecName[0] == 'V')
    {
        track.trackType = MKV_TRACK_TYPE_VIDEO;
        const auto mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
        if (mpegReader)
        {
            track.width = mpegReader->getStreamWidth();
            track.height = mpegReader->getStreamHeight();
            track.fps = mpegReader->getFPS();
            track.interlaced = mpegReader->getInterlaced();
        }
    }
    else if (codecName[0] == 'A')
    {
        track.trackType = 2;  // audio
        const auto simpleReader = dynamic_cast<SimplePacketizerReader*>(codecReader);
        if (simpleReader)
        {
            track.sampleRate = simpleReader->getFreq();
            track.channels = simpleReader->getChannels();
        }
        // Bit depth for LPCM
        const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(codecReader);
        if (lpcmReader)
        {
            // 20 bit samples are carried in 3 bytes each, and A_PCM/INT/LIT has no other field to give the size of a
            // sample: BitDepth 20 over 3 byte samples was read as 20 bit samples, noise at 1.5 times the length.
            track.bitDepth = lpcmReader->m_bitsPerSample == 20 ? 24 : lpcmReader->m_bitsPerSample;
            // A_PCM/INT/LIT is plain little endian PCM in WAV channel order. Left to itself the
            // reader produces Blu-ray LPCM: a 4 byte header on every 5 ms frame, big endian samples,
            // Blu-ray channel order and a silent extra channel when the count is odd. Every other
            // player decoded that as noise. Its demux mode is the one that writes .wav, which is
            // exactly the payload this codec ID promises.
            lpcmReader->setDemuxMode(true);
        }
    }
    else if (codecName[0] == 'S')
    {
        track.trackType = MKV_TRACK_TYPE_SUBTITLE;
    }

    m_tracks[streamIndex] = track;
}

// ──────────────── Codec Private builders ─────────────────────────────────────

std::vector<uint8_t> MatroskaMuxer::buildAVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto h264 = dynamic_cast<H264StreamReader*>(reader);
    if (!h264)
        return {};

    // Collect SPS and PPS NAL units
    std::vector<std::pair<std::vector<uint8_t>, int>> spsUnits, ppsUnits;

    for (auto& [id, sps] : h264->m_spsMap)
    {
        uint8_t buf[4096];
        const int len = sps->serializeBuffer(buf, buf + sizeof(buf), false);
        if (len > 0)
            spsUnits.push_back({std::vector<uint8_t>(buf, buf + len), id});
    }
    for (auto& [id, pps] : h264->m_ppsMap)
    {
        uint8_t buf[4096];
        const int len = pps->serializeBuffer(buf, buf + sizeof(buf), false);
        if (len > 0)
            ppsUnits.push_back({std::vector<uint8_t>(buf, buf + len), id});
    }

    if (spsUnits.empty())
        return {};

    // Parse first SPS to extract profile/level
    const auto& firstSps = spsUnits[0].first;
    uint8_t profileIdc = firstSps.size() > 1 ? firstSps[1] : 66;
    uint8_t profileCompat = firstSps.size() > 2 ? firstSps[2] : 0;
    uint8_t levelIdc = firstSps.size() > 3 ? firstSps[3] : 30;

    std::vector<uint8_t> record;
    record.push_back(1);  // configurationVersion
    record.push_back(profileIdc);
    record.push_back(profileCompat);
    record.push_back(levelIdc);
    record.push_back(0xFF);  // lengthSizeMinusOne = 3 (4-byte NAL length) | reserved 0xFC
    record.push_back(static_cast<uint8_t>(0xE0 | (spsUnits.size() & 0x1F)));  // numSPS | reserved 0xE0

    for (auto& [data, id] : spsUnits)
    {
        const uint16_t sz = static_cast<uint16_t>(data.size());
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz & 0xFF));
        record.insert(record.end(), data.begin(), data.end());
    }

    record.push_back(static_cast<uint8_t>(ppsUnits.size()));
    for (auto& [data, id] : ppsUnits)
    {
        const uint16_t sz = static_cast<uint16_t>(data.size());
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz & 0xFF));
        record.insert(record.end(), data.begin(), data.end());
    }

    // ISO/IEC 14496-15 gives the record of a profile 100, 110, 122 or 144 stream four more bytes: the chroma format,
    // the two bit depths and the number of SPS extensions. They were left out, so the record of every High profile
    // stream ended early, and a reader following the standard met the end of the data where it expects them.
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 144)
    {
        const SPSUnit* sps = h264->m_spsMap.begin()->second;
        record.push_back(static_cast<uint8_t>(0xFC | (sps->chroma_format_idc & 3)));
        record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_luma_minus8 & 7)));
        record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_chroma_minus8 & 7)));
        record.push_back(0);  // numOfSequenceParameterSetExt
    }

    return record;
}

// The MVC configuration record for a 3D pair. Measured off a reference 3D file before it was
// written: the same shape as the AVC record, but carrying BOTH views' parameter sets. The base
// view's SPS comes first and the dependent view's SUBSET SPS second, then both views' PPS.
//
// The profile and level are taken from the subset SPS, not the base one, because they describe the
// PAIR. A 3D disc reads 128 there, Stereo High, where the base view on its own says 100.
std::vector<uint8_t> MatroskaMuxer::buildMVCDecoderConfigRecord(AbstractStreamReader* baseReader,
                                                                AbstractStreamReader* depReader)
{
    const auto h264Base = dynamic_cast<H264StreamReader*>(baseReader);
    const auto h264Dep = dynamic_cast<H264StreamReader*>(depReader);
    if (!h264Base || !h264Dep)
        return {};

    const auto serialize = [](const auto* ps) -> std::vector<uint8_t>
    {
        uint8_t buf[4096];
        const int len = ps->serializeBuffer(buf, buf + sizeof(buf), false);
        return len > 0 ? std::vector<uint8_t>(buf, buf + len) : std::vector<uint8_t>();
    };
    const auto nalTypeIs = [](const std::vector<uint8_t>& v, const NALUnit::NALType t)
    { return !v.empty() && (v[0] & 0x1F) == static_cast<int>(t); };

    std::vector<std::vector<uint8_t>> spsUnits, ppsUnits;
    size_t subsetSpsIdx = SIZE_MAX;

    for (auto& [id, sps] : h264Base->m_spsMap)
        if (auto v = serialize(sps); !v.empty())
            spsUnits.push_back(std::move(v));

    // From the dependent view take the SUBSET sequence parameter set only. Its map can also hold a
    // copy of the base view's own SPS, and writing that one twice would describe a stream that does
    // not exist.
    for (auto& [id, sps] : h264Dep->m_spsMap)
    {
        auto v = serialize(sps);
        if (!nalTypeIs(v, NALUnit::NALType::nuSubSPS))
            continue;
        subsetSpsIdx = spsUnits.size();
        spsUnits.push_back(std::move(v));
    }

    // No subset SPS means the second stream is not a dependent view, whatever it was labelled, and
    // a record built from it would be a promise the file cannot keep.
    if (subsetSpsIdx == SIZE_MAX || spsUnits.empty())
        return {};

    for (auto& [id, pps] : h264Base->m_ppsMap)
        if (auto v = serialize(pps); !v.empty())
            ppsUnits.push_back(std::move(v));
    for (auto& [id, pps] : h264Dep->m_ppsMap)
    {
        auto v = serialize(pps);
        if (v.empty())
            continue;
        // The dependent view repeats the base view's picture parameter sets as well as adding its
        // own. Compared by bytes rather than by id, because the two streams number them separately.
        if (std::find(ppsUnits.begin(), ppsUnits.end(), v) == ppsUnits.end())
            ppsUnits.push_back(std::move(v));
    }

    const std::vector<uint8_t>& subset = spsUnits[subsetSpsIdx];
    if (subset.size() < 4)
        return {};

    std::vector<uint8_t> record;
    record.push_back(1);          // configurationVersion
    record.push_back(subset[1]);  // profile_idc, from the subset SPS: 128, Stereo High
    record.push_back(subset[2]);  // profile compatibility flags
    record.push_back(subset[3]);  // level_idc
    record.push_back(0xFF);       // lengthSizeMinusOne = 3, four byte NAL lengths | reserved
    record.push_back(static_cast<uint8_t>(0xE0 | (spsUnits.size() & 0x1F)));

    const auto appendSet = [&record](const std::vector<uint8_t>& v)
    {
        const uint16_t sz = static_cast<uint16_t>(v.size());
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz & 0xFF));
        record.insert(record.end(), v.begin(), v.end());
    };
    for (const auto& v : spsUnits) appendSet(v);
    record.push_back(static_cast<uint8_t>(ppsUnits.size()));
    for (const auto& v : ppsUnits) appendSet(v);

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildHEVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto hevc = dynamic_cast<HEVCStreamReader*>(reader);
    if (!hevc || !hevc->m_sps || !hevc->m_vps)
        return {};

    // Serialize each parameter set
    uint8_t buf[8192];
    std::vector<uint8_t> vpsData, spsData, ppsData;

    int len = hevc->m_vps->serializeBuffer(buf, buf + sizeof(buf));
    if (len > 0)
        vpsData.assign(buf, buf + len);

    len = hevc->m_sps->serializeBuffer(buf, buf + sizeof(buf));
    if (len > 0)
        spsData.assign(buf, buf + len);

    if (hevc->m_pps)
    {
        len = hevc->m_pps->serializeBuffer(buf, buf + sizeof(buf));
        if (len > 0)
            ppsData.assign(buf, buf + len);
    }

    if (spsData.empty())
        return {};

    // Build HEVCDecoderConfigurationRecord
    const HevcSpsUnit* sps = hevc->m_sps;
    std::vector<uint8_t> record;
    record.push_back(1);  // configurationVersion

    // general_profile_space, general_tier_flag, general_profile_idc, general_profile_compatibility_flags,
    // general_constraint_indicator_flags and general_level_idc: the first 12 bytes of the SPS's profile_tier_level,
    // which starts on a byte boundary right after its first byte. They were written as the profile and level alone,
    // with the tier, every compatibility flag and every constraint flag 0, so a High tier Main 10 stream, compatible
    // with Main 10 and progressive, was described as Main tier and compatible with nothing.
    const uint8_t* rbsp = sps->nalBuffer();
    uint8_t temporal = 0x0F;  // numTemporalLayers 1, temporalIdNested 1, lengthSizeMinusOne 3
    if (rbsp && sps->nalBufferLen() >= 15)
    {
        record.insert(record.end(), rbsp + 3, rbsp + 15);
        // sps_max_sub_layers_minus1 and sps_temporal_id_nesting_flag, from the byte before them
        temporal = static_cast<uint8_t>((((rbsp[2] >> 1) & 7) + 1) << 3 | (rbsp[2] & 1) << 2 | 3);
    }
    else
    {
        record.push_back(static_cast<uint8_t>(sps->profile_idc & 0x1F));
        for (int i = 0; i < 10; i++) record.push_back(0);
        record.push_back(static_cast<uint8_t>(sps->level_idc));
    }
    // min_spatial_segmentation_idc
    record.push_back(0xF0);
    record.push_back(0x00);
    // parallelismType
    record.push_back(0xFC);
    // chromaFormatIdc
    record.push_back(static_cast<uint8_t>(0xFC | (sps->chromaFormat & 0x03)));
    // bitDepthLumaMinus8
    record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_luma_minus8 & 0x07)));
    // bitDepthChromaMinus8
    record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_chroma_minus8 & 0x07)));
    // avgFrameRate
    record.push_back(0);
    record.push_back(0);
    // constantFrameRate(2) 0 | numTemporalLayers(3) | temporalIdNested(1) | lengthSizeMinusOne(2) 3
    record.push_back(temporal);

    int numArrays = 0;
    if (!vpsData.empty())
        numArrays++;
    if (!spsData.empty())
        numArrays++;
    if (!ppsData.empty())
        numArrays++;
    record.push_back(static_cast<uint8_t>(numArrays));

    // VPS array
    if (!vpsData.empty())
    {
        record.push_back(0x20);  // array_completeness=0 | NAL_unit_type=32 (VPS)
        record.push_back(0);
        record.push_back(1);  // numNalus
        record.push_back(static_cast<uint8_t>(vpsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(vpsData.size() & 0xFF));
        record.insert(record.end(), vpsData.begin(), vpsData.end());
    }

    // SPS array
    if (!spsData.empty())
    {
        record.push_back(0x21);  // NAL_unit_type=33 (SPS)
        record.push_back(0);
        record.push_back(1);
        record.push_back(static_cast<uint8_t>(spsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(spsData.size() & 0xFF));
        record.insert(record.end(), spsData.begin(), spsData.end());
    }

    // PPS array
    if (!ppsData.empty())
    {
        record.push_back(0x22);  // NAL_unit_type=34 (PPS)
        record.push_back(0);
        record.push_back(1);
        record.push_back(static_cast<uint8_t>(ppsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(ppsData.size() & 0xFF));
        record.insert(record.end(), ppsData.begin(), ppsData.end());
    }

    return record;
}

// V_MPEGI/ISO/VVC carries a VvcDecoderConfigurationRecord (ISO/IEC 14496-15, 11.2.4.2) in CodecPrivate. What went there
// before was the parameter sets with a four byte length each and nothing else, which no reader takes for a record: the
// first byte was read as the length size and the profile, a decoder found no picture in any block, and a file of 60
// pictures decoded to none. The record says the NAL units in the blocks have four byte lengths, carries the profile,
// tier and level of the SPS byte for byte, and holds the parameter sets in arrays. array_completeness is 0 because the
// blocks repeat the parameter sets.
std::vector<uint8_t> MatroskaMuxer::buildVVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto vvc = dynamic_cast<VVCStreamReader*>(reader);
    if (!vvc || !vvc->m_sps || vvc->m_spsBuffer.size() == 0)
        return {};
    const VvcSpsUnit& sps = *vvc->m_sps;

    std::vector<uint8_t> record;
    const bool ptlPresent = !sps.ptl_bytes.empty();
    record.push_back(static_cast<uint8_t>(0xF8 | 3 << 1 | (ptlPresent ? 1 : 0)));  // reserved, LengthSizeMinusOne
    if (ptlPresent)
    {
        const unsigned numSublayers = sps.max_sublayers_minus1 + 1u;
        // ols_idx (9 bits) 0, num_sublayers (3), constant_frame_rate (2) 0 = not known, chroma_format_idc (2)
        const unsigned word = numSublayers << 4 | (sps.chroma_format_idc & 3u);
        record.push_back(static_cast<uint8_t>(word >> 8));
        record.push_back(static_cast<uint8_t>(word));
        record.push_back(static_cast<uint8_t>((sps.bitdepth_minus8 & 7u) << 5 | 0x1F));
        record.push_back(sps.ptl_constraint_info_bytes & 0x3F);  // VvcPTLRecord: reserved, num_bytes_constraint_info
        record.insert(record.end(), sps.ptl_bytes.begin(), sps.ptl_bytes.end());
        for (const unsigned v : {sps.pic_width_max_in_luma_samples, sps.pic_height_max_in_luma_samples, 0u})
        {
            record.push_back(static_cast<uint8_t>(v >> 8));  // max_picture_width, max_picture_height, avg_frame_rate
            record.push_back(static_cast<uint8_t>(v));
        }
    }
    std::vector<std::pair<uint8_t, const MemoryBlock*>> arrays;
    if (vvc->m_vpsBuffer.size() > 0)
        arrays.emplace_back(static_cast<uint8_t>(VvcUnit::NalType::VPS), &vvc->m_vpsBuffer);
    arrays.emplace_back(static_cast<uint8_t>(VvcUnit::NalType::SPS), &vvc->m_spsBuffer);
    if (vvc->m_ppsBuffer.size() > 0)
        arrays.emplace_back(static_cast<uint8_t>(VvcUnit::NalType::PPS), &vvc->m_ppsBuffer);
    record.push_back(static_cast<uint8_t>(arrays.size()));
    for (const auto& [type, nal] : arrays)
    {
        const auto size = static_cast<uint16_t>(nal->size());
        record.push_back(type & 0x1F);  // array_completeness 0, reserved, NAL_unit_type
        record.push_back(0);            // num_nalus
        record.push_back(1);
        record.push_back(static_cast<uint8_t>(size >> 8));
        record.push_back(static_cast<uint8_t>(size));
        record.insert(record.end(), nal->data(), nal->data() + size);
    }
    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildAV1ConfigRecord(AbstractStreamReader* reader)
{
    const auto av1 = dynamic_cast<AV1StreamReader*>(reader);
    if (!av1 || !av1->m_seqHdrFound)
        return {};

    // AV1CodecConfigurationRecord (4 bytes) + sequence header OBU in low-overhead format
    const auto& hdr = av1->m_seqHdr;
    std::vector<uint8_t> record;

    // marker(1) | version(7) = 0x81
    record.push_back(0x81);
    // seq_profile(3) | seq_level_idx_0(5)
    record.push_back(static_cast<uint8_t>(((hdr.seq_profile & 0x07) << 5) | (hdr.seq_level_idx_0 & 0x1F)));
    // seq_tier_0(1) | high_bitdepth(1) | twelve_bit(1) | monochrome(1) |
    // chroma_subsampling_x(1) | chroma_subsampling_y(1) | chroma_sample_position(2)
    const uint8_t bitDepth = hdr.getBitDepth();
    const uint8_t highBitdepth = (bitDepth > 8) ? 1 : 0;
    const uint8_t twelveBit = (bitDepth == 12) ? 1 : 0;
    record.push_back(static_cast<uint8_t>(((hdr.seq_tier_0 & 1) << 7) |  // seq_tier_0, which was always written 0
                                          (highBitdepth << 6) |          // high_bitdepth
                                          (twelveBit << 5) |             // twelve_bit
                                          (hdr.mono_chrome << 4) | (hdr.chroma_subsampling_x << 3) |
                                          (hdr.chroma_subsampling_y << 2) | (hdr.chroma_sample_position & 0x03)));
    // initial_presentation_delay_present(1) | reserved/initial_presentation_delay_minus_one(3) | padding(4)
    record.push_back(0x00);

    // configOBUs: the sequence header OBU, as the file stores it (low overhead format, with its size field, no
    // emulation prevention). The record ended after its four bytes, which is allowed, but the Matroska mapping of AV1
    // has the CodecPrivate carry the sequence header so that a reader need not look for it in the first block.
    const std::vector<uint8_t> seqHdrObu = av1->sequenceHeaderObu();
    record.insert(record.end(), seqHdrObu.begin(), seqHdrObu.end());

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildAACConfig(AbstractStreamReader* reader)
{
    const auto aac = dynamic_cast<AACStreamReader*>(reader);
    if (!aac)
        return {};

    // audioObjectType(5 bits) | samplingFrequencyIndex(4 bits) | channelConfiguration(4 bits) | 3 flags, and a
    // program config element after them when the layout is given by one
    return aac->audioSpecificConfig();
}

void MatroskaMuxer::buildCodecPrivate(MkvTrackInfo& track)
{
    switch (track.codecID)
    {
    case CODEC_V_MPEG4_H264:
        track.codecPrivate = buildAVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_MPEG4_H265:
        track.codecPrivate = buildHEVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_MPEG4_H266:
        track.codecPrivate = buildVVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_AV1:
        track.codecPrivate = buildAV1ConfigRecord(track.codecReader);
        break;
    case CODEC_V_VC1:
    {
        // V_MS/VFW/FOURCC is described entirely by a BITMAPINFOHEADER: without one a reader cannot
        // even tell which codec the track holds. It used to be written EMPTY, so other software
        // dropped the video or refused the file. For WVC1 the header is followed by a zero byte
        // and the stream's own sequence header and entry point, start codes included.
        const auto vc1 = dynamic_cast<VC1StreamReader*>(track.codecReader);
        if (!vc1 || vc1->getSequenceHeader().empty())
            break;
        std::vector<uint8_t> extra(1, 0);
        extra.insert(extra.end(), vc1->getSequenceHeader().begin(), vc1->getSequenceHeader().end());
        extra.insert(extra.end(), vc1->getEntryPoint().begin(), vc1->getEntryPoint().end());
        const uint32_t width = vc1->getCodedWidth();
        const uint32_t height = vc1->getCodedHeight();
        const uint32_t fields[] = {static_cast<uint32_t>(40 + extra.size()), width, height};
        std::vector<uint8_t>& bih = track.codecPrivate;
        bih.clear();
        auto le32 = [&bih](const uint32_t v)
        {
            for (int i = 0; i < 4; ++i) bih.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        for (const uint32_t f : fields) le32(f);  // biSize, biWidth, biHeight
        bih.push_back(1);                         // biPlanes, 16 bit
        bih.push_back(0);
        bih.push_back(24);  // biBitCount, 16 bit
        bih.push_back(0);
        for (const char c : {'W', 'V', 'C', '1'}) bih.push_back(static_cast<uint8_t>(c));  // biCompression
        le32(width * height * 3);                                                          // biSizeImage
        le32(0);                                                                           // biXPelsPerMeter
        le32(0);                                                                           // biYPelsPerMeter
        le32(0);                                                                           // biClrUsed
        le32(0);                                                                           // biClrImportant
        bih.insert(bih.end(), extra.begin(), extra.end());
        break;
    }
    case CODEC_A_AAC:
        track.codecPrivate = buildAACConfig(track.codecReader);
        break;
    case CODEC_A_FLAC:
    {
        const auto flac = dynamic_cast<FLACStreamReader*>(track.codecReader);
        if (flac && !flac->getCodecPrivate().empty())
            track.codecPrivate = flac->getCodecPrivate();
        break;
    }
    case CODEC_A_OPUS:
    {
        const auto opus = dynamic_cast<OpusStreamReader*>(track.codecReader);
        if (opus && !opus->getCodecPrivate().empty())
            track.codecPrivate = opus->getCodecPrivate();
        break;
    }
    case CODEC_A_ALAC:
    {
        // The ALACSpecificConfig, as the source had it.
        const auto alac = dynamic_cast<AlacStreamReader*>(track.codecReader);
        if (alac)
            track.codecPrivate = alac->getCodecPrivate();
        break;
    }
    case CODEC_A_VORBIS:
    {
        // The three header packets, Xiph laced, as the source had them.
        const auto vorbis = dynamic_cast<VorbisStreamReader*>(track.codecReader);
        if (vorbis)
            track.codecPrivate = vorbis->getCodecPrivate();
        break;
    }
    case CODEC_V_VP9:
        // Optional for VP9, and kept as the source wrote it; VP8 has none.
        track.codecPrivate = track.srcVpxPrivate;
        break;
    case CODEC_V_PRORES:
        // The FourCC of the sample entry, as the codec specifications of Matroska ask
        if (const auto* copy = dynamic_cast<const VideoCopyStreamReader*>(track.codecReader))
            for (int i = 3; i >= 0; --i) track.codecPrivate.push_back(static_cast<uint8_t>(copy->fourcc() >> (8 * i)));
        break;
    case CODEC_S_MKVCOPY:
        // The source's own, as it stands: the ASS or SSA script header, the VobSub .idx, the DVB page ids
        if (const auto* copy = dynamic_cast<const MatroskaCopyStreamReader*>(track.codecReader))
            track.codecPrivate = copy->codecPrivate();
        break;
    default:
        // AC3, DTS, LPCM, SRT, PGS, MPEG-2, VC-1 etc. – no codec private needed in MKV
        // (or it's handled differently)
        break;
    }
}

// ──────────────── parseMuxOpt ────────────────────────────────────────────────

void MatroskaMuxer::parseMuxOpt(const std::string& opts)
{
    // Splitting belongs to the transport stream writer; this one has no notion of it. Asking for it
    // on Matroska output used to produce ONE file and "Mux successful complete" with nothing said,
    // so a 421 MB source asked to split at 50 MB came back whole while the same meta aimed at .ts
    // gave nine parts. Say so, the way the demux path already does for the same reason.
    for (const std::string& token : splitQuotedStr(opts.c_str(), ' '))
    {
        const size_t eq = token.find('=');
        const std::string key = token.substr(0, eq);
        const std::string value = eq == std::string::npos ? std::string() : token.substr(eq + 1);
        if ((key == "--split-size" || key == "--split-duration") && value.empty())
            THROW(ERR_COMMON, "Missing value for " << key)
        if (key == "--split-size")
            m_splitSize = checkedSplitSize(value);
        else if (key == "--split-duration")
        {
            m_splitDuration = checkedSplitDuration(value);
        }
        else if (key == "--split-chapters")
            m_splitByChapters = true;
    }

    // --title="..." names the file (the segment's Title), over any title a Matroska source has.
    for (const std::string& token : splitQuotedStr(opts.c_str(), ' '))
        if (token.compare(0, 8, "--title=") == 0)
        {
            m_title = unquoteStr(token.substr(8));
            m_titleFromMuxOpt = true;
        }

    // --dv-profile=<7|8.1>. 7 is the default and means "carry the disc as it is", the faithful dual
    // layer track. 8.1 converts the RPU so the file plays as single layer Dolby Vision on the many
    // devices that do not understand profile 7.
    const std::string key = "--dv-profile=";
    const size_t at = opts.find(key);
    if (at == std::string::npos)
        return;

    size_t valStart = at + key.size();
    size_t valEnd = opts.find_first_of(" \t", valStart);
    if (valEnd == std::string::npos)
        valEnd = opts.size();
    const std::string value = opts.substr(valStart, valEnd - valStart);

    if (value == "7")
    {
        m_dvWriteProfile81 = false;
    }
    else if (value == "8.1" || value == "81")
    {
        // The conversion to 8.1 cannot be undone on its own: it is many to one, so nothing can
        // recover a profile 7 RPU from an 8.1 one. What makes it safe is that the disc's own RPUs
        // travel with the file as an attachment and are put back when it is split, which is
        // measured rather than assumed, on whole features, byte for byte.
        //
        // Checked HERE rather than when the first RPU turns up, because that would be most of the
        // way through a mux of a feature. The message names the library and what to do about it.
        // When the file IS there, telling the user to go and get it would be wrong: the reason is all
        // they need. Unless it is not the library at all (wrongLibrary): then the right one is fetched.
        if (!DoviLib::instance().available() && DoviLib::instance().fileFound() && !DoviLib::instance().wrongLibrary())
        {
            THROW(ERR_COMMON, "--dv-profile=8.1 needs "
                                  << DoviLib::libraryName() << ", which converts the Dolby Vision metadata, and "
                                  << DoviLib::instance().loadError() << ". Or leave --dv-profile at 7.")
        }
        if (!DoviLib::instance().available())
        {
            THROW(ERR_COMMON, "--dv-profile=8.1 needs "
                                  << DoviLib::libraryName() << ", which converts the Dolby Vision metadata, and "
                                  << DoviLib::instance().loadError()
                                  << (DoviLib::instance().wrongLibrary() ? ". Put the library itself there instead"
                                                                         : ". Put it beside tsMuxeR")
                                  << ". On 64 bit Windows take "
                                     "libdovi-<version>-x86_64-pc-windows-msvc.zip from the dovi_tool releases page "
                                     "and use the dovi.dll inside it. Do NOT take dovi_tool-<version>.zip from the "
                                     "same page: that is the command line tool and this cannot use it. Other "
                                     "platforms build the library from source. Or leave --dv-profile at 7.")
        }
        m_dvWriteProfile81 = true;
    }
    else
    {
        THROW(ERR_COMMON, "Invalid --dv-profile value '" << value << "'. Expected 7 or 8.1.")
    }
}

// ──────────────── File I/O helpers ───────────────────────────────────────────

void MatroskaMuxer::writeToFile(const uint8_t* data, int len)
{
    if (len > 0)
    {
        m_file.write(data, len);
        if (m_crcActive)
            m_crcValue = static_cast<uint32_t>(crc32(m_crcValue, data, static_cast<uInt>(len)));
    }
}

void MatroskaMuxer::writeToFile(const std::vector<uint8_t>& data)
{
    if (!data.empty())
        writeToFile(data.data(), static_cast<int>(data.size()));
}

// The CRC-32 element: ID 0xBF, size 4, then the checksum (IEEE 802.3) of everything in the parent after it, stored
// little endian (RFC 8794, 11.3.1). Six bytes, which every top-level element counts in its size.
static constexpr int CRC_ELEMENT_BYTES = 6;

// WebM has no CRC-32. Its place is taken by a Void element of the same six bytes, so no size changes.
void MatroskaMuxer::putCrcElement(uint8_t* at, const uint32_t crc) const
{
    at[0] = m_webm ? 0xEC : 0xBF;
    at[1] = 0x84;
    for (int i = 0; i < 4; ++i) at[2 + i] = m_webm ? 0 : static_cast<uint8_t>(crc >> (8 * i));
}

void MatroskaMuxer::beginCrc()
{
    uint8_t element[CRC_ELEMENT_BYTES];
    putCrcElement(element, 0);
    writeToFile(element, CRC_ELEMENT_BYTES);
    m_crcValuePos = m_file.pos() - 4;
    m_crcValue = static_cast<uint32_t>(crc32(0L, Z_NULL, 0));
    m_crcActive = true;
}

void MatroskaMuxer::endCrc()
{
    m_crcActive = false;
    uint8_t element[CRC_ELEMENT_BYTES];
    putCrcElement(element, m_crcValue);
    const int64_t resume = m_file.pos();
    m_file.seek(m_crcValuePos);
    m_file.write(element + 2, 4);
    m_file.seek(resume);
}

void MatroskaMuxer::patchHeaderBody(std::vector<uint8_t>& body, const int64_t bodyFilePos,
                                    const int64_t crcValueFilePos, const size_t at, const uint8_t* bytes,
                                    const size_t len)
{
    if (body.empty() || at + len > body.size())
        return;
    memcpy(body.data() + at, bytes, len);
    m_file.seek(bodyFilePos + static_cast<int64_t>(at));
    m_file.write(bytes, static_cast<int>(len));
    uint8_t element[CRC_ELEMENT_BYTES];
    putCrcElement(element, static_cast<uint32_t>(crc32(0L, body.data(), static_cast<uInt>(body.size()))));
    m_file.seek(crcValueFilePos);
    m_file.write(element + 2, 4);
}

// A Void element occupying EXACTLY totalBytes, header included, so a reserved area can be filled
// without its size shifting. The size field is written at a fixed two bytes rather than the
// shortest that fits, because the arithmetic has to be exact and reversible.
void MatroskaMuxer::writeVoid(const int totalBytes)
{
    if (totalBytes < VOID_MIN_BYTES)
        return;
    uint8_t buf[8];
    int len = ebml_write_id(buf, EBML_ID_VOID);
    len += ebml_write_size_fixed(buf + len, static_cast<uint64_t>(totalBytes) - VOID_MIN_BYTES, 2);
    writeToFile(buf, len);
    const std::vector<uint8_t> filler(static_cast<size_t>(totalBytes) - VOID_MIN_BYTES, 0);
    writeToFile(filler);
}

// ──────────────── EBML Header ────────────────────────────────────────────────

void MatroskaMuxer::writeEBMLHeader()
{
    // Build the EBML header content
    uint8_t buf[256];
    int pos = 0;
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLVERSION, 1);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLREADVERSION, 1);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLMAXIDLENGTH, 4);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLMAXSIZELENGTH, 8);
    pos += ebml_write_string(buf + pos, EBML_ID_DOCTYPE, m_webm ? "webm" : "matroska");
    pos += ebml_write_uint(buf + pos, EBML_ID_DOCTYPEVERSION, 4);
    pos += ebml_write_uint(buf + pos, EBML_ID_DOCTYPEREADVERSION, 2);

    // Write the EBML master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, EBML_ID_HEADER);
    hdrLen += ebml_write_size(header + hdrLen, pos);
    writeToFile(header, hdrLen);
    writeToFile(buf, pos);
}

// ──────────────── SegmentInfo ────────────────────────────────────────────────

void MatroskaMuxer::writeSegmentInfo()
{
    m_segmentInfoPos = m_file.pos() - m_segmentStartPos;

    std::vector<uint8_t> bufStore(512 + m_title.size());
    uint8_t* buf = bufStore.data();
    int pos = 0;
    pos += ebml_write_uint(buf + pos, MATROSKA_ID_TIMECODESCALE, 1000000);  // 1 ms

    // Duration placeholder (patched in close() with actual value)
    const int durationElementStart = pos;
    pos += ebml_write_float(buf + pos, MATROSKA_ID_DURATION, 0.0);

    // The title of the file: --title=, or a Matroska source's own, which a remux used to drop.
    if (!m_title.empty())
        pos += ebml_write_string(buf + pos, MATROSKA_ID_TITLE, m_title);
    // --keep-editions: the source's identity, which the ordered editions of this file and of the files linked to it
    // name it by.
    if (m_keepEditions)
        for (const auto& [id, uid] :
             {std::pair<uint32_t, const std::string*>{MATROSKA_ID_SEGMENTUID, &m_sourceEditions.segmentUid},
              {0x3CB923, &m_sourceEditions.prevUid},
              {0x3EB923, &m_sourceEditions.nextUid}})
            if (uid->size() == 16)
                pos += ebml_write_binary(buf + pos, id, reinterpret_cast<const uint8_t*>(uid->data()), 16);
    // MuxingApp names the library with its version (RFC 9559, 5.1.2.13); the version was missing.
    pos += ebml_write_string(buf + pos, MATROSKA_ID_MUXINGAPP, "tsMuxeR " TSMUXER_VERSION);
    // The version that wrote the file. Without it nobody could tell from a file which tsMuxeR
    // made it, which is the first thing a fault report needs
    pos += ebml_write_string(buf + pos, MATROSKA_ID_WRITINGAPP, "tsMuxeR " TSMUXER_VERSION);

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_INFO);
    hdrLen += ebml_write_size(header + hdrLen, pos + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();
    m_infoCrcFilePos = m_crcValuePos;
    m_infoBodyFilePos = m_file.pos();
    m_infoBody.assign(buf, buf + pos);

    // MATROSKA_ID_DURATION (0x4489) = 2-byte ID + 1-byte size → float64 at offset +3
    m_durationValueFilePos = m_file.pos() + durationElementStart + 3;

    writeToFile(buf, pos);
    endCrc();
}

// ──────────────── Tracks ─────────────────────────────────────────────────────

int MatroskaMuxer::writeColourInfo(uint8_t* dst, const MkvTrackInfo& track)
{
    const unsigned* const hdr10 = track.hdr10Metadata;
    const bool haveMastering = track.isHdr10 && (hdr10[3] != 0 || hdr10[4] != 0);
    // What the source container says, for what the stream does not. HDR metadata that a file keeps only in its
    // container, as many do, was dropped by a remux, so the copy played as plain SDR.
    const auto src = [&track](const char* key) { return track.srcColour.find(key); };
    const bool srcColourDesc =
        src("m") != track.srcColour.end() || src("t") != track.srcColour.end() || src("p") != track.srcColour.end();
    const bool srcMastering = src("lmax") != track.srcColour.end() || src("rx") != track.srcColour.end();
    const bool srcLight = src("cll") != track.srcColour.end() || src("fall") != track.srcColour.end();
    if (!track.hasColourDesc && !haveMastering && track.srcColour.empty())
        return 0;

    uint8_t body[256];
    int p = 0;
    if (track.hasColourDesc)
    {
        // The stream's own value wins where it gives one; where it says 2 (unspecified), the container's fills in
        const auto value = [&src, &track](const char* key, const uint8_t own)
        {
            const auto it = src(key);
            return own == 2 && it != track.srcColour.end() ? static_cast<uint64_t>(it->second) : own;
        };
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMATRIXCOEFF, value("m", track.colourMatrix));
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURTRANSFERCHARACTER, value("t", track.colourTransfer));
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURPRIMARIES, value("p", track.colourPrimaries));
    }
    else if (srcColourDesc)
    {
        for (const auto& [key, id] : {std::pair<const char*, uint32_t>{"m", MATROSKA_ID_COLOURMATRIXCOEFF},
                                      {"t", MATROSKA_ID_COLOURTRANSFERCHARACTER},
                                      {"p", MATROSKA_ID_COLOURPRIMARIES}})
            if (const auto it = src(key); it != track.srcColour.end())
                p += ebml_write_uint(body + p, id, static_cast<uint64_t>(it->second));
    }
    if (const auto it = src("r"); it != track.srcColour.end())
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURRANGE, static_cast<uint64_t>(it->second));
    if (!haveMastering && (srcLight || srcMastering))
    {
        for (const auto& [key, id] :
             {std::pair<const char*, uint32_t>{"cll", MATROSKA_ID_COLOURMAXCLL}, {"fall", MATROSKA_ID_COLOURMAXFALL}})
            if (const auto it = src(key); it != track.srcColour.end() && it->second > 0)
                p += ebml_write_uint(body + p, id, static_cast<uint64_t>(it->second));
        if (srcMastering)
        {
            uint8_t mm[192];
            int m = 0;
            for (const auto& [key, id] : {std::pair<const char*, uint32_t>{"rx", MATROSKA_ID_PRIMARYRCHROMATICITYX},
                                          {"ry", MATROSKA_ID_PRIMARYRCHROMATICITYY},
                                          {"gx", MATROSKA_ID_PRIMARYGCHROMATICITYX},
                                          {"gy", MATROSKA_ID_PRIMARYGCHROMATICITYY},
                                          {"bx", MATROSKA_ID_PRIMARYBCHROMATICITYX},
                                          {"by", MATROSKA_ID_PRIMARYBCHROMATICITYY},
                                          {"wx", MATROSKA_ID_WHITEPOINTCHROMATICITYX},
                                          {"wy", MATROSKA_ID_WHITEPOINTCHROMATICITYY},
                                          {"lmax", MATROSKA_ID_LUMINANCEMAX},
                                          {"lmin", MATROSKA_ID_LUMINANCEMIN}})
                if (const auto it = src(key); it != track.srcColour.end())
                    m += ebml_write_float(mm + m, id, it->second);
            p += ebml_write_master_open(body + p, MATROSKA_ID_MASTERINGMETADATA, m);
            memcpy(body + p, mm, m);
            p += m;
        }
    }

    if (haveMastering)
    {
        // MaxCLL and MaxFALL live beside MasteringMetadata, not inside it. Both are written only
        // when non-zero: a stream can legitimately signal zeros, and writing those back says
        // "measured as zero" rather than "unknown", so passing them on would be wrong.
        const unsigned maxCLL = hdr10[5] >> 16;
        const unsigned maxFALL = hdr10[5] & 0xffff;
        if (maxCLL)
            p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMAXCLL, maxCLL);
        if (maxFALL)
            p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMAXFALL, maxFALL);

        constexpr double CHROMA_UNIT = 50000.0;  // SEI sends 0.00002 units
        uint8_t mm[192];
        int m = 0;
        const auto x = [](const unsigned packed) { return (packed >> 16) / CHROMA_UNIT; };
        const auto y = [](const unsigned packed) { return (packed & 0xffff) / CHROMA_UNIT; };
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYGCHROMATICITYX, x(hdr10[0]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYGCHROMATICITYY, y(hdr10[0]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYBCHROMATICITYX, x(hdr10[1]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYBCHROMATICITYY, y(hdr10[1]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYRCHROMATICITYX, x(hdr10[2]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYRCHROMATICITYY, y(hdr10[2]));
        m += ebml_write_float(mm + m, MATROSKA_ID_WHITEPOINTCHROMATICITYX, x(hdr10[3]));
        m += ebml_write_float(mm + m, MATROSKA_ID_WHITEPOINTCHROMATICITYY, y(hdr10[3]));
        // [4] packs max in whole cd/m2 (already divided by 10000 on parse) and min still in 0.0001 cd/m2 units.
        m += ebml_write_float(mm + m, MATROSKA_ID_LUMINANCEMAX, static_cast<double>(hdr10[4] >> 16));
        m += ebml_write_float(mm + m, MATROSKA_ID_LUMINANCEMIN, (hdr10[4] & 0xffff) / 10000.0);

        p += ebml_write_master_open(body + p, MATROSKA_ID_MASTERINGMETADATA, m);
        memcpy(body + p, mm, m);
        p += m;
    }

    int pos = ebml_write_master_open(dst, MATROSKA_ID_VIDEOCOLOUR, p);
    memcpy(dst + pos, body, p);
    return pos + p;
}

std::vector<uint8_t> MatroskaMuxer::buildTrackEntry(const MkvTrackInfo& track, int* channelsValueAt)
{
    // Build inner content of the TrackEntry.
    // Size the buffer dynamically: fixed fields + colour and mastering metadata (~130 bytes on
    // its own, eight 64 bit floats among them) + codec private data + the enhancement layer's
    // configuration record, which is another one of comparable size on a dual layer track.
    // The name, the language and the codec ID are counted too: they come from the meta file, which sets no length, and
    // a track-name= of 2000 characters ran past the end of this buffer and corrupted the heap.
    const size_t bufSize = 1024 + track.codecPrivate.size() + track.dvElConfig.size() + 2 * track.mvcConfig.size() +
                           track.name.size() + track.language.size() + track.languageBCP47.size() +
                           track.matroskaCodecID.size() + [&track]
    {
        size_t n = 0;
        for (const auto& m : track.copyAddMappings) n += m.size() + 16;
        return n;
    }();
    std::vector<uint8_t> inner(bufSize);
    int pos = 0;

    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKNUMBER, track.trackNumber);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKUID, track.trackUID);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKTYPE, track.trackType);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKFLAGLACING, 0);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKFLAGDEFAULT, track.isDefault ? 1 : 0);
    // WebM has FlagForced but not the flags RFC 9559 added (hearing and visual impaired, text descriptions, original,
    // commentary): a .webm output carries only the elements of the WebM subset
    for (const auto& [flagId, value] : track.flags)
        if (!m_webm || flagId == MATROSKA_ID_TRACKFLAGFORCED)
            pos += ebml_write_uint(inner.data() + pos, flagId, value);
    if (!track.name.empty())
        pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_TRACKNAME, track.name);
    // Text subtitles in WebM are WebVTT, whose blocks hold the cue text as a Matroska text track's do.
    pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_CODECID,
                             m_webm && track.matroskaCodecID == MATROSKA_CODEC_ID_SRT ? MATROSKA_CODEC_ID_WEBM_WEBVTT
                                                                                      : track.matroskaCodecID);

    // Matroska treats a MISSING Language as "eng", so leaving it out does not mean "unknown", it
    // silently claims every untagged track is English. Write "und" instead when we have nothing.
    pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_TRACKLANGUAGE,
                             track.language.empty() ? std::string("und") : track.language);
    // The source's BCP 47 tag, so that pt-BR stays pt-BR and is not reduced to the three letters of Language. A reader
    // that knows the element takes it and ignores Language (RFC 9559, 12); one that does not still finds Language.
    // Not a WebM element: a .webm output has Language only.
    if (!track.languageBCP47.empty() && !m_webm)
        pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_TRACKLANGUAGEBCP47, track.languageBCP47);

    if (!track.codecPrivate.empty())
    {
        pos += ebml_write_binary(inner.data() + pos, MATROSKA_ID_CODECPRIVATE, track.codecPrivate.data(),
                                 static_cast<int>(track.codecPrivate.size()));
    }

    // Opus needs 80 ms of decoded audio before a seek point to converge, and its Matroska mapping
    // says so with SeekPreRoll. Without it a player that seeks starts decoding cold, at the seek
    // point itself.
    if (track.codecID == CODEC_A_OPUS)
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_SEEKPREROLL, 80000000);
    // The Matroska rules for Opus ask for CodecDelay, the pre-skip of the OpusHead in CodecPrivate (bytes 10 and 11,
    // little endian, always counted at 48 kHz). Without it a player takes the block times as they stand, and the
    // audio came out later by the pre-skip, 6.5 ms for a usual encoder, than the same blocks in the source.
    if (track.codecID == CODEC_A_OPUS && track.codecPrivate.size() >= 19 &&
        memcmp(track.codecPrivate.data(), "OpusHead", 8) == 0)
    {
        const unsigned preSkip = track.codecPrivate[10] | (track.codecPrivate[11] << 8);
        if (preSkip > 0)
            pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_CODECDELAY,
                                   static_cast<uint64_t>(preSkip) * 1000000000ULL / 48000);
    }
    // The encoder priming an MP4 source's edit list skips (srccodecdelay): a player drops it as CodecDelay asks,
    // where it came out as sound at the start of the track.
    else if (track.srcCodecDelayNs > 0)
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_CODECDELAY, track.srcCodecDelayNs);

    if (track.fps > 0)
    {
        // rounded, as the audio's is: cut off, 29.97 fps was written 33366666 ns where it is 33366666.7
        const auto durationNs = static_cast<uint64_t>(llround(1000000000.0 / track.fps));
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKDEFAULTDURATION, durationNs);
    }
    else if (track.trackType == 2 && track.audioFrameNs > 0)
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKDEFAULTDURATION, track.audioFrameNs);

    // Video sub-element
    if (track.trackType == 1 && track.width > 0 && track.height > 0)
    {
        // Big enough for the dimensions plus the whole Colour master: MasteringMetadata alone is
        // eight 64 bit floats. The old 128 was sized before any of that existed and overflowed
        // the moment colour metadata was written into it.
        uint8_t videoBuf[512];
        int vPos = 0;
        vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOPIXELWIDTH, track.width);
        vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOPIXELHEIGHT, track.height);
        // FlagInterlaced: 1 interlaced, 2 progressive. Progressive was not written, which a reader takes as
        // undetermined (RFC 9559, 5.1.4.1.28.1, where 0 SHOULD be avoided). Only the MPEG-2, H.264 and VC-1 readers
        // find out which it is; for the others nothing is written, as before.
        if (track.interlaced)
        {
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOFLAGINTERLACED, 1);
            // The source's field order, which only an interlaced track has.
            if (track.srcFieldOrder >= 0)
                vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOFIELDORDER,
                                        static_cast<uint64_t>(track.srcFieldOrder));
        }
        else if (track.codecID == CODEC_V_MPEG2 || track.codecID == CODEC_V_MPEG4_H264 || track.codecID == CODEC_V_VC1)
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOFLAGINTERLACED, 2);

        // DisplayWidth / DisplayHeight, written when the picture is not shown at its pixel size. The shape the stream
        // gives first, then the size the source container gave over it: a picture shown 4:3 by its container came out
        // shown at its pixel size. A container size equal to the picture over a stream that is square too adds
        // nothing and is not written, so such a remux keeps the bytes it had.
        unsigned displayW = track.width;
        unsigned displayH = track.height;
        if (!track.arForced && track.sampleAR > 0 && track.width && track.height)
        {
            // The pixel shape the stream gives. It was rounded to the nearest of the aspect ratios a Blu-ray disc can
            // say, so square pixels of 720x576 (5:4) and 720x480 (3:2) were shown at 4:3, and the pixel shape of HEVC
            // was not read at all: 1440x1080 at 4:3 pixels (16:9) was shown at 4:3. Square pixels are shown at their
            // size. A shape within 3 % of 4:3, 16:9 or 2.21:1 keeps that ratio: the pixel shapes of standard
            // definition (12:11, 16:11, 40:33 ...) make the 704 wide picture inside a 720 wide frame 4:3 or 16:9.
            unsigned displayWidth = track.width;
            if (std::fabs(track.sampleAR - 1.0) > 1e-6)
            {
                const double dar = track.width * track.sampleAR / track.height;
                displayWidth = static_cast<unsigned>(std::lround(track.width * track.sampleAR));
                for (const auto& [ratio, num, den] :
                     {std::tuple{4.0 / 3.0, 4u, 3u}, std::tuple{16.0 / 9.0, 16u, 9u}, std::tuple{2.21, 221u, 100u}})
                    if (std::fabs(dar / ratio - 1.0) <= 0.03)
                        displayWidth = (track.height * num + den / 2) / den;
            }
            displayW = displayWidth;
        }
        // The nearest of the disc's aspect ratios, for a stream that gives no pixel shape of its own: MPEG-2 and VC-1,
        // or ar= of the meta. H.264 that says nothing about its shape (no aspect_ratio_info) has square pixels, and
        // its nearest ratio is a guess: 720x576 was shown at 4:3.
        else if (track.streamAR != VideoAspectRatio::AR_KEEP_DEFAULT && track.streamAR != VideoAspectRatio::AR_VGA &&
                 (track.arForced || track.codecID != CODEC_V_MPEG4_H264))
        {
            unsigned displayWidth = track.width;
            unsigned displayHeight = track.height;
            switch (track.streamAR)
            {
            case VideoAspectRatio::AR_3_4:
                displayWidth = (track.height * 4 + 1) / 3;
                break;
            case VideoAspectRatio::AR_16_9:
                displayWidth = (track.height * 16 + 4) / 9;
                break;
            case VideoAspectRatio::AR_221_100:
                displayWidth = (track.height * 221 + 50) / 100;
                break;
            default:
                break;
            }
            displayW = displayWidth;
            displayH = displayHeight;
        }
        // Written when it is not the picture size, or when the container's size overrules a shape of the stream's own:
        // left out, a player would take the stream's. So is an ar= of the meta, which overrules the stream's shape
        // also where it comes out at the picture size (ar=4:3 on 1440x1080 that the stream makes 16:9).
        const bool streamShaped = displayW != track.width || displayH != track.height || track.arForced;
        if (track.srcDisplayWidth && track.srcDisplayHeight)
        {
            displayW = track.srcDisplayWidth;
            displayH = track.srcDisplayHeight;
        }
        if (streamShaped || displayW != track.width || displayH != track.height)
        {
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYWIDTH, displayW);
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYHEIGHT, displayH);
        }

        // Colour description, and HDR10 mastering metadata when the stream carries it.
        //
        // Every value here is ALREADY parsed for the Blu-ray path: the VUI colour fields at
        // hevc.cpp:400-402 and the SEI 137 / SEI 144 payloads in HDR10_metadata. Nothing new is
        // decoded. Writing them means a Blu-ray remuxed to MKV is correctly tagged without the
        // user having to supply the values by hand, which is not something the usual reference
        // muxer does: it propagates a Colour element that a source container already has, or
        // takes explicit options, but it never derives one from the bitstream.
        //
        // ORDER TRAP: SEI 137 sends the display primaries as GREEN, BLUE, RED, not RGB. The
        // comments at hevc.cpp:769-771 get two of the three wrong. HDR10_metadata[0] is green,
        // [1] is blue, [2] is red, [3] is the white point, each packed as (x << 16) | y in units
        // of 0.00002. Matroska wants floats in the 0..1 range, hence the 50000 divisor.
        vPos += writeColourInfo(videoBuf + vPos, track);

        // The source's crop and projection (rotation among it), as the source gave them.
        static constexpr uint32_t CROP_IDS[4] = {MATROSKA_ID_VIDEOPIXELCROPTOP, MATROSKA_ID_VIDEOPIXELCROPBOTTOM,
                                                 MATROSKA_ID_VIDEOPIXELCROPLEFT, MATROSKA_ID_VIDEOPIXELCROPRIGHT};
        for (int i = 0; i < 4; ++i)
            if (track.srcCrop[i] > 0)
                vPos += ebml_write_uint(videoBuf + vPos, CROP_IDS[i], track.srcCrop[i]);
        if (!track.srcProjection.empty() && track.srcProjection.size() <= 256 &&
            vPos + static_cast<int>(track.srcProjection.size()) + 8 <= static_cast<int>(sizeof(videoBuf)))
            vPos += ebml_write_binary(videoBuf + vPos, MATROSKA_ID_VIDEOPROJECTION, track.srcProjection.data(),
                                      static_cast<int>(track.srcProjection.size()));

        // 3D: both views travel in one block, so the track has to say so or a player has no
        // reason to look for a second one. 13 is "both eyes laced in one block, left eye first",
        // which is how a reference 3D file describes exactly this arrangement; 14 the same with the right eye first,
        // where the base view is the right eye. 13 was written for both, and a player showed each eye the other's view.
        if (track.foldKind == MkvTrackInfo::FoldKind::MvcDependent)
            vPos += ebml_write_uint(
                videoBuf + vPos, MATROSKA_ID_VIDEOSTEREOMODE_CURRENT,
                m_rightEyeFirst ? MATROSKA_STEREO_BOTH_EYES_RIGHT_FIRST : MATROSKA_STEREO_BOTH_EYES_LEFT_FIRST);
        // The source's StereoMode for a picture that holds both views itself (1 to 12: side by side, top and bottom,
        // checkerboard, interleaved rows or columns, anaglyph), which was dropped. 13 and 14 say that a block holds
        // two views, which only the folded 3D track above does.
        else if (track.srcStereoMode >= 1 && track.srcStereoMode < MATROSKA_STEREO_BOTH_EYES_LEFT_FIRST)
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOSTEREOMODE_CURRENT, track.srcStereoMode);

        // Write Video master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKVIDEO, vPos);
        memcpy(inner.data() + pos, videoBuf, vPos);
        pos += vPos;

        // Dolby Vision configuration, as a BlockAdditionMapping beside the Video master. Without
        // it a player has no way to know the track is Dolby Vision, even when the RPU is present
        // in the stream, so the track simply plays as its HDR10 base layer.
        //
        // These mappings, and the hvcE and mvcC ones below, carry a configuration record and nothing else: no block of
        // the track has BlockAdditions. So they have no BlockAddIDValue, and MaxBlockAdditionID keeps its default, 0,
        // which says so. The value was written as 1, which RFC 9559 allows only for a mapping of type 0 (5.1.4.1.17.1
        // and .3), and MaxBlockAdditionID as 1 although no block had one.
        if (track.dvBlockAddIdType)
        {
            uint8_t body[64];
            int b = 0;
            b += ebml_write_uint(body + b, MATROSKA_ID_BLOCKADDIDTYPE, track.dvBlockAddIdType);
            b += ebml_write_binary(body + b, MATROSKA_ID_BLOCKADDIDEXTRADATA, track.dvConfig,
                                   static_cast<int>(sizeof(track.dvConfig)));
            pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_BLOCKADDITIONMAPPING, b);
            memcpy(inner.data() + pos, body, b);
            pos += b;

            // Dual layer: a second mapping, "hvcE", carrying the enhancement layer's own HEVC
            // configuration record. The Dolby Vision record above says an enhancement layer is
            // present; this one says how to decode it.
            if (!track.dvElConfig.empty())
            {
                std::vector<uint8_t> body2(64 + track.dvElConfig.size());
                int b2 = 0;
                b2 += ebml_write_uint(body2.data() + b2, MATROSKA_ID_BLOCKADDIDTYPE, 0x68766345 /* hvcE */);
                b2 += ebml_write_binary(body2.data() + b2, MATROSKA_ID_BLOCKADDIDEXTRADATA, track.dvElConfig.data(),
                                        static_cast<int>(track.dvElConfig.size()));
                pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_BLOCKADDITIONMAPPING, b2);
                memcpy(inner.data() + pos, body2.data(), b2);
                pos += b2;
            }
        }

        // 3D: the MVC configuration record, saying how the second view in every block decodes.
        if (!track.mvcConfig.empty())
        {
            std::vector<uint8_t> body(64 + track.mvcConfig.size());
            int b = 0;
            b += ebml_write_uint(body.data() + b, MATROSKA_ID_BLOCKADDIDTYPE, 0x6D766343 /* mvcC */);
            b += ebml_write_binary(body.data() + b, MATROSKA_ID_BLOCKADDIDEXTRADATA, track.mvcConfig.data(),
                                   static_cast<int>(track.mvcConfig.size()));
            pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_BLOCKADDITIONMAPPING, b);
            memcpy(inner.data() + pos, body.data(), b);
            pos += b;
        }
    }

    // A track copied as it is: the source's BlockAdditionMappings and MaxBlockAdditionID as they stand, so that the
    // BlockAdditions its blocks carry still say what they are (RFC 9559, 5.1.4.1.17)
    if (track.copyMaxBlockAddId > 0)
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_MAXBLOCKADDITIONID, track.copyMaxBlockAddId);
    for (const auto& mapping : track.copyAddMappings)
    {
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_BLOCKADDITIONMAPPING,
                                      static_cast<int>(mapping.size()));
        memcpy(inner.data() + pos, mapping.data(), mapping.size());
        pos += static_cast<int>(mapping.size());
    }

    // Audio sub-element
    if (track.trackType == 2 && track.sampleRate > 0)
    {
        uint8_t audioBuf[128];
        int aPos = 0;
        aPos += ebml_write_float(audioBuf + aPos, MATROSKA_ID_AUDIOSAMPLINGFREQ, static_cast<double>(track.sampleRate));
        const int channelsAt = aPos + 2;  // the one byte ID 0x9F and a one byte size come first
        aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOCHANNELS, track.channels);
        if (track.bitDepth > 0)
            aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOBITDEPTH, track.bitDepth);

        // Write Audio master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKAUDIO, aPos);
        if (channelsValueAt && track.channels > 0 && track.channels < 256)
            *channelsValueAt = pos + channelsAt;
        memcpy(inner.data() + pos, audioBuf, aPos);
        pos += aPos;
    }

    inner.resize(pos);
    return inner;
}

void MatroskaMuxer::writeTracks()
{
    m_tracksPos = m_file.pos() - m_segmentStartPos;

    // Build all track entries, in TRACK NUMBER order rather than in map order. The map is keyed by
    // stream index because that is what packets are routed by, and the AC-3 core companion of a
    // TrueHD track is keyed above every real stream to stay clear of them, which would otherwise put
    // it at the end of the file behind the subtitles. It belongs directly after the track it was
    // taken from, which is where a disc remux carries it.
    std::vector<const MkvTrackInfo*> ordered;
    ordered.reserve(m_tracks.size());
    for (const auto& [streamIdx, track] : m_tracks)
    {
        // A Dolby Vision enhancement layer folded into its base track is not a track of its own.
        if (track.foldedIntoStream >= 0)
            continue;
        ordered.push_back(&track);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const MkvTrackInfo* a, const MkvTrackInfo* b) { return a->trackNumber < b->trackNumber; });

    std::vector<uint8_t> allEntries;
    std::map<int, int64_t> channelsAtInEntries;
    for (const auto* trackPtr : ordered)
    {
        int channelsAt = -1;
        std::vector<uint8_t> entryContent = buildTrackEntry(*trackPtr, &channelsAt);

        // Write TrackEntry master header + content
        uint8_t header[16];
        int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKENTRY);
        hdrLen += ebml_write_size(header + hdrLen, entryContent.size());
        allEntries.insert(allEntries.end(), header, header + hdrLen);
        if (channelsAt >= 0)
            channelsAtInEntries[trackPtr->streamIndex] = static_cast<int64_t>(allEntries.size()) + channelsAt;
        allEntries.insert(allEntries.end(), entryContent.begin(), entryContent.end());
    }

    // Write Tracks master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKS);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size() + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();
    m_tracksCrcFilePos = m_crcValuePos;
    const int64_t entriesFilePos = m_file.pos();
    m_tracksBodyFilePos = entriesFilePos;
    m_tracksBody = allEntries;
    for (const auto& [streamIdx, at] : channelsAtInEntries) m_channelsValueFilePos[streamIdx] = entriesFilePos + at;
    writeToFile(allEntries);
    endCrc();
}

// ──────────────── openDstFile ────────────────────────────────────────────────

// Could a fold happen at all? A NECESSARY condition, not the full test, and the distinction is the
// whole point of this function.
//
// The real pairing cannot be run here. It needs dvBlockAddIdType, which is built from the codec
// reader in refreshTrackProperties, and that runs on the first packet precisely because a reader
// has not parsed anything yet at this stage. Asking the exact question early gets the answer "no"
// for every mux, including the ones that work.
//
// What IS knowable here is the track list, and folding needs two HEVC video tracks. With fewer
// than two there is nothing a later reader could discover that would make a fold possible, so
// refusing is safe. With two or more this returns true and the exact check in
// refreshTrackProperties still decides.
// 3D: a disc stores the two views as two elementary streams, and Matroska carries both in ONE
// track. Without this a 3D disc came out as two video tracks, the second one labelled with a codec
// id general software does not know, so the file round tripped back to a disc correctly and played
// as flat 2D everywhere else.
//
// The pairing is by CODEC, not by order or size: a dependent view is the only thing that decodes to
// CODEC_V_MPEG4_H264_DEP, so there is nothing to guess. The base view is the video track in front
// of it, which is the order a disc lists them and the order the demuxer hands them over.
//
// A Matroska 3D source lists its two views the other way round, the dependent view first (subTrack=1) and the base
// view after it, and a meta written from the listing has them in that order. Its views were not paired at all, and
// such a remux came out as two video tracks that play as flat 2D. A dependent view with no base view in front of it
// is paired with the H.264 track that follows it.
void MatroskaMuxer::pairMvcViews()
{
    std::vector<MkvTrackInfo*> video;
    for (auto& [streamIdx, track] : m_tracks)
        if (track.trackType == 1)
            video.push_back(&track);
    const auto freeBase = [](const MkvTrackInfo* t)
    { return t->codecID == CODEC_V_MPEG4_H264 && t->foldElStreamIndex < 0 && t->foldedIntoStream < 0; };
    for (size_t i = 0; i < video.size(); ++i)
    {
        MkvTrackInfo& track = *video[i];
        if (track.codecID != CODEC_V_MPEG4_H264_DEP || track.foldedIntoStream >= 0)
            continue;
        // The nearest ordinary video track in front of it, as a disc lists them. A dependent view belongs to the one in
        // front of it, not simply to the first track in the file, so a mux carrying two 3D pairs pairs each of them
        // with its own base view.
        MkvTrackInfo* baseTrack = nullptr;
        for (size_t j = i; j-- > 0;)
            if (video[j]->codecID != CODEC_V_MPEG4_H264_DEP)
            {
                baseTrack = freeBase(video[j]) ? video[j] : nullptr;
                break;
            }
        // Otherwise the one right after it, as a Matroska source lists them.
        if (baseTrack == nullptr && i + 1 < video.size() && freeBase(video[i + 1]))
            baseTrack = video[i + 1];
        if (baseTrack == nullptr)
            continue;
        baseTrack->foldKind = MkvTrackInfo::FoldKind::MvcDependent;
        baseTrack->foldElStreamIndex = track.streamIndex;
        track.foldedIntoStream = baseTrack->streamIndex;
        track.foldKind = MkvTrackInfo::FoldKind::MvcDependent;
    }
}

bool MatroskaMuxer::couldFoldDualLayer() const
{
    int hevcVideoTracks = 0;
    for (const auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType != 1)
            continue;
        if (dynamic_cast<HEVCStreamReader*>(track.codecReader) != nullptr)
            ++hevcVideoTracks;
    }
    return hevcVideoTracks >= 2;
}

void MatroskaMuxer::openDstFile()
{
    m_fileName = m_origFileName;
    if (isSplitting())
    {
        m_partNum = 1;
        m_fileName = partFileName(1);
    }

    // Refuse BEFORE the destination is touched. This check used to live in refreshTrackProperties,
    // which runs on the first packet, by which time the file has been created and truncated: a mux
    // that refused to run replaced whatever was at that path with a 308 byte stub. Measured on a
    // 96,002 byte file. The muxer knows its tracks by now, since they are added before this is
    // called, so nothing is lost by asking here.
    if (m_dvWriteProfile81 && !couldFoldDualLayer())
        THROW(ERR_COMMON, DV_PROFILE81_NEEDS_DUAL_LAYER)

    // A .webm output is WebM, the subset of Matroska a web browser plays: VP8, VP9 and AV1 video, Opus and Vorbis
    // audio, and of the elements only those the WebM container guidelines list.
    m_webm = strToLowerCase(extractFileExt(m_origFileName)) == "webm";
    if (m_keepEditions && (m_webm || isSplitting()))
        THROW(ERR_COMMON, "--keep-editions keeps one Matroska file as it is stored: it cannot be used with "
                              << (m_webm ? "a WebM output, which has no editions or segment links." : "a split."))
    if (m_webm)
        for (const auto& [index, track] : m_tracks)
            if (track.codecID != CODEC_V_VP8 && track.codecID != CODEC_V_VP9 && track.codecID != CODEC_V_AV1 &&
                track.codecID != CODEC_A_OPUS && track.codecID != CODEC_A_VORBIS && track.codecID != CODEC_S_SRT &&
                !(track.codecID == CODEC_S_MKVCOPY && isWebmVttText(track.matroskaCodecID)))
                THROW(ERR_COMMON,
                      (track.codecReader ? track.codecReader->getCodecInfo().displayName : std::string("A track"))
                          << " cannot go into a WebM file, which holds VP8, VP9 and AV1 video, Opus and "
                             "Vorbis audio and text subtitles only. Give the output an .mkv name for it, or "
                             "leave the track out.")

    // ** THE DESTINATION IS NOT CREATED HERE, AND THAT IS THE WHOLE POINT. **
    //
    // Opening a file for writing truncates it, so creating it here meant that every refusal raised
    // afterwards destroyed whatever was already at that path. The gate above closes the route it
    // can close, but it only COUNTS video tracks: the exact Dolby Vision test needs a value built
    // from the codec reader and cannot run until a frame has been parsed. Measured with a 96,002
    // byte file at the output path: two HEVC tracks that are not a Dolby Vision pair, and a real
    // dual layer pair listed enhancement layer first, both left a 308 byte stub behind.
    //
    // So the file is created in openDestination instead, which writeDeferredHeader calls after
    // refreshTrackProperties has had its chance to throw. Every one of this class's uses of m_file
    // is after the header, so there is exactly one place to open it rather than one per write.
    //
    // The path is still checked NOW, so a bad destination is reported at the same moment as before
    // rather than after parsing has started. ofNoTruncate opens without emptying the file, so the
    // check costs the user nothing. If the check is what brought the file into being, it is removed
    // again: a refusal should not leave an empty file where there was none.
    const bool existedBeforeProbe = fileExists(m_fileName);
    if (!m_file.open(m_fileName.c_str(), File::ofWrite + File::ofNoTruncate))
        THROW(ERR_CANT_CREATE_FILE, "Can't create output file " << m_fileName)
    m_file.close();
    if (!existedBeforeProbe)
        deleteFile(m_fileName);

    m_headerWritten = false;
}

void MatroskaMuxer::openDestination()
{
    if (m_destinationOpen)
        return;

    if (!m_file.open(m_fileName.c_str(), File::ofWrite))
        THROW(ERR_CANT_CREATE_FILE, "Can't create output file " << m_fileName)
    m_destinationOpen = true;

    // 1. Write EBML Header
    writeEBMLHeader();

    // 2. Write Segment header with unknown size (patched at close time)
    uint8_t segBuf[16];
    int pos = ebml_write_id(segBuf, MATROSKA_ID_SEGMENT);
    writeToFile(segBuf, pos);
    m_segmentSizePos = m_file.pos();
    pos = ebml_write_unknown_size(segBuf, 8);
    writeToFile(segBuf, pos);
    m_segmentStartPos = m_file.pos();

    // 3. Reserve room at the FRONT of the segment for a SeekHead.
    //
    // Cues, and now the attachments, are written after the clusters because their contents are not
    // known until the mux has finished. A reader that only looks at the head of the file therefore
    // never finds them, and one that copies a file from what it found silently leaves them behind.
    // Measured: a file whose attachments sat only after the clusters came back from a third party
    // remux with the video intact and the attachments gone, which for the Dolby Vision carrier
    // means the originals needed to rebuild the disc are lost without a word.
    //
    // A SeekHead at the front is what the format expects and what fixes it. The size is not known
    // yet, so a Void of fixed size holds the space and close() writes the real thing over it,
    // padding whatever is left with a smaller Void.
    m_seekHeadReservePos = m_file.pos();
    writeVoid(SEEKHEAD_RESERVE);
}

void MatroskaMuxer::refreshTrackProperties()
{
    // A track whose source stored BlockAdditions beside its frames keeps them, and so needs its MaxBlockAdditionID and
    // the mappings that say what they are. The configuration-only mappings tsMuxeR writes itself, for Dolby Vision and
    // 3D, are not taken over, or they would be there twice.
    for (auto& [index, track] : m_tracks)
    {
        const auto src = m_srcBlockAdd.find(index);
        if (src == m_srcBlockAdd.end() || track.codecID == CODEC_S_MKVCOPY || track.frameAdditions)
            continue;
        track.frameAdditions = true;
        track.copyMaxBlockAddId = src->second.first;
        for (const auto& mapping : src->second.second)
        {
            uint64_t type = 0;
            for (size_t i = 0; i + 2 < mapping.size();)
            {
                const uint32_t id = static_cast<uint32_t>(mapping[i]) << 8 | mapping[i + 1];
                const size_t len = mapping[i + 2] & 0x7F;  // these small elements have a one byte size
                if (!(mapping[i + 2] & 0x80) || i + 3 + len > mapping.size())
                    break;
                if (id == MATROSKA_ID_BLOCKADDIDTYPE)
                    for (size_t k = 0; k < len; ++k) type = type << 8 | mapping[i + 3 + k];
                i += 3 + len;
            }
            if (type != 0x64766343 && type != 0x64767643 && type != 0x68766345 && type != 0x6D766343)
                track.copyAddMappings.push_back(mapping);  // not dvcC, dvvC, hvcE or mvcC
        }
    }
    // Resolve exactly one default track per type. A MISSING FlagDefault means 1 in Matroska, so
    // writing nothing made every track claim to be the default, including two audio tracks at
    // once. Honour an explicit "default" where the meta gave one, otherwise the first track of
    // that type, and write the flag on every track so nothing is left to the spec default.
    // A type counts as settled if ANY of its tracks carried the key, whatever the key said. That
    // is what makes "no default" expressible: default=none on every track of a type leaves the type
    // settled with nothing wanted, where before the key had to say yes to count at all.
    std::set<uint8_t> typeHasExplicit;
    for (auto& [streamIdx, track] : m_tracks)
        if (track.defaultExplicit)
            typeHasExplicit.insert(track.trackType);
    std::set<uint8_t> typeDone;
    for (auto& [streamIdx, track] : m_tracks)
    {
        const bool wantIt =
            typeHasExplicit.count(track.trackType) ? track.markedDefault : !typeDone.count(track.trackType);
        track.isDefault = wantIt && !typeDone.count(track.trackType);
        if (track.isDefault)
            typeDone.insert(track.trackType);
    }

    for (auto& [streamIdx, track] : m_tracks)
    {
        // The codec ID has to be re-derived here for the same reason the sizes below do. At
        // intAddStream time an AC-3 family reader has not seen a frame yet, so isEAC3() is still
        // false and getCodecInfo() hands back the plain AC-3 entry: an E-AC-3 track was being
        // declared as A_AC3 and a TrueHD track would be too. The payload was always correct, only
        // the CodecID was wrong, so a player that trusts it would decode the wrong thing.
        const CodecInfo& info = track.codecReader->getCodecInfo();
        if (info.codecID != track.codecID)
        {
            track.codecID = info.codecID;
            track.matroskaCodecID = codecNameToMatroskaID(info.programName, info.codecID);
        }
        // An AC-3 core with an E-AC-3 part beside it is E-AC-3 to a decoder, and is declared so. It
        // went out as A_AC3, named after whichever frame happened to be read last.
        if (const auto ac3 = dynamic_cast<AC3StreamReader*>(track.codecReader);
            ac3 && track.ac3CoreOfStream < 0 && track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_AC3 &&
            ac3->carriesEac3())
            track.matroskaCodecID = MATROSKA_CODEC_ID_AUDIO_EAC3;
        // Three formats were labelled as their bigger relatives, because one reader serves both: MPEG-1 video as
        // V_MPEG2, MLP that is not TrueHD as A_TRUEHD, and DTS Express as plain DTS. Matroska names each on its own,
        // and a player that trusts the label decodes the wrong thing. What a stream is, is known once its header is
        // read.
        if (const auto m2v = dynamic_cast<MPEG2StreamReader*>(track.codecReader);
            m2v && track.matroskaCodecID == MATROSKA_CODEC_ID_VIDEO_MPEG2 && m2v->isMpeg1())
            track.matroskaCodecID = MATROSKA_CODEC_ID_VIDEO_MPEG1;
        if (const auto mlp = dynamic_cast<MLPStreamReader*>(track.codecReader);
            mlp && track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_TRUEHD && mlp->m_subType == MlpSubType::stMLP)
            track.matroskaCodecID = MATROSKA_CODEC_ID_AUDIO_MLP;
        if (const auto dts = dynamic_cast<DTSStreamReader*>(track.codecReader);
            dts && track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_DTS &&
            dts->getDTSHDMode() == DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS)
            track.matroskaCodecID = MATROSKA_CODEC_ID_AUDIO_DTS_EXPRESS;
        // MPEG audio is one reader for three layers, and Matroska names each: an MP2 track was
        // declared A_MPEG/L3. The layer is known once a frame has been read.
        if (const auto mpa = dynamic_cast<MpegAudioStreamReader*>(track.codecReader); mpa && mpa->getLayer() >= 1)
            track.matroskaCodecID = mpa->getLayer() == 1   ? "A_MPEG/L1"
                                    : mpa->getLayer() == 2 ? "A_MPEG/L2"
                                                           : MATROSKA_CODEC_ID_AUDIO_MPEG_L3;

        if (track.trackType == 1)  // video
        {
            // MPEG-4 Part 2 and ProRes copied from an MP4 file: the size the MP4 gave, read with the first record
            if (const auto copy = dynamic_cast<const VideoCopyStreamReader*>(track.codecReader))
            {
                track.width = copy->width();
                track.height = copy->height();
                track.fps = copy->fps();
            }
            const auto mpegReader = dynamic_cast<MPEGStreamReader*>(track.codecReader);
            if (mpegReader)
            {
                track.width = mpegReader->getStreamWidth();
                track.height = mpegReader->getStreamHeight();
                track.fps = mpegReader->getFPS();
                track.interlaced = mpegReader->getInterlaced();
                track.streamAR = mpegReader->getStreamAR();
                track.sampleAR = mpegReader->getSampleAspectRatio();
                track.arForced = mpegReader->aspectRatioForced();
                track.hasColourDesc =
                    mpegReader->getColourDesc(track.colourPrimaries, track.colourTransfer, track.colourMatrix);
                // getStreamHDR: 1 SDR, 2 HDR10, 4 Dolby Vision, 16 HDR10+. Everything except
                // plain SDR carries an HDR10 base layer, so mastering metadata applies.
                //
                // Only HEVC answers it: every other reader says 0, which was taken for HDR, and the mastering values
                // are the track's own. They were read from one global set, so plain H.264 beside an HDR10 HEVC track
                // was given the HEVC track's mastering display and light levels, and two HDR10 tracks shared one set.
                track.isHdr10 = false;
                if (const auto hevc = dynamic_cast<HEVCStreamReader*>(track.codecReader))
                {
                    track.isHdr10 = hevc->getStreamHDR() != 1;
                    std::copy_n(hevc->getHdr10Metadata(), 6, track.hdr10Metadata);
                }
                // Dolby Vision configuration, when the stream carries an RPU. Written whatever
                // the profile, because a single layer profile 8 track is complete on its own; a
                // profile 7 base layer without its enhancement layer is handled below.
                if (const auto hevcReader = dynamic_cast<HEVCStreamReader*>(track.codecReader))
                    track.dvBlockAddIdType = hevcReader->buildDoViConfigRecord(track.dvConfig);
            }
        }
        else if (track.trackType == 2)  // audio
        {
            const auto simpleReader = dynamic_cast<SimplePacketizerReader*>(track.codecReader);
            if (simpleReader)
            {
                track.sampleRate = simpleReader->getFreq();
                track.channels = simpleReader->getChannels();
            }
            // A DD+ 7.1 track carries channels 7 and 8 in a dependent substream, and the reader counts only the 5.1
            // of the independent one, so the track header said 6 while the audio decodes as 7.1. The same two
            // channels are added wherever the listing and the transport stream descriptor name the track 7.1.
            if (const auto ac3 = dynamic_cast<AC3StreamReader*>(track.codecReader);
                ac3 && track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_EAC3 && ac3->hasExtChannels())
                track.channels = ac3->getChannels() + 2;
            const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(track.codecReader);
            if (lpcmReader)
                track.bitDepth = lpcmReader->m_bitsPerSample == 20 ? 24 : lpcmReader->m_bitsPerSample;
            // BitDepth for the lossless codecs whose readers know it: FLAC from its STREAMINFO, DTS-HD from its
            // extension. It was written for LPCM alone.
            // An MP3's LAME delay and padding (muxPacket), in samples, with the 529 samples an MP3 decoder itself puts
            // in front: what a player skips at the start, and what it drops of the last frame.
            if (track.mp3Delay >= 0 && track.sampleRate > 0)
            {
                track.srcCodecDelayNs = static_cast<uint64_t>(track.mp3Delay + 529) * 1000000000ULL /
                                        static_cast<uint64_t>(track.sampleRate);
                if (track.mp3Padding > 529)
                    track.endPaddingNs = static_cast<int64_t>(track.mp3Padding - 529) * 1000000000LL / track.sampleRate;
            }
            // Vorbis: the first packet gives no sound, its length is the CodecDelay a player skips, so that the sound
            // starts at the time of the second packet, as a Matroska source has it.
            if (const auto vorbis = dynamic_cast<VorbisStreamReader*>(track.codecReader);
                vorbis && vorbis->leadSamples() > 0 && track.sampleRate > 0)
                track.srcCodecDelayNs = static_cast<uint64_t>(vorbis->leadSamples()) * 1000000000ULL /
                                        static_cast<uint64_t>(track.sampleRate);
            if (const auto flac = dynamic_cast<FLACStreamReader*>(track.codecReader);
                flac && flac->getBitsPerSample() > 0)
                track.bitDepth = flac->getBitsPerSample();
            if (const auto dts = dynamic_cast<DTSStreamReader*>(track.codecReader); dts && dts->getHDBitDepth() > 0)
                track.bitDepth = dts->getHDBitDepth();
            if (const auto alac = dynamic_cast<AlacStreamReader*>(track.codecReader); alac && alac->getBitDepth() > 0)
                track.bitDepth = alac->getBitDepth();
            // DefaultDuration for audio whose frames are all of one length: the step the reader moves each frame's
            // time by, so it is the spacing of the blocks written. Not for E-AC-3, DTS-HD, TrueHD, FLAC, Opus, Vorbis
            // or LPCM, whose frames may differ in length. ALAC's do only in the last, which says so (writeBlock).
            const std::string kind = track.codecReader->getCodecInfo().displayName;
            if (simpleReader &&
                (track.codecID == CODEC_A_AAC || track.codecID == CODEC_A_MPEG_AUDIO || track.codecID == CODEC_A_ALAC ||
                 (track.codecID == CODEC_A_AC3 && kind == "AC3") || (track.codecID == CODEC_A_DTS && kind == "DTS")))
            {
                const double frame = simpleReader->frameDurationInternal();
                if (frame > 0)
                    track.audioFrameNs = static_cast<uint64_t>(llround(frame * 1e9 / INTERNAL_PTS_FREQ));
            }
        }
    }

    // A Dolby Vision configuration record is a promise to the player that THIS track is playable
    // Dolby Vision. That is true of a single layer track, which carries its base layer and its RPU
    // together. It is NOT true of the enhancement layer of a dual layer disc: the RPU rides on the
    // enhancement layer, so it is that track which looks like Dolby Vision here, while the picture
    // lives on the base layer. Writing the record there produced a quarter resolution track
    // announcing itself as complete single layer Dolby Vision, which it is not, is not the base
    // layer, and cannot be played on its own.
    //
    // Matroska carries a dual layer title as ONE track with both layers interleaved, so that is
    // what is built here: the enhancement layer is folded into the base track, gets no track entry
    // of its own, and the merged track is described as profile 7 with base layer, enhancement layer
    // and RPU all present.
    //
    // The pairing is keyed on "a video track after the first one that carries an RPU", NOT on "has
    // an RPU", because an RPU means CARRIES DOLBY VISION DATA and not IS AN ENHANCEMENT LAYER: a
    // single layer profile 8 track has one too and must be left alone. A Dolby Vision base layer
    // sitting beside a picture in picture stream also stays untouched, because a picture in picture
    // stream carries no RPU.
    MkvTrackInfo* baseTrack = nullptr;
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType != 1)
            continue;
        if (baseTrack == nullptr)
        {
            baseTrack = &track;
            continue;
        }
        if (!track.dvBlockAddIdType || baseTrack->foldElStreamIndex >= 0)
            continue;

        const auto blReader = dynamic_cast<HEVCStreamReader*>(baseTrack->codecReader);
        const auto elReader = dynamic_cast<HEVCStreamReader*>(track.codecReader);
        if (blReader == nullptr || elReader == nullptr)
            continue;
        // An enhancement layer is never larger than its base: a 4K profile 8.1 track listed after a 1080p one was
        // folded into it, and the output showed only the 1080p picture. Such a track stays a track of its own.
        const unsigned elWidth = dynamic_cast<MPEGStreamReader*>(elReader)->getStreamWidth();
        const unsigned blWidth = dynamic_cast<MPEGStreamReader*>(blReader)->getStreamWidth();
        if (elWidth > blWidth)
            continue;
        // Nor is every track with an RPU an enhancement layer: a single layer profile 8.1 track carries one too. Its
        // RPU says which it is (HevcDvRpuUnit), and a track whose RPU says single layer is a picture of its own. A
        // plain 4K track followed by a 4K profile 8.1 one was folded: 36 of its 60 pictures were left out, and the one
        // track left was described as profile 6. Where no RPU could be read, only a smaller track is taken for a layer,
        // as the enhancement layer of a disc is a quarter of its base.
        if (elReader->dvRpuElPresent() == 0 || (elReader->dvRpuElPresent() < 0 && elWidth >= blWidth))
            continue;

        // In profile 8.1 mode the pairing still happens, because the enhancement layer is still
        // folded into the one track, but the track is DESCRIBED as single layer 8.1: that is what
        // the converted RPU makes it, and it is why players that refuse profile 7 will take it.
        uint8_t merged[24];
        const uint32_t mergedType = m_dvWriteProfile81 ? blReader->buildDoViConfigRecordProfile81(merged)
                                                       : blReader->buildDoViConfigRecordDualLayer(merged, *elReader);
        if (mergedType == 0)
            continue;

        // In profile 8.1 mode, also work out the record this track WOULD have declared as a dual
        // layer profile 7 one, and keep it for the manifest. It is what a rebuild has to restore,
        // and it cannot be derived later because the enhancement layer reader is gone by then.
        if (m_dvWriteProfile81)
            m_dvProfile7ConfigType = blReader->buildDoViConfigRecordDualLayer(m_dvProfile7Config, *elReader);

        baseTrack->foldElStreamIndex = track.streamIndex;
        baseTrack->dvBlockAddIdType = mergedType;
        memcpy(baseTrack->dvConfig, merged, sizeof(merged));
        track.foldedIntoStream = baseTrack->streamIndex;
        track.dvBlockAddIdType = 0;

        if (m_dvWriteProfile81)
            LTRACE(LT_INFO, 2,
                   "Dolby Vision: writing profile 8.1. The RPU is converted, so the track declares "
                   "single layer profile "
                       << ((merged[2] >> 1) & 0x7F) << ", level " << (((merged[2] & 1) << 5) | (merged[3] >> 3))
                       << ", HDR10 compatible, which plays on devices that refuse profile 7. The disc's "
                          "enhancement layer still travels in the track, skipped by decoders, so the file "
                          "is NOT smaller than a profile 7 one.");
        else
            LTRACE(LT_INFO, 2,
                   "Dolby Vision: folding the enhancement layer into the base video track, as Matroska "
                   "requires. The result is one track, profile "
                       << ((merged[2] >> 1) & 0x7F) << ", level " << (((merged[2] & 1) << 5) | (merged[3] >> 3))
                       << ", with base layer, enhancement layer and RPU.");
    }

    // A dual layer pair listed the WRONG WAY ROUND never pairs, and used to say nothing true about
    // it. The base layer is whichever video track comes first, and the enhancement layer is the one
    // that carries the RPU, so with the enhancement layer listed first the loop above looks at the
    // base layer, finds no RPU on it, and gives up.
    //
    // Without --dv-profile that came out as two separate tracks with the enhancement layer wearing
    // a Dolby Vision record of its own, at "Mux successful complete" and with nothing said: a dual
    // layer disc in, a file with no Dolby Vision out. With --dv-profile=8.1 it was refused, and the
    // refusal said the mux has no dual layer track and that both streams must be listed, when both
    // WERE listed.
    //
    // The shape is recognised without changing which track is chosen, so nothing that pairs today
    // pairs differently: the first video track carries an RPU, a later one carries none, and that
    // later one is larger in both directions. A picture in picture stream is smaller than the video
    // it accompanies, never larger, so it cannot match, and a single layer track on its own has no
    // second video track to compare against.
    bool reversedDualLayer = false;
    if (baseTrack != nullptr && baseTrack->foldElStreamIndex < 0 && baseTrack->dvBlockAddIdType)
    {
        const auto elReader = dynamic_cast<HEVCStreamReader*>(baseTrack->codecReader);
        for (const auto& [streamIdx, track] : m_tracks)
        {
            if (track.trackType != 1 || track.streamIndex == baseTrack->streamIndex || track.dvBlockAddIdType)
                continue;
            const auto blReader = dynamic_cast<HEVCStreamReader*>(track.codecReader);
            if (blReader == nullptr || elReader == nullptr)
                continue;
            // A first track whose RPU says single layer (a 1080p profile 8.1 picture before a plain 4K one) is no
            // enhancement layer, whatever its size.
            if (elReader->dvRpuElPresent() == 0)
                continue;
            if (blReader->getStreamWidth() > elReader->getStreamWidth() &&
                blReader->getStreamHeight() > elReader->getStreamHeight())
            {
                reversedDualLayer = true;
                break;
            }
        }
    }

    if (reversedDualLayer && !m_dvWriteProfile81)
        LTRACE(LT_WARN, 2,
               "Dolby Vision: the enhancement layer is listed BEFORE the base layer, so the two are not "
               "folded into one track and this file carries no Dolby Vision. The base layer is the larger "
               "picture and belongs first. Swap the two video lines and the pair is folded as it should be.");

    // --dv-profile=8.1 with nothing to convert used to be accepted in silence. There is only ever
    // something to convert when a dual layer source has been folded into one track, so if no fold
    // happened the option did nothing at all and the file came out exactly as profile 7 would have
    // made it, with no way to tell from the log. Refusing names the reason instead.
    //
    // A single layer file is the ordinary case here: it is already profile 5 or 8 and there is no
    // second layer to fold, so nothing about it needs converting.
    if (m_dvWriteProfile81)
    {
        bool folded = false;
        for (const auto& [streamIdx, track] : m_tracks)
        {
            if (track.foldElStreamIndex >= 0)
            {
                folded = true;
                break;
            }
        }
        // Reaching this is now the narrow case only: openDstFile has already refused a mux with no
        // candidate pair at all, before the destination was opened. What is left here is a pair
        // that looked foldable and whose configuration record could not be built, which is rare
        // enough to be worth keeping as a backstop rather than assuming it cannot happen.
        if (!folded)
        {
            if (reversedDualLayer)
                THROW(ERR_COMMON,
                      "--dv-profile=8.1 needs the two layers in the order a disc has them, and this mux lists "
                      "the ENHANCEMENT LAYER FIRST. The base layer is the larger picture and belongs on the "
                      "first video line; the enhancement layer, the smaller one carrying the Dolby Vision RPU, "
                      "belongs on the second. Swap the two video lines and the pair is folded and converted.")
            THROW(ERR_COMMON, DV_PROFILE81_NEEDS_DUAL_LAYER)
        }
    }

    pairMvcViews();

    // A Blu-ray TrueHD track arrives as its lossless frames PLUS a 448 kbps AC-3 core, both on one
    // PID, because a disc has to carry something for a player that cannot decode the lossless
    // stream. Matroska has no such arrangement: an A_TRUEHD track holds the lossless stream alone,
    // and an AC-3 frame sitting inside one is something a player will hand to an MLP decoder. The
    // core was therefore dropped, which is what a Matroska player expects, but it also destroyed
    // the only part a Blu-ray needs: authoring a disc back out of that file produced a stream the
    // spec does not allow, and the audio could not be recovered from anywhere.
    //
    // So the core keeps its bytes and gets a track of its own, which is what the reference muxers
    // write and what a disc remux normally looks like. merge-ac3-track= braids the two back onto
    // one PID when the file is authored back to a disc, closing the round trip.
    //
    // This has to happen HERE rather than at intAddStream time: an AC-3 family reader has not seen
    // a frame when the stream is added, so isTrueHD() is still false and no core would be found.
    // The entries are collected first and inserted afterwards, because inserting into the map while
    // walking it would let the walk reach the entries it is creating.
    std::vector<std::pair<int, MkvTrackInfo>> coreTracks;
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType != 2 || track.matroskaCodecID != MATROSKA_CODEC_ID_AUDIO_TRUEHD || track.dropAc3Core)
            continue;
        // isTrueHD means "AC-3 core plus lossless", the disc arrangement. A TrueHD track read out
        // of a Matroska file has no core, and down-to-ac3 has already turned the whole thing into
        // plain AC-3.
        const auto ac3 = dynamic_cast<AC3Codec*>(track.codecReader);
        const bool discCore = ac3 != nullptr && ac3->isTrueHD() && !ac3->getDownconvertToAC3();

        // ** THE SAME PAIRING ARRIVING BY THE OTHER ROUTE USED TO BE THROWN AWAY IN SILENCE. **
        //
        // merge-ac3-track= and merge-ac3-file= build a TrueHD track with a core beside it out of two
        // separate sources, and they do it through a reader that is not in the AC-3 family at all,
        // so the question above answered no, no companion track was made, and every core packet met
        // the ac3CoreStreamIndex < 0 line in muxPacketInternal and was dropped. Measured on a
        // 4,261,662 byte TrueHD stream with a 1,120,000 byte AC-3 companion: the output was the same
        // size to the byte as the same TrueHD muxed with no companion at all, one track instead of
        // two, at "Mux successful complete" with nothing said. The core the user asked for was gone.
        //
        // The question that decides this is whether the track has a core travelling with it, and two
        // readers can answer it.
        const auto merged = dynamic_cast<TrueHDAC3MergeReader*>(track.codecReader);
        const bool mergedCore = merged != nullptr && merged->hasAc3Core();

        if (!discCore && !mergedCore)
            continue;

        MkvTrackInfo core;
        // Real stream indexes are small and are what packets are routed by, so they cannot be
        // renumbered to make room. The companion is keyed above every real one instead, which also
        // places it after its TrueHD track in the file.
        core.streamIndex = AC3_CORE_STREAM_BASE + streamIdx;
        core.codecReader = track.codecReader;
        core.codecID = CODEC_A_AC3;
        core.matroskaCodecID = MATROSKA_CODEC_ID_AUDIO_AC3;
        core.trackType = 2;
        core.language = track.language;
        // The rate and channel count read from an AC-3 family reader a few lines above describe the
        // CORE, not the lossless stream, so they belong to this track rather than the one they were
        // read onto. A merged core is the other way round: its track was opened as A_MLP and carries
        // the LOSSLESS figures, 8 channels where the core has 6, so the core is asked for its own.
        core.sampleRate = mergedCore ? merged->ac3CoreSampleRate() : track.sampleRate;
        core.channels = mergedCore ? merged->ac3CoreChannels() : track.channels;
        core.markedDefault = false;
        core.ac3CoreOfStream = streamIdx;
        static std::mt19937_64 rng(std::random_device{}());
        core.trackUID = rng() | 1ull << 63;  // 8 bytes always, as above

        track.ac3CoreStreamIndex = core.streamIndex;
        coreTracks.emplace_back(core.streamIndex, core);

        if (mergedCore)
            LTRACE(LT_INFO, 2,
                   "TrueHD: the merged AC-3 core is written as its own track. Matroska cannot carry "
                   "it inside the lossless track the way a disc does, so the two travel side by "
                   "side here and merge-ac3-track= puts them back on one stream when this file is "
                   "authored to a disc. Use drop-ac3-core to leave the core out.");
        else
            LTRACE(LT_INFO, 2,
                   "TrueHD: keeping the AC-3 compatibility core as its own track. Matroska cannot "
                   "carry it inside the lossless track the way a disc does, and dropping it would "
                   "lose the only part a Blu-ray needs. Use merge-ac3-track= to put them back on "
                   "one stream when authoring a disc from this file, or drop-ac3-core to leave the "
                   "core out.");
    }
    for (auto& [key, core] : coreTracks) m_tracks[key] = core;

    // Track numbers are handed out as streams are added, so folding one away leaves a gap in the
    // sequence. A gap is legal, a track number is only an identifier, but renumbering costs nothing
    // at this point because no block has been written yet, and it keeps the file ordinary.
    int nextNumber = 1;
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.foldedIntoStream >= 0 || track.ac3CoreOfStream >= 0)
            continue;
        track.trackNumber = nextNumber++;
        // The core takes the number straight after the track it came from, so it sits beside it
        // rather than after every other stream. Its map key cannot express that, because the key is
        // what packets are routed by, so the numbering carries it and writeTracks writes in that
        // order. This matches how a disc remux is normally laid out: lossless track, then its core,
        // then the disc's other audio.
        if (track.ac3CoreStreamIndex >= 0)
        {
            const auto core = m_tracks.find(track.ac3CoreStreamIndex);
            if (core != m_tracks.end())
                core->second.trackNumber = nextNumber++;
        }
    }
}

void MatroskaMuxer::writeDeferredHeader()
{
    // Re-read track properties now that stream readers have parsed their headers. This is where
    // the exact Dolby Vision test lives and where it throws, so nothing below it may run before it.
    refreshTrackProperties();

    // A text subtitle that is drawn into pictures is PGS by now, which a WebM file cannot hold; the check at
    // openDstFile could not tell. Still before the file is created.
    if (m_webm)
        for (const auto& [index, track] : m_tracks)
            if (track.trackType == MKV_TRACK_TYPE_SUBTITLE && track.matroskaCodecID != MATROSKA_CODEC_ID_SRT &&
                !isWebmVttText(track.matroskaCodecID))
                THROW(ERR_COMMON,
                      "A WebM file holds subtitles as text only, and this one is drawn into pictures. "
                      "Keep the subtitles as text, give the output an .mkv name, or leave the track out.")

    // WebM has no attachments.
    if (m_webm && !m_sourceAttachments.empty())
    {
        LTRACE(LT_INFO, 2,
               "Matroska: " << m_sourceAttachments.size()
                            << " attachment(s) of the source left out: a WebM file cannot hold attachments.");
        m_sourceAttachments.clear();
    }

    // A merged Dolby Vision or 3D track holds frames back for the other layer, and a part cannot be closed with frames
    // still held. Splitting is refused for it, rather than parts that lose pictures at every cut, and here, before the
    // destination exists, so that the refusal leaves no file behind.
    if (isSplitting())
        for (const auto& [streamIdx, track] : m_tracks)
            if (track.foldElStreamIndex >= 0)
                THROW(ERR_COMMON,
                      "Splitting a Matroska output is not supported with a merged dual layer Dolby Vision "
                      "or 3D track. Make one file, or mux the layers to a transport stream.")

    // Only now is the mux known to be going ahead, so only now is the destination created. Before
    // this line the file at that path is whatever the user had there.
    openDestination();

    // Build codec private data for all tracks
    for (auto& [streamIdx, track] : m_tracks) buildCodecPrivate(track);

    // The enhancement layer's own HEVC configuration record, kept on the base track and written
    // beside the Dolby Vision record as the "hvcE" block addition mapping. It is nothing more than
    // the enhancement track's CodecPrivate, so no parameter set is parsed a second time. This has
    // to happen HERE and not while the pairing is decided, because CodecPrivate is built above,
    // after refreshTrackProperties has run: reading it any earlier stores an empty record and the
    // mapping is written as a zero length blob without anything failing.
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.foldElStreamIndex < 0)
            continue;
        const auto el = m_tracks.find(track.foldElStreamIndex);

        // 3D takes a different record: the two views share one, built from both their parameter
        // sets, and written as the "mvcC" block addition mapping.
        //
        // A reference 3D file ALSO appends it to CodecPrivate, inside an mvcC box after the avcC
        // record, and this does not: the codec mapping keeps that form for legacy reasons and
        // prefers the block addition mapping, which carries the record unambiguously.
        if (track.foldKind == MkvTrackInfo::FoldKind::MvcDependent)
        {
            if (el != m_tracks.end())
                track.mvcConfig = buildMVCDecoderConfigRecord(track.codecReader, el->second.codecReader);
            if (track.mvcConfig.empty())
            {
                LTRACE(LT_WARN, 2,
                       "3D: the dependent view has no subset sequence parameter set, so no MVC "
                       "configuration record is written. Both views are still in the track.");
            }
            continue;
        }

        if (el != m_tracks.end())
            track.dvElConfig = el->second.codecPrivate;
        if (track.dvElConfig.empty())
            LTRACE(LT_WARN, 2,
                   "Dolby Vision: the enhancement layer has no HEVC configuration record, so the hvcE "
                   "block addition mapping is not written. The track still plays as HDR10.");
    }

    // Write SegmentInfo
    writeSegmentInfo();

    // Write Tracks
    writeTracks();

    // Chapters must precede the first Cluster, because a player reads the header once and does
    // not go looking for them later.
    if (isSplitting())
    {
        m_allChapters = m_chapters;
        m_allChapterInfo = m_chapterInfo;
        for (const double seconds : m_chapters)
            if (seconds > 0.0)
                m_chapterSplitPts.push_back(static_cast<int64_t>(seconds * INTERNAL_PTS_FREQ));
        std::sort(m_chapterSplitPts.begin(), m_chapterSplitPts.end());
        // The lowest numbered video track, or audio track when there is no video, or the lowest numbered track. A
        // subtitle track first in the meta decided the split of a sound-only output, so parts were cut where a
        // subtitle began, and a part could be as long as the gap between two subtitles.
        for (const int wanted : {static_cast<int>(MKV_TRACK_TYPE_VIDEO), static_cast<int>(MKV_TRACK_TYPE_AUDIO), -1})
            for (const auto& [streamIdx, track] : m_tracks)
                if (m_mainTrackNumber == 0 && (wanted < 0 || static_cast<int>(track.trackType) == wanted))
                {
                    m_mainTrackNumber = track.trackNumber;
                    // Without video every audio track drives the split (drivesSplit): the first one alone did, and
                    // where it was the shorter the split stopped with it. A 2 s track listed before a 6 s one gave
                    // a part of 1.5 s and one of 5 s for --split-duration=1s.
                    m_splitByAnyAudio = wanted == static_cast<int>(MKV_TRACK_TYPE_AUDIO);
                }
    }
    else
        writeChapters();
    writeTags();

    m_headerWritten = true;
}

// ---- every edition of a Matroska source ----

namespace
{
struct EbmlNode
{
    uint32_t id = 0;
    std::vector<uint8_t> body;       // a leaf's body
    std::vector<EbmlNode> children;  // a master's children
    bool master = false;
};

bool readEbmlNumber(const std::vector<uint8_t>& d, size_t& at, const size_t end, const bool keep, uint64_t& v)
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
}

// The masters of the Chapters element: EditionEntry, ChapterAtom, ChapterDisplay, ChapProcess, ChapProcessCommand,
// ChapterTrack
bool isChapterMaster(const uint32_t id)
{
    return id == MATROSKA_ID_EDITIONENTRY || id == MATROSKA_ID_CHAPTERATOM || id == 0x80 || id == 0x6944 ||
           id == 0x6911 || id == 0x8F;
}

std::vector<EbmlNode> parseEbml(const std::vector<uint8_t>& d, size_t at, const size_t end)
{
    std::vector<EbmlNode> out;
    while (at < end)
    {
        uint64_t id = 0, len = 0;
        if (!readEbmlNumber(d, at, end, true, id) || !readEbmlNumber(d, at, end, false, len) || len > end - at)
            break;
        EbmlNode node;
        node.id = static_cast<uint32_t>(id);
        node.master = isChapterMaster(node.id);
        if (node.master)
            node.children = parseEbml(d, at, at + static_cast<size_t>(len));
        else
            node.body.assign(d.begin() + static_cast<std::ptrdiff_t>(at),
                             d.begin() + static_cast<std::ptrdiff_t>(at + len));
        out.push_back(std::move(node));
        at += static_cast<size_t>(len);
    }
    return out;
}

uint64_t uintOf(const std::vector<uint8_t>& b)
{
    uint64_t v = 0;
    for (const uint8_t c : b) v = v << 8 | c;
    return v;
}

void writeEbml(const std::vector<EbmlNode>& nodes, std::vector<uint8_t>& out)
{
    for (const EbmlNode& n : nodes)
    {
        std::vector<uint8_t> body;
        if (n.master)
            writeEbml(n.children, body);
        else
            body = n.body;
        uint8_t hdr[16];
        const int h = ebml_write_master_open(hdr, n.id, body.size());
        out.insert(out.end(), hdr, hdr + h);
        out.insert(out.end(), body.begin(), body.end());
    }
}

void setUint(EbmlNode& n, const uint64_t v)
{
    n.body.clear();
    int bytes = 1;
    while (bytes < 8 && v >> (8 * bytes)) ++bytes;
    for (int i = bytes - 1; i >= 0; --i) n.body.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

int64_t atomStart(const EbmlNode& atom)
{
    for (const EbmlNode& c : atom.children)
        if (c.id == MATROSKA_ID_CHAPTERTIMESTART)
            return static_cast<int64_t>(uintOf(c.body));
    return 0;
}

// The atoms of one level that fall into [winStart, winEnd) of the source's clock, moved so that origin is 0. With
// keepPlaying, the last atom that starts at or before winStart is kept as well, starting at winStart: the chapter
// playing where a cut begins, as MuxerManager::readSourceChapters keeps it. An end is clipped to the window; where
// nothing of an atom is left it is dropped, and an ordered edition, which needs every end, drops it altogether.
void moveAtoms(std::vector<EbmlNode>& nodes, const int64_t origin, const int64_t winStart, const int64_t winEnd,
               const bool keepPlaying, const bool ordered, const int64_t slack)
{
    // The chapter playing at the cut, and apart from it the hidden one in effect there: a hidden chapter is not what a
    // player shows, so it must not take the place of the one it does
    const auto hidden = [](const EbmlNode& atom)
    {
        for (const EbmlNode& c : atom.children)
            if (c.id == MATROSKA_ID_CHAPTERFLAGHIDDEN)
                return uintOf(c.body) != 0;
        return false;
    };
    int playing = -1, playingHidden = -1;
    if (keepPlaying)
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        {
            if (nodes[i].id != MATROSKA_ID_CHAPTERATOM || atomStart(nodes[i]) > winStart)
                continue;
            int& which = hidden(nodes[i]) ? playingHidden : playing;
            if (which < 0 || atomStart(nodes[i]) >= atomStart(nodes[which]))
                which = i;
        }
    std::vector<EbmlNode> kept;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        EbmlNode& n = nodes[i];
        if (n.id != MATROSKA_ID_CHAPTERATOM)
        {
            kept.push_back(std::move(n));
            continue;
        }
        const int64_t start = atomStart(n);
        const bool inside =
            keepPlaying ? start > winStart && start < winEnd : start >= winStart - slack && start < winEnd - slack;
        if (!inside && i != playing && i != playingHidden)
            continue;
        const int64_t newStart = (std::max)(start, winStart) - origin;
        bool drop = false;
        std::vector<EbmlNode> children;
        for (EbmlNode& c : n.children)
        {
            if (c.id == MATROSKA_ID_CHAPTERTIMESTART)
                setUint(c, static_cast<uint64_t>((std::max)(int64_t{0}, newStart)));
            else if (c.id == MATROSKA_ID_CHAPTERTIMEEND)
            {
                const int64_t end = (std::min)(static_cast<int64_t>(uintOf(c.body)), winEnd) - origin;
                if (end <= newStart)
                {
                    drop = ordered;
                    continue;  // nothing of it is left in the window: no end, as a chapter that is not ordered allows
                }
                setUint(c, static_cast<uint64_t>(end));
            }
            children.push_back(std::move(c));
        }
        if (drop)
            continue;
        n.children = std::move(children);
        moveAtoms(n.children, origin, winStart, winEnd, keepPlaying, ordered, slack);
        kept.push_back(std::move(n));
    }
    nodes = std::move(kept);
}
}  // namespace

void MatroskaMuxer::setSourceChapterTree(const std::vector<uint8_t>& chapters, const int64_t cutStartNs,
                                         const int64_t cutEndNs)
{
    m_sourceChapterTree.clear();
    const std::vector<EbmlNode> editions = parseEbml(chapters, 0, chapters.size());
    int editionCount = 0;
    bool hiddenOrNested = false;
    const std::function<void(const std::vector<EbmlNode>&, int)> look =
        [&](const std::vector<EbmlNode>& nodes, const int depth)
    {
        for (const EbmlNode& n : nodes)
        {
            if (n.id == MATROSKA_ID_CHAPTERATOM)
            {
                if (depth > 0)
                    hiddenOrNested = true;
                // A disabled chapter too: the flat list leaves it out, and the tree keeps it with its flag.
                for (const EbmlNode& c : n.children)
                    if ((c.id == MATROSKA_ID_CHAPTERFLAGHIDDEN && uintOf(c.body) != 0) ||
                        (c.id == MATROSKA_ID_CHAPTERFLAGENABLED && uintOf(c.body) == 0))
                        hiddenOrNested = true;
                look(n.children, depth + 1);
            }
        }
    };
    for (const EbmlNode& e : editions)
        if (e.id == MATROSKA_ID_EDITIONENTRY)
        {
            ++editionCount;
            look(e.children, 0);
        }
    // The flat list of the default edition says all of it otherwise, and is written as before
    if (editionCount > 1 || hiddenOrNested)
    {
        m_sourceChapterTree = chapters;
        m_treeCutStartNs = cutStartNs;
        m_treeCutEndNs = cutEndNs;
    }
}

// Every edition of the source, moved: for the whole output by the cut, for a part of a split to that part. The UIDs,
// the names, the flags and the nesting stay as the source has them. EditionFlagHidden is left out, as from the
// chapters tsMuxeR writes itself: RFC 9559 does not define it. False when nothing of any edition falls into the window.
bool MatroskaMuxer::writeChapterTree()
{
    std::vector<EbmlNode> editions = parseEbml(m_sourceChapterTree, 0, m_sourceChapterTree.size());
    int64_t origin = m_treeCutStartNs, winStart = m_treeCutStartNs, winEnd = m_treeCutEndNs;
    bool keepPlaying = m_treeCutStartNs > 0;
    if (m_writingPart)
    {
        origin = winStart = m_treeCutStartNs + static_cast<int64_t>(m_partStartSec * 1e9 + 0.5);
        winEnd = m_partEndSec >= 1e299
                     ? m_treeCutEndNs
                     : (std::min)(m_treeCutEndNs, m_treeCutStartNs + static_cast<int64_t>(m_partEndSec * 1e9 + 0.5));
        keepPlaying = false;
    }
    std::vector<EbmlNode> kept;
    for (EbmlNode& e : editions)
    {
        if (e.id != MATROSKA_ID_EDITIONENTRY)
            continue;
        bool ordered = false;
        std::vector<EbmlNode> children;
        for (EbmlNode& c : e.children)
        {
            if (c.id == 0x45BD)  // EditionFlagHidden
                continue;
            if (c.id == MATROSKA_ID_EDITIONFLAGORDERED && uintOf(c.body) != 0)
                ordered = true;
            children.push_back(std::move(c));
        }
        e.children = std::move(children);
        moveAtoms(e.children, origin, winStart, winEnd, keepPlaying, ordered, 500000);
        if (std::any_of(e.children.begin(), e.children.end(),
                        [](const EbmlNode& c) { return c.id == MATROSKA_ID_CHAPTERATOM; }))
            kept.push_back(std::move(e));
    }
    if (kept.empty())
        return false;
    std::vector<uint8_t> body;
    writeEbml(kept, body);
    uint8_t hdr[16];
    const int h = ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERS, body.size() + CRC_ELEMENT_BYTES);
    m_chaptersPos = m_file.pos() - m_segmentStartPos;
    writeToFile(hdr, h);
    beginCrc();
    writeToFile(body.data(), static_cast<int>(body.size()));
    endCrc();
    return true;
}

// A Blu-ray playlist carries a couple of hundred chapter marks and an MKV source carries its own,
// and until now neither reached Matroska output: the muxer had no chapter support at all, so a
// remux silently dropped every one of them. The Blu-ray path already turns the same list into
// playlist marks (BlurayHelper::createMPLSFile).
void MatroskaMuxer::writeChapters()
{
    // --keep-editions: the source's own Chapters, every edition and every link to another file, as they stand.
    if (m_keepEditions)
    {
        if (m_sourceEditions.chapters.empty())
            return;
        uint8_t hdr[16];
        const int h =
            ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERS, m_sourceEditions.chapters.size() + CRC_ELEMENT_BYTES);
        m_chaptersPos = m_file.pos() - m_segmentStartPos;
        writeToFile(hdr, h);
        beginCrc();
        writeToFile(m_sourceEditions.chapters.data(), static_cast<int>(m_sourceEditions.chapters.size()));
        endCrc();
        return;
    }
    // Every edition of a Matroska source, its hidden and nested chapters with them. Not for WebM, which has no
    // editions and no chapter flags.
    if (!m_sourceChapterTree.empty() && !m_webm)
    {
        writeChapterTree();
        return;
    }
    if (m_chapters.empty())
        return;

    // ChapterTimeStart is in nanoseconds and is NOT scaled by TimestampScale, unlike block
    // timestamps, so it must not go through the cluster timestamp conversion.
    std::vector<uint8_t> atoms;
    uint64_t uid = 0x1000;
    size_t chapterNo = 0;
    for (const double startSec : m_chapters)
    {
        const size_t index = chapterNo++;
        if (startSec < 0)
            continue;

        // EVERY chapter gets a name. Versions of this program before 2.19.0 keep a chapter only if
        // it has a title, so a nameless chapter written here would vanish when one of them reads
        // the file back. A source that named its chapters gets those names; anything else is
        // numbered the way every other tool numbers them.
        std::string name;
        std::string language;
        if (index < m_chapterInfo.size())
        {
            name = m_chapterInfo[index].cTitle;
            language = m_chapterInfo[index].language;
        }
        if (name.empty())
            name = "Chapter " + std::to_string(index + 1);
        if (language.empty())
            language = "und";

        // Every name the source gave, one ChapterDisplay each, in the source's order; one name otherwise. A Matroska
        // source with a title in several languages kept only one of them.
        std::vector<AVChapter::Name> names;
        if (index < m_chapterInfo.size())
            names = m_chapterInfo[index].names;
        if (names.empty())
            names.push_back({name, language, ""});
        std::vector<uint8_t> displays;
        for (auto& [title, lang, tag] : names)
        {
            if (title.empty())
                title = name;
            // The source's own language where it gave one. "und" otherwise, which is what tsMuxeR
            // already uses for an unknown track language, rather than claiming a language it guessed.
            if (lang.empty())
                lang = "und";
            // Sized from the strings rather than fixed, because a source title has no length limit.
            std::vector<uint8_t> display(title.size() + lang.size() + tag.size() + 64);
            int d = 0;
            d += ebml_write_string(display.data() + d, MATROSKA_ID_CHAPSTRING, title);
            d += ebml_write_string(display.data() + d, MATROSKA_ID_CHAPTERLANGUAGE, lang);
            if (!tag.empty() && !m_webm)  // not a WebM element
                d += ebml_write_string(display.data() + d, MATROSKA_ID_CHAPLANGUAGEBCP47, tag);
            uint8_t dispHdr[16];
            const int dh = ebml_write_master_open(dispHdr, MATROSKA_ID_CHAPTERDISPLAY, d);
            displays.insert(displays.end(), dispHdr, dispHdr + dh);
            displays.insert(displays.end(), display.begin(), display.begin() + d);
        }

        std::vector<uint8_t> atom(64);
        int p = 0;
        p += ebml_write_uint(atom.data() + p, MATROSKA_ID_CHAPTERUID, ++uid);
        p += ebml_write_uint(atom.data() + p, MATROSKA_ID_CHAPTERTIMESTART,
                             static_cast<uint64_t>(startSec * 1000000000.0 + 0.5));
        // The end the source gave, which was dropped. Optional for chapters that are not ordered (RFC 9559, 20.2.2).
        if (index < m_chapterInfo.size() &&
            m_chapterInfo[index].end > static_cast<int64_t>(startSec * 1000000000.0 + 0.5))
            p += ebml_write_uint(atom.data() + p, MATROSKA_ID_CHAPTERTIMEEND,
                                 static_cast<uint64_t>(m_chapterInfo[index].end));
        if (!m_webm)  // not a WebM element
            p += ebml_write_uint(atom.data() + p, MATROSKA_ID_CHAPTERFLAGHIDDEN, 0);
        atom.resize(p);
        atom.insert(atom.end(), displays.begin(), displays.end());

        uint8_t hdr[16];
        const int h = ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERATOM, atom.size());
        atoms.insert(atoms.end(), hdr, hdr + h);
        atoms.insert(atoms.end(), atom.begin(), atom.end());
    }
    if (atoms.empty())
        return;

    std::vector<uint8_t> edition;
    uint8_t ed[32];
    int e = 0;
    if (!m_webm)  // neither is a WebM element
    {
        e += ebml_write_uint(ed + e, MATROSKA_ID_EDITIONUID, 0x0EDU);
        // No EditionFlagHidden: RFC 9559 does not define it (0x45BD is not in its element list).
        e += ebml_write_uint(ed + e, MATROSKA_ID_EDITIONFLAGDEFAULT, 1);
    }
    edition.insert(edition.end(), ed, ed + e);
    edition.insert(edition.end(), atoms.begin(), atoms.end());

    uint8_t hdr[16];
    int h = ebml_write_master_open(hdr, MATROSKA_ID_EDITIONENTRY, edition.size());
    std::vector<uint8_t> chapters(hdr, hdr + h);
    chapters.insert(chapters.end(), edition.begin(), edition.end());

    h = ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERS, chapters.size() + CRC_ELEMENT_BYTES);
    m_chaptersPos = m_file.pos() - m_segmentStartPos;
    writeToFile(hdr, h);
    beginCrc();
    writeToFile(chapters.data(), static_cast<int>(chapters.size()));
    endCrc();
}

// How many tracks have to deliver a packet before the header can be written. Subtitle tracks do
// not, so a file of nothing but subtitles gives zero here and the header goes out on the first
// packet, which is correct: there is nothing left to discover.
size_t MatroskaMuxer::tracksNeedingFirstPacket() const
{
    size_t n = 0;
    for (const auto& [streamIdx, track] : m_tracks)
        if (track.trackType != MKV_TRACK_TYPE_SUBTITLE)
            ++n;
    return n;
}

void MatroskaMuxer::replayBufferedPackets()
{
    if (m_preHeaderPackets.empty())
        return;

    // Determine the minimum PTS across all buffered packets so that no track
    // produces negative relative timestamps. A VC-1 track is stamped with its DTS, which can lie
    // before every PTS, so that counts as well.
    //
    // A PGS track is written as one block per display set, at the time of its composition segment.
    // The window, palette and object segments of a set carry EARLIER times, because a decoder has
    // to receive them before the set is shown, and counting them moved the whole file: a subtitle
    // due at 0 ms started every track 68 ms late. So only the composition segments count. A segment
    // longer than a packet arrives in pieces, and a piece can begin with any byte, so the segment
    // boundaries are followed per track rather than trusting the first byte of each packet.
    std::map<int, size_t> pgsBytesLeft;
    bool haveMin = false;
    int64_t minPts = 0;
    for (const auto& pkt : m_preHeaderPackets)
    {
        const auto it = m_tracks.find(pkt.stream_index);
        if (it != m_tracks.end() && it->second.codecID == CODEC_S_PGS)
        {
            size_t& left = pgsBytesLeft[pkt.stream_index];
            const size_t size = pkt.data.size();
            if (left > 0)
            {
                left -= (std::min)(left, size);
                continue;
            }
            if (size < 3)
                continue;
            const size_t segment = 3 + (static_cast<size_t>(pkt.data[1]) << 8 | pkt.data[2]);
            left = segment > size ? segment - size : 0;
            if (pkt.data[0] != PGS_SEGMENT_PCS)
                continue;
        }
        minPts = haveMin ? (std::min)(minPts, pkt.pts) : pkt.pts;
        haveMin = true;
        if (it != m_tracks.end() && it->second.codecID == CODEC_V_VC1)
            minPts = (std::min)(minPts, pkt.dts);
    }
    if (!haveMin)
        minPts = m_preHeaderPackets[0].pts;

    // A file of nothing but subtitles keeps the source's own times. Every reader hands its packets over on the
    // source's clock, whose zero is where the source starts (an SRT file's 00:00:00,000, a disc clip's first picture),
    // and a file with video keeps them because its video starts there. Subtitles alone started at their first
    // subtitle instead: one due at 1 s came out at 0, every later one as early, and a delay on the track did nothing.
    // The origin is now the source's zero, or the start of a --cut-start, when the first subtitle comes after it.
    // A file of audio alone, or of audio and subtitles, started at its first audio frame the same way, so a delay
    // given to the audio was dropped without a word. Every file without video now keeps the source's times.
    const bool hasVideo = std::any_of(m_tracks.begin(), m_tracks.end(),
                                      [](const auto& entry) { return entry.second.trackType == MKV_TRACK_TYPE_VIDEO; });
    if (!hasVideo && m_owner != nullptr)
        minPts = (std::min)(minPts, (std::max)(int64_t{0}, m_owner->getCutStart()));

    m_firstTimecode = minPts;
    m_firstTimecodeSet = true;

    // Replay all buffered packets through the normal mux path
    for (auto& pkt : m_preHeaderPackets)
    {
        AVPacket tmpPacket;
        tmpPacket.stream_index = pkt.stream_index;
        tmpPacket.pts = pkt.pts;
        tmpPacket.dts = pkt.dts;
        tmpPacket.flags = pkt.flags;
        tmpPacket.duration = pkt.duration;
        tmpPacket.data = pkt.data.data();
        tmpPacket.size = static_cast<int>(pkt.data.size());
        muxPacketInternal(tmpPacket);
    }
    m_preHeaderPackets.clear();
    m_preHeaderPackets.shrink_to_fit();
}

// ──────────────── Cluster writing ────────────────────────────────────────────

int64_t MatroskaMuxer::startCluster(int64_t timecodeMs)
{
    if (m_clusterOpen)
        flushCluster();
    // The cluster just closed may have moved the part back; this one is on the moved timeline.
    const int64_t shift = m_partShiftApplied;
    m_partShiftApplied = 0;
    timecodeMs -= shift;

    m_clusterTimecodeMs = timecodeMs;
    m_clusterBuf.clear();
    m_clusterDataSize = 0;
    m_clusterOpen = true;
    m_clusterBlocks = 0;

    // Record cluster position for cue entries
    m_clusterStartFilePos = m_file.pos() - m_segmentStartPos;

    // The CRC-32 first, its value filled in by flushCluster. A cue's position inside the cluster counts it.
    m_clusterBuf.resize(CRC_ELEMENT_BYTES);
    putCrcElement(m_clusterBuf.data(), 0);
    m_clusterDataSize += CRC_ELEMENT_BYTES;

    // Write ClusterTimecode into buffer
    uint8_t buf[16];
    const int len = ebml_write_uint(buf, MATROSKA_ID_CLUSTERTIMECODE, static_cast<uint64_t>(timecodeMs));
    m_clusterBuf.insert(m_clusterBuf.end(), buf, buf + len);
    m_clusterDataSize += len;
    return shift;
}

// ──────────────── Frame data conversion ──────────────────────────────────────

std::vector<uint8_t> MatroskaMuxer::convertAV1ToLowOverhead(const uint8_t* data, int size)
{
    // Convert from start-code-separated OBUs (with emulation prevention bytes)
    // to MKV's "low overhead bitstream format" (obu_has_size_field=1, LEB128 sizes).
    //
    // Per the AV1-in-Matroska spec:
    //   - Temporal delimiter OBUs are stripped.
    //   - Duplicate SEQUENCE_HEADER OBUs are deduplicated (keep only the last one
    //     before the first FRAME/FRAME_HEADER).  The duplicates arise because
    //     extractData() prepends the SH from the codec private, but the SimpleBlock
    //     itself usually contains its own SH with potentially different trailing bits.

    uint8_t* const dataStart = const_cast<uint8_t*>(data);
    uint8_t* const dataEnd = const_cast<uint8_t*>(data + size);

    // Temporary buffer for removing emulation prevention bytes
    std::vector<uint8_t> rawBuf(size);

    // ---- Pass 1: collect each OBU in low-overhead form ----
    struct ConvertedObu
    {
        Av1ObuType type;
        std::vector<uint8_t> bytes;  // header + LEB128 size + raw payload
    };
    std::vector<ConvertedObu> obus;
    obus.reserve(16);

    uint8_t* curObu = NALUnit::findNextNAL(dataStart, dataEnd);

    while (curObu < dataEnd)
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(curObu, dataEnd);
        if (hdrLen < 0)
            break;

        // Find the start of the NEXT start code to determine current OBU's boundary
        uint8_t* nextStartCode = NALUnit::findNALWithStartCode(curObu, dataEnd, true);

        // OBU payload runs from curObu+hdrLen to nextStartCode
        // Trim trailing zero bytes (they're part of the start code prefix, not the OBU)
        uint8_t* obuPayloadEnd = nextStartCode;
        while (obuPayloadEnd > curObu + hdrLen && obuPayloadEnd[-1] == 0) obuPayloadEnd--;

        const uint8_t* payload = curObu + hdrLen;
        const int payloadWithEPLen = static_cast<int>(obuPayloadEnd - payload);

        // Remove emulation prevention bytes from payload
        int rawPayloadLen = 0;
        if (payloadWithEPLen > 0)
        {
            rawPayloadLen =
                av1_remove_emulation_prevention(payload, payload + payloadWithEPLen, rawBuf.data(), rawBuf.size());
            if (rawPayloadLen < 0)
            {
                // Fallback: use payload as-is
                memcpy(rawBuf.data(), payload, payloadWithEPLen);
                rawPayloadLen = payloadWithEPLen;
            }
        }

        // Skip temporal delimiter OBUs (not needed in MKV)
        if (obuHdr.obu_type != Av1ObuType::TEMPORAL_DELIMITER)
        {
            ConvertedObu obu;
            obu.type = obuHdr.obu_type;

            // Write OBU header byte(s) with obu_has_size_field=1 (bit 1)
            const uint8_t hdrByte = curObu[0] | 0x02;
            obu.bytes.push_back(hdrByte);
            if (obuHdr.obu_extension_flag)
                obu.bytes.push_back(curObu[1]);

            // Write LEB128-encoded payload size
            uint8_t leb128Buf[8];
            const int leb128Len = encodeLeb128(leb128Buf, static_cast<uint64_t>(rawPayloadLen));
            obu.bytes.insert(obu.bytes.end(), leb128Buf, leb128Buf + leb128Len);

            // Write raw payload (emulation prevention bytes removed)
            if (rawPayloadLen > 0)
                obu.bytes.insert(obu.bytes.end(), rawBuf.data(), rawBuf.data() + rawPayloadLen);

            obus.push_back(std::move(obu));
        }

        // Advance to the next OBU (skip past the next start code)
        if (nextStartCode < dataEnd)
            curObu = NALUnit::findNextNAL(nextStartCode, dataEnd);
        else
            break;
    }

    // ---- Pass 2: deduplicate SEQUENCE_HEADER OBUs ----
    // If multiple SEQUENCE_HEADERs appear before the first FRAME/FRAME_HEADER,
    // keep only the last one (from the SimpleBlock data, not the codec private copy).
    int lastShIdx = -1;
    int firstFrameIdx = static_cast<int>(obus.size());
    for (int i = 0; i < static_cast<int>(obus.size()); i++)
    {
        if (obus[i].type == Av1ObuType::SEQUENCE_HEADER)
            lastShIdx = i;
        if (obus[i].type == Av1ObuType::FRAME || obus[i].type == Av1ObuType::FRAME_HEADER)
        {
            firstFrameIdx = i;
            break;
        }
    }

    // ---- Pass 3: emit the final byte stream ----
    std::vector<uint8_t> result;
    result.reserve(size);

    for (int i = 0; i < static_cast<int>(obus.size()); i++)
    {
        // Skip duplicate SEQUENCE_HEADERs that precede the first FRAME
        if (obus[i].type == Av1ObuType::SEQUENCE_HEADER && i < firstFrameIdx && i != lastShIdx)
            continue;

        result.insert(result.end(), obus[i].bytes.begin(), obus[i].bytes.end());
    }

    return result;
}

// The NAL type, which sits in a different place in each codec.
int MatroskaMuxer::nalTypeOf(const int codecID, const uint8_t* nal, const int size)
{
    if (size < 1)
        return -1;
    switch (codecID)
    {
    case CODEC_V_MPEG4_H264:
        return nal[0] & 0x1F;
    case CODEC_V_MPEG4_H265:
        return (nal[0] >> 1) & 0x3F;
    case CODEC_V_MPEG4_H266:
        return size >= 2 ? (nal[1] >> 3) & 0x1F : -1;
    default:
        return -1;
    }
}

void MatroskaMuxer::noteStartCode(MkvTrackInfo& track, const int nalType, const int len)
{
    if (nalType < 0 || track.startCodeMixed)
        return;
    const auto seen = track.startCodeByType.find(nalType);
    if (seen == track.startCodeByType.end())
        track.startCodeByType[nalType] = len;
    else if (seen->second != len)
        track.startCodeMixed = true;  // this type is framed both ways, so no rule describes it
}

// Turn the observations into the line that goes into the manifest, or nothing.
//
// Nothing is written when a type was framed both ways, or when every type used four bytes, which is
// what this muxer writes anyway and therefore needs no recording.
std::string MatroskaMuxer::startCodeRule(const MkvTrackInfo& track)
{
    if (track.startCodeMixed || track.startCodeByType.empty())
        return {};

    std::string three, four;
    for (const auto& [nalType, len] : track.startCodeByType)
    {
        std::string& into = len == 3 ? three : four;
        if (!into.empty())
            into += ",";
        into += std::to_string(nalType);
    }
    if (three.empty())
        return {};  // all four byte, which is the default

    std::string rule;
    if (!four.empty())
        rule = "4:" + four;
    if (!three.empty())
    {
        if (!rule.empty())
            rule += " ";
        rule += "3:" + three;
    }
    return rule;
}

std::vector<uint8_t> MatroskaMuxer::convertAnnexBToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data,
                                                                  const int size)
{
    // Convert Annex B start-code-separated NALUs to 4-byte length-prefixed NALUs.
    // This is the format required for H.264/HEVC/VVC in Matroska.

    const uint8_t* end = data + size;
    std::vector<uint8_t> result;
    result.reserve(size);

    uint8_t* curPos = NALUnit::findNextNAL(const_cast<uint8_t*>(data), const_cast<uint8_t*>(end));

    while (curPos < end)
    {
        // Find the next start code to determine NALU boundaries
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curPos, const_cast<uint8_t*>(end), true);

        // NALU data runs from curPos to (nextNal minus trailing zeros of the start code)
        uint8_t* naluEnd = nextNal;
        if (nextNal < end)
        {
            // Back up past the trailing zeros that are part of the next start code
            while (naluEnd > curPos && naluEnd[-1] == 0) naluEnd--;
        }

        const int naluSize = static_cast<int>(naluEnd - curPos);
        if (naluSize > 0)
        {
            // How the source framed this one. curPos sits just after the start code, so the byte in
            // front of the three that make it up says whether there was a fourth. Guarded, because
            // the very first NAL of the buffer may have nothing in front of it.
            const int startCodeLen = (curPos - data >= 4 && curPos[-4] == 0) ? 4 : 3;
            noteStartCode(track, nalTypeOf(track.codecID, curPos, naluSize), startCodeLen);

            // Write 4-byte big-endian length
            result.push_back(static_cast<uint8_t>((naluSize >> 24) & 0xFF));
            result.push_back(static_cast<uint8_t>((naluSize >> 16) & 0xFF));
            result.push_back(static_cast<uint8_t>((naluSize >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(naluSize & 0xFF));
            // Write NALU data
            result.insert(result.end(), curPos, naluEnd);
        }

        // Advance to next NALU
        curPos = NALUnit::findNextNAL(nextNal, const_cast<uint8_t*>(end));
    }

    return result;
}

// The dependent view of a 3D pair, converted into the same length prefixed form and appended after
// the base view's NALs. Nothing is wrapped and nothing is stripped, unlike the Dolby Vision
// enhancement layer: both views are H.264, and a decoder that does not understand MVC already
// ignores the dependent view's NAL types on its own.
//
// The view and dependency delimiter, NAL type 24, is KEPT, though at least one other 3D file does
// not carry it. Dropping it was measured and costs the byte exact rebuild: h264StreamReader writes
// a replacement when the view is muxed back to a disc, but with a three byte start code where the
// disc had four, which moves every byte after it. Keeping it costs nothing, because type 24 is
// unspecified in H.264 and a decoder that does not know it has to ignore it.
std::vector<uint8_t> MatroskaMuxer::convertMvcDepToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data,
                                                                  const int size)
{
    return convertAnnexBToLengthPrefixed(track, data, size);
}

std::vector<uint8_t> MatroskaMuxer::convertDvElToLengthPrefixed(MkvTrackInfo& track, const uint8_t* data, int size)
{
    // The enhancement layer half of a dual layer Dolby Vision access unit, converted into the same
    // length prefixed form and appended after the base layer's NALs.
    //
    // Every enhancement layer NAL is wrapped in an unspecified NAL of type 63, which is two bytes,
    // 7E 01, in front of the original NAL INCLUDING its own header. Type 63 is unspecified, so a
    // decoder that knows nothing about Dolby Vision skips the whole thing and plays the base layer.
    // That is the entire mechanism.
    //
    // The RPU is the exception: it is NAL type 62, already unspecified, and travels as it is. On a
    // disc it is the last NAL of the enhancement layer access unit, so simply preserving order puts
    // it last here too, which is where it belongs.
    //
    // Nothing is re-escaped. The NAL was emulation prevented before it was wrapped, so it can never
    // contain 00 00 00, 00 00 01 or 00 00 02, and prefixing two bytes cannot create one.
    const uint8_t* end = data + size;
    std::vector<uint8_t> result;
    result.reserve(size + size / 64);

    uint8_t* curPos = NALUnit::findNextNAL(const_cast<uint8_t*>(data), const_cast<uint8_t*>(end));

    while (curPos < end)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curPos, const_cast<uint8_t*>(end), true);
        uint8_t* naluEnd = nextNal;
        if (nextNal < end)
        {
            while (naluEnd > curPos && naluEnd[-1] == 0) naluEnd--;
        }

        const int naluSize = static_cast<int>(naluEnd - curPos);
        if (naluSize > 0)
        {
            const int nalType = (curPos[0] >> 1) & 0x3F;
            const bool isRpu = nalType == static_cast<int>(HevcUnit::NalType::DVRPU);

            // The enhancement layer's own framing, recorded against the NAL's own type. The split
            // strips the wrapper and re-emits this same NAL, so this type is the one that decides
            // how it is framed on the way back out.
            const int startCodeLen = (curPos - data >= 4 && curPos[-4] == 0) ? 4 : 3;
            noteStartCode(track, nalType, startCodeLen);

            // Profile 8.1 mode: the RPU that goes into the picture is the CONVERTED one, and the
            // original is kept aside so the disc can be rebuilt from this file later. Everything
            // else, the enhancement layer included, is untouched and still travels inert.
            std::vector<uint8_t> converted;
            if (isRpu && m_dvWriteProfile81)
            {
                std::string err;
                if (!DoviLib::instance().convertRpuToProfile81(curPos, static_cast<size_t>(naluSize), converted, err))
                {
                    // Refusing beats finishing. A file whose RPUs are part converted and part not
                    // would look complete and play wrong, which is the failure this whole design is
                    // built to avoid.
                    THROW(ERR_COMMON, "Dolby Vision: converting an RPU to profile 8.1 failed after "
                                          << m_dvRpusConverted << " frames: " << err)
                }
                // Keep the original. The payload only, WITHOUT its two byte NAL header, which is
                // what an extracted RPU file stores; the start code is added when the attachment is
                // written, so nothing has to be stripped again to reorder it.
                DvRpuEntry entry;
                entry.pts = m_dvCurrentRpuPts;
                entry.offset = m_dvRpuPayload.size();
                entry.length = static_cast<uint32_t>(naluSize - 2);
                m_dvRpuPayload.insert(m_dvRpuPayload.end(), curPos + 2, naluEnd);
                m_dvRpuIndex.push_back(entry);
                m_dvRpusConverted++;
            }

            const bool useConverted = !converted.empty();
            const int payloadSize = useConverted ? static_cast<int>(converted.size()) : naluSize;
            const int outSize = isRpu ? payloadSize : payloadSize + 2;

            result.push_back(static_cast<uint8_t>((outSize >> 24) & 0xFF));
            result.push_back(static_cast<uint8_t>((outSize >> 16) & 0xFF));
            result.push_back(static_cast<uint8_t>((outSize >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(outSize & 0xFF));
            if (!isRpu)
            {
                result.push_back(0x7E);  // NAL type 63, nuh_layer_id 0
                result.push_back(0x01);  // nuh_temporal_id_plus1 1
            }
            if (useConverted)
                result.insert(result.end(), converted.begin(), converted.end());
            else
                result.insert(result.end(), curPos, naluEnd);
        }

        curPos = NALUnit::findNextNAL(nextNal, const_cast<uint8_t*>(end));
    }

    return result;
}

void MatroskaMuxer::flushCluster()
{
    if (!m_clusterOpen || m_clusterBuf.empty())
        return;

    // The first cluster of a split part, cut at the earliest time a picture of it could show, which for a keyframe that
    // nothing is shown before is a frame or two early. Now its pictures are known: the cluster moves back by the
    // earliest of them, every block of the part with it, so its first picture is at 0 as the first part's is. An
    // audio frame that began before it stays where it is in time, a few milliseconds before 0. The cluster's timestamp
    // stays the same width, so no position in the cluster moves; where it would not, the part keeps the start it has.
    if (m_partOriginPending)
    {
        m_partOriginPending = false;
        const int64_t delta = m_partMinRelMs;
        const int64_t newTc = m_clusterTimecodeMs - delta;
        uint8_t oldBuf[16], newBuf[16];
        const int oldLen =
            ebml_write_uint(oldBuf, MATROSKA_ID_CLUSTERTIMECODE, static_cast<uint64_t>(m_clusterTimecodeMs));
        if (delta != INT64_MAX && delta > 0 && newTc >= 0 &&
            ebml_write_uint(newBuf, MATROSKA_ID_CLUSTERTIMECODE, static_cast<uint64_t>(newTc)) == oldLen &&
            m_clusterBuf.size() >= static_cast<size_t>(CRC_ELEMENT_BYTES + oldLen) &&
            memcmp(m_clusterBuf.data() + CRC_ELEMENT_BYTES, oldBuf, oldLen) == 0)
        {
            memcpy(m_clusterBuf.data() + CRC_ELEMENT_BYTES, newBuf, oldLen);
            m_clusterTimecodeMs = newTc;
            m_firstTimecode += delta * INTERNAL_PTS_PER_MS;
            for (CueEntry& cue : m_cueEntries) cue.timecodeMs -= delta;
            for (auto& [trackNumber, ms] : m_lastAudioCueMs) ms -= delta;
            m_partShiftApplied = delta;
        }
    }

    // Write Cluster master element with known size
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CLUSTER);
    hdrLen += ebml_write_size(header + hdrLen, m_clusterBuf.size());
    writeToFile(header, hdrLen);
    if (m_clusterBuf.size() >= CRC_ELEMENT_BYTES)
        putCrcElement(m_clusterBuf.data(),
                      static_cast<uint32_t>(crc32(0L, m_clusterBuf.data() + CRC_ELEMENT_BYTES,
                                                  static_cast<uInt>(m_clusterBuf.size() - CRC_ELEMENT_BYTES))));
    writeToFile(m_clusterBuf);

    m_clusterBuf.clear();
    m_clusterOpen = false;
    m_clusterDataSize = 0;
}

// ──────────────── muxPacket ──────────────────────────────────────────────────

void MatroskaMuxer::flushPendingFrame(MkvTrackInfo& track)
{
    if (!track.hasPendingFrame || track.pendingFrameData.empty())
    {
        track.hasPendingFrame = false;
        track.pendingFrameData.clear();
        return;
    }

    // Convert the accumulated raw data to MKV format
    const uint8_t* frameData = track.pendingFrameData.data();
    int frameSize = static_cast<int>(track.pendingFrameData.size());
    std::vector<uint8_t> convertedData;

    switch (track.codecID)
    {
    case CODEC_V_AV1:
        convertedData = convertAV1ToLowOverhead(frameData, frameSize);
        break;
    case CODEC_V_VP8:
    case CODEC_V_VP9:
        convertedData = VpxStreamReader::unframe(frameData, frameSize);
        frameData = convertedData.data();
        frameSize = static_cast<int>(convertedData.size());
        break;
    case CODEC_V_MPEG4_H264:
    case CODEC_V_MPEG4_H265:
    case CODEC_V_MPEG4_H266:
        convertedData = convertAnnexBToLengthPrefixed(track, frameData, frameSize);
        break;
    case CODEC_A_AAC:
        // A_AAC carries raw frames and describes them once, in CodecPrivate. The reader hands over
        // ADTS frames, header included, and they used to go in as they were. Some players forgive
        // that, but extracting the track puts a new ADTS header in front of each block, and the
        // doubled header decodes to nothing. 7 bytes, or 9 when a CRC follows the header.
        if (frameSize >= 7 && frameData[0] == 0xFF && (frameData[1] & 0xF6) == 0xF0)
        {
            const int headerLen = (frameData[1] & 0x01) ? 7 : 9;
            if (frameSize > headerLen)
            {
                frameData += headerLen;
                frameSize -= headerLen;
            }
        }
        break;
    default:
        break;
    }

    if (!convertedData.empty())
    {
        frameData = convertedData.data();
        frameSize = static_cast<int>(convertedData.size());
    }

    // Dual layer Dolby Vision: this base layer picture cannot be written yet. The two layers do
    // NOT arrive in step, the base layer runs several access units ahead, so the enhancement layer
    // for this picture has very likely not been delivered at all. Hold the frame instead and write
    // it once its enhancement access unit is complete.
    if (track.foldElStreamIndex >= 0)
    {
        MkvTrackInfo::HeldFrame held;
        held.data.assign(frameData, frameData + frameSize);
        held.pts = track.pendingPts;
        held.flags = track.pendingFlags;
        track.heldFrames.push_back(std::move(held));
        track.pendingFrameData.clear();
        track.hasPendingFrame = false;
        drainHeldFrames(track, false);
        return;
    }

    // V_MS/VFW/FOURCC is the Video for Windows mapping, which has no notion of reordered frames:
    // its blocks go out in decode order with DECODE timestamps. Stamped with presentation times,
    // the leading B pictures landed before zero and only 565 of 1436 frames could be decoded.
    const int64_t blockTime = track.codecID == CODEC_V_VC1 ? track.pendingDts : track.pendingPts;
    writeBlock(track, frameData, frameSize, blockTime, track.pendingFlags);

    track.pendingFrameData.clear();
    track.hasPendingFrame = false;
}

void MatroskaMuxer::flushHeldText(const int64_t upToPts)
{
    while (!m_heldText.empty() && m_heldText.begin()->first <= upToPts)
    {
        const auto node = m_heldText.extract(m_heldText.begin());
        const auto it = m_tracks.find(node.mapped().streamIndex);
        if (it != m_tracks.end())
            writeTextBlock(it->second, node.mapped().data.data(), static_cast<int>(node.mapped().data.size()),
                           node.key(), node.mapped().duration, node.mapped().additions);
    }
}

// One text subtitle: a BlockGroup holding its Block and its BlockDuration, the form the Matroska codec
// mapping gives S_TEXT/UTF8. A subtitle without a duration, shown until the next one, is a SimpleBlock.
void MatroskaMuxer::writeTextBlock(MkvTrackInfo& track, const uint8_t* data, int size, const int64_t pts,
                                   const int64_t duration, const std::vector<uint8_t>& additions)
{
    // WebVTT in WebM: a block is the cue's identifier, its settings and its text, a line each before the text. Both
    // are empty here.
    std::vector<uint8_t> webVtt;
    if (m_webm && track.matroskaCodecID == MATROSKA_CODEC_ID_SRT)
    {
        webVtt.assign({'\n', '\n'});
        webVtt.insert(webVtt.end(), data, data + size);
        data = webVtt.data();
        size = static_cast<int>(webVtt.size());
    }
    if (duration <= 0 && additions.empty())
    {
        writeBlock(track, data, size, pts, AVPacket::IS_IFRAME);
        return;
    }
    // The start and the end on the same millisecond grid as every other block, so that the end
    // written is where the subtitle really ends, whatever the fraction of a millisecond the start had.
    // A block copied with BlockAdditions but no duration is a BlockGroup without BlockDuration.
    int64_t relMs = internalToRoundedMs(pts - m_firstTimecode);
    const int64_t durationMs =
        duration <= 0 ? 0 : (std::max)(int64_t{1}, internalToRoundedMs(pts + duration - m_firstTimecode) - relMs);
    if (!track.hasMaxBlockPts || pts > track.maxBlockPts)
    {
        track.maxBlockPts = pts;
        track.hasMaxBlockPts = true;
    }
    if (!track.hasTextEnd || pts + (std::max)(int64_t{0}, duration) > track.maxTextEnd)
    {
        track.maxTextEnd = pts + (std::max)(int64_t{0}, duration);
        track.hasTextEnd = true;
    }
    const bool needNewCluster = !m_clusterOpen || relMs - m_clusterTimecodeMs >= CLUSTER_MAX_DURATION_MS ||
                                m_clusterDataSize >= CLUSTER_MAX_SIZE;
    const int64_t offset = relMs - m_clusterTimecodeMs;
    if (needNewCluster || (m_clusterOpen && relMs >= 0 && (offset < INT16_MIN || offset > INT16_MAX)))
        relMs -= startCluster(relMs);
    const auto relTimeMs = static_cast<int16_t>(relMs - m_clusterTimecodeMs);
    addCue(track, relMs, durationMs);

    uint8_t trackNumBuf[8];
    const int trackNumLen = ebml_write_size(trackNumBuf, track.trackNumber);
    const int blockPayloadSize = trackNumLen + 2 + 1 + size;
    uint8_t blockHeader[16];
    int blockHeaderLen = ebml_write_id(blockHeader, MATROSKA_ID_BLOCK);
    blockHeaderLen += ebml_write_size(blockHeader + blockHeaderLen, blockPayloadSize);
    uint8_t durationElement[16];
    const int durationLen =
        durationMs > 0 ? ebml_write_uint(durationElement, MATROSKA_ID_BLOCKDURATION, static_cast<uint64_t>(durationMs))
                       : 0;
    uint8_t additionsHeader[16];
    const int additionsHeaderLen =
        additions.empty()
            ? 0
            : ebml_write_master_open(additionsHeader, MATROSKA_ID_BLOCKADDITIONS, static_cast<int>(additions.size()));
    const int groupPayloadSize =
        blockHeaderLen + blockPayloadSize + durationLen + additionsHeaderLen + static_cast<int>(additions.size());
    uint8_t groupHeader[16];
    int groupHeaderLen = ebml_write_id(groupHeader, MATROSKA_ID_BLOCKGROUP);
    groupHeaderLen += ebml_write_size(groupHeader + groupHeaderLen, groupPayloadSize);

    m_clusterBuf.insert(m_clusterBuf.end(), groupHeader, groupHeader + groupHeaderLen);
    m_clusterBuf.insert(m_clusterBuf.end(), blockHeader, blockHeader + blockHeaderLen);
    m_clusterBuf.insert(m_clusterBuf.end(), trackNumBuf, trackNumBuf + trackNumLen);
    m_clusterBuf.push_back(static_cast<uint8_t>((relTimeMs >> 8) & 0xFF));
    m_clusterBuf.push_back(static_cast<uint8_t>(relTimeMs & 0xFF));
    m_clusterBuf.push_back(0);  // Block flags: no lacing
    m_clusterBuf.insert(m_clusterBuf.end(), data, data + size);
    m_clusterBuf.insert(m_clusterBuf.end(), durationElement, durationElement + durationLen);
    m_clusterBuf.insert(m_clusterBuf.end(), additionsHeader, additionsHeader + additionsHeaderLen);
    m_clusterBuf.insert(m_clusterBuf.end(), additions.begin(), additions.end());
    m_clusterDataSize += groupHeaderLen + groupPayloadSize;
    m_clusterBlocks++;
}

// Write one completed frame as a SimpleBlock. Split out of flushPendingFrame so that a dual layer
// Dolby Vision base track, which has to hold its frames back until the matching enhancement layer
// access unit has arrived, can emit them through exactly the same path later.
void MatroskaMuxer::writeBlock(MkvTrackInfo& track, const uint8_t* frameData, int frameSize, int64_t pts,
                               uint8_t pendingFlags)
{
    if (m_partNum > 0 && m_headerWritten)
    {
        if (!m_muxStartSet)
        {
            m_muxStartPts = m_firstTimecode;
            m_muxStartSet = true;
        }
        // A part starts at the earliest picture it shows, which with reordered pictures may come before the
        // keyframe. VP8, VP9 and AV1 never reorder, but their readers put the decoding time a frame before the
        // keyframe's, so a part started a frame early and its first picture showed at 42 ms instead of 0.
        const bool reorders =
            track.codecID != CODEC_V_VP8 && track.codecID != CODEC_V_VP9 && track.codecID != CODEC_V_AV1;
        if (!m_startingPart && drivesSplit(track) && (pendingFlags & AVPacket::IS_IFRAME) && m_clusterOpen &&
            splitDue(pts))
            startNextPart(track, (std::min)(pts, track.hasPendingFrame && reorders ? track.pendingDts : pts));
    }
    if (track.trackType != MKV_TRACK_TYPE_SUBTITLE && !m_heldText.empty())
        flushHeldText(pts);
    // Compute PTS relative to stream start (convert internal PTS units to milliseconds)
    int64_t relMs = internalToRoundedMs(pts - m_firstTimecode);

    // This track's two largest timestamps, for the Segment Duration at close
    if (!track.hasMaxBlockPts || pts > track.maxBlockPts)
    {
        if (track.hasMaxBlockPts)
        {
            track.nextMaxBlockPts = track.maxBlockPts;
            track.hasNextMaxBlockPts = true;
        }
        track.maxBlockPts = pts;
        track.hasMaxBlockPts = true;
    }
    else if (pts < track.maxBlockPts && (!track.hasNextMaxBlockPts || pts > track.nextMaxBlockPts))
    {
        track.nextMaxBlockPts = pts;
        track.hasNextMaxBlockPts = true;
    }

    // Decide whether to start a new cluster
    const bool needNewCluster = !m_clusterOpen || (relMs - m_clusterTimecodeMs >= CLUSTER_MAX_DURATION_MS) ||
                                (m_clusterDataSize >= CLUSTER_MAX_SIZE) ||
                                (track.trackType == 1 && (pendingFlags & AVPacket::IS_IFRAME) && m_clusterOpen &&
                                 (relMs - m_clusterTimecodeMs >= 1000));

    // The block offset is 16 bits. A block further from its cluster than that, in either direction,
    // gets a cluster of its own rather than a wrapped, wrong time. Never at a negative time: the
    // cluster timestamp is unsigned
    const int64_t offset = relMs - m_clusterTimecodeMs;
    if (needNewCluster || (m_clusterOpen && relMs >= 0 && (offset < INT16_MIN || offset > INT16_MAX)))
        relMs -= startCluster(relMs);

    // Cue points, as RFC 9559 (22.1) recommends: every video keyframe, every subtitle a player can show from, and
    // in a file without video each audio track at most once every 500 ms. Only video keyframes had one, so a player
    // could not seek to a subtitle, and a file of audio alone had no Cues at all.
    if ((pendingFlags & AVPacket::IS_IFRAME) && (track.trackType == 1 || track.trackType == MKV_TRACK_TYPE_SUBTITLE ||
                                                 (track.trackType == 2 && audioCueDue(track, relMs))))
        addCue(track, relMs, -1);

    if (m_partOriginPending && drivesSplit(track))
        m_partMinRelMs = (std::min)(m_partMinRelMs, relMs);
    const int16_t relTimeMs = static_cast<int16_t>(relMs - m_clusterTimecodeMs);

    // The last frame of a track that ends in padding goes into a BlockGroup with its DiscardPadding, which a
    // SimpleBlock cannot carry (RFC 9559, 10.4), so that a player drops the padding as the source said.
    // An ALAC track's last frame is a partial one, as a rule, shorter than the DefaultDuration says: it gets its
    // own BlockDuration, so that a player does not take it for a whole frame.
    int64_t lastFrameMs = 0;
    if (track.finalFlush && track.codecID == CODEC_A_ALAC && track.audioFrameNs > 0)
        if (const auto simple = dynamic_cast<SimplePacketizerReader*>(track.codecReader))
        {
            const double lastNs = simple->frameDurationInternal() * 1e9 / INTERNAL_PTS_FREQ;
            if (lastNs > 0 && lastNs + 500000 < static_cast<double>(track.audioFrameNs))
                lastFrameMs = (std::max)(int64_t{1}, static_cast<int64_t>(llround(lastNs / 1e6)));
        }
    if (track.finalFlush && (track.endPaddingNs > 0 || lastFrameMs > 0))
    {
        uint8_t trackNumBuf[8];
        const int trackNumLen = ebml_write_size(trackNumBuf, track.trackNumber);
        const int blockPayloadSize = trackNumLen + 2 + 1 + frameSize;
        uint8_t blockHeader[16];
        int blockHeaderLen = ebml_write_id(blockHeader, MATROSKA_ID_BLOCK);
        blockHeaderLen += ebml_write_size(blockHeader + blockHeaderLen, blockPayloadSize);
        // after the Block: its BlockDuration, its DiscardPadding, or both
        uint8_t paddingElement[32];
        int paddingLen = 0;
        if (lastFrameMs > 0)
            paddingLen +=
                ebml_write_uint(paddingElement, MATROSKA_ID_BLOCKDURATION, static_cast<uint64_t>(lastFrameMs));
        if (track.endPaddingNs > 0)
            paddingLen += ebml_write_sint(paddingElement + paddingLen, MATROSKA_ID_DISCARDPADDING, track.endPaddingNs);
        const int groupPayloadSize = blockHeaderLen + blockPayloadSize + paddingLen;
        uint8_t groupHeader[16];
        int groupHeaderLen = ebml_write_id(groupHeader, MATROSKA_ID_BLOCKGROUP);
        groupHeaderLen += ebml_write_size(groupHeader + groupHeaderLen, groupPayloadSize);
        m_clusterBuf.insert(m_clusterBuf.end(), groupHeader, groupHeader + groupHeaderLen);
        m_clusterBuf.insert(m_clusterBuf.end(), blockHeader, blockHeader + blockHeaderLen);
        m_clusterBuf.insert(m_clusterBuf.end(), trackNumBuf, trackNumBuf + trackNumLen);
        m_clusterBuf.push_back(static_cast<uint8_t>((relTimeMs >> 8) & 0xFF));
        m_clusterBuf.push_back(static_cast<uint8_t>(relTimeMs & 0xFF));
        m_clusterBuf.push_back(0);  // Block flags: no lacing
        m_clusterBuf.insert(m_clusterBuf.end(), frameData, frameData + frameSize);
        m_clusterBuf.insert(m_clusterBuf.end(), paddingElement, paddingElement + paddingLen);
        m_clusterDataSize += groupHeaderLen + groupPayloadSize;
        m_clusterBlocks++;
        return;
    }

    // A frame the source stored with BlockAdditions (HDR10+ metadata, for one) is a BlockGroup that carries them. A
    // BlockGroup has no keyframe flag: a frame that is not one says so with a ReferenceBlock, to the block before.
    if (track.frameAdditions && m_owner)
    {
        std::vector<uint8_t> additions;
        if (m_owner->takeBlockAdditions(track.streamIndex, pts, additions) && !additions.empty())
        {
            uint8_t trackNumBuf[8];
            const int trackNumLen = ebml_write_size(trackNumBuf, track.trackNumber);
            const int blockPayloadSize = trackNumLen + 2 + 1 + frameSize;
            uint8_t blockHeader[16];
            int blockHeaderLen = ebml_write_id(blockHeader, MATROSKA_ID_BLOCK);
            blockHeaderLen += ebml_write_size(blockHeader + blockHeaderLen, blockPayloadSize);
            uint8_t referenceElement[16];
            int referenceLen = 0;
            if (!(pendingFlags & AVPacket::IS_IFRAME))
            {
                const int64_t ref = track.lastBlockRelMs - relMs;
                referenceLen = ebml_write_sint(referenceElement, MATROSKA_ID_BLOCKREFERENCE, ref != 0 ? ref : -1);
            }
            uint8_t additionsHeader[16];
            const int additionsHeaderLen =
                ebml_write_master_open(additionsHeader, MATROSKA_ID_BLOCKADDITIONS, additions.size());
            const int groupPayloadSize = blockHeaderLen + blockPayloadSize + referenceLen + additionsHeaderLen +
                                         static_cast<int>(additions.size());
            uint8_t groupHeader[16];
            int groupHeaderLen = ebml_write_id(groupHeader, MATROSKA_ID_BLOCKGROUP);
            groupHeaderLen += ebml_write_size(groupHeader + groupHeaderLen, groupPayloadSize);
            m_clusterBuf.insert(m_clusterBuf.end(), groupHeader, groupHeader + groupHeaderLen);
            m_clusterBuf.insert(m_clusterBuf.end(), blockHeader, blockHeader + blockHeaderLen);
            m_clusterBuf.insert(m_clusterBuf.end(), trackNumBuf, trackNumBuf + trackNumLen);
            m_clusterBuf.push_back(static_cast<uint8_t>((relTimeMs >> 8) & 0xFF));
            m_clusterBuf.push_back(static_cast<uint8_t>(relTimeMs & 0xFF));
            m_clusterBuf.push_back(0);  // Block flags: no lacing
            m_clusterBuf.insert(m_clusterBuf.end(), frameData, frameData + frameSize);
            m_clusterBuf.insert(m_clusterBuf.end(), referenceElement, referenceElement + referenceLen);
            m_clusterBuf.insert(m_clusterBuf.end(), additionsHeader, additionsHeader + additionsHeaderLen);
            m_clusterBuf.insert(m_clusterBuf.end(), additions.begin(), additions.end());
            m_clusterDataSize += groupHeaderLen + groupPayloadSize;
            m_clusterBlocks++;
            track.lastBlockRelMs = relMs;
            return;
        }
    }
    track.lastBlockRelMs = relMs;

    // Write SimpleBlock

    uint8_t trackNumBuf[8];
    const int trackNumLen = ebml_write_size(trackNumBuf, track.trackNumber);
    const int blockPayloadSize = trackNumLen + 2 + 1 + frameSize;

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_SIMPLEBLOCK);
    hdrLen += ebml_write_size(header + hdrLen, blockPayloadSize);

    m_clusterBuf.insert(m_clusterBuf.end(), header, header + hdrLen);
    m_clusterBuf.insert(m_clusterBuf.end(), trackNumBuf, trackNumBuf + trackNumLen);
    m_clusterBuf.push_back(static_cast<uint8_t>((relTimeMs >> 8) & 0xFF));
    m_clusterBuf.push_back(static_cast<uint8_t>(relTimeMs & 0xFF));

    uint8_t flags = 0;
    if (pendingFlags & AVPacket::IS_IFRAME)
        flags |= 0x80;
    m_clusterBuf.push_back(flags);

    m_clusterBuf.insert(m_clusterBuf.end(), frameData, frameData + frameSize);
    m_clusterDataSize += hdrLen + blockPayloadSize;
    m_clusterBlocks++;
}

// A cue point for the block about to be written to the open cluster. Where it is not the cluster's first block, the
// point also says where in the cluster it lies (CueRelativePosition), so that a player goes straight to it.
void MatroskaMuxer::addCue(const MkvTrackInfo& track, const int64_t relMs, const int64_t durationMs)
{
    CueEntry cue;
    cue.timecodeMs = relMs;
    cue.trackNumber = track.trackNumber;
    cue.clusterOffset = m_clusterStartFilePos;
    if (m_clusterBlocks > 0)
        cue.relativePosition = static_cast<int64_t>(m_clusterBuf.size());
    cue.durationMs = durationMs;
    m_cueEntries.push_back(cue);
}

// In a file with video, the video keyframes are the cue points. Without video, an audio track gets one, but not
// more often than every 500 ms (RFC 9559, 22.1).
bool MatroskaMuxer::audioCueDue(const MkvTrackInfo& track, const int64_t relMs)
{
    for (const auto& [index, other] : m_tracks)
        if (other.trackType == 1)
            return false;
    const auto last = m_lastAudioCueMs.find(track.trackNumber);
    if (last != m_lastAudioCueMs.end() && relMs - last->second < 500)
        return false;
    m_lastAudioCueMs[track.trackNumber] = relMs;
    return true;
}

// Emit held base layer frames whose enhancement layer access unit has arrived complete.
//
// The two layers do not arrive in step: the base layer runs ahead by a few access units, so a base
// frame written the moment it is complete would go out before its enhancement layer exists. Frames
// are therefore held in arrival order and released from the front as their partner turns up.
//
// Matching is by TIMESTAMP, not by position, because position would resynchronise silently to the
// wrong picture if either stream ever skipped one. An enhancement access unit counts as complete
// only once the next one has started, or at end of stream, since a large one arrives as several
// packets.
//
// The queue is bounded. If the two streams ever drift further apart than the bound, the oldest
// frame is written WITHOUT its enhancement layer and counted, rather than growing memory without
// limit or deadlocking. That count is reported at the end of the mux.
void MatroskaMuxer::drainHeldFrames(MkvTrackInfo& track, const bool atEndOfStream)
{
    constexpr size_t MAX_HELD_FRAMES = 64;

    if (atEndOfStream && !track.pendingElData.empty())
    {
        // The last enhancement access unit has no successor to close it.
        track.elDone[track.elPts] = std::move(track.pendingElData);
        track.pendingElData.clear();
    }

    while (!track.heldFrames.empty())
    {
        MkvTrackInfo::HeldFrame& front = track.heldFrames.front();
        const auto el = track.elDone.find(front.pts);
        const bool forced = atEndOfStream || track.heldFrames.size() > MAX_HELD_FRAMES;

        if (el == track.elDone.end() && !forced)
            break;  // its enhancement layer may still be on its way

        if (el != track.elDone.end())
        {
            // Which picture this enhancement access unit belongs to. In profile 8.1 mode the
            // original RPU is kept aside as it passes, and its timestamp is what later puts the
            // attachment into display order.
            std::vector<uint8_t> wrapped;
            if (track.foldKind == MkvTrackInfo::FoldKind::MvcDependent)
            {
                wrapped = convertMvcDepToLengthPrefixed(track, el->second.data(), static_cast<int>(el->second.size()));
            }
            else
            {
                m_dvCurrentRpuPts = front.pts;
                wrapped = convertDvElToLengthPrefixed(track, el->second.data(), static_cast<int>(el->second.size()));
            }
            front.data.insert(front.data.end(), wrapped.begin(), wrapped.end());
            track.elDone.erase(el);
            track.elFramesMerged++;
        }
        else
        {
            track.elFramesUnmatched++;
        }

        writeBlock(track, front.data.data(), static_cast<int>(front.data.size()), front.pts, front.flags);
        track.heldFrames.pop_front();
    }

    if (atEndOfStream)
    {
        // Anything still here belongs to pictures that were never delivered.
        track.elFramesUnmatched += static_cast<int64_t>(track.elDone.size());
        track.elDone.clear();
    }
}

// The first frame of an MP3 written by LAME and its kind is a Xing or Info frame: no sound, but the stream's length
// and, in the LAME tag after it, the encoder delay and padding in samples (12 bits each). True when frame is one,
// with delay and padding set, -1 for the delay when it has no LAME tag.
static bool readMp3InfoFrame(const uint8_t* frame, const int size, int& delay, int& padding)
{
    delay = -1;
    padding = 0;
    if (size < 4 || frame[0] != 0xFF || (frame[1] & 0xE0) != 0xE0)
        return false;
    const bool mpeg1 = (frame[1] >> 3 & 3) == 3;
    const bool crc = (frame[1] & 1) == 0;
    const bool mono = (frame[3] >> 6) == 3;
    const int sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const int tag = 4 + (crc ? 2 : 0) + sideInfo;
    if (tag + 8 > size || (memcmp(frame + tag, "Xing", 4) != 0 && memcmp(frame + tag, "Info", 4) != 0))
        return false;
    const uint32_t flags =
        static_cast<uint32_t>(frame[tag + 4]) << 24 | frame[tag + 5] << 16 | frame[tag + 6] << 8 | frame[tag + 7];
    int p = tag + 8;
    if (flags & 1)
        p += 4;  // frames
    if (flags & 2)
        p += 4;  // bytes
    if (flags & 4)
        p += 100;  // seek table
    if (flags & 8)
        p += 4;  // quality
    // the LAME tag: a 9 character encoder name, then at 21 bytes from it the delay and the padding
    if (p + 24 <= size)
    {
        bool printable = true;
        for (int i = 0; i < 4; ++i) printable = printable && frame[p + i] >= 0x20 && frame[p + i] < 0x7F;
        if (printable)
        {
            delay = frame[p + 21] << 4 | frame[p + 22] >> 4;
            padding = (frame[p + 22] & 0x0F) << 8 | frame[p + 23];
        }
    }
    return true;
}

bool MatroskaMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
        return true;

    auto it = m_tracks.find(avPacket.stream_index);
    if (it == m_tracks.end())
        return true;

    // The Xing or Info frame at the start of an MP3 holds no sound, and was written as a frame of silence. It is
    // left out, and the LAME encoder delay and padding it gives become CodecDelay and DiscardPadding
    // (writeDeferredHeader), as a player skips them in an MP3 file.
    if (it->second.codecID == CODEC_A_MPEG_AUDIO && !it->second.mp3InfoChecked)
    {
        it->second.mp3InfoChecked = true;
        if (readMp3InfoFrame(avPacket.data, avPacket.size, it->second.mp3Delay, it->second.mp3Padding))
            return true;
    }

    // Before the header is written, buffer packets and wait until every track that needs its first
    // packet has delivered one. This ensures those codec readers are fully initialized (e.g. audio
    // sample rate, channels) before we write the Matroska track headers.
    //
    // SUBTITLE TRACKS ARE NOT WAITED FOR, and that is not a shortcut. A subtitle entry is complete
    // at intAddStream: it sets trackType and nothing else, refreshTrackProperties leaves it alone,
    // and no field of it is discovered by parsing. It is also the ONE kind of track that legitimately
    // carries nothing for a long stretch, because a forced subtitle track only has content in the
    // scenes that need it.
    //
    // Waiting for one was measured on a pressed disc whose third subtitle track has its first packet
    // 74.99 percent of the way into a 72 GB title: the muxer held 42 GB of packets in memory on a
    // 64 GB machine, then spent 178 seconds writing them out and 127 more freeing them, with the
    // progress figure stopped at 75.0 percent the whole time because nothing was being read.
    if (!m_headerWritten)
    {
        if (it->second.trackType != MKV_TRACK_TYPE_SUBTITLE)
            m_seenStreams.insert(avPacket.stream_index);
        if (it->second.trackType == 1)
        {
            auto& [lastPts, count] =
                m_preHeaderPictures.try_emplace(avPacket.stream_index, avPacket.pts, 0).first->second;
            if (count == 0 || avPacket.pts != lastPts)
                ++count;
            lastPts = avPacket.pts;
        }

        // Buffer a copy of this packet
        BufferedPacket bp;
        bp.stream_index = avPacket.stream_index;
        bp.pts = avPacket.pts;
        bp.dts = avPacket.dts;
        bp.flags = avPacket.flags;
        bp.duration = avPacket.duration;
        bp.data.assign(avPacket.data, avPacket.data + avPacket.size);
        m_preHeaderPackets.push_back(std::move(bp));

        // The timeline starts at the earliest timestamp in this buffer, so the buffer must reach
        // every picture that can come before it. A stream that begins at an open GOP, an HEVC CRA
        // or an MPEG-2 GOP whose B pictures lead it, delivers those leading pictures AFTER its
        // first one, with earlier timestamps. Writing the header at the first video packet left
        // them before zero: -125, -83 and -41 ms on an HEVC CRA start, which other software moved
        // to zero or read as a 42 ms file. So each video track first delivers a few pictures.
        bool videoReady = true;
        for (const auto& [idx, track] : m_tracks)
            if (track.trackType == 1 && track.foldedIntoStream < 0)
            {
                const auto pic = m_preHeaderPictures.find(idx);
                videoReady &= pic != m_preHeaderPictures.end() && pic->second.second >= PRE_HEADER_PICTURES;
            }
        // Leading pictures come a few frames late, never seconds. A video track with fewer pictures than that
        // (a short picture-in-picture, a still or a slide show) held the header back to the end of the mux, with
        // every packet of every track copied into memory meanwhile. Ten seconds of held packets are enough.
        if (!videoReady)
        {
            if (!m_preHeaderSpanStarted || avPacket.pts < m_preHeaderMinPts)
                m_preHeaderMinPts = avPacket.pts;
            m_preHeaderSpanStarted = true;
            if (avPacket.pts - m_preHeaderMinPts >= PRE_HEADER_MAX_SPAN)
                videoReady = true;
        }
        if (m_seenStreams.size() >= tracksNeedingFirstPacket() && videoReady)
        {
            writeDeferredHeader();
            replayBufferedPackets();
        }
        return true;
    }

    // Track first timecode for relative calculations
    if (!m_firstTimecodeSet)
    {
        m_firstTimecode = avPacket.pts;
        m_firstTimecodeSet = true;
    }

    return muxPacketInternal(avPacket);
}

// Whether the bytes end exactly where a PGS segment ends, and the type of the last segment
static bool pgsWholeSegments(const std::vector<uint8_t>& data, uint8_t& lastType)
{
    size_t pos = 0;
    lastType = 0;
    while (pos + 3 <= data.size())
    {
        lastType = data[pos];
        pos += 3 + (static_cast<size_t>(data[pos + 1]) << 8 | data[pos + 2]);
    }
    return pos == data.size();
}

// Whether the bytes are whole PGS segments, the last of them an END segment
static bool pgsDisplaySetComplete(const std::vector<uint8_t>& data)
{
    uint8_t lastType = 0;
    return pgsWholeSegments(data, lastType) && lastType == PGS_SEGMENT_END;
}

// Whether a display set can be shown on its own. Its composition segment says so: an epoch start
// (0x80) or an acquisition point (0x40) carries everything the decoder needs, while a normal case
// (0x00) only changes what an earlier set began. The state is the eighth byte of the segment body.
static bool pgsSetIsRandomAccess(const std::vector<uint8_t>& data)
{
    return data.size() > 10 && data[0] == PGS_SEGMENT_PCS && (data[10] & 0xC0) != 0;
}

bool MatroskaMuxer::muxPacketInternal(AVPacket& avPacket)
{
    auto it = m_tracks.find(avPacket.stream_index);
    if (it == m_tracks.end())
        return true;

    MkvTrackInfo& track = it->second;

    // A TrueHD track arrives as its lossless frames PLUS the AC-3 core frames, because a disc has
    // to carry both beside each other on one PID for a player that cannot decode the lossless
    // stream. Matroska has no such arrangement: an A_TRUEHD track holds the lossless stream alone,
    // and an AC-3 frame sitting inside one is something a player will hand to an MLP decoder. So
    // the core leaves this track, and unless it was turned down it is written to a track of its
    // own (built in refreshTrackProperties) rather than thrown away.
    if (track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_TRUEHD)
    {
        // Two core frames arrive WITHOUT the flag: the first one, before the reader's state machine
        // has classified the stream, and the last one, which end of stream flushing hands over
        // directly. Position is not a safe test for either, so the packet is asked what it is.
        //
        // An MLP frame states its own length in its first two bytes, as a count of 16 bit words, so
        // a genuine lossless frame's length field matches its size. An AC-3 frame read the same way
        // claims something else entirely: the 351 byte core frame at the end of a stream reads as
        // 5870 bytes. So a packet is treated as the core only when it carries the AC-3 sync word
        // AND fails to describe itself as MLP, which no real lossless frame can do.
        const bool isAc3Core = isTrueHDCorePacket(avPacket.data, avPacket.size);
        if ((avPacket.flags & AVPacket::IS_CORE_PACKET) || isAc3Core)
        {
            if (track.ac3CoreStreamIndex < 0)
                return true;
            // Re-enter as an ordinary packet on the companion track. That track is A_AC3, so it
            // cannot take this branch a second time, and the core flag is cleared so nothing
            // downstream still reads it as part of a TrueHD stream.
            AVPacket corePacket = avPacket;
            corePacket.stream_index = track.ac3CoreStreamIndex;
            corePacket.flags &= ~AVPacket::IS_CORE_PACKET;
            return muxPacketInternal(corePacket);
        }
    }

    // A folded Dolby Vision enhancement layer writes no block of its own. Its bytes are collected
    // on the base track, one access unit at a time, and appended to the matching base layer frame.
    if (track.foldedIntoStream >= 0)
    {
        const auto base = m_tracks.find(track.foldedIntoStream);
        if (base == m_tracks.end())
            return true;
        MkvTrackInfo& bl = base->second;

        // A packet with a new timestamp means the previous access unit is complete. Only then can
        // it be matched, because a large one arrives as several packets.
        if (!bl.pendingElData.empty() && bl.elPts != avPacket.pts)
        {
            bl.elDone[bl.elPts] = std::move(bl.pendingElData);
            bl.pendingElData.clear();
            drainHeldFrames(bl, false);
        }
        bl.elPts = avPacket.pts;
        bl.pendingElData.insert(bl.pendingElData.end(), avPacket.data, avPacket.data + avPacket.size);
        return true;
    }

    // A PGS display set is ONE block, at the time of its composition segment, and it is complete
    // when its END segment arrives. Grouping by equal timestamps, as below, got that wrong twice.
    // Text rendered to PGS gives each segment of a set its own decode side time, so a set was cut
    // into three or four blocks, out of order, of which other software kept a few. And a block was
    // written only when the NEXT packet arrived, which for the last piece of a set is the next
    // subtitle, possibly a minute later: far enough behind its cluster to overflow the 16 bit block
    // offset, so a cue due at 6.0 s was written as 71.5 s in a 60 s file.
    // A text subtitle is written as it is: one block per subtitle, at its start, with its duration. It
    // can arrive long before its time, because the next subtitle is read as soon as the one before
    // has ended, so it waits until the other tracks reach it; written at once it would open a cluster
    // ahead of the video and the clusters after it would go back in time. A file of subtitles alone
    // has nothing to wait for. Never merged with a packet of the same time: two subtitles may start
    // together.
    // A subtitle copied as it is waits for the other tracks the same way. Its packet is the length of the block's
    // BlockAdditions body (4 bytes), that body, and then the block's data (MatroskaCopyStreamReader).
    if (track.codecID == CODEC_S_MKVCOPY)
    {
        if (avPacket.size < 4)
            return true;
        const uint8_t* p = avPacket.data;
        const size_t addSize =
            static_cast<size_t>(p[0]) << 24 | static_cast<size_t>(p[1]) << 16 | static_cast<size_t>(p[2]) << 8 | p[3];
        if (4 + addSize > static_cast<size_t>(avPacket.size))
            return true;
        HeldText held{avPacket.stream_index, avPacket.duration,
                      std::vector<uint8_t>(p + 4 + addSize, p + avPacket.size),
                      std::vector<uint8_t>(p + 4, p + 4 + addSize)};
        bool othersPresent = false;
        for (const auto& [idx, t] : m_tracks) othersPresent |= t.trackType != MKV_TRACK_TYPE_SUBTITLE;
        if (!othersPresent)
        {
            writeTextBlock(track, held.data.data(), static_cast<int>(held.data.size()), avPacket.pts, held.duration,
                           held.additions);
            return true;
        }
        m_heldText.emplace(avPacket.pts, std::move(held));
        return true;
    }

    if (track.codecID == CODEC_S_SRT)
    {
        if (avPacket.size <= 0)
            return true;
        bool othersPresent = false;
        for (const auto& [idx, t] : m_tracks) othersPresent |= t.trackType != MKV_TRACK_TYPE_SUBTITLE;
        if (!othersPresent)
        {
            writeTextBlock(track, avPacket.data, avPacket.size, avPacket.pts, avPacket.duration);
            return true;
        }
        m_heldText.emplace(avPacket.pts, HeldText{avPacket.stream_index, avPacket.duration,
                                                  std::vector<uint8_t>(avPacket.data, avPacket.data + avPacket.size)});
        return true;
    }

    if (track.codecID == CODEC_S_PGS && avPacket.size > 0)
    {
        // A composition segment opens the next set, so a set still open without its END is
        // written first. But a segment longer than a packet arrives in pieces, and a piece after
        // the first begins with whatever byte the object data has there. When that byte happened
        // to be 0x16 the set was cut in the middle of its object: two broken blocks, the second
        // at a wrong time. A packet only starts a segment when the bytes collected so far end
        // exactly where a segment ends, so the type byte is believed only then.
        uint8_t lastType = 0;
        if (track.hasPendingFrame && avPacket.data[0] == PGS_SEGMENT_PCS &&
            pgsWholeSegments(track.pendingFrameData, lastType))
            flushPendingFrame(track);  // a set that never got its END
        if (!track.hasPendingFrame)
        {
            track.pendingPts = avPacket.pts;
            track.pendingDts = avPacket.dts;
            track.pendingFlags = avPacket.flags;
            track.hasPendingFrame = true;
        }
        track.pendingFrameData.insert(track.pendingFrameData.end(), avPacket.data, avPacket.data + avPacket.size);
        // A set that can be shown on its own is a random access point, and RFC 9559 (10.4) says a
        // SimpleBlock that is one MUST carry the keyframe flag. The packets of a PGS track never
        // carried it, so no subtitle block was ever marked, and a player seeking into the file had
        // nothing to tell it where a subtitle could be shown from.
        if (pgsSetIsRandomAccess(track.pendingFrameData))
            track.pendingFlags |= AVPacket::IS_IFRAME;
        if (pgsDisplaySetComplete(track.pendingFrameData))
            flushPendingFrame(track);
        return true;
    }

    // If this packet has a different PTS than the pending frame, flush the pending frame first.
    // This handles the case where the MPEG stream reader splits large frames into multiple
    // packets with the same PTS.
    if (track.hasPendingFrame && avPacket.pts != track.pendingPts)
        flushPendingFrame(track);

    // Accumulate data
    if (!track.hasPendingFrame)
    {
        track.pendingPts = avPacket.pts;
        track.pendingDts = avPacket.dts;
        track.pendingFlags = avPacket.flags;
        track.hasPendingFrame = true;
    }
    else
    {
        // Same PTS - merge flags (keep keyframe flag if any chunk has it)
        track.pendingFlags |= (avPacket.flags & AVPacket::IS_IFRAME);
    }
    // The DTS reader marks a frame as one a player can start from by its core, and DTS Express has no core, so not
    // one of its blocks carried the keyframe flag. Every DTS Express frame decodes on its own.
    if (track.matroskaCodecID == MATROSKA_CODEC_ID_AUDIO_DTS_EXPRESS)
        track.pendingFlags |= AVPacket::IS_IFRAME;
    track.pendingFrameData.insert(track.pendingFrameData.end(), avPacket.data, avPacket.data + avPacket.size);

    return true;
}

// ──────────────── doFlush ────────────────────────────────────────────────────

bool MatroskaMuxer::doFlush()
{
    // Flush all pending accumulated frames: the end of the stream, so each one is its track's last
    for (auto& [streamIdx, track] : m_tracks)
    {
        track.finalFlush = true;
        flushPendingFrame(track);
    }

    finishFoldedTracks();
    flushHeldText(INT64_MAX);

    flushCluster();
    return true;
}

// Release every base layer frame still held for its enhancement layer, and say how the merge went.
// doFlush is the usual place, but when some track never delivers a packet the header is first
// written in close(): doFlush then found no folded track at all, and the frames close() replays
// stayed held and were never written. The last frames of a dual layer Dolby Vision or MVC track
// went missing with no message. Called from both, it does its work once there is any.
void MatroskaMuxer::finishFoldedTracks()
{
    bool anyFold = false;
    for (auto& [streamIdx, track] : m_tracks) anyFold |= track.foldElStreamIndex >= 0;
    if (m_foldsFinished || !anyFold)
        return;
    m_foldsFinished = true;

    for (auto& [streamIdx, track] : m_tracks)
        if (track.foldElStreamIndex >= 0)
            drainHeldFrames(track, true);

    // Say plainly how the dual layer merge went. A count of frames that could not be placed is the
    // one number that says the output is not what it claims to be, and it must not be silent: the
    // file would still play, as HDR10, with a Dolby Vision record promising more than it delivers.
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.foldElStreamIndex < 0)
            continue;
        const bool mvc = track.foldKind == MkvTrackInfo::FoldKind::MvcDependent;
        LTRACE(LT_INFO, 2,
               (mvc ? "3D: " : "Dolby Vision: ")
                   << track.elFramesMerged << (mvc ? " dependent view frames" : " enhancement layer frames")
                   << " merged into the base video track.");
        if (track.elFramesUnmatched > 0)
            LTRACE(LT_WARN, 2,
                   (mvc ? "3D: " : "Dolby Vision: ")
                       << track.elFramesUnmatched << (mvc ? " dependent view frames" : " enhancement layer frames")
                       << (mvc ? " could not be matched to a base view picture"
                               : " could not be matched to a base layer picture")
                       << " and were left out.");
    }
}

// ──────────────── Cues ───────────────────────────────────────────────────────

void MatroskaMuxer::writeCues()
{
    if (m_cueEntries.empty())
        return;

    m_cuesPos = m_file.pos() - m_segmentStartPos;

    // In time order (RFC 9559, 22.1); points of several tracks at the same time share one CuePoint, in the order
    // they were made. With subtitles and audio in the cues, the order they were made in is not always the order of
    // time.
    std::stable_sort(m_cueEntries.begin(), m_cueEntries.end(),
                     [](const CueEntry& a, const CueEntry& b) { return a.timecodeMs < b.timecodeMs; });
    std::vector<uint8_t> allPoints;
    for (size_t i = 0; i < m_cueEntries.size();)
    {
        const int64_t timecodeMs = m_cueEntries[i].timecodeMs;
        uint8_t timeBuf[16];
        const int timeLen = ebml_write_uint(timeBuf, MATROSKA_ID_CUETIME, static_cast<uint64_t>(timecodeMs));
        std::vector<uint8_t> point(timeBuf, timeBuf + timeLen);
        for (; i < m_cueEntries.size() && m_cueEntries[i].timecodeMs == timecodeMs; ++i)
        {
            const CueEntry& cue = m_cueEntries[i];
            uint8_t ctpBuf[64];
            int ctpLen = 0;
            ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUETRACK, cue.trackNumber);
            ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUECLUSTERPOSITION, cue.clusterOffset);
            if (cue.relativePosition >= 0)
                ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUERELATIVEPOSITION,
                                          static_cast<uint64_t>(cue.relativePosition));
            if (cue.durationMs >= 0)
                ctpLen +=
                    ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUEDURATION, static_cast<uint64_t>(cue.durationMs));
            uint8_t ctpHeader[16];
            const int ctpHeaderLen = ebml_write_master_open(ctpHeader, MATROSKA_ID_CUETRACKPOSITION, ctpLen);
            point.insert(point.end(), ctpHeader, ctpHeader + ctpHeaderLen);
            point.insert(point.end(), ctpBuf, ctpBuf + ctpLen);
        }
        uint8_t peBuf[16];
        int peLen = ebml_write_id(peBuf, MATROSKA_ID_POINTENTRY);
        peLen += ebml_write_size(peBuf + peLen, point.size());
        allPoints.insert(allPoints.end(), peBuf, peBuf + peLen);
        allPoints.insert(allPoints.end(), point.begin(), point.end());
    }

    // Write Cues master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CUES);
    hdrLen += ebml_write_size(header + hdrLen, allPoints.size() + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();
    writeToFile(allPoints);
    endCrc();
}

// ──────────────── Attachments ────────────────────────────────────────────────

// The name the preserved RPUs are attached under. Deliberately the same layout, and now also the
// same order, that an extracted RPU file uses, so it is worth something outside tsMuxeR.
static const char DV_RPU_ATTACHMENT_NAME[] = "dv-original-rpu.bin";
static const char DV_RPU_PTS_ATTACHMENT_NAME[] = "dv-original-rpu-pts.bin";
// MANIFEST_ATTACHMENT_NAME lives in nalUnits.h, since both readers of it need the same name.

static std::string hexBytes(const uint8_t* data, const int len)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(len) * 2);
    for (int i = 0; i < len; ++i)
    {
        out += digits[(data[i] >> 4) & 0x0F];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

static std::string fourCcToString(const uint32_t fourCc)
{
    std::string out;
    for (int i = 3; i >= 0; --i)
    {
        const char c = static_cast<char>((fourCc >> (i * 8)) & 0xFF);
        out += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return out;
}

// A file that declares profile 8.1 while carrying profile 7 data is a convention of this project's
// own making, and a convention that is not written down is indistinguishable from a mistake. The
// manifest is what keeps it honest: it says what the file really is, in text, so a person and a
// tool can both find out without reverse engineering the video track.
// The start code section, shared by both manifests so the two say the same thing the same way.
static std::string startCodeSection(const std::string& rule, const std::string& trackLines)
{
    if (rule.empty())
        return {};
    return "  start-code            " + rule + "\r\n" + trackLines +
           "\r\n"
           "start-code\r\n"
           "----------\r\n"
           "How the source framed its NALs, which Matroska cannot hold because it stores these\r\n"
           "codecs length prefixed, with no start codes at all. Read as \"length: NAL types\", so\r\n"
           "\"4:35,32,33,34 3:1,39\" means the access unit delimiter and the parameter sets used a\r\n"
           "four byte start code and the slices and SEI used a three byte one. Both forms are legal\r\n"
           "and the coded video is identical either way; recording it lets a disc built from this\r\n"
           "file carry the source's own framing instead of always the four byte form.\r\n"
           "\r\n"
           "A type framed both ways in the same source is not recorded at all, and neither is a\r\n"
           "source that used four bytes throughout, since that is what is written by default.\r\n"
           "\r\n";
}

std::string MatroskaMuxer::buildDvManifest(const uint64_t rpuBytes, const uint32_t rpuCrc, const uint64_t ptsBytes,
                                           const uint32_t ptsCrc, const std::string& scRule) const
{
    // The record the track itself declares, so the manifest can state both sides of the swap.
    std::string declared = "none";
    for (const auto& [streamIdx, track] : m_tracks)
    {
        if (track.foldElStreamIndex >= 0 && track.dvBlockAddIdType != 0)
        {
            declared = fourCcToString(track.dvBlockAddIdType) + " " + hexBytes(track.dvConfig, 24);
            break;
        }
    }
    const std::string original = m_dvProfile7ConfigType != 0
                                     ? fourCcToString(m_dvProfile7ConfigType) + " " + hexBytes(m_dvProfile7Config, 24)
                                     : std::string("none");

    char crcBuf[16];
    snprintf(crcBuf, sizeof(crcBuf), "%08x", rpuCrc);
    char ptsCrcBuf[16];
    snprintf(ptsCrcBuf, sizeof(ptsCrcBuf), "%08x", ptsCrc);

    std::ostringstream s;
    s << "tsMuxeR Dolby Vision carrier\r\n"
         "============================\r\n"
         "\r\n"
         "This file plays as an ordinary single layer Dolby Vision profile 8.1 video, and it also\r\n"
         "carries everything needed to rebuild the dual layer profile 7 disc it was made from.\r\n"
         "\r\n"
         "The usual profile 7 to 8.1 conversion cannot be undone. It drops the enhancement layer and\r\n"
         "rewrites every RPU, and that rewrite is many to one: different originals land on the same\r\n"
         "result, so nothing can tell them apart afterwards. This file keeps the originals instead of\r\n"
         "trying to reconstruct them, which is why it is not smaller than a profile 7 one.\r\n"
         "\r\n";

    s << "  dv-container-version  1\r\n"
      << "  declared-profile      8.1\r\n"
      << "  carried-profile       7\r\n"
      << "  frames                " << m_dvRpuIndex.size() << "\r\n"
      << "  rpu-attachment        " << DV_RPU_ATTACHMENT_NAME << "\r\n"
      << "  rpu-count             " << m_dvRpuIndex.size() << "\r\n"
      << "  rpu-bytes             " << rpuBytes << "\r\n"
      << "  rpu-crc32             " << crcBuf << "\r\n"
      << "  rpu-order             display\r\n"
      << "  rpu-format            start-code\r\n"
      << "  pts-attachment        " << DV_RPU_PTS_ATTACHMENT_NAME << "\r\n"
      << "  pts-bytes             " << ptsBytes << "\r\n"
      << "  pts-crc32             " << ptsCrcBuf << "\r\n"
      << "  pts-format            int64-be-milliseconds\r\n"
      << "  declared-config       " << declared << "\r\n"
      << "  original-config       " << original << "\r\n";
    if (!scRule.empty())
        s << startCodeSection(scRule, m_startCodeTrackLines);
    else
        s << "\r\n";

    s << "What the video track holds\r\n"
         "--------------------------\r\n"
         "  the base layer, unchanged\r\n"
         "  the enhancement layer, every NAL inside an unspecified NAL of type 63, which a decoder\r\n"
         "    is required to skip, so it costs nothing at playback\r\n"
         "  one RPU per picture, converted to profile 8.1\r\n"
         "\r\n"
         "rpu-format: start-code\r\n"
         "----------------------\r\n"
         "Each entry is the four bytes 00 00 00 01 followed by the RPU payload with its two byte NAL\r\n"
         "header removed. That is the layout an extracted RPU file uses.\r\n"
         "\r\n"
         "rpu-order: display\r\n"
         "------------------\r\n"
         "The entries run in presentation order, which is what an extracted RPU file uses. It is NOT\r\n"
         "the order the pictures are stored in: a stream with B pictures is stored in decode order,\r\n"
         "and the two orders part company from the first picture after a key frame onwards. Anything\r\n"
         "matching these entries to pictures has to sort the pictures by presentation time first.\r\n"
         "\r\n"
         "pts-attachment\r\n"
         "--------------\r\n"
         "So that nothing has to work that mapping out, the presentation time of every entry is\r\n"
         "written down beside it, in the same order, as a signed 64 bit big endian count of\r\n"
         "milliseconds from the start of the stream. That is the same value a reader recovers from a\r\n"
         "block, so putting an original back is a lookup rather than a reconstruction. The list is\r\n"
         "sorted, because presentation order is what sorts it.\r\n"
         "\r\n"
         "Rebuilding the disc\r\n"
         "-------------------\r\n"
         "Split the video track: NAL types 62 and 63 go to the enhancement layer, everything else\r\n"
         "goes to the base layer, and the type 63 wrapper is removed. Then put each original RPU back\r\n"
         "in place of the converted one, taking them in presentation order, and restore\r\n"
         "original-config as the track's Dolby Vision record. tsMuxeR does all of this through\r\n"
         "subTrack= on the meta line.\r\n"
         "\r\n"
         "If the count or the checksum does not agree, refuse rather than guess. A mismatch means the\r\n"
         "file has been cut or re-muxed and these entries no longer line up with the pictures, and\r\n"
         "the result would be a disc that looks right and is wrong.\r\n";

    return s.str();
}

// A manifest carrying nothing but the start code rule, for a file that is not a profile 8.1
// carrier. Written only when there IS a rule, so an ordinary file gains no attachment at all.
void MatroskaMuxer::writeStartCodeOnlyManifest(const std::string& scRule)
{
    std::ostringstream s;
    s << "tsMuxeR stream notes\r\n"
         "====================\r\n"
         "\r\n"
         "This file records how its source framed the video, so that a disc built from it can be\r\n"
         "framed the same way. Nothing here changes how the file plays.\r\n"
         "\r\n";
    s << startCodeSection(scRule, m_startCodeTrackLines);
    const std::string manifest = s.str();

    const uint32_t crc = static_cast<uint32_t>(crc32(
        crc32(0, nullptr, 0), reinterpret_cast<const uint8_t*>(manifest.data()), static_cast<uInt>(manifest.size())));

    std::vector<uint8_t> head(256 + strlen(MANIFEST_ATTACHMENT_NAME));
    int p = 0;
    p += ebml_write_string(head.data() + p, MATROSKA_ID_FILEDESCRIPTION,
                           "How the source framed the video, so a disc built from this file matches it.");
    p += ebml_write_string(head.data() + p, MATROSKA_ID_FILENAME, MANIFEST_ATTACHMENT_NAME);
    p += ebml_write_string(head.data() + p, MATROSKA_ID_FILEMIMETYPE, "text/plain");
    p += ebml_write_uint(head.data() + p, MATROSKA_ID_FILEUID, (static_cast<uint64_t>(crc) << 32) | 3);
    head.resize(p);

    uint8_t dataHdr[16];
    int dataHdrLen = ebml_write_id(dataHdr, MATROSKA_ID_FILEDATA);
    dataHdrLen += ebml_write_size(dataHdr + dataHdrLen, manifest.size());

    const uint64_t fileSize = head.size() + dataHdrLen + manifest.size();
    uint8_t fileHdr[16];
    const int fileHdrLen = ebml_write_master_open(fileHdr, MATROSKA_ID_ATTACHEDFILE, fileSize);

    m_attachmentsPos = m_file.pos() - m_segmentStartPos;
    uint8_t header[16];
    const int hdrLen = ebml_write_master_open(header, MATROSKA_ID_ATTACHMENTS,
                                              fileHdrLen + fileSize + sourceAttachmentBytes() + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();
    writeToFile(fileHdr, fileHdrLen);
    writeToFile(head);
    writeToFile(dataHdr, dataHdrLen);
    writeToFile(reinterpret_cast<const uint8_t*>(manifest.data()), static_cast<int>(manifest.size()));
    writeSourceAttachments();
    endCrc();

    LTRACE(LT_INFO, 2,
           "Recorded how the source framed the video (" << scRule
                                                        << "), so a disc built from this file can be framed the "
                                                           "same way rather than always the four byte form.");
}

// Write the preserved original RPUs and the manifest. Only in profile 8.1 mode: a profile 7 file
// carries its originals inline and needs neither.
void MatroskaMuxer::writeAttachments()
{
    // The start code rule of the video track, if the source used anything other than the four byte
    // form this muxer writes. Taken from the merged Dolby Vision track when there is one, since
    // that is the track a disc gets rebuilt from, otherwise from the first video track.
    //
    // That one line served every track of the file, and a reader applied it to every track: with two video tracks
    // framed differently, or H.264 beside HEVC, one of them came back framed the other's way. So each video track's
    // own rule is written as well, on a line of its own, and a reader that finds such lines uses those. The line for
    // the whole file stays, for a reader that knows only that one.
    std::string scRule;
    m_startCodeTrackLines.clear();
    for (const auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType != 1 || track.foldedIntoStream >= 0)
            continue;
        const std::string rule = startCodeRule(track);
        if (!rule.empty())
            m_startCodeTrackLines += "  start-code-track-uid-" + std::to_string(track.trackUID) + "  " + rule + "\r\n";
        if (!rule.empty())
        {
            scRule = rule;
            if (track.foldElStreamIndex >= 0)
                break;  // the merged Dolby Vision track wins outright
        }
    }

    if (!m_dvWriteProfile81 || m_dvRpuIndex.empty())
    {
        // Not a profile 8.1 carrier. There is still something worth recording if the source framed
        // its NALs in a way this muxer would not reproduce, because a disc rebuilt from this file
        // would otherwise come out differing from the source in every start code.
        if (!scRule.empty())
            writeStartCodeOnlyManifest(scRule);
        else if (!m_sourceAttachments.empty())
        {
            m_attachmentsPos = m_file.pos() - m_segmentStartPos;
            uint8_t header[16];
            const int hdrLen =
                ebml_write_master_open(header, MATROSKA_ID_ATTACHMENTS, sourceAttachmentBytes() + CRC_ELEMENT_BYTES);
            writeToFile(header, hdrLen);
            beginCrc();
            writeSourceAttachments();
            endCrc();
        }
        return;
    }

    // Into DISPLAY order. They were collected as the mux walked the pictures, which is decode
    // order; see the note beside m_dvRpuIndex for why the attachment does not keep that order.
    std::vector<const DvRpuEntry*> ordered;
    ordered.reserve(m_dvRpuIndex.size());
    for (const auto& entry : m_dvRpuIndex) ordered.push_back(&entry);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const DvRpuEntry* a, const DvRpuEntry* b) { return a->pts < b->pts; });

    // Each original is tagged with the presentation time of the picture it belongs to, so two
    // entries sharing one means a single picture was given more than one RPU. That is seen when a
    // source stops part way through an access unit: the last enhancement access unit has no
    // picture of its own to pair with and its RPU lands on the previous one.
    //
    // Such a file cannot be put back, because the originals no longer match the pictures one for
    // one, and no ordering of them would fix that. Refusing beats writing a file that claims to be
    // reversible and is not.
    for (size_t i = 1; i < ordered.size(); ++i)
    {
        if (ordered[i]->pts == ordered[i - 1]->pts)
            THROW(ERR_COMMON,
                  "Dolby Vision: one picture carries "
                      << "more than one RPU, so the originals cannot be matched to pictures one for one and "
                         "the profile 8.1 file would not be reversible. This is what a source that ends part "
                         "way through an access unit looks like; re-mux from a complete source, or use "
                         "--dv-profile=7, which carries the disc unchanged.")
    }

    // The index that lets the split put each original back without guessing.
    //
    // The entries are in presentation order; the pictures in the file are in decode order. Rather
    // than make the reader work the mapping out by looking ahead, the timestamp of each entry is
    // written down beside it, and the swap becomes a lookup.
    //
    // IT MUST BE THE TIMESTAMP THE READER WILL SEE, which is NOT the one used to sort. The sort key
    // is the source's own presentation time in internal units; what goes into the file is a
    // millisecond value relative to the start of the stream, and that is what a reader recovers
    // from a block. Writing the sort key here would produce an index that never matches anything.
    std::vector<uint8_t> ptsBlob;
    ptsBlob.reserve(ordered.size() * 8);
    int64_t previousMs = INT64_MIN;
    for (const auto* entry : ordered)
    {
        const int64_t ms = internalToRoundedMs(entry->pts - m_firstTimecode);  // as the block's time
        if (ms == previousMs)
            THROW(ERR_COMMON,
                  "Dolby Vision: two pictures land on the same millisecond, so the original RPUs "
                  "cannot be indexed by timestamp and the profile 8.1 file would not be reversible.")
        previousMs = ms;
        for (int shift = 56; shift >= 0; shift -= 8) ptsBlob.push_back(static_cast<uint8_t>((ms >> shift) & 0xFF));
    }

    static constexpr uint8_t START_CODE[4] = {0, 0, 0, 1};

    uint64_t rpuBytes = 0;
    uint32_t rpuCrc = static_cast<uint32_t>(crc32(0, nullptr, 0));
    for (const auto* entry : ordered)
    {
        rpuBytes += sizeof(START_CODE) + entry->length;
        rpuCrc = static_cast<uint32_t>(crc32(rpuCrc, START_CODE, sizeof(START_CODE)));
        rpuCrc = static_cast<uint32_t>(crc32(rpuCrc, m_dvRpuPayload.data() + entry->offset, entry->length));
    }

    const uint32_t ptsCrc =
        static_cast<uint32_t>(crc32(crc32(0, nullptr, 0), ptsBlob.data(), static_cast<uInt>(ptsBlob.size())));
    const std::string manifest = buildDvManifest(rpuBytes, rpuCrc, ptsBlob.size(), ptsCrc, scRule);
    const uint32_t manifestCrc = static_cast<uint32_t>(crc32(
        crc32(0, nullptr, 0), reinterpret_cast<const uint8_t*>(manifest.data()), static_cast<uInt>(manifest.size())));

    // Derived from the content rather than drawn at random, so muxing the same input twice gives
    // the same file. A UID has to be non-zero, and a count of zero returned earlier.
    const uint64_t rpuUid = (static_cast<uint64_t>(rpuCrc) << 32) | m_dvRpuIndex.size();
    const uint64_t ptsUid = (static_cast<uint64_t>(ptsCrc) << 32) | 2;
    const uint64_t manifestUid = (static_cast<uint64_t>(manifestCrc) << 32) | 1;

    auto attachedFileHead =
        [](const std::string& desc, const std::string& name, const std::string& mime, const uint64_t uid)
    {
        std::vector<uint8_t> buf(desc.size() + name.size() + mime.size() + 96);
        int p = 0;
        p += ebml_write_string(buf.data() + p, MATROSKA_ID_FILEDESCRIPTION, desc);
        p += ebml_write_string(buf.data() + p, MATROSKA_ID_FILENAME, name);
        p += ebml_write_string(buf.data() + p, MATROSKA_ID_FILEMIMETYPE, mime);
        p += ebml_write_uint(buf.data() + p, MATROSKA_ID_FILEUID, uid);
        buf.resize(p);
        return buf;
    };

    const std::vector<uint8_t> rpuHead = attachedFileHead(
        "Original Dolby Vision profile 7 RPUs, one per picture, in presentation order. "
        "See " +
            std::string(MANIFEST_ATTACHMENT_NAME) + ".",
        DV_RPU_ATTACHMENT_NAME, "application/octet-stream", rpuUid);

    // The two small ones travel the same way: built in memory and written after the RPUs.
    struct SmallAttachment
    {
        std::vector<uint8_t> head;
        std::vector<uint8_t> data;
        uint8_t dataHdr[16];
        int dataHdrLen;
        uint64_t fileSize;
        uint8_t fileHdr[16];
        int fileHdrLen;
    };
    std::vector<SmallAttachment> small(2);
    small[0].head = attachedFileHead("Presentation time of each entry in " + std::string(DV_RPU_ATTACHMENT_NAME) +
                                         ", same order, 8 bytes big endian each, in milliseconds.",
                                     DV_RPU_PTS_ATTACHMENT_NAME, "application/octet-stream", ptsUid);
    small[0].data = std::move(ptsBlob);
    small[1].head = attachedFileHead("What this file is and how the original disc is rebuilt from it.",
                                     MANIFEST_ATTACHMENT_NAME, "text/plain", manifestUid);
    small[1].data.assign(manifest.begin(), manifest.end());

    for (auto& s : small)
    {
        s.dataHdrLen = ebml_write_id(s.dataHdr, MATROSKA_ID_FILEDATA);
        s.dataHdrLen += ebml_write_size(s.dataHdr + s.dataHdrLen, s.data.size());
        s.fileSize = s.head.size() + s.dataHdrLen + s.data.size();
        s.fileHdrLen = ebml_write_master_open(s.fileHdr, MATROSKA_ID_ATTACHEDFILE, s.fileSize);
    }

    uint8_t rpuDataHdr[16];
    int rpuDataHdrLen = ebml_write_id(rpuDataHdr, MATROSKA_ID_FILEDATA);
    rpuDataHdrLen += ebml_write_size(rpuDataHdr + rpuDataHdrLen, rpuBytes);

    const uint64_t rpuFileSize = rpuHead.size() + rpuDataHdrLen + rpuBytes;

    uint8_t rpuFileHdr[16];
    const int rpuFileHdrLen = ebml_write_master_open(rpuFileHdr, MATROSKA_ID_ATTACHEDFILE, rpuFileSize);

    uint64_t total = rpuFileHdrLen + rpuFileSize + sourceAttachmentBytes();
    for (const auto& s : small) total += s.fileHdrLen + s.fileSize;

    m_attachmentsPos = m_file.pos() - m_segmentStartPos;

    uint8_t header[16];
    const int hdrLen = ebml_write_master_open(header, MATROSKA_ID_ATTACHMENTS, total + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();

    writeToFile(rpuFileHdr, rpuFileHdrLen);
    writeToFile(rpuHead);
    writeToFile(rpuDataHdr, rpuDataHdrLen);

    // Batched, because a feature has upwards of 160,000 of these and each one would otherwise be
    // two calls into the file.
    std::vector<uint8_t> chunk;
    chunk.reserve(1024 * 1024 + 4096);
    for (const auto* entry : ordered)
    {
        chunk.insert(chunk.end(), START_CODE, START_CODE + sizeof(START_CODE));
        chunk.insert(chunk.end(), m_dvRpuPayload.data() + entry->offset,
                     m_dvRpuPayload.data() + entry->offset + entry->length);
        if (chunk.size() >= 1024 * 1024)
        {
            writeToFile(chunk);
            chunk.clear();
        }
    }
    writeToFile(chunk);

    for (const auto& s : small)
    {
        writeToFile(s.fileHdr, s.fileHdrLen);
        writeToFile(s.head);
        writeToFile(s.dataHdr, s.dataHdrLen);
        writeToFile(s.data);
    }
    writeSourceAttachments();
    endCrc();

    LTRACE(LT_INFO, 2,
           "Dolby Vision: attached " << m_dvRpuIndex.size() << " original profile 7 RPUs (" << rpuBytes
                                     << " bytes, crc32 " << std::hex << rpuCrc << std::dec
                                     << ") in presentation order, with their timestamps and the manifest that "
                                        "explains them. The original disc can be rebuilt from this file.");
}

// What a Matroska source carries for this output. A --title= in the MUXOPT line wins over the source's title.
void MatroskaMuxer::setSourceExtras(const std::string& title, const std::vector<std::vector<uint8_t>>& attachments,
                                    const std::vector<std::vector<uint8_t>>& tags)
{
    if (!m_titleFromMuxOpt)
        m_title = title;
    m_sourceAttachments = attachments;
    m_sourceTags = tags;
}

void MatroskaMuxer::setSourceTrackTags(const std::map<int, std::vector<AbstractDemuxer::SourceTrackTag>>& tags)
{
    m_sourceTrackTags = tags;
}

uint64_t MatroskaMuxer::sourceAttachmentBytes() const
{
    uint64_t total = 0;
    for (const auto& raw : m_sourceAttachments) total += raw.size();
    return total;
}

// The source's attachments, inside the one Attachments element (RFC 9559 allows one), after this file's own.
void MatroskaMuxer::writeSourceAttachments()
{
    for (const auto& raw : m_sourceAttachments) writeToFile(raw);
    if (!m_sourceAttachments.empty())
        LTRACE(LT_INFO, 2, "Matroska: " << m_sourceAttachments.size() << " attachment(s) of the source carried over.");
}

namespace
{
// One EBML number at p, an element ID with its marker kept, a size without; false where it does not end before end.
bool readEbmlNumber(const uint8_t*& p, const uint8_t* end, uint64_t& value, const bool keepMarker)
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

// Tag elements as the source had them, without TagLanguageBCP47, which is not a WebM element: the source's tags
// went into a .webm file with it. Tag and SimpleTag (which may hold SimpleTags) are rebuilt around what is left;
// whatever cannot be read is kept as it is.
std::vector<uint8_t> withoutTagBcp47(const uint8_t* p, const uint8_t* end)
{
    std::vector<uint8_t> out;
    while (p < end)
    {
        const uint8_t* at = p;
        uint64_t id = 0, size = 0;
        if (!readEbmlNumber(p, end, id, true) || !readEbmlNumber(p, end, size, false) ||
            size > static_cast<uint64_t>(end - p))
        {
            out.insert(out.end(), at, end);
            break;
        }
        const uint8_t* body = p;
        p += size;
        if (id == MATROSKA_ID_TAGLANGBCP47)
            continue;
        if (id == MATROSKA_ID_TAG || id == MATROSKA_ID_SIMPLETAG)
        {
            const std::vector<uint8_t> inner = withoutTagBcp47(body, p);
            uint8_t header[16];
            const int headerLen = ebml_write_master_open(header, static_cast<uint32_t>(id), inner.size());
            out.insert(out.end(), header, header + headerLen);
            out.insert(out.end(), inner.begin(), inner.end());
        }
        else
            out.insert(out.end(), at, p);
    }
    return out;
}
}  // namespace

// The source's file-wide tags, before the clusters as RFC 9559 (25.3.1) lays a file out.
void MatroskaMuxer::writeTags()
{
    // The source's tags about one track, each again with the UID the track has in this file: the source's UID names
    // a track that is not here. Everything else in the Tag as the source had it.
    std::vector<std::vector<uint8_t>> trackTags;
    for (const auto& [index, tags] : m_sourceTrackTags)
    {
        const auto it = m_tracks.find(index);
        if (it == m_tracks.end())
            continue;
        for (const auto& t : tags)
        {
            uint8_t uidElement[16];
            const int uidLen = ebml_write_uint(uidElement, MATROSKA_ID_TAGTARGETS_TRACKUID, it->second.trackUID);
            const uint64_t targetsBody = t.targetsRest.size() + uidLen;
            uint8_t targetsHeader[16];
            const int targetsHeaderLen = ebml_write_master_open(targetsHeader, MATROSKA_ID_TAGTARGETS, targetsBody);
            uint8_t tagHeader[16];
            const int tagHeaderLen = ebml_write_master_open(tagHeader, MATROSKA_ID_TAG,
                                                            targetsHeaderLen + targetsBody + t.simpleTags.size());
            std::vector<uint8_t> raw(tagHeader, tagHeader + tagHeaderLen);
            raw.insert(raw.end(), targetsHeader, targetsHeader + targetsHeaderLen);
            raw.insert(raw.end(), t.targetsRest.begin(), t.targetsRest.end());
            raw.insert(raw.end(), uidElement, uidElement + uidLen);
            raw.insert(raw.end(), t.simpleTags.begin(), t.simpleTags.end());
            trackTags.push_back(std::move(raw));
        }
    }
    if (m_sourceTags.empty() && trackTags.empty())
        return;
    std::vector<std::vector<uint8_t>> globalTags = m_sourceTags;
    if (m_webm)
        for (auto* list : {&globalTags, &trackTags})
            for (auto& raw : *list) raw = withoutTagBcp47(raw.data(), raw.data() + raw.size());
    uint64_t total = 0;
    for (const auto& raw : globalTags) total += raw.size();
    for (const auto& raw : trackTags) total += raw.size();
    m_tagsPos = m_file.pos() - m_segmentStartPos;
    uint8_t header[16];
    const int hdrLen = ebml_write_master_open(header, MATROSKA_ID_TAGS, total + CRC_ELEMENT_BYTES);
    writeToFile(header, hdrLen);
    beginCrc();
    for (const auto& raw : globalTags) writeToFile(raw);
    for (const auto& raw : trackTags) writeToFile(raw);
    endCrc();
}

// Everything that closes one output file, once its last block is written: its chapters when the output is split,
// the Cues, the attachments, the SeekHead, and the sizes and the Duration patched in. partEndPts is where the next
// part starts, INT64_MAX for the last or only file.
void MatroskaMuxer::finishPart(const int64_t partEndPts)
{
    flushCluster();
    if (m_partNum > 0)
        writePartChapters(partEndPts);

    // Write Cues
    writeCues();

    // Write the preserved original RPUs and the manifest, in profile 8.1 mode
    writeAttachments();

    // The SeekHead goes into the space reserved at the front, where RFC 9559 (6.3) puts the first one. A copy of it
    // was also written at the end, a second SeekHead the first did not point to and that listed more than clusters,
    // both of which 6.3 rules out. The end now gets it only when it does not fit at the front.
    if (!writeFrontSeekHead())
        writeSeekHead();

    // Patch the Segment size now that we know the total length
    const int64_t segmentEnd = m_file.pos();
    const uint64_t segmentSize = static_cast<uint64_t>(segmentEnd - m_segmentStartPos);
    m_file.seek(m_segmentSizePos);
    uint8_t sizeBuf[8];
    ebml_write_size_fixed(sizeBuf, segmentSize, 8);
    m_file.write(sizeBuf, 8);

    // Patch the Duration element with the end of the track that ends LAST, each track measured by
    // its OWN frames.
    //
    // It used to be the largest block start over all tracks, cut to whole milliseconds, plus one
    // VIDEO frame, whichever track that block belonged to. The last block to start is usually
    // audio, so a 0.833 ms TrueHD access unit was given a 41.7 ms video frame and the file came out
    // 40 ms longer than its tracks really are. An audio only file had nothing added and came out
    // one frame short.
    //
    //   video     its largest timestamp plus one frame: the step its reader advances by and the
    //             DefaultDuration written for it. The largest, not the last written, because video
    //             goes out in decode order
    //   audio     its largest timestamp plus the distance to the one before it. NOT the packet's own
    //             duration: a braided TrueHD reader takes its timestamps from the sample count but
    //             its durations from the AC-3 side, so an access unit there can claim a whole AC-3
    //             frame
    //   subtitle  its largest timestamp. A display set has no length of its own
    //
    // In double and relative to the first timestamp, so nothing is truncated before the end is
    // known and no timeline, however long, can overflow the sum.
    //
    // A file with nothing to measure, such as a single audio frame, gets no Duration at all. The
    // placeholder is 0.0, which is outside the element's range, and the element is optional, so it
    // becomes a Void of the same size rather than staying as a value that is not allowed.
    double durationMs = 0.0;
    for (const auto& [idx, track] : m_tracks)
    {
        if (!track.hasMaxBlockPts)
            continue;
        double endMs = static_cast<double>(track.maxBlockPts - m_firstTimecode) / INTERNAL_PTS_PER_MS;
        const auto simple = dynamic_cast<SimplePacketizerReader*>(track.codecReader);
        if (track.trackType == 1 && track.fps > 0)
            endMs += 1000.0 / track.fps;
        // Opus, Vorbis and ALAC: the length of the last frame itself, which their readers know and which may be
        // shorter than the one before it (a partial ALAC frame, a shorter Opus packet).
        else if (simple &&
                 (track.codecID == CODEC_A_OPUS || track.codecID == CODEC_A_VORBIS || track.codecID == CODEC_A_ALAC) &&
                 simple->frameDurationInternal() > 0)
            endMs += simple->frameDurationInternal() / INTERNAL_PTS_PER_MS;
        else if (track.trackType != MKV_TRACK_TYPE_SUBTITLE && track.hasNextMaxBlockPts)
            endMs += static_cast<double>(track.maxBlockPts - track.nextMaxBlockPts) / INTERNAL_PTS_PER_MS;
        if (track.hasTextEnd)
            endMs = (std::max)(endMs, static_cast<double>(track.maxTextEnd - m_firstTimecode) / INTERNAL_PTS_PER_MS);
        durationMs = std::max(durationMs, endMs);
    }

    // A DD+ 7.1 track carries channels 7 and 8 in a dependent substream, which the reader may meet only after the
    // track header has been written: the header said 6 while the audio decodes as 7.1. The count is corrected here,
    // in place, with the same one byte size, once the whole track has been read.
    for (const auto& [streamIdx, track] : m_tracks)
    {
        const auto at = m_channelsValueFilePos.find(streamIdx);
        const auto ac3 = dynamic_cast<AC3StreamReader*>(track.codecReader);
        if (at == m_channelsValueFilePos.end() || !ac3 || track.matroskaCodecID != MATROSKA_CODEC_ID_AUDIO_EAC3 ||
            !ac3->hasExtChannels() || track.channels != ac3->getChannels())
            continue;
        const auto full = static_cast<uint8_t>(ac3->getChannels() + 2);
        patchHeaderBody(m_tracksBody, m_tracksBodyFilePos, m_tracksCrcFilePos,
                        static_cast<size_t>(at->second - m_tracksBodyFilePos), &full, 1);
    }

    if (m_durationValueFilePos > 0 && !(std::isfinite(durationMs) && durationMs > 0))
    {
        // back over the 2 byte ID and 1 byte size: a Void of the same 11 bytes, written as writeVoid writes one
        uint8_t voidBytes[3 + 8] = {0};
        const int idLen = ebml_write_id(voidBytes, EBML_ID_VOID);
        ebml_write_size_fixed(voidBytes + idLen, static_cast<uint64_t>(3 + 8) - VOID_MIN_BYTES, 2);
        patchHeaderBody(m_infoBody, m_infoBodyFilePos, m_infoCrcFilePos,
                        static_cast<size_t>(m_durationValueFilePos - 3 - m_infoBodyFilePos), voidBytes,
                        sizeof(voidBytes));
    }
    else if (m_durationValueFilePos > 0)
    {
        // Write as big-endian IEEE 754 float64
        uint64_t bits;
        memcpy(&bits, &durationMs, 8);
        uint8_t durationBuf[8];
        for (int i = 7; i >= 0; i--)
        {
            durationBuf[i] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        patchHeaderBody(m_infoBody, m_infoBodyFilePos, m_infoCrcFilePos,
                        static_cast<size_t>(m_durationValueFilePos - m_infoBodyFilePos), durationBuf, 8);
    }

    m_file.close();
}

// The name of part n of a split output, as a transport stream's parts are named: name.split.n.mkv.
std::string MatroskaMuxer::partFileName(const int part) const
{
    const std::string ext = extractFileExt(m_origFileName);
    return m_origFileName.substr(0, m_origFileName.size() - ext.size() - 1) + ".split." + int32ToStr(part) + "." + ext;
}

// Whether a track's key frames may start a part: the main track's, and in an output without video every audio
// track's (m_splitByAnyAudio).
bool MatroskaMuxer::drivesSplit(const MkvTrackInfo& track) const
{
    return track.trackNumber == m_mainTrackNumber || (m_splitByAnyAudio && track.trackType == MKV_TRACK_TYPE_AUDIO);
}

// Whether the block about to be written starts a new part: a key frame of the main track once the part is as large
// or as long as asked, or has reached the next chapter.
bool MatroskaMuxer::splitDue(const int64_t pts)
{
    if (m_splitSize > 0)
        return m_file.pos() + static_cast<int64_t>(m_clusterBuf.size()) > m_splitSize;
    // At the first keyframe at or after the length asked for: a keyframe exactly on it starts the next part.
    if (m_splitDuration > 0)
        return pts - m_firstTimecode >= m_splitDuration;
    bool due = false;
    // Step past every chapter already reached: two marks closer together than the next key frame give one split.
    while (m_nextChapterSplit < m_chapterSplitPts.size() &&
           pts - m_muxStartPts >= m_chapterSplitPts[m_nextChapterSplit])
    {
        due = true;
        m_nextChapterSplit++;
    }
    return due;
}

// Close the part being written and open the next, at origin, which becomes its time zero.
void MatroskaMuxer::startNextPart(MkvTrackInfo& current, const int64_t origin)
{
    // What came before the cut belongs to the part that ends here. Those frames do not start a part themselves: with
    // every audio track driving the split (drivesSplit), the frame of the other track started the next part, whose
    // flush started the next, without end.
    m_startingPart = true;
    for (auto& [streamIdx, track] : m_tracks)
        if (&track != &current)
            flushPendingFrame(track);
    m_startingPart = false;
    flushHeldText(origin);
    finishPart(origin);

    m_partNum++;
    m_fileName = partFileName(m_partNum);
    m_destinationOpen = false;
    m_cueEntries.clear();
    m_clusterOpen = false;
    m_clusterBuf.clear();
    m_clusterDataSize = 0;
    m_clusterBlocks = 0;
    m_cuesPos = 0;
    m_attachmentsPos = 0;
    m_chaptersPos = 0;
    m_tagsPos = 0;
    m_lastAudioCueMs.clear();
    m_channelsValueFilePos.clear();
    for (auto& [streamIdx, track] : m_tracks)
    {
        track.hasMaxBlockPts = false;
        track.hasNextMaxBlockPts = false;
        track.hasTextEnd = false;
    }
    m_firstTimecode = origin;
    m_partOriginPending = true;
    m_partMinRelMs = INT64_MAX;

    openDestination();
    writeSegmentInfo();
    writeTracks();
    writeTags();
}

// A split output's chapters go to the part they fall into, counted from that part's start, written once the part is
// complete and so after its clusters, where the SeekHead points to them.
void MatroskaMuxer::writePartChapters(const int64_t partEndPts)
{
    const double partStart = static_cast<double>(m_firstTimecode - m_muxStartPts) / INTERNAL_PTS_FREQ;
    const double partEnd =
        partEndPts == INT64_MAX ? 1e300 : static_cast<double>(partEndPts - m_muxStartPts) / INTERNAL_PTS_FREQ;
    m_chapters.clear();
    m_chapterInfo.clear();
    for (size_t i = 0; i < m_allChapters.size(); ++i)
    {
        if (m_allChapters[i] < partStart - 0.0005 || m_allChapters[i] >= partEnd - 0.0005)
            continue;
        m_chapters.push_back((std::max)(0.0, m_allChapters[i] - partStart));
        AVChapter info = i < m_allChapterInfo.size() ? m_allChapterInfo[i] : AVChapter();
        if (info.end >= 0)  // counted from the part's start too, and not past the part's end
        {
            const double endSec = (std::min)(static_cast<double>(info.end) / 1e9, partEnd) - partStart;
            info.end = endSec > 0 ? static_cast<int64_t>(endSec * 1e9 + 0.5) : -1;
        }
        m_chapterInfo.push_back(info);
    }
    m_writingPart = true;
    m_partStartSec = partStart;
    m_partEndSec = partEnd;
    writeChapters();
    m_writingPart = false;
}

// ──────────────── SeekHead ───────────────────────────────────────────────────

// Build the complete SeekHead element, header included, so it can be written both at the front of
// the segment, into the reserved area, and at the end where it has always gone.
std::vector<uint8_t> MatroskaMuxer::buildSeekHead() const
{
    struct SeekItem
    {
        uint32_t id;
        int64_t pos;
    };

    std::vector<SeekItem> items;
    items.push_back({MATROSKA_ID_INFO, m_segmentInfoPos});
    items.push_back({MATROSKA_ID_TRACKS, m_tracksPos});
    // Every top-level element (RFC 9559, 6.3). The chapters, written before the clusters, were left out.
    if (m_chaptersPos > 0)
        items.push_back({MATROSKA_ID_CHAPTERS, m_chaptersPos});
    if (m_tagsPos > 0)
        items.push_back({MATROSKA_ID_TAGS, m_tagsPos});
    if (m_cuesPos > 0)
        items.push_back({MATROSKA_ID_CUES, m_cuesPos});
    if (m_attachmentsPos > 0)
        items.push_back({MATROSKA_ID_ATTACHMENTS, m_attachmentsPos});

    std::vector<uint8_t> allEntries;

    for (const auto& item : items)
    {
        uint8_t entryBuf[64];
        int entryLen = 0;

        // SeekID
        uint8_t idBytes[4];
        const int idLen = ebml_write_id(idBytes, item.id);
        entryLen += ebml_write_binary(entryBuf + entryLen, MATROSKA_ID_SEEKID, idBytes, idLen);

        // SeekPosition
        entryLen += ebml_write_uint(entryBuf + entryLen, MATROSKA_ID_SEEKPOSITION, static_cast<uint64_t>(item.pos));

        // SeekEntry master
        uint8_t header[8];
        int hdrLen = ebml_write_id(header, MATROSKA_ID_SEEKENTRY);
        hdrLen += ebml_write_size(header + hdrLen, entryLen);
        allEntries.insert(allEntries.end(), header, header + hdrLen);
        allEntries.insert(allEntries.end(), entryBuf, entryBuf + entryLen);
    }

    uint8_t crcElement[CRC_ELEMENT_BYTES];
    putCrcElement(crcElement,
                  static_cast<uint32_t>(crc32(0L, allEntries.data(), static_cast<uInt>(allEntries.size()))));
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_SEEKHEAD);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size() + CRC_ELEMENT_BYTES);

    std::vector<uint8_t> out(header, header + hdrLen);
    out.insert(out.end(), crcElement, crcElement + CRC_ELEMENT_BYTES);
    out.insert(out.end(), allEntries.begin(), allEntries.end());
    return out;
}

void MatroskaMuxer::writeSeekHead()
{
    const std::vector<uint8_t> seekHead = buildSeekHead();
    writeToFile(seekHead);
}

// Put the SeekHead in the space reserved at the front of the segment. Without it, everything
// written after the clusters is invisible to a reader that does not scan the whole file, and a
// tool that copies the file leaves those elements behind. False when it does not fit.
bool MatroskaMuxer::writeFrontSeekHead()
{
    if (m_seekHeadReservePos <= 0)
        return false;

    const std::vector<uint8_t> seekHead = buildSeekHead();
    if (static_cast<int>(seekHead.size()) + VOID_MIN_BYTES > SEEKHEAD_RESERVE)
    {
        // Never seen in practice: five entries need about 120 bytes against 256 reserved. Saying so
        // beats writing over whatever follows.
        LTRACE(LT_WARN, 2,
               "Matroska: the seek index needs " << seekHead.size() << " bytes but only " << SEEKHEAD_RESERVE
                                                 << " were reserved at the front of the file, so it is written "
                                                    "only at the end. Elements after the clusters may not be "
                                                    "found by other software.");
        return false;
    }

    const int64_t resume = m_file.pos();
    m_file.seek(m_seekHeadReservePos);
    writeToFile(seekHead);
    writeVoid(SEEKHEAD_RESERVE - static_cast<int>(seekHead.size()));
    m_file.seek(resume);
    return true;
}

// ──────────────── close ──────────────────────────────────────────────────────

bool MatroskaMuxer::close()
{
    // If the header was never written (e.g. a track never sent data), force-write
    // it now so the file is at least structurally valid.
    if (!m_headerWritten && !m_tracks.empty())
    {
        writeDeferredHeader();
        replayBufferedPackets();
    }

    // A mux with no tracks at all never reaches writeDeferredHeader, so the destination is still
    // uncreated. It was created at this point before, and everything below writes to it, so it is
    // created here to keep that case exactly as it was. close() runs only after a successful mux,
    // never while an exception is unwinding, so this cannot undo what a refusal protected.
    openDestination();

    // Flush any pending accumulated frames for all tracks: each one's last frame
    for (auto& [streamIdx, track] : m_tracks)
    {
        track.finalFlush = true;
        flushPendingFrame(track);
    }
    finishFoldedTracks();
    flushHeldText(INT64_MAX);

    // Flush any remaining cluster data
    flushCluster();

    finishPart(INT64_MAX);

    // A track has one display size, written from the pixel shape at the stream's start. A stream that changes its
    // shape later was written in silence; where the size came from the stream, that is said now.
    for (const auto& [streamIdx, track] : m_tracks)
        if (const auto mpegReader = dynamic_cast<const MPEGStreamReader*>(track.codecReader);
            mpegReader && track.trackType == MKV_TRACK_TYPE_VIDEO && mpegReader->sampleAspectRatioChanged() &&
            !track.arForced && !(track.srcDisplayWidth && track.srcDisplayHeight))
            LTRACE(LT_WARN, 2,
                   "Warning: the pixel shape of video track "
                       << track.trackNumber << " changes in the stream, from "
                       << doubleToStr(track.sampleAR > 0 ? track.sampleAR : 1.0, 4) << " to "
                       << doubleToStr(
                              mpegReader->laterSampleAspectRatio() > 0 ? mpegReader->laterSampleAspectRatio() : 1.0, 4)
                       << " (width to height). A Matroska track has one display size, and it is written for the "
                          "shape at the start (none for square pixels); a player that follows it shows the later part "
                          "in that shape too. Give ar= to choose the shape.");
    return true;
}
