#ifndef VOD_COMMON_H_
#define VOD_COMMON_H_

#include <types/types.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if 1
extern bool sLastMsg;
#define LTRACE(level, errIndex, msg)               \
    do                                             \
    {                                              \
        {                                          \
            if ((errIndex) & 2)                    \
            {                                      \
                if ((level) <= LT_WARN)            \
                    std::cerr << msg << std::endl; \
                else if ((level) == LT_INFO)       \
                    std::cout << msg << std::endl; \
                if ((level) <= LT_INFO)            \
                    sLastMsg = true;               \
            }                                      \
        }                                          \
    } while (0)

class Process
{
   public:
    static void sleep(const int millisec) { std::this_thread::sleep_for(std::chrono::milliseconds(millisec)); }
};

#endif

#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define bswap_32(x) my_ntohl(x)

static constexpr unsigned DETECT_STREAM_BUFFER_SIZE = 1024 * 1024 * 64;
static constexpr unsigned TS_PID_NULL = 8191;
static constexpr unsigned TS_PID_PAT = 0;
static constexpr unsigned TS_PID_PMT = 1;
static constexpr unsigned TS_PID_CAS = 2;

static constexpr unsigned LT_ERR_COMMON = 0;
static constexpr unsigned LT_ERR_MPEG = 1;
static constexpr unsigned LT_MUXER = 2;
static constexpr unsigned LT_TRAFFIC = 3;

constexpr int TS_FRAME_SIZE = 188;

constexpr int DEFAULT_FILE_BLOCK_SIZE = 1024 * 1024 * 2;
constexpr int TS188_ROUND_BLOCK_SIZE = DEFAULT_FILE_BLOCK_SIZE / TS_FRAME_SIZE * TS_FRAME_SIZE;

constexpr unsigned PCR_FREQUENCY = 90000;

static constexpr int64_t INTERNAL_PTS_FREQ = 196 * 27000000ll;
static constexpr int64_t INT_FREQ_TO_TS_FREQ = INTERNAL_PTS_FREQ / PCR_FREQUENCY;
static constexpr int64_t INTERNAL_PTS_PER_MS = INTERNAL_PTS_FREQ / 1000;

// "this packet has no timestamp". It was private to the Matroska reader; it is here because a
// filter downstream now has to be able to tell an absent timestamp from a real one.
static constexpr int64_t AV_NOPTS_VALUE_INTERNAL = 0x8000000000000000LL;

static constexpr uint32_t GOP_BUFFER_SIZE = 2 * 1024 * 1024;  // 512*1024;

static constexpr int CANT_CREATE_INDEX_FILE = -1;
static constexpr int TOO_RARE_PCR = -2;
static constexpr int MULTIPLE_PMT_NOT_SUPPORTED = -3;
static constexpr int INVALID_PAT = -4;
static constexpr int MPEG_INTERNAL_ERROR = -5;
static constexpr int CANT_CREATE_ASSET_FILE = -6;
static constexpr int ERR_ENCODING_ALREADY_STARTED = -7;
static constexpr int CANT_FIND_VIDEO_DECODER = -8;
static constexpr int INVALID_TS_FRAME_SYNC_CODE = -9;
static constexpr int NOT_ENOUGH_BUFFER = -10;
static constexpr int INVALID_BITSTREAM_SYNTAX = -11;
// A unit (a parameter set, a slice header) that ends before its fields do: damaged or cut, not waiting for more data
static constexpr int UNIT_DAMAGED = -12;
static constexpr int MAX_ERROR = -100;

bool isFillerNullPacket(uint8_t* curBuf);

std::string unquoteStr(const std::string& val);
std::string quoteStr(const std::string& val);

std::vector<std::string> extractFileList(const std::string& val);

struct AVRational
{
    int num;  ///< numerator
    int den;  ///< denominator
    AVRational() { num = den = 0; }
    AVRational(const int _num, const int _den)
    {
        num = _num;
        den = _den;
    }
};

typedef std::vector<std::pair<int, int>> PriorityDataInfo;  // mark some data as priority data

enum class DiskType
{
    NONE,
    BLURAY,
    AVCHD
};

// Usable byte capacities of the burnable Blu-ray media classes (BDA/ECMA logical volume sizes).
// These are the real recordable ("BD-R") capacities, not marketing decimal-GB (25 GB BD-R holds
// 25,025,314,816 bytes, not 25,000,000,000). Used by the --disc-size fit-to-disc guard.
constexpr int64_t BD25_CAPACITY = 25025314816LL;    // BD-R  SL  (single layer,  "25 GB")
constexpr int64_t BD50_CAPACITY = 50050629632LL;    // BD-R  DL  (dual layer,    "50 GB")
constexpr int64_t BD100_CAPACITY = 100103356416LL;  // BD-R XL  (triple layer,  "100 GB")
constexpr int64_t BD128_CAPACITY = 128001769472LL;  // BD-R XL  (quad layer,    "128 GB")

uint16_t AV_RB16(const uint8_t* buffer);
uint32_t AV_RB24(const uint8_t* buffer);
uint32_t AV_RB32(uint8_t* buffer);
void AV_WB16(uint8_t* buffer, uint16_t value);
void AV_WB24(uint8_t* buffer, uint32_t value);
void AV_WB32(uint8_t* buffer, uint32_t value);
void AV_WB64(uint8_t* buffer, uint64_t value);

std::string floatToTime(double time, char msSeparator = '.');
double timeToFloat(const std::string& chapterStr);
std::string toNativeSeparators(const std::string& dirName);
double correctFps(double fps);

// Audio delay carried in a file name, using the convention eac3to established:
// "<name> DELAY -17ms.<ext>". A raw elementary stream has nowhere else to keep the offset a
// track had against the video in its container.
// parseTimeshiftToMs accepts the same forms as the meta timeshift parameter (ms, s, ns).
// delayFromFileName returns 0 when there is no token; it matches the LAST one so that a
// stale token earlier in the name cannot win over the current one.
int64_t parseTimeshiftToMs(const std::string& value);

// A time written as a number and a unit (ns, us, ms, s, min or h, in any case), such as 90s or 1.5min, or as a
// clock time, such as 1:30, 00:01:30 or 00:01:30.500. A sign is allowed in front. defaultUnit, when given, is the unit
// of a number written without one; without it a unit is required. Returns false for anything else, a decimal comma
// included (1,5s could be meant as 1.5 s or as 15 s), and for more than 100,000,000 seconds.
bool parseTimeToTicks(const std::string& text, const char* defaultUnit, int64_t* ticks);
int64_t delayFromFileName(const std::string& fileName);
std::string stripDelayToken(const std::string& fileName);

// A language code carried in a file name, which is the only place a raw elementary stream can
// keep one. Returns an empty string unless the LAST token before the extension is three letters
// that really are an ISO 639-2 code, so a name that merely contains such letters is not mistaken
// for a tagged one.
bool isIso639_2(const std::string& code);
std::string langFromFileName(const std::string& fileName);

// True when a subtitle file's name marks it as the forced track, as the tools that split
// the forced pictures out write it: "... fra (forced).sup", "movie.forced.sup".
// The word has to stand on its own, so a film whose title contains it is not caught.
bool isForcedSubtitleName(const std::string& fileName);

// Does a "default" track parameter say NO? The parameter is a disc concept whose documented
// values are "all" and "forced", so it was only ever tested for PRESENCE, and default=0 marked
// a track as THE default: the opposite of what anyone writing that means. These values say no:
// none, no, 0, false, off. Case does not matter.
bool defaultParamMeansNo(const std::string& value);
// What default= says, in any case: 0 no (none, no, 0, false, off), 1 the default track (all, yes, 1, true, on), 2 the
// forced one (forced), -1 none of these.
int defaultParamKind(const std::string& value);

// Judge ONE --layer-break-lbn value, in one place, because the two paths that read the option had
// drifted: the meta path refused a sector of zero or less and the --bdmv-to-iso command line
// accepted anything std::stoi would swallow, including negative numbers.
//
// Returns an error message when the value cannot be a sector at all, or an empty string when it can.
// Fills `warning` when the value IS a legal sector but is far too small to be a layer boundary on
// any Blu-ray, which is what a number written with thousands separators turns into: a comma
// separates VALUES here, so 12,219,392 arrives as 12, 219 and 392 and the guard lands inside the
// payload. Warning rather than refusing, because plausibility is a judgement and a wrong refusal is
// worse than a wrong guard.
std::string checkLayerBreakLbn(int lbn, std::string& warning);
// The layer breaks of a disc of discBytes: the sectors of a disc of the window's disc table split evenly over its
// layers (25 GB one layer, none; 50 GB two; 66 GB, the first two layers of a 100 GB disc, two; 100 GB three; 128 GB
// four), at full capacity or with defect management. A size within 2 % of one of them, such as 50g, takes that disc's
// breaks, with a warning in `warning`; any other size larger than a single layer disc has no known breaks, and
// `error` says so. Without a size, the break of a BD-R DL.
std::vector<int> defaultLayerBreaks(int64_t discBytes, std::string& warning, std::string& error);
// The value of --layer-break-lbn, on a MUXOPT line and for --bdmv-to-iso alike: sectors, digits only, separated by
// commas. Returns an error text, or an empty one with lbns filled and any warnings in warnings.
std::string parseLayerBreakList(const std::string& value, std::vector<int>& lbns, std::vector<std::string>& warnings);
// The value of --layer-break-guard and --layer-break-guard-before: whole megabytes from 0 to 9999, on both routes
// and in the window's boxes; a meta file refused more than 1024, which the window offers for defects near 1.5 GB.
// Returns an error text, or an empty one with mb set.
std::string parseGuardMB(const std::string& option, const std::string& value, int& mb);

// save-srt writes its file under a temporary name beside the name asked for, and the file takes that name only when the
// job has succeeded. A job that failed after its first subtitle had written over a file of that name.
std::string saveSrtTempName(const std::string& name);
// Called where the job has succeeded, before the readers are closed
void setSaveSrtJobSucceeded();
// For a save-srt file its writer has closed: 0 it took its name, 1 the job failed and it was removed, 2 the job
// succeeded but it could not take its name (it is kept under the temporary name, reason in why)
int finishSaveSrt(const std::string& tempName, const std::string& name, std::string& why);

static int64_t internalClockToPts(const int64_t value) { return value / INT_FREQ_TO_TS_FREQ; }

// Sample count to internal clock ticks. The clock runs at 5.292 GHz, so the single product samples x clock passes the
// 64-bit range after 1,742,889,652 samples: two and a half hours at 192 kHz, after which every timestamp came out
// wrong. Whole seconds and the rest are converted apart, which gives the same value to the tick and cannot overflow
// for any real length.
static int64_t samplesToInternal(const int64_t samples, const int64_t rate)
{
    return samples / rate * INTERNAL_PTS_FREQ + samples % rate * INTERNAL_PTS_FREQ / rate;
}
static int64_t ptsToInternalClock(const int64_t value) { return value * INT_FREQ_TO_TS_FREQ; }

struct PIPParams
{
    enum class PipCorner
    {
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft
    };

    PIPParams() : scaleIndex(1), corner(PipCorner::TopLeft), hOffset(0), vOffset(0), lumma(3) {}

    [[nodiscard]] bool isFullScreen() const { return scaleIndex == 5; }

    [[nodiscard]] float getScaleCoeff() const
    {
        if (scaleIndex == 2)
            return 0.5;
        if (scaleIndex == 3)
            return 0.25;
        if (scaleIndex == 4)
            return 1.5;

        return 1.0;
    }

    int scaleIndex;
    PipCorner corner;
    unsigned hOffset;
    unsigned vOffset;
    int lumma;
};

#endif
