// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef VOD_CORE_EXCEPTION_H_
#define VOD_CORE_EXCEPTION_H_

#include <sstream>
#include <stdexcept>
#include <string>

class VodCoreException
{
   public:
    VodCoreException(const int errCode, std::string errStr) : m_errCode(errCode), m_errStr(std::move(errStr)) {}
    VodCoreException(const int errCode, const char* errStr) : m_errCode(errCode), m_errStr(errStr) {}

    int m_errCode;
    std::string m_errStr;
};

#define THROW(errCode, msg)                        \
    {                                              \
        std::ostringstream ss;                     \
        ss << msg;                                 \
        throw VodCoreException(errCode, ss.str()); \
    }

static constexpr int ERR_INVALID_HANDLE = 1;

static constexpr int ERR_CANT_EXEC_PROCESS = 2;

static constexpr int ERR_COMMON = 3;

// What a general error is about, for the exit code (ExitCode::of)
static constexpr int ERR_USAGE = 10;   // the command line or the meta file
static constexpr int ERR_SOURCE = 11;  // a source that cannot be read or is not supported
static constexpr int ERR_OUTPUT = 12;  // the output cannot be written

static constexpr int ERR_FILE_NOT_FOUND = 100;
static constexpr int ERR_FILE_SEEK = 101;
static constexpr int ERR_CANT_OPEN_STREAM = 102;
static constexpr int ERR_CANT_CREATE_FILE = 103;
static constexpr int ERR_FILE_EXISTS = 104;
static constexpr int ERR_FILE_COMMON = 105;

static constexpr int ERR_TS_COMMON = 200;
static constexpr int ERR_TS_UNKNOWN_MEDIA = 201;
static constexpr int ERR_TS_RARE_PMT = 202;

static constexpr int ERR_H264_RARE_SPS = 300;
static constexpr int ERR_H264_PARSE_ERROR = 301;
static constexpr int ERR_H264_UNSUPPORTED_PARAMETER = 302;

static constexpr int ERR_MPEG2_ERR_FPS = 350;
static constexpr int ERR_VC1_ERR_FPS = 351;
static constexpr int ERR_VC1_ERR_PROFILE = 352;

static constexpr int ERR_AAC_RARE_FRAME = 400;
static constexpr int ERR_AAC_TO_SMALL_AV_PACKET = 401;
static constexpr int ERR_AC3_RARE_FRAME = 410;
static constexpr int ERR_AC3_TO_SMALL_AV_PACKET = 411;

static constexpr int ERR_MP3_RARE_FRAME = 420;

static constexpr int ERR_INVALID_CODEC_INFO_FORMAT = 500;
static constexpr int ERR_UNKNOWN_CODEC = 501;
static constexpr int ERR_INVALID_CODEC_FORMAT = 502;
static constexpr int ERR_INVALID_STREAMS_SELECTED = 504;

static constexpr int ERR_COMMON_SMALL_BUFFER = 600;
static constexpr int ERR_COMMON_NETWORK = 601;
static constexpr int ERR_COMMON_MEMORY = 602;

static constexpr int ERR_UNSUPPORTER_CONTAINER_FORMAT = 700;
static constexpr int ERR_CONTAINER_STREAM_NOT_SYNC = 701;

static constexpr int ERR_AV_FRAME_TOO_LARGE = 800;

static constexpr int ERR_MATROSKA_PARSE = 900;

static constexpr int ERR_MOV_PARSE = 950;

static constexpr int ERR_WAV_PARSE = 950;

static constexpr int ERR_INTERNAL_ERROR = 1000;
static constexpr char ERR_INTERNAL_ERROR_STR[] = "Internal error";

static constexpr int MSG_ENDOFFILE = 2000;

static constexpr int ERR_FEC_UNSUPPORTED_FEATURE = 3000;

// ** EXIT CODES WITH A MEANING. ** The program ended with -1, -2, -3 or -4 by the kind of exception that ended it,
// which said nothing about what was wrong: a script could tell success from failure and no more. The code now names the
// reason, so that a script can stop on a wrong command line and go on after a source it cannot read.
// A stop by Ctrl+C or SIGTERM is not among them: the program then ends as the system ends one that is stopped, so a
// script that runs it in a loop stops too (endBySignal in main.cpp). 4 is kept free for it.
namespace ExitCode
{
constexpr int Done = 0;
constexpr int Usage = 1;     // the command line or the meta file is wrong
constexpr int Source = 2;    // a source cannot be read or is not supported
constexpr int Output = 3;    // the output cannot be written
constexpr int Stopped = 4;   // kept free, see above
constexpr int Internal = 5;  // an error inside the program
constexpr int Batch = 6;     // --batch: a job failed or was not run

inline int of(const int errCode)
{
    switch (errCode)
    {
    case ERR_USAGE:
    case 500:  // ERR_INVALID_CODEC_INFO_FORMAT
    case 501:  // ERR_UNKNOWN_CODEC
    case 502:  // ERR_INVALID_CODEC_FORMAT
    case 504:  // ERR_INVALID_STREAMS_SELECTED
        return Usage;
    case ERR_OUTPUT:
    case 103:  // ERR_CANT_CREATE_FILE
    case 104:  // ERR_FILE_EXISTS
    case 105:  // ERR_FILE_COMMON
        return Output;
    case 1:     // ERR_INVALID_HANDLE
    case 600:   // ERR_COMMON_SMALL_BUFFER
    case 602:   // ERR_COMMON_MEMORY
    case 1000:  // ERR_INTERNAL_ERROR
        return Internal;
    default:
        return Source;
    }
}
}  // namespace ExitCode

// An error that is not a VodCoreException, by what it is about: any other std::runtime_error is about a source
struct OutputError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

#endif
