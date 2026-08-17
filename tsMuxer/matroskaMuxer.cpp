#include "matroskaMuxer.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <sstream>

extern "C"
{
#include "zlib.h"
}

#include "aac.h"
#include "aacStreamReader.h"
#include "ac3StreamReader.h"
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
#include "muxerManager.h"
#include "nalUnits.h"
#include "opusStreamReader.h"
#include "simplePacketizerReader.h"
#include "tsMuxer.h"  // HDR10_metadata, filled from the HEVC SEI while parsing
#include "vodCoreException.h"
#include "vvc.h"
#include "vvcStreamReader.h"

// Key for the AC-3 core companion of a TrueHD track (see refreshTrackProperties). Packets are
// routed by real stream index, so the companion is keyed clear of every real one.
static constexpr int AC3_CORE_STREAM_BASE = 0x40000000;

using namespace std;

// INTERNAL_PTS_PER_MS, the divisor from the internal PTS frequency to milliseconds, now lives in
// vod_common.h: the Dolby Vision splitter has to convert the same way round on the way back.

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
      m_lastTimecodeMs(0),
      m_durationValueFilePos(0),
      m_headerWritten(false)
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
    track.matroskaCodecID = codecNameToMatroskaID(codecName, track.codecID);

    // Same parameter map the TS muxer reads the language from (tsMuxer.cpp). An explicit lang=
    // wins; "srclang" is the language muxerManager carried over from the source container.
    if (const auto itr = params.find("lang"); itr != params.end())
        track.language = itr->second;
    else if (const auto src = params.find("srclang"); src != params.end())
        track.language = src->second;

    if (const auto nm = params.find("track-name"); nm != params.end())
        track.name = nm->second;
    // "default" already exists and is documented for Blu-ray; it just never reached Matroska.
    track.markedDefault = params.find("default") != params.end();
    track.dropAc3Core = params.find("drop-ac3-core") != params.end();

    // Generate a random UID
    static std::mt19937_64 rng(std::random_device{}());
    track.trackUID = rng();

    // Determine track type & properties
    if (codecName[0] == 'V')
    {
        track.trackType = 1;  // video
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
            track.bitDepth = lpcmReader->m_bitsPerSample;
    }
    else if (codecName[0] == 'S')
    {
        track.trackType = 17;  // subtitle
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

    // general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
    record.push_back(static_cast<uint8_t>(sps->profile_idc & 0x1F));
    // general_profile_compatibility_flags (4 bytes)
    for (int i = 0; i < 4; i++) record.push_back(0);
    // general_constraint_indicator_flags (6 bytes)
    for (int i = 0; i < 6; i++) record.push_back(0);
    // general_level_idc
    record.push_back(static_cast<uint8_t>(sps->level_idc));
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
    // constantFrameRate(2) | numTemporalLayers(3) | temporalIdNested(1) | lengthSizeMinusOne(2)
    record.push_back(0x0F);  // lengthSizeMinusOne=3

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

std::vector<uint8_t> MatroskaMuxer::buildVVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto vvc = dynamic_cast<VVCStreamReader*>(reader);
    if (!vvc)
        return {};

    // Use raw buffers from VVC stream reader
    // For VVC, the CodecPrivate is typically the raw VPS+SPS+PPS with 4-byte length prefixes
    // But the standard VVCDecoderConfigurationRecord is complex; for now use the raw parameter sets
    // as CodecPrivate data in Annex B format (start codes)
    std::vector<uint8_t> record;

    // Simply concatenate the parameter set NALUs with 4-byte lengths
    // This is what most MKV muxers do for VVC
    if (vvc->m_vpsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_vpsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_vpsBuffer.data(), vvc->m_vpsBuffer.data() + sz);
    }
    if (vvc->m_spsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_spsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_spsBuffer.data(), vvc->m_spsBuffer.data() + sz);
    }
    if (vvc->m_ppsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_ppsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_ppsBuffer.data(), vvc->m_ppsBuffer.data() + sz);
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
    record.push_back(static_cast<uint8_t>((0 << 7) |             // seq_tier_0
                                          (highBitdepth << 6) |  // high_bitdepth
                                          (twelveBit << 5) |     // twelve_bit
                                          (hdr.mono_chrome << 4) | (hdr.chroma_subsampling_x << 3) |
                                          (hdr.chroma_subsampling_y << 2) | (hdr.chroma_sample_position & 0x03)));
    // initial_presentation_delay_present(1) | reserved/initial_presentation_delay_minus_one(3) | padding(4)
    record.push_back(0x00);

    // Note: We only write the 4-byte AV1CodecConfigurationRecord here.
    // The sequence header OBU is included in the first frame's data (as part of
    // the low-overhead OBU stream), so the decoder will pick it up from there.
    // This avoids issues with emulation prevention byte round-tripping.

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildAACConfig(AbstractStreamReader* reader)
{
    const auto aac = dynamic_cast<AACStreamReader*>(reader);
    if (!aac)
        return {};

    // Build a 2-byte AudioSpecificConfig
    // audioObjectType(5 bits) | samplingFrequencyIndex(4 bits) | channelConfiguration(4 bits) | padding(3 bits)
    const uint8_t objectType = aac->m_profile + 1;  // AAC profile is 0-based, objectType is 1-based
    const uint8_t freqIndex = aac->m_sample_rates_index;
    const uint8_t chanConfig = aac->m_channels_index;

    std::vector<uint8_t> config(2);
    config[0] = static_cast<uint8_t>((objectType << 3) | (freqIndex >> 1));
    config[1] = static_cast<uint8_t>(((freqIndex & 1) << 7) | (chanConfig << 3));

    return config;
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
    default:
        // AC3, DTS, LPCM, SRT, PGS, MPEG-2, VC-1 etc. – no codec private needed in MKV
        // (or it's handled differently)
        break;
    }
}

// ──────────────── parseMuxOpt ────────────────────────────────────────────────

void MatroskaMuxer::parseMuxOpt(const std::string& opts)
{
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
        if (!DoviLib::instance().available())
        {
            THROW(ERR_COMMON, "--dv-profile=8.1 needs "
                                  << DoviLib::libraryName() << ", which converts the Dolby Vision metadata, and "
                                  << DoviLib::instance().loadError()
                                  << ". Put it beside tsMuxeR (a prebuilt one is published for 64 bit Windows; other "
                                     "platforms build it from source), or leave --dv-profile at 7.")
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
        m_file.write(data, len);
}

void MatroskaMuxer::writeToFile(const std::vector<uint8_t>& data)
{
    if (!data.empty())
        m_file.write(data.data(), static_cast<int>(data.size()));
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
    pos += ebml_write_string(buf + pos, EBML_ID_DOCTYPE, "matroska");
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

    uint8_t buf[512];
    int pos = 0;
    pos += ebml_write_uint(buf + pos, MATROSKA_ID_TIMECODESCALE, 1000000);  // 1 ms

    // Duration placeholder (patched in close() with actual value)
    const int durationElementStart = pos;
    pos += ebml_write_float(buf + pos, MATROSKA_ID_DURATION, 0.0);

    pos += ebml_write_string(buf + pos, MATROSKA_ID_MUXINGAPP, "tsMuxeR");
    pos += ebml_write_string(buf + pos, MATROSKA_ID_WRITINGAPP, "tsMuxeR");

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_INFO);
    hdrLen += ebml_write_size(header + hdrLen, pos);
    writeToFile(header, hdrLen);

    // MATROSKA_ID_DURATION (0x4489) = 2-byte ID + 1-byte size → float64 at offset +3
    m_durationValueFilePos = m_file.pos() + durationElementStart + 3;

    writeToFile(buf, pos);
}

// ──────────────── Tracks ─────────────────────────────────────────────────────

int MatroskaMuxer::writeColourInfo(uint8_t* dst, const MkvTrackInfo& track)
{
    const bool haveMastering = track.isHdr10 && (HDR10_metadata[3] != 0 || HDR10_metadata[4] != 0);
    if (!track.hasColourDesc && !haveMastering)
        return 0;

    uint8_t body[256];
    int p = 0;
    if (track.hasColourDesc)
    {
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMATRIXCOEFF, track.colourMatrix);
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURTRANSFERCHARACTER, track.colourTransfer);
        p += ebml_write_uint(body + p, MATROSKA_ID_COLOURPRIMARIES, track.colourPrimaries);
    }

    if (haveMastering)
    {
        // MaxCLL and MaxFALL live beside MasteringMetadata, not inside it. Both are written only
        // when non-zero: a stream can legitimately signal zeros, and writing those back says
        // "measured as zero" rather than "unknown", so passing them on would be wrong.
        const unsigned maxCLL = HDR10_metadata[5] >> 16;
        const unsigned maxFALL = HDR10_metadata[5] & 0xffff;
        if (maxCLL)
            p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMAXCLL, maxCLL);
        if (maxFALL)
            p += ebml_write_uint(body + p, MATROSKA_ID_COLOURMAXFALL, maxFALL);

        constexpr double CHROMA_UNIT = 50000.0;  // SEI sends 0.00002 units
        uint8_t mm[192];
        int m = 0;
        const auto x = [](const unsigned packed) { return (packed >> 16) / CHROMA_UNIT; };
        const auto y = [](const unsigned packed) { return (packed & 0xffff) / CHROMA_UNIT; };
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYGCHROMATICITYX, x(HDR10_metadata[0]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYGCHROMATICITYY, y(HDR10_metadata[0]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYBCHROMATICITYX, x(HDR10_metadata[1]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYBCHROMATICITYY, y(HDR10_metadata[1]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYRCHROMATICITYX, x(HDR10_metadata[2]));
        m += ebml_write_float(mm + m, MATROSKA_ID_PRIMARYRCHROMATICITYY, y(HDR10_metadata[2]));
        m += ebml_write_float(mm + m, MATROSKA_ID_WHITEPOINTCHROMATICITYX, x(HDR10_metadata[3]));
        m += ebml_write_float(mm + m, MATROSKA_ID_WHITEPOINTCHROMATICITYY, y(HDR10_metadata[3]));
        // HDR10_metadata[4] packs max in whole cd/m2 (already divided by 10000 on parse) and min
        // still in 0.0001 cd/m2 units.
        m += ebml_write_float(mm + m, MATROSKA_ID_LUMINANCEMAX, static_cast<double>(HDR10_metadata[4] >> 16));
        m += ebml_write_float(mm + m, MATROSKA_ID_LUMINANCEMIN, (HDR10_metadata[4] & 0xffff) / 10000.0);

        p += ebml_write_master_open(body + p, MATROSKA_ID_MASTERINGMETADATA, m);
        memcpy(body + p, mm, m);
        p += m;
    }

    int pos = ebml_write_master_open(dst, MATROSKA_ID_VIDEOCOLOUR, p);
    memcpy(dst + pos, body, p);
    return pos + p;
}

std::vector<uint8_t> MatroskaMuxer::buildTrackEntry(const MkvTrackInfo& track)
{
    // Build inner content of the TrackEntry.
    // Size the buffer dynamically: fixed fields + colour and mastering metadata (~130 bytes on
    // its own, eight 64 bit floats among them) + codec private data + the enhancement layer's
    // configuration record, which is another one of comparable size on a dual layer track.
    const size_t bufSize = 1024 + track.codecPrivate.size() + track.dvElConfig.size();
    std::vector<uint8_t> inner(bufSize);
    int pos = 0;

    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKNUMBER, track.trackNumber);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKUID, track.trackUID);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKTYPE, track.trackType);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKFLAGLACING, 0);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKFLAGDEFAULT, track.isDefault ? 1 : 0);
    if (!track.name.empty())
        pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_TRACKNAME, track.name);
    pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_CODECID, track.matroskaCodecID);

    // Matroska treats a MISSING Language as "eng", so leaving it out does not mean "unknown", it
    // silently claims every untagged track is English. Write "und" instead when we have nothing.
    pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_TRACKLANGUAGE,
                             track.language.empty() ? std::string("und") : track.language);

    if (!track.codecPrivate.empty())
    {
        pos += ebml_write_binary(inner.data() + pos, MATROSKA_ID_CODECPRIVATE, track.codecPrivate.data(),
                                 static_cast<int>(track.codecPrivate.size()));
    }

    if (track.fps > 0)
    {
        const uint64_t durationNs = static_cast<uint64_t>(1000000000.0 / track.fps);
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKDEFAULTDURATION, durationNs);
    }

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
        if (track.interlaced)
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOFLAGINTERLACED, 1);

        // Write DisplayWidth / DisplayHeight for non-square-pixel content
        if (track.streamAR != VideoAspectRatio::AR_KEEP_DEFAULT && track.streamAR != VideoAspectRatio::AR_VGA)
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
            if (displayWidth != track.width || displayHeight != track.height)
            {
                vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYWIDTH, displayWidth);
                vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYHEIGHT, displayHeight);
            }
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

        // Write Video master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKVIDEO, vPos);
        memcpy(inner.data() + pos, videoBuf, vPos);
        pos += vPos;

        // Dolby Vision configuration, as a BlockAdditionMapping beside the Video master. Without
        // it a player has no way to know the track is Dolby Vision, even when the RPU is present
        // in the stream, so the track simply plays as its HDR10 base layer.
        if (track.dvBlockAddIdType)
        {
            uint8_t body[64];
            int b = 0;
            b += ebml_write_uint(body + b, MATROSKA_ID_BLOCKADDIDVALUE, 1);
            b += ebml_write_uint(body + b, MATROSKA_ID_BLOCKADDIDTYPE, track.dvBlockAddIdType);
            b += ebml_write_binary(body + b, MATROSKA_ID_BLOCKADDIDEXTRADATA, track.dvConfig,
                                   static_cast<int>(sizeof(track.dvConfig)));
            pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_MAXBLOCKADDITIONID, 1);
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
                b2 += ebml_write_uint(body2.data() + b2, MATROSKA_ID_BLOCKADDIDVALUE, 1);
                b2 += ebml_write_uint(body2.data() + b2, MATROSKA_ID_BLOCKADDIDTYPE, 0x68766345 /* hvcE */);
                b2 += ebml_write_binary(body2.data() + b2, MATROSKA_ID_BLOCKADDIDEXTRADATA, track.dvElConfig.data(),
                                        static_cast<int>(track.dvElConfig.size()));
                pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_BLOCKADDITIONMAPPING, b2);
                memcpy(inner.data() + pos, body2.data(), b2);
                pos += b2;
            }
        }
    }

    // Audio sub-element
    if (track.trackType == 2 && track.sampleRate > 0)
    {
        uint8_t audioBuf[128];
        int aPos = 0;
        aPos += ebml_write_float(audioBuf + aPos, MATROSKA_ID_AUDIOSAMPLINGFREQ, static_cast<double>(track.sampleRate));
        aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOCHANNELS, track.channels);
        if (track.bitDepth > 0)
            aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOBITDEPTH, track.bitDepth);

        // Write Audio master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKAUDIO, aPos);
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
        if (track.dvMergedIntoStream >= 0)
            continue;
        ordered.push_back(&track);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const MkvTrackInfo* a, const MkvTrackInfo* b) { return a->trackNumber < b->trackNumber; });

    std::vector<uint8_t> allEntries;
    for (const auto* trackPtr : ordered)
    {
        std::vector<uint8_t> entryContent = buildTrackEntry(*trackPtr);

        // Write TrackEntry master header + content
        uint8_t header[16];
        int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKENTRY);
        hdrLen += ebml_write_size(header + hdrLen, entryContent.size());
        allEntries.insert(allEntries.end(), header, header + hdrLen);
        allEntries.insert(allEntries.end(), entryContent.begin(), entryContent.end());
    }

    // Write Tracks master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKS);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size());
    writeToFile(header, hdrLen);
    writeToFile(allEntries);
}

// ──────────────── openDstFile ────────────────────────────────────────────────

void MatroskaMuxer::openDstFile()
{
    m_fileName = m_origFileName;

    if (!m_file.open(m_fileName.c_str(), File::ofWrite))
        THROW(ERR_CANT_CREATE_FILE, "Can't create output file " << m_fileName)

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

    // SegmentInfo and Tracks are deferred to the first muxPacket call,
    // because stream readers haven't parsed their headers yet at this point.
    m_headerWritten = false;
}

void MatroskaMuxer::refreshTrackProperties()
{
    // Resolve exactly one default track per type. A MISSING FlagDefault means 1 in Matroska, so
    // writing nothing made every track claim to be the default, including two audio tracks at
    // once. Honour an explicit "default" where the meta gave one, otherwise the first track of
    // that type, and write the flag on every track so nothing is left to the spec default.
    std::set<uint8_t> typeHasExplicit;
    for (auto& [streamIdx, track] : m_tracks)
        if (track.markedDefault)
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

        if (track.trackType == 1)  // video
        {
            const auto mpegReader = dynamic_cast<MPEGStreamReader*>(track.codecReader);
            if (mpegReader)
            {
                track.width = mpegReader->getStreamWidth();
                track.height = mpegReader->getStreamHeight();
                track.fps = mpegReader->getFPS();
                track.interlaced = mpegReader->getInterlaced();
                track.streamAR = mpegReader->getStreamAR();
                track.hasColourDesc =
                    mpegReader->getColourDesc(track.colourPrimaries, track.colourTransfer, track.colourMatrix);
                // getStreamHDR: 1 SDR, 2 HDR10, 4 Dolby Vision, 16 HDR10+. Everything except
                // plain SDR carries an HDR10 base layer, so mastering metadata applies.
                track.isHdr10 = mpegReader->getStreamHDR() != 1;
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
            const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(track.codecReader);
            if (lpcmReader)
                track.bitDepth = lpcmReader->m_bitsPerSample;
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
        if (!track.dvBlockAddIdType || baseTrack->dvElStreamIndex >= 0)
            continue;

        const auto blReader = dynamic_cast<HEVCStreamReader*>(baseTrack->codecReader);
        const auto elReader = dynamic_cast<HEVCStreamReader*>(track.codecReader);
        if (blReader == nullptr || elReader == nullptr)
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

        baseTrack->dvElStreamIndex = track.streamIndex;
        baseTrack->dvBlockAddIdType = mergedType;
        memcpy(baseTrack->dvConfig, merged, sizeof(merged));
        track.dvMergedIntoStream = baseTrack->streamIndex;
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
            if (track.dvElStreamIndex >= 0)
            {
                folded = true;
                break;
            }
        }
        if (!folded)
            THROW(ERR_COMMON,
                  "--dv-profile=8.1 converts the Dolby Vision metadata of a DUAL LAYER source, and this mux has no "
                  "dual layer Dolby Vision track to convert. A single layer file already carries its picture and its "
                  "RPU in one track, so there is nothing to fold and nothing to convert: leave --dv-profile out and "
                  "the file is written as it is. A dual layer disc needs BOTH of its video streams listed, the base "
                  "layer and the enhancement layer.")
    }

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
        const auto ac3 = dynamic_cast<AC3Codec*>(track.codecReader);
        // isTrueHD means "AC-3 core plus lossless", the disc arrangement, and it is the only case
        // with a core to rescue. A TrueHD track read out of a Matroska file has none, and
        // down-to-ac3 has already turned the whole thing into plain AC-3.
        if (ac3 == nullptr || !ac3->isTrueHD() || ac3->getDownconvertToAC3())
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
        // read onto.
        core.sampleRate = track.sampleRate;
        core.channels = track.channels;
        core.markedDefault = false;
        core.ac3CoreOfStream = streamIdx;
        static std::mt19937_64 rng(std::random_device{}());
        core.trackUID = rng();

        track.ac3CoreStreamIndex = core.streamIndex;
        coreTracks.emplace_back(core.streamIndex, core);

        LTRACE(LT_INFO, 2,
               "TrueHD: keeping the AC-3 compatibility core as its own track. Matroska cannot carry "
               "it inside the lossless track the way a disc does, and dropping it would lose the "
               "only part a Blu-ray needs. Use merge-ac3-track= to put them back on one stream when "
               "authoring a disc from this file, or drop-ac3-core to leave the core out.");
    }
    for (auto& [key, core] : coreTracks) m_tracks[key] = core;

    // Track numbers are handed out as streams are added, so folding one away leaves a gap in the
    // sequence. A gap is legal, a track number is only an identifier, but renumbering costs nothing
    // at this point because no block has been written yet, and it keeps the file ordinary.
    int nextNumber = 1;
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.dvMergedIntoStream >= 0 || track.ac3CoreOfStream >= 0)
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
    // Re-read track properties now that stream readers have parsed their headers
    refreshTrackProperties();

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
        if (track.dvElStreamIndex < 0)
            continue;
        const auto el = m_tracks.find(track.dvElStreamIndex);
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
    writeChapters();

    m_headerWritten = true;
}

// A Blu-ray playlist carries a couple of hundred chapter marks and an MKV source carries its own,
// and until now neither reached Matroska output: the muxer had no chapter support at all, so a
// remux silently dropped every one of them. The Blu-ray path already turns the same list into
// playlist marks (BlurayHelper::createMPLSFile).
void MatroskaMuxer::writeChapters()
{
    if (m_chapters.empty())
        return;

    // ChapterTimeStart is in nanoseconds and is NOT scaled by TimestampScale, unlike block
    // timestamps, so it must not go through the cluster timestamp conversion.
    std::vector<uint8_t> atoms;
    uint64_t uid = 0x1000;
    for (const double startSec : m_chapters)
    {
        if (startSec < 0)
            continue;
        uint8_t atom[64];
        int p = 0;
        p += ebml_write_uint(atom + p, MATROSKA_ID_CHAPTERUID, ++uid);
        p += ebml_write_uint(atom + p, MATROSKA_ID_CHAPTERTIMESTART,
                             static_cast<uint64_t>(startSec * 1000000000.0 + 0.5));
        p += ebml_write_uint(atom + p, MATROSKA_ID_CHAPTERFLAGHIDDEN, 0);

        uint8_t hdr[16];
        int h = ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERATOM, p);
        atoms.insert(atoms.end(), hdr, hdr + h);
        atoms.insert(atoms.end(), atom, atom + p);
    }
    if (atoms.empty())
        return;

    std::vector<uint8_t> edition;
    uint8_t ed[32];
    int e = 0;
    e += ebml_write_uint(ed + e, MATROSKA_ID_EDITIONUID, 0x0EDU);
    e += ebml_write_uint(ed + e, MATROSKA_ID_EDITIONFLAGHIDDEN, 0);
    e += ebml_write_uint(ed + e, MATROSKA_ID_EDITIONFLAGDEFAULT, 1);
    edition.insert(edition.end(), ed, ed + e);
    edition.insert(edition.end(), atoms.begin(), atoms.end());

    uint8_t hdr[16];
    int h = ebml_write_master_open(hdr, MATROSKA_ID_EDITIONENTRY, edition.size());
    std::vector<uint8_t> chapters(hdr, hdr + h);
    chapters.insert(chapters.end(), edition.begin(), edition.end());

    h = ebml_write_master_open(hdr, MATROSKA_ID_CHAPTERS, chapters.size());
    writeToFile(hdr, h);
    writeToFile(chapters.data(), static_cast<int>(chapters.size()));
}

void MatroskaMuxer::replayBufferedPackets()
{
    if (m_preHeaderPackets.empty())
        return;

    // Determine the minimum PTS across all buffered packets so that no track
    // produces negative relative timestamps.
    int64_t minPts = m_preHeaderPackets[0].pts;
    for (const auto& pkt : m_preHeaderPackets) minPts = std::min(minPts, pkt.pts);

    m_firstTimecode = minPts;
    m_firstTimecodeSet = true;

    // Replay all buffered packets through the normal mux path
    for (auto& pkt : m_preHeaderPackets)
    {
        AVPacket tmpPacket;
        tmpPacket.stream_index = pkt.stream_index;
        tmpPacket.pts = pkt.pts;
        tmpPacket.dts = pkt.pts;
        tmpPacket.flags = pkt.flags;
        tmpPacket.data = pkt.data.data();
        tmpPacket.size = static_cast<int>(pkt.data.size());
        muxPacketInternal(tmpPacket);
    }
    m_preHeaderPackets.clear();
    m_preHeaderPackets.shrink_to_fit();
}

// ──────────────── Cluster writing ────────────────────────────────────────────

void MatroskaMuxer::startCluster(int64_t timecodeMs)
{
    if (m_clusterOpen)
        flushCluster();

    m_clusterTimecodeMs = timecodeMs;
    m_clusterBuf.clear();
    m_clusterDataSize = 0;
    m_clusterOpen = true;

    // Record cluster position for cue entries
    m_clusterStartFilePos = m_file.pos() - m_segmentStartPos;

    // Write ClusterTimecode into buffer
    uint8_t buf[16];
    const int len = ebml_write_uint(buf, MATROSKA_ID_CLUSTERTIMECODE, static_cast<uint64_t>(timecodeMs));
    m_clusterBuf.insert(m_clusterBuf.end(), buf, buf + len);
    m_clusterDataSize += len;
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

    // Write Cluster master element with known size
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CLUSTER);
    hdrLen += ebml_write_size(header + hdrLen, m_clusterBuf.size());
    writeToFile(header, hdrLen);
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
    case CODEC_V_MPEG4_H264:
    case CODEC_V_MPEG4_H265:
    case CODEC_V_MPEG4_H266:
        convertedData = convertAnnexBToLengthPrefixed(track, frameData, frameSize);
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
    if (track.dvElStreamIndex >= 0)
    {
        MkvTrackInfo::HeldFrame held;
        held.data.assign(frameData, frameData + frameSize);
        held.pts = track.pendingPts;
        held.flags = track.pendingFlags;
        track.dvHeldFrames.push_back(std::move(held));
        track.pendingFrameData.clear();
        track.hasPendingFrame = false;
        drainHeldFrames(track, false);
        return;
    }

    writeBlock(track, frameData, frameSize, track.pendingPts, track.pendingFlags);

    track.pendingFrameData.clear();
    track.hasPendingFrame = false;
}

// Write one completed frame as a SimpleBlock. Split out of flushPendingFrame so that a dual layer
// Dolby Vision base track, which has to hold its frames back until the matching enhancement layer
// access unit has arrived, can emit them through exactly the same path later.
void MatroskaMuxer::writeBlock(MkvTrackInfo& track, const uint8_t* frameData, int frameSize, int64_t pts,
                               uint8_t pendingFlags)
{
    // Compute PTS relative to stream start (convert internal PTS units to milliseconds)
    const int64_t relMs = (pts - m_firstTimecode) / INTERNAL_PTS_PER_MS;

    // Track the maximum timecode for the Duration element
    if (relMs > m_lastTimecodeMs)
        m_lastTimecodeMs = relMs;

    // Decide whether to start a new cluster
    const bool needNewCluster = !m_clusterOpen || (relMs - m_clusterTimecodeMs >= CLUSTER_MAX_DURATION_MS) ||
                                (m_clusterDataSize >= CLUSTER_MAX_SIZE) ||
                                (track.trackType == 1 && (pendingFlags & AVPacket::IS_IFRAME) && m_clusterOpen &&
                                 (relMs - m_clusterTimecodeMs >= 1000));

    if (needNewCluster)
        startCluster(relMs);

    // Record cue entry for video keyframes
    if (track.trackType == 1 && (pendingFlags & AVPacket::IS_IFRAME))
    {
        CueEntry cue;
        cue.timecodeMs = relMs;
        cue.trackNumber = track.trackNumber;
        cue.clusterOffset = m_clusterStartFilePos;
        m_cueEntries.push_back(cue);
    }

    // Write SimpleBlock
    const int16_t relTimeMs = static_cast<int16_t>(relMs - m_clusterTimecodeMs);

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
    track.anyBlockWritten = true;
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
        track.dvElDone[track.dvElPts] = std::move(track.pendingElData);
        track.pendingElData.clear();
    }

    while (!track.dvHeldFrames.empty())
    {
        MkvTrackInfo::HeldFrame& front = track.dvHeldFrames.front();
        const auto el = track.dvElDone.find(front.pts);
        const bool forced = atEndOfStream || track.dvHeldFrames.size() > MAX_HELD_FRAMES;

        if (el == track.dvElDone.end() && !forced)
            break;  // its enhancement layer may still be on its way

        if (el != track.dvElDone.end())
        {
            // Which picture this enhancement access unit belongs to. In profile 8.1 mode the
            // original RPU is kept aside as it passes, and its timestamp is what later puts the
            // attachment into display order.
            m_dvCurrentRpuPts = front.pts;
            const std::vector<uint8_t> wrapped =
                convertDvElToLengthPrefixed(track, el->second.data(), static_cast<int>(el->second.size()));
            front.data.insert(front.data.end(), wrapped.begin(), wrapped.end());
            track.dvElDone.erase(el);
            track.dvElFramesMerged++;
        }
        else
        {
            track.dvElFramesUnmatched++;
        }

        writeBlock(track, front.data.data(), static_cast<int>(front.data.size()), front.pts, front.flags);
        track.dvHeldFrames.pop_front();
    }

    if (atEndOfStream)
    {
        // Anything still here belongs to pictures that were never delivered.
        track.dvElFramesUnmatched += static_cast<int64_t>(track.dvElDone.size());
        track.dvElDone.clear();
    }
}

bool MatroskaMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
        return true;

    auto it = m_tracks.find(avPacket.stream_index);
    if (it == m_tracks.end())
        return true;

    // Before the header is written, buffer packets and wait until all tracks
    // have delivered at least one packet.  This ensures all codec readers are
    // fully initialized (e.g. audio sample rate, channels) before we write the
    // Matroska track headers.
    if (!m_headerWritten)
    {
        m_seenStreams.insert(avPacket.stream_index);

        // Buffer a copy of this packet
        BufferedPacket bp;
        bp.stream_index = avPacket.stream_index;
        bp.pts = avPacket.pts;
        bp.flags = avPacket.flags;
        bp.data.assign(avPacket.data, avPacket.data + avPacket.size);
        m_preHeaderPackets.push_back(std::move(bp));

        if (m_seenStreams.size() >= m_tracks.size())
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
        const int mlpLength = avPacket.size >= 2 ? ((((avPacket.data[0] & 0x0F) << 8) | avPacket.data[1]) * 2) : 0;
        const bool isAc3Core =
            avPacket.size >= 2 && avPacket.data[0] == 0x0B && avPacket.data[1] == 0x77 && mlpLength != avPacket.size;
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
    if (track.dvMergedIntoStream >= 0)
    {
        const auto base = m_tracks.find(track.dvMergedIntoStream);
        if (base == m_tracks.end())
            return true;
        MkvTrackInfo& bl = base->second;

        // A packet with a new timestamp means the previous access unit is complete. Only then can
        // it be matched, because a large one arrives as several packets.
        if (!bl.pendingElData.empty() && bl.dvElPts != avPacket.pts)
        {
            bl.dvElDone[bl.dvElPts] = std::move(bl.pendingElData);
            bl.pendingElData.clear();
            drainHeldFrames(bl, false);
        }
        bl.dvElPts = avPacket.pts;
        bl.pendingElData.insert(bl.pendingElData.end(), avPacket.data, avPacket.data + avPacket.size);
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
        track.pendingFlags = avPacket.flags;
        track.hasPendingFrame = true;
    }
    else
    {
        // Same PTS - merge flags (keep keyframe flag if any chunk has it)
        track.pendingFlags |= (avPacket.flags & AVPacket::IS_IFRAME);
    }
    track.pendingFrameData.insert(track.pendingFrameData.end(), avPacket.data, avPacket.data + avPacket.size);

    return true;
}

// ──────────────── doFlush ────────────────────────────────────────────────────

bool MatroskaMuxer::doFlush()
{
    // Flush all pending accumulated frames
    for (auto& [streamIdx, track] : m_tracks) flushPendingFrame(track);

    // Then release every base layer frame still held for its enhancement layer.
    for (auto& [streamIdx, track] : m_tracks)
        if (track.dvElStreamIndex >= 0)
            drainHeldFrames(track, true);

    // Say plainly how the dual layer merge went. A count of frames that could not be placed is the
    // one number that says the output is not what it claims to be, and it must not be silent: the
    // file would still play, as HDR10, with a Dolby Vision record promising more than it delivers.
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.dvElStreamIndex < 0)
            continue;
        LTRACE(LT_INFO, 2,
               "Dolby Vision: " << track.dvElFramesMerged << " enhancement layer frames merged into the base "
                                << "video track.");
        if (track.dvElFramesUnmatched > 0)
            LTRACE(LT_WARN, 2,
                   "Dolby Vision: " << track.dvElFramesUnmatched
                                    << " enhancement layer frames could not be matched to a base "
                                       "layer picture and were left out.");
    }

    flushCluster();
    return true;
}

// ──────────────── Cues ───────────────────────────────────────────────────────

void MatroskaMuxer::writeCues()
{
    if (m_cueEntries.empty())
        return;

    m_cuesPos = m_file.pos() - m_segmentStartPos;

    // Build all cue point entries
    std::vector<uint8_t> allPoints;

    for (const auto& cue : m_cueEntries)
    {
        // CueTrackPositions content
        uint8_t ctpBuf[64];
        int ctpLen = 0;
        ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUETRACK, cue.trackNumber);
        ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUECLUSTERPOSITION, cue.clusterOffset);

        // CuePoint content
        uint8_t cpBuf[128];
        int cpLen = 0;
        cpLen += ebml_write_uint(cpBuf + cpLen, MATROSKA_ID_CUETIME, static_cast<uint64_t>(cue.timecodeMs));

        // CueTrackPositions master
        cpLen += ebml_write_master_open(cpBuf + cpLen, MATROSKA_ID_CUETRACKPOSITION, ctpLen);
        memcpy(cpBuf + cpLen, ctpBuf, ctpLen);
        cpLen += ctpLen;

        // PointEntry master
        uint8_t peBuf[8];
        int peLen = ebml_write_id(peBuf, MATROSKA_ID_POINTENTRY);
        peLen += ebml_write_size(peBuf + peLen, cpLen);
        allPoints.insert(allPoints.end(), peBuf, peBuf + peLen);
        allPoints.insert(allPoints.end(), cpBuf, cpBuf + cpLen);
    }

    // Write Cues master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CUES);
    hdrLen += ebml_write_size(header + hdrLen, allPoints.size());
    writeToFile(header, hdrLen);
    writeToFile(allPoints);
}

// ──────────────── Attachments ────────────────────────────────────────────────

// The name the preserved RPUs are attached under. Deliberately the same layout, and now also the
// same order, that an extracted RPU file uses, so it is worth something outside tsMuxeR.
static const char DV_RPU_ATTACHMENT_NAME[] = "dv-original-rpu.bin";
static const char DV_RPU_PTS_ATTACHMENT_NAME[] = "dv-original-rpu-pts.bin";
// DV_MANIFEST_ATTACHMENT_NAME lives in nalUnits.h, since both readers of it need the same name.

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
static std::string startCodeSection(const std::string& rule)
{
    if (rule.empty())
        return {};
    return "  start-code            " + rule +
           "\r\n"
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
        if (track.dvElStreamIndex >= 0 && track.dvBlockAddIdType != 0)
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
        s << startCodeSection(scRule);
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
    s << startCodeSection(scRule);
    const std::string manifest = s.str();

    const uint32_t crc = static_cast<uint32_t>(crc32(
        crc32(0, nullptr, 0), reinterpret_cast<const uint8_t*>(manifest.data()), static_cast<uInt>(manifest.size())));

    std::vector<uint8_t> head(256 + strlen(DV_MANIFEST_ATTACHMENT_NAME));
    int p = 0;
    p += ebml_write_string(head.data() + p, MATROSKA_ID_FILEDESCRIPTION,
                           "How the source framed the video, so a disc built from this file matches it.");
    p += ebml_write_string(head.data() + p, MATROSKA_ID_FILENAME, DV_MANIFEST_ATTACHMENT_NAME);
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
    const int hdrLen = ebml_write_master_open(header, MATROSKA_ID_ATTACHMENTS, fileHdrLen + fileSize);
    writeToFile(header, hdrLen);
    writeToFile(fileHdr, fileHdrLen);
    writeToFile(head);
    writeToFile(dataHdr, dataHdrLen);
    writeToFile(reinterpret_cast<const uint8_t*>(manifest.data()), static_cast<int>(manifest.size()));

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
    std::string scRule;
    for (const auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType != 1 || track.dvMergedIntoStream >= 0)
            continue;
        const std::string rule = startCodeRule(track);
        if (!rule.empty())
        {
            scRule = rule;
            if (track.dvElStreamIndex >= 0)
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
        const int64_t ms = (entry->pts - m_firstTimecode) / INTERNAL_PTS_PER_MS;
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
            std::string(DV_MANIFEST_ATTACHMENT_NAME) + ".",
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
                                     DV_MANIFEST_ATTACHMENT_NAME, "text/plain", manifestUid);
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

    uint64_t total = rpuFileHdrLen + rpuFileSize;
    for (const auto& s : small) total += s.fileHdrLen + s.fileSize;

    m_attachmentsPos = m_file.pos() - m_segmentStartPos;

    uint8_t header[16];
    const int hdrLen = ebml_write_master_open(header, MATROSKA_ID_ATTACHMENTS, total);
    writeToFile(header, hdrLen);

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

    LTRACE(LT_INFO, 2,
           "Dolby Vision: attached " << m_dvRpuIndex.size() << " original profile 7 RPUs (" << rpuBytes
                                     << " bytes, crc32 " << std::hex << rpuCrc << std::dec
                                     << ") in presentation order, with their timestamps and the manifest that "
                                        "explains them. The original disc can be rebuilt from this file.");
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

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_SEEKHEAD);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size());

    std::vector<uint8_t> out(header, header + hdrLen);
    out.insert(out.end(), allEntries.begin(), allEntries.end());
    return out;
}

void MatroskaMuxer::writeSeekHead()
{
    const std::vector<uint8_t> seekHead = buildSeekHead();
    writeToFile(seekHead);
}

// Put the same SeekHead in the space reserved at the front of the segment. Without it, everything
// written after the clusters is invisible to a reader that does not scan the whole file, and a
// tool that copies the file leaves those elements behind.
void MatroskaMuxer::writeFrontSeekHead()
{
    if (m_seekHeadReservePos <= 0)
        return;

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
        return;
    }

    const int64_t resume = m_file.pos();
    m_file.seek(m_seekHeadReservePos);
    writeToFile(seekHead);
    writeVoid(SEEKHEAD_RESERVE - static_cast<int>(seekHead.size()));
    m_file.seek(resume);
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

    // Flush any pending accumulated frames for all tracks
    for (auto& [streamIdx, track] : m_tracks) flushPendingFrame(track);

    // Flush any remaining cluster data
    flushCluster();

    // Write Cues
    writeCues();

    // Write the preserved original RPUs and the manifest, in profile 8.1 mode
    writeAttachments();

    // Write SeekHead at the end, and again into the space reserved at the front so that everything
    // after the clusters can actually be found
    writeSeekHead();
    writeFrontSeekHead();

    // Patch the Segment size now that we know the total length
    const int64_t segmentEnd = m_file.pos();
    const uint64_t segmentSize = static_cast<uint64_t>(segmentEnd - m_segmentStartPos);
    m_file.seek(m_segmentSizePos);
    uint8_t sizeBuf[8];
    ebml_write_size_fixed(sizeBuf, segmentSize, 8);
    m_file.write(sizeBuf, 8);

    // Patch Duration element: highest PTS + one frame duration
    if (m_durationValueFilePos > 0 && m_lastTimecodeMs > 0)
    {
        double frameDurationMs = 0.0;
        for (const auto& [idx, track] : m_tracks)
        {
            if (track.trackType == 1 && track.fps > 0)
            {
                frameDurationMs = 1000.0 / track.fps;
                break;
            }
        }
        const double durationMs = static_cast<double>(m_lastTimecodeMs) + frameDurationMs;

        // Write as big-endian IEEE 754 float64
        uint64_t bits;
        memcpy(&bits, &durationMs, 8);
        uint8_t durationBuf[8];
        for (int i = 7; i >= 0; i--)
        {
            durationBuf[i] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        m_file.seek(m_durationValueFilePos);
        m_file.write(durationBuf, 8);
    }

    m_file.close();
    return true;
}
