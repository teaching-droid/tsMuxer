// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef SAMPLE_CUTTER_H_
#define SAMPLE_CUTTER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ** A SAMPLE OF A SOURCE FOR A PROBLEM REPORT (--make-sample). ** A problem with a source can mostly be looked into
// only with the source, which is too big to attach to an issue: a file there may be 25 MB at most. A sample is a part
// of the source, its first or last megabytes or those around a time, copied byte for byte so that whatever the source
// does wrong the sample does too. Nothing is decoded or made anew: the sample begins where a reader can begin, a PAT of
// a transport stream, a cluster of a Matroska file, a pack with a sequence header of a program stream, and it keeps
// what a reader needs before that, the headers of a Matroska or Ogg file or a WAV file's format.
namespace sample
{
enum class Take
{
    Start,
    End,
    At
};

struct Request
{
    std::string source;
    Take take = Take::Start;
    double atSeconds = 0;         // for Take::At: the time of the source the sample is taken around
    int64_t maxBytes = 20000000;  // the sample is at most this long
    bool keepNames = false;       // the title and the track and chapter names of a Matroska file are kept, not blanked
};

struct Result
{
    std::string kind;                     // what the source was read as: "MPEG-TS", "Matroska", ...
    std::string extension;                // of the sample, from the source's name: ".ts"
    std::vector<std::string> lines;       // how and where it was cut, for the report
    std::vector<std::string> extraFiles;  // files beside the source that belong to it: the IFO file of a VOB
};

// Writes the sample of r.source to target. Throws std::runtime_error with the reason when it cannot be made so.
Result cut(const Request& r, const std::string& target);

// A zip archive of stored files, each given by its name in the archive and the file it is read from. Stored, as video
// and sound hardly compress; written here, as nothing else in tsMuxeR writes a zip.
void writeZip(const std::string& zipName, const std::vector<std::pair<std::string, std::string>>& entries);
}  // namespace sample

#endif
