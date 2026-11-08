// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef DISC_TITLES_H_
#define DISC_TITLES_H_

#include <string>
#include <utility>
#include <vector>

// The titles of a Blu-ray disc, from its playlists alone. A disc folder or image given as a source was refused with
// "give the playlist of the title", and a disc can hold hundreds of playlists, most of them no title: one clip played
// again and again, many short pieces, copies of another playlist, the clips of the film in another order. Each such
// playlist is left out with the reason, never silently, and the main film is the title that plays the most footage.
struct DiscTitle
{
    int number = 0;        // the title's place in the listing, the main film first
    std::string playlist;  // the playlist file, the full path
    std::string name;      // its name, 00011.mpls
    double seconds = 0;    // what it plays, every play item counted
    double footage = 0;    // the footage it plays, each part of a clip counted once
    size_t chapters = 0;
    size_t clips = 0;                // the play items
    std::vector<std::string> audio;  // the languages of its audio, in order, each once
    bool is3d = false;
    int angles = 1;
    // Where the angles differ: each part of several angles, its start and end in seconds of the title, and the clip
    // of each angle there (angleClips[angle - 1]). Empty for a title of one angle.
    std::vector<std::pair<double, double>> angleParts;
    std::vector<std::vector<std::string>> angleClips;
    bool isMain = false;  // a main title: the main film, or one of the episodes or films of a disc of several
    std::string note;     // "main film", "version of title 1 (4 clips differ)", or why a playlist is left out
    // What the playlist says of its picture: the dynamic range of its video (the first video entry of its stream
    // table: 0 SDR, 1 HDR10, 2 Dolby Vision; -1 for a playlist without video), and a sub path of type 10, which plays
    // the enhancement layer of a dual layer Dolby Vision picture. With the PID of that video, whether it is HEVC, and
    // the clip the title plays the most footage of, for a look at the start of its video (addPictureLabels).
    int dynamicRange = -1;
    bool dolbyVisionLayer = false;
    bool hevc = false;
    int videoPid = 0;
    std::string mainClip;
    std::string picture;  // the label of the listing: "SDR", "HDR10", "HDR10+, Dolby Vision profile 7" ...
};

// The lines under a title of several angles that say where they differ: "    angles differ at 0:00:00-0:03:42", then
// for each angle its clips there ("    angle 2: 00101.m2ts"), when clips are given. Empty when parts is empty.
std::string anglePartsText(const std::vector<std::pair<double, double>>& parts,
                           const std::vector<std::vector<std::string>>& clips);

struct DiscTitles
{
    std::vector<DiscTitle> titles;       // the main titles first, in disc order; empty when no playlist is a title
    int mainTitles = 0;                  // how many: more than one on a disc of episodes or of several films
    std::vector<DiscTitle> leftOut;      // with the reason in the note, by playlist name
    std::vector<std::string> shortOnes;  // the playlists under the minimum length, by name
    double minSeconds = 120;
    bool minApplied = true;  // false when no playlist is that long: the shorter ones are the titles then
};

// The disc folder (the one holding BDMV, or BDMV itself) of a path, also inside a disc image; empty when the path is no
// Blu-ray folder.
std::string blurayDiscRoot(const std::string& path);

// The Blu-ray and DVD disc folders and images one level down in `folder` (a box set copied disc by disc), each with its
// kind ("Blu-ray folder", "DVD image" ...), by name; empty when there is none.
std::vector<std::pair<std::string, std::string>> discsInFolder(const std::string& folder);

// The titles of the disc at `root`. With `all`, the left out and the short playlists are listed as titles too, after
// the others, with the reason in their note.
DiscTitles listDiscTitles(const std::string& root, double minSeconds, bool all);

// The picture of each title, for the listing: SDR, HDR10, HDR10+, Dolby Vision (with its profile where known) or a
// combination, in DiscTitle::picture. What the playlist says is always known, also from the control files of a disc
// alone. HDR10+ and a single layer Dolby Vision are in the video stream only: the start of the main clip of each main
// title is read for them (not of every title, which would take long on a disc of hundreds), when the clip is there.
void addPictureLabels(DiscTitles& t, const std::string& root);

// The listing printed for a disc folder or image given alone on the command line.
std::string describeDiscTitles(const DiscTitles& t);

// The playlist of title=<number> or title=main of a disc folder or image. Throws with the reason when there is none.
// minSeconds and all: the listing the number counts in (--show-title after --min-title-length or --all-playlists).
std::string discTitlePlaylist(const std::string& discPath, const std::string& title, double minSeconds = 120,
                              bool all = false);

#endif  // DISC_TITLES_H_
