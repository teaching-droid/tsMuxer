#ifndef ASS_RENDER_H_
#define ASS_RENDER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bitmapSubtitles.h"

class File;

// ASS and SSA subtitles drawn with libass (https://github.com/libass/libass, ISC licence) into pictures, and those
// redrawn as PGS display sets as a DVD or DVB subtitle track is: styles, colours, positions, rotations and the
// animations of the script, as libass draws them. libass is not built into tsMuxeR. It is loaded when an ASS or SSA
// track is drawn, from a libass library beside tsmuxer.exe (Windows: libass.dll, with the libraries it needs) or
// installed in the system; without it the track is drawn as SRT text, as before, with a warning.
namespace ass_render
{
// Whether libass could be loaded; the first call tries, later ones say what it found. why, when not, says why.
bool available(std::string* why = nullptr);

// A font for libass, from an attachment of the source file
struct Font
{
    std::string name;
    std::vector<uint8_t> data;
};

// The SRT file save-srt writes the text of an ASS or SSA track to. One for the whole track: each file of a join gets a
// decoder of its own, and each opened the file anew, so that only the last file's subtitles were left in it. It is
// made with the first subtitle written, so that a job refused before the mux leaves none, and it is closed when the
// last drawing that holds it ends.
class SaveSrtFile
{
   public:
    explicit SaveSrtFile(std::string name);
    ~SaveSrtFile();
    SaveSrtFile(const SaveSrtFile&) = delete;
    SaveSrtFile& operator=(const SaveSrtFile&) = delete;
    void write(const std::string& text, int64_t inMs, int64_t outMs);

   private:
    std::string m_name;
    std::unique_ptr<File> m_file;
    int m_count = 0;
};

// How an ASS or SSA track is drawn: the picture size and frame rate of the video, a folder of fonts besides those
// attached to the file (empty: none), and the SRT file save-srt writes the text to (empty: none)
struct Drawing
{
    int width = 0;
    int height = 0;
    double fps = 0;
    std::string fontsDir;
    std::string saveSrt;
    std::shared_ptr<SaveSrtFile> srtFile;  // the file of saveSrt, shared by the copies of the drawing
};

// The fonts among the attachments of a Matroska file (whole AttachedFile elements, as getPassThroughAttachments gives
// them): those whose media type or file name says font
std::vector<Font> fontsFromAttachments(const std::vector<std::vector<uint8_t>>& attachments);
}  // namespace ass_render

// One ASS or SSA track: its script header (CodecPrivate), then one block for each event, as Matroska stores them
// (ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text) with the event's time and duration.
class AssDecoder final : public BitmapSubDecoder
{
   public:
    AssDecoder(const uint8_t* header, int headerSize, const ass_render::Drawing& drawing,
               const std::vector<ass_render::Font>& fonts);
    ~AssDecoder() override;
    void decode(const uint8_t* data, int size, int64_t pts, int64_t duration, std::vector<uint8_t>& out) override;
    // The events not drawn yet, then the end of the last picture
    void finish(std::vector<uint8_t>& out) override;

   private:
    struct Impl;
    Impl* m;
    // Draws the frames of the video from the first not drawn yet up to the one given (not that one), and hands on a
    // picture wherever libass says the screen changed
    void drawUpTo(int64_t endFrame, std::vector<uint8_t>& out);
};

#endif
