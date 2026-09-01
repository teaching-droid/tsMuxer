#ifndef AV_CODECS_H_
#define AV_CODECS_H_

#include <types/types.h>
#include <string>

static constexpr int CODEC_ID_NONE = 0;
static constexpr int CODEC_V_MPEG4_H264 = 1;
static constexpr int CODEC_A_AAC = 2;
static constexpr int CODEC_A_AC3 = 3;
static constexpr int CODEC_A_EAC3 = 4;
static constexpr int CODEC_A_DTS = 5;
static constexpr int CODEC_V_VC1 = 6;
static constexpr int CODEC_V_MPEG2 = 7;
static constexpr int CODEC_A_HDAC3 = 8;
static constexpr int CODEC_A_MPEG_AUDIO = 9;
static constexpr int CODEC_A_LPCM = 10;
static constexpr int CODEC_S_SUP = 11;
static constexpr int CODEC_S_PGS = 12;
static constexpr int CODEC_S_SRT = 13;
static constexpr int CODEC_V_MPEG4_H264_DEP = 14;
static constexpr int CODEC_V_MPEG4_H265 = 15;
static constexpr int CODEC_V_MPEG4_H266 = 16;
static constexpr int CODEC_A_MLP = 17;
static constexpr int CODEC_V_AV1 = 18;
static constexpr int CODEC_A_FLAC = 19;
static constexpr int CODEC_A_OPUS = 20;

struct CodecInfo
{
    CodecInfo() : codecID(0) {}
    CodecInfo(const int codecID, const std::string& displayName, const std::string& programName)
    {
        this->codecID = codecID;
        this->displayName = displayName;
        this->programName = programName;
    }
    int codecID;
    std::string displayName;
    std::string programName;
};

struct CheckStreamRez
{
    CheckStreamRez()
        : trackID(0),
          delay(0),
          containerStreamType(0),
          multiSubStream(false),
          subTrack(0),
          isSecondary(false),
          unused(false)
    {
    }
    CodecInfo codecInfo;
    std::string streamDescr;
    // For a merged dual layer Dolby Vision track only: how the ENHANCEMENT layer describes itself.
    // The track is listed as two rows and the two layers are different pictures, so one string
    // cannot describe both. Empty for every other kind of track, and empty when the enhancement
    // layer's own parameter sets could not be read, which leaves the old behaviour in place.
    std::string elStreamDescr;
    std::string lang;
    int32_t trackID;
    int64_t delay;  // auto delay for audio
    // stream_coding_type as declared by a TS/M2TS PMT, 0 for other containers. Kept so that a
    // track no reader accepts can still be named in diagnostics instead of just failing.
    int containerStreamType;

    bool multiSubStream;
    // Which half of a combined track this entry is, as it appears in subTrack= on a meta line.
    // 0 means the entry is not a sub track.
    //
    // It has to be carried rather than derived. For a combined AVC and MVC track the two halves
    // have DIFFERENT codec ids, so the number could be worked out from the codec alone; a merged
    // dual layer Dolby Vision track is two HEVC entries with the SAME id, and the two conventions
    // run OPPOSITE ways round: MVC calls the dependent view 1, Dolby Vision calls the base layer
    // 1. Deriving it would silently swap the layers and author a disc with them reversed.
    int subTrack;
    bool isSecondary;
    bool unused;
};

const static CodecInfo vvcCodecInfo(CODEC_V_MPEG4_H266, "VVC", "V_MPEGI/ISO/VVC");
const static CodecInfo hevcCodecInfo(CODEC_V_MPEG4_H265, "HEVC", "V_MPEGH/ISO/HEVC");
const static CodecInfo h264CodecInfo(CODEC_V_MPEG4_H264, "H.264", "V_MPEG4/ISO/AVC");
const static CodecInfo h264DepCodecInfo(CODEC_V_MPEG4_H264_DEP, "MVC",
                                        "V_MPEG4/ISO/MVC");  // H.264/MVC dependent stream
const static CodecInfo aacCodecInfo(CODEC_A_AAC, "AAC", "A_AAC");
const static CodecInfo mlpCodecInfo(CODEC_A_MLP, "TRUE-HD", "A_MLP");
const static CodecInfo dtsCodecInfo(CODEC_A_DTS, "DTS", "A_DTS");
const static CodecInfo dtshdCodecInfo(CODEC_A_DTS, "DTS-HD", "A_DTS");
const static CodecInfo ac3CodecInfo(CODEC_A_AC3, "AC3", "A_AC3");
const static CodecInfo eac3CodecInfo(CODEC_A_EAC3, "E-AC3 (DD+)", "A_AC3");
const static CodecInfo lpcmCodecInfo(CODEC_A_LPCM, "LPCM", "A_LPCM");
const static CodecInfo trueHDCodecInfo(CODEC_A_HDAC3, "TRUE-HD", "A_AC3");
const static CodecInfo vc1CodecInfo(CODEC_V_VC1, "VC-1", "V_MS/VFW/WVC1");
const static CodecInfo mpeg2CodecInfo(CODEC_V_MPEG2, "MPEG-2", "V_MPEG-2");
const static CodecInfo mpegAudioCodecInfo(CODEC_A_MPEG_AUDIO, "MPEG-Audio", "A_MP3");
const static CodecInfo dvbSubCodecInfo(CODEC_S_SUP, "SUP", "S_SUP");
const static CodecInfo pgsCodecInfo(CODEC_S_PGS, "PGS", "S_HDMV/PGS");
const static CodecInfo srtCodecInfo(CODEC_S_SRT, "SRT", "S_TEXT/UTF8");
const static CodecInfo av1CodecInfo(CODEC_V_AV1, "AV1", "V_AV1");
const static CodecInfo flacCodecInfo(CODEC_A_FLAC, "FLAC", "A_FLAC");
const static CodecInfo opusCodecInfo(CODEC_A_OPUS, "Opus", "A_OPUS");

#endif
