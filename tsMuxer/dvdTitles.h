#ifndef DVD_TITLES_H_
#define DVD_TITLES_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The titles of a DVD-Video disc, from its IFO files (VIDEO_TS.IFO and VTS_nn_0.IFO, or their .BUP copies): what a
// title plays (cells of the joined VTS_nn_k.VOB files), its chapters, angles and streams. A folder or a disc image;
// an image is read through the file layer (discImage.h). The DVD counterpart of discTitles.h.

struct DvdCell
{
    double seconds = 0;
    uint32_t firstSector = 0;  // first VOBU start, in the joined VOB files of the title set
    uint32_t lastSector = 0;   // last VOBU end
    uint16_t vobId = 0;
    uint8_t cellId = 0;
    bool angle = false;     // part of an angle block
    uint8_t block = 0;      // 0 not in a block, 1 its first cell, 2 inside, 3 its last
    bool stcBreak = false;  // its timestamps do not follow on from the cell before (STC discontinuity)
};

struct DvdAudio
{
    std::string coding;  // AC-3, MPEG-1, MPEG-2, LPCM, DTS
    std::string lang;    // two letters as on the disc, empty when none is given
    int channels = 0;
    int track = 0;  // its track number in the VOB files: the stream id, private stream 1 sub-stream id included
};

struct DvdTitle
{
    int number = 0;     // the title's place in the listing, the main title first
    int discTitle = 0;  // its number on the disc (TT_SRPT)
    int titleSet = 0;
    int angles = 1;
    std::vector<int> pgcs;  // its program chains, in play order
    double seconds = 0;     // what it plays, the first angle of each angle block
    double footage = 0;     // the same, each cell counted once
    std::vector<double> chapters;
    std::vector<DvdCell> cells;
    std::vector<DvdAudio> audio;           // the streams its first program chain enables, in order
    std::vector<std::string> subpictures;  // their languages
    // the sub-stream ids (0x20 + stream number) its sub-pictures are in, with their languages: a stream can be in
    // several, one for each form of the picture (4:3, wide, letterbox, pan-scan)
    std::vector<std::pair<int, std::string>> subpictureTracks;
    std::array<uint32_t, 16> palette{};  // the colours of its sub-pictures, 0x00YYCrCb, from its first program chain
    int pictureWidth = 720, pictureHeight = 480;
    bool encrypted = false;   // its title set's video is still encrypted
    double missingShare = 0;  // the part of its cells beyond the end of the VOB files
    bool isMain = false;      // a main title: the film, or one of the episodes or films of the disc
    std::string note;
};

struct DvdTitles
{
    std::vector<DvdTitle> titles;   // the main titles first, in disc order
    int mainTitles = 0;             // how many: more than one on a disc of episodes or of several films
    std::vector<DvdTitle> leftOut;  // the reason in the note
    std::vector<int> shortOnes;     // disc title numbers under the minimum length
    double minSeconds = 120;
    bool minApplied = true;
    std::string video;  // "MPEG-2 720x576 25 fps 16:9" of the title set of the main title
};

// The VIDEO_TS folder of a path (VIDEO_TS itself, the folder holding it, or a disc image), also inside an image; empty
// when the path is no DVD-Video folder.
std::string dvdVideoTs(const std::string& path);

// The titles of the DVD whose VIDEO_TS folder is given. Throws with the reason when its IFO files and their copies
// cannot be read.
DvdTitles listDvdTitles(const std::string& videoTs, double minSeconds, bool all);

std::string describeDvdTitles(const DvdTitles& t);

// The source of title=<number> or title=main of a DVD folder or image, for a meta line: the cells of the title, the
// first angle, as parts of its VOB files under names that read them (discImage::addVirtualFile), one for each run of
// cells whose timestamps follow on, joined as "part 1"+"part 2". Throws with the reason when there is no such title,
// or its video is still encrypted. languages: the language of each audio track, by track number. chapters: where each
// chapter starts, in seconds from the start of the title (the playback times of the cells before it). discNumber: title
// is the number the disc gives the title (dvd-title=), which stays the same whatever the listing makes of it, and names
// the short and the left out titles too. minSeconds and all: the listing a number counts in, as for discTitlePlaylist.
// discTitle: the disc's own number of the title.
std::string dvdTitleSource(const std::string& videoTs, const std::string& title,
                           std::vector<std::pair<int, std::string>>* languages = nullptr,
                           std::vector<double>* chapters = nullptr, bool discNumber = false, double minSeconds = 120,
                           bool all = false, int* discTitle = nullptr);

// The .idx text the DVD subtitles of a VOB file are drawn with (VobSubDecoder): "size: 720x480" from its title set
// and "palette: rrggbb, ..." of the program chain that plays it. For the VOB files of a title (dvdTitleSource) the
// title's own colours, for a VTS_nn_k.VOB those of the first program chain of its title set that has cells. Empty when
// no IFO file of it can be read.
std::string dvdSubtitleIdx(const std::string& vobPath);
// The sub-picture streams the same program chain enables, as track numbers (0x20 + stream) with their languages: the
// listing gives a stream whose first subtitle comes after the part of the file it reads as well
std::vector<std::pair<int, std::string>> dvdSubtitleTracks(const std::string& vobPath);

#endif  // DVD_TITLES_H_
