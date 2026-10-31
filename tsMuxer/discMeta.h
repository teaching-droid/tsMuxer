#ifndef DISC_META_H_
#define DISC_META_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The disc library of a Blu-ray disc, BDMV/META/DL: the disc's title in one bdmt_<lang>.xml per language, and the
// pictures those files name. A player that shows a disc by its name and picture, as a PS3 does, reads it; without it
// such a player shows a blank disc named BDMV. A mux of a disc wrote an empty META folder, so the title and the
// pictures of the source disc were lost (#30).
struct DiscMeta
{
    std::string folder;                                       // where the files are read from
    std::vector<std::string> files;                           // the files that go into BDMV/META/DL, by name
    std::vector<std::pair<std::string, std::string>> titles;  // the language and the title of each bdmt file

    [[nodiscard]] bool empty() const { return files.empty(); }
};

// A folder given with --meta-dir: the folder itself, or the DL, META/DL or BDMV/META/DL folder in it, whichever holds
// the bdmt files. Throws when none of them does. What a player may not read in the files is said as a warning.
DiscMeta readGivenDiscMeta(const std::string& dir);

// The disc library of the disc a source file is on, found from a clip in BDMV/STREAM or BDMV/STREAM/SSIF. Empty when
// the file is not on a disc, or its disc has none.
DiscMeta discMetaOfSource(const std::string& sourceFile);

// Where the files come from and the titles they give, for the log.
std::string describeDiscMeta(const DiscMeta& meta);

// The bytes of one file of the library. Throws when it cannot be read.
std::vector<uint8_t> readDiscMetaFile(const DiscMeta& meta, const std::string& name);

#endif  // DISC_META_H_
