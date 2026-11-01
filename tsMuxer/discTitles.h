#ifndef DISC_TITLES_H_
#define DISC_TITLES_H_

#include <string>
#include <vector>

// The titles of a Blu-ray disc, from its playlists alone. A disc folder or image given as a source was refused with
// "give the playlist of the title", and a disc can hold hundreds of playlists, most of them no title: one clip played
// again and again, many short pieces, copies of another playlist, the clips of the film in another order. Each such
// playlist is left out with the reason, never silently, and the main film is the title that plays the most footage.
struct DiscTitle
{
    int number = 0;        // the title's place in the listing, the main film first
    std::string playlist;  // the playlist file, the full path
    std::string name;      // its name, 00800.mpls
    double seconds = 0;    // what it plays, every play item counted
    double footage = 0;    // the footage it plays, each part of a clip counted once
    size_t chapters = 0;
    size_t clips = 0;                // the play items
    std::vector<std::string> audio;  // the languages of its audio, in order, each once
    bool is3d = false;
    int angles = 1;
    std::string note;  // "main film", "version of title 1 (4 clips differ)", or why a playlist is left out
};

struct DiscTitles
{
    std::vector<DiscTitle> titles;       // the main film first; empty when no playlist is a title
    std::vector<DiscTitle> leftOut;      // with the reason in the note, by playlist name
    std::vector<std::string> shortOnes;  // the playlists under the minimum length, by name
    double minSeconds = 120;
    bool minApplied = true;  // false when no playlist is that long: the shorter ones are the titles then
};

// The disc folder (the one holding BDMV, or BDMV itself) of a path, also inside a disc image; empty when the path is no
// Blu-ray folder.
std::string blurayDiscRoot(const std::string& path);

// The titles of the disc at `root`. With `all`, the left out and the short playlists are listed as titles too, after
// the others, with the reason in their note.
DiscTitles listDiscTitles(const std::string& root, double minSeconds, bool all);

// The listing printed for a disc folder or image given alone on the command line.
std::string describeDiscTitles(const DiscTitles& t);

// The playlist of title=<number> or title=main of a disc folder or image. Throws with the reason when there is none.
std::string discTitlePlaylist(const std::string& discPath, const std::string& title);

#endif  // DISC_TITLES_H_
