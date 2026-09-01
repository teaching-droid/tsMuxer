#ifndef BD_SSIF_IMPORT_H_
#define BD_SSIF_IMPORT_H_

#include <cstdint>
#include <string>
#include <vector>

// Reading a pressed 3D Blu-ray's own description of how its two video views are laid out.
//
// A 3D disc stores its video ONCE and gives it three names. The base view and the dependent view are
// cut into chunks and written alternately, dependent first, and that single run of sectors is what
// the .ssif names. The same sectors are then addressed again as two .m2ts: one naming only the base
// chunks, one naming only the dependent chunks, so a 2D player opens the base .m2ts and never sees
// the rest. Three file names, one copy of the data.
//
// Copying those three files by name writes the video twice, which is what BDMV to ISO does today.
// To write it once, two things have to be known and BOTH are stated on the disc:
//
//   where the chunks divide   the clip info file, extension data id 2/4, a list of start points
//   which two clips pair up   the playlist, extension data id 2/2, a sub path of type 8
//
// Measured on two pressed discs from different studios: the pairing rule held 36 times out of 36,
// every chunk size was a multiple of 6144, and a 43 GB .ssif was rebuilt byte for byte from these
// tables alone.

// One interleaved group: an .ssif and the two clips it is made of. Paths are disc relative with
// forward slashes, e.g. "BDMV/STREAM/00800.m2ts".
struct BdSsifGroup
{
    std::string ssifRel;
    std::string baseRel;
    std::string depRel;
    // Chunk sizes in bytes, in file order, one entry per chunk. The two vectors always have the same
    // length: the views interleave one for one.
    std::vector<int64_t> baseChunkBytes;
    std::vector<int64_t> depChunkBytes;
};

// The chunk boundaries from one clip info file. Fills sizes with the byte size of each chunk EXCEPT
// THE LAST, because the table gives start points and the final chunk runs to the end of the file;
// the caller adds the tail from the file size.
//
// Returns whether a table was found at all, which is NOT the same question as whether sizes is
// empty: a clip written as ONE chunk has a table with a single start point and therefore no
// differences, so it comes back true with an empty list. Treating empty as failure loses exactly
// those clips, which is what it did on the first disc it met.
bool bdReadClipChunkSizes(const std::string& clpiPath, std::vector<int64_t>& sizes);

// The base and dependent clip ids from one playlist, e.g. "00800" and "01064". Both empty when the
// playlist describes no stereoscopic sub path, which is every 2D playlist.
bool bdReadPlaylistStereoPair(const std::string& mplsPath, std::string& baseClipId, std::string& depClipId);

// Every interleaved group under a disc root, found by reading the playlists and then the clip info
// of each pair. srcRoot is the folder holding BDMV. Groups whose files are missing are left out.
std::vector<BdSsifGroup> bdFindSsifGroups(const std::string& srcRoot);

#endif  // BD_SSIF_IMPORT_H_
