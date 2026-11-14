// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef AUDIO_CONVERT_H_
#define AUDIO_CONVERT_H_

#include <string>

// ** SOUND A DISC DOES NOT CARRY, CONVERTED BY THE USER'S FFMPEG. ** A Blu-ray carries LPCM, AC-3 and DTS (and their
// extensions); a player has to play only those. AAC, MP3, Opus, Vorbis, FLAC and the like from an MKV or MP4 file
// were written to a disc with a warning, or refused. A track can now be converted on its way to the output: to AC-3
// or to LPCM, also to stereo, or with a stereo track beside it. tsMuxeR does not convert sound itself and ships no
// converter: it asks the ffmpeg the user has (--ffmpeg, else one beside tsMuxeR or on the search path) for a file of
// the new kind, and muxes that file in place of the track.
//
// The converted sound starts where the film starts: ffmpeg is asked where the first picture of the video of the same
// file is shown, and pads or trims the sound to begin there, on its own timeline, which knows the edit lists, start
// delays and codec delays of every container it reads. A track of a file without video keeps its own start.
namespace audioConvert
{
struct Spec
{
    enum class Kind
    {
        none,
        ac3,
        lpcm
    };
    Kind kind = Kind::none;
    int kbps = 0;         // AC-3: kbit/s; 0: the suggested rate
    int bits = 0;         // LPCM: 16 or 24; 0: as the source
    int rate = 0;         // LPCM: 48000 or 96000; 0: as the source where a disc takes it, else 48000
    bool stereo = false;  // down-mixed to two channels
};

// Reads "ac3", "ac3:448", "ac3:stereo", "ac3:224:stereo", "lpcm", "lpcm:24", "lpcm:16:48", "lpcm:24:96:stereo".
// Returns the reason when the text is no such choice; empty when it is.
std::string parse(const std::string& text, Spec& spec);

// What is known of the track to convert, from its listing
struct Source
{
    std::string file;   // the file, as it is named on the line
    std::string map;    // the stream for ffmpeg: "0:a:0", "0:<index>" (Matroska) or "0:i:<id>" (MP4, TS)
    std::string codec;  // the codec of the line, such as A_AAC
    int channels = 0;   // 0: not known
    int rate = 0;       // Hz; 0: not known
    int bits = 0;       // of a lossless source; 0: not known
    int kbps = 0;       // of a lossy source; 0: not known
    bool lossless = false;
    // Where the converted sound has to begin, as the mux times the file: the start the mux measures the track's own
    // start against. timeKnown: at `startSeconds` on the file's own clock (a transport stream, whose mux can leave
    // out pictures before the first that can be decoded). firstPacket: at the first picture in decoding order (a
    // Matroska file, whose tracks are measured from the first block of the video). Otherwise the earliest picture.
    bool timeKnown = false;
    double startSeconds = 0;
    bool firstPacket = false;
};

// The AC-3 rate a track gets where none is given: a step above the source's own rate, as AC-3 needs more bits than
// AAC or Opus for the same sound, but never more than the source can fill; the most for a lossless source. Stereo
// 192 to 320 kbit/s, more channels 384 to 640 (the most a Blu-ray takes).
int suggestedKbps(const Source& source, bool stereo);
// The LPCM sample size and rate a track gets where none is given: 24 bit for a lossless source of more than 16 bit,
// else 16 (a lossy source has no more to keep); the source's rate where a disc takes it (48, 96 kHz; 192 kHz up to 6
// channels), else 48 kHz (96 kHz for 88.2 kHz and above).
int suggestedBits(const Source& source);
int suggestedRate(const Source& source, bool stereo);

// A text of the choice as the log gives it: "AC-3 5.1 at 448 kbit/s", "LPCM stereo, 48 kHz, 16 bit"
std::string describe(const Spec& spec, const Source& source);

// The codec of the line that takes the converted file: A_AC3 or A_LPCM
std::string codecOf(const Spec& spec);

// Converts the track into a file of the folder for temporary files and returns its name in `made`. ffmpeg is the
// one found (dvLayer::ffmpegToUse). Returns the reason when it failed; empty when the file was made.
std::string run(const std::string& ffmpeg, const Source& source, const Spec& spec, std::string& made, bool& aligned);

// Removes the files run() made in this run
void removeMade();
}  // namespace audioConvert

#endif
