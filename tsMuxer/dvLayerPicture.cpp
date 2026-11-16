// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "dvLayerPicture.h"

#include <fs/file.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fs/systemlog.h>

#include "batchRunner.h"
#include "hevc.h"
#include "hevcStreamReader.h"
#include "nalUnits.h"
#include "streamDiscoveryData.h"
#include "tsMuxer.h"
#include "vod_common.h"

namespace dvLayer
{
namespace
{
std::vector<uint8_t> thePicture;
}

const std::vector<uint8_t>& picture() { return thePicture; }

std::string take(const uint8_t* data, const size_t size)
{
    static constexpr uint8_t startCode[4] = {0, 0, 0, 1};
    if (data == nullptr || size < 8 || size > 4 * 1024 * 1024)
        return "it is not an HEVC stream of one picture (a file of a few hundred bytes to a few thousand)";
    std::vector<uint8_t> source(data, data + size);
    std::vector<uint8_t> kept;
    int keyPictures = 0;
    int otherPictures = 0;
    bool sets[3] = {false, false, false};  // VPS, SPS, PPS
    uint8_t* end = source.data() + source.size();
    for (uint8_t* nal = NALUnit::findNextNAL(source.data(), end); nal < end;)
    {
        uint8_t* next = NALUnit::findNALWithStartCode(nal, end, true);
        const uint8_t* nalEnd = next;
        if (next < end)
            while (nalEnd > nal && nalEnd[-1] == 0) --nalEnd;  // the zeroes of the next start code
        if (nalEnd - nal >= 3)
        {
            const int type = nal[0] >> 1 & 0x3f;
            const bool slice = type < 32;
            const bool set = type >= 32 && type <= 34;
            if (slice)
            {
                // The first slice of a picture has the top bit of its first byte set
                // (first_slice_segment_in_pic_flag); a message between two slices of a picture does not end it.
                // An IDR picture only: written again and again, each copy starts the count of the pictures anew,
                // which the reader of the layer times it by. Another key picture (CRA) does not, and every copy
                // of it got the time of the first.
                if ((nal[2] & 0x80) || keyPictures + otherPictures == 0)
                    ++(type == 19 || type == 20 ? keyPictures : otherPictures);
            }
            if (set)
                sets[type - 32] = true;
            if (slice || set)
            {
                kept.insert(kept.end(), startCode, startCode + 4);
                kept.insert(kept.end(), static_cast<const uint8_t*>(nal), nalEnd);
            }
        }
        nal = NALUnit::findNextNAL(next, end);
    }
    if (keyPictures != 1 || otherPictures != 0)
        return "it holds " + std::to_string(keyPictures + otherPictures) + " pictures, " + std::to_string(keyPictures) +
               " of them IDR pictures: one IDR picture is wanted";
    if (!sets[0] || !sets[1] || !sets[2])
        return "its parameter sets (VPS, SPS, PPS) are not all there";
    // The look at the picture leaves no mark on the job: a reader marks the disc with what its stream is (HDR10 for
    // this picture), and a job with no HDR video then had its disc marked HDR10 and its subtitles on other PIDs.
    struct KeepDiscMarks
    {
        const int flags = V3_flags;
        unsigned hdr10[6] = {HDR10_metadata[0], HDR10_metadata[1], HDR10_metadata[2],
                             HDR10_metadata[3], HDR10_metadata[4], HDR10_metadata[5]};
        ~KeepDiscMarks()
        {
            V3_flags = flags;
            std::copy(hdr10, hdr10 + 6, HDR10_metadata);  // the light levels of the probed file are not the disc's
        }
    } keepDiscMarks;
    HEVCStreamReader reader;
    std::vector<uint8_t> probe = kept;
    const CheckStreamRez rez = reader.checkStream(probe.data(), static_cast<int>(probe.size()));
    if (!rez.codecInfo.codecID)
        return "it could not be read as HEVC";
    StreamDiscoveryData facts;
    reader.fillVideoDiscoveryData(facts);
    if (facts.width != 1920 || facts.height != 1080)
        return "it is " + std::to_string(facts.width) + "x" + std::to_string(facts.height) +
               ": the second layer of a disc is 1920x1080";
    if (rez.streamDescr.find("Main10") == std::string::npos)
        return "it is not of the Main 10 profile (" + rez.streamDescr + "): the second layer of a disc has 10 bits";
    // PQ and the chroma place of a Blu-ray: by these two the reader of the layer takes it for the second layer of a
    // dual layer disc (profile 7). A picture without them is written as another kind of Dolby Vision stream.
    int profile = 0;
    int compatibility = 0;
    reader.doViProfileAndCompatibility(true, profile, compatibility);
    if (profile != 7)
        return "it is not marked as PQ with the chroma sample place of a Blu-ray (transfer characteristics 16, chroma "
               "sample location 2), which the second layer of a disc has";
    thePicture = std::move(kept);
    return {};
}

std::string takeFile(const std::string& name)
{
    File file;
    if (!file.open(name.c_str(), File::ofRead))
        return "the file cannot be opened";
    int64_t size = 0;
    file.size(&size);
    if (size <= 0 || size > 4 * 1024 * 1024)
        return "it is not an HEVC stream of one picture (a file of a few hundred bytes to a few thousand)";
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (file.read(bytes.data(), static_cast<uint32_t>(size)) != size)
        return "the file cannot be read";
    return take(bytes.data(), bytes.size());
}

namespace
{
std::filesystem::path pathOf(const std::string& utf8) { return {std::u8string(utf8.begin(), utf8.end())}; }

std::string textOf(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// The ffmpeg to start: the one given, else one beside this program, else the first in a folder of the search path.
// Only folders named in full are looked into: an entry such as "." would start whatever is called ffmpeg in the
// folder the job happens to run in.
std::string findFfmpeg(const std::string& given)
{
    std::error_code ec;
    if (!given.empty())
        return std::filesystem::is_regular_file(pathOf(given), ec) ? given : std::string();
#ifdef _WIN32
    const std::filesystem::path name = L"ffmpeg.exe";
    const wchar_t* searchPath = _wgetenv(L"PATH");  // as Windows holds it: the other form loses letters
    const std::wstring all = searchPath ? searchPath : L"";
    const wchar_t separator = L';';
#else
    const std::filesystem::path name = "ffmpeg";
    const char* searchPath = std::getenv("PATH");
    const std::string all = searchPath ? searchPath : "";
    const char separator = ':';
#endif
    if (const std::string own = batch::programFile(); !own.empty())
    {
        const std::filesystem::path beside = pathOf(own).parent_path() / name;
        if (beside.is_absolute() && std::filesystem::is_regular_file(beside, ec))
            return textOf(beside);
    }
    for (size_t at = 0; at <= all.size();)
    {
        size_t end = all.find(separator, at);
        if (end == std::string::npos)
            end = all.size();
        if (end > at)
        {
            const std::filesystem::path folder(all.substr(at, end - at));
            if (folder.is_absolute() && std::filesystem::is_regular_file(folder / name, ec))
                return textOf(folder / name);
        }
        at = end + 1;
    }
    return {};
}

// The frame rate as ffmpeg takes it
std::string rateText(const double fps)
{
    static constexpr struct
    {
        double fps;
        const char* text;
    } RATES[] = {{23.976, "24000/1001"}, {24, "24"}, {25, "25"}, {29.97, "30000/1001"}, {30, "30"}, {50, "50"},
                 {59.94, "60000/1001"},  {60, "60"}};
    for (const auto& rate : RATES)
        if (std::fabs(fps - rate.fps) < 0.01)
            return rate.text;
    return "24000/1001";
}

std::string makeWith(const std::string& ffmpeg, const double fps)
{
    std::error_code ec;
#ifdef _WIN32
    const int process = _getpid();
#else
    const int process = static_cast<int>(getpid());
#endif
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path(ec) / ("tsMuxeR_dv_layer_" + std::to_string(process) + ".hevc");
    if (ec)
        return "the folder for temporary files could not be found";
    struct Remove
    {
        const std::filesystem::path& file;
        ~Remove()
        {
            std::error_code gone;
            std::filesystem::remove(file, gone);
        }
    } removeAtEnd{temp};
    const std::string out = textOf(temp);
    const std::vector<std::string> source = {
        "-hide_banner", "-loglevel", "quiet",
        "-nostdin",     "-y",        "-f",
        "lavfi",        "-i",        "color=c=black:s=1920x1080:r=" + rateText(fps),
        "-frames:v",    "1"};
    // PQ, BT.2020 and the chroma place of a Blu-ray: by the encoder's own options, or written into the stream after it
    const std::string colourFilter =
        "hevc_metadata=colour_primaries=9:transfer_characteristics=16:"
        "matrix_coefficients=9:chroma_sample_loc_type=2:video_full_range_flag=0";
    const std::vector<std::vector<std::string>> encoders = {
        {"-c:v", "libx265", "-profile:v", "main10", "-pix_fmt", "yuv420p10le", "-x265-params",
         "level-idc=51:high-tier=1:no-info=1:repeat-headers=1:aud=0:colorprim=bt2020:transfer=smpte2084:"
         "colormatrix=bt2020nc:chromaloc=2:range=limited:log-level=none"},
        {"-c:v", "hevc_nvenc", "-profile:v", "main10", "-pix_fmt", "p010le", "-bsf:v", colourFilter},
        {"-c:v", "hevc_qsv", "-profile:v", "main10", "-pix_fmt", "p010le", "-bsf:v", colourFilter},
        {"-c:v", "hevc_amf", "-profile:v", "main10", "-pix_fmt", "p010le", "-bsf:v", colourFilter}};
    LTRACE(LT_INFO, 2, "Dolby Vision: " << ffmpeg << " is asked for the one picture the second layer is written with.");
    std::string refused;  // why a picture that was made could not be used: the more telling reason
    for (const auto& encoder : encoders)
    {
        std::vector<std::string> args = source;
        args.insert(args.end(), encoder.begin(), encoder.end());
        args.insert(args.end(), {"-f", "hevc", out});
        std::filesystem::remove(temp, ec);
        int exitCode = 0;
        const int ran = batch::runHelper(ffmpeg, args, 60, exitCode);
        if (ran == 1)
            return "it could not be started";
        if (ran == 2)
        {
            if (refused.empty())
                refused = "it did not end within a minute with its encoder " + encoder[1];
            continue;
        }
        if (exitCode != 0)
            continue;
        const std::string why = takeFile(out);
        if (why.empty())
            return {};
        if (refused.empty())
            refused = "the picture its encoder " + encoder[1] + " made cannot be used: " + why;
    }
    return refused.empty() ? "none of its HEVC encoders (libx265, hevc_nvenc, hevc_qsv, hevc_amf) made a picture"
                           : refused;
}
}  // namespace

std::string ffmpegToUse(const std::string& given) { return findFfmpeg(given); }

std::string make(const std::string& ffmpegGiven, const double fps)
{
    // nothing here ends the job: a job without the picture is written as it always was
    try
    {
        const std::string ffmpeg = findFfmpeg(ffmpegGiven);
        if (ffmpeg.empty())
            return ffmpegGiven.empty()
                       ? "no ffmpeg was found beside tsMuxeR or on the search path (--ffmpeg=<file> names one)"
                       : "the ffmpeg given, " + ffmpegGiven + ", is not there";
        const std::string why = makeWith(ffmpeg, fps);
        return why.empty() ? std::string() : why + " (" + ffmpeg + ")";
    }
    catch (const std::exception& e)
    {
        return std::string("ffmpeg could not be looked for or started (") + e.what() + ")";
    }
    catch (...)
    {
        return "ffmpeg could not be looked for or started";
    }
}
}  // namespace dvLayer
