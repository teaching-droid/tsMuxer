// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "audioConvert.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#include <cerrno>
#include <csignal>
#endif

#include <fs/systemlog.h>

#include "batchRunner.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace audioConvert
{
namespace
{
// the rates of AC-3 (ATSC A/52, frmsizecod), in kbit/s
constexpr int AC3_RATES[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
constexpr int MONO_STEPS[] = {96, 112, 128, 160, 192};
constexpr int STEREO_STEPS[] = {192, 224, 256, 320};
constexpr int MULTI_STEPS[] = {384, 448, 512, 576, 640};
// six hours: the longest a conversion may take before it is given up
constexpr int MAX_SECONDS = 6 * 3600;

std::vector<std::filesystem::path> madeFiles;
// what this run converted already, by its source and choice: a second pass over the meta file (the search of the key
// pictures of a cut) reads the same lines again, and is given the same file
struct Done
{
    std::string file;
    bool aligned = false;
};
std::map<std::string, Done> doneFiles;
// the files are removed when the program ends, also after an error
struct RemoveAtExit
{
    ~RemoveAtExit() { removeMade(); }
} removeAtExit;

std::filesystem::path pathOf(const std::string& utf8) { return {std::u8string(utf8.begin(), utf8.end())}; }
std::string textOf(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

int process()
{
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

// The name of this computer, letters and digits only: a container that shares the folder for temporary files has
// process numbers of its own, and its files are not taken for those of a run that is gone
std::string host()
{
    static const std::string name = []
    {
        char buf[256] = {};
#ifdef _WIN32
        DWORD size = sizeof(buf);
        if (!GetComputerNameA(buf, &size))
            buf[0] = 0;
#else
        if (gethostname(buf, sizeof(buf) - 1) != 0)
            buf[0] = 0;
#endif
        std::string out;
        for (const char* p = buf; *p && out.size() < 32; ++p)
            if (isalnum(static_cast<unsigned char>(*p)))
                out += *p;
        return out.empty() ? std::string("host") : out;
    }();
    return name;
}

// A name in the folder for temporary files that this run has not used
// Whether a process of that number runs (here or for another user); true where it cannot be told
bool running(const long long pid)
{
#ifdef _WIN32
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD code = 0;
    const bool alive = !GetExitCodeProcess(h, &code) || code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

// ** WHAT A KILLED RUN CONVERTED IS REMOVED BY THE NEXT. ** A run ended hard (the task manager, SIGKILL) runs no code
// of its own, and its converted sound, gigabytes of LPCM for a film, stayed in the folder for temporary files. The
// files of a run whose process is gone are removed, once, by the first conversion of a later run.
void removeLeftOverIn(const std::filesystem::path& folder)
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const std::string HEAD = "tsMuxeR_convert_" + host() + "_";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::u8string name8 = it->path().filename().u8string();
        const std::string name(name8.begin(), name8.end());
        if (name.rfind(HEAD, 0) != 0)
            continue;
        const size_t digits = name.find_first_not_of("0123456789", HEAD.size());
        if (digits == HEAD.size() || digits == std::string::npos || name[digits] != '_')
            continue;
        const long long pid = std::atoll(name.c_str() + HEAD.size());
        std::error_code gone;
        if (pid > 0 && pid != static_cast<long long>(process()) && !running(pid))
            std::filesystem::remove(it->path(), gone);
    }
}

std::filesystem::path tempFile(const std::string& ext, std::string& why)
{
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        why = "the folder for temporary files could not be found";
        return {};
    }
    removeLeftOverIn(folder);
    for (int n = 1;; ++n)
    {
        std::filesystem::path p = folder / ("tsMuxeR_convert_" + host() + "_" + std::to_string(process()) + "_" +
                                            std::to_string(n) + "." + ext);
        if (!std::filesystem::exists(p, ec))
            return p;
    }
}

int channelsOut(const Spec& spec, const Source& source)
{
    if (spec.stereo)
        return source.channels == 1 ? 1 : 2;  // a mono track is not made two channels of the same
    if (spec.kind == Spec::Kind::ac3 && source.channels > 6)
        return 6;  // AC-3 carries 5.1 at most: ffmpeg folds 7.1 down
    return source.channels;
}

std::string channelText(const int channels)
{
    switch (channels)
    {
    case 1:
        return "mono";
    case 2:
        return "stereo";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    case 0:
        return "with its channels";
    default:
        return std::to_string(channels) + " channels";
    }
}

[[noreturn]] void stopped()
{
    removeMade();
    THROW(ERR_COMMON, "The job was stopped by " << stopSignalName(stopSignal()) << " before it was finished.")
}

// ffmpeg's framecrc of `args` (the file it writes is added), as lines; false where ffmpeg failed. A stop request ends
// it and the job.
bool probeLines(const std::string& ffmpeg, std::vector<std::string> args, std::vector<std::string>& lines)
{
    lines.clear();
    std::string why;
    const std::filesystem::path probe = tempFile("txt", why);
    if (probe.empty())
        return false;
    struct Remove
    {
        const std::filesystem::path& file;
        ~Remove()
        {
            std::error_code gone;
            std::filesystem::remove(file, gone);
        }
    } removeAtEnd{probe};
    args.insert(args.end(), {"-f", "framecrc", textOf(probe)});
    int exitCode = 0;
    const int ran = batch::runHelper(ffmpeg, args, 600, exitCode, true);
    if (ran == 3 || stopSignal() != 0)
        stopped();
    if (ran != 0 || exitCode != 0)
        return false;
    std::ifstream in(probe);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return true;
}

// The time of the first picture of the first video track of the file that is shown (or, with firstPacket, of the
// first in decoding order), on ffmpeg's timeline with the times of the file kept (-copyts). False where the file has
// no video, or ffmpeg cannot tell.
bool videoStart(const std::string& ffmpeg, const std::string& file, const bool firstPacket, double& seconds)
{
    // the packets of the first 64 pictures as they are, without decoding: their presentation times; the earliest is
    // the first picture shown. V (not v): no cover picture of the file.
    std::vector<std::string> lines;
    if (!probeLines(ffmpeg,
                    {"-hide_banner", "-nostdin", "-loglevel", "error", "-copyts", "-i", file, "-map", "0:V:0", "-c",
                     "copy", "-frames:v", "64"},
                    lines))
        return false;
    double tbNum = 0, tbDen = 0;
    bool found = false;
    long long best = 0;
    for (const std::string& line : lines)
    {
        if (line.rfind("#tb 0:", 0) == 0)
        {
            const size_t slash = line.find('/');
            if (slash != std::string::npos)
            {
                tbNum = std::atof(line.c_str() + 6);
                tbDen = std::atof(line.c_str() + slash + 1);
            }
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        // stream_index, dts, pts, duration, size, hash
        std::istringstream fields(line);
        std::string stream, dts, pts;
        if (!std::getline(fields, stream, ',') || !std::getline(fields, dts, ',') || !std::getline(fields, pts, ','))
            continue;
        // a picture without a time (NOPTS, written as the lowest number): its time is not known. Taken as the
        // earliest, the start was 0, and the sound of a broadcast padded with silence from there
        if (pts.find("NOPTS") != std::string::npos)
            continue;
        const long long p = std::atoll(pts.c_str());
        if (p == LLONG_MIN)
            continue;
        if (!found || (!firstPacket && p < best))
            best = p;
        found = true;
        if (firstPacket)
            break;
    }
    if (!found || tbNum <= 0 || tbDen <= 0)
        return false;
    seconds = static_cast<double>(best) * tbNum / tbDen;
    return true;
}

// The time of the first sample of the track ffmpeg decodes, with the times of the file kept: for an MP4 the start
// of the track's edit (what the edit list skips, priming and the sound before a cut, is skipped by the decoder)
// decoded: the first sample the decoder gives; otherwise the first packet as it is read (before the decoder skips
// what the edit list skips)
bool firstSoundTime(const std::string& ffmpeg, const Source& source, double& seconds, const bool decoded = true)
{
    std::vector<std::string> lines;
    std::vector<std::string> args = {"-hide_banner", "-nostdin", "-loglevel", "error", "-copyts",   "-i",
                                     source.file,    "-map",     source.map,  "-vn",   "-frames:a", "1"};
    if (decoded)
        args.insert(args.end(), {"-c:a", "pcm_s16le"});
    else
        args.insert(args.end(), {"-c", "copy"});
    if (!probeLines(ffmpeg, args, lines))
        return false;
    double tbNum = 0, tbDen = 0;
    for (const std::string& line : lines)
    {
        if (line.rfind("#tb 0:", 0) == 0)
        {
            const size_t slash = line.find('/');
            if (slash != std::string::npos)
            {
                tbNum = std::atof(line.c_str() + 6);
                tbDen = std::atof(line.c_str() + slash + 1);
            }
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        std::string stream, dts, pts;
        if (!std::getline(fields, stream, ',') || !std::getline(fields, dts, ',') || !std::getline(fields, pts, ',') ||
            pts.find("NOPTS") != std::string::npos || tbNum <= 0 || tbDen <= 0)
            return false;
        const long long p = std::atoll(pts.c_str());
        if (p == LLONG_MIN)
            return false;
        seconds = static_cast<double>(p) * tbNum / tbDen;
        return true;
    }
    return false;
}

// The codecs ffmpeg names for a codec of tsMuxeR; empty: any sound
std::vector<std::string> ffmpegCodecs(const std::string& codec)
{
    static const std::map<std::string, std::vector<std::string>> NAMES = {{"A_AAC", {"aac", "aac_latm"}},
                                                                          {"A_MP3", {"mp3", "mp2", "mp1"}},
                                                                          {"A_OPUS", {"opus"}},
                                                                          {"A_VORBIS", {"vorbis"}},
                                                                          {"A_FLAC", {"flac"}},
                                                                          {"A_ALAC", {"alac"}},
                                                                          {"A_WAVPACK4", {"wavpack"}},
                                                                          {"A_AC3", {"ac3", "eac3"}},
                                                                          {"A_DTS", {"dts"}},
                                                                          {"A_MLP", {"truehd", "mlp"}}};
    const auto found = NAMES.find(codec);
    return found != NAMES.end() ? found->second : std::vector<std::string>();
}

// ** FFMPEG READS THE TRACK THE LINE NAMES. ** The two number the tracks of a file each their own way (Matroska by
// place, MP4 by track_ID, a program stream by start code): where they differ, another track was converted without a
// word, or the job failed. ffmpeg's stream is checked against the codec and the rate of the track first. Empty where
// it is the track.
std::string checkTrack(const std::string& ffmpeg, const Source& source)
{
    std::vector<std::string> lines;
    if (!probeLines(ffmpeg,
                    {"-hide_banner", "-nostdin", "-loglevel", "error", "-i", source.file, "-map", source.map, "-c",
                     "copy", "-frames:a", "1"},
                    lines))
        return "ffmpeg found no stream " + source.map.substr(2) + " in the file";
    std::string type, codec;
    int rate = 0;
    const auto valueOf = [](const std::string& line) { return trimStr(line.substr(line.find(':', 4) + 1)); };
    for (const std::string& line : lines)
        if (line.rfind("#media_type 0:", 0) == 0)
            type = valueOf(line);
        else if (line.rfind("#codec_id 0:", 0) == 0)
            codec = valueOf(line);
        else if (line.rfind("#sample_rate 0:", 0) == 0)
            rate = std::atoi(valueOf(line).c_str());
    const std::vector<std::string> names = ffmpegCodecs(source.codec);
    const bool codecOk = source.codec == "A_LPCM" ? codec.rfind("pcm_", 0) == 0
                         : names.empty()          ? true
                                                  : std::find(names.begin(), names.end(), codec) != names.end();
    // AAC with SBR: the container gives the rate of the core or of the whole, either of the two
    const bool rateOk = source.rate == 0 || rate == 0 || rate == source.rate || rate == 2 * source.rate ||
                        2 * rate == source.rate || std::abs(rate - source.rate) * 100 < source.rate;
    if (type != "audio" || !codecOk || !rateOk)
        return "ffmpeg's stream " + source.map.substr(2) + " of the file is " +
               (type.empty() ? std::string("not known") : type + " " + codec) +
               (rate ? " at " + std::to_string(rate) + " Hz" : std::string()) + ", not this track";
    return {};
}
}  // namespace

std::string parse(const std::string& text, Spec& spec)
{
    spec = Spec();
    std::vector<std::string> parts;
    for (size_t at = 0; at <= text.size();)
    {
        size_t end = text.find(':', at);
        if (end == std::string::npos)
            end = text.size();
        parts.push_back(lower(trimStr(text.substr(at, end - at))));
        at = end + 1;
    }
    if (parts.empty() || parts[0].empty())
        return "it names no kind: ac3 or lpcm";
    if (parts[0] == "ac3")
        spec.kind = Spec::Kind::ac3;
    else if (parts[0] == "lpcm")
        spec.kind = Spec::Kind::lpcm;
    else
        return "\"" + parts[0] + "\" is no kind it converts to: ac3 or lpcm";
    int numbers = 0;
    for (size_t i = 1; i < parts.size(); ++i)
    {
        const std::string& p = parts[i];
        if (p == "stereo")
        {
            spec.stereo = true;
            continue;
        }
        if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos)
            return "\"" + p + "\" is neither a number nor \"stereo\"";
        const int n = std::atoi(p.c_str());
        if (spec.kind == Spec::Kind::ac3)
        {
            if (numbers++ > 0)
                return "AC-3 takes one number, its rate in kbit/s";
            if (std::find(std::begin(AC3_RATES), std::end(AC3_RATES), n) == std::end(AC3_RATES))
                return std::to_string(n) +
                       " kbit/s is no rate of AC-3; they are 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, "
                       "320, 384, 448, 512, 576 and 640";
            spec.kbps = n;
        }
        else if (numbers++ == 0)
        {
            if (n != 16 && n != 24)
                return "LPCM is made with 16 or 24 bit, not " + p;
            spec.bits = n;
        }
        else if (numbers == 2)
        {
            const int hz = n < 1000 ? n * 1000 : n;
            if (hz != 48000 && hz != 96000 && hz != 192000)
                return "LPCM on a disc is at 48, 96 or 192 kHz, not " + p;
            spec.rate = hz;
        }
        else
            return "LPCM takes two numbers at most: the bits and the rate";
    }
    return {};
}

int suggestedKbps(const Source& source, const bool stereo)
{
    // a mono track stays mono (channelsOut), and 192 kbit/s is twice what mono AAC of 96 kbit/s can fill
    const bool mono = source.channels == 1;
    const bool two = stereo || (source.channels > 0 && source.channels <= 2);
    const int* steps = mono ? MONO_STEPS : two ? STEREO_STEPS : MULTI_STEPS;
    const size_t count = mono ? std::size(MONO_STEPS) : two ? std::size(STEREO_STEPS) : std::size(MULTI_STEPS);
    int lowest = steps[0];
    if (!two && source.channels >= 5)
        lowest = 448;
    if (source.lossless || source.kbps <= 0)
        return steps[count - 1];
    // a stereo track made of a source of more channels gets the share of its two channels
    double wanted = source.kbps * 1.25;
    if (two && source.channels > 2)
        wanted = wanted * 2 / source.channels;
    for (size_t i = 0; i < count; ++i)
        if (steps[i] >= wanted)
            return (std::max)(steps[i], lowest);
    return steps[count - 1];
}

int suggestedBits(const Source& source)
{
    if (!source.lossless || source.avchd)
        return 16;
    return source.bits == 0 || source.bits > 16 ? 24 : 16;
}

int suggestedRate(const Source& source, const bool stereo)
{
    const int channels = stereo ? 2 : source.channels;
    if (source.avchd)
        return 48000;  // the LPCM of an AVCHD disc as cameras write it
    if (source.rate == 48000 || source.rate == 96000)
        return source.rate;
    if (source.rate == 192000)
        return channels <= 6 ? 192000 : 96000;
    if (source.rate >= 88200)
        return 96000;
    return 48000;
}

std::string codecOf(const Spec& spec) { return spec.kind == Spec::Kind::ac3 ? "A_AC3" : "A_LPCM"; }

std::string describe(const Spec& spec, const Source& source)
{
    const int channels = channelsOut(spec, source);
    std::ostringstream text;
    if (spec.kind == Spec::Kind::ac3)
        text << "AC-3 " << channelText(channels) << " at "
             << (spec.kbps > 0 ? spec.kbps : suggestedKbps(source, spec.stereo)) << " kbit/s";
    else
    {
        const int rate = spec.rate > 0 ? spec.rate : suggestedRate(source, spec.stereo);
        text << "LPCM " << channelText(channels) << ", " << rate / 1000 << " kHz, "
             << (spec.bits > 0 ? spec.bits : suggestedBits(source)) << " bit";
    }
    if (spec.kind == Spec::Kind::ac3 && !spec.stereo && source.channels > 6)
        text << " (folded down from " << channelText(source.channels) << ")";
    return text.str();
}

std::string run(const std::string& ffmpeg, const Source& source, const Spec& spec, std::string& made, bool& aligned)
{
    made.clear();
    aligned = false;
    // ** A STOP REQUEST REMOVES WHAT WAS CONVERTED. ** From the first conversion on, a request to stop is watched (and
    // acted on by the next conversion, or by the mux, which remove the converted files): the program ended at once
    // between two conversions and before the mux, and left the files of the first in the folder for temporary
    // files. The mux stops watching at its end.
    setStopWatched(true);
    if (stopSignal() != 0)
        stopped();
    const std::string key = source.file + "|" + source.map + "|" + std::to_string(static_cast<int>(spec.kind)) + "|" +
                            std::to_string(spec.kbps) + "|" + std::to_string(spec.bits) + "|" +
                            std::to_string(spec.rate) + "|" + (spec.stereo ? "s" : "");
    if (const auto done = doneFiles.find(key); done != doneFiles.end())
    {
        made = done->second.file;
        aligned = done->second.aligned;
        return {};
    }
    if (std::string wrong = checkTrack(ffmpeg, source); !wrong.empty())
        return wrong;
    std::string why;
    const bool ac3 = spec.kind == Spec::Kind::ac3;
    const std::filesystem::path out = tempFile(ac3 ? "ac3" : "wav", why);
    if (out.empty())
        return why;
    const int rate = ac3 ? 48000 : spec.rate > 0 ? spec.rate : suggestedRate(source, spec.stereo);
    const int channels = channelsOut(spec, source);
    if (!ac3 && rate == 192000 && channels > 6)
        return "LPCM at 192 kHz carries 6 channels at most on a disc; choose 96 or 48 kHz";

    // where the film starts, on the timeline the conversion is read with
    double start = source.startSeconds;
    if (source.soundAnchor)
    {
        double first = 0;
        if (!firstSoundTime(ffmpeg, source, first))
            return "ffmpeg could not read the start of the track";
        // ** FFMPEG HAS TO READ THE TIMES AS THE MUX DOES. ** The decoder skips at the start of the track what its edit
        // list skips (and the pre-skip of Opus, a frame at most beyond it). A fragmented MP4 whose edit lists ffmpeg
        // reads otherwise skipped 1.75 s more, a jump in its times, and the converted sound came 1.75 s early. Such a
        // file is not converted; muxed into a Matroska file first, it is. (ffmpeg's pictures are no measure: in a
        // fragmented file it leaves their edit list out.)
        double packet = 0;
        if (firstSoundTime(ffmpeg, source, packet, false) &&
            (first - packet > source.editSkipSeconds + 0.05 || first - packet < -0.001))
            return std::string(TIMES_DIFFER) + ": it skips " + std::to_string(std::lround((first - packet) * 1000)) +
                   " ms at the start of the sound, where the edit list of the track skips " +
                   std::to_string(std::llround(source.editSkipSeconds * 1000)) +
                   " ms. Mux the file into a Matroska file first, and convert the track of that file";
        start = first - source.anchorSeconds;
        aligned = true;
    }
    else if (source.timeKnown)
    {
        aligned = true;
        double video = 0;
        static constexpr double WRAP = 8589934592.0 / 90000.0;
        // only where the rest is small: the first picture of a multiplex can be of another program, whose clock
        // has nothing to do with this one
        if (source.wrapsAt33Bits && videoStart(ffmpeg, source.file, true, video))
            if (const double turns = std::round((video - start) / WRAP);
                turns != 0 && std::abs(video - start - turns * WRAP) < 60)
                start += turns * WRAP;
    }
    else
        aligned = videoStart(ffmpeg, source.file, source.firstPacket, start);
    std::vector<std::string> args = {"-hide_banner", "-nostdin", "-loglevel", "error", "-y"};
    if (aligned)
        args.push_back("-copyts");
    args.insert(args.end(), {"-i", source.file, "-map", source.map, "-vn", "-sn", "-dn"});
    std::string filter;
    if (aligned)
    {
        // pads with silence, or trims, so that the sound begins at the first picture; gaps of a tenth of a second
        // and more in the track are filled, so that what follows stays in its place. A start before 0 (the pictures
        // a cut MP4 keeps before its edit) is padded too: the mux keeps those pictures
        const long long first = std::llround(start * rate);
        filter = "aresample=" + std::to_string(rate) + ":async=1:first_pts=" + std::to_string(first);
    }
    else
        filter = "aresample=" + std::to_string(rate);
    // An AC-3 encoder runs 256 samples late: they are taken from the start, so that the sound keeps its place, and
    // added at the end, so that it keeps its length (as the window's AC-3 core does)
    if (ac3)
        filter += ",atrim=start_sample=256,apad=pad_len=256";
    args.insert(args.end(), {"-af", filter, "-ar", std::to_string(rate)});
    if (channels > 0 && channels != source.channels)
        args.insert(args.end(), {"-ac", std::to_string(channels)});
    if (ac3)
    {
        const int kbps = spec.kbps > 0 ? spec.kbps : suggestedKbps(source, spec.stereo);
        args.insert(args.end(), {"-c:a", "ac3", "-b:a", std::to_string(kbps) + "k", "-f", "ac3"});
    }
    else
    {
        const int bits = spec.bits > 0 ? spec.bits : suggestedBits(source);
        // RF64 where the file grows past 4 GB, which a WAV header cannot say
        args.insert(args.end(), {"-c:a", bits == 24 ? "pcm_s24le" : "pcm_s16le", "-rf64", "auto", "-f", "wav"});
    }
    args.push_back(textOf(out));
    madeFiles.push_back(out);
    // ** A CLOCK THAT STARTS AGAIN IS NOT CONVERTED. ** A recording or a VOB whose clock starts again inside it (a
    // short clip before the film, a joined recording, a left-out cell) is read by the mux part after part; ffmpeg,
    // keeping the times of the file, padded the jump with silence or cut the sound at it: the sound of a VOB with a
    // 2 s first part came 4 s off, and broken. The times of the first video are listed in the same run (reading the
    // file once) and checked as the mux parts the file (ProgramStreamDemuxer::noteVideoTime): a decoding time that
    // goes back (the list holds it at the last one: three pictures at one time), or on more than a second, refuses
    // the conversion. A gap in the sound alone is padded, as the mux keeps it. A file without video is not checked.
    std::filesystem::path times;
    if (source.clockMayJump)
    {
        times = tempFile("txt", why);
        if (!times.empty())
        {
            madeFiles.push_back(times);
            args.insert(args.end(), {"-map", "0:V:0?", "-c", "copy", "-f", "framecrc", textOf(times)});
        }
    }
    int exitCode = 0;
    // A request to stop ends the conversion and the job (exit code 4), as it ends a mux (the stop is watched, above):
    // the ffmpeg it started would go on writing a file no one removes
    const int ran = batch::runHelper(ffmpeg, args, MAX_SECONDS, exitCode, true);
    if (ran == 3 || stopSignal() != 0)
        stopped();
    if (ran == 1)
        return "ffmpeg (" + ffmpeg + ") could not be started";
    if (ran == 2)
        return "ffmpeg did not end within six hours";
    std::error_code ec;
    if (const auto room = std::filesystem::space(out.parent_path(), ec);
        exitCode != 0 && !ec && room.available < 64ULL * 1024 * 1024)
        return "the folder for temporary files (" + textOf(out.parent_path()) +
               ") is full: the converted sound is written there first. Make room, or point TEMP (TMPDIR) to a "
               "drive with room";
    if (exitCode != 0 || !std::filesystem::is_regular_file(out, ec) || std::filesystem::file_size(out, ec) == 0)
        return "ffmpeg ended with exit code " + std::to_string(exitCode) + " and made no sound file (" + source.map +
               " of " + source.file + ")";
    if (!times.empty())
    {
        std::ifstream in(times);
        double tbNum = 0, tbDen = 0;
        long long last = LLONG_MIN;
        int stalled = 0;
        std::string line;
        std::string jump;
        while (jump.empty() && std::getline(in, line))
        {
            if (line.rfind("#tb 0:", 0) == 0)
            {
                const size_t slash = line.find('/');
                if (slash != std::string::npos)
                {
                    tbNum = std::atof(line.c_str() + 6);
                    tbDen = std::atof(line.c_str() + slash + 1);
                }
                continue;
            }
            if (line.empty() || line[0] == '#' || tbNum <= 0 || tbDen <= 0)
                continue;
            std::istringstream fields(line);
            std::string stream, dts;
            if (!std::getline(fields, stream, ',') || !std::getline(fields, dts, ',') ||
                dts.find("NOPTS") != std::string::npos)
                continue;
            const long long d = std::atoll(dts.c_str());
            if (d == LLONG_MIN)
                continue;
            if (last != LLONG_MIN)
            {
                const double step = static_cast<double>(d - last) * tbNum / tbDen;
                // the writer of the list holds a time that goes back at the last one: pictures that do not move on
                stalled = d == last ? stalled + 1 : 0;
                if (step < 0 || step > 1 || stalled >= 2)
                    jump =
                        (stalled >= 3 ? std::string("a step back") : std::to_string(std::lround(step * 1000)) + " ms") +
                        " at " + std::to_string(std::lround(static_cast<double>(last) * tbNum / tbDen)) + " s";
            }
            last = d;
        }
        in.close();
        std::filesystem::remove(times, ec);
        if (!jump.empty())
        {
            std::filesystem::remove(out, ec);
            return std::string(TIMES_DIFFER) + ": its clock starts again inside it (" + jump +
                   " on the clock of the file). Mux the file into a Matroska file first, and convert the track of that "
                   "file";
        }
    }
    if (!aligned)
        LTRACE(LT_INFO, 2,
               "The file has no video ffmpeg could time, so the converted sound keeps the start of its own track.");
    made = textOf(out);
    doneFiles[key] = Done{made, aligned};
    return {};
}

void removeLeftOver()
{
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::temp_directory_path(ec);
    if (!ec)
        removeLeftOverIn(folder);
}

void removeMade()
{
    // a file still open (the mux reads it, on Windows without leave to remove it) is kept on the list, for the
    // removal when the program ends (endBySignal, RemoveAtExit), after the readers are closed
    std::vector<std::filesystem::path> left;
    for (const auto& f : madeFiles)
    {
        std::error_code ec;
        if (!std::filesystem::remove(f, ec) && ec && std::filesystem::exists(f, ec))
            left.push_back(f);
    }
    madeFiles = left;
    doneFiles.clear();
}
}  // namespace audioConvert
