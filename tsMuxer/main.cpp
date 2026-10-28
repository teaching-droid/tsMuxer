#include <fs/directory.h>
#include <fs/systemlog.h>
#include <fs/textfile.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <climits>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include "bdSsifImport.h"
#include "blank_patterns.h"
#include "blurayHelper.h"
#include "chapterFile.h"
#include "convertUTF.h"
#include "h264StreamReader.h"
#include "hevcStreamReader.h"
#include "iso_writer.h"
#include "matroskaDemuxer.h"
#include "matroskaMuxer.h"
#include "metaDemuxer.h"
#include "movDemuxer.h"
#include "mpegStreamReader.h"
#include "muxerManager.h"
#include "pgsStreamReader.h"
#include "simplePacketizerReader.h"
#include "singleFileMuxer.h"
#include "tsMuxer.h"

using namespace std;

BufferedReaderManager readManager(2, DEFAULT_FILE_BLOCK_SIZE, DEFAULT_FILE_BLOCK_SIZE + MAX_AV_PACKET_SIZE,
                                  DEFAULT_FILE_BLOCK_SIZE / 2);
TSMuxerFactory tsMuxerFactory;
MatroskaMuxerFactory matroskaMuxerFactory;
SingleFileMuxerFactory singleFileMuxerFactory;

// A UTF-8 path for std::filesystem. On Windows a long one gets the \\?\ prefix (windowsPath), as every file tsMuxeR
// opens does: without it the old output kept until a new one is complete, and the image of --bdmv-to-iso, were not
// found on a path of 248 characters or more.
static std::filesystem::path fsPath(const string& utf8)
{
#ifdef _WIN32
    const string p = windowsPath(utf8);
#else
    const string& p = utf8;
#endif
    return {std::u8string(p.begin(), p.end())};
}

static constexpr char EXCEPTION_ERR_MSG[] =
    ". It does not have to be! Please contact application support team for more information.";

#define LTRACE2(level, msg)                   \
    {{if ((level) <= LT_WARN) cerr << msg;    \
    else if ((level) == LT_INFO) cout << msg; \
    if ((level) <= LT_INFO)                   \
        sLastMsg = true;                      \
    }                                         \
    }
// A meta file saved in UTF-16, as some editors save text, was refused as "Invalid codec format" with the raw bytes of
// its first line, and a folder given as the meta file was read as an empty one and said "No tracks selected". Both are
// said as what they are, before anything reads the file.
static void refuseUnreadableMeta(const std::string& metaFileName)
{
    std::error_code ec;
    if (std::filesystem::is_directory(fsPath(metaFileName), ec))
        THROW(ERR_COMMON, metaFileName << " is a folder, not a meta file.")
    File file;
    uint8_t bom[2] = {};
    if (!file.open(metaFileName.c_str(), File::ofRead) || file.read(bom, 2) != 2)
        return;  // said as before by the reader of the meta file
    if ((bom[0] == 0xFF && bom[1] == 0xFE) || (bom[0] == 0xFE && bom[1] == 0xFF))
        THROW(ERR_COMMON, "The meta file " << metaFileName
                                           << " is saved in UTF-16. tsMuxeR reads meta files in UTF-8: save it as "
                                              "UTF-8.")
}

DiskType checkBluRayMux(const char* metaFileName, int& autoChapterLen, vector<double>& customChaptersList,
                        vector<AVChapter>& chapterInfo, int& firstMplsOffset, int& firstM2tsOffset, bool& insertBlankPL,
                        int& blankNum, bool& stereoMode, std::string& isoDiskLabel, bool& demux)
{
    autoChapterLen = 0;
    demux = false;
    stereoMode = false;
    bool fromChapterFile = false;
    TextFile file(metaFileName, File::ofRead);
    string str;
    file.readLine(str);
    DiskType result = DiskType::NONE;
    while (str.length() > 0)
    {
        // A MUXOPT line written with spaces or tabs in front lost every option on it without a word: this test and
        // MuxerManager's looked at the raw line, while the track reader trims it and then skips it as MUXOPT.
        if (strStartWith(str.substr((std::min)(str.find_first_not_of(" 	"), str.size())), "MUXOPT"))
        {
            vector<string> params = splitQuotedStr(MuxerManager::muxOptLine(str).c_str(), ' ');
            std::string previous;  // the option before, when it had a value
            for (const auto& param : params)
            {
                vector<string> paramPair = splitOption(trimStr(param));
                if (paramPair.empty())
                    continue;
                // Digits standing alone after an option with a value are the rest of that value, written with spaces:
                // --layer-break-lbn=12 219 392 was read as sector 12, and 219 and 392 were ignored as unknown options
                // with a warning (LBN spaces)
                if (!previous.empty() && paramPair.size() == 1 &&
                    paramPair[0].find_first_not_of("0123456789") == std::string::npos)
                    THROW(ERR_COMMON, previous << " is followed by " << paramPair[0]
                                               << " on the MUXOPT line: a value is written without spaces, such as "
                                                  "--layer-break-lbn=12219392.")
                // A time after --custom-chapters is the rest of its list, written with a space after a semicolon:
                // --custom-chapters=00:00:01.000; 00:00:03.000 lost the second chapter, with a warning about an
                // unknown option only
                if (strStartWith(previous, "--custom-chapters") && paramPair.size() == 1 &&
                    paramPair[0].find_first_not_of("0123456789:.;") == std::string::npos)
                    THROW(ERR_COMMON, previous << " is followed by " << paramPair[0]
                                               << " on the MUXOPT line: write the chapter times without spaces, such "
                                                  "as --custom-chapters=00:00:01.000;00:00:03.000, or put the list "
                                                  "in quotes.")
                previous = paramPair.size() > 1 ? trimStr(param) : std::string();
                // ** A NUMBER IS READ WHOLE OR REFUSED. ** These were read up to their first other character, so
                // --start-time=10s was 10 ticks of 45 kHz, --vbv-len=0.5s was 0, --extra-iso-space=64K was 64 and -50
                // stayed -50, and --auto-chapters=5,5 was 5, each without a word. They are checked here, before
                // anything is written; the muxers read them as before.
                if (paramPair.size() > 1)
                {
                    const std::string v = trimStr(paramPair[1]);
                    const bool digits = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
                    if (paramPair[0] == "--start-time" && v.find(':') == std::string::npos && !digits)
                        THROW(ERR_COMMON, "--start-time=" << v
                                                          << " is neither a number of 45 kHz ticks nor a time written "
                                                             "hh:mm:ss.zzz, such as --start-time=00:10:00.")
                    // A clock time is read as a chapter time is, minutes and seconds below 60: 99:99:99 was taken as
                    // 100:40:39 and 00:00:10s as 10 s. Ticks beyond 15 digits did not fit when doubled to 90 kHz.
                    if (paramPair[0] == "--start-time" && v.find(':') != std::string::npos)
                        chapterTimeToSeconds(v, "--start-time");
                    if (paramPair[0] == "--start-time" && digits && v.size() > 15)
                        THROW(ERR_COMMON, "--start-time=" << v << " is past the clock's limit of about 26.5 hours.")
                    // The numbers of a disc's files as well: --mplsOffset=5abc was read as 5, --m2tsOffset=1e3 as 1
                    // and 0x10 as 0, without a word
                    if ((paramPair[0] == "--vbv-len" || paramPair[0] == "--extra-iso-space" ||
                         paramPair[0] == "--auto-chapters" || paramPair[0] == "--mplsOffset" ||
                         paramPair[0] == "--m2tsOffset" || paramPair[0] == "--blankOffset") &&
                        !digits)
                        THROW(ERR_COMMON, paramPair[0]
                                              << "=" << v
                                              << " is not a whole number of 0 or more. Write digits alone, such as "
                                              << (paramPair[0] == "--vbv-len"           ? "--vbv-len=500 (ms)"
                                                  : paramPair[0] == "--extra-iso-space" ? "--extra-iso-space=64"
                                                  : paramPair[0] == "--auto-chapters"   ? "--auto-chapters=5"
                                                  : paramPair[0] == "--m2tsOffset"      ? "--m2tsOffset=100"
                                                                                        : paramPair[0] + "=5")
                                              << ".")
                    // strtol held a larger --vbv-len at 2147483647 ms without a word
                    if (paramPair[0] == "--vbv-len" && (v.size() > 10 || strtoll(v.c_str(), nullptr, 10) > INT_MAX))
                        THROW(ERR_COMMON, "--vbv-len=" << v << " is too large: the most is 2147483647 (ms).")
                    if (paramPair[0] == "--extra-iso-space")
                        if (const std::string why = IsoWriter::tooManyExtraBlocks(
                                v.size() > 18 ? INT64_MAX : strtoll(v.c_str(), nullptr, 10));
                            !why.empty())
                            THROW(ERR_COMMON, why)
                }
                // ** AN OPTION WRITTEN WITH A SPACE INSTEAD OF AN EQUALS SIGN READ PAST THE END OF THE
                // VECTOR. ** splitStr on "--auto-chapters" alone yields one element, and every branch
                // below reached for the second. That is undefined behaviour, so it crashed on some
                // options and quietly read rubbish on others; three of these segfaulted outright.
                // --custom-chapters, in this same block, always had the guard, and --cut-start already
                // words the refusal, so both the shape and the sentence are the ones already here.
                if (paramPair[0] == "--auto-chapters")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    // Minutes between chapters. A negative value put chapters before the start of the
                    // film, and from about 35.8 million minutes on the seconds no longer fitted. Every
                    // value longer than a playlist can last (its times are 32-bit counts of 1/45,000 s,
                    // about 26.5 hours) gives the same single chapter, so the largest ones are held at
                    // the most the seconds can take.
                    const int64_t minutes = strToInt64(paramPair[1].c_str());
                    if (minutes < 0)
                        THROW(ERR_COMMON,
                              "The value of --auto-chapters is a number of minutes, 0 or more, not " << paramPair[1])
                    autoChapterLen = static_cast<int>(std::min<int64_t>(minutes, INT_MAX / 60) * 60);
                }
                else if (paramPair[0] == "--custom-chapters" && paramPair.size() > 1)
                {
                    // A list in quotes, which may have spaces after its semicolons, was read with the quotes as part
                    // of its first and last times, and refused showing them
                    vector<string> chapList = splitStr(unquoteStr(trimStr(paramPair[1])).c_str(), ';');
                    // An empty place between two semicolons is no chapter: it was read as a chapter at 0:00
                    for (const string& chap : chapList)
                        if (!trimStr(chap).empty())
                            customChaptersList.push_back(chapterTimeToSeconds(trimStr(chap), "--custom-chapters"));
                }
                else if (paramPair[0] == "--chapters-file")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    // A list in a meta line is times only. A file carries the names as well, so the
                    // chapters are kept whole here and the times are taken from them, rather than
                    // the other way round. A second file replaced the first without a word: one is taken.
                    if (fromChapterFile)
                        THROW(ERR_COMMON, "--chapters-file is given more than once. Give one chapter file.")
                    chapterInfo = readChapterFile(unquoteStr(paramPair[1]));
                    fromChapterFile = true;
                }
                else if (paramPair[0] == "--mplsOffset")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    firstMplsOffset = strToInt32(paramPair[1].c_str());
                    if (firstMplsOffset < 0 || firstMplsOffset > 1999)
                        THROW(ERR_COMMON,
                              "The playlist number of --mplsOffset must be 0 to 1999, not " << firstMplsOffset)
                }
                else if (paramPair[0] == "--blankOffset")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    blankNum = strToInt32(paramPair[1].c_str());
                    if (blankNum < 0 || blankNum > 1999)
                        THROW(ERR_COMMON,
                              "The blank playlist number of --blankOffset must be 0 to 1999, not " << blankNum)
                }
                else if (paramPair[0] == "--m2tsOffset")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    firstM2tsOffset = strToInt32(paramPair[1].c_str());
                    if (firstM2tsOffset < 0 || firstM2tsOffset > 99999)
                        THROW(ERR_COMMON, "The clip number of --m2tsOffset must be 0 to 99999, not " << firstM2tsOffset)
                }
                else if (paramPair[0] == "--insertBlankPL")
                    insertBlankPL = true;
                else if (paramPair[0] == "--label")
                {
                    if (paramPair.size() < 2)
                        THROW(ERR_COMMON, "Missing value for " << paramPair[0])
                    // The whole text after the first '=': a label with '=' in it ("Rock=Roll") was cut to "Rock".
                    const std::string whole = trimStr(param);
                    isoDiskLabel = whole.substr(whole.find('=') + 1);
                }
                else if (paramPair[0] == "--demux")
                    demux = true;

                // Every MUXOPT line counts, as it does for the muxers: a line without a disc option keeps the disc a
                // line before it asked for. Only the last line decided, so --blu-ray on the first of two lines wrote
                // no image and reported success, or a demux folder where a disc folder was asked for. The options are
                // taken as the muxers read them: the disc was looked for in the text of the line, so a label holding
                // "--avchd" next to --blu-ray was refused as both discs, and --label="no --blu-ray here"
                // alone made a Blu-ray disc.
                const DiskType optionType = paramPair[0] == "--blu-ray" || paramPair[0] == "--blu-ray-v3"
                                                ? DiskType::BLURAY
                                            : paramPair[0] == "--avchd" ? DiskType::AVCHD
                                                                        : DiskType::NONE;
                if (paramPair[0] == "--blu-ray-v3")
                    V3_flags |= HDMV_V3;
                if (optionType != DiskType::NONE)
                {
                    if (result != DiskType::NONE && result != optionType)
                        THROW(ERR_COMMON,
                              "The MUXOPT lines ask for both a Blu-ray disc and an AVCHD disc. Give one "
                              "of --blu-ray and --avchd.")
                    result = optionType;
                }
            }
        }
        else if (strStartWith(str, "V_MPEG4/ISO/MVC"))
            stereoMode = true;

        file.readLine(str);
    }

    // ** ONE LIST OF CHAPTERS, IN TIME ORDER, WHATEVER ORDER THE OPTIONS AND THE FILE GAVE. ** --chapters-file
    // replaced the --custom-chapters before it on the line and was added to by those after it, so the two orders gave
    // two results; and a chapter file is keyed by its chapter numbers, so a file whose numbers were not in time order,
    // or a typed list out of order, put marks on a disc out of order and twice at one time. The chapters of the file
    // and the typed times are now one list, sorted by time, with one chapter at each time (the first given, a named
    // one before a typed one).
    if (fromChapterFile)
    {
        for (const double t : customChaptersList) chapterInfo.emplace_back(llround(t * 1e9), std::string());
        std::stable_sort(chapterInfo.begin(), chapterInfo.end());
        chapterInfo.erase(std::unique(chapterInfo.begin(), chapterInfo.end(),
                                      [](const AVChapter& a, const AVChapter& b) { return a.start == b.start; }),
                          chapterInfo.end());
        customChaptersList.clear();
        for (const auto& chapter : chapterInfo) customChaptersList.push_back(static_cast<double>(chapter.start) / 1e9);
    }
    else
    {
        std::sort(customChaptersList.begin(), customChaptersList.end());
        customChaptersList.erase(std::unique(customChaptersList.begin(), customChaptersList.end()),
                                 customChaptersList.end());
    }
    // The blank playlist is written under its number twice: as a playlist and as the clip it plays. The same number as
    // the main playlist, or as the main title's first clip, replaced that file without a word (in an image, the blank
    // was added to it). Both are known now, before anything is written. A clip of a later part of a split is known
    // only after the mux, and is checked there (blankPlaylistClash). A 3D disc gets no blank playlist.
    if (result != DiskType::NONE && insertBlankPL && !stereoMode)
    {
        if (blankNum == firstMplsOffset)
            THROW(ERR_COMMON,
                  "The blank playlist (--blankOffset) and the main playlist (--mplsOffset) both have the number "
                      << strPadLeft(int32ToStr(blankNum), 5, '0')
                      << ", and the blank one would replace the main one. Give --blankOffset another number.")
        if (blankNum == firstM2tsOffset)
            THROW(ERR_COMMON,
                  "The blank playlist's clip (--blankOffset) and the first clip of the main title "
                  "(--m2tsOffset) both have the number "
                      << strPadLeft(int32ToStr(blankNum), 5, '0')
                      << ", and the blank one would replace it. Give --blankOffset a number that the main "
                         "title's clips do not use.")
    }
    return result;
}

// A Blu-ray playlist holds at most 999 marks. More were written into a buffer of fixed size at the end of the mux,
// which stopped there with "Bitstream exception", after all the work; 1000 were written in silence. They are counted
// before anything is written. A chapter past the end of the title gets no mark, and counted all the same, 1200
// chapters a second apart on a 5 s title were refused: when the sources state their length, only the chapters
// inside it count. A raw stream states none, and then every chapter given counts, as the message says.
// createMPLSFile refuses more than 999 marks as well, for a length that was stated wrong. A list with no time at 0 gets
// a mark at the start of the title as well (createMPLSFile), and it counts.
static void refuseTooManyMarks(MuxerManager& muxerManager, const vector<double>& chapters)
{
    constexpr size_t MAX_MARKS = 999;
    const size_t startMark = !chapters.empty() && chapters.front() > 0.0 ? 1 : 0;
    if (chapters.size() + startMark <= MAX_MARKS)
        return;
    const auto seconds = [](const int64_t pts) { return static_cast<double>(pts) / INTERNAL_PTS_FREQ; };
    const double stated = static_cast<double>(muxerManager.statedDurationNano()) / 1e9;
    const double start = muxerManager.getCutStart() > 0 ? seconds(muxerManager.getCutStart()) : 0.0;
    const double end = muxerManager.getCutEnd() > 0 ? seconds(muxerManager.getCutEnd()) : stated;
    if (end > start)
    {
        const auto inside = static_cast<size_t>(
            std::count_if(chapters.begin(), chapters.end(), [&](const double t) { return t <= end - start; }));
        if (inside + startMark > MAX_MARKS)
            THROW(ERR_COMMON,
                  "A Blu-ray playlist holds at most 999 chapter marks, and the chapters given within the "
                  "title are "
                      << inside + startMark << (startMark ? ", with the mark at its start" : "")
                      << ". Give fewer chapters.")
        return;
    }
    THROW(ERR_COMMON, "A Blu-ray playlist holds at most 999 chapter marks, and the chapters given are "
                          << chapters.size() + startMark
                          << (startMark ? ", with the mark at the start of the title" : "")
                          << ". The length of the title is not known before the mux, as its sources do not state it, "
                             "so chapters past its end count too. Give fewer chapters.")
}

// The blank playlist's clip takes the number of --blankOffset. A split gives the main title one clip a part, numbered
// on from --m2tsOffset, so a blank number that is one of them would replace that part. Checked before the blank is
// written; the rest of the disc is complete by then.
static void blankPlaylistClash(const TSMuxer& mainMuxer, const int blankNum)
{
    const int first = mainMuxer.getFirstFileNum();
    const int last = first + static_cast<int>(mainMuxer.splitFileCnt()) - 1;
    if (blankNum >= first && blankNum <= last)
        THROW(ERR_COMMON, "The blank playlist was not written: its clip number "
                              << strPadLeft(int32ToStr(blankNum), 5, '0')
                              << " (--blankOffset) is one of the main title's clips, "
                              << strPadLeft(int32ToStr(first), 5, '0') << " to " << strPadLeft(int32ToStr(last), 5, '0')
                              << ", and would have replaced it. The rest of the disc is complete. Give --blankOffset a "
                                 "number outside that range.")
}

// A clip of a playlist after the first: the media file to probe and its clip info.
struct LaterClip
{
    std::string media;
    std::string clipInfo;
    bool chosen = false;  // the clip of the play item whose STN table describes the playlist (chosenStnItem)
};

// A playlist may open with a clip that does not carry every stream: a short logo without the film's audio, for one.
// The playlist declares the rest and later clips carry them, so a stream the first clip lacks is listed from the
// first later clip that has it. It used to be left out of the listing, and the window could not offer it at all.
// A later clip is read only when its clip info lists a stream still missing, or has no clip info that can be read;
// one that cannot be read adds nothing. The rows keep the order a single clip gives: video first, then the rest,
// each by track ID.
static void addTracksFromLaterClips(vector<CheckStreamRez>& streams, const MPLSParser& mpls, const string& firstClip,
                                    const vector<LaterClip>& laterClips)
{
    std::set<int> wanted;
    for (const MPLSStreamInfo& si : mpls.m_streamInfo)
        if (si.type != 2 && si.streamPID != 0)  // a type 2 entry lives in a sub path clip, not in the play item's
            wanted.insert(si.streamPID);
    for (const CheckStreamRez& s : streams) wanted.erase(s.trackID);
    if (wanted.empty())
        return;

    std::set<string> probed{firstClip};
    for (const LaterClip& clip : laterClips)
    {
        if (wanted.empty())
            break;
        if (!probed.insert(clip.media).second)
            continue;
        CLPIParser clpi;
        if (clpi.parse(clip.clipInfo.c_str()) &&
            std::none_of(wanted.begin(), wanted.end(), [&](const int pid) { return clpi.m_streamInfo.count(pid) > 0; }))
            continue;
        DetectStreamRez later;
        try
        {
            later = METADemuxer::DetectStreamReader(readManager, clip.media, false);
        }
        catch (...)
        {
            continue;
        }
        std::set<int> found;
        for (const CheckStreamRez& s : later.streams)
            if (wanted.count(s.trackID))
            {
                streams.push_back(s);
                found.insert(s.trackID);
            }
        for (const int pid : found) wanted.erase(pid);
    }
    std::stable_sort(streams.begin(), streams.end(),
                     [](const CheckStreamRez& a, const CheckStreamRez& b)
                     {
                         const bool videoA = strStartWith(a.codecInfo.programName, "V_");
                         const bool videoB = strStartWith(b.codecInfo.programName, "V_");
                         if (videoA != videoB)
                             return videoA;
                         return a.trackID < b.trackID;
                     });
}

// A player offers the tracks of a playlist in the order of its STN table: audio stream 1 is the first primary audio
// entry, whatever its PID. A mux gives out the PIDs of each kind of track in the order of the meta lines, which the
// window takes from the listing. The listing gave the tracks by PID, so a disc whose table orders them otherwise was
// remuxed with another first audio track: English where the disc offers French first. The tracks of each list of the
// table (primary audio, subtitles, secondary audio and so on) now take the places those tracks had, in the order of
// the table. Video still comes first, and a track the table does not name keeps its place.
static void orderAsPlaylist(vector<CheckStreamRez>& streams, const MPLSParser& mpls)
{
    // The lists in the order STN_table() stores them; the picture-in-picture subtitles follow the others in one list
    const int counts[] = {mpls.number_of_primary_video_stream_entries,
                          mpls.number_of_primary_audio_stream_entries,
                          mpls.number_of_PG_textST_stream_entries + mpls.number_of_PiP_PG_textST_stream_entries_plus,
                          mpls.number_of_IG_stream_entries,
                          mpls.number_of_secondary_audio_stream_entries,
                          mpls.number_of_secondary_video_stream_entries,
                          mpls.number_of_DolbyVision_video_stream_entries};
    // The list and the place in the table of each PID of the play item's clip; the first entry wins, as in
    // getStreamByPID
    std::map<int, std::pair<int, size_t>> places;
    size_t index = 0;
    for (int list = 0; list < static_cast<int>(std::size(counts)); ++list)
        for (int k = 0; k < counts[list] && index < mpls.m_streamInfo.size(); ++k, ++index)
        {
            const MPLSStreamInfo& entry = mpls.m_streamInfo[index];
            if (entry.streamPID && mpls.isInMainClip(entry))
                places.emplace(entry.streamPID, std::make_pair(list, index));
        }

    for (int list = 0; list < static_cast<int>(std::size(counts)); ++list)
    {
        std::vector<size_t> slots;
        std::vector<CheckStreamRez> tracks;
        for (size_t i = 0; i < streams.size(); ++i)
        {
            const auto itr = places.find(streams[i].trackID);
            if (itr != places.end() && itr->second.first == list)
            {
                slots.push_back(i);
                tracks.push_back(streams[i]);
            }
        }
        std::stable_sort(tracks.begin(), tracks.end(), [&](const CheckStreamRez& a, const CheckStreamRez& b)
                         { return places.at(a.trackID).second < places.at(b.trackID).second; });
        for (size_t k = 0; k < slots.size(); ++k) streams[slots[k]] = std::move(tracks[k]);
    }
}

// The playlist's details (languages, track order) come from its longest play item (chosenStnItem), but every track
// was described as the first clip has it. A short first clip whose sound is coded otherwise, a stereo intro before the
// 5.1 film, listed "Channels: 2" for the film's track. An audio track whose clip information gives the two clips
// another coding, layout or sample rate is now described as the chosen clip has it. Only then is that clip read, and
// quietly: what its readers report belongs to no line of the listing.
static void describeAsChosenClip(vector<CheckStreamRez>& streams, const string& firstClipInfo,
                                 const vector<LaterClip>& laterClips)
{
    const auto clip = std::find_if(laterClips.begin(), laterClips.end(), [](const LaterClip& c) { return c.chosen; });
    if (clip == laterClips.end() || firstClipInfo.empty() || clip->clipInfo == firstClipInfo)
        return;
    CLPIParser first, chosen;
    if (!first.parse(firstClipInfo.c_str()) || !chosen.parse(clip->clipInfo.c_str()))
        return;
    std::set<int> differ;
    for (const auto& [pid, a] : first.m_streamInfo)
    {
        const auto itr = chosen.m_streamInfo.find(pid);
        if (itr != chosen.m_streamInfo.end() && isAudioStreamType(a.stream_coding_type) &&
            (itr->second.stream_coding_type != a.stream_coding_type ||
             itr->second.audio_presentation_type != a.audio_presentation_type ||
             itr->second.sampling_frequency_index != a.sampling_frequency_index))
            differ.insert(pid);
    }
    if (differ.empty())
        return;
    DetectStreamRez rez;
    std::streambuf* const out = std::cout.rdbuf(nullptr);
    std::streambuf* const err = std::cerr.rdbuf(nullptr);
    const bool lastMsg = sLastMsg;
    try
    {
        rez = METADemuxer::DetectStreamReader(readManager, clip->media, false);
    }
    catch (...)
    {
    }
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
    sLastMsg = lastMsg;
    for (CheckStreamRez& s : streams)
    {
        if (!differ.count(s.trackID))
            continue;
        for (const CheckStreamRez& c : rez.streams)
            if (c.trackID == s.trackID && c.codecInfo.codecID == s.codecInfo.codecID && c.subTrack == s.subTrack &&
                !c.streamDescr.empty())
            {
                s.streamDescr = c.streamDescr;
                break;
            }
    }
}

void detectStreamReader(const char* fileName, MPLSParser* mplsParser, bool isSubMode,
                        const vector<LaterClip>* laterClips = nullptr, const string& firstClipInfo = string())
{
    // The later clips of a playlist also give a frame rate that a short first clip cannot give: the listing shows the
    // rate the mux takes, and the window writes it
    std::vector<std::string> laterFiles;
    if (mplsParser && laterClips)
        for (const LaterClip& clip : *laterClips) laterFiles.push_back(clip.media);
    DetectStreamRez streamInfo =
        METADemuxer::DetectStreamReader(readManager, fileName, mplsParser == nullptr, laterFiles);
    vector<CheckStreamRez>& streams = streamInfo.streams;
    if (mplsParser && laterClips && !laterClips->empty())
    {
        addTracksFromLaterClips(streams, *mplsParser, fileName, *laterClips);
        describeAsChosenClip(streams, firstClipInfo, *laterClips);
    }
    if (mplsParser)
        orderAsPlaylist(streams, *mplsParser);

    // A 3D disc's .m2ts names ONE view, so this file may be half a picture. The disc says so
    // in its playlists; say it too, on the video line, rather than leaving someone to find out
    // when the 3D they built plays flat.
    bool ssifIsBaseView = false;
    const std::string ssifPartner = bdFindSsifForM2ts(fileName, ssifIsBaseView);

    // A raw elementary stream has nowhere but its file name to carry a language or a delay,
    // and the muxer reads both from there. THE LISTING NEVER SAID SO. The window builds its
    // track table from these lines, so a file that was going to be tagged correctly showed an
    // empty language, and the only way to find out was to mux it and look afterwards.
    //
    // trackID is 0 exactly when there is no container track numbering, which is the same
    // condition the muxer uses to decide that the name is the only source there is.
    const std::string nameLang = langFromFileName(fileName);
    const int64_t nameDelay = delayFromFileName(fileName);

    for (unsigned i = 0; i < streams.size(); i++)
    {
        if (streams[i].trackID == 0)
        {
            // Only ever fill a gap. Anything the stream itself declared wins, exactly as an
            // explicit lang= or timeshift= wins over the name when the mux runs.
            if (streams[i].lang.empty())
                streams[i].lang = nameLang;
            if (streams[i].delay == 0)
                streams[i].delay = nameDelay;
        }
        if (streams[i].trackID != 0)
        {
            if (i > 0)
                LTRACE(LT_INFO, 2, "");
            LTRACE(LT_INFO, 2, "Track ID:    " << streams[i].trackID);
        }
        if (streams[i].codecInfo.codecID)
        {
            if (mplsParser)
            {
                MPLSStreamInfo mplsStreamInfo = mplsParser->getStreamByPID(streams[i].trackID);
                if (mplsStreamInfo.streamPID)
                {
                    if (mplsStreamInfo.isSecondary)
                        streams[i].isSecondary = true;
                    // A disc says what language a stream is in TWICE, in the playlist and in the
                    // clip's CLPI, and a re-authored disc can have the two disagree. The language
                    // came from the CLPI alone, so such a disc was reported in the language the
                    // clip was originally in rather than the one the playlist offers.
                    //
                    // The playlist wins here, because it is what a player presents to the viewer
                    // and what the disc's own navigation uses. A bare .m2ts opened on its own has
                    // no playlist, so nothing changes there: the CLPI is still the only source.
                    const std::string mplsLang = applyLangCodeStyle(mplsStreamInfo.language_code);
                    if (isLanguageCode(mplsLang) && mplsLang != "und" && mplsLang != streams[i].lang)
                    {
                        LTRACE(LT_INFO, 2,
                               "Language for track " << streams[i].trackID << " taken from the playlist, " << mplsLang
                                                     << ", where the clip says "
                                                     << (streams[i].lang.empty() ? "nothing" : streams[i].lang));
                        streams[i].lang = mplsLang;
                    }
                }
                else
                {
                    if (!(streams[i].codecInfo.codecID == CODEC_V_MPEG4_H264_DEP && mplsParser->isDependStreamExist))
                        streams[i].unused = true;
                }
            }

            string postfix;
            if (isSubMode && streams[i].codecInfo.codecID == CODEC_S_PGS)
                postfix = " (depended view)";
            LTRACE(LT_INFO, 2, "Stream type: " << streams[i].codecInfo.displayName << postfix);
            if (streams[i].isSecondary)
                LTRACE(LT_INFO, 2, "Secondary: 1");
            if (streams[i].unused)
                LTRACE(LT_INFO, 2, "Unselected: 1");

            LTRACE(LT_INFO, 2, "Stream ID:   " << streams[i].codecInfo.programName);
            std::string descr = streams[i].streamDescr;
            // The subtitles of the picture-in-picture video were listed as ordinary ones, numbered after the film's
            if (streams[i].codecInfo.codecID == CODEC_S_PGS && mplsParser &&
                mplsParser->getStreamByPID(streams[i].trackID).isPiP)
                descr += "   picture-in-picture subtitle";
            if (streams[i].codecInfo.codecID == CODEC_S_PGS && mplsParser && mplsParser->isDependStreamExist)
            {
                // PG stream
                MPLSStreamInfo mplsStreamInfo = mplsParser->getStreamByPID(streams[i].trackID);
                int pgTrackNum = mplsStreamInfo.streamPID - 0x1200;
                if (pgTrackNum >= 0)
                {
                    if (mplsStreamInfo.offsetId != 0xff)
                    {
                        descr += "   3d-plane: ";
                        descr += int32ToStr(mplsStreamInfo.offsetId);
                    }
                    else
                    {
                        descr += "   3d-plane: undefined";
                    }
                    if (mplsStreamInfo.isSSPG)
                    {
                        descr += "   (stereo, right=";
                        descr += (mplsStreamInfo.rightEye->type == 2 ? "dep-view " : "");
                        descr += int32ToStr(mplsStreamInfo.rightEye->streamPID);
                        descr += ", left=";
                        descr += (mplsStreamInfo.leftEye->type == 2 ? "dep-view " : "");
                        descr += int32ToStr(mplsStreamInfo.leftEye->streamPID);
                        descr += ")";
                    }
                    for (int a = 0; a < 2; ++a)
                    {
                        const MPLSStreamInfo::AlignedRef& ref = a == 0 ? mplsStreamInfo.topAS : mplsStreamInfo.bottomAS;
                        if (ref.pid)
                            descr += std::string(a == 0 ? "   (top aligned=" : "   (bottom aligned=") +
                                     (ref.type == 2 ? "dep-view " : "") + int32ToStr(ref.pid) + ")";
                    }
                }
                else
                {
                    // A stream the 3D table names as an eye of a stereo subtitle or as an aligned subtitle is muxed
                    // with the track it belongs to, in that track's language. It was listed as an unrelated subtitle
                    // in the clip's language.
                    std::string role;
                    const MPLSStreamInfo parent = mplsParser->getStreamReferencing(streams[i].trackID, role);
                    if (parent.streamPID)
                    {
                        descr += "   (3D " + role + " subtitle of track " + int32ToStr(parent.streamPID) +
                                 ", muxed with it)";
                        const std::string parentLang = applyLangCodeStyle(parent.language_code);
                        if (isLanguageCode(parentLang) && parentLang != "und")
                            streams[i].lang = parentLang;
                    }
                    else
                        descr += "   (disabled)";
                }
            }
            // Both halves are worth telling. The base view looks like ordinary 2D video, and
            // the dependent view is recognisable but says nothing about where its partner is.
            if (!ssifPartner.empty() && (streams[i].codecInfo.codecID == CODEC_V_MPEG4_H264 ||
                                         streams[i].codecInfo.codecID == CODEC_V_MPEG4_H264_DEP))
                descr += ssifIsBaseView ? "   3D: base view only, both views are in " + ssifPartner
                                        : "   3D: dependent view only, both views are in " + ssifPartner;
            LTRACE(LT_INFO, 2, "Stream info: " << descr);
            LTRACE(LT_INFO, 2, "Stream lang: " << streams[i].lang);
            // A Matroska track's name and the flags it has set, under the names the meta takes, so the window can
            // start from them. The name is the file's own text, on one line.
            if (!streams[i].name.empty())
            {
                std::string name = streams[i].name;
                for (char& c : name)
                    if (c == '\r' || c == '\n')
                        c = ' ';
                LTRACE(LT_INFO, 2, "Stream name: " << name);
            }
            if (!streams[i].flags.empty())
            {
                static const std::pair<const char*, const char*> NAMES[] = {
                    {"forced=1", "forced"},          {"hearing=1", "hearing-impaired"},
                    {"visual=1", "visual-impaired"}, {"descriptions=1", "text-descriptions"},
                    {"original=1", "original"},      {"commentary=1", "commentary"}};
                std::string set;
                for (const std::string& item : splitStr(streams[i].flags.c_str(), ','))
                    for (const auto& [src, option] : NAMES)
                        if (item == src)
                            set += std::string(set.empty() ? "" : ", ") + option;
                if (!set.empty())
                    LTRACE(LT_INFO, 2, "Stream flags: " << set);
            }
            if (streams[i].delay)
                LTRACE(LT_INFO, 2, "Stream delay: " << streams[i].delay);
            if (streams[i].multiSubStream)
            {
                // Print what the entry says it is. Deriving the number from the codec id worked
                // only while the two halves of a combined track had different ids; a merged dual
                // layer Dolby Vision track is two HEVC entries, and its numbering runs the
                // opposite way round from the MVC one.
                const int subTrack = streams[i].subTrack != 0
                                         ? streams[i].subTrack
                                         : (streams[i].codecInfo.codecID == CODEC_V_MPEG4_H264_DEP ? 1 : 2);
                LTRACE(LT_INFO, 2, "subTrack: " << subTrack);
            }
        }
        else
        {
            // No reader accepted this track. When it came from a TS/M2TS we still know what the
            // PMT called it, so say so instead of leaving the user with a bare failure. These
            // lines deliberately avoid the "Stream type: "/"Stream ID:   " prefixes the GUI
            // parses, so an unmuxable track is reported without being offered as selectable.
            const char* known = nullptr;
            switch (static_cast<StreamType>(streams[i].containerStreamType))
            {
            case StreamType::SUB_IGS:
                known = "Interactive Graphics, the disc's menu overlay";
                break;
            case StreamType::SUB_TGS:
                known = "Text subtitles (Text-ST)";
                break;
            case StreamType::PRIVATE_DATA:
                // 0x06 on its own says only "private data". The descriptor beside it says what
                // that data really is, and without it this track was reported as an unknown
                // number, which is what issue 878 describes as being ignored.
                if (streams[i].containerDvbDescriptor == static_cast<int>(TSDescriptorTag::DVB_SUBTITLING))
                    known = "DVB bitmap subtitles (EN 300 743)";
                else if (streams[i].containerDvbDescriptor == static_cast<int>(TSDescriptorTag::DVB_TELETEXT))
                    known = "EBU teletext (EN 300 468)";
                break;
            default:
                break;
            }
            if (!streams[i].notSupported.empty())
                LTRACE(LT_INFO, 2,
                       "Not supported: " << streams[i].notSupported
                                         << ". Muxing this kind of track is not implemented, so the track is skipped.");
            else if (known)
            {
                // The descriptor goes in the line as well as the stream type. The window rebuilds
                // this sentence in its own language from the numbers, and stream type 0x06 on its
                // own says only "private data": the descriptor beside it is the whole difference
                // between subtitles and teletext, so without it the window cannot translate them.
                std::ostringstream codes;
                codes << "stream type 0x" << std::hex << streams[i].containerStreamType << std::dec;
                if (streams[i].containerDvbDescriptor)
                    codes << ", descriptor 0x" << std::hex << streams[i].containerDvbDescriptor << std::dec;
                LTRACE(LT_INFO, 2,
                       "Not supported: " << known << " (" << codes.str()
                                         << "). Muxing this stream type is not implemented, "
                                            "so the track is skipped.");
            }
            else if (streams[i].containerStreamType)
                LTRACE(LT_INFO, 2,
                       "Can't detect stream type (the container declares stream type 0x"
                           << std::hex << streams[i].containerStreamType << std::dec << ")");
            // A Matroska track says what it is by its CodecID, which is worth passing on: "V_FFV1" says why at once,
            // and for a codec tsMuxeR reads it points at the data instead.
            else if (!streams[i].codecId.empty())
                LTRACE(LT_INFO, 2, "Can't detect stream type (Matroska codec " << streams[i].codecId << ")");
            else
                LTRACE(LT_INFO, 2, "Can't detect stream type");
        }
    }

    // The playlist's streams that are not in its clips but in a clip a sub path plays, as a disc's text subtitles
    // always are. They were left out of the listing without a word, so a playlist with subtitles of that kind seemed
    // to have none, and the user could not know a track was dropped. Under "Not supported:", which the window shows
    // as a track that is not offered (it rebuilds the text for a text subtitle from the stream type). Subtitles, audio
    // (the music of a slideshow) and video (a picture-in-picture of a clip of its own): a pop-up menu in a clip of its
    // own is how most discs carry it, and is no track a mux could have had.
    if (mplsParser)
    {
        for (const MPLSStreamInfo& entry : mplsParser->getOutOfMuxStreams())
        {
            const auto codingType = static_cast<int>(entry.stream_coding_type);
            const std::string kind = MPLSParser::outOfMuxTrackKind(entry);
            if (codingType != 0x90 && codingType != 0x92 && kind.empty())
                continue;
            std::string clip;
            if (entry.refSubPathId >= 0 && static_cast<size_t>(entry.refSubPathId) < mplsParser->m_subPaths.size() &&
                !mplsParser->m_subPaths[entry.refSubPathId].clips.empty())
                clip = "clip " + mplsParser->m_subPaths[entry.refSubPathId].clips[0] + ", ";
            const std::string lang = applyLangCodeStyle(entry.language_code);
            std::ostringstream where;
            where << "stream type 0x" << std::hex << codingType << ", PID 0x" << entry.streamPID << std::dec
                  << (isLanguageCode(lang) ? ", language " + lang : std::string()) << ", " << clip << "sub path "
                  << entry.refSubPathId;
            const std::string what = codingType == 0x92   ? "Text subtitles (Text-ST)"
                                     : codingType == 0x90 ? "Presentation graphics subtitles in a separate clip"
                                                          : kind + " in a separate clip";
            LTRACE(LT_INFO, 2, "");
            LTRACE(LT_INFO, 2,
                   "Not supported: " << what << " (" << where.str()
                                     << "). The playlist plays it from a clip of its own, and a stream of such a "
                                        "clip is not muxed from the playlist, so the track is skipped.");
        }
    }

    AVChapters& chapters = streamInfo.chapters;
    // The application that wrote a Matroska file, on one line: the window asks whether an AC-3 track beside a TrueHD
    // track is its core, and in a file tsMuxeR wrote it is. Control characters of the file's text are left out.
    std::string writingApp;
    for (const char c : streamInfo.writingApp)
        if (static_cast<unsigned char>(c) >= 0x20)
            writingApp += c;
    const bool timeLines =
        !chapters.empty() || streamInfo.fileDurationNano > 0 || streamInfo.videoEndNano > 0 || !writingApp.empty();
    if (timeLines)
        LTRACE(LT_INFO, 2, "");
    if (!writingApp.empty())
        LTRACE(LT_INFO, 2, "Writing application: " << writingApp);
    if (streamInfo.fileDurationNano)
        LTRACE(LT_INFO, 2, "Duration: " << floatToTime((double)streamInfo.fileDurationNano / 1e9));
    // Where the video starts and ends on the file's own clock. A join puts the next file where the video ends, which
    // is not always where the Duration ends, and the window places the chapters of joined files by these two lines.
    if (streamInfo.videoEndNano > 0)
    {
        LTRACE(LT_INFO, 2, "Video start: " << floatToTime((double)streamInfo.videoStartNano / 1e9));
        LTRACE(LT_INFO, 2, "Video end: " << floatToTime((double)streamInfo.videoEndNano / 1e9));
    }
    for (size_t j = 0; j < chapters.size(); j++)
    {
        uint64_t time = chapters[j].start;
        if (j % 5 == 0)
        {
            LTRACE(LT_INFO, 2, "");
            LTRACE2(LT_INFO, "Marks: ")
        }
        LTRACE2(LT_INFO, floatToTime((double)time / 1e9) << " ")
    }
    if (timeLines)
        LTRACE(LT_INFO, 2, "");
}

// The same answer as the meta path gives, from one place (METADemuxer::playlistStreamDir).
string getBlurayStreamDir(const string& mplsName) { return METADemuxer::playlistStreamDir(mplsName); }

// A blank pattern with the frame rate in its sequence parameter set (the first NAL unit) set to fps, as the H.264
// reader sets it for fps= on a track: the pattern states 23.976 (or 29.97 or 25 when interlaced), and the blank of a
// title at another rate was decoded as "Frame rate: 23.976" with a line that the rate was changed.
static std::vector<uint8_t> blankPatternAt(uint8_t* pattern, const int patternSize, const double fps)
{
    uint8_t* const end = pattern + patternSize;
    uint8_t* const nal = pattern + 4;  // after the start code 00 00 00 01
    uint8_t* const spsEnd = NALUnit::findNALWithStartCode(nal, end, true);
    SPSUnit sps;
    sps.decodeBuffer(nal, spsEnd);
    std::vector<uint8_t> rez(static_cast<size_t>(patternSize) + 64);
    long spsLen = -1;
    if (sps.deserialize() == 0 && fabs(sps.getFPS() - fps) > 5e-5)  // the reader's margin (mpegStreamReader.cpp)
    {
        sps.setFps(fps);
        spsLen = sps.serializeBuffer(rez.data(), rez.data() + 64 + (spsEnd - pattern), true);
    }
    if (spsLen <= 0)
        return std::vector<uint8_t>(pattern, end);
    std::copy(spsEnd, end, rez.begin() + spsLen);
    rez.resize(static_cast<size_t>(spsLen) + (end - spsEnd));
    return rez;
}

void muxBlankPL(const string& appDir, BlurayHelper& blurayHelper, const PIDListMap& pidList, DiskType dt, int blankNum)
{
    unsigned videoWidth = 1920;
    unsigned videoHeight = 1080;
    double fps = 23.976;
    bool interlaced = false;
    for (const auto& [pid, si] : pidList)
    {
        const PMTStreamInfo& streamInfo = si;
        const auto streamReader = dynamic_cast<MPEGStreamReader*>(streamInfo.m_codecReader);
        if (streamReader)
        {
            videoWidth = streamReader->getStreamWidth();
            videoHeight = streamReader->getStreamHeight();
            if (streamReader->getFPS() > 0)
                fps = streamReader->getFPS();
            interlaced = streamReader->getInterlaced();
            break;
        }
    }
    // The blank has the title's frame rate and scan, so that a player keeps its output mode from the blank to the
    // title. It was 23.976 progressive for every title: a 480i title got a 480p blank, which is no disc format, and a
    // 1080i, 576i or 720p 59.94 title a blank in another mode.
    uint8_t* pattern;
    int patternSize;
    bool isNtsc = videoWidth <= 854 && videoHeight <= 480 && (fabs(25 - fps) >= 0.5 && fabs(50 - fps) >= 0.5);
    bool isPal = videoWidth <= 1024 && videoHeight <= 576 && (fabs(25 - fps) < 0.5 || fabs(50 - fps) < 0.5);
    if (isNtsc)
    {
        pattern = interlaced ? pattern_ntsc_i : pattern_ntsc;
        patternSize = interlaced ? sizeof(pattern_ntsc_i) : sizeof(pattern_ntsc);
    }
    else if (isPal)
    {
        pattern = interlaced ? pattern_pal_i : pattern_pal;
        patternSize = interlaced ? sizeof(pattern_pal_i) : sizeof(pattern_pal);
    }
    else if (videoWidth >= 1300)
    {
        pattern = interlaced ? pattern_1920_i : pattern_1920;
        patternSize = interlaced ? sizeof(pattern_1920_i) : sizeof(pattern_1920);
    }
    else
    {
        pattern = pattern_1280;
        patternSize = sizeof(pattern_1280);
    }
    auto fname_time = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::high_resolution_clock::now().time_since_epoch())
                          .count();
    string tmpFileName = appDir + string("blank_") + std::to_string(fname_time) + string(".264");
    File file;
    if (!file.open(tmpFileName.c_str(), File::ofWrite))
        THROW(ERR_COMMON, "can't create file " << tmpFileName)
    // About half a second, as three patterns of four pictures are at 23.976: more of them at a higher rate
    const std::vector<uint8_t> blank = blankPatternAt(pattern, patternSize, fps);
    const int repeats = (std::max)(3, static_cast<int>(lround(fps / 8.0)));
    for (int i = 0; i < repeats; ++i)
    {
        if (file.write(blank.data(), static_cast<uint32_t>(blank.size())) != static_cast<int>(blank.size()))
        {
            deleteFile(tmpFileName);
            THROW(ERR_COMMON, "can't write data to file " << tmpFileName)
        }
    }
    file.close();
    map<string, string> videoParams;
    videoParams["insertSEI"];
    std::ostringstream fpsText;
    fpsText << std::setprecision(15) << fps;
    videoParams["fps"] = fpsText.str();
    {
        MuxerManager muxerManager(readManager, tsMuxerFactory);
        // Its progress lines came after the finishing line of the main mux, from 0 percent again, with no finishing
        // line of their own: a window that shows the figure went back to 0 at the end of the job
        muxerManager.setQuiet();
        muxerManager.parseMuxOpt("MUXOPT --no-pcr-on-video-pid --vbr --avchd --vbv-len=500");
        muxerManager.addStream("V_MPEG4/ISO/AVC", tmpFileName, videoParams);
        string dstFile = blurayHelper.m2tsFileName(blankNum);
        muxerManager.doMux(dstFile, &blurayHelper);

        auto tsMuxer = dynamic_cast<TSMuxer*>(muxerManager.getMainMuxer());

        blurayHelper.createMPLSFile(tsMuxer, nullptr, 0, vector<double>(), dt, blankNum, false);
        blurayHelper.createCLPIFile(tsMuxer, blankNum, true);
    }
    deleteFile(tmpFileName);
}

void doTruncatedFile(const char* fileName, const int64_t offset)
{
    File f;
    File outFile;

    f.open(fileName, File::ofRead);
    const std::string outName = std::string(fileName) + std::string(".back");
    outFile.open(outName.c_str(), File::ofWrite);

    constexpr uint32_t bufSize = 1024 * 64;
    const auto buffer = new uint8_t[bufSize];
    f.seek(offset);
    int readed = f.read(buffer, bufSize);
    while (readed > 0)
    {
        outFile.write(buffer, readed);
        readed = f.read(buffer, bufSize);
    }
}

void showHelp()
{
    constexpr char help[] = R"help(
tsMuxeR is a simple program to mux video to TS/M2TS/MKV files or create BD disks.
tsMuxeR does not use external filters (codecs).

Examples:
    tsMuxeR <media file name>
    tsMuxeR <meta file name> <out file/dir name>

tsMuxeR can be run in track detection mode or muxing mode. If tsMuxeR is run
with only one argument, then the program displays track information required to
construct a meta file. When running with two arguments, tsMuxeR starts the
muxing or demuxing process.

Meta file format:
File MUST have the .meta extension and be encoded in UTF-8 (but see README.md).
This file defines the files you want to multiplex.
The first line of a meta file contains additional parameters that apply to all
tracks. In this case the first line should begin with the word MUXOPT.

The following lines form a list of tracks and their parameters.  The format is
as follows:   <code name>,   <file name>,   <parameters>   Parameters are
separated with commas, with each parameter consisting of a name and a value,
separated with an equals sign.
Example of META file:

MUXOPT --blu-ray
V_MPEG4/ISO/AVC, D:/media/test/stream.h264, fps=25
A_AC3, D:/media/test/stream.ac3, timeshift=-10000ms

In this example one AC3 audio stream and one H264 video stream are multiplexed
into BD disc. The input file name can reference an elementary stream or a track
located inside a container.

Supported input containers:
- TS/M2TS/MTS
- EVO/VOB/MPG/MPEG
- MKV
- MOV/MP4
- MPLS (Blu-ray media play list file)

Names of codecs in the meta file:
- V_MPEGI/ISO/VVC   H.266/VVC
- V_MPEGH/ISO/HEVC  H.265/HEVC
- V_MPEG4/ISO/AVC   H.264/AVC
- V_MPEG4/ISO/MVC   H.264/MVC
- V_MS/VFW/WVC1     VC1
- V_MPEG-2          MPEG2
- V_AV1             AV1
- V_VP8             VP8, from MKV/WebM/IVF, to MKV output or demuxed to .ivf
- V_VP9             VP9, from MKV/WebM/IVF, to MKV output or demuxed to .ivf
- V_MPEG4/ISO/ASP   MPEG-4 Part 2 video, from MP4/MOV, copied into MKV output only
- V_PRORES          Apple ProRes, from MOV, copied into MKV output only
- A_AC3             AC3/AC3+/TRUE-HD
- A_MLP             TRUE-HD (standalone TrueHD stream). For Blu-ray style TrueHD + AC-3 core from an MKV that
                    stores them as separate tracks, use merge-ac3-track=<n> with track=<TrueHD n> (see below).
- A_AAC             AAC
- A_DTS             DTS/DTS-Express/DTS-HD
- A_MP3             MPEG audio layer 1/2/3
- A_FLAC            FLAC, to MKV output or demuxed only
- A_OPUS            Opus, to TS or MKV output or demuxed only
- A_VORBIS          Vorbis, from MKV/WebM, to MKV output or demuxed to .ogg
- A_ALAC            Apple Lossless, from MKV, to MKV output only
- A_LPCM            raw pcm data or PCM WAV file
- S_HDMV/PGS        Presentation graphic stream (BD subtitle format)
- S_TEXT/UTF8        SRT subtitle format. Encoding MUST be UTF-8/UTF-16/UTF-32

Each track may have additional parameters. Track parameters do not have dashes.
If a parameter's value consists of several words, it must be enclosed in quotes.

Common additional parameters for any type of track:
- track             track number if input file is a container.
- lang              track language: three letters, an ISO 639-2 code such as eng
                    or deu, or a BCP 47 tag such as de, pt-BR or zh-Hant.

Additional parameters for audio tracks:
- timeshift         Shift a track by the given time: 120ms, 1.5s, 500us or 2000ns.
                    A number alone is in milliseconds. Video too, see below.
                    Can be negative.
- down-to-dts       Available only for DTS-HD tracks. Filter out HD part.
- down-to-ac3       For TRUE-HD and E-AC3 (DD+) tracks. Keep the AC-3 core, drop the HD part.
- drop-ac3-core     A Blu-ray TrueHD stream carries a 448 kbps AC-3 core beside its
                    lossless part; Matroska cannot hold the two in one track, so the core is written
                    as a track of its own and merge-ac3-track= puts them back together when the file
                    is muxed to a disc again. Add this to leave the core out and keep the lossless
                    stream alone, at the cost of not being able to author a spec-legal disc from it.
                    On a demux it does the same: the file holds the lossless stream on its own and
                    is named .thd rather than .ac3+thd, which is what a decoder that cannot read
                    the disc form needs. Without it the demux is unchanged.
- w64               On a demux only, and only for an LPCM track. Writes the audio as
                    Sony Wave64 instead of RIFF WAVE, with the extension .w64. A RIFF
                    header states its sizes in 32 bits, so it cannot describe a file
                    past 4 GiB and a long track comes out as two; Wave64 is the same
                    audio with 64 bit sizes and stays one file. tsMuxeR reads both.
- split-ac3-core    On a demux only, and only for the disc form of a TrueHD track. Writes the
                    two halves as files of their own, .thd and .ac3, beside the .ac3+thd holding
                    the pair, in one pass over the source instead of three. The .ac3+thd file is
                    the bytes exactly as the disc carries them; the other two are what a decoder
                    that cannot read the pair wants. It costs twice the disk space of the one
                    file, so ask for it when you want the halves rather than by habit.
- secondary         Mux as secondary audio. Available for DD+ and DTS-Express.
- default           Mark this track as the default. Used for Blu-ray, and written as the
                    Matroska default flag when the output is MKV. Without it the first track
                    of each type is the default.
- track-name        Track name, MKV output only. Shown by players in the track list.
                    Put it in quotes when it has a comma: track-name="Director, commentary".
- forced, hearing-impaired, visual-impaired, text-descriptions, original, commentary
                    Matroska track flags, MKV output only: forced subtitles, made for the
                    hearing or the visually impaired, text descriptions of the picture,
                    the original language, commentary. =1 (or the name alone) sets the
                    flag, =0 clears it. A Matroska source's own flags are kept otherwise.
- stretch           Stretch audio by a given factor. Can be a decimal value or a
                    fraction (e.g. 25/24), from 0.5 up to 2. Useful for fixing A/V
                    sync issues caused by frame rate discrepancies. Audio tracks only.

TrueHD + AC-3 core merge (MKV only):
- merge-ac3-track   Matroska track number of a classic AC-3 (Dolby Digital) stream to interleave with this A_MLP
                    TrueHD track for BD muxing. Requires track=<TrueHD track number>; do not add the AC-3 track as
                    a separate line in the meta file.
                    Example: A_MLP, "movie.mkv", track=2, merge-ac3-track=3
An MKV that tsMuxeR wrote from a disc already holds that pair: the lossless track and the disc's own AC-3 core
beside it, so the core is the real one and nothing has to be re-encoded.
If the file has TrueHD but no AC-3 track, decode the TrueHD track to AC-3 with ffmpeg (pick the correct audio
index for 0:a:N), for example:
    ffmpeg -i input.mkv -map 0:a:0 -c:a ac3 -b:a 640k -ac 6 compat.ac3
Remux the original MKV streams plus compat.ac3 into one MKV (e.g. mkvmerge), then reference TrueHD with
track= and the new AC-3 with merge-ac3-track= in the meta file.

TrueHD + AC-3 core merge (elementary streams):
- merge-ac3-file    Path to an external classic AC-3 file to interleave with a standalone TrueHD (.thd) stream.
                    Example: A_MLP, "audio.thd", merge-ac3-file="compat.ac3"

Additional parameters for video tracks:
- timeshift         The same option listed under audio. It shifts a video track as well,
                    which is what a stream taken from another source needs.
- fps               The number of frames per second. If not defined, the value
                    is auto detected from the source stream or container
                    metadata (e.g. MKV default_duration, MP4 timescale), or
                    from the times of its pictures in a TS, an M2TS or a
                    Blu-ray playlist. If none of them gives it, it defaults
                    to 25.
- delPulldown       Remove pulldown from the track, if it exists. If the
                    pulldown is present, the FPS value is changed from 30 to 24.
- ar                Override video aspect ratio: 1:1, 4:3, 16:9 or 2.21:1.

)help"
                            R"help(Additional parameters for H.264 video tracks:
- level             Overwrite the level in the H264 stream. Do note that this
                    option only updates the headers and does not reencode the
                    stream, which may not meet the requirements for a lower 
                    level.
- insertSEI         If the original stream does not contain SEI picture timing,
                    SEI buffering period or VUI parameters, add this data to
                    the stream. This option is recommended for BD muxing.
- forceSEI          Add SEI picture timing, buffering period and VUI parameters
                    to the stream and rebuild this data if it already exists.
- contSPS           If the original video doesn't contain repetitive SPS/PPS,
                    then SPS/PPS will be added to the stream before each key
                    frame. This option is recommended for BD muxing.
- subTrack          Selects one half of a track that carries two streams.
                    tsMuxeR always splits such a track into its two parts.
                    For a combined AVC/MVC track, 1 is the MVC part and 2 is
                    the AVC part. For a dual layer Dolby Vision track, 1 is
                    the base layer and 2 is the enhancement layer. The two
                    cases do not number their parts the same way.
- secondary         Mux as secondary video (PIP).
- pipCorner         Corner for PIP video. Allowed values: "TopLeft","TopRight",
                    "BottomRight", "BottomLeft". 
- pipHOffset        PIP window horizontal offset from the corner in pixels.
- pipVOffset        PIP window vertical offset from the corner in pixels.
- pipScale          PIP window scale factor. Allowed values: "1", "1/2", "1/4",
                    "1.5", "fullScreen".
- pipLumma          Allow the PIP window to be transparent. Transparent colors
                    are lumma colors in range [0..pipLumma].

Additional parameters for HEVC video tracks:
- level             Overwrite the level in the HEVC stream, for example
                    level=5.1. Like the H.264 option of the same name this only
                    rewrites the headers and does not reencode, so a level lower
                    than the stream actually needs will not be met. Raising it
                    is safe, since a higher level is a superset of a lower one.
                    Written to every VPS and SPS.
- hdr10-override    UHD Blu-ray only. For a stream that says SDR: the disc
                    marks it as HDR10 in its playlist, clip information and
                    index. The picture is not changed, so a player switches
                    the screen to HDR while the picture is coded for SDR; a
                    warning says so. Ignored, with a warning, for any other
                    output. A stream that is already HDR10, HDR10+ or Dolby
                    Vision is marked as it is.

Additional parameters for PG and SRT tracks:

- video-width       The width of the video in pixels.
- video-height      The height of the video in pixels.
- default           Mark this track as the default. Used for Blu-ray, and written as the
                    Matroska default flag when the output is MKV. Without it the first track
                    of each type is the default.
                    Allowed values are "all" (or yes, 1, true, on) which causes
                    all subtitles to be shown, and "forced" which shows only
                    elements marked as "forced" in the subtitle stream. They
                    are read in any case; any other value is refused.
                    "default" with no value is the same as "all".
                    "none", and also no, 0, false and off, mean this track is NOT
                    the default. Put it on every track of a type to say that the
                    type has no default at all: with nothing said either way, the
                    first track of each type becomes the default.
- track-name        Track name, MKV output only. Shown by players in the track list.
                    Put it in quotes when it has a comma: track-name="Director, commentary".
- forced, hearing-impaired, visual-impaired, text-descriptions, original, commentary
                    Matroska track flags, MKV output only: forced subtitles, made for the
                    hearing or the visually impaired, text descriptions of the picture,
                    the original language, commentary. =1 (or the name alone) sets the
                    flag, =0 clears it. A Matroska source's own flags are kept otherwise.
- fps               Video fps. It is recommended to define this parameter in
                    order to enable more careful timing processing.
- 3d-plane          Defines the number of the '3D offset track' which is placed
                    inside the MVC track. Each message has an individual 3D
                    offset. This information is stored inside 3D offset track.
                    A whole number from 0 up to one less than the planes of the
                    MVC track (3d-pg-planes in its listing), or 255 for none.

Additional parameters for SRT tracks:

- font-name         Font name to render.
- font-color        Font color, defined as a hexadecimal or decimal number.
                    24-bit long numbers (for instance 0xFF00FF) define RGB
                    components, while 32-bit long ones (for instance
                    0x80FF00FF) define ARGB components.
- font-size         Font size in pixels: a whole number from 1.
- font-italic       Italic display text.
- font-bold         Bold display text.
- font-underline    Underlined text.
- font-strike-out   Strikethrough text.
- font-charset      Font character set (numeric). Allows selection of a specific
                    character set for font rendering.
- bottom-offset     Distance from the lower edge while displaying text.
- font-border       Outline width: a number from 0.
- fadein-time       Time in seconds for smooth subtitle appearance, for
                    example 0.25.
- fadeout-time      Time in seconds for smooth subtitle disappearance, for
                    example 0.25.
- line-spacing      Interval between subtitle lines: a number from 0. Default
                    value is 1.0.
- keep-text         Keep the subtitles as text instead of drawing them into
                    pictures: each one is copied with its text, its tags and
                    its own start and end. For Matroska output, and for a
                    demux, which then writes an SRT file; ASS, SSA and WebVTT
                    subtitles from a Matroska file are written as .ass, .ssa
                    and .vtt files instead, in their own form. Other outputs
                    draw the subtitles as usual, with a warning, which needs
                    video-width, video-height and fps.
- save-srt          save-srt="name.srt": while the subtitles are drawn into
                    pictures, also write them as text to that SRT file
                    (UTF-8, numbered from 1), as a demux with keep-text would.
                    The file takes its name when the job has succeeded.
                    Ignored, with a warning, on a line with keep-text.
- fonts-dir         fonts-dir="folder": a folder of fonts that ASS and SSA
                    subtitles drawn with libass may use, besides the fonts
                    attached to the file and those installed in the system.
- ass-as-srt        Draw ASS and SSA subtitles as plain text with the font
                    parameters above, as SRT subtitles are drawn, instead of
                    with libass. Without it, ASS and SSA subtitles drawn into
                    pictures are drawn with libass, as their script says, and
                    the font parameters above do not apply to them.

tsMuxeR supports additional tags inside SRT tracks. The syntax and parameters
coincide with HTML: <b>, <i>, <u>, <strike>, <font>. Default relative font size
(used in these tags) is 3.  For example:

<b><font size=5 color="deepskyblue" name="Arial"><u>Test</u>
<font size= 4 color="#806040">colored</font>text</font>
</b>

)help"
                            R"help(Global additional parameters are placed in the first line of the META file,
which must begin with the MUXOPT token.
All parameters in this group start with two dashes:

--no-pcr-on-video-pid Allocate a separate PID for PCR and do not use the existing
                      video PID. Activated automatically for BD and AVCHD muxing,
                      and for a mux without video.
--new-audio-pes       Use bytes 0xfd instead of 0xbd for AC3, True-HD, DTS and
                      DTS-HD. Activated automatically for BD and AVCHD muxing.
--no-hdmv-descriptors Use ITU-T H.222.0 | ISO/IEC 13818-1 descriptors instead of
                      HDMV descriptors. Not activated for BD or AVCHD muxing.
--vbr                 Use variable bitrate. This is the default mode, so on its
                      own the word changes nothing.
--minbitrate          Sets the lower limit of the muxing rate, in kbps. If the
                      streams together come to less, null packets are inserted
                      to make up the difference. Has no effect unless --bitrate
                      or --maxbitrate is given as well.
--maxbitrate          Set a ceiling on the muxing rate, in kbps. The stream is
                      paced so it never asks to be read faster than that. The
                      rate counts what is written. For a fixed rate use
                      --bitrate instead. A --bitrate or --minbitrate above it
                      is lowered to it, with a warning. Must be above 90.
--cbr                 Ask for a fixed bitrate. The rate comes from --bitrate, so
                      --cbr on its own does nothing. --vbr and --cbr ask for
                      opposite things: given both, a warning says so, and
                      --bitrate decides as always.
--bitrate             Set a fixed bitrate in kbps, so --bitrate=35000 means
                      35 Mbps. This sets both the minimum and the maximum to
                      that value. Where the streams need less, null packets pad
                      the difference; where they need more they are still
                      written in full, so the output can come out above the
                      figure asked for. Must be above 90.
--vbv-len             The  length  of the  virtual  buffer  in milliseconds.  The
                      default value  is 500.  Typically, this  option  is used
                      together with --cbr. The parameter is similar to  the value
                      of vbv-buffer-size  in  the  x264  codec,  but  defined in
                      milliseconds instead of kbit.
--no-asyncio          Do not  create  a separate thread  for writing. This option
                      also disables the FILE_FLAG_NO_BUFFERING flag on Windows
                      when writing.
                      This option is deprecated.
--auto-chapters       Insert a chapter every <n> minutes. Used only in BD/AVCHD
                      mode.
--custom-chapters     A semicolon delimited list of hh:mm:ss.zzz strings,
                      representing the chapters' start times. On a disc the
                      title always begins with a chapter: when the list has no
                      time at the start, a mark is added there, so 5;10 gives
                      three chapters.
--chapters-file       Read the chapter list from a file instead. Two formats are
                      accepted and are told apart by what is in them: the simple
                      text form, with CHAPTER01= and CHAPTER01NAME= lines, and
                      Matroska XML as mkvextract writes it. Unlike
                      --custom-chapters these carry the chapter NAMES, which
                      Matroska output then keeps.
--no-chapters         Do not take over the chapters of the source. A Matroska
                      output then has none, and a demux writes no chapter
                      files. Chapters given with --custom-chapters or
                      --chapters-file still go into a Matroska output.
--demux               Run in demux mode : the selected audio and video tracks are
                      stored as separate files. The output name must be a folder
                      name. All selected effects (such as changing the level of
                      a H264 stream) are processed. When demuxing, certain types
                      of tracks are always changed :
                      - Subtitles in a Presentation Graphic Stream are converted
                        into sup format.
                      - PCM audio is saved as WAV files.
--blu-ray             Mux as a BD disc. If the output file name is a folder, a
                      Blu-Ray folder structure is created inside that folder.
                      SSIF files for BD3D discs are not created in this case. If
                      the output name has an .iso extension, then the disc is
                      created directly as an image file.
--blu-ray-v3          As above - except mux to UHD BD discs.
--avchd               Mux to AVCHD disc.
--cut-start           Where to start, measured from the beginning of the file.
                      Followed by "ms", "s" or "min" (90s, 1.5min), or a clock
                      time (00:01:30).
--cut-end             Where to STOP, measured from the beginning too, not from
                      the end. --cut-end=30s keeps the first 30 seconds.
--cut-pictures=<which> Which pictures of the video a cut keeps. A video that is
                      not encoded again can only begin at a key picture, and
                      the pictures shown just before an end can be made from
                      one shown after it. cover, the default, keeps every
                      picture the cut needs: the video begins at the key
                      picture at or before --cut-start, and on a Blu-ray or
                      AVCHD disc it ends with the last picture decoded before
                      --cut-end. The disc playlist plays only from --cut-start
                      to --cut-end, from the first picture shown at or after
                      each, so no picture outside the cut is shown, and a film
                      cut in two loses no picture between the parts. A TS, M2TS
                      or Matroska output shows up to one group of pictures
                      before the sound at the start, and it ends as with
                      inside: a film cut in two loses a picture between the
                      parts only at a cut exactly on a key picture, the few
                      shown just before it and made from it. inside keeps only
                      the pictures shown inside the cut, as earlier versions
                      did: the video begins at the first key picture at or
                      after --cut-start, and the pictures before that key
                      picture, and those shown just before --cut-end, are in no
                      part. The sound and subtitles start and stop at the cut
                      either way.
--split-duration      Split the output into several files, with each of them being
                      <n> seconds long. A unit may follow: ms, s, min or h.
--lang-codes=<form>   Which form a language READ from a source is reported in.
                      ISO 639-2 gives twenty languages two codes: terminological
                      is ell, deu, nld, and bibliographic is gre, ger, dut, which
                      is what eac3to shows. Takes source, terminological or
                      bibliographic; terminological is the default and is what
                      earlier versions did. A lang= of three letters reaches the
                      disc exactly as it is typed; a lang= given as a BCP 47 tag
                      (pt-BR) is written as its ISO 639-2 code in this form.
                      Unlike the options below it is a command line argument, not
                      a MUXOPT entry, so that it also applies when listing a file.
--playlist-clips=<how> How the clips of a Blu-ray playlist are muxed. played,
                      the default, takes from each clip only the part its play
                      item plays: from the key picture at or before the point
                      where a player starts to the one at or after the point
                      where it stops. whole takes every clip whole, as earlier
                      versions did. It is a MUXOPT entry and also a command line
                      argument, as --lang-codes is, so that a listing of a
                      playlist gives the lengths and marks of the same parts.
--keep-editions       Matroska output only, from one Matroska file: keep its
                      chapters exactly as they are, every edition of them and
                      every link to another file, and its segment identity
                      (SegmentUID, PrevUUID, NextUUID), so that ordered editions
                      and linked files still work after a remux. The file is
                      read as it is stored, not as its ordered edition plays it.
                      Remux each file of a linked set this way. Not with a join,
                      a cut, a split, --custom-chapters, --chapters-file or a
                      .webm output.
--title               Matroska output: the title of the file. Without it a
                      Matroska source's own title is kept.
--split-chapters      Split the output at the chapter marks the source carries,
                      so each file begins at a chapter. Takes no value. A
                      Blu-ray playlist's marks and a Matroska's own chapter
                      list are both used; a source with neither splits nothing.
                      As with the other two, a split happens at the first key
                      frame at or after the mark, never in the middle of a
                      picture.
--split-size          Split the output into several files of about the given
                      size each: a part ends at the first key picture after the
                      size is reached, so it can be larger by up to one group of
                      pictures. KB, KiB, MB, MiB, GB, GiB, TB
                      and TiB are accepted as size units, and K, M, G and T as
                      KB, MB, GB and TB.
                      Matroska output is split by all three the same way, into
                      name.split.1.mkv, name.split.2.mkv and on. Each part starts
                      at time zero and carries the chapters that fall into it.
                      Not with a merged dual layer Dolby Vision or 3D track.
--right-eye           Use base video stream for right eye. Used for 3DBD only.
--dv-profile          Matroska output only, and only for a dual layer Dolby Vision
                      source. 7 (the default) carries the disc as it is: profile 7,
                      both layers. 8.1 converts the Dolby Vision metadata so the file
                      plays as single layer profile 8.1, which many more devices
                      accept, while the disc's enhancement layer still travels inside
                      the track where decoders skip it, and the disc's OWN metadata is
                      attached beside it so the two layers can be separated again
                      exactly. The file is therefore NOT smaller than a profile 7 one;
                      that is the price of being able to rebuild the disc from it.
                      8.1 needs the libdovi library (dovi.dll on Windows) beside
                      tsMuxeR. It is published for 64 bit Windows; other platforms
                      build it from source. Without it the option is refused before
                      muxing starts. The published dovi.dll needs Windows 8 or
                      newer, so on Windows 7 this option may not work; that is a
                      limit of the library build, not of tsMuxeR.
)help"
                            R"help(--start-time          Timestamp of the first video frame. May be defined as 45Khz
                      clock (just a number) or as time in hh:mm:ss.zzz format.
                      If not set, muxing starts at 600 s (27000000 ticks). For every
                      output except plain *.ts, values below 524280 ticks (11.65 s)
                      are raised to 524280: below that point Blu-ray players cannot
                      navigate back to the start of the disc (their 32-bit 45 Khz
                      registers underflow), and commercial discs never start lower.
--mplsOffset          The number of the first MPLS file.  Used for BD disc mode.
--m2tsOffset          The number of the first M2TS file.  Used for BD disc mode.
--insertBlankPL       Add an additional short playlist. Used for cropped video
                      muxed to BD disc.
--blankOffset         Blank playlist number, 0 to 1999, 1900 if not set. Its clip
                      gets the same number, so it must differ from --mplsOffset
                      and from the main title's clips (from --m2tsOffset on, one
                      a part when the output is split).
--label               Disk label when muxing to ISO.
--extra-iso-space     Allocate extra space in 64K units for ISO metadata (file
                      and directory names). Normally, tsMuxeR allocates this space
                      automatically, and for a split it counts the parts from the
                      split size, the split length or the chapters. A split it
                      cannot count, such as --split-duration on a source that
                      states no length, may need this option. When the space is
                      too small, the mux ends with a message saying so.
--constant-iso-hdr    Generates an ISO header that does not depend on the program
                      version or the current time. Not meant for normal usage.
--disc-size           Fit-to-disc guard for BD/ISO output. Aborts before muxing if
                      the estimated image will not fit the target disc. Accepts the
                      keywords bd25/bd50/bd100/bd128, or a byte count with an optional
                      k/m/g (decimal) or ki/mi/gi (binary) suffix (e.g. 25000000000,
                      24g, 23gib).
--allow-oversize      With --disc-size, downgrade an over-capacity overrun from a
                      hard error to a warning and build the (oversized) image anyway.
--layer-break-guard   Dual-layer safety padding for ISO output: place <n> megabytes
                      of zero-filler AFTER the BD-R/RE DL layer break (the start of
                      layer 1, where physical discs are defect-prone), plus a sixteenth
                      of it (at least 4 MB) before it, so no file data sits on those
                      sectors. The crossing file (usually the movie) stays logically
                      contiguous and plays without a pause. 0 aligns to the break without
                      filler. Without this option, an ISO image whose data crosses a
                      layer break gets 288 MB after it and 18 MB before it (306 MB),
                      as a burnt dual-layer disc often reads badly at the layer
                      change and a player then stalls there. An image that would not
                      fit the disc with that guard is written without it, with a
                      warning. Not with --inner-only, which pads in its own way.
--no-layer-break-guard  ISO output: leave out the guard an image crossing a layer
                      break gets by default. A guard given with --layer-break-guard
                      is still written.
--layer-break-guard-before  Optional: size the BEFORE-break zone (MB) on its own instead
                      of a sixteenth of --layer-break-guard (at least 4 MB), making the
                      guard symmetric or custom.
                      Use when a disc is also defect-prone just before the layer break.
--layer-break-lbn     Layer break sector(s) for --layer-break-guard, in 2048-byte LBA
                      sectors = the disc's TOTAL sectors / number of layers.
                      WRITE THE NUMBER WITH NO GROUPING. A comma SEPARATES VALUES here and
                      is NOT a thousands separator: 12,219,392 is refused, and a list of
                      small numbers such as 12,219 is read as breaks at sectors 12 and 219,
                      with a warning, as the guard would land in the middle of your data.
                      Write 12219392.
                      One value for BD-R/RE DL, a list for BDXL: 100GB has 2 breaks
                      (Free/3 and 2*Free/3), 128GB has 3. Examples, with the Free Sectors
                      each was worked out from:
                        --layer-break-lbn=12219392
                              BD-R DL 50GB, 24438784 sectors, 2 layers. This is the default
                        --layer-break-lbn=16292864,32585728
                              BD-R XL 100GB, 48878592 sectors, 3 layers
                        --layer-break-lbn=15625216,31250432,46875648
                              BD-R XL 128GB, 62500864 sectors, 4 layers
                      Those three are FULL capacities. A disc formatted with defect
                      management reports less, 47305728 for 100GB say, and then the breaks
                      are different: work them out from what your own disc reports.
                      Read the total from the disc's FULL formatted capacity (ImgBurn
                      "Free Sectors"), NOT a partial/POW value (which gives a wrong break).
--inner-only          ISO output: keep the data on the inner part of every layer
                      and fill the outer edge of the disc with zeros, where discs
                      burn worst. Needs --disc-size (and --layer-break-lbn for a disc
                      whose breaks differ from its size's). The disc is muxed into a
                      folder beside the image first, <name>.tsmuxer-inner-<number>,
                      and the image is then laid out from it as --bdmv-to-iso
                      --inner-only --3d-single-copy --original-order lays it out, so
                      the drive needs room for both while it runs. The folder is
                      removed at the end; it is kept as an ordinary Blu-ray folder
                      only when the finished image does not fit the disc. Replaces
                      --layer-break-guard (a line with both is refused). Each
                      layer holds an even share at the centre of the disc; only the
                      breaks at the outer edge (the 1st and 3rd) are padded. A single
                      layer disc has nothing to pad.
--bdmv-to-iso         Separate mode: tsMuxeR --bdmv-to-iso [options] <BDMV_folder> <out.iso>
                      Wrap an existing BDMV folder into a UDF 2.50 BD-ROM ISO byte-for-byte
                      - no re-mux, no re-numbering - so BD-J menus and
                      all clip/playlist references stay valid, while applying the
                      dual-layer guard band. The largest .m2ts is written first so the
                      main title straddles the layer break and gets the guard.
                      Options for this mode:
                        --layer-break-guard=<MB>         see above
                        --layer-break-guard-before=<MB>  see above
                        --no-layer-break-guard           see above
                        --layer-break-lbn=<s[,s...]>     see above
                        --disc-capacity=<sectors>  Total sectors of the target disc. Read it from
                              the disc's FULL formatted capacity. Needed by --inner-only, used to
                              decide whether a file fits after the break, and used to CHECK THE
                              RESULT: with it, the build is refused up front when the payload plus
                              the guard cannot fit, and the finished image is measured against the
                              disc before the run ends. Without it neither check can run, and the
                              log says so.
                        --allow-oversize  Report an over-capacity image as a warning instead of
                              refusing, and build (or keep) it anyway.
                        --keep-extra-files  Copy EVERYTHING in the source folder, not just
                              BDMV, CERTIFICATE and AACS. Without this, anything else at the
                              top level is skipped with a "skipping ..." line: helper folders
                              from ripping tools, cover art, and also the companion asset
                              folders some discs place beside BDMV for their BD-J features.
                              If your source has such folders and you want a faithful image,
                              use this.
                        --3d-single-copy  Store a 3D disc's video once instead of twice: the
                              base view, the dependent view and the .ssif all point to the same
                              sectors, as on the source disc. Without it the video is written
                              twice and a 3D disc from a BD50 does not fit back onto one.
                        --inner-only  Keep the payload on the inner tracks of every layer and
                              pad each layer's outer rim with zeros, where discs burn worst.
                              Each layer holds an even share of the payload at the centre of
                              the disc. Multi-layer discs run from the centre out on layer 0,
                              back in on layer 1, out again on layer 2, so only the 1st and
                              3rd breaks are at the edge and padded; the 2nd, at the centre,
                              is not. Requires --disc-capacity. A single layer disc has
                              nothing to pad and is written as without it.
                        --constant-iso-hdr  An image header that does not depend on the
                              program version or the current time, as in a MUXOPT line.
                        --original-order  Write the files in disc order instead of largest
                              .m2ts first. Better for seamless branching titles, whose many
                              segments should stay physically close to their playback order.
                        --no-layer-fit  Do not move a file that would cross the layer break to
                              start after it; split it instead.
                        --label=<string>  Volume label for the image; Blu-Ray when not given.
                        --extra-iso-space=<64K blocks>  More room for the file system, on
                              top of what is counted from the folder's files and names.
)help";
    LTRACE(LT_INFO, 2, help);
}

#ifdef _WIN32
#include <shellapi.h>
#else
#include <unistd.h>
#endif

// Wrap an existing BDMV folder into a burnable UDF 2.50 BD-ROM ISO byte-for-byte (works with any
// unprotected BDMV, authored or copied), applying the dual-layer guard band. No re-mux, no re-numbering
// - so BD-J menus and every clip/playlist
// reference stay valid. The largest .m2ts (the main movie) is written FIRST so it straddles the ~25 GB
// physical layer break and gets the guard band; the rest fill layer 1. Skips MakeMKV helper folders.
// Read one PCR (90 kHz base units) from an .m2ts file near the given byte offset, scanning up to
// ~7.7 MB of source packets backward (dir=-1) or forward (dir=+1). Used to translate a guard pad's
// position inside a stream file into an approximate playback time.
static bool findM2tsPcrNear(const string& fileName, const int64_t startOffset, const int dir, uint64_t* pcrOut)
{
    File f;
    if (!f.open(fileName.c_str(), File::ofRead))
        return false;
    constexpr int UNIT = 192;  // m2ts source packet: TP_extra_header(4) + TS packet(188)
    constexpr int64_t MAX_SCAN = 40000;
    int64_t packet = startOffset / UNIT;
    if (dir < 0 && packet > 0)
        --packet;
    uint8_t u[UNIT];
    for (int64_t k = 0; k < MAX_SCAN && packet >= 0; ++k, packet += dir)
    {
        if (f.seek(packet * UNIT, File::SeekMethod::smBegin) != packet * UNIT)
            break;
        if (f.read(u, UNIT) != UNIT)
            break;
        if (u[4] != 0x47)
            continue;  // TS sync byte
        const int afc = (u[4 + 3] >> 4) & 0x3;
        if (afc != 2 && afc != 3)
            continue;  // no adaptation field
        if (u[4 + 4] < 7 || !(u[4 + 5] & 0x10))
            continue;  // adaptation field too short or no PCR_flag
        *pcrOut = (static_cast<uint64_t>(u[4 + 6]) << 25) | (static_cast<uint64_t>(u[4 + 7]) << 17) |
                  (static_cast<uint64_t>(u[4 + 8]) << 9) | (static_cast<uint64_t>(u[4 + 9]) << 1) | (u[4 + 10] >> 7);
        f.close();
        return true;
    }
    f.close();
    return false;
}

// Robust playback-position read. A single PCR read near a pad boundary can land in a small block
// with a foreign timeline (measured on a real UHD stream: one spot answered 1:47 where the movie
// time was 0:47), so sample several offsets spread over ~32 MB and take the median; an isolated
// stray block cannot win a median.
static bool findM2tsPcrMedian(const string& fileName, const int64_t offset, const int dir, uint64_t* pcrOut)
{
    // spread of a few MB: wide enough to step over a stray block (a measured one was under
    // 256 KB), narrow enough that the median stays within seconds of the true position
    static constexpr int64_t SPREAD[] = {0, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024};
    std::vector<uint64_t> vals;
    for (const int64_t step : SPREAD)
    {
        const int64_t o = dir < 0 ? offset - step : offset + step;
        if (o < 0)
            continue;
        uint64_t pcr = 0;
        if (findM2tsPcrNear(fileName, o, dir, &pcr))
            vals.push_back(pcr);
    }
    if (vals.empty())
        return false;
    std::sort(vals.begin(), vals.end());
    *pcrOut = vals[vals.size() / 2];
    return true;
}

// Write one 3D group into the image the way a pressed disc holds it. The base view and the
// dependent view are cut into the chunks their own clip info files name and written alternately,
// dependent chunk first, and the .ssif is then given those same sectors under its own name. The
// video is stored once and all three names resolve to it, so the group costs what the two views
// cost instead of twice that.
//
// The alternation is what creates the pieces. IsoWriter opens a new extent whenever the file being
// written changes, so writing dep, base, dep, base leaves each view holding every other chunk, and
// createInterleavedFile lists them back in that same order.
// dataStartLbn comes back as the sector the group's DATA begins on, which is not where it was
// called: a guard pad placed before the first pair sits in between. The layer break report maps a
// pad to the file it interrupted by sector range, so a range that started before its own pad would
// leave that pad describing itself.
static bool writeSsifGroup(IsoWriter* iso, const string& srcRoot, const BdSsifGroup& g, vector<uint8_t>& buf,
                           int64_t& written, const int64_t total, int& lastTenths, int64_t& dataStartLbn)
{
    File baseIn, depIn;
    if (!baseIn.open((srcRoot + "/" + g.baseRel).c_str(), File::ofRead))
    {
        LTRACE(LT_ERROR, 2, "Can't read " << g.baseRel);
        return false;
    }
    if (!depIn.open((srcRoot + "/" + g.depRel).c_str(), File::ofRead))
    {
        LTRACE(LT_ERROR, 2, "Can't read " << g.depRel);
        return false;
    }
    ISOFile* baseOut = iso->createFile();
    baseOut->open(g.baseRel.c_str(), File::ofWrite);
    ISOFile* depOut = iso->createFile();
    depOut->open(g.depRel.c_str(), File::ofWrite);

    auto copyChunk = [&](File& in, ISOFile* out, int64_t bytes)
    {
        while (bytes > 0)
        {
            const auto want = static_cast<uint32_t>(
                bytes < static_cast<int64_t>(buf.size()) ? bytes : static_cast<int64_t>(buf.size()));
            const int rd = in.read(buf.data(), want);
            if (rd <= 0)
                return false;
            if (out->write(buf.data(), static_cast<uint32_t>(rd)) < 0)
                return false;
            bytes -= rd;
            written += rd;
            if (total > 0)
            {
                // 99.9 at most, as for the plain files (buildGuardedIso)
                const int tenths = (std::min)(
                    999, static_cast<int>(static_cast<double>(written) / static_cast<double>(total) * 1000.0));
                if (tenths != lastTenths)
                {
                    lastTenths = tenths;
                    cout << tenths / 10 << '.' << tenths % 10 << "% complete" << std::endl;
                }
            }
        }
        return true;
    };

    bool ok = true;
    dataStartLbn = iso->currentImageLBA();
    for (size_t i = 0; ok && i < g.baseChunkBytes.size(); ++i)
    {
        // Any layer break guard is laid down HERE, between one pair and the next, so both views
        // are cut the same way and the alias below can describe them. Without this the pad falls
        // wherever a copy write reaches the zone, which for a chunk larger than the copy buffer is
        // inside a chunk: measured on a six group tree, one view came out with 5 pieces against
        // the other's 4 and the group could not be rebuilt at all.
        iso->padBeforeInterleavedPair(g.depChunkBytes[i] + g.baseChunkBytes[i]);
        if (i == 0)
            dataStartLbn = iso->currentImageLBA();
        // Dependent chunk first. Confirmed on three discs, and invisible in the file sizes: getting
        // it the wrong way round produces an .ssif of exactly the right length with the two views
        // swapped, which no size check would ever catch.
        ok = copyChunk(depIn, depOut, g.depChunkBytes[i]) && copyChunk(baseIn, baseOut, g.baseChunkBytes[i]);
    }
    baseOut->close();
    delete baseOut;
    depOut->close();
    delete depOut;
    baseIn.close();
    depIn.close();
    if (!ok)
    {
        LTRACE(LT_ERROR, 2, "Read error while writing " << g.ssifRel << " from its two views");
        return false;
    }

    // NOTE THE ORDER. createInterleavedFile emits inFile2's piece before inFile1's, so passing
    // (base, dep) is what puts the dependent chunk first and matches the disc.
    return iso->createInterleavedFile(g.baseRel, g.depRel, g.ssifRel);
}

// What the folder to ISO build is given. --bdmv-to-iso reads it from its command line, and a direct mux to an ISO with
// --inner-only from its MUXOPT line: the disc is muxed into a folder first and the image is built from that folder by
// the same code, so both routes give the same image.
struct GuardedIsoOptions
{
    std::string srcRoot;  // the folder that holds BDMV
    std::string outIso;
    int layerBreakGuardMB = -1;
    int layerBreakGuardBeforeMB = -1;
    std::vector<int> layerBreakLbns;
    bool originalOrder = false;
    bool layerFit = true;
    int64_t discCapacitySectors = 0;
    bool keepExtras = false;
    bool singleCopy3d = false;
    bool innerOnly = false;
    bool noLayerBreakGuard = false;  // --no-layer-break-guard: no guard unless one is given
    bool allowOversize = false;
    bool reproducibleHeader = false;  // --constant-iso-hdr
    int extraIsoSpace = 0;            // --extra-iso-space, in 64K blocks, on top of the estimate
    std::string discLabel;
    bool keepTooBigImage = true;  // an image refused by the final size check stays where it was written
    // Set by a mux with --inner-only, which ends with an error of its own when the image fails: an unfinished image
    // removed is said after that error, and not at all when outIso is the temporary name of KeepOldOutput
    bool partOfMux = false;
    bool tempName = false;
};

// ** AN OUTPUT A FAILED JOB MADE IS SAID TO BE REMOVED. ** One line, after the error that ended the job: while an
// exception is on its way to main, the line waits there (printRemovedOutputNotes), and a job that ends with a return
// code has said its error already.
static std::vector<std::string>& removedOutputNotes()
{
    static std::vector<std::string> notes;
    return notes;
}

static void noteRemovedOutput(const std::string& name, const bool afterError = false)
{
    std::string line = "The unfinished output " + name + " was removed.";
    if (afterError || std::uncaught_exceptions() > 0)
        removedOutputNotes().push_back(std::move(line));
    else
        LTRACE(LT_WARN, 2, line);
}

// lineOpen: the error before it was written without an end of line, so the note begins one of its own
static void printRemovedOutputNotes(const bool lineOpen = false)
{
    if (lineOpen && !removedOutputNotes().empty())
        LTRACE2(LT_ERROR, "\n")
    for (const std::string& line : removedOutputNotes()) LTRACE(LT_WARN, 2, line);
    removedOutputNotes().clear();
}

// sizeRefused, when given, comes back true when the finished image was refused because it does not fit the disc
static int buildGuardedIso(const GuardedIsoOptions& opt, bool* sizeRefused = nullptr);

// "100.0% complete" was printed when the sources were read, and a job that then failed, a track found empty, an image
// too big for the disc, had shown it before its error. The progress lines stop at 99.9 percent, and this is printed
// once every final check has passed, right before the job says it has succeeded. A job that fails never shows it.
static void reportJobComplete() { cout << "100.0% complete" << std::endl; }

static int bdmvFolderToGuardedIso(const int argc, char** argv)
{
    int layerBreakGuardMB = -1;
    int layerBreakGuardBeforeMB = -1;
    std::vector<int> layerBreakLbns;
    bool originalOrder = false;
    bool layerFit = true;
    int64_t discCapacitySectors = 0;
    bool keepExtras = false;
    bool singleCopy3d = false;
    bool innerOnly = false;
    bool noLayerBreakGuard = false;
    bool allowOversize = false;
    bool reproducibleHeader = false;
    int extraIsoSpace = 0;  // --extra-iso-space, in 64K blocks, on top of the estimate
    string discLabel;
    vector<string> positional;
    for (int i = 2; i < argc; ++i)
    {
        const string a = argv[i];
        try
        {
            // The guard sizes had no limit here, while a meta file refuses more than 1024 MB: 2000 built an image
            // with 2000 MB of zeros on this route and was refused on the other. One reading for both now.
            if (a.rfind("--layer-break-guard-before=", 0) == 0 || a.rfind("--layer-break-guard=", 0) == 0)
            {
                const size_t eq = a.find('=');
                int& mb = a.rfind("--layer-break-guard-before=", 0) == 0 ? layerBreakGuardBeforeMB : layerBreakGuardMB;
                if (const std::string err = parseGuardMB(a.substr(0, eq), a.substr(eq + 1), mb); !err.empty())
                {
                    LTRACE(LT_ERROR, 2, err);
                    return -1;
                }
            }
            else if (a.rfind("--layer-break-lbn=", 0) == 0)
            {
                std::vector<std::string> warnings;
                if (const std::string err = parseLayerBreakList(a.substr(18), layerBreakLbns, warnings); !err.empty())
                {
                    LTRACE(LT_ERROR, 2, "--layer-break-lbn: " << err);
                    return -1;
                }
                for (const auto& warning : warnings) LTRACE(LT_WARN, 2, "Warning: " << warning);
            }
            else if (a.rfind("--disc-capacity=", 0) == 0)
            {
                // A number of sectors, digits only. It was read up to the first sign that is not a digit, so
                // 24.438.784 was 24 sectors, 66g 66, and 24438784abc was taken; a disc of 24 sectors held nothing.
                const std::string value = a.substr(16);
                if (value.empty() || value.size() > 12 || value.find_first_not_of("0123456789") != std::string::npos ||
                    std::stoll(value) == 0)
                {
                    LTRACE(LT_ERROR, 2,
                           "--disc-capacity=" << value
                                              << " is not a number of sectors. Write it in digits only, such as "
                                                 "24438784 for a 50 GB disc.");
                    return -1;
                }
                discCapacitySectors = std::stoll(value);
            }
            else if (a == "--original-order")
                originalOrder = true;
            else if (a == "--no-layer-fit")
                layerFit = false;
            else if (a == "--keep-extra-files")
                keepExtras = true;
            else if (a == "--3d-single-copy")
                singleCopy3d = true;
            else if (a == "--inner-only")
                innerOnly = true;
            else if (a == "--no-layer-break-guard")
                noLayerBreakGuard = true;
            else if (a == "--allow-oversize")
                allowOversize = true;
            // The header of the image was always random on this route, so two builds of one folder could not be
            // compared byte for byte, as a direct mux to an ISO can with the same option.
            else if (a == "--constant-iso-hdr")
                reproducibleHeader = true;
            else if (a.rfind("--label=", 0) == 0)
                discLabel = a.substr(8);
            // The refusal of an image whose metadata does not fit advises this option, which was not taken here.
            else if (a.rfind("--extra-iso-space=", 0) == 0)
            {
                const string v = a.substr(18);
                if (v.empty() || v.find_first_not_of("0123456789") != string::npos)
                {
                    LTRACE(LT_ERROR, 2, "Invalid value in " << a << ": a number of 64K blocks.");
                    return -1;
                }
                if (const std::string why =
                        IsoWriter::tooManyExtraBlocks(v.size() > 18 ? INT64_MAX : strtoll(v.c_str(), nullptr, 10));
                    !why.empty())
                {
                    LTRACE(LT_ERROR, 2, why);
                    return -1;
                }
                extraIsoSpace = static_cast<int>(strToInt32(v.c_str()));
            }
            else
                positional.push_back(a);
        }
        catch (...)
        {
            LTRACE(LT_ERROR, 2, "Invalid value in " << a);
            return -1;
        }
    }
    if (positional.size() != 2)
    {
        LTRACE(LT_ERROR, 2,
               "Usage: tsMuxeR --bdmv-to-iso [--layer-break-guard=<MB>] [--layer-break-guard-before=<MB>] "
               "[--layer-break-lbn=<sector>] [--disc-capacity=<sectors>] [--original-order] [--no-layer-fit] "
               "[--keep-extra-files] [--3d-single-copy] [--inner-only] [--allow-oversize] [--label=<string>] "
               "[--extra-iso-space=<64K blocks>] [--constant-iso-hdr] "
               "<BDMV_folder> <out.iso>");
        return -1;
    }
    GuardedIsoOptions opt;
    opt.srcRoot = positional[0];
    opt.outIso = positional[1];
    opt.layerBreakGuardMB = layerBreakGuardMB;
    opt.layerBreakGuardBeforeMB = layerBreakGuardBeforeMB;
    opt.layerBreakLbns = layerBreakLbns;
    opt.originalOrder = originalOrder;
    opt.layerFit = layerFit;
    opt.discCapacitySectors = discCapacitySectors;
    opt.keepExtras = keepExtras;
    opt.singleCopy3d = singleCopy3d;
    opt.innerOnly = innerOnly;
    opt.noLayerBreakGuard = noLayerBreakGuard;
    // Decided 27 Oct: a guard given by size is written, and the line says the other option is not used.
    if (noLayerBreakGuard && (layerBreakGuardMB >= 0 || layerBreakGuardBeforeMB >= 0))
        LTRACE(LT_WARN, 2,
               "Warning: --no-layer-break-guard and a guard size are both given; the guard of the size given is "
               "used.");
    opt.allowOversize = allowOversize;
    opt.reproducibleHeader = reproducibleHeader;
    opt.extraIsoSpace = extraIsoSpace;
    opt.discLabel = discLabel;
    return buildGuardedIso(opt);
}

// Without a disc size and without --layer-break-lbn, an image is laid out for the layer break of a BD-R DL: the second
// copy of its file system and any layer break guard go there (BlurayHelper::open). An image larger than that disc is
// burnt to a 66 GB or 100 GB disc, which changes layer elsewhere, and nothing said so. The disc is not chosen from the
// size: an image of 60 GB fits both, and their breaks differ. Breaks given with --layer-break-lbn are the user's, so
// the image is laid out for the disc they belong to.
static void reportBd50Layout(const std::string& image, const char* bd100, const char* bd66)
{
    const auto size = static_cast<int64_t>(getFileSize(image));
    if (size <= BD50_CAPACITY)
        return;
    std::string warning, error;
    const std::vector<int> breaks = defaultLayerBreaks(0, warning, error);
    if (breaks.empty())
        return;
    LTRACE(LT_WARN, 2,
           "Warning: the finished image is "
               << size << " bytes, more than the " << BD50_CAPACITY
               << " bytes of a BD-R DL (50 GB), and it is laid out for the layer break of that disc, at sector "
               << breaks.front()
               << ", as no disc size was given. A disc of 66 GB or 100 GB changes layer elsewhere. Give " << bd100
               << " for a 100 GB disc or " << bd66 << " for a 66 GB one, to lay the image out for it.");
}

static int buildGuardedIso(const GuardedIsoOptions& opt, bool* sizeRefused)
{
    int layerBreakGuardMB = opt.layerBreakGuardMB;
    int layerBreakGuardBeforeMB = opt.layerBreakGuardBeforeMB;
    std::vector<int> layerBreakLbns = opt.layerBreakLbns;
    const bool originalOrder = opt.originalOrder;
    const bool layerFit = opt.layerFit;
    const int64_t discCapacitySectors = opt.discCapacitySectors;
    const bool keepExtras = opt.keepExtras;
    const bool singleCopy3d = opt.singleCopy3d;
    bool innerOnly = opt.innerOnly;
    const bool allowOversize = opt.allowOversize;
    const int extraIsoSpace = opt.extraIsoSpace;
    const string& discLabel = opt.discLabel;
    if (sizeRefused)
        *sizeRefused = false;
    string srcRoot = opt.srcRoot;
    const string outIso = opt.outIso;
    while (!srcRoot.empty() && (srcRoot.back() == '/' || srcRoot.back() == '\\')) srcRoot.pop_back();
    // A common slip is selecting the BDMV folder itself instead of the disc root that contains it.
    // Building from there would put STREAM/ at the ISO root and the disc would not play, so step up
    // to the parent automatically.
    {
        const size_t sep = srcRoot.find_last_of("/\\");
        if (sep != string::npos)
        {
            string base = srcRoot.substr(sep + 1);
            for (auto& c : base) c = static_cast<char>(toupper(c));
            if (base == "BDMV")
            {
                srcRoot = srcRoot.substr(0, sep);
                LTRACE(LT_INFO, 2, "The selected folder is BDMV itself; using its parent " << srcRoot);
            }
        }
    }

    // normalize a full source path to a forward-slash, disc-relative path (strip srcRoot prefix, unify
    // separators, collapse the '//' that findFiles() leaves where the recursion concatenates paths)
    auto toRel = [&srcRoot](const string& full)
    {
        string out;
        for (char c : full.substr(srcRoot.size()))
        {
            if (c == '\\')
                c = '/';
            if (c == '/' && (out.empty() || out.back() == '/'))
                continue;
            out += c;
        }
        return out;
    };

    vector<string> files;
    // findDirs() appends "*" to the path, so the walk root needs a trailing separator to list children
    if (!findFilesRecursive(srcRoot + getDirSeparator(), "*", &files) || files.empty())
    {
        LTRACE(LT_ERROR, 2, "No files found under " << srcRoot);
        return -1;
    }
    // The order of the listing decides the order of the entries and their unique ids in the image, and a folder lists
    // its names in no fixed order on Linux and macOS (on Windows too, off NTFS). One order on every system, the one an
    // NTFS folder gives: the files of a folder before its subfolders, each by name with the letters a-z taken as A-Z,
    // then byte for byte, so the same folder gives the same image everywhere.
    {
        auto lessName = [](const string& a, const string& b)
        {
            auto up = [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; };
            const size_t n = (std::min)(a.size(), b.size());
            for (size_t i = 0; i < n; ++i)
                if (up(a[i]) != up(b[i]))
                    return up(a[i]) < up(b[i]);
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        };
        auto lessPath = [&lessName](const string& a, const string& b)
        {
            size_t pa = 0, pb = 0;
            for (;;)
            {
                const size_t ea = a.find('/', pa), eb = b.find('/', pb);
                const string na = a.substr(pa, ea == string::npos ? string::npos : ea - pa);
                const string nb = b.substr(pb, eb == string::npos ? string::npos : eb - pb);
                if (na != nb)
                {
                    // a file of this folder comes before a subfolder of it
                    if ((ea == string::npos) != (eb == string::npos))
                        return ea == string::npos;
                    return lessName(na, nb);
                }
                if (ea == string::npos || eb == string::npos)
                    return ea == string::npos && eb != string::npos;
                pa = ea + 1;
                pb = eb + 1;
            }
        };
        vector<std::pair<string, string>> listed;
        listed.reserve(files.size());
        for (auto& f : files) listed.emplace_back(toRel(f), std::move(f));
        std::sort(listed.begin(), listed.end(),
                  [&lessPath](const std::pair<string, string>& a, const std::pair<string, string>& b)
                  { return lessPath(a.first, b.first); });
        for (size_t i = 0; i < listed.size(); ++i) files[i] = std::move(listed[i].second);
    }

    // keep BD structure only (drop MakeMKV/other helper folders); precompute disc-relative paths + sizes
    struct Item
    {
        string full;
        string rel;
        int64_t size;
        int ssifGroup;  // -1 for an ordinary file; otherwise the 3D group this one entry stands for
    };
    vector<Item> items;
    int64_t total = 0;
    // Only the disc-structure folders belong in a BD-ROM image. A whitelist keeps everything else
    // out by construction: previously built ISOs sitting next to BDMV, MakeMKV helper folders,
    // desktop.ini and other strays would otherwise be muxed in (and inflate the size until the
    // image no longer fits the disc).
    std::set<string> skippedTop;
    for (auto& f : files)
    {
        const string rel = toRel(f);
        string top = rel.substr(0, rel.find('/'));
        for (auto& c : top) c = static_cast<char>(toupper(c));
        // --keep-extra-files opts out of the whitelist so companion files (readme, cover art, extra
        // folders) are written to the disc image alongside the BD structure.
        if (!keepExtras && top != "BDMV" && top != "CERTIFICATE" && top != "AACS")
        {
            if (!top.empty())
                skippedTop.insert(rel.substr(0, rel.find('/')));
            continue;
        }
        const auto sz = static_cast<int64_t>(getFileSize(f));
        items.push_back({f, rel, sz, -1});
        total += sz;
    }
    for (const auto& s : skippedTop)
        LTRACE(LT_INFO, 2, "  skipping \"" << s << "\" (not part of the BDMV disc structure)");

    // Two names of one folder that a UDF file identifier cannot tell apart: a name in 16 bit characters keeps only
    // its first 127. The writer refused them only when it finished the image, so a refused image was left behind,
    // 43 MB in the case measured. Every level of every path is checked here, before the image is opened.
    //
    // Compared as the text the identifiers hold, not as their bytes: a name of 127 Latin letters is kept in 8 bit
    // characters, and the same 127 letters with one more character outside Latin-1 are kept in 16 bit ones, cut to the
    // same 127. The bytes differed, and both were written as two entries a reader shows with one name.
    {
        const auto textOf = [](const std::vector<uint8_t>& id)
        {
            std::u16string text;
            if (!id.empty() && id[0] == 16)
                for (size_t i = 1; i + 1 < id.size(); i += 2)
                    text.push_back(static_cast<char16_t>(id[i] << 8 | id[i + 1]));
            else
                for (size_t i = 1; i < id.size(); ++i) text.push_back(id[i]);
            return text;
        };
        std::map<std::pair<string, std::u16string>, string> seen;
        for (const auto& item : items)
        {
            string parent;
            size_t from = 0;
            while (from <= item.rel.size())
            {
                const size_t slash = item.rel.find('/', from);
                const string part = item.rel.substr(from, slash == string::npos ? string::npos : slash - from);
                const auto [it, isNew] = seen.emplace(std::make_pair(parent, textOf(udfFileIdentifier(part))), part);
                if (!isNew && it->second != part)
                    THROW(ERR_COMMON, "bdmv-to-iso: the names \""
                                          << it->second << "\" and \"" << part << "\" in "
                                          << (parent.empty() ? string("the disc folder") : parent)
                                          << " are the same as far as a UDF file identifier holds a name. "
                                             "Shorten one of them.")
                if (slash == string::npos)
                    break;
                parent += part + "/";
                from = slash + 1;
            }
        }
    }

    // A 3D disc stores its video ONCE and names it three times: the .ssif and the two .m2ts are
    // views over the same sectors. Copying all three by name writes the video twice and roughly
    // doubles the image. --3d-single-copy writes it once instead, the way the disc itself does.
    const std::vector<BdSsifGroup> ssifGroups = bdFindSsifGroups(srcRoot);
    size_t interleavedGroups = 0;
    size_t missingSsif = 0;
    for (const auto& g : ssifGroups)
    {
        // A folder copied without its .ssif files used to give an image that played in 2D without a word.
        // The .ssif holds no data of its own, so --3d-single-copy rebuilds it from the two clips.
        if (g.ssifMissing)
        {
            ++missingSsif;
            LTRACE(LT_INFO, 2,
                   "  3D: " << g.ssifRel << " is missing from the folder; it "
                            << (singleCopy3d ? "is rebuilt" : "can be rebuilt") << " from " << g.baseRel << " and "
                            << g.depRel);
            continue;
        }
        int64_t dup = 0;
        for (const int64_t c : g.baseChunkBytes) dup += c;
        for (const int64_t c : g.depChunkBytes) dup += c;
        LTRACE(LT_INFO, 2,
               "  3D: " << g.ssifRel << " is " << g.baseRel << " and " << g.depRel << " interleaved in "
                        << g.baseChunkBytes.size() << " chunk pairs, so " << dup / (1024 * 1024)
                        << " MB of this image is the same video twice");
    }
    if (ssifGroups.size() > missingSsif && !singleCopy3d)
        LTRACE(LT_INFO, 2, "  3D: add --3d-single-copy to store that video once, as the source disc does");
    if (missingSsif > 0 && !singleCopy3d)
        LTRACE(LT_WARN, 2,
               "Warning: " << missingSsif
                           << " of the 3D files (.ssif) are missing from the folder, so this image plays those titles "
                              "in 2D only. Add --3d-single-copy to rebuild them from their two views, as the disc "
                              "stores them.");

    // Fold each group's three files into ONE entry in the copy list, sized at what the group really
    // costs the image: both views, once. Everything downstream then works off the truth, so the
    // largest-first sort still puts the main feature at the front, the capacity check still refuses
    // an image that will not fit, and the progress bar still ends at 100.
    if (singleCopy3d && !ssifGroups.empty())
    {
        auto upperOf = [](string t)
        {
            for (auto& c : t) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            return t;
        };
        std::map<string, size_t> memberOf;  // three entries per group, all pointing at it
        for (size_t gi = 0; gi < ssifGroups.size(); ++gi)
        {
            memberOf[upperOf(ssifGroups[gi].ssifRel)] = gi;
            memberOf[upperOf(ssifGroups[gi].baseRel)] = gi;
            memberOf[upperOf(ssifGroups[gi].depRel)] = gi;
        }
        vector<Item> kept;
        kept.reserve(items.size());
        vector<bool> placed(ssifGroups.size(), false);
        int64_t saved = 0;
        for (const auto& it : items)
        {
            const auto found = memberOf.find(upperOf(it.rel));
            if (found == memberOf.end())
            {
                kept.push_back(it);
                continue;
            }
            const size_t gi = found->second;
            if (placed[gi])
                continue;  // the group is already in the list; its other two names add nothing
            placed[gi] = true;
            int64_t payload = 0;
            for (const int64_t c : ssifGroups[gi].baseChunkBytes) payload += c;
            for (const int64_t c : ssifGroups[gi].depChunkBytes) payload += c;
            kept.push_back({string(), ssifGroups[gi].ssifRel, payload, static_cast<int>(gi)});
            if (!ssifGroups[gi].ssifMissing)
                saved += payload;  // exactly the second copy that is no longer written (none when it was missing)
            ++interleavedGroups;
        }
        items.swap(kept);
        total = 0;
        for (const auto& it : items) total += it.size;
        if (interleavedGroups > 0 && saved > 0)  // nothing is saved for a .ssif the folder did not have
            LTRACE(LT_INFO, 2,
                   "  3D: writing " << interleavedGroups << " interleaved group(s) once, so the image is "
                                    << saved / (1024 * 1024) << " MB smaller");
    }
    if (items.empty())
    {
        LTRACE(LT_ERROR, 2, "No BDMV content found under " << srcRoot);
        return -1;
    }

    // Match tsMuxeR's own BD authoring: give the ISO the full standard folder structure by default so
    // players that expect it are satisfied. The empty standard folders (META/BDJO/JAR/AUXDATA + the
    // CERTIFICATE/BACKUP sub-tree) are added after the copy via BlurayHelper::createBluRayDirs(); here we
    // populate BACKUP with the small navigation files (never the large .m2ts, which BACKUP excludes) when
    // the source disc did not ship its own BACKUP.
    auto up = [](string s)
    {
        for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return s;
    };
    auto has = [&](const char* prefix)
    {
        const string p = up(prefix);
        for (const auto& it : items)
            if (up(it.rel).rfind(p, 0) == 0)
                return true;
        return false;
    };
    const bool hasBdmv = has("BDMV/");
    if (hasBdmv && !has("BDMV/BACKUP/"))
    {
        auto baseName = [](const string& r)
        {
            const auto p = r.find_last_of('/');
            return p == string::npos ? r : r.substr(p + 1);
        };
        vector<Item> backup;
        for (const auto& it : items)
        {
            const string R = up(it.rel);
            string dst;
            if (R == "BDMV/INDEX.BDMV")
                dst = "BDMV/BACKUP/index.bdmv";
            else if (R == "BDMV/MOVIEOBJECT.BDMV")
                dst = "BDMV/BACKUP/MovieObject.bdmv";
            else if (R.rfind("BDMV/PLAYLIST/", 0) == 0)
                dst = "BDMV/BACKUP/PLAYLIST/" + baseName(it.rel);
            else if (R.rfind("BDMV/CLIPINF/", 0) == 0)
                dst = "BDMV/BACKUP/CLIPINF/" + baseName(it.rel);
            else if (R.rfind("BDMV/BDJO/", 0) == 0)
                dst = "BDMV/BACKUP/BDJO/" + baseName(it.rel);
            if (!dst.empty())
                backup.push_back({it.full, dst, it.size, -1});
        }
        for (const auto& b : backup)
        {
            items.push_back(b);
            total += b.size;
        }
        if (!backup.empty())
            LTRACE(LT_INFO, 2, "  standard folders: generated BACKUP from " << backup.size() << " navigation file(s)");
    }

    // Default: largest .m2ts first, so the main movie straddles the layer break and gets the guard
    // band. --original-order keeps the files in their disc order instead; better for seamless
    // branching titles whose many segments should stay physically close to their playback order.
    if (originalOrder)
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rel < b.rel; });
    else
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });

    // A stream file, and the first view of a 3D group, starts on a 64 KB boundary of the image: up to 31 sectors of
    // zeros before each, which the arithmetic below keeps room for
    auto isStreamItem = [](const Item& it)
    {
        const string r = strToUpperCase(it.rel);
        return it.ssifGroup >= 0 || strEndWith(r, ".M2TS") || strEndWith(r, ".SSIF");
    };
    const int64_t alignSectors = 31 * static_cast<int64_t>(std::count_if(items.begin(), items.end(), isStreamItem));

    // The copy buffer below, named here because two pieces of arithmetic depend on its size: a guard
    // pad is laid down from wherever the current write lands to the far edge of the zone, so it can
    // begin up to one whole buffer before the zone starts and the image grows by that much more than
    // the guard asked for.
    constexpr int64_t COPY_BUFFER_BYTES = 16 * 1024 * 1024;
    const int64_t overrunSectors = COPY_BUFFER_BYTES / 2048;

    // --inner-only: keep the payload on the inner tracks of every layer and pad each layer's outer edge
    // (the rim) with zeros. BD-R DL fixes the layer break at the physical layer-0 capacity, so the drive
    // cannot be made to jump early; instead we widen the layer-break guard symmetrically, which pushes the
    // movie toward both hubs and fills the outer/rim part with zeros. The worst-burned outer tracks then
    // carry no data. Needs the disc capacity to know the free space.
    // --layer-break-guard-before on its own is a guard before the break only; it was ignored. Without breaks given, the
    // breaks are those of the disc --disc-capacity names (its sectors split over its layers), or the break of a
    // BD-R DL: a 100 GB disc got the guard at half a BD50.
    if (layerBreakGuardMB < 0 && layerBreakGuardBeforeMB >= 0)
        layerBreakGuardMB = 0;
    if ((layerBreakGuardMB >= 0 || innerOnly) && layerBreakLbns.empty())
    {
        std::string warning, error;
        layerBreakLbns = defaultLayerBreaks(discCapacitySectors > 0 ? discCapacitySectors * 2048 : 0, warning, error);
        if (!error.empty())
        {
            LTRACE(LT_ERROR, 2, error);
            return -1;
        }
        if (!warning.empty())
            LTRACE(LT_WARN, 2, warning);
        if (layerBreakLbns.empty())
        {
            // --inner-only alone asks for no guard, and says below why it has nothing to do
            if (layerBreakGuardMB >= 0)
                LTRACE(LT_WARN, 2, "Warning: a single layer disc has no layer break, so the guard is not applied.");
            layerBreakGuardMB = -1;
            layerBreakGuardBeforeMB = -1;
        }
    }
    // The zone of each break --inner-only pads, in sectors, for IsoWriter::setLayerBreakGuards. Empty when it pads
    // none.
    std::vector<int> innerBefore, innerAfter;
    int innerLayers = 0;  // the layers of the disc --inner-only lays out, 0 when it lays out none
    if (innerOnly)
    {
        if (discCapacitySectors <= 0)
        {
            LTRACE(LT_ERROR, 2, "--inner-only needs the disc capacity; add --disc-capacity=<sectors>");
            return -1;
        }
        // A disc of one layer has no layer change and no outer edge before one: its data starts at the centre and
        // ends where it ends. A break was made up at half the disc and padded, which put the second half of the data
        // at the rim, the very place the option keeps empty.
        if (layerBreakLbns.empty())
        {
            LTRACE(LT_INFO, 2,
                   "  inner-only: a single layer disc has no layer break and no outer edge to pad: its data already "
                   "starts at the centre of the disc. The image is laid out as without --inner-only.");
            innerOnly = false;
        }
    }
    if (innerOnly)
    {
        // Blu-ray discs of more than one layer use the opposite track path: layer 0 is written from the centre (the
        // hub) out to the rim, layer 1 from the rim back to the hub, layer 2 from the hub out again, layer 3 back in.
        // So the first and third layer changes are at the RIM and the second is at the HUB. Each layer keeps an even
        // share of the data at its hub end, and only the rim breaks are padded, each from the end of its outward layer
        // to the start of the data of its inward one. The hub break gets no zeros: the data runs across it from one
        // layer's hub end to the next one's. With an odd number of layers the image ends inside the last layer, from
        // the hub outward, and the rest of that layer is left unwritten. Every break used to get the same zone around
        // it, which padded the hub of a 100 GB or 128 GB disc and moved the data of layer 2 to its rim.
        //
        // A pair of layers around a rim break holds two shares. The last pair, which ends at the end of the disc,
        // also keeps the margin and every pad's overrun, as the one pair of a two-layer disc always has, so a
        // two-layer image comes out as before. The overrun: a pad starts wherever the current write lands, up to one
        // copy buffer before its zone.
        const int64_t payloadSectors = (total + 2047) / 2048;
        const int64_t marginSectors = 8192 + alignSectors;  // ~16 MB for UDF overhead + a small inner free tail
        const int nLayers = static_cast<int>(layerBreakLbns.size()) + 1;
        std::vector<int64_t> bound(1, 0);  // where each layer starts, and the end of the disc
        for (const int b : layerBreakLbns) bound.push_back(b);
        bound.push_back(discCapacitySectors);
        std::vector<int64_t> share(nLayers, payloadSectors / nLayers);
        share.back() = payloadSectors - payloadSectors / nLayers * (nLayers - 1);
        const int nPads = nLayers / 2;
        std::vector<int> rimBreaks;
        std::vector<int64_t> guard;
        for (int j = 1; j < nLayers; j += 2)
        {
            const bool lastPair = j + 1 == nLayers;
            const int64_t reserve = lastPair ? marginSectors + nPads * overrunSectors : 0;
            rimBreaks.push_back(layerBreakLbns[j - 1]);
            guard.push_back((bound[j + 1] - bound[j - 1] - share[j - 1] - share[j] - reserve) / 2);
        }
        // With an odd count, the last layer holds its share plus all the margin and overruns: when the disc is near
        // full the zones give way, the last one first, so the image still ends inside the disc.
        int64_t excess = payloadSectors + marginSectors + nPads * overrunSectors - discCapacitySectors;
        for (const int64_t g : guard) excess += 2 * g;
        for (size_t k = guard.size(); excess > 0 && k-- > 0;)
        {
            const int64_t take = (std::min)((excess + 1) / 2, (std::max<int64_t>)(guard[k], 0));
            guard[k] -= take;
            excess -= 2 * take;
        }
        bool room = true;
        for (const int64_t g : guard)
            if (g <= 0)
                room = false;
        // "Keeping the normal guard" was said whatever was given. Only a guard asked for with --layer-break-guard (taken
        // by --bdmv-to-iso beside --inner-only) is kept here; without one there is no guard to keep, and a direct mux
        // never has one, since a meta line refuses the two together. Measured: a 355 MB image on the direct route
        // said it kept the guard and was written with no zeros at the break.
        if (!room && layerBreakGuardMB >= 0)
            LTRACE(LT_WARN, 2,
                   "--inner-only: the disc is (nearly) full, so there is no room to pad; the guard given by "
                   "--layer-break-guard is used instead");
        else if (!room)
            LTRACE(LT_WARN, 2,
                   "--inner-only: the disc is (nearly) full, so there is no room to pad; the image is laid out with "
                   "no zeros at the layer break");
        else
        {
            // whole MB, as the guard has always been given
            std::vector<int> guardMB;
            for (const int64_t g : guard) guardMB.push_back(static_cast<int>(g * 2048 / (1024 * 1024)));
            // With --bdmv-to-iso a guard is taken beside --inner-only, as it was when released, with a warning (decided 12 Oct).
            // The guard was replaced without a word. A meta line refuses the two together, so this is that route only.
            if (layerBreakGuardMB >= 0)
                LTRACE(LT_WARN, 2,
                       "Warning: --inner-only works out the zeros at the layer break from the disc and the data, so "
                       "the guard given by --layer-break-guard or --layer-break-guard-before is not used.");
            layerBreakGuardMB = guardMB.front();
            layerBreakGuardBeforeMB = guardMB.front();
            if (nLayers == 2)
                LTRACE(LT_INFO, 2,
                       "  inner-only: data on the inner tracks; guard " << guardMB.front()
                                                                        << " MB each side over 1 break(s), "
                                                                           "outer/rim padded with zeros");
            else
            {
                LTRACE(LT_INFO, 2,
                       "  inner-only: data on the inner tracks of each of the "
                           << nLayers << " layers, about " << share.front() / 512
                           << " MB each; zeros only where a layer ends at the outer edge");
                for (int j = 1; j < nLayers; ++j)
                {
                    if (j % 2 == 1)
                        LTRACE(LT_INFO, 2,
                               "    break " << j << " at sector " << layerBreakLbns[j - 1]
                                            << " (outer edge): " << guardMB[j / 2] << " MB of zeros each side");
                    else
                        LTRACE(LT_INFO, 2,
                               "    break " << j << " at sector " << layerBreakLbns[j - 1]
                                            << " (centre of the disc): no zeros, the data runs on into layer " << j);
                }
                if (nLayers % 2 == 1)
                    LTRACE(LT_INFO, 2,
                           "    the image ends inside layer " << nLayers - 1
                                                              << ", which is written from the centre outward; the rest "
                                                                 "of it is left unwritten");
            }
            // Only the rim breaks are given to the writer, each with its own zone
            layerBreakLbns = rimBreaks;
            innerLayers = nLayers;
            for (const int mb : guardMB)
            {
                const int sectors = static_cast<int>(static_cast<int64_t>(mb) * 1024 * 1024 / 2048);
                innerBefore.push_back(sectors);
                innerAfter.push_back(sectors);
            }
        }
    }

    // the metadata partition must hold ~1 File Entry per file + directory content; size it from the count
    // Each folded 3D group is one entry in `items` but still three files in the image.
    // A folder takes two sectors, its entry and its contents, and a file one: a tree of some 300 folders of one file
    // each was refused, its metadata did not fit, as only the files were counted.
    std::set<string> folders;
    for (const auto& it : items)
        for (size_t s = it.rel.find_first_of("/\\"); s != string::npos; s = it.rel.find_first_of("/\\", s + 1))
            folders.insert(strToUpperCase(it.rel.substr(0, s)));
    int baseISOBlocks = static_cast<int>(items.size() + 2 * folders.size() + 2 * interleavedGroups) / 32 + 16;
    // The estimate gives each folder one sector for its listing. A listing holds 38 bytes and the name for each
    // entry, so a folder of many files, or of files with long names, needs many more: 6000 names of 230 characters
    // in one folder need about 800 sectors, the image was refused at the very end, after every byte had been
    // copied, and only --extra-iso-space helped. The listings are counted from the names now, and the area is
    // enlarged where the estimate would not hold them; an image the estimate fits is left exactly as it was.
    {
        std::map<string, int64_t> listingBytes;  // folder ("" = the root) -> size of its listing
        std::set<string> listedFolders;
        auto addPath = [&](const string& rel)
        {
            string parent;
            size_t from = 0;
            while (from <= rel.size())
            {
                const size_t slash = rel.find('/', from);
                const string part = rel.substr(from, slash == string::npos ? string::npos : slash - from);
                const bool isFolder = slash != string::npos;
                if (!isFolder || listedFolders.insert(parent + part).second)
                    listingBytes[parent] += (38 + static_cast<int64_t>(udfFileIdentifier(part).size()) + 3) / 4 * 4;
                if (!isFolder)
                    break;
                parent += part + "/";
                from = slash + 1;
            }
        };
        for (const auto& it : items)
        {
            if (it.ssifGroup >= 0 && static_cast<size_t>(it.ssifGroup) < ssifGroups.size())
            {
                const BdSsifGroup& g = ssifGroups[static_cast<size_t>(it.ssifGroup)];
                for (const string* rel : {&g.ssifRel, &g.baseRel, &g.depRel}) addPath(*rel);
            }
            else
                addPath(it.rel);
        }
        listingBytes[""];  // the root has a listing even when it holds no file
        int64_t sectors = static_cast<int64_t>(items.size() + 2 * interleavedGroups);  // one File Entry per file
        for (const auto& [folder, bytes] : listingBytes)
            sectors += 1 + (40 + bytes + 2047) / 2048;  // its File Entry, and the listing behind its parent entry
        // The file set and terminating descriptors, the system stream folder, and room for the standard folders
        // createBluRayDirs adds
        sectors += 16 + 64;
        const int neededBlocks = static_cast<int>((sectors + 31) / 32);
        if (neededBlocks > baseISOBlocks)
            baseISOBlocks = neededBlocks;
    }
    const int extraISOBlocks = baseISOBlocks + extraIsoSpace;

    // An image whose data crosses a layer break gets a guard of 288 MB after each break and 18 MB before it when none
    // is given, as a burnt dual-layer disc often reads badly at the layer change and a player then stalls there. The
    // breaks are found as for a guard given. --inner-only lays out zeros of its own, and an image that ends before the
    // break is left as it was. An image that would not fit the disc with the guard is written without it, with a
    // warning: only a guard asked for may refuse the build below.
    bool defaultGuard = false;
    if (!opt.noLayerBreakGuard && !opt.innerOnly && layerBreakGuardMB < 0 && layerBreakGuardBeforeMB < 0)
    {
        std::string warning, error;
        const std::vector<int> breaks =
            !layerBreakLbns.empty()
                ? layerBreakLbns
                : defaultLayerBreaks(discCapacitySectors > 0 ? discCapacitySectors * 2048 : 0, warning, error);
        // the before-zone of IsoWriter::setLayerBreakGuard(afterSectors): a sixteenth, at least 4 MB
        constexpr int64_t afterSectors = static_cast<int64_t>(DEFAULT_GUARD_AFTER_MB) * 1024 * 1024 / 2048;
        constexpr int64_t beforeSectors =
            (std::min)(afterSectors, (std::max<int64_t>)(4 * 1024 * 1024 / 2048, afterSectors / 16));
        constexpr int64_t udfSectors = 8192;  // the UDF reserve of the capacity check below
        const int64_t dataSectors = (total + 2047) / 2048 + alignSectors + udfSectors;
        if (error.empty() && !breaks.empty() && dataSectors > static_cast<int64_t>(breaks.front()) - beforeSectors)
        {
            // Without a disc, the one the breaks belong to: the layers hold the data evenly, so the disc ends one
            // layer past the last break.
            const int64_t capacity =
                discCapacitySectors > 0 ? discCapacitySectors : static_cast<int64_t>(breaks.back()) + breaks.front();
            int64_t guardSectors = 0;
            for (const int bp : breaks)
                if (dataSectors > static_cast<int64_t>(bp) - beforeSectors)
                    guardSectors += beforeSectors + afterSectors + overrunSectors;
            if (dataSectors + guardSectors > capacity)
                LTRACE(LT_WARN, 2,
                       "Warning: the image does not fit the disc with the layer break guard, so it is written without "
                       "it. A burnt dual-layer disc may then read badly at the layer change.");
            else
            {
                defaultGuard = true;
                layerBreakLbns = breaks;
                layerBreakGuardMB = DEFAULT_GUARD_AFTER_MB;
            }
        }
    }

    LTRACE(
        LT_INFO, 2,
        "bdmv-to-iso: " << items.size() << " files, " << static_cast<int64_t>(total / 1000000) << " MB -> " << outIso);
    if (layerBreakGuardMB >= 0 && !defaultGuard)
    {
        // the first-file size is only meaningful for largest-first (it names the main movie);
        // in original order the first file is typically a tiny playlist and "0 MB" reads broken
        if (originalOrder)
            LTRACE(LT_INFO, 2,
                   "  layer-break guard " << layerBreakGuardMB << " MB after"
                                          << (layerBreakGuardBeforeMB >= 0 ? " + custom before-zone" : "")
                                          << "; original file order");
        else
            LTRACE(LT_INFO, 2,
                   "  layer-break guard " << layerBreakGuardMB << " MB after"
                                          << (layerBreakGuardBeforeMB >= 0 ? " + custom before-zone" : "")
                                          << "; largest file first (" << static_cast<int64_t>(items[0].size / 1000000)
                                          << " MB)");
    }

    // Fit-to-disc check. Of the three padding paths only --inner-only ever consulted the target
    // capacity: it sizes its guard FROM the free space. A manual --layer-break-guard added its
    // padding with no reference to the capacity, and a plain build wrote the image with no
    // reference to it either. Measured on this tree: a folder left 2 MB under a BD50 produced an
    // image 2.06 MB OVER it, and a 288 MB guard on a full BD50 produced one 329 MB over. Both were
    // silent and both exited 0, so the first sign of trouble was a failed burn.
    //
    // Four things make up the finished image. Two are CERTAIN before a byte is written:
    //   payload  the source files, each rounded up to whole sectors
    //   guard    both zones of every break the payload actually reaches. The before-zone is a
    //            sixteenth of the after-zone with a 4 MB floor, unless --layer-break-guard-before
    //            sets it outright. A zone beyond the end of the payload is never padded at all
    // and two are only BOUNDED:
    //   overrun  checkLayerBreakPoint() pads from wherever the current write lands to the far edge
    //            of the zone, so a guarded break costs anywhere from nothing to one whole copy
    //            buffer MORE than the guard asked for. Measured 5.94 MB on a BD50 and 15.06 MB on
    //            a 1 GB payload: real, variable, and not knowable in advance
    //   UDF      the volume structures, which grow with the disc because the metadata partition
    //            takes another 64 KB block per 16 GB: 3.81 MB on a BD25, 4.06 on a BD50, 4.44 on
    //            a BD100
    //
    // That difference decides what may be refused. Payload plus guard zones is a true lower bound:
    // the pad always covers at least the whole zone, so nothing can bring the image below it and
    // exceeding the capacity there is a fact rather than a forecast. Refusing on it turns away no
    // correct build. The bounded two may not refuse anything, so a build that clears the lower
    // bound but lands inside them is reported as close rather than judged, and the finished image
    // is measured exactly further down, where no model is needed.
    //
    // Refusing up front costs a second, where finding out afterwards costs an hour of copying.
    constexpr int64_t UDF_MARGIN_SECTORS = 8192;  // ~16 MB, the same reserve --inner-only uses
    const int64_t payloadSect = (total + 2047) / 2048;
    int64_t guardZoneSectors = 0;     // certain: the pad is never smaller than this
    int64_t guardOverrunSectors = 0;  // bounded: 0 to one copy buffer for each break reached
    if (layerBreakGuardMB >= 0)
    {
        const int64_t afterSectors = static_cast<int64_t>(layerBreakGuardMB) * 1024 * 1024 / 2048;
        int64_t beforeSectors = static_cast<int64_t>(layerBreakGuardBeforeMB) * 1024 * 1024 / 2048;
        if (layerBreakGuardBeforeMB < 0)
        {
            // mirrors IsoWriter::setLayerBreakGuard(afterSectors): a sixteenth, with a 4 MB floor,
            // never larger than the after-zone itself
            constexpr int64_t floorSectors = 4 * 1024 * 1024 / 2048;
            beforeSectors = afterSectors / 16 < floorSectors ? floorSectors : afterSectors / 16;
            if (beforeSectors > afterSectors)
                beforeSectors = afterSectors;
        }
        const std::vector<int>& breaks = layerBreakLbns;  // filled above, as BlurayHelper fills them
        for (const int bp : breaks)
        {
            // A zone the payload never reaches costs nothing, so counting it would refuse builds
            // that fit. The payload alone reaching the zone is the conservative test: the header
            // and any earlier pad only push the data further in, never back.
            if (payloadSect > static_cast<int64_t>(bp) - beforeSectors)
            {
                guardZoneSectors += beforeSectors + afterSectors;
                guardOverrunSectors += overrunSectors;
            }
        }
    }
    // --inner-only is exempt from the ESTIMATE only. It has already done this arithmetic against the
    // same capacity, reserving both the UDF margin and the overrun, so its guard is by construction
    // whatever is left over; estimating it again would count those reserves twice and refuse every
    // inner-only build. The exact check below still applies, and matters most there, since that path
    // fills the disc deliberately.
    if (discCapacitySectors <= 0)
        LTRACE(LT_INFO, 2, "  no --disc-capacity given, so the image is NOT checked against a disc size");
    else if (innerOnly)
        LTRACE(LT_INFO, 2, "  capacity: --inner-only sized the guard from the disc; the image is checked when written");
    else
    {
        const int64_t leastSectors = payloadSect + guardZoneSectors;  // the image cannot come out smaller
        const double capMB = static_cast<double>(discCapacitySectors) / 512.0;
        if (leastSectors > discCapacitySectors)
        {
            std::ostringstream why;
            why << std::fixed << std::setprecision(1) << "the payload"
                << (guardZoneSectors > 0 ? " and guard need " : " needs ") << static_cast<double>(leastSectors) / 512.0
                << " MB before any UDF structure is written, and the disc holds " << capMB << " MB, so this image is "
                << static_cast<double>(leastSectors - discCapacitySectors) / 512.0 << " MB too big";
            if (guardZoneSectors > 0)
            {
                const int64_t roomSectors = discCapacitySectors - payloadSect;
                why << ". The guard asks for " << static_cast<double>(guardZoneSectors) / 512.0
                    << " MB of padding, and the payload leaves room for at most "
                    << static_cast<double>(roomSectors > 0 ? roomSectors : 0) / 512.0 << " MB";
            }
            if (allowOversize)
                LTRACE(LT_WARN, 2, "  capacity: " << why.str() << "; continuing because --allow-oversize was given");
            else
            {
                LTRACE(LT_ERROR, 2,
                       "  capacity: " << why.str() << ". " << (guardZoneSectors > 0 ? "Shrink the guard, use" : "Use")
                                      << " a larger disc, drop files, or pass --allow-oversize to build it anyway. "
                                         "Nothing has been written");
                return -1;
            }
        }
        else if (leastSectors + guardOverrunSectors + alignSectors + UDF_MARGIN_SECTORS > discCapacitySectors)
            // Clear of the lower bound, but inside the band the guard overrun and the UDF structures
            // decide. Saying "it fits" here would be a guess, so say what is actually known and let
            // the exact check at the end of the run settle it.
            LTRACE(LT_WARN, 2,
                   "  capacity: this will be close. "
                       << std::fixed << std::setprecision(1)
                       << static_cast<double>(discCapacitySectors - leastSectors) / 512.0 << " MB is left for the "
                       << (guardOverrunSectors > 0 ? "guard overrun (up to 16 MB) and the " : "")
                       << "UDF structures (about 4 MB, more on a larger disc). The finished image is measured against "
                          "the disc before this run ends");
        else
            LTRACE(LT_INFO, 2,
                   "  capacity: fits, with " << std::fixed << std::setprecision(1)
                                             << static_cast<double>(discCapacitySectors - leastSectors) / 512.0
                                             << " MB to spare before the UDF structures");
    }

    // An image this run makes and does not finish is removed: a refusal that comes while the image is written, such as
    // a path too long for the system after the files were copied, left an image of several MB that holds no disc. It
    // is made before the helper, so the helper has closed the image when it is removed. The .layerbreak.txt written
    // beside it goes with it, when this run made that as well.
    struct UnfinishedImage
    {
        std::string name;
        bool keep = false;
        bool sideFileExisted = true;
        bool quiet = false;       // the error already says the image is not kept, or name is a temporary name
        bool afterError = false;  // part of a mux that ends with its own error
        ~UnfinishedImage()
        {
            std::error_code ec;
            if (keep || name.empty())
                return;
            if (!sideFileExisted)
                std::filesystem::remove(fsPath(name + ".layerbreak.txt"), ec);
            if (std::filesystem::remove(fsPath(name), ec) && !quiet)
                noteRemovedOutput(name, afterError);
        }
    } unfinished;
    {
        std::error_code ec;
        if (!std::filesystem::exists(fsPath(outIso), ec))
        {
            unfinished.name = outIso;
            unfinished.sideFileExisted = std::filesystem::exists(fsPath(outIso + ".layerbreak.txt"), ec);
            unfinished.quiet = opt.tempName;
            unfinished.afterError = opt.partOfMux;
        }
    }
    BlurayHelper helper;
    if (!helper.open(outIso, DiskType::BLURAY, total, extraISOBlocks, opt.reproducibleHeader, layerBreakGuardMB,
                     layerBreakLbns, layerBreakGuardBeforeMB, discCapacitySectors > 0 ? discCapacitySectors * 2048 : 0,
                     defaultGuard))
    {
        LTRACE(LT_ERROR, 2, "Can't create output ISO " << outIso);
        return -1;
    }
    IsoWriter* iso = helper.isoWriter();
    if (!innerBefore.empty())
        iso->setLayerBreakGuards(innerBefore, innerAfter);
    // The volume label, as the direct ISO path sets it: "Blu-Ray" when none is given. Without --label, or with an empty
    // one, the image had a label of one space, which a file manager shows as a drive with no name.
    helper.setVolumeLabel(discLabel);

    // Emit "<pct>% complete" as the copy runs so the GUI progress bar advances. The copy is the slow
    // part (it may be reading an optical disc at only a few MB/s), so without this the bar sits at 0.0%
    // for the whole build and looks hung. Print only when the tenth-of-a-percent changes, so at most
    // ~1000 short lines are written regardless of the ISO size; std::endl flushes each so the GUI, which
    // reads stdout through a pipe, sees them live rather than after the buffer fills.
    // 16 MB: fewer syscalls and higher throughput on NVMe. Shares COPY_BUFFER_BYTES with the
    // capacity estimate above, which has to know how far ahead of a guard zone a write can land.
    vector<uint8_t> buf(COPY_BUFFER_BYTES);
    int64_t written = 0;
    int lastTenths = -1;
    // Per-file data ranges in absolute image sectors, aligned with `items`; used afterwards to map
    // each written guard pad back to the file (and playback position) it interrupted.
    struct FileRange
    {
        int64_t startLbn;
        int64_t endLbn;
        int64_t dataLbn;  // where the file's data begins: a stream file starts on a 64 KB boundary, past startLbn
    };
    vector<FileRange> ranges;
    ranges.reserve(items.size());
    int64_t restBytes = total;  // this item and every one after it
    int64_t restStreams = std::count_if(items.begin(), items.end(), isStreamItem);  // of them, the stream files
    for (auto& item : items)
    {
        // Layer-fit: start the file after the break instead of splitting it, when it fits there.
        // A folded 3D group is treated as one file here, which is what it is on the disc.
        // Only when the rest still fits behind the zone and, under --inner-only on more than two layers, the shares
        // stay even. The rest was checked under --inner-only only: a move leaves the end of the layer in front of the
        // zone unused, the capacity check above counts the data running on without that gap, and an image it said fits
        // came out too big after the whole copy. The rest counts the 64 KB start of each stream file and the overrun
        // of each zone still ahead, as the capacity check does; --inner-only keeps the reserve it sized its zones with.
        if (layerFit)
            iso->padOverZoneIfFileCrosses(
                item.size, discCapacitySectors, innerLayers > 0 ? restBytes : restBytes + restStreams * 31 * 2048,
                innerLayers > 2 ? overrunSectors : -1, isStreamItem(item), innerLayers > 0 ? 0 : overrunSectors);
        restBytes -= item.size;
        if (isStreamItem(item))
            --restStreams;
        const int64_t startLbn = iso->currentImageLBA();
        if (item.ssifGroup >= 0)
        {
            int64_t dataStartLbn = startLbn;
            if (!writeSsifGroup(iso, srcRoot, ssifGroups[item.ssifGroup], buf, written, total, lastTenths,
                                dataStartLbn))
                return -1;
            ranges.push_back({dataStartLbn, iso->currentImageLBA(), dataStartLbn});
            continue;
        }
        File in;
        if (!in.open(item.full.c_str(), File::ofRead))
        {
            LTRACE(LT_ERROR, 2, "Can't read " << item.full);
            return -1;
        }
        ISOFile* out = iso->createFile();
        out->open(item.rel.c_str(), File::ofWrite);
        const int64_t dataLbn = iso->currentImageLBA();
        int n;
        int64_t copied = 0;
        while ((n = in.read(buf.data(), static_cast<uint32_t>(buf.size()))) > 0)
        {
            if (out->write(buf.data(), static_cast<uint32_t>(n)) < 0)
            {
                LTRACE(LT_ERROR, 2, "Write error on " << item.rel);
                return -1;
            }
            copied += n;
            written += n;
            if (total > 0)
            {
                // The copy stops at 99.9 percent: the file system is still to be written and the size checked, and
                // either can fail. 100.0 is printed when the image is finished (reportJobComplete).
                const int tenths = (std::min)(
                    999, static_cast<int>(static_cast<double>(written) / static_cast<double>(total) * 1000.0));
                if (tenths != lastTenths)
                {
                    lastTenths = tenths;
                    cout << tenths / 10 << '.' << tenths % 10 << "% complete" << std::endl;
                }
            }
        }
        out->close();
        delete out;
        in.close();
        // A read that fails part way, a scratched disc or a mount that drops, ended the loop above as if the file had
        // ended there: the file went into the image short and the run finished as if nothing had happened. A 3D
        // group (writeSsifGroup) already stopped on it.
        if (n < 0 || copied != item.size)
        {
            LTRACE(LT_ERROR, 2,
                   "Error: " << item.full << " could not be read in full (" << copied << " of " << item.size
                             << " bytes), so the image is not finished and cannot be used.");
            return -1;
        }
        ranges.push_back({startLbn, iso->currentImageLBA(), dataLbn});
    }
    LTRACE(LT_INFO, 2,
           "Finishing the image: writing the last folders and files and the file system"
               << (discCapacitySectors > 0 ? ", and checking that it fits the disc..." : "..."));
    // Lay down the standard BD folder skeleton (empty META/BDJO/JAR/AUXDATA, CERTIFICATE, and the BACKUP
    // sub-tree), exactly as tsMuxeR's own BD authoring does via the same call. createDir() is idempotent,
    // so folders that already hold copied content are left untouched.
    if (hasBdmv)
        helper.createBluRayDirs();
    // The BACKUP copies go behind everything else, and before the pads are read, as their writing can lay one
    iso->writeDeferredFiles();

    const auto pads = iso->layerBreakPads();  // copy: helper.close() finalizes the writer
    int64_t mirrorFrom = 0, mirrorTo = 0;     // the sectors the metadata mirror took out of the data, if any
    const bool mirrorInData = iso->mirrorGap(mirrorFrom, mirrorTo);
    helper.close();

    // Human-readable map of every guard pad: which break, how much fill on each side, which file it
    // interrupted, and (for stream files) the playback time. Printed to the log AND written as a
    // sidecar "<out.iso>.layerbreak.txt" so verification tools know where to look on the disc.
    if (!pads.empty())
    {
        auto isM2ts = [](const string& s)
        {
            if (s.size() < 5)
                return false;
            string ext = s.substr(s.size() - 5);
            for (auto& c : ext) c = static_cast<char>(tolower(c));
            return ext == ".m2ts";
        };
        std::ostringstream side;
        side << "tsMuxeR layer break map (sectors of 2048 bytes)\n";
        // the image's own name: over an existing image, outIso is the temporary name, gone after the swap
        side << "image: " << shownOutputName(outIso) << "\n";
        for (const auto& pad : pads)
        {
            // The fill can start past the break: a break below the first data sector (warned about when it is read),
            // or a write that stood inside the zone already. Then all of it lies after the break, and the "before"
            // came out negative.
            const int64_t fillAtBreak = (std::max)(pad.padStartLbn, static_cast<int64_t>(pad.breakLbn));
            const double beforeMB = static_cast<double>(fillAtBreak - pad.padStartLbn) / 512.0;
            const double afterMB = static_cast<double>(pad.padEndLbn - fillAtBreak) / 512.0;
            std::ostringstream line;
            line << "break " << pad.breakLbn << ": zeros " << pad.padStartLbn << " .. " << pad.padEndLbn << " ("
                 << std::fixed << std::setprecision(1) << beforeMB << " MB before + " << afterMB << " MB after)";
            string loc;
            for (size_t j = 0; j < items.size() && loc.empty(); ++j)
            {
                if (pad.padStartLbn > ranges[j].startLbn && pad.padStartLbn < ranges[j].endLbn)
                {
                    // pad splits this file; in-file offset excludes any earlier pads inside the same file
                    int64_t off = (pad.padStartLbn - ranges[j].dataLbn) * 2048;
                    for (const auto& p2 : pads)
                        if (p2.padStartLbn > ranges[j].startLbn && p2.padStartLbn < pad.padStartLbn)
                            off -= (p2.padEndLbn - p2.padStartLbn) * 2048;
                    if (mirrorInData && mirrorFrom >= ranges[j].dataLbn && mirrorFrom < pad.padStartLbn)
                        off -= (mirrorTo - mirrorFrom) * 2048;
                    loc = "inside " + items[j].rel + " at file offset " + std::to_string(off);
                    uint64_t pcr0 = 0, pcrX = 0;
                    if (isM2ts(items[j].rel) && findM2tsPcrMedian(items[j].full, 0, +1, &pcr0) &&
                        findM2tsPcrMedian(items[j].full, off, -1, &pcrX) && pcrX >= pcr0)
                    {
                        const auto sec = static_cast<int64_t>((pcrX - pcr0) / 90000);
                        std::ostringstream ts;
                        ts << sec / 3600 << ":" << std::setw(2) << std::setfill('0') << (sec / 60) % 60 << ":"
                           << std::setw(2) << std::setfill('0') << sec % 60;
                        loc += ", playback time about " + ts.str();
                    }
                }
                else if (pad.padEndLbn == ranges[j].startLbn)
                    loc = "between files; " + items[j].rel + " starts on the next layer";
            }
            if (loc.empty())
                loc = "location not matched to a file";
            LTRACE(LT_INFO, 2, "  " << line.str());
            LTRACE(LT_INFO, 2, "    " << loc);
            side << line.str() << "\n  " << loc << "\n";
        }
        std::ofstream sideFile(fsPath(outIso + ".layerbreak.txt"), std::ios::binary);
        if (sideFile)
            sideFile << side.str();
    }
    // Now the exact answer. The estimate above had to reserve a margin for the UDF structures and
    // the guard overrun; the finished file needs no estimate, so measure it. This is also what
    // catches anything the model above does not know about, instead of leaving it to a failed burn.
    if (discCapacitySectors > 0)
    {
        int64_t isoBytes = -1;
        File img;
        if (img.open(outIso.c_str(), File::ofRead))
        {
            isoBytes = img.size();
            img.close();
        }
        if (isoBytes < 0)
            LTRACE(LT_WARN, 2, "  capacity: cannot re-open the finished image, so its size was not checked");
        else
        {
            const int64_t capBytes = discCapacitySectors * 2048;
            const double diffMB = static_cast<double>(isoBytes - capBytes) / (1024.0 * 1024.0);
            if (isoBytes > capBytes)
            {
                // The image is kept either way: it took a long time to write and deleting a user's
                // output to make a point would be worse than telling them plainly.
                if (allowOversize)
                    LTRACE(LT_WARN, 2,
                           "  capacity: the image is " << std::fixed << std::setprecision(2) << diffMB
                                                       << " MB TOO BIG for the disc and will not burn; kept because "
                                                          "--allow-oversize was given");
                else
                {
                    LTRACE(LT_ERROR, 2,
                           "  capacity: the image is " << std::fixed << std::setprecision(2) << diffMB
                                                       << " MB TOO BIG for the disc and will not burn (" << isoBytes
                                                       << " bytes against " << capBytes << ")."
                                                       << (opt.keepTooBigImage ? " It was kept at " + outIso : ""));
                    unfinished.keep = opt.keepTooBigImage;
                    unfinished.quiet = true;  // the mux's error says the image is not kept
                    if (sizeRefused)
                        *sizeRefused = true;
                    return -1;
                }
            }
            else
                LTRACE(LT_INFO, 2,
                       "  capacity: the image fits the disc, with " << std::fixed << std::setprecision(2) << -diffMB
                                                                    << " MB to spare");
        }
    }
    else if (opt.layerBreakLbns.empty())
        reportBd50Layout(outIso, "--disc-capacity=48878592", "--disc-capacity=32585728");
    unfinished.keep = true;
    // Written in place over an image that was there (--bdmv-to-iso), an image with no pads left the layer break map of
    // the old image beside it, which described a disc that was no longer there. Over a temporary name (KeepOldOutput)
    // the old map is removed when the new image takes the old one's place.
    if (pads.empty() && unfinished.name.empty() && !opt.tempName)
    {
        std::error_code ec;
        std::filesystem::remove(fsPath(outIso + ".layerbreak.txt"), ec);
    }
    if (!opt.partOfMux)  // a mux prints it with its own success
        reportJobComplete();
    LTRACE(LT_INFO, 2, "bdmv-to-iso complete -> " << shownOutputName(outIso));
    return 0;
}

// Say so when a track lost data on the way through.
//
// A track read under a codec name that does not match what is actually in it does not fail. The
// reader hunts for the next frame it recognises, throws away everything in between, and the run
// ends with "Demux complete", exit 0, and a file that probes as healthy. Measured on a Blu-ray
// TrueHD track carrying an AC-3 compatibility core, asked for as A_MLP: 1,841,388 bytes out of
// 12,177,210, which decodes 1.54 seconds of a 39.87 second track. Every label on that file is
// correct. Nothing on screen said that six sevenths of it had been dropped, because the only sign
// was 375 "Resync stream" lines scattered through the log.
//
// MEASURED AT THE END, from what the readers actually did, rather than predicted from the meta
// file. Three attempts to predict it were built and all three were withdrawn: each read the
// container up front to decide, and that read is exactly what tsMuxeR's shared streaming reader
// cannot afford, so each broke joins or refused correct input. An outcome cannot false refuse.
//
// A WARNING RATHER THAN A REFUSAL. What could be read is a usable output, and a refusal would lose
// that part as well: a stream read as the wrong codec, or one with damaged bytes, is muxed as far as
// it can be read. What was missing is the size of the loss, and that is the part worth adding.
//
// The threshold is any loss at all, after the first frame is found. That needs no tuning: 48
// correct sources measured here lose exactly zero bytes, and the 32 that begin part way through a
// frame account for all of their skip before the first sync, which getLostSize does not count.
// One line per track that lost data, and the explanation ONCE.
//
// ** THE EXPLANATION USED TO BE REPEATED PER TRACK, AND THAT GETS WORSE THE MORE TRACKS ARE
// AFFECTED. ** Measured: a six track job printed six warnings of 427 characters
// each, 2,562 in total, identical apart from the first 55 - about 32 lines of near duplicate prose
// on an 80 column console, in which the six facts that actually differ are the least visible part.
// A six track Blu-ray demux is an everyday job, and the signal to noise ratio FELL as the number of
// affected tracks rose, which is the wrong direction.
//
// A SINGLE track still prints exactly the sentence it always did: that is the common case, and the
// suite asserts its shape.
// ** A SPLIT PART THAT CANNOT DECODE ON ITS OWN, WRITTEN WITHOUT A WORD. **
//
// H.264 carries its parameter sets once at the start unless asked otherwise, and splitting cuts the
// stream into files that each have to stand alone. Every part after the first therefore refers to a
// parameter set that is not in it. Measured on an ordinary 11 MB source split at 3 MB: the first
// part decodes 203 frames and the other three fail with "non-existing PPS 0 referenced", while
// tsMuxeR reports "Mux successful complete". On a Blu-ray that is three unplayable clips out of
// four.
//
// contSPS already exists and the help already calls it recommended for BD muxing. What was missing
// is anything telling the user that this particular job needed it. Nothing here changes an output
// byte: it is a warning, and the worst a mistake in it can do is print a line that did not apply.
//
// It stays quiet when the source already repeats its parameter sets per GOP, because then each part
// carries its own and the option is not needed.
// --dv-profile is read by the Matroska muxer and by nothing else, so on any other output it was
// accepted, did nothing, and said nothing: measured, the transport stream produced with it is md5
// identical to the one produced without it. Say so, rather than leave the user believing a
// conversion happened. A warning and not a refusal, following the split options, which are also
// simply not implemented for one output and now say so instead of ignoring the request.
static void reportDvProfileIgnored(const MuxerManager& muxerManager, const char* outputName)
{
    // The option, not the text of the line: a label holding "--dv-profile" gave this warning.
    bool given = false;
    for (const std::string& token : splitQuotedStr(muxerManager.getMuxOpts().c_str(), ' '))
        if (const vector<string> option = splitOption(trimStr(token)); !option.empty() && option[0] == "--dv-profile")
            given = true;
    if (!given)
        return;
    LTRACE(LT_WARN, 2,
           "Warning: --dv-profile was ignored, because it applies to Matroska output only and this is "
               << outputName
               << ". Matroska has to carry a dual layer Dolby Vision source in ONE track, and the option "
                  "chooses how its metadata is written when that happens. A disc and a transport stream "
                  "carry the two layers as two streams, the way the source disc does, so there is nothing "
                  "to convert. The output is byte for byte what it would have been without the option.");
}

// Options that do nothing for the output at hand were ignored without a word: --auto-chapters outside a disc, chapter
// times on a demux, the Matroska title, track names and flags outside a Matroska file, and a track of a kind the
// Matroska file name leaves out (.mka for sound, .mks for subtitles).
static void reportOptionsIgnored(const MuxerManager& muxerManager, const int autoChapterLen, const bool disc,
                                 const bool demux, const bool chaptersGiven, const std::string& mkvExt)
{
    if (autoChapterLen > 0 && !disc)
        LTRACE(LT_WARN, 2,
               "Warning: --auto-chapters is used for a Blu-ray or AVCHD only, and is ignored for this output. Give "
               "the chapter times with --custom-chapters instead: a Matroska file writes them, and --split-chapters "
               "splits at them.");
    if (demux && chaptersGiven)
        LTRACE(LT_WARN, 2,
               "Warning: --custom-chapters and --chapters-file are not used on a demux. A demux writes chapters.txt "
               "from the source's own chapters only.");
    if (mkvExt.empty())
    {
        bool title = false;
        for (const std::string& token : splitQuotedStr(muxerManager.getMuxOpts().c_str(), ' '))
            if (const vector<string> option = splitOption(trimStr(token)); !option.empty() && option[0] == "--title")
                title = true;
        if (title)
            LTRACE(LT_WARN, 2,
                   "Warning: --title is written into a Matroska file only, and is ignored for this output.");
        for (const StreamInfo& si : muxerManager.getStreamInfo())
            for (const char* name :
                 {"track-name", "hearing-impaired", "visual-impaired", "text-descriptions", "original", "commentary"})
                if (si.m_addParams.count(name))
                    LTRACE(LT_WARN, 2,
                           "Warning: " << name << " is written into a Matroska file only, and is ignored for track "
                                       << si.m_streamReader->getStreamIndex() << " of this output.");
        return;
    }
    const char kind = mkvExt == "MKA" ? 'A' : mkvExt == "MKS" ? 'S' : 0;
    if (kind == 0)
        return;
    for (const StreamInfo& si : muxerManager.getStreamInfo())
        if (!si.m_codec.empty() && si.m_codec[0] != kind && (si.m_codec[0] == 'V' || si.m_codec[0] == 'A'))
        {
            LTRACE(LT_WARN, 2,
                   "Warning: the output is named ." << strToLowerCase(mkvExt) << ", the name of a Matroska file of "
                                                    << (kind == 'A' ? "sound" : "subtitles") << " only, and track "
                                                    << si.m_streamReader->getStreamIndex() << " is "
                                                    << (si.m_codec[0] == 'V' ? "video" : "audio")
                                                    << ". Name it .mkv so that a player expects every track.");
            return;
        }
}

// A single layer Dolby Vision stream (profile 8) on a disc: the Dolby Vision data travels inside the HDR10 picture,
// and a UHD Blu-ray player takes Dolby Vision from two layers only, so the disc plays as its HDR10, SDR or HLG base.
// The disc is written as asked; this only says so, which was left to be found out on a television. A disc with an
// enhancement layer on its own PID (0x1015) is a dual layer disc, whichever of the two is listed first, and then
// nothing is said: that includes a minimal enhancement layer made from a profile 8 file. A second video that is no
// layer, such as a plain 1080p picture beside a 4K profile 8.1 one, goes to a PID of its own and the disc still plays
// as the base: it was counted as a layer, and the warning was not given. Profile 5 has no such base and is warned about
// by the reader itself.
static void reportSingleLayerDvOnDisc(const MuxerManager& muxerManager)
{
    for (const AbstractMuxer* muxer : {muxerManager.getMainMuxer(), muxerManager.getSubMuxer()})
        if (const auto* ts = dynamic_cast<const TSMuxer*>(muxer))
            for (const auto& [pid, info] : ts->getPidList())
                if (info.isDvEnhancement)
                    return;
    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        if (si.m_codec.empty() || si.m_codec[0] != 'V' || si.m_streamReader == nullptr ||
            si.m_streamReader->isSecondary() || si.m_isSubStream)
            continue;
        const auto* hevc = dynamic_cast<const HEVCStreamReader*>(si.m_streamReader);
        if (hevc == nullptr || si.m_streamReader->getStreamHDR() != 4)
            continue;
        int profile = 0;
        int compatibility = 0;
        // A merged dual layer track given on one line (no subTrack=) goes to the disc as one stream, its enhancement
        // layer beside its base on the same PID, where a player does not look for it. It was called single layer
        // profile 8, which it is not, and the way to a dual layer disc was not named.
        if (hevc->holdsDvEnhancementLayer())
        {
            hevc->doViProfileAndCompatibility(true, profile, compatibility);
            LTRACE(LT_WARN, 2,
                   "Warning: the video track holds both layers of a dual layer Dolby Vision stream (profile "
                       << profile
                       << "), and on one meta line it goes to the disc as one stream. A UHD Blu-ray player takes the "
                          "enhancement layer from a stream of its own only, so this disc plays as HDR10. For a dual "
                          "layer disc give the track on two lines, one with subTrack=1 (the base layer) and one with "
                          "subTrack=2 (the enhancement layer).");
            return;
        }
        hevc->doViProfileAndCompatibility(false, profile, compatibility);
        if (compatibility == 0)
            continue;
        const char* base = compatibility == 2 ? "SDR" : compatibility == 4 ? "HLG" : "HDR10";
        LTRACE(LT_WARN, 2,
               "Warning: the video is single layer Dolby Vision (profile "
                   << profile
                   << "). A UHD Blu-ray player plays Dolby Vision from "
                      "two layers only, so this disc plays as "
                   << base << ".");
        return;
    }
}

// HLG video (transfer characteristics 18) on a Blu-ray or AVCHD disc. The playlist and clip information of a disc have
// no dynamic range for HLG (they know SDR, HDR10 and Dolby Vision): the stream is marked as SDR with its colours
// (composeStreamCodingInfo), and a disc player shows it as SDR. It went onto the disc without a word. The disc is
// written as asked; this only says so, once for each track. H.264 is asked as well: its VUI gives the same value. A
// Dolby Vision stream with an HLG base is asked too: the disc marks it as Dolby Vision, but a player without Dolby
// Vision plays its HLG base, and so does a UHD Blu-ray player for single layer Dolby Vision (above). Transfer 14 and 15
// are the SDR transfer of BT.2020 (the reader takes them as SDR in markHdr10ByMasteringSei), so they are not HLG here.
static void reportHlgOnDisc(const MuxerManager& muxerManager)
{
    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        if (si.m_codec.empty() || si.m_codec[0] != 'V' || si.m_isSubStream)
            continue;
        const auto* video = dynamic_cast<const MPEGStreamReader*>(si.m_streamReader);
        uint8_t primaries = 0, transfer = 0, matrix = 0;
        if (video == nullptr || !video->getColourDesc(primaries, transfer, matrix) || transfer != 18)
            continue;
        const auto trackParam = si.m_addParams.find("track");
        std::ostringstream who;
        if (trackParam != si.m_addParams.end() && !trackParam->second.empty())
            who << "track " << trackParam->second << " of ";
        who << (si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName);
        const bool dv = si.m_streamReader->getStreamHDR() == 4;
        LTRACE(LT_WARN, 2,
               "Warning: the video of " << who.str() << " is " << (dv ? "Dolby Vision with an HLG picture" : "HLG")
                                        << " (transfer characteristics 18). A disc has no HLG dynamic range, "
                                        << (dv ? "and a player that plays the HLG picture shows it as SDR."
                                               : "so the disc marks it as SDR and a player shows it as SDR.")
                                        << " The disc is written as asked.");
    }
}

// --disc-size is checked before the mux against an estimate, the sources and five per cent, which counts neither a
// layer break guard, nor the null packets of a constant bitrate, nor the file system. The window promises that the
// finished size is checked against the disc and that it warns when it does not fit, and nothing did. The finished image
// is measured now, or for a disc folder the files in it; a folder becomes slightly larger as an image, by its file
// system.
static void reportFinishedDiscSize(const std::string& outputRoot, const int64_t discLimit, const int64_t extraIsoBlocks)
{
    if (discLimit <= 0)
        return;
    int64_t size = 0;
    int64_t sectorBytes = 0;  // each file in whole sectors, as an image holds it
    const bool image = strEndWith(strToLowerCase(outputRoot), ".iso");
    if (image)
        size = static_cast<int64_t>(getFileSize(outputRoot));
    else
    {
        // Only what the disc holds: the BDMV and CERTIFICATE folders. Every file in the output folder was counted, so a
        // source kept there put a disc of 2.4 MB over a limit of 3 MB.
        for (const char* sub : {"BDMV", "CERTIFICATE"})
        {
            std::vector<std::string> files;
            findFilesRecursive(outputRoot + getDirSeparator() + sub + getDirSeparator(), "*", &files);
            for (const std::string& f : files)
            {
                const auto bytes = static_cast<int64_t>(getFileSize(f));
                size += bytes;
                sectorBytes += static_cast<int64_t>(roundUp64(static_cast<uint64_t>(bytes), SECTOR_SIZE));
            }
        }
    }
    if (image)
    {
        if (size > discLimit)
            LTRACE(LT_WARN, 2,
                   "Warning: the finished image is " << size << " bytes, more than the " << discLimit
                                                     << " bytes of --disc-size, so it does not fit that disc.");
        else
            LTRACE(LT_INFO, 2,
                   "The finished image is " << size << " bytes, within the " << discLimit << " bytes of --disc-size.");
        return;
    }
    // A disc is burnt from an image, which adds its file system to the files: a folder of 39,618,392 bytes was
    // "within" 40,000,000 while the image of the same job is 41,353,216. The image is reckoned as IsoWriter lays
    // it out: 640 KB in front, the metadata area and its mirror, the files in whole sectors, a 64 KB block to align,
    // and 576 KB at the end, in whole blocks of 64 KB. It is the size of an .iso output of the same job; an image made
    // from the folder with --bdmv-to-iso reserves more metadata and is somewhat larger.
    const int64_t meta = IsoWriter::metadataBlocks(size, extraIsoBlocks) * ALLOC_BLOCK_SIZE;
    const int64_t imageEstimate = static_cast<int64_t>(
        roundUp64(static_cast<uint64_t>(static_cast<int64_t>(METADATA_START_ADDR) * SECTOR_SIZE + 2 * meta +
                                        sectorBytes + ALLOC_BLOCK_SIZE + 1024 * 512 + ALLOC_BLOCK_SIZE),
                  ALLOC_BLOCK_SIZE));
    if (imageEstimate > discLimit)
        LTRACE(LT_WARN, 2,
               "Warning: the files of the finished disc folder come to "
                   << size << " bytes, and an image of them, with its file system, to about " << imageEstimate
                   << " bytes, more than the " << discLimit << " bytes of --disc-size, so it does not fit that disc.");
    else
        LTRACE(LT_INFO, 2,
               "The files of the finished disc folder come to "
                   << size << " bytes, and an image of them, with its file system, to about " << imageEstimate
                   << " bytes, within the " << discLimit << " bytes of --disc-size.");
}

static void reportSplitWithoutParameterSets(const MuxerManager& muxerManager)
{
    const auto mainMuxer = dynamic_cast<TSMuxer*>(muxerManager.getMainMuxer());
    if (mainMuxer == nullptr || !mainMuxer->isSplitting())
        return;

    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        // Every other H.264 stream now gets its parameter sets at the start of each part (h264StreamReader.cpp,
        // newFileNeedsSpsPps). The second view of a 3D stream keeps the old rule, so only it can still be affected.
        const auto reader = dynamic_cast<H264StreamReader*>(si.m_streamReader);
        if (reader == nullptr || !reader->isSubStream() || reader->spsPpsRepeatedInStream())
            continue;
        const std::string name = si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName;
        LTRACE(LT_WARN, 2,
               "Warning: " << name
                           << " (the second view of a 3D stream) was split, and its parameter sets appear only "
                              "once at the start, so every part after the first refers to a parameter set it does "
                              "not contain and will not decode on its own. Add contSPS to that line to repeat them "
                              "before each key frame. The parts already written are affected; this is not a "
                              "warning about the next run.");
    }
}

// ** A TRACK THAT PRODUCED NOTHING USED TO SAY NOTHING. **
//
// The codec on a meta line is taken as fact and nothing checks it against the file. Naming the
// wrong one is quiet in every direction:
//
//   A_AC3 on an H.264 stream      exit 0, no output, no message
//   A_DTS on an H.264 stream      exit 0, no output, no message
//   V_MPEG4/ISO/AVC on a TRUE-HD  exit 0, a 17 MB file, "Processed 0 video frames"
//
// A coreless TRUE-HD named A_AC3 is the same fault wearing a disguise: the AC-3 reader scans a
// stream that has no AC-3 in it, finds nothing, and takes minutes to find nothing, while the same
// file named A_MLP muxes in two seconds.
//
// Nothing here guesses what the codec should have been. It reports what happened: this track was
// read and no frame of the codec it was named as came out of it.
// A mux that stops before it has read its sources used to print "Mux successful complete" and
// exit 0. One report had 584 of 148368 frames written, no output file at all, that message, and a
// progress line stuck at 87.3 percent. The figure was there the whole time and nothing asked it.
//
// getDemuxedSize over totalSize is exactly what the progress line shows. Measured on complete jobs
// it reaches 100.0 percent, including a job that takes one track out of a container and a job whose
// source file is truncated, because a short file is still read to its end.
//
// A deliberately partial job is left alone: --cut-start and --cut-end stop early because they were
// asked to. Splitting is not excluded, since a split job reads everything it was given.
// ** AN OUTPUT THAT IS ONE OF THE SOURCES IS REFUSED BEFORE ANYTHING IS WRITTEN. ** The output was opened for writing,
// which empties it, while the same file was still to be read: a transport stream given as its own output kept 4 of its
// 25 MB and a Matroska file was cut at 6 MB, and both runs ended with an error over a source that was gone. Names are
// compared as the system does, so another case, other slashes or a relative path are the same file.
// The sources are every file a track reads, not only the names on its line: a playlist line names the .mpls, and
// its clips were not compared, so an M2TS output named as one of them, or a disc written into the folder of the
// disc it reads, wrote over a clip while it was being read. A disc output writes into its BDMV and CERTIFICATE
// folders, so a source inside those is refused as well.
static bool pathIsInside(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    std::error_code ec;
    const std::filesystem::path c = std::filesystem::weakly_canonical(child, ec);
    if (ec)
        return false;
    const std::filesystem::path p = std::filesystem::weakly_canonical(parent, ec);
    if (ec)
        return false;
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
    {
        if (pi->empty())
            continue;
        if (ci == c.end())
            return false;
#ifdef _WIN32
        if (strToLowerCase(pi->string()) != strToLowerCase(ci->string()))
            return false;
#else
        if (*pi != *ci)
            return false;
#endif
    }
    return true;
}

// ** AN EXISTING OUTPUT IS KEPT UNTIL THE NEW ONE IS COMPLETE. ** Some refusals can only come during the mux: a
// cut past the end of the sources, a split that runs out of clip numbers, a disc whose PMT is too long, a track
// that turns out not to be what its line says. An output of the same name had been opened by then, so a refused
// job left an empty image, or a disc folder with an empty clip, where a finished one had been. When the output
// already exists, the new one is written under a temporary name beside it and takes its place only when the job
// has succeeded; a job that fails removes what it wrote and leaves the old output as it was. An output that does
// not exist yet is written in place, as before. A demux into a folder that exists is written into a temporary folder
// inside it: a demux refused at its end (a cut past the end of the sources) had emptied the files of an earlier demux
// of the same sources.
// ** AN OUTPUT THE JOB MADE GOES WHEN THE JOB FAILS. ** An output that did not exist was written in place and left as
// far as the job had come: a meta line with the wrong codec, a track a WebM file cannot hold or a mux that stopped
// before its sources were read left a file, a disc folder or a demux folder that looked like a result. Only what this
// job made goes: the file and its split parts, the BDMV and CERTIFICATE folders of a disc folder, the files of a demux
// folder, and the folders made for them; a folder that was there before is never removed, nor anything in it the
// job did not write. One line says so. A source that is cut off is the one exception: the job ends with an error that
// says the output holds what the file has, and a new output is kept then (keepNewOutput), as that message says.
class KeepOldOutput
{
   public:
    KeepOldOutput() = default;
    KeepOldOutput(const KeepOldOutput&) = delete;
    KeepOldOutput& operator=(const KeepOldOutput&) = delete;
    ~KeepOldOutput()
    {
        setShownOutputName({}, {});
        if (!m_committed)
            removeFreshOutput();
        if (!m_armed || m_committed || m_swapBegun)
            return;
        std::error_code ec;
        if (m_folder || m_demux)
            std::filesystem::remove_all(m_tmp, ec);
        else
            for (const auto& p : ownFiles()) std::filesystem::remove(p, ec);
    }

    // A file output (TS, M2TS, ISO, Matroska): returns the name to write. Its split parts are name.split.n.ext.
    std::string forFile(const std::string& dst, const int64_t estimate)
    {
        const std::filesystem::path out = fsPath(dst);
        const std::filesystem::path dir = out.has_parent_path() ? out.parent_path() : std::filesystem::path(".");
        const std::string stem = pathStr(out.stem());
        const std::string ext = pathStr(out.extension());
        std::error_code ec;
        bool exists = std::filesystem::exists(out, ec);
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (startsWithName(pathStr(e.path().filename()), stem + ".split."))
                exists = true;
        if (!exists)
        {
            // Neither the file nor a split part of it is there: whatever of them a failed job writes goes again. An
            // image this job makes and does not finish holds no disc: it goes also when a source is cut off.
            m_freshFile = out;
            m_freshName = dst;
            m_freshDir = dir;
            m_freshSplitPrefix = stem + ".split.";
            m_freshIsImage = strToLowerCase(ext) == ".iso";
            m_createdDirs = missingFolders(dir);
            return dst;
        }
        m_dir = dir;
        m_oldPrefix = stem + ".";
        m_ext = ext;
        // A name of up to 255 bytes, as a file name may have, made a temporary name longer than that: it could not be
        // created, and an existing output of such a name could no longer be replaced. The stem is cut short in the
        // temporary name, at a character, so that it, its split parts (".split.<n>") and the layer break map of an
        // image stay within 255 bytes.
        const std::string tag = ".tsmuxer-new-" + std::to_string(processId()) + ".";
        const size_t longest = tag.size() + std::string("split.99999.layerbreak.txt").size() + ext.size();
        size_t keep = longest < 255 ? (std::min)(stem.size(), 255 - longest) : 0;
        while (keep > 0 && keep < stem.size() && (static_cast<uint8_t>(stem[keep]) & 0xC0) == 0x80)
            --keep;  // not inside a UTF-8 sequence
        m_newPrefix = stem.substr(0, keep) + tag;
        if (strToLowerCase(ext) == ".iso")
            m_imageSideFile = ext.substr(1) + ".layerbreak.txt";
        m_armed = true;
        warnRoom(dir, estimate);
        // the messages name stem.ext and stem.split.n.ext, as the user gave them
        const std::string written = pathStr(dir / u8path(m_newPrefix.substr(0, m_newPrefix.size() - 1)));
        setShownOutputName(written, strEndWith(dst, ext) ? dst.substr(0, dst.size() - ext.size())
                                                         : pathStr(dir / u8path(stem)));
        if (ext.empty())
            return written;
        return pathStr(dir / u8path(m_newPrefix + ext.substr(1)));
    }

    // A Blu-ray or AVCHD folder: the disc is written into a folder beside it, and its BDMV and CERTIFICATE folders
    // replace the old ones. Anything else in the folder is not touched.
    std::string forDiscFolder(const std::string& dst, const int64_t estimate)
    {
        const std::filesystem::path out = fsPath(dst);
        std::error_code ec;
        if (!std::filesystem::exists(out / "BDMV", ec) && !std::filesystem::exists(out / "CERTIFICATE", ec))
        {
            m_fresh = out;
            m_freshName = dst;
            m_freshFolderExisted = std::filesystem::exists(out, ec);
            m_createdDirs = missingFolders(out);
            return dst;
        }
        std::filesystem::path base = out;
        if (!base.has_filename())
            base = base.parent_path();
        m_dir = out;
        m_tmp = base;
        m_tmp += u8path(".tsmuxer-new-" + std::to_string(processId()));
        m_folder = true;
        m_armed = true;
        warnRoom(base.has_parent_path() ? base.parent_path() : std::filesystem::path("."), estimate);
        setShownOutputName(pathStr(m_tmp), dst);
        return pathStr(m_tmp);
    }

    // A demux folder: the files are written into a folder inside it, and replace the files of the same names there.
    // Every other file in the folder is not touched.
    std::string forDemuxFolder(const std::string& dst, const int64_t estimate)
    {
        const std::filesystem::path out = fsPath(dst);
        std::error_code ec;
        if (!std::filesystem::is_directory(out, ec))
        {
            // Only a folder this job makes. A file of that name is not a folder of the job's, and was never removed
            // with it: the demux fails on it and it stays.
            if (!std::filesystem::exists(out, ec))
            {
                m_freshDemux = out;
                m_freshName = dst;
                m_createdDirs = missingFolders(out);
            }
            return dst;
        }
        m_dir = out;
        m_tmp = out / u8path(".tsmuxer-new-" + std::to_string(processId()));
        std::filesystem::remove_all(m_tmp, ec);  // left by a job of an earlier process with the same number
        m_demux = true;
        m_armed = true;
        warnRoom(out, estimate);
        setShownOutputName(pathStr(m_tmp), dst);
        return pathStr(m_tmp);
    }

    // The job ends with an error over a source that is cut off, and that error says the output holds what the file
    // has: a new file or disc folder is kept, as before. An image still goes, and an old output stays as it was.
    void keepNewOutput() { m_keepNew = true; }

    // The output is written under a temporary name, as one of that name exists
    [[nodiscard]] bool armed() const { return m_armed; }

    // The job has succeeded: the new output takes the old one's place. When that fails, the new output is left where
    // the message says it is: it used to be removed as the job ended, after the message had named it as complete.
    void commit()
    {
        if (!m_armed || m_committed)
        {
            m_committed = true;
            return;
        }
        m_swapBegun = true;
        std::error_code ec;
        if (m_demux)
        {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(m_tmp, ec)) files.push_back(e.path());
            if (ec)
                throw runtime_error("The new demux is complete in " + pathStr(m_tmp) +
                                    ", but its files could not be moved into " + pathStr(m_dir) + ": " + ec.message());
            for (const auto& p : files)
            {
                const std::filesystem::path to = m_dir / p.filename();
                std::filesystem::rename(p, to, ec);
                if (ec)
                    throw runtime_error("The new demux is complete in " + pathStr(m_tmp) + ", but " +
                                        pathStr(p.filename()) + " could not replace " + pathStr(to) + ": " +
                                        ec.message());
            }
            std::filesystem::remove_all(m_tmp, ec);
        }
        else if (m_folder)
        {
            for (const char* sub : {"BDMV", "CERTIFICATE"})
            {
                std::filesystem::remove_all(m_dir / sub, ec);
                if (std::filesystem::exists(m_tmp / sub))
                {
                    std::filesystem::rename(m_tmp / sub, m_dir / sub, ec);
                    if (ec)
                        throw runtime_error("The new disc is complete in " + pathStr(m_tmp) + ", but its " + sub +
                                            " folder could not be moved into " + pathStr(m_dir) + ": " + ec.message());
                }
            }
            std::filesystem::remove_all(m_tmp, ec);
        }
        else
        {
            // An image with no layer break pads has no layer break map of its own: the map of the old image stayed
            // beside it, and described a disc that was no longer there. It goes once the new image is in place.
            const bool newSideFile =
                !m_imageSideFile.empty() && std::filesystem::exists(m_dir / u8path(m_newPrefix + m_imageSideFile), ec);
            std::vector<std::string> written;
            bool split = false;
            for (const auto& p : ownFiles())
            {
                const std::string name = pathStr(p.filename());
                written.push_back(m_oldPrefix + name.substr(m_newPrefix.size()));
                split = split || isSplitPart(written.back());
                const std::filesystem::path to = m_dir / u8path(written.back());
                std::filesystem::rename(p, to, ec);
                if (ec)
                    throw runtime_error("The new output is complete as " + pathStr(p) + ", but it could not replace " +
                                        pathStr(to) + ": " + ec.message());
            }
            if (!m_imageSideFile.empty() && !newSideFile)
                std::filesystem::remove(m_dir / u8path(m_oldPrefix + m_imageSideFile), ec);
            // A split job over the parts of an earlier one replaced parts 1 to n, and parts n+1 and on stayed beside
            // them without a word: the folder held one set of parts made by two jobs. Those parts go, and only parts
            // named as this job names its own (name.split.<number>.ext).
            if (split)
                removeStaleParts(written);
        }
        m_committed = true;
    }

   private:
    static std::filesystem::path u8path(const std::string& s) { return {std::u8string(s.begin(), s.end())}; }
    static std::string pathStr(const std::filesystem::path& p)
    {
        const std::u8string u = p.u8string();
        return {u.begin(), u.end()};
    }
    static bool startsWithName(const std::string& name, const std::string& prefix)
    {
#ifdef _WIN32
        return strStartWith(strToLowerCase(name), strToLowerCase(prefix));
#else
        return strStartWith(name, prefix);
#endif
    }
    static unsigned long processId()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(GetCurrentProcessId());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }
    // name.split.<number>.ext, with the name and extension of this output
    bool isSplitPart(const std::string& name) const
    {
        const std::string prefix = m_oldPrefix + "split.";
        if (!startsWithName(name, prefix) || name.size() <= prefix.size() + m_ext.size() ||
            !startsWithName(name.substr(name.size() - m_ext.size()), m_ext))
            return false;
        const std::string number = name.substr(prefix.size(), name.size() - prefix.size() - m_ext.size());
        return std::all_of(number.begin(), number.end(), [](const char c) { return c >= '0' && c <= '9'; });
    }
    void removeStaleParts(const std::vector<std::string>& written) const
    {
        std::vector<std::filesystem::path> stale;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(m_dir, ec))
        {
            const std::string name = pathStr(e.path().filename());
            if (!e.is_regular_file(ec) || !isSplitPart(name))
                continue;
            if (std::none_of(written.begin(), written.end(),
                             [&](const std::string& w) { return w.size() == name.size() && startsWithName(name, w); }))
                stale.push_back(e.path());
        }
        std::sort(stale.begin(), stale.end());
        for (const auto& p : stale)
        {
            std::filesystem::remove(p, ec);
            if (ec)
                LTRACE(LT_WARN, 2,
                       "Warning: " << pathStr(p) << ", a part of an earlier output that this job did not write, could "
                                   << "not be removed: " << ec.message());
            else
                LTRACE(LT_INFO, 2,
                       "Removed " << pathStr(p) << ", a part of an earlier output that this job did not "
                                  << "write.");
        }
    }
    std::vector<std::filesystem::path> ownFiles() const
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(m_dir, ec))
            if (startsWithName(pathStr(e.path().filename()), m_newPrefix))
                files.push_back(e.path());
        return files;
    }
    // The folders of p that are not there yet, p first: the job makes them, and a failed job removes those of them
    // that are empty when its output has gone.
    static std::vector<std::filesystem::path> missingFolders(std::filesystem::path p)
    {
        std::vector<std::filesystem::path> dirs;
        std::error_code ec;
        while (!p.empty() && !std::filesystem::exists(p, ec) && !ec)
        {
            dirs.push_back(p);
            if (!p.has_parent_path() || p.parent_path() == p)
                break;
            p = p.parent_path();
        }
        return dirs;
    }

    // Removes the file p, and says whether it had a byte in it. An empty file or folder that goes is not reported:
    // that was the rule for an empty disc skeleton and an empty demux folder before.
    static bool removeFile(const std::filesystem::path& p)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        const bool hadData = !ec && size > 0;
        return std::filesystem::remove(p, ec) && hadData;
    }

    // A failed job: the output it made in place goes (see above the class). Nothing that was there when the job began
    // is touched: forFile, forDiscFolder and forDemuxFolder name an output here only when it was not there.
    void removeFreshOutput() const
    {
        std::error_code ec;
        bool removedData = false;
        if (!m_freshFile.empty())
        {
            if (m_keepNew && !m_freshIsImage)
                return;
            // the file and its split parts: none of them was there (forFile)
            std::vector<std::filesystem::path> files;
            if (std::filesystem::is_regular_file(m_freshFile, ec))
                files.push_back(m_freshFile);
            for (const auto& e : std::filesystem::directory_iterator(m_freshDir, ec))
                if (startsWithName(pathStr(e.path().filename()), m_freshSplitPrefix) && e.is_regular_file(ec))
                    files.push_back(e.path());
            for (const auto& p : files) removedData = removeFile(p) || removedData;
        }
        else if (!m_fresh.empty())
        {
            if (m_keepNew)
            {
                removeEmptySkeleton();
                return;
            }
            // BDMV and CERTIFICATE were not there (forDiscFolder): the job made them and all that is in them
            for (const char* sub : {"BDMV", "CERTIFICATE"})
            {
                std::vector<std::filesystem::path> files;
                for (auto it = std::filesystem::recursive_directory_iterator(m_fresh / sub, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                    if (it->is_regular_file(ec))
                        files.push_back(it->path());
                for (const auto& p : files) removedData = removeFile(p) || removedData;
                std::filesystem::remove_all(m_fresh / sub, ec);  // the folders the job made in them
            }
        }
        else if (!m_freshDemux.empty())
        {
            // the folder was not there (forDemuxFolder): the files in it are the job's
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(m_freshDemux, ec))
                if (e.is_regular_file(ec))
                    files.push_back(e.path());
            for (const auto& p : files) removedData = removeFile(p) || removedData;
        }
        else
            return;
        for (const auto& d : m_createdDirs)
            if (std::filesystem::is_directory(d, ec) && std::filesystem::is_empty(d, ec) && !ec)
                std::filesystem::remove(d, ec);
        if (removedData)
            noteRemovedOutput(m_freshName);
    }

    // A disc folder written in place whose job failed before any of its files had a byte in it: the BDMV and
    // CERTIFICATE folders made for it, and the folder itself when this job made it, go again. A refusal that can come
    // only during the mux, a program map table longer than 1021 bytes, left an empty disc skeleton. Now used only when
    // a source is cut off (keepNewOutput): a disc with data in it is kept then.
    void removeEmptySkeleton() const
    {
        std::error_code ec;
        for (const char* sub : {"BDMV", "CERTIFICATE"})
            for (auto it = std::filesystem::recursive_directory_iterator(m_fresh / sub, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec) && it->file_size(ec) > 0)
                    return;
        for (const char* sub : {"BDMV", "CERTIFICATE"}) std::filesystem::remove_all(m_fresh / sub, ec);
        if (!m_freshFolderExisted && std::filesystem::is_empty(m_fresh, ec) && !ec)
            std::filesystem::remove(m_fresh, ec);
    }

    static void warnRoom(const std::filesystem::path& dir, const int64_t estimate)
    {
        std::error_code ec;
        const auto space = std::filesystem::space(dir, ec);
        if (!ec && estimate > 0 && static_cast<int64_t>(space.available) < estimate)
            LTRACE(LT_WARN, 2,
                   "Warning: the output exists already. It is kept until the new one is complete, so the drive needs "
                   "room for both: about "
                       << space.available / 1000000 << " MB are free, and the new output may need about "
                       << estimate / 1000000 << " MB. If the drive fills up, the job stops and the old output stays.");
    }

    bool m_armed = false;
    bool m_committed = false;
    bool m_swapBegun = false;  // the swap has begun: what the job wrote is not removed any more
    bool m_folder = false;
    std::filesystem::path m_fresh;  // a disc folder written in place (forDiscFolder)
    // a demux folder this job makes (forDemuxFolder)
    std::filesystem::path m_freshDemux;
    // a file this job makes (forFile): an ISO image, a transport stream or a Matroska file, and its split parts
    std::filesystem::path m_freshFile;
    std::filesystem::path m_freshDir;
    std::string m_freshSplitPrefix;
    bool m_freshIsImage = false;
    std::string m_freshName;  // the output as the job names it, for the line that says it was removed
    std::vector<std::filesystem::path> m_createdDirs;  // the folders the output needs that were not there, deepest first
    bool m_keepNew = false;                            // keepNewOutput
    bool m_freshFolderExisted = false;
    bool m_demux = false;
    std::filesystem::path m_dir;
    std::filesystem::path m_tmp;
    std::string m_oldPrefix;
    std::string m_ext;  // the output's extension with its dot, as the user gave it
    std::string m_newPrefix;
    std::string m_imageSideFile;  // "iso.layerbreak.txt" when the file is an ISO image, after either prefix
};

// --inner-only on a direct mux to an ISO image. The image cannot be laid out while the disc is muxed into it: the
// zeros start at the end of the first layer's share of the data, and the size of the data is known only when the mux
// is over (the estimate before it is off both ways). So the disc is muxed into a folder beside the image, and the
// image is then laid out from that folder by the code of the BDMV folder to ISO route, which gives the same image as
// those two steps by hand. The folder goes when the job ends, unless the image was refused for its size: the folder is
// a whole disc then and is kept, so an image can be made from it without muxing again.
struct InnerOnlyFolder
{
    std::filesystem::path path;
    bool keep = false;
    InnerOnlyFolder() = default;
    InnerOnlyFolder(const InnerOnlyFolder&) = delete;
    InnerOnlyFolder& operator=(const InnerOnlyFolder&) = delete;
    ~InnerOnlyFolder()
    {
        std::error_code ec;
        if (!keep && !path.empty())
            std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string str() const
    {
        const std::u8string u = path.u8string();
        return {u.begin(), u.end()};
    }
};

static unsigned long currentProcessId()
{
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Whether this job takes the --inner-only route of a direct mux to an ISO image. Refuses, before anything is made, what
// the option cannot do; says so where it is ignored.
static bool innerOnlyIsoAsked(const MuxerManager& muxerManager, const DiskType dt, const std::string& dstFile)
{
    if (!muxerManager.getInnerOnly())
        return false;
    if (dt == DiskType::AVCHD)
        THROW(ERR_COMMON, "--inner-only lays out a Blu-ray disc image, and this is an AVCHD disc. Leave it out.")
    if (dt != DiskType::BLURAY)
    {
        LTRACE(LT_WARN, 2,
               "Warning: --inner-only applies to a Blu-ray disc image (--blu-ray and an .iso output) and is being "
               "ignored.");
        return false;
    }
    if (strToUpperCase(extractFileExt(dstFile)) != "ISO")
    {
        LTRACE(LT_WARN, 2,
               "Warning: --inner-only applies to an .iso destination and is being ignored, because this mux writes a "
               "folder and only a disc image has sector positions. Author the folder as now, then run it through BDMV "
               "folder to ISO with --inner-only, or name the output .iso.");
        return false;
    }
    const int64_t discBytes = muxerManager.getDiscSizeLimit();
    if (discBytes <= 0)
        THROW(ERR_COMMON,
              "--inner-only needs the disc it is for, to know where its layers end: add --disc-size, such as "
              "--disc-size=bd50, or the Free Sectors of your disc times 2048. Nothing has been written.")
    std::vector<int> breaks = muxerManager.getLayerBreakLbns();
    if (breaks.empty())
    {
        std::string warning, error;
        breaks = defaultLayerBreaks(discBytes, warning, error);
        if (!error.empty())
            THROW(ERR_COMMON, error << " Nothing has been written.")
    }
    if (breaks.empty())
    {
        LTRACE(LT_INFO, 2,
               "--inner-only: a single layer disc has no layer break and no outer edge to pad: its data already starts "
               "at the centre of the disc. The image is muxed as without --inner-only.");
        return false;
    }
    // A guard given beside --inner-only on the meta line is refused when the line is read (parseMuxOpt).
    return true;
}

static void refuseSourceAsOutput(const MuxerManager& muxerManager, const std::string& dstFile)
{
    std::error_code ec;
    const std::filesystem::path out(std::u8string(dstFile.begin(), dstFile.end()));
    // A split writes name.split.n.ext beside the name it was given, so a source named so is written over as well. A
    // job without a split writes no such name, and was refused all the same.
    if (muxerManager.splitAsked())
    {
        const std::u8string stemU = out.stem().u8string();
        const std::u8string extU = out.extension().u8string();
        const std::string prefix = strToLowerCase(std::string(stemU.begin(), stemU.end()) + ".split.");
        const std::string ext = strToLowerCase(std::string(extU.begin(), extU.end()));
        const std::filesystem::path outDir = out.has_parent_path() ? out.parent_path() : std::filesystem::path(".");
        for (const auto& si : muxerManager.getStreamInfo())
            for (const std::string& name : si.m_sourceFiles)
            {
                const std::filesystem::path src(std::u8string(name.begin(), name.end()));
                const std::u8string fn = src.filename().u8string();
                const std::u8string se = src.extension().u8string();
                const std::filesystem::path srcDir =
                    src.has_parent_path() ? src.parent_path() : std::filesystem::path(".");
                if (strStartWith(strToLowerCase(std::string(fn.begin(), fn.end())), prefix) &&
                    strToLowerCase(std::string(se.begin(), se.end())) == ext &&
                    std::filesystem::equivalent(srcDir, outDir, ec) && !ec)
                    throw runtime_error("The source " + name + " has the name of a part of the split output " +
                                        dstFile +
                                        ", and writing that part would destroy it. Choose another output "
                                        "name.");
            }
    }
    if (!std::filesystem::exists(out, ec))
        return;
    const bool outIsFolder = std::filesystem::is_directory(out, ec) && !ec;
    for (const auto& si : muxerManager.getStreamInfo())
    {
        std::vector<std::string> names;
        for (const std::string& part : splitQuotedStr(si.m_fullStreamName.c_str(), '+'))
            names.push_back(unquoteStr(trimStr(part)));
        names.insert(names.end(), si.m_sourceFiles.begin(), si.m_sourceFiles.end());
        for (const std::string& name : names)
        {
            const std::filesystem::path src(std::u8string(name.begin(), name.end()));
            if (std::filesystem::equivalent(src, out, ec) && !ec)
                throw runtime_error("The output " + dstFile + " is one of the source files (" + name +
                                    "), and writing it would destroy that source. Choose another output name.");
            if (outIsFolder)
                for (const char* sub : {"BDMV", "CERTIFICATE"})
                    if (pathIsInside(src, out / sub))
                        throw runtime_error("The output folder " + dstFile + " holds the source " + name + " in its " +
                                            sub +
                                            " folder, and writing the disc there would destroy that source. Choose "
                                            "another output folder.");
        }
    }
}

// ** THE FILES OF SAVE-SRT ARE CHECKED BEFORE ANYTHING IS WRITTEN. ** Each .srt is opened with its first subtitle,
// in the middle of the mux: two tracks naming one .srt failed there, with an empty output and part of the .srt left
// behind, and a .srt named as the output itself did the same. A .srt named as a source of another track would empty
// that source while it is read. The names are compared as the system does, also for files that do not exist yet.
static void refuseSaveSrtClash(const MuxerManager& muxerManager, const std::string& dstFile)
{
    const auto sameName = [](const std::string& a, const std::string& b)
    {
        std::error_code ec;
        const std::filesystem::path pa = std::filesystem::absolute(std::u8string(a.begin(), a.end()), ec);
        if (ec)
            return false;
        const std::filesystem::path pb = std::filesystem::absolute(std::u8string(b.begin(), b.end()), ec);
        if (ec)
            return false;
        if (std::filesystem::equivalent(pa, pb, ec) && !ec)
            return true;
        return pathIsInside(pa, pb) && pathIsInside(pb, pa);
    };
    std::vector<std::string> saved;
    for (const auto& si : muxerManager.getStreamInfo())
    {
        const auto it = si.m_addParams.find("save-srt");
        if (it == si.m_addParams.end() || si.m_addParams.count("keep-text"))
            continue;  // keep-text ignores save-srt
        const std::string name = unquoteStr(it->second);
        if (name.empty())
            continue;
        for (const std::string& other : saved)
            if (sameName(name, other))
                THROW(ERR_COMMON, "Two tracks write their subtitles with save-srt to the same file "
                                      << name << ". Give each track a file of its own.")
        if (sameName(name, dstFile))
            THROW(ERR_COMMON, "save-srt names the output "
                                  << dstFile
                                  << ", and the subtitles would be written over it. Give the .srt another name.")
        for (const auto& src : muxerManager.getStreamInfo())
        {
            std::vector<std::string> names;
            for (const std::string& part : splitQuotedStr(src.m_fullStreamName.c_str(), '+'))
                names.push_back(unquoteStr(trimStr(part)));
            names.insert(names.end(), src.m_sourceFiles.begin(), src.m_sourceFiles.end());
            for (const std::string& srcName : names)
                if (sameName(name, srcName))
                    THROW(ERR_COMMON,
                          "save-srt names the source "
                              << srcName << ", and writing the subtitles would destroy it. Give the .srt another name.")
        }
        saved.push_back(name);
    }
}

static void checkSourceConsumed(MuxerManager& muxerManager)
{
    const int64_t total = muxerManager.totalSize();
    if (total <= 0 || muxerManager.getCutStart() > 0 || muxerManager.getCutEnd() > 0)
        return;
    const int64_t done = muxerManager.getDemuxedSize();
    if (done >= total)
        return;
    const double pct = static_cast<double>(done) / static_cast<double>(total) * 100.0;
    if (pct >= 99.0)
        return;
    // Nothing of the sources at all, as from a .sup of one END segment: "0.0 percent read" said that the job had
    // stopped early, where the sources hold nothing to mux.
    if (done == 0)
        THROW(ERR_COMMON, "No track of the meta file gave anything to mux: its sources hold "
                              << total << " bytes, and none of them is data of its track. Nothing was written.")
    THROW(ERR_COMMON, "The mux stopped before the source was finished: "
                          << doubleToStr(pct, 1) << " percent read, " << done << " bytes of " << total
                          << ". The output is incomplete. This is not a warning: the job did not do "
                             "what it was asked to do.")
}

// An MP4 or Matroska source that was cut off is read as far as it goes, and the job then ends with an error that says
// what is missing, unless --cut-end stops before the first of it.
static void checkSourcesCutOff(const MuxerManager& muxerManager)
{
    const auto check =
        [&muxerManager](const std::string& file, const double from, const unsigned track, const char* what)
    {
        if (muxerManager.getCutEnd() > 0 && static_cast<double>(muxerManager.getCutEnd()) / INTERNAL_PTS_FREQ <= from)
            return;
        THROW(ERR_COMMON, "The source " << file << " is cut off: " << what << ", in track " << track << " from "
                                        << doubleToStr(from, 3)
                                        << " s on. The output holds what the file has and is incomplete.")
    };
    for (const auto& [file, cut] : MovDemuxer::cutOffFiles())
        check(file, cut.from, cut.track, "its index lists data past the end of the file");
    for (const auto& [file, cut] : MatroskaDemuxer::cutOffFiles())
        check(file, cut.from, cut.track, "a block runs past the end of the file");
}

// ** AN AUDIO OR VIDEO TRACK THAT GAVE NOTHING IS A FAILED JOB. **
// It was only a warning after "Mux successful complete" and exit 0, so a meta line that named the wrong codec, an
// AC-3 file as HEVC or an H.264 file as A_MLP, left an output without that track, or an empty one, that a script took
// for a good one. Audio and video are what the job was asked for, so it now ends with an error before success is
// said, and an existing output of the same name is kept. A cut may leave a short track with nothing in its range, and a
// subtitle track may have nothing to show, so those stay a warning (reportEmptyTracks).
static bool isEmptyTrack(const StreamInfo& si)
{
    const AbstractStreamReader* reader = si.m_streamReader;
    return reader != nullptr && reader->getDeliveredFrames() == 0;  // -1 means this reader does not count
}

static std::string emptyTrackText(const StreamInfo& si)
{
    const auto trackParam = si.m_addParams.find("track");
    std::ostringstream who;
    if (trackParam != si.m_addParams.end() && !trackParam->second.empty())
        who << "track " << trackParam->second << " of ";
    who << (si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName);
    const std::string reason = si.m_streamReader ? si.m_streamReader->emptyTrackReason() : std::string();
    if (!reason.empty())
        who << " produced no frames at all. " << reason;
    else
        who << " produced no frames at all. It was read as " << si.m_codec
            << ", and nothing in it was recognised as that. The usual reason is that the codec on the meta line is "
               "not the codec the file holds. List the file on its own to see what tsMuxeR makes of it.";
    return who.str();
}

static bool isAudioOrVideoLine(const StreamInfo& si)
{
    return strStartWith(si.m_codec, "A_") || strStartWith(si.m_codec, "V_");
}

static std::string trackSourceName(const StreamInfo& si)
{
    return si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName;
}

static constexpr const char* WRONG_CODEC_HINT =
    "The usual reason is that the codec on the meta line is not the codec the file holds. List the file on its own to "
    "see what tsMuxeR makes of it. The output is incomplete.";

// ** A DAMAGED FILE OF THE RIGHT CODEC WAS CALLED ANOTHER CODEC. ** Noise over more than half of an AC-3 file is
// refused by the rules below as it should be, but the message gave a wrong codec as the usual reason, although every
// frame found was AC-3 at one rate. Where the frames found are many and keep one rate and channel layout, as a real
// stream does and the frames found by chance in other data do not (see the rule for sync changes), both are said.
static const char* lossHint(const SimplePacketizerReader& audio)
{
    if (audio.getDeliveredFrames() >= 20 && audio.getSyncChanges() == 0)
        return "The frames that were found are all of this codec at one sample rate, so either the file is badly "
               "damaged or the codec on the meta line is not the codec the file holds. List the file on its own to "
               "see what tsMuxeR makes of it. The output is incomplete.";
    return WRONG_CODEC_HINT;
}

static void checkEmptyTracks(const MuxerManager& muxerManager)
{
    const bool cut = muxerManager.getCutStart() > 0 || muxerManager.getCutEnd() > 0;
    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        if (cut)
            break;
        if (isAudioOrVideoLine(si) && isEmptyTrack(si))
            THROW(ERR_COMMON, "The " << emptyTrackText(si) << " The output is incomplete.")
        // The same for audio of which a few frames were found by chance and the rest dropped: AC-3 named A_AAC gave
        // a third of its bytes as "AAC" and a warning. More than half of the data that could not be used is not a
        // damaged track but another codec.
        const auto audio = dynamic_cast<const SimplePacketizerReader*>(si.m_streamReader);
        if (audio == nullptr || !strStartWith(si.m_codec, "A_"))
            continue;
        // The tags are left aside here as in the rule below: getLostSize does not count the bytes of the tags at the
        // end, so a big one no longer stands for lost data, and the bytes read are counted without them too.
        // The padding among the lost bytes is left aside on both sides, as in the rule below (getLostPadBytes).
        const int64_t total = audio->getReadSize();
        const int64_t tags = (std::min)(audio->getTagBytes(), total);
        const int64_t lostPad = (std::min)(audio->getLostPadBytes(), total - tags);
        const int64_t lost = audio->getLostSize() - lostPad;
        const int64_t notTags = total - tags - lostPad;
        if (notTags > 0 && lost > notTags / 2)
            THROW(ERR_COMMON, trackSourceName(si) << ": " << lost << " bytes of the " << notTags
                                                  << " read for this track as " << si.m_codec
                                                  << (tags > 0 && lostPad > 0 ? ", its tags and padding left aside,"
                                                      : tags > 0              ? ", its tags left aside,"
                                                      : lostPad > 0           ? ", its padding left aside,"
                                                                              : "")
                                                  << " could not be used, more than half of it. " << lossHint(*audio))
        // ** WHAT THE LOSS LEAVES OUT. ** It does not count what comes before the first frame and after the last, on
        // purpose (a stream may begin in the middle of a frame and end with a tag), so a WAV file named A_MP3 that
        // gave two frames at its start lost nothing of its 8 MB and passed with 1.7 KB of output. The frames have to
        // carry half of what was read, the tags and the padding left aside (getPadBytes: zeros or 0xFF are no codec).
        // LPCM has a reader of its own and its own loss count.
        const int64_t pad = (std::min)(audio->getPadBytes(), total - tags);
        const int64_t besideTags = total - tags - pad;
        const char* const aside = tags > 0 && pad > 0 ? ", its tags and padding left aside,"
                                  : tags > 0          ? ", its tags left aside,"
                                  : pad > 0           ? ", its padding left aside,"
                                                      : "";
        if (si.m_codec != "A_LPCM" && besideTags > 0 && audio->getFrameBytes() < besideTags / 2)
            THROW(ERR_COMMON, trackSourceName(si)
                                  << ": only " << audio->getFrameBytes() << " of the " << besideTags
                                  << " bytes read for this track as " << si.m_codec << aside
                                  << " are frames of that codec, less than half of it. " << lossHint(*audio))
        // ** FRAMES FOUND ONE BY ONE, EACH WITH ANOTHER RATE. ** The AAC sync word turns up in PCM often enough that
        // two thirds of a WAV file named A_AAC became "frames", every one found after the one before failed, and with
        // another sample rate or channel count nearly every time. A real stream found again after damage keeps them.
        const int64_t frames = audio->getDeliveredFrames();
        if (frames >= 20 && audio->getSyncChanges() * 10 > frames)
            THROW(ERR_COMMON, trackSourceName(si) << ": the sample rate or the channels changed at "
                                                  << audio->getSyncChanges() << " of the " << frames
                                                  << " frames read as " << si.m_codec << "; a real stream keeps them. "
                                                  << WRONG_CODEC_HINT)
    }
    // ** VIDEO OF ANOTHER CODEC. ** A video reader passed a track as soon as one picture was found, and other data
    // has start codes too (DTS, TrueHD and MPEG-2 video have many), some of them followed by what passes for a
    // parameter set or a slice: DTS named as HEVC gave 2,633 "pictures" and "Mux successful". Each reader counts the
    // units it cannot use (MPEGStreamReader::unusableUnit). Real video has at most a few bytes in a thousand of them,
    // also when damaged, as damage seldom makes a start code; other data read as video has from 71 % up.
    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        if (cut)
            break;
        auto* video = dynamic_cast<MPEGStreamReader*>(si.m_streamReader);
        if (video == nullptr || !strStartWith(si.m_codec, "V_"))
            continue;
        const int64_t unusable = video->getUnusableBytes();
        if (si.m_readCnt > 0 && unusable > si.m_readCnt / 2)
            THROW(ERR_COMMON, trackSourceName(si)
                                  << ": " << unusable << " bytes of the " << si.m_readCnt << " read for this track as "
                                  << si.m_codec << " are in units that "
                                  << (dynamic_cast<H264StreamReader*>(video) ? std::string("H.264")
                                                                             : video->getCodecInfo().displayName)
                                  << " does not allow or that cannot be what they say, more than half of it. "
                                  << WRONG_CODEC_HINT)
    }
    // ** AN OUTPUT OF NOTHING IS NOT A SUCCESS. ** A subtitle track may have nothing to show beside other tracks, but a
    // job none of whose tracks read anything, as an empty .sup muxed alone, said "Mux successful complete" and left an
    // output of 0 bytes. After the checks above, which name the track where it is audio or video.
    bool anyData = false;
    for (const StreamInfo& si : muxerManager.getStreamInfo()) anyData = anyData || si.m_readCnt > 0;
    if (!muxerManager.getStreamInfo().empty() && !anyData)
        THROW(ERR_COMMON, "No track of the meta file read any data from its source. Nothing was written.")
}

static void reportEmptyTracks(const MuxerManager& muxerManager)
{
    const bool cut = muxerManager.getCutStart() > 0 || muxerManager.getCutEnd() > 0;
    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        const AbstractStreamReader* reader = si.m_streamReader;
        if (reader == nullptr)
            continue;
        const int64_t frames = reader->getDeliveredFrames();
        if (frames != 0)  // -1 means this reader does not count, so nothing can be said
            continue;

        const auto trackParam = si.m_addParams.find("track");
        std::ostringstream who;
        if (trackParam != si.m_addParams.end() && !trackParam->second.empty())
            who << "track " << trackParam->second << " of ";
        who << (si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName);

        // A subtitle track of a cut may have nothing in the cut's range, which is no fault of its codec: it was given
        // the wrong-codec text all the same
        if (cut && strStartWith(si.m_codec, "S_"))
        {
            LTRACE(LT_WARN, 2,
                   "Warning: " << who.str() << " (" << si.m_codec
                               << ") has no subtitle in the muxed range (--cut-start/--cut-end), so its track in the "
                                  "output is empty. Leave the track out if it is not wanted.");
            continue;
        }
        if (const std::string reason = reader->emptyTrackReason(); !reason.empty())
        {
            LTRACE(LT_WARN, 2, "Warning: " << who.str() << " produced no frames at all. " << reason);
            continue;
        }
        LTRACE(LT_WARN, 2,
               "Warning: " << who.str() << " produced no frames at all. It was read as " << si.m_codec
                           << ", and nothing in it was recognised as that. The usual reason is that the "
                              "codec on the meta line is not the codec the file holds. List the file on its "
                              "own to see what tsMuxeR makes of it.");
    }
}

static void reportLostData(const MuxerManager& muxerManager)
{
    struct Entry
    {
        std::string who;
        std::string codec;
        int64_t lost;
        int64_t total;
        bool isJoin;
    };
    std::vector<Entry> hits;

    for (const StreamInfo& si : muxerManager.getStreamInfo())
    {
        const auto reader = dynamic_cast<SimplePacketizerReader*>(si.m_streamReader);
        if (reader == nullptr)
            continue;
        const int64_t lost = reader->getLostSize();
        // ** WHAT WAS READ, NOT WHAT GOT THROUGH. ** This was getProcessedSize(), which omits
        // anything the reader steps over on purpose, so a trailing tag made the denominator short
        // by exactly its own length: two files of IDENTICAL size with the SAME real loss printed
        // different totals. merge-ac3-track= braids a second stream in through its own path, and
        // getReadSize() counts that half where setBuffer never saw it.
        const int64_t total = reader->getReadSize();
        if (lost <= 0 || total <= 0)
            continue;

        // THE TRACK IS NAMED AS THE USER NAMED IT. The internal identifier is zero for every meta
        // line that carries no track number, which is every elementary stream, so two different
        // tracks both printed "track 0" and the number appeared nowhere else in the log.
        const auto trackParam = si.m_addParams.find("track");
        std::ostringstream who;
        if (trackParam != si.m_addParams.end() && !trackParam->second.empty())
            who << "track " << trackParam->second << " of ";
        // m_fullStreamName is the text as it was typed and ALREADY CARRIES ITS QUOTES. Adding
        // another pair printed a doubled quote around every path.
        const std::string name = si.m_fullStreamName.empty() ? ("\"" + si.m_streamName + "\"") : si.m_fullStreamName;
        who << name;

        // On a join the name is the WHOLE "a"+"b" string, so "list the file on its own" asks the
        // reader to paste that back, which prints a banner, one blank line and exit 0 - the same
        // thing tsMuxeR prints for a path that does not exist. extractFileList is the same splitter
        // the meta parser uses, so "a+b" inside ONE quoted path is not mistaken for a join.
        hits.push_back({who.str(), si.m_codec, lost, total, extractFileList(name).size() > 1});
    }

    if (hits.empty())
        return;

    const bool anyJoin = std::any_of(hits.begin(), hits.end(), [](const Entry& e) { return e.isJoin; });
    const bool allJoin = std::all_of(hits.begin(), hits.end(), [](const Entry& e) { return e.isJoin; });
    // ** THE SEAM CLAUSE NAMED ONLY ONE END OF THE SEAM, AND IT WAS THE WRONG ONE. ** It said "when
    // a joined part STARTS part way through a frame". The case measured is the opposite end: the
    // FIRST part does not END on a frame boundary, so its 975 byte tail cannot be used. A reader of
    // the old sentence checked whether any part began mid frame, found that none did, and was left
    // with two causes they could each disprove in one command.
    const std::string why =
        "That happens when the source is damaged, when a joined part does not begin or end on a "
        "frame boundary so the fragment at the seam cannot be used, or when the codec name on that "
        "line is not the one the track holds. ";
    // ** ONE CLOSING SENTENCE IS PRINTED FOR EVERY TRACK LISTED, SO IT HAS TO BE TRUE OF ALL OF
    // THEM. ** Choosing it on anyJoin told a track that is NOT a join to "List each part on its
    // own", when it has no parts to list. That is the same defect as the one fixed for the single
    // track shape, reappearing in the many track shape, which is why the mixed case is now a
    // fixture rather than an assumption: one ordinary file and one join, both losing data.
    const std::string advice = allJoin   ? "List each part on its own to see the codec name tsMuxeR reports for it."
                               : anyJoin ? "List each file, and each part of a join, on its own to see the codec name "
                                           "tsMuxeR reports for it."
                                         : "List the file on its own to see the codec name tsMuxeR reports for it.";

    if (hits.size() == 1)
    {
        const Entry& e = hits.front();
        std::ostringstream msg;
        msg << "Warning: " << e.who << ": " << e.lost << " bytes of the " << e.total << " read for this track as "
            << e.codec << " could not be used and were dropped. " << why << advice;
        LTRACE(LT_WARN, 2, msg.str());
        return;
    }

    std::ostringstream head;
    head << "Warning: " << hits.size() << " tracks lost data on the way through:";
    LTRACE(LT_WARN, 2, head.str());
    for (const Entry& e : hits)
    {
        std::ostringstream line;
        line << "  " << e.who << ": " << e.lost << " bytes of the " << e.total << " read for this track as " << e.codec
             << " could not be used and were dropped.";
        LTRACE(LT_WARN, 2, line.str());
    }
    LTRACE(LT_WARN, 2, why + advice);
}

// Ctrl+C (SIGINT) and SIGTERM during the mux loop stop the job there (MuxerManager::doMux), so that it ends as a failed
// job does and its unfinished output is removed; outside the loop, and at a second signal, the program ends at once,
// as it always did. The exit code stays that of the signal (endBySignal).
static void onStopSignal(const int sig)
{
    if (!stopWatched() || stopSignal() != 0)
    {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    setStopSignal(sig);
}

static void endBySignal()
{
    if (const int sig = stopSignal(); sig != 0)
    {
        std::cout.flush();  // the signal ends the program without flushing, and a log in a file lost the last lines
        std::cerr.flush();
#ifdef _WIN32
        // Raised again, SIGINT ends the program with code 3 here; Ctrl+C ended it with STATUS_CONTROL_C_EXIT
        std::_Exit(static_cast<int>(0xC000013AU));
#else
        std::signal(sig, SIG_DFL);
        std::raise(sig);
#endif
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    auto argvWide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> argv_utf8;
    argv_utf8.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        argv_utf8.emplace_back(toUtf8(argvWide[i]));
    }
    LocalFree(argvWide);
    std::vector<char*> argv_vec;
    argv_vec.reserve(argv_utf8.size());
    for (auto&& s : argv_utf8)
    {
        argv_vec.push_back(s.data());
    }
    argv = argv_vec.data();
#endif
    LTRACE(LT_INFO, 2, "tsMuxeR version " TSMUXER_VERSION << ". github.com/teaching-droid/tsMuxer");
    // A signal the program was started with ignored, as a job in the background of a script is for Ctrl+C, stays so
    for (const int sig : {SIGINT, SIGTERM})
        if (std::signal(sig, onStopSignal) == SIG_IGN)
            std::signal(sig, SIG_IGN);

    // Which form a language read out of a source is reported in. It has to work when LISTING a
    // file as well as when muxing one, and listing is invoked as "tsMuxeR <file>" with no meta
    // file to carry an option, so this is an argument rather than a MUXOPT entry.
    //
    // It is taken OUT of the list before anything below counts the arguments, because the modes
    // are chosen by how many there are: two means list, three means mux.
    std::string langCodesGiven;  // the value of an earlier --lang-codes: given twice, the last won without a word
    for (int i = 1; i < argc;)
    {
        const std::string arg = argv[i];
        if (arg.compare(0, 13, "--lang-codes=") != 0)
        {
            ++i;
            continue;
        }
        const std::string value = arg.substr(13);
        if (!langCodesGiven.empty())
            LTRACE(LT_WARN, 2,
                   "Warning: --lang-codes is given twice (" << langCodesGiven << " and " << value
                                                            << "). The last one is used; remove the other.");
        langCodesGiven = value;
        if (value == "source")
            setLangCodeStyle(LangCodeStyle::Source);
        else if (value == "bibliographic")
            setLangCodeStyle(LangCodeStyle::Bibliographic);
        else if (value == "terminological")
            setLangCodeStyle(LangCodeStyle::Terminological);
        else
        {
            // This loop runs BEFORE the try block further down, so a THROW here escaped main
            // altogether: the process died with 0xC0000409 and printed nothing but the version
            // banner, while the sentence written for exactly this case was never seen. Report it
            // here instead, the same way the handler at the end of main would have.
            LTRACE(LT_ERROR, 2,
                   "Unknown --lang-codes value \"" << value << "\". It takes source, terminological or bibliographic.");
            return -2;
        }
        for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
        --argc;
    }
    // The same for --playlist-clips: a listing of a playlist gives the lengths and marks of the parts the mux takes,
    // and the window takes the chapters from it, so the listing has to know the choice too. A mux takes it from the
    // MUXOPT line of its meta file.
    bool listPlaylistPlayed = PLAYLIST_CLIPS_PLAYED_BY_DEFAULT;
    for (int i = 1; i < argc;)
    {
        const std::string arg = argv[i];
        if (arg.compare(0, 17, "--playlist-clips=") != 0)
        {
            ++i;
            continue;
        }
        const std::string value = arg.substr(17);
        if (value != "played" && value != "whole")
        {
            LTRACE(LT_ERROR, 2, "Unknown --playlist-clips value \"" << value << "\". It takes played or whole.");
            return -2;
        }
        listPlaylistPlayed = value == "played";
        for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
        --argc;
    }
    int firstMplsOffset = 0;
    int firstM2tsOffset = 0;
    int blankNum = 1900;
    bool insertBlankPL = false;
    // createBluRayDirs("c:/workshop/");

    // MPLSParser parser;
    // parser.parse("d:/hdtv/SHERLOCK_HOLMES/BDMV/PLAYLIST/00100.mpls");

    // CLPIParser parser;
    // parser.parse("h:/BDMV/CLIPINF/00000.clpi");
    // parser.parse("d:/workshop/test_orig_disk2/BDMV/CLIPINF/00003.clpi");

    // uint8_t moBuffer[1024];
    // MovieObject mo;
    // mo.parse("h:/BDMV/MovieObject.bdmv");
    // int moLen = mo.compose(moBuffer, sizeof(moBuffer));
    // file.write(moBuffer, moLen);

    try
    {
        // Route on the mode alone, not on how many arguments came with it. At argc >= 4 a call
        // that left out the output path fell through to the ordinary muxing path, which tried to
        // open "--bdmv-to-iso" as a meta file and printed one character of the resulting exception
        // instead of saying what the mode expects.
        if (argc >= 2 && string(argv[1]) == "--bdmv-to-iso")
            return bdmvFolderToGuardedIso(argc, argv);
        if (argc == 2)
        {
            string str = argv[1];
            // --help, -h and -? show the help a call without arguments shows. They were taken as the name of a file to
            // list, which printed the version line and nothing else.
            // In any case, as -H and --HELP were also taken for a file name; a name of blanks only names no file.
            const string lower = strToLowerCase(str);
            if (lower == "--help" || lower == "-h" || lower == "-?" || lower == "/?")
            {
                showHelp();
                return 0;
            }
            if (trimStr(str).empty())
            {
                showHelp();
                return -1;
            }
            string fileExt = extractFileExt(str);
            fileExt = strToLowerCase(fileExt);
            if (fileExt == "mpls" || fileExt == "mpl")
            {
                bool shortExt = fileExt == "mpl";
                MPLSParser mplsParser;
                mplsParser.parse(argv[1]);
                string streamDir = getBlurayStreamDir(argv[1]);
                std::string mediaExt = shortExt ? ".MTS" : ".m2ts";
                std::string ssifExt = shortExt ? ".SIF" : ".ssif";
                bool mode3D = mplsParser.isDependStreamExist;
                bool switchToSsif = false;
                for (const std::string& clip : mplsParser.m_skippedClips)
                    LTRACE(LT_INFO, 2, "Skipped clip: " << clip << mediaExt);
                mplsParser.reportLeftOut(argv[1], true);

                if (!mplsParser.m_playItems.empty())
                {
                    // The parser has already dropped any clip whose streams contradict the rest, so
                    // this is the first clip that fits the playlist. It may still carry fewer
                    // streams than the playlist declares, and those come from the later clips.
                    MPLSPlayItem& item = mplsParser.m_playItems[0];
                    string itemName = streamDir + item.fileName + mediaExt;
                    if (fileExists(itemName))
                    {
                        if (mode3D && !mplsParser.m_mvcFiles.empty())
                        {
                            string subItemName = streamDir + mplsParser.m_mvcFiles[0] + mediaExt;
                            if (fileExists(subItemName))
                                detectStreamReader(subItemName.c_str(), &mplsParser, true);
                            else
                                switchToSsif = true;
                        }
                    }
                    else
                    {
                        switchToSsif = true;
                    }
                    if (switchToSsif)
                    {
                        string ssifName = streamDir + string("SSIF") + getDirSeparator() + item.fileName + ssifExt;
                        if (fileExists(ssifName))
                            itemName = ssifName;  // if m2ts file absent then swith to ssif
                    }
                    vector<LaterClip> laterClips;
                    const string clipInfoDir = streamDir + ".." + getDirSeparator() + "CLIPINF" + getDirSeparator();
                    for (size_t i = 1; i < mplsParser.m_playItems.size(); ++i)
                    {
                        const string& name = mplsParser.m_playItems[i].fileName;
                        string media = streamDir + name + mediaExt;
                        if (!fileExists(media))
                            media = streamDir + string("SSIF") + getDirSeparator() + name + ssifExt;
                        if (fileExists(media))
                            laterClips.push_back({media, clipInfoDir + name + (shortExt ? ".CPI" : ".clpi"),
                                                  i == mplsParser.chosenStnItem()});
                    }
                    detectStreamReader(itemName.c_str(), &mplsParser, false, &laterClips,
                                       clipInfoDir + item.fileName + (shortExt ? ".CPI" : ".clpi"));
                }
                // The lengths and marks of the play items, as the mux places them (the default of --playlist-clips)
                METADemuxer::cutPlayItems(mplsParser, argv[1], listPlaylistPlayed);

                size_t markIndex = 0;
                int64_t prevFileOffset = 0;
                // A clip of the playlist that is not there was listed with its duration and no word; the mux then
                // stops at it ("Can't open file"), so a window user learned it only there
                std::vector<std::string> missingClips;
                for (size_t i = 0; i < mplsParser.m_playItems.size(); i++)
                {
                    MPLSPlayItem& item = mplsParser.m_playItems[i];

                    string itemName;
                    if (mode3D)
                        itemName = streamDir + string("SSIF") + getDirSeparator() + item.fileName + ".ssif";
                    else
                        // Must not be streamDir.append(...): append MUTATES, and streamDir is
                        // shared across every play item, so each iteration permanently grew it and
                        // the printed name accumulated every previous clip. The GUI parses these
                        // lines into its playlist file list (tsmuxerwindow.cpp), so the wrong names
                        // were user visible. The 3D branch above already builds a temporary.
                        itemName = streamDir + item.fileName + mediaExt;  // 2d mode
                    // The name in the case the disc has, as the mux opens it (clipNameAsOnDisc)
                    if (!mode3D && !fileExists(itemName))
                        itemName =
                            METADemuxer::clipNameAsOnDisc(itemName.substr(0, itemName.size() - mediaExt.size() + 1),
                                                          strToLowerCase(mediaExt.substr(1)));

                    LTRACE(LT_INFO, 2, "");
                    LTRACE(LT_INFO, 2, "File #" << strPadLeft(int64ToStr(i), 5, '0') << " name=" << itemName);
                    // A 3D clip is read from its two .m2ts, as the mux reads it, and from the .ssif only when one of
                    // them is missing; a folder without the SSIF folder is complete
                    string missingName;
                    if (!mode3D)
                    {
                        if (!fileExists(itemName))
                            missingName = itemName;
                    }
                    else
                    {
                        const string ssifName = METADemuxer::clipNameAsOnDisc(
                            streamDir + string("SSIF") + getDirSeparator() + item.fileName + ".",
                            strToLowerCase(ssifExt.substr(1)));
                        if (!fileExists(ssifName))
                        {
                            const string lowerMediaExt = strToLowerCase(mediaExt.substr(1));
                            const string baseName =
                                METADemuxer::clipNameAsOnDisc(streamDir + item.fileName + ".", lowerMediaExt);
                            const string depName = i < mplsParser.m_mvcFiles.size() && !mplsParser.m_mvcFiles[i].empty()
                                                       ? METADemuxer::clipNameAsOnDisc(
                                                             streamDir + mplsParser.m_mvcFiles[i] + ".", lowerMediaExt)
                                                       : string();
                            if (!fileExists(baseName))
                                missingName = baseName;
                            else if (!depName.empty() && !fileExists(depName))
                                missingName = depName;
                        }
                    }
                    if (!missingName.empty() &&
                        std::find(missingClips.begin(), missingClips.end(), missingName) == missingClips.end())
                        missingClips.push_back(missingName);
                    LTRACE(LT_INFO, 2,
                           "Duration: " << floatToTime(
                               (mplsParser.m_playItems[i].OUT_time - mplsParser.m_playItems[i].IN_time) /
                               (double)45000.0));
                    if (mplsParser.isDependStreamExist)
                    {
                        if (mplsParser.mvc_base_view_r)
                        {
                            LTRACE(LT_INFO, 2, "Base view: right-eye");
                        }
                        else
                        {
                            LTRACE(LT_INFO, 2, "Base view: left-eye");
                        }
                    }
                    if (!mplsParser.m_playItems.empty())
                        LTRACE(LT_INFO, 2, "start-time: " << mplsParser.m_playItems[0].IN_time);
                    int marksPerFile = 0;
                    for (; markIndex < mplsParser.m_marks.size(); markIndex++)
                    {
                        PlayListMark& curMark = mplsParser.m_marks[markIndex];
                        if (static_cast<unsigned>(curMark.m_playItemID) > i)
                            break;
                        uint64_t time = curMark.m_markTime - mplsParser.m_playItems[i].IN_time + prevFileOffset;
                        if (marksPerFile % 5 == 0)
                        {
                            if (marksPerFile > 0)
                                LTRACE(LT_INFO, 2, "");
                            LTRACE2(LT_INFO, "Marks: ")
                        }
                        marksPerFile++;
                        LTRACE2(LT_INFO, floatToTime((double)time / 45000.0) << " ")
                    }
                    if (marksPerFile > 0)
                        LTRACE(LT_INFO, 2, "");
                    prevFileOffset += mplsParser.m_playItems[i].OUT_time - mplsParser.m_playItems[i].IN_time;
                }
                if (!missingClips.empty())
                {
                    LTRACE(LT_INFO, 2, "");
                    for (const std::string& clip : missingClips)
                        LTRACE(LT_WARN, 2,
                               "Warning: the clip " << clip
                                                    << " of this playlist does not exist, so a mux of the playlist "
                                                       "cannot read it. Copy the whole disc folder, or choose a "
                                                       "playlist whose clips are all there.");
                }
            }
            else
            {
                // A file that cannot be opened, missing or not readable, printed the version line only, at exit 0: a
                // script read "no tracks". It is said why, as the mux says it, and the exit is an error.
                File file;
                std::error_code ec;
                if (const std::string hint = discSourceHint(str); !hint.empty())
                    THROW(ERR_FILE_NOT_FOUND, "Can't list " << str << ": " << hint << ".")
                if (std::filesystem::is_directory(fsPath(str), ec) || !file.open(argv[1], File::ofRead))
                    THROW(ERR_FILE_NOT_FOUND, "Can't open file: " << str << openFailureReason(str))
                file.close();
                detectStreamReader(argv[1], nullptr, false);
            }
            cout << endl;
            return 0;
        }
        if (argc != 3)
        {
            /*
                        LTRACE(LT_INFO, 2, "Usage: ");
                        LTRACE(LT_INFO, 2, "For start muxing: " << "tsMuxeR <meta file name> <out file/dir name>");
                        LTRACE(LT_INFO, 2, "For detect stream params: " << "tsMuxeR <media file name>");
                        LTRACE(LT_INFO, 2, "For more information about meta file see readme.txt");
                        cout << endl;
            */
            showHelp();
            return -1;
        }
        // Check the destination before anything reads it. It used to be checked further down,
        // after several things had already taken it apart.
        if (trimStr(unquoteStr(argv[2])).empty())
        {
            LTRACE(LT_ERROR, 2, "No output file or folder was given. It is the second argument.");
            return -1;
        }
        string fileExt = extractFileExt(argv[2]);
        fileExt = strToUpperCase(fileExt);
        auto startTime = std::chrono::steady_clock::now();

        int autoChapterLen = 0;
        vector<double> customChapterList;
        vector<AVChapter> chapterInfo;
        bool stereoMode = false;
        string isoDiskLabel;
        bool demuxAsked = false;
        refuseUnreadableMeta(argv[1]);
        DiskType dt = checkBluRayMux(argv[1], autoChapterLen, customChapterList, chapterInfo, firstMplsOffset,
                                     firstM2tsOffset, insertBlankPL, blankNum, stereoMode, isoDiskLabel, demuxAsked);
        // --demux asks for a folder of tracks whatever the output is named, as --help says: the name decided, so
        // --demux with a name ending .ts, .m2ts, .mts, .m2t, .mkv or .iso wrote that file instead. A disc
        // and a demux in one job cannot both be made.
        if (demuxAsked && dt != DiskType::NONE)
            THROW(ERR_COMMON, "The MUXOPT lines ask for both a demux and a disc. Give one of --demux and "
                                  << (dt == DiskType::AVCHD ? "--avchd" : "--blu-ray") << ".")
        std::string fileExt2 = unquoteStr(fileExt);
        // .mks (subtitles only) and .mk3d (stereoscopic) are Matroska files too: they went to a demux folder
        bool mkvMode = !demuxAsked && (fileExt2 == "MKV" || fileExt2 == "MKA" || fileExt2 == "MKS" ||
                                       fileExt2 == "MK3D" || fileExt2 == "WEBM");
        // .mts and .m2t are muxed as the muxer has always read them (setMuxFormat); they went to a demux folder, and
        // the job said it had finished
        bool muxMode =
            !demuxAsked && (fileExt2 == "M2TS" || fileExt2 == "MTS" || fileExt2 == "M2T" || fileExt2 == "TS" ||
                            fileExt2 == "SSIF" || fileExt2 == "ISO" || dt != DiskType::NONE);
        // An output named as a kind of file tsMuxeR does not write became a folder of demuxed tracks without a word:
        // movie.mp4 was a folder named movie.mp4, and the job said "Demux complete."
        if (!demuxAsked && !mkvMode && !muxMode)
            for (const char* kind :
                 {"MP4", "MOV", "M4V", "M4A", "MPG", "MPEG", "VOB", "AVI", "FLV", "OGG", "OGM", "WMV"})
                if (fileExt2 == kind)
                    THROW(ERR_COMMON, "tsMuxeR does not write ." << strToLowerCase(fileExt2)
                                                                 << " files. Name the output .ts, .m2ts, .mkv or .iso, "
                                                                    "or add --demux to the MUXOPT line for a folder of "
                                                                    "tracks.")

        if (mkvMode)
        {
            KeepOldOutput keepOld;  // made first, so it is ended last: the muxers have closed their files by then
            MuxerManager muxerManager(readManager, matroskaMuxerFactory);
            muxerManager.setMatroskaOutput(fileExt2 == "WEBM" ? 2 : 1);
            muxerManager.openMetaFile(argv[1]);

            string dstFile = unquoteStr(argv[2]);
            if (!isValidFileName(dstFile))
                throw runtime_error(string("The output file name is empty or contains characters that "
                                           "cannot be used: \"") +
                                    dstFile + "\"");
            refuseSourceAsOutput(muxerManager, dstFile);
            refuseSaveSrtClash(muxerManager, dstFile);

            if (muxerManager.getTrackCnt() == 0)
                THROW(ERR_COMMON, "No tracks selected")
            reportOptionsIgnored(muxerManager, autoChapterLen, false, false, false, fileExt2);
            // Same list the Blu-ray path hands to createMPLSFile. checkBluRayMux() has already
            // parsed --custom-chapters out of the meta above, whatever the output format is.
            muxerManager.moveChaptersByCut(customChapterList, chapterInfo);
            if (!customChapterList.empty())
                muxerManager.setChapters(customChapterList);
            if (!chapterInfo.empty())
                muxerManager.setChapterInfo(chapterInfo);
            muxerManager.setFinishingNote(
                "Finishing the output: writing the index, closing the file and checking the tracks...");
            muxerManager.doMux(keepOld.forFile(dstFile, muxerManager.totalSize()), nullptr);

            checkSourceConsumed(muxerManager);
            try
            {
                checkSourcesCutOff(muxerManager);
            }
            catch (...)
            {
                keepOld.keepNewOutput();  // the error says the output holds what the file has
                throw;
            }
            checkEmptyTracks(muxerManager);
            keepOld.commit();
            setSaveSrtJobSucceeded();
            reportJobComplete();
            LTRACE(LT_INFO, 2, "Mux successful complete");
            reportSplitWithoutParameterSets(muxerManager);
            reportLostData(muxerManager);
            reportEmptyTracks(muxerManager);
        }
        else if (muxMode)
        {
            KeepOldOutput keepOld;  // made first, so it is ended last: the image and the muxers are closed by then
            InnerOnlyFolder innerFolder;  // --inner-only on an ISO: the disc folder the image is made from
            BlurayHelper blurayHelper;

            MuxerManager muxerManager(readManager, tsMuxerFactory);
            muxerManager.setAllowStereoMux(fileExt2 == "SSIF" || dt != DiskType::NONE);
            muxerManager.openMetaFile(argv[1]);
            // The Matroska branch above hands this list over because Matroska WRITES chapters. A
            // transport stream has nowhere to put them, so this branch never did, and the disc
            // path passes the same list separately to createMPLSFile.
            //
            // --split-chapters needs them here as well, because splitting happens in the muxer.
            // Handing them over changes nothing on its own: TSMuxer keeps the list and only looks
            // at it when that option is on. A source that carries its own chapters, a playlist or
            // a Matroska, is already covered by the fallback in preinitMux. A cut moves the --custom-chapters list,
            // with the times of --chapters-file, as it moves the source's own chapters, here for the split and for the
            // disc's playlist marks alike. A disc's marks come only from this list or --auto-chapters, not from the
            // source; the window writes the source's marks into --custom-chapters itself.
            muxerManager.moveChaptersByCut(customChapterList, chapterInfo);
            if (!customChapterList.empty())
                muxerManager.setChapters(customChapterList);
            if (dt != DiskType::NONE)
                refuseTooManyMarks(muxerManager, customChapterList);
            reportDvProfileIgnored(muxerManager, dt != DiskType::NONE ? "a disc" : "a transport stream");
            reportOptionsIgnored(muxerManager, autoChapterLen, dt != DiskType::NONE, false, false, std::string());
            if (!isV3() && dt == DiskType::BLURAY && muxerManager.getHevcFound())
            {
                LTRACE(LT_INFO, 2, "HEVC stream detected: changing Blu-Ray version to V3.");
                V3_flags |= HDMV_V3;
            }
            // A UHD disc (V3) has no 3D, so a 3D track written onto one gave a disc that players show in 2D,
            // with nothing said. The window ticks V3 from the last video added, so it is easily left on. The
            // 3D track wins: the disc is written as a Blu-ray 3D disc. With HEVC in the mux the disc has to
            // stay UHD, so that case is only reported.
            if (isV3() && dt == DiskType::BLURAY && muxerManager.getMvcFound())
            {
                if (!muxerManager.getHevcFound())
                {
                    LTRACE(LT_WARN, 2,
                           "Warning: --blu-ray-v3 is left out: the mux has 3D (MVC) video, and a UHD disc cannot "
                           "carry 3D, so the disc is written as a Blu-ray 3D disc.");
                    V3_flags &= ~HDMV_V3;
                }
                else
                    LTRACE(LT_WARN, 2,
                           "Warning: the mux has 3D (MVC) video and HEVC video. HEVC needs a UHD disc, and a UHD "
                           "disc cannot carry 3D, so players show this disc in 2D.");
            }

            // output path - is checked for invalid characters on our platform
            string dstFile = unquoteStr(argv[2]);

            if (!isValidFileName(dstFile))
                throw runtime_error(string("The output file name is empty or contains characters that "
                                           "cannot be used: \"") +
                                    dstFile + "\"");

            refuseSourceAsOutput(muxerManager, dstFile);
            refuseSaveSrtClash(muxerManager, dstFile);
            const string outputRoot = dstFile;  // the image or the disc folder, before dstFile names its first clip

            // ** BEFORE THE DESTINATION IS CREATED, NOT AFTER. ** This stood below the block that
            // creates the output image, so a meta that selects no tracks replaced an existing .iso
            // with a stub and then refused. The capacity guard just below already had this right.
            if (muxerManager.getTrackCnt() == 0)
                THROW(ERR_COMMON, "No tracks selected")
            // Refused when the muxer was set up, which an ISO image of the same name did not survive
            if (muxerManager.keepEditions())
                THROW(ERR_COMMON, "--keep-editions applies to a Matroska output (.mkv) only.")
            const bool innerIso = innerOnlyIsoAsked(muxerManager, dt, dstFile);

            if (dt != DiskType::NONE)
            {
                // Fit-to-disc guard (--disc-size): abort BEFORE the (multi-hour) mux if the image
                // won't fit the target disc, unless --allow-oversize downgrades it to a warning.
                const int64_t discLimit = muxerManager.getDiscSizeLimit();
                if (discLimit > 0)
                {
                    // muxedSourceSize() is the sum of source elementary-stream sizes the mux takes (a cut, the
                    // tracks of a transport stream that are taken); the muxed BD M2TS adds
                    // TS(188/184)+M2TS(192/188) framing, PSI/PCR/padding and UDF/BDMV overhead, so the
                    // real image runs ~5% larger. Compare that inflated estimate against the capacity.
                    // A constant bitrate pads with null packets up to its rate, and then the image is the rate
                    // times the length, however little the sources hold (paddedOutputSize). Not counted, a
                    // constant bitrate image half as big again as the disc was reported as fitting before the mux.
                    const int64_t estSize =
                        (std::max)(muxerManager.muxedSourceSize() / 100 * 105, muxerManager.paddedOutputSize());
                    if (muxerManager.paddedLengthUnknown())
                        LTRACE(LT_INFO, 2,
                               "Capacity guard: the sources do not state their length, so the null packets of the "
                               "constant bitrate are not in the estimate; the finished disc is measured after the "
                               "mux.");
                    const double estGb = estSize / 1e9;
                    const double capGb = discLimit / 1e9;
                    if (estSize > discLimit)
                    {
                        if (muxerManager.getAllowOversize())
                            LTRACE(LT_WARN, 2,
                                   "Capacity guard: estimated image ~"
                                       << estGb << " GB exceeds the " << capGb
                                       << " GB target disc; continuing due to --allow-oversize (it will NOT fit "
                                          "that disc).");
                        else
                            THROW(ERR_COMMON,
                                  "Capacity guard: estimated image ~"
                                      << estGb << " GB exceeds the " << capGb
                                      << " GB target disc. Lower the bitrate/duration, choose a larger --disc-size, "
                                         "or pass --allow-oversize to build it anyway. Aborted before muxing.");
                    }
                    else
                        LTRACE(LT_INFO, 2,
                               "Capacity guard: estimated image ~" << estGb << " GB fits the " << capGb
                                                                   << " GB target disc.");
                }
                muxerManager.checkTracksBeforeOutput();  // before the folders are made, as for a demux
                if (innerIso)
                {
                    // Beside the image, named like the temporary outputs of KeepOldOutput. A folder of that name left
                    // by an earlier job of a process with the same number is not anybody's disc.
                    const std::filesystem::path out = fsPath(dstFile);
                    const std::u8string stem = out.stem().u8string();
                    const std::string name =
                        std::string(stem.begin(), stem.end()) + ".tsmuxer-inner-" + std::to_string(currentProcessId());
                    innerFolder.path = (out.has_parent_path() ? out.parent_path() : std::filesystem::path(".")) /
                                       std::filesystem::path(std::u8string(name.begin(), name.end()));
                    std::error_code ec;
                    std::filesystem::remove_all(innerFolder.path, ec);
                    const auto space = std::filesystem::space(
                        out.has_parent_path() ? out.parent_path() : std::filesystem::path("."), ec);
                    if (!ec && static_cast<int64_t>(space.available) < 2 * muxerManager.totalSize())
                        LTRACE(
                            LT_WARN, 2,
                            "Warning: --inner-only writes the disc into a folder beside the image first, so the drive "
                            "needs room for both: about "
                                << space.available / 1000000 << " MB are free, and the two may need about "
                                << 2 * muxerManager.totalSize() / 1000000 << " MB.");
                    LTRACE(LT_INFO, 2,
                           "--inner-only, step 1 of 2: the disc is muxed into the folder "
                               << innerFolder.str() << "; step 2 lays out the image from it.");
                    dstFile = innerFolder.str();
                }
                else
                    dstFile = strToUpperCase(extractFileExt(dstFile)) == "ISO"
                                  ? keepOld.forFile(dstFile, muxerManager.totalSize())
                                  : keepOld.forDiscFolder(dstFile, muxerManager.totalSize());
                // The guard of an --inner-only image is laid out in step 2, and the folder has no sectors for it.
                // A Blu-ray image gets a guard of 288 MB after each break and 18 MB before it when none is given: a
                // burnt dual-layer disc often reads badly at the layer change, and a player then stalls there. It
                // is only written when the data reaches the break, so a single layer image is the same as before.
                bool defaultGuard = !innerIso && dt == DiskType::BLURAY &&
                                    strToUpperCase(extractFileExt(outputRoot)) == "ISO" &&
                                    !muxerManager.getNoLayerBreakGuard() && muxerManager.getLayerBreakGuardMB() < 0 &&
                                    muxerManager.getLayerBreakGuardBeforeMB() < 0;
                // An image that would not fit the disc with that guard is muxed without it, with a warning, as
                // --bdmv-to-iso writes it. The size is not known while the guard is written, so it is decided here,
                // from the estimate of the capacity check above, the zones of each break it reaches, the UDF reserve
                // and one copy buffer. The disc is the one --disc-size names, else the one the breaks belong to.
                if (defaultGuard)
                {
                    std::string warning, error;
                    const std::vector<int> breaks =
                        !muxerManager.getLayerBreakLbns().empty()
                            ? muxerManager.getLayerBreakLbns()
                            : defaultLayerBreaks(muxerManager.getDiscSizeLimit(), warning, error);
                    if (error.empty() && !breaks.empty())
                    {
                        constexpr int64_t afterSectors =
                            static_cast<int64_t>(DEFAULT_GUARD_AFTER_MB) * 1024 * 1024 / 2048;
                        constexpr int64_t beforeSectors =
                            (std::min)(afterSectors, (std::max<int64_t>)(4 * 1024 * 1024 / 2048, afterSectors / 16));
                        constexpr int64_t reserveSectors = 8192 + 8192;
                        const int64_t estSize =
                            (std::max)(muxerManager.muxedSourceSize() / 100 * 105, muxerManager.paddedOutputSize());
                        const int64_t dataSectors = (estSize + 2047) / 2048 + reserveSectors;
                        const int64_t capacity = muxerManager.getDiscSizeLimit() > 0
                                                     ? muxerManager.getDiscSizeLimit() / 2048
                                                     : static_cast<int64_t>(breaks.back()) + breaks.front();
                        int64_t guardSectors = 0;
                        for (const int bp : breaks)
                            if (dataSectors > static_cast<int64_t>(bp) - beforeSectors)
                                guardSectors += beforeSectors + afterSectors;
                        if (guardSectors > 0 && dataSectors + guardSectors > capacity)
                        {
                            LTRACE(LT_WARN, 2,
                                   "Warning: the image does not fit the disc with the layer break guard, so it is "
                                   "written without it. A burnt dual-layer disc may then read badly at the layer "
                                   "change.");
                            defaultGuard = false;
                        }
                    }
                }
                if (!blurayHelper.open(dstFile, dt, muxerManager.totalSize(), muxerManager.getExtraISOBlocks(),
                                       muxerManager.useReproducibleIsoHeader(),
                                       innerIso       ? -1
                                       : defaultGuard ? DEFAULT_GUARD_AFTER_MB
                                                      : muxerManager.getLayerBreakGuardMB(),
                                       innerIso ? std::vector<int>() : muxerManager.getLayerBreakLbns(),
                                       innerIso || defaultGuard ? -1 : muxerManager.getLayerBreakGuardBeforeMB(),
                                       muxerManager.getDiscSizeLimit(), defaultGuard))
                    throw runtime_error(string("Can't create output file ") + dstFile);
                blurayHelper.setVolumeLabel(isoDiskLabel);
                blurayHelper.createBluRayDirs();
                dstFile = blurayHelper.m2tsFileName(firstM2tsOffset);
            }
            else
                dstFile = keepOld.forFile(dstFile, muxerManager.totalSize());
            if (dt != DiskType::NONE)
                muxerManager.setFinishingNote(
                    std::string("Finishing the output: closing the clips, writing the clip information and the "
                                "playlists") +
                    (!innerIso && strToUpperCase(extractFileExt(outputRoot)) == "ISO" ? ", finishing the image" : "") +
                    " and checking the tracks...");
            else
                muxerManager.setFinishingNote(std::string("Finishing the output: closing the ") +
                                              (muxerManager.splitAsked() ? "files" : "file") +
                                              " and checking the tracks...");
            muxerManager.doMux(dstFile, dt != DiskType::NONE ? &blurayHelper : nullptr);
            if (dt != DiskType::NONE)
            {
                blurayHelper.writeBluRayFiles(muxerManager, insertBlankPL, firstMplsOffset, blankNum, stereoMode);
                auto mainMuxer = dynamic_cast<TSMuxer*>(muxerManager.getMainMuxer());
                auto subMuxer = dynamic_cast<TSMuxer*>(muxerManager.getSubMuxer());

                if (mainMuxer)
                    blurayHelper.createCLPIFile(mainMuxer, mainMuxer->getFirstFileNum(), true);
                if (subMuxer)
                {
                    blurayHelper.createCLPIFile(subMuxer, subMuxer->getFirstFileNum(), false);

                    IsoWriter* IsoWriter = blurayHelper.isoWriter();
                    if (IsoWriter)
                    {
                        for (size_t i = 0; i < mainMuxer->splitFileCnt(); ++i)
                        {
                            string file1 = mainMuxer->getFileNameByIdx(i);
                            string file2 = subMuxer->getFileNameByIdx(i);
                            int ssifNum = strToInt32(extractFileName(file1));
                            // The result is checked because it can now mean something. It used to be
                            // guarded by assertions, which a release build removes, so there was
                            // nothing to check; they refuse and say why instead. Carrying on past a
                            // refusal would finish with "Mux successful complete" and leave a
                            // zero-length .ssif in the image, which is a 3D disc that cannot play.
                            if (!file1.empty() && !file2.empty() &&
                                !IsoWriter->createInterleavedFile(file1, file2, blurayHelper.ssifFileName(ssifNum)))
                                throw runtime_error(string("Can't build the interleaved 3D file ") +
                                                    blurayHelper.ssifFileName(ssifNum) +
                                                    ". The reason is above; the disc would not play in 3D.");
                        }
                    }
                }

                if (subMuxer)
                    mainMuxer->alignPTS(subMuxer);

                blurayHelper.createMPLSFile(mainMuxer, subMuxer, autoChapterLen, customChapterList, dt, firstMplsOffset,
                                            muxerManager.isMvcBaseViewR());

                if (insertBlankPL && mainMuxer && !subMuxer)
                {
                    blankPlaylistClash(*mainMuxer, blankNum);
                    LTRACE(LT_INFO, 2, "Adding blank play list");
                    muxBlankPL(extractFileDir(argv[0]), blurayHelper, mainMuxer->getPidList(), dt, blankNum);
                }
            }
            // An image is finished here, before success is reported: finishing can still refuse, when its metadata
            // area is full, and it used to happen in the destructor after "Mux successful complete", which ended the
            // program without a message.
            blurayHelper.close();
            checkSourceConsumed(muxerManager);
            try
            {
                checkSourcesCutOff(muxerManager);
            }
            catch (...)
            {
                keepOld.keepNewOutput();  // the error says the output holds what the file has
                throw;
            }
            checkEmptyTracks(muxerManager);
            if (innerIso)
            {
                // Step 2, with what the MUXOPT line gave: the disc's sectors and breaks, the label, the header, the
                // extra room. The video of a 3D disc is stored once, as a direct mux to an ISO has always stored it,
                // and the files are written in their disc order, as a direct mux writes them: the navigation files
                // first, at the start of layer 0, then the clips in the order they play.
                GuardedIsoOptions iso;
                iso.srcRoot = innerFolder.str();
                iso.outIso = keepOld.forFile(outputRoot, muxerManager.totalSize());
                iso.layerBreakLbns = muxerManager.getLayerBreakLbns();
                iso.originalOrder = true;
                iso.discCapacitySectors = muxerManager.getDiscSizeLimit() / 2048;
                iso.singleCopy3d = true;
                iso.innerOnly = true;
                iso.allowOversize = muxerManager.getAllowOversize();
                iso.reproducibleHeader = muxerManager.useReproducibleIsoHeader();
                iso.extraIsoSpace = muxerManager.getExtraIsoSpaceGiven();
                iso.discLabel = isoDiskLabel;
                iso.keepTooBigImage = false;  // the folder is kept instead, see below
                iso.partOfMux = true;
                iso.tempName = keepOld.armed();
                LTRACE(LT_INFO, 2, "--inner-only, step 2 of 2: the image is laid out from the folder.");
                bool sizeRefused = false;
                if (buildGuardedIso(iso, &sizeRefused) != 0)
                {
                    if (sizeRefused)
                    {
                        innerFolder.keep = true;
                        THROW(ERR_COMMON,
                              "The image is not kept, as it does not fit the disc. The disc it was made from "
                              "is kept as an ordinary Blu-ray folder in "
                                  << innerFolder.str()
                                  << ": an image can be made from it with BDMV folder to ISO "
                                     "(--bdmv-to-iso), for a larger disc say, without muxing again.")
                    }
                    // the folder goes with the job, so it is not named
                    THROW(ERR_COMMON, "The image could not be laid out from the muxed disc; the reason is above.")
                }
            }
            keepOld.commit();
            setSaveSrtJobSucceeded();
            if (dt != DiskType::NONE)
                reportFinishedDiscSize(outputRoot, muxerManager.getDiscSizeLimit(), muxerManager.getExtraISOBlocks());
            if (dt != DiskType::NONE && strToUpperCase(extractFileExt(outputRoot)) == "ISO" &&
                muxerManager.getDiscSizeLimit() <= 0 && muxerManager.getLayerBreakLbns().empty())
                reportBd50Layout(outputRoot, "--disc-size=bd100", "--disc-size=66735570944");
            reportJobComplete();
            LTRACE(LT_INFO, 2, "Mux successful complete");
            reportSplitWithoutParameterSets(muxerManager);
            reportLostData(muxerManager);
            reportEmptyTracks(muxerManager);
            if (dt == DiskType::BLURAY)
                reportSingleLayerDvOnDisc(muxerManager);
            if (dt != DiskType::NONE)
                reportHlgOnDisc(muxerManager);
        }
        else
        {
            KeepOldOutput keepOld;  // made first, so it is ended last: the demux has closed its files by then
            MuxerManager sMuxer(readManager, singleFileMuxerFactory);
            sMuxer.openMetaFile(argv[1]);
            reportDvProfileIgnored(sMuxer, "a demux");
            reportOptionsIgnored(sMuxer, autoChapterLen, false, true,
                                 !customChapterList.empty() || !chapterInfo.empty(), std::string());
            if (sMuxer.getTrackCnt() == 0)
                THROW(ERR_COMMON, "No tracks selected")

            // output path - is checked for invalid characters on our platform
            string dstFile = unquoteStr(argv[2]);

            if (!isValidFileName(dstFile))
                throw runtime_error(string("The output file name is empty or contains characters that "
                                           "cannot be used: \"") +
                                    dstFile + "\"");

            sMuxer.checkTracksBeforeOutput();  // before the folder is made, so a refusal leaves nothing behind
            refuseSaveSrtClash(sMuxer, dstFile);
            // Refused when the muxer was set up, after the demux folder had been made, which was left empty
            if (sMuxer.keepEditions())
                THROW(ERR_COMMON, "--keep-editions applies to a Matroska output (.mkv) only.")
            sMuxer.setDemuxFolder(dstFile);
            const std::string demuxDir = keepOld.forDemuxFolder(dstFile, sMuxer.totalSize());
            createDir(demuxDir, true);
            sMuxer.setFinishingNote("Finishing the demux: closing the track files and checking the tracks...");
            sMuxer.doMux(demuxDir, nullptr);
            checkEmptyTracks(sMuxer);
            keepOld.commit();
            setSaveSrtJobSucceeded();
            reportJobComplete();
            LTRACE(LT_INFO, 2, "Demux complete.");
            reportLostData(sMuxer);
            reportEmptyTracks(sMuxer);
        }
        auto endTime = std::chrono::steady_clock::now();
        auto totalTime = endTime - startTime;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(totalTime);
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(totalTime);
        if (muxMode || mkvMode)
        {
            LTRACE2(LT_INFO, "Muxing time: ")
        }
        else
            LTRACE2(LT_INFO, "Demuxing time: ")
        if (minutes.count() > 0)
        {
            LTRACE2(LT_INFO, minutes.count() << " min ")
            seconds -= minutes;
        }
        LTRACE(LT_INFO, 2, seconds.count() << " sec");

        return 0;
    }
    catch (runtime_error& e)
    {
        if (argc == 2)
            LTRACE2(LT_ERROR, "Error: ")
        LTRACE2(LT_ERROR, e.what())
        printRemovedOutputNotes(true);
        return -1;
    }
    catch (VodCoreException& e)
    {
        if (argc == 2)
            LTRACE2(LT_ERROR, "Error: ")
        LTRACE(LT_ERROR, 2, e.m_errStr.c_str());
        printRemovedOutputNotes();
        endBySignal();
        return -2;
    }
    catch (BitStreamException& e)
    {
        if (argc == 2)
            LTRACE2(LT_ERROR, "Error: ")
        LTRACE(LT_ERROR, 2, "Bitstream exception " << e.what() << EXCEPTION_ERR_MSG);
        printRemovedOutputNotes();
        return -3;
    }
    catch (...)
    {
        if (argc == 2)
            LTRACE2(LT_ERROR, "Error: ")
        LTRACE(LT_ERROR, 2, "Unknown exception" << EXCEPTION_ERR_MSG);
        printRemovedOutputNotes();
        return -4;
    }
}
