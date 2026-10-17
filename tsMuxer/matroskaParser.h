#ifndef MATROSKA_PARSER_H
#define MATROSKA_PARSER_H

// #include "avcodecs.h"
#include <map>
#include <string>
#include <vector>

#include "aac.h"
#include "abstractStreamReader.h"
#include "avPacket.h"
#include "bitmapSubtitles.h"
#include "ioContextDemuxer.h"

typedef IOContextTrackType MatroskaTrackType;
typedef Track MatroskaTrack;

/* EBML version supported */
static constexpr int EBML_VERSION = 1;

/* top-level master-IDs */
static constexpr uint32_t EBML_ID_HEADER = 0x1A45DFA3;

/* IDs in the HEADER master */
static constexpr uint16_t EBML_ID_EBMLVERSION = 0x4286;
static constexpr uint16_t EBML_ID_EBMLREADVERSION = 0x42F7;
static constexpr uint16_t EBML_ID_EBMLMAXIDLENGTH = 0x42F2;
static constexpr uint16_t EBML_ID_EBMLMAXSIZELENGTH = 0x42F3;
static constexpr uint16_t EBML_ID_DOCTYPE = 0x4282;
static constexpr uint16_t EBML_ID_DOCTYPEVERSION = 0x4287;
static constexpr uint16_t EBML_ID_DOCTYPEREADVERSION = 0x4285;

/* IDs in the chapters master */
static constexpr uint16_t MATROSKA_ID_EDITIONENTRY = 0x45B9;
static constexpr uint16_t MATROSKA_ID_CHAPTERATOM = 0xB6;
static constexpr uint16_t MATROSKA_ID_CHAPTERTIMESTART = 0x91;
static constexpr uint16_t MATROSKA_ID_CHAPTERTIMEEND = 0x92;
static constexpr uint16_t MATROSKA_ID_CHAPTERDISPLAY = 0x80;
static constexpr uint16_t MATROSKA_ID_CHAPSTRING = 0x85;
static constexpr uint16_t MATROSKA_ID_CHAPTERLANGUAGE = 0x437C;
static constexpr uint16_t MATROSKA_ID_EDITIONUID = 0x45BC;
static constexpr uint16_t MATROSKA_ID_EDITIONFLAGHIDDEN = 0x45BD;
static constexpr uint16_t MATROSKA_ID_EDITIONFLAGDEFAULT = 0x45DB;
static constexpr uint16_t MATROSKA_ID_CHAPTERUID = 0x73C4;
static constexpr uint16_t MATROSKA_ID_CHAPTERFLAGHIDDEN = 0x98;
static constexpr uint16_t MATROSKA_ID_CHAPLANGUAGEBCP47 = 0x437D;
static constexpr uint16_t MATROSKA_ID_EDITIONFLAGORDERED = 0x45DD;

/* general EBML types */
static constexpr uint8_t EBML_ID_VOID = 0xEC;
static constexpr uint8_t EBML_ID_CRC32 = 0xBF;

/*
 * Matroska element IDs. max. 32-bit.
 */

/* toplevel segment */
static constexpr uint32_t MATROSKA_ID_SEGMENT = 0x18538067;

/* matroska top-level master IDs */
static constexpr uint32_t MATROSKA_ID_INFO = 0x1549A966;
static constexpr uint32_t MATROSKA_ID_TRACKS = 0x1654AE6B;
static constexpr uint32_t MATROSKA_ID_CUES = 0x1C53BB6B;
static constexpr uint32_t MATROSKA_ID_TAGS = 0x1254C367;
static constexpr uint32_t MATROSKA_ID_SEEKHEAD = 0x114D9B74;
static constexpr uint32_t MATROSKA_ID_CLUSTER = 0x1F43B675;
static constexpr uint32_t MATROSKA_ID_CHAPTERS = 0x1043A770;
static constexpr uint32_t MATROSKA_ID_ATTACHMENTS = 0x1941A469;

/* IDs in the attachments master */
static constexpr uint16_t MATROSKA_ID_ATTACHEDFILE = 0x61A7;
static constexpr uint16_t MATROSKA_ID_FILEDESCRIPTION = 0x467E;
static constexpr uint16_t MATROSKA_ID_FILENAME = 0x466E;
static constexpr uint16_t MATROSKA_ID_FILEMIMETYPE = 0x4660;
static constexpr uint16_t MATROSKA_ID_FILEDATA = 0x465C;
static constexpr uint16_t MATROSKA_ID_FILEUID = 0x46AE;

/* IDs in the info master */
static constexpr uint32_t MATROSKA_ID_TIMECODESCALE = 0x2AD7B1;
static constexpr uint16_t MATROSKA_ID_DURATION = 0x4489;
static constexpr uint16_t MATROSKA_ID_TITLE = 0x7BA9;
static constexpr uint16_t MATROSKA_ID_WRITINGAPP = 0x5741;
static constexpr uint16_t MATROSKA_ID_MUXINGAPP = 0x4D80;
static constexpr uint16_t MATROSKA_ID_DATEUTC = 0x4461;
static constexpr uint16_t MATROSKA_ID_SEGMENTUID = 0x73A4;

/* ID in the tracks master */
static constexpr uint8_t MATROSKA_ID_TRACKENTRY = 0xAE;

/* IDs in the trackentry master */
static constexpr uint8_t MATROSKA_ID_TRACKNUMBER = 0xD7;
static constexpr uint16_t MATROSKA_ID_TRACKUID = 0x73C5;
static constexpr uint8_t MATROSKA_ID_TRACKTYPE = 0x83;
static constexpr uint8_t MATROSKA_ID_TRACKAUDIO = 0xE1;
static constexpr uint8_t MATROSKA_ID_TRACKVIDEO = 0xE0;
static constexpr uint8_t MATROSKA_ID_CODECID = 0x86;
static constexpr uint16_t MATROSKA_ID_CODECPRIVATE = 0x63A2;
static constexpr uint16_t MATROSKA_ID_SEEKPREROLL = 0x56BB;
static constexpr uint16_t MATROSKA_ID_CODECDELAY = 0x56AA;
static constexpr uint32_t MATROSKA_ID_CODECNAME = 0x258688;
static constexpr uint32_t MATROSKA_ID_CODECINFOURL = 0x3B4040;
static constexpr uint32_t MATROSKA_ID_CODECDOWNLOADURL = 0x26B240;
static constexpr uint16_t MATROSKA_ID_TRACKNAME = 0x536E;
static constexpr uint32_t MATROSKA_ID_TRACKLANGUAGE = 0x22B59C;
static constexpr uint32_t MATROSKA_ID_TRACKLANGUAGEBCP47 = 0x22B59D;
static constexpr uint8_t MATROSKA_ID_TRACKFLAGENABLED = 0xB9;
static constexpr uint8_t MATROSKA_ID_TRACKFLAGDEFAULT = 0x88;
static constexpr uint8_t MATROSKA_ID_TRACKFLAGLACING = 0x9C;
static constexpr uint16_t MATROSKA_ID_TRACKMINCACHE = 0x6DE7;
static constexpr uint16_t MATROSKA_ID_TRACKMAXCACHE = 0x6DF8;
static constexpr uint32_t MATROSKA_ID_TRACKDEFAULTDURATION = 0x23E383;

static constexpr uint32_t MATROSKA_ID_TRACKCONTENTENCODINGS = 0x6D80;
static constexpr uint32_t MATROSKA_ID_TRACKCONTENTENCODING = 0x6240;
static constexpr uint32_t MATROSKA_ID_ENCODINGCOMPRESSION = 0x5034;
static constexpr uint32_t MATROSKA_ID_ENCODINGCOMPALGO = 0x4254;
static constexpr uint32_t MATROSKA_ID_ENCODINGCOMPSETTINGS = 0x4255;

/* IDs in the trackvideo master */
static constexpr uint32_t MATROSKA_ID_VIDEOFRAMERATE = 0x2383E3;
static constexpr uint16_t MATROSKA_ID_VIDEODISPLAYWIDTH = 0x54B0;
static constexpr uint16_t MATROSKA_ID_VIDEODISPLAYHEIGHT = 0x54BA;
static constexpr uint8_t MATROSKA_ID_VIDEOPIXELWIDTH = 0xB0;
static constexpr uint8_t MATROSKA_ID_VIDEOPIXELHEIGHT = 0xBA;
static constexpr uint16_t MATROSKA_ID_VIDEODISPLAYUNIT = 0x54B2;
static constexpr uint16_t MATROSKA_ID_DISCARDPADDING = 0x75A2;
static constexpr uint16_t MATROSKA_ID_VIDEOPIXELCROPBOTTOM = 0x54AA;
static constexpr uint32_t MATROSKA_ID_TRACKTIMESTAMPSCALE = 0x23314F;
static constexpr uint16_t MATROSKA_ID_VIDEOPIXELCROPTOP = 0x54BB;
static constexpr uint16_t MATROSKA_ID_VIDEOPIXELCROPLEFT = 0x54CC;
static constexpr uint16_t MATROSKA_ID_VIDEOPIXELCROPRIGHT = 0x54DD;
static constexpr uint8_t MATROSKA_ID_VIDEOFIELDORDER = 0x9D;
static constexpr uint16_t MATROSKA_ID_VIDEOPROJECTION = 0x7670;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGFORCED = 0x55AA;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGHEARINGIMPAIRED = 0x55AB;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGVISUALIMPAIRED = 0x55AC;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGTEXTDESCRIPTIONS = 0x55AD;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGORIGINAL = 0x55AE;
static constexpr uint16_t MATROSKA_ID_TRACKFLAGCOMMENTARY = 0x55AF;
static constexpr uint8_t MATROSKA_ID_VIDEOFLAGINTERLACED = 0x9A;
static constexpr uint16_t MATROSKA_ID_VIDEOSTEREOMODE = 0x53B9;
// 0x53B9 above is OldStereoMode, which almost nothing writes any more. The current element
// is 0x53B8, and its values are not the same set: 13 there means both views are carried in
// one block, left eye first, which is what a folded 3D track is.
static constexpr uint16_t MATROSKA_ID_VIDEOSTEREOMODE_CURRENT = 0x53B8;
static constexpr uint64_t MATROSKA_STEREO_BOTH_EYES_LEFT_FIRST = 13;
static constexpr uint64_t MATROSKA_STEREO_BOTH_EYES_RIGHT_FIRST = 14;
static constexpr uint16_t MATROSKA_ID_VIDEOASPECTRATIO = 0x54B3;
static constexpr uint32_t MATROSKA_ID_VIDEOCOLOURSPACE = 0x2EB524;

/* Block addition mapping, used to carry the Dolby Vision configuration records */
static constexpr uint16_t MATROSKA_ID_MAXBLOCKADDITIONID = 0x55EE;
static constexpr uint16_t MATROSKA_ID_BLOCKADDITIONMAPPING = 0x41E4;
static constexpr uint16_t MATROSKA_ID_BLOCKADDIDVALUE = 0x41F0;
static constexpr uint16_t MATROSKA_ID_BLOCKADDIDTYPE = 0x41E7;
static constexpr uint16_t MATROSKA_ID_BLOCKADDIDEXTRADATA = 0x41ED;
/* Data stored beside a block (RFC 9559, 5.1.3.5.2) */
static constexpr uint16_t MATROSKA_ID_BLOCKADDITIONS = 0x75A1;

/* Colour description and HDR mastering metadata, inside the Video master */
static constexpr uint16_t MATROSKA_ID_VIDEOCOLOUR = 0x55B0;
static constexpr uint16_t MATROSKA_ID_COLOURMATRIXCOEFF = 0x55B1;
static constexpr uint16_t MATROSKA_ID_COLOURRANGE = 0x55B9;
static constexpr uint16_t MATROSKA_ID_COLOURTRANSFERCHARACTER = 0x55BA;
static constexpr uint16_t MATROSKA_ID_COLOURPRIMARIES = 0x55BB;
static constexpr uint16_t MATROSKA_ID_COLOURMAXCLL = 0x55BC;
static constexpr uint16_t MATROSKA_ID_COLOURMAXFALL = 0x55BD;
static constexpr uint16_t MATROSKA_ID_MASTERINGMETADATA = 0x55D0;
static constexpr uint16_t MATROSKA_ID_PRIMARYRCHROMATICITYX = 0x55D1;
static constexpr uint16_t MATROSKA_ID_PRIMARYRCHROMATICITYY = 0x55D2;
static constexpr uint16_t MATROSKA_ID_PRIMARYGCHROMATICITYX = 0x55D3;
static constexpr uint16_t MATROSKA_ID_PRIMARYGCHROMATICITYY = 0x55D4;
static constexpr uint16_t MATROSKA_ID_PRIMARYBCHROMATICITYX = 0x55D5;
static constexpr uint16_t MATROSKA_ID_PRIMARYBCHROMATICITYY = 0x55D6;
static constexpr uint16_t MATROSKA_ID_WHITEPOINTCHROMATICITYX = 0x55D7;
static constexpr uint16_t MATROSKA_ID_WHITEPOINTCHROMATICITYY = 0x55D8;
static constexpr uint16_t MATROSKA_ID_LUMINANCEMAX = 0x55D9;
static constexpr uint16_t MATROSKA_ID_LUMINANCEMIN = 0x55DA;

/* IDs in the trackaudio master */
static constexpr uint8_t MATROSKA_ID_AUDIOSAMPLINGFREQ = 0xB5;
static constexpr uint16_t MATROSKA_ID_AUDIOOUTSAMPLINGFREQ = 0x78B5;

static constexpr uint16_t MATROSKA_ID_AUDIOBITDEPTH = 0x6264;
static constexpr uint8_t MATROSKA_ID_AUDIOCHANNELS = 0x9F;

/* ID in the cues master */
static constexpr uint8_t MATROSKA_ID_POINTENTRY = 0xBB;

/* IDs in the pointentry master */
static constexpr uint8_t MATROSKA_ID_CUETIME = 0xB3;
static constexpr uint8_t MATROSKA_ID_CUETRACKPOSITION = 0xB7;

/* IDs in the cuetrackposition master */
static constexpr uint8_t MATROSKA_ID_CUETRACK = 0xF7;
static constexpr uint8_t MATROSKA_ID_CUECLUSTERPOSITION = 0xF1;
static constexpr uint8_t MATROSKA_ID_CUERELATIVEPOSITION = 0xF0;
static constexpr uint8_t MATROSKA_ID_CUEDURATION = 0xB2;

/* IDs in the tags master */
static constexpr uint16_t MATROSKA_ID_TAG = 0x7373;
static constexpr uint16_t MATROSKA_ID_TAGTARGETS = 0x63C0;
static constexpr uint32_t MATROSKA_ID_TAGTARGETS_TYPEVALUE = 0x68CA;
static constexpr uint32_t MATROSKA_ID_TAGTARGETS_TRACKUID = 0x63C5;
static constexpr uint16_t MATROSKA_ID_SIMPLETAG = 0x67C8;
static constexpr uint16_t MATROSKA_ID_TAGNAME = 0x45A3;
static constexpr uint16_t MATROSKA_ID_TAGSTRING = 0x4487;
static constexpr uint16_t MATROSKA_ID_TAGLANG = 0x447A;
static constexpr uint16_t MATROSKA_ID_TAGLANGBCP47 = 0x447B;

/* IDs in the seekhead master */
static constexpr uint16_t MATROSKA_ID_SEEKENTRY = 0x4DBB;

/* IDs in the seekpoint master */
static constexpr uint16_t MATROSKA_ID_SEEKID = 0x53AB;
static constexpr uint16_t MATROSKA_ID_SEEKPOSITION = 0x53AC;

/* IDs in the cluster master */
static constexpr uint8_t MATROSKA_ID_CLUSTERTIMECODE = 0xE7;
static constexpr uint8_t MATROSKA_ID_BLOCKGROUP = 0xA0;
static constexpr uint8_t MATROSKA_ID_SIMPLEBLOCK = 0xA3;

/* IDs in the blockgroup master */
static constexpr uint8_t MATROSKA_ID_BLOCK = 0xA1;
static constexpr uint8_t MATROSKA_ID_BLOCKDURATION = 0x9B;
static constexpr uint8_t MATROSKA_ID_BLOCKREFERENCE = 0xFB;

enum class MatroskaEyeMode
{
    MONO = 0x0,
    RIGHT = 0x1,
    LEFT = 0x2,
    BOTH = 0x3
};

enum class MatroskaAspectRatioMode
{
    FREE = 0x0,
    KEEP = 0x1,
    FIXED = 0x2
};

/*
 * These aren't in any way "matroska-form" things,
 * it's just something I use in the muxer/demuxer.
 */

typedef enum
{
    MATROSKA_TRACK_ENABLED = (1 << 0),
    MATROSKA_TRACK_DEFAULT = (1 << 1),
    MATROSKA_TRACK_LACING = (1 << 2),
    MATROSKA_TRACK_REAL_V = (1 << 4),
    MATROSKA_TRACK_SHIFT = (1 << 16)
} MatroskaTrackFlags;

typedef enum
{
    MATROSKA_VIDEOTRACK_INTERLACED = (MATROSKA_TRACK_SHIFT << 0)
} MatroskaVideoTrackFlags;

/*
 * Matroska Codec IDs. Strings.
 */

struct CodecTags
{
    const char* str;
    int id;
};

constexpr auto MATROSKA_CODEC_ID_VVC_FOURCC = "V_MPEGI/ISO/VVC";
constexpr auto MATROSKA_CODEC_ID_HEVC_FOURCC = "V_MPEGH/ISO/HEVC";
constexpr auto MATROSKA_CODEC_ID_AVC_FOURCC = "V_MPEG4/ISO/AVC";
constexpr auto MATROSKA_CODEC_ID_VIDEO_VFW_FOURCC = "V_MS/VFW/FOURCC";
constexpr auto MATROSKA_CODEC_ID_VIDEO_VC1 = "V_VC1";
constexpr auto MATROSKA_CODEC_ID_AUDIO_ACM = "A_MS/ACM";
constexpr auto MATROSKA_CODEC_ID_AUDIO_AC3 = "A_AC3";
constexpr auto MATROSKA_CODEC_ID_SRT = "S_TEXT/UTF8";
constexpr auto MATROSKA_CODEC_ID_TEXT_ASCII = "S_TEXT/ASCII";
constexpr auto MATROSKA_CODEC_ID_TEXT_ASS = "S_TEXT/ASS";
constexpr auto MATROSKA_CODEC_ID_TEXT_SSA = "S_TEXT/SSA";
constexpr auto MATROSKA_CODEC_ID_TEXT_WEBVTT = "S_TEXT/WEBVTT";
constexpr auto MATROSKA_CODEC_ID_WEBM_WEBVTT = "D_WEBVTT/SUBTITLES";
// WebM stores WebVTT as D_WEBVTT/ and its kind: SUBTITLES, CAPTIONS, DESCRIPTIONS or METADATA, all in the same form. Only
// SUBTITLES was recognised, so captions and descriptions were not read at all. METADATA is not text to be shown and is
// still not read.
inline bool isWebmVttText(const std::string& codecId)
{
    return codecId == "D_WEBVTT/SUBTITLES" || codecId == "D_WEBVTT/CAPTIONS" || codecId == "D_WEBVTT/DESCRIPTIONS";
}
constexpr auto MATROSKA_CODEC_ID_AUDIO_AAC = "A_AAC";
constexpr auto MATROSKA_CODEC_ID_AUDIO_PCM_BIG = "A_PCM/INT/BIG";
constexpr auto MATROSKA_CODEC_ID_AUDIO_PCM_LIT = "A_PCM/INT/LIT";
constexpr auto MATROSKA_CODEC_ID_SUBTITLE_PGS = "S_HDMV/PGS";
constexpr auto MATROSKA_CODEC_ID_SUBTITLE_VOBSUB = "S_VOBSUB";
constexpr auto MATROSKA_CODEC_ID_SUBTITLE_DVB = "S_DVBSUB";
constexpr auto MATROSKA_CODEC_ID_AV1 = "V_AV1";
constexpr auto MATROSKA_CODEC_ID_VP8 = "V_VP8";
constexpr auto MATROSKA_CODEC_ID_VP9 = "V_VP9";
constexpr auto MATROSKA_CODEC_ID_AUDIO_VORBIS = "A_VORBIS";
constexpr auto MATROSKA_CODEC_ID_AUDIO_ALAC = "A_ALAC";
constexpr auto MATROSKA_CODEC_ID_AUDIO_DTS = "A_DTS";
constexpr auto MATROSKA_CODEC_ID_AUDIO_EAC3 = "A_EAC3";
constexpr auto MATROSKA_CODEC_ID_AUDIO_TRUEHD = "A_TRUEHD";
constexpr auto MATROSKA_CODEC_ID_AUDIO_MPEG_L3 = "A_MPEG/L3";
constexpr auto MATROSKA_CODEC_ID_VIDEO_MPEG2 = "V_MPEG2";
constexpr auto MATROSKA_CODEC_ID_VIDEO_MPEG1 = "V_MPEG1";
constexpr auto MATROSKA_CODEC_ID_AUDIO_MLP = "A_MLP";
constexpr auto MATROSKA_CODEC_ID_AUDIO_DTS_EXPRESS = "A_DTS/EXPRESS";
constexpr auto MATROSKA_CODEC_ID_AUDIO_FLAC = "A_FLAC";
constexpr auto MATROSKA_CODEC_ID_AUDIO_OPUS = "A_OPUS";

/* max. depth in the EBML tree structure */
constexpr auto EBML_MAX_DEPTH = 16;

// extern CodecTags ff_mkv_codec_tags[];

class ParsedH264TrackData : public ParsedTrackPrivData
{
   public:
    ParsedH264TrackData(uint8_t* buff, int size);
    ~ParsedH264TrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    void setStartCodeRule(const std::string& rule) override;

   protected:
    uint8_t m_nalSize;
    bool m_firstExtract;

    std::vector<std::vector<uint8_t>> m_spsPpsList;
    // NAL type to 3 or 4. Empty means four throughout, which is what this has always written.
    std::map<int, int> m_startCodeByType;

    void writeNalHeader(uint8_t*& dst, int nalType) const;
    // The NAL type sits in a different place in each codec, and it is what the rule is keyed on.
    [[nodiscard]] virtual int nalTypeOf(const uint8_t* nal, int size) const;
    [[nodiscard]] size_t getSPSPPSLen() const;
    int writeSPSPPS(uint8_t* dst) const;
    virtual bool spsppsExists(uint8_t* buff, int size);
    bool nextNal(const uint8_t* curPos, const uint8_t* end, uint32_t& elSize) const;
};

class ParsedH265TrackData final : public ParsedH264TrackData
{
   public:
    ParsedH265TrackData(const uint8_t* buff, int size);
    ~ParsedH265TrackData() override = default;

    bool spsppsExists(uint8_t* buff, int size) override;

   protected:
    [[nodiscard]] int nalTypeOf(const uint8_t* nal, int size) const override;
};

class ParsedH266TrackData final : public ParsedH264TrackData
{
   public:
    ParsedH266TrackData(const uint8_t* buff, int size);
    ~ParsedH266TrackData() override = default;

    bool spsppsExists(uint8_t* buff, int size) override;

   protected:
    [[nodiscard]] int nalTypeOf(const uint8_t* nal, int size) const override;
};

class ParsedAV1TrackData final : public ParsedTrackPrivData
{
   public:
    ParsedAV1TrackData(const uint8_t* buff, int size);
    ~ParsedAV1TrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;

   private:
    std::vector<std::vector<uint8_t>> m_configOBUs;  // start-code-prefixed config OBUs
    bool m_firstExtract;
};

class ParsedAC3TrackData final : public ParsedTrackPrivData
{
   public:
    ParsedAC3TrackData(uint8_t* buff, int size);
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    ~ParsedAC3TrackData() override = default;

   private:
    bool m_firstPacket;
    bool m_shortHeaderMode;
};

class ParsedAACTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedAACTrackData(uint8_t* buff, int size);
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    ~ParsedAACTrackData() override = default;

   private:
    AACCodec m_aacRaw;
};

class ParsedLPCMTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedLPCMTrackData(MatroskaTrack* track);
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    ~ParsedLPCMTrackData() override = default;

   private:
    bool m_convertBytes;
    uint16_t m_bitdepth;
    uint16_t m_channels;
    MemoryBlock m_waveBuffer;
};

// The text of a WebVTT cue as SRT text: <i>, <b> and <u> kept, the other tags dropped with their text kept, the
// character references turned into their characters (Matroska S_TEXT/WEBVTT, and WebVTT in an MP4 file)
std::string webVttToSrtText(const std::string& cue);
// The text of an ASS or SSA event block as SRT text, its styles and override codes but italic, bold and underline left
// out
std::string assToSrtText(const std::string& event);

class ParsedSRTTrackData final : public ParsedTrackPrivData
{
   public:
    // The text subtitle formats of Matroska, all handed on as SRT: S_TEXT/UTF8 as it is, and the others turned into
    // its text, which keeps italic, bold and underline and drops what SRT cannot carry.
    enum class Form
    {
        Srt,     // S_TEXT/UTF8, S_TEXT/ASCII
        Ass,     // S_TEXT/ASS, S_TEXT/SSA
        WebVtt,  // S_TEXT/WEBVTT: the cue text
        WebmVtt  // D_WEBVTT/SUBTITLES: the cue identifier and settings lines first
    };
    ParsedSRTTrackData(uint8_t* buff, int size, Form form = Form::Srt);
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    ~ParsedSRTTrackData() override = default;

   private:
    int m_packetCnt;
    Form m_form;
};

class ParsedVC1TrackData final : public ParsedTrackPrivData
{
   public:
    // decSpecStruc: the CodecPrivate is a VC1DecSpecStruc (V_VC1) rather than a bitmap header (V_MS/VFW/FOURCC).
    ParsedVC1TrackData(uint8_t* buff, int size, bool decSpecStruc = false);
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    ~ParsedVC1TrackData() override = default;

   private:
    std::vector<uint8_t> m_seqHeader;
    bool m_firstPacket;
};

class ParsedFLACTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedFLACTrackData(uint8_t* buff, int size);
    ~ParsedFLACTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;

   private:
    bool m_firstPacket;
    std::vector<uint8_t> m_codecPrivate;
};

// VP8 and VP9: each block is one frame and has no framing of its own. It is framed as VpxStreamReader reads it, a
// start code, the frame escaped as in H.264, and a stop byte 0x80.
class ParsedVpxTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedVpxTrackData() = default;
    ~ParsedVpxTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
};

// Vorbis and ALAC: each packet gets a 4 byte big endian length in front, and the first one the track's CodecPrivate in
// front of that, behind an 8 character mark ("VORBISCP", "ALACCPRV") and its length. Their readers read both.
class ParsedMarkedTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedMarkedTrackData(const char* mark, const uint8_t* buff, int size);
    ~ParsedMarkedTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;

   private:
    char m_mark[8];
    std::vector<uint8_t> m_codecPrivate;
    bool m_firstPacket = true;
};

// A track copied as it is into a Matroska output (MatroskaCopyStreamReader). Each block becomes a record that carries
// what the copy needs and the reader cannot find again: "MKCP", the time and the duration (8 bytes each, big endian,
// internal clock, the duration -1 when the block gives none), the keyframe flag (1 byte), the BlockAdditions body
// (4 byte length, then the bytes) and the block itself (4 byte length, then the bytes).
class ParsedCopyTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedCopyTrackData() = default;
    ~ParsedCopyTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    void setBlockAdditions(const uint8_t* body, const int size) override { m_additions.assign(body, body + size); }

   private:
    std::vector<uint8_t> m_additions;
};

class ParsedOpusTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedOpusTrackData(uint8_t* buff, int size);
    ~ParsedOpusTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;

   private:
    bool m_firstPacket;
    std::vector<uint8_t> m_codecPrivate;
};

// A bitmap subtitle track of another format, DVD (S_VOBSUB) or DVB (S_DVBSUB), its pictures redrawn as PGS display
// sets with their "PG" headers, read on as a PGS track.
class ParsedBitmapSubTrackData final : public ParsedTrackPrivData
{
   public:
    explicit ParsedBitmapSubTrackData(BitmapSubDecoder* decoder) : m_decoder(decoder) {}
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
    bool finish(AVPacket* pkt) override;
    [[nodiscard]] bool rateGuessed() const { return m_decoder->rateGuessed(); }

   private:
    std::unique_ptr<BitmapSubDecoder> m_decoder;
};

class ParsedPGTrackData final : public ParsedTrackPrivData
{
   public:
    ParsedPGTrackData() = default;
    ~ParsedPGTrackData() override = default;
    void extractData(AVPacket* pkt, uint8_t* buff, int size) override;
};

typedef struct MatroskaVideoTrack : MatroskaTrack
{
    int pixel_width;
    int pixel_height;
    int display_width;
    int display_height;

    uint32_t fourcc;

    MatroskaAspectRatioMode ar_mode;
    MatroskaEyeMode eye_mode;

    //..
} MatroskaVideoTrack;

typedef struct MatroskaAudioTrack : MatroskaTrack
{
    uint16_t channels;
    uint16_t bitdepth;
    int internal_samplerate;
    int samplerate;
    int block_align;

    /* real audio header */
    int coded_framesize;
    int sub_packet_h;
    int frame_size;
    int sub_packet_size;
    int sub_packet_cnt;
    int pkt_cnt;
    uint8_t* buf;
    //..
} MatroskaAudioTrack;

typedef struct MatroskaSubtitleTrack : MatroskaTrack
{
    int ass;
    //..
} MatroskaSubtitleTrack;

#endif  // MATROSKA_PARSER_H
