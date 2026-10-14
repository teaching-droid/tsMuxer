#ifndef BITMAP_SUBTITLES_H_
#define BITMAP_SUBTITLES_H_

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace text_subtitles
{
class TextToPGSConverter;
}

// The pictures of a bitmap subtitle format other than PGS, redrawn as PGS display sets in the form of a .sup file:
// every segment behind a "PG" header that carries its own times. The PGS reader takes them from there as it takes
// any PGS track, for a transport stream, a disc or a Matroska output alike.
class BitmapToPgs
{
   public:
    BitmapToPgs();
    ~BitmapToPgs();
    // The size of the pictures and the frame rate the display sets are timed to.
    void setCanvas(uint16_t width, uint16_t height, double fps);
    [[nodiscard]] uint16_t width() const { return m_width; }
    [[nodiscard]] uint16_t height() const { return m_height; }
    // The picture, a 32 bit BGRA value per pixel and 0 where it is transparent.
    uint32_t* pixels() { return m_canvas.data(); }
    void clearCanvas();
    // The display sets that show the picture from showTime on (internal clock). First the end of the picture before,
    // where it ends before this one starts; a picture that starts first replaces it. The end of this one, endTime,
    // is held until the next picture or finish(), and is -1 when it stays until the next. A picture with nothing
    // in it ends what is shown.
    void addPicture(int64_t showTime, int64_t endTime, bool forced, std::vector<uint8_t>& out);
    // The end that is still held, at the end of the track.
    void finish(std::vector<uint8_t>& out);

   private:
    std::unique_ptr<text_subtitles::TextToPGSConverter> m_pgs;
    std::vector<uint32_t> m_canvas;
    uint16_t m_width = 0;
    uint16_t m_height = 0;
    bool m_shown = false;    // a picture is on the screen
    int64_t m_heldEnd = -1;  // when it goes, internal clock; -1 when it stays until the next
    bool m_warnedColours = false;
    void addClear(double atTimeSec, std::vector<uint8_t>& out);
};

// The frame rate a PGS stream can be marked with that is nearest to fps, or 0 when none is near.
double nearestPgsFrameRate(double fps);

// A bitmap subtitle track of another format, its blocks decoded into pictures and handed on as PGS.
class BitmapSubDecoder
{
   public:
    virtual ~BitmapSubDecoder() = default;
    // One block, at pts (internal clock), that lasts duration, or -1 when it does not say.
    virtual void decode(const uint8_t* data, int size, int64_t pts, int64_t duration, std::vector<uint8_t>& out) = 0;
    virtual void finish(std::vector<uint8_t>& out) { m_pgs.finish(out); }
    // The frame rate was not known from a video track and was taken from the picture size.
    [[nodiscard]] bool rateGuessed() const { return m_rateGuessed; }

   protected:
    BitmapToPgs m_pgs;
    bool m_rateGuessed = false;
    // The canvas, marked with the video's frame rate (videoFps, 0 when the file has no video) or, without one, with
    // guessedFps, the rate the format's television standard has for the picture height.
    void setUp(int width, int height, double videoFps, double guessedFps);
};

// The subpictures of a DVD subtitle track, S_VOBSUB in Matroska: the CodecPrivate is the text of the .idx file, with
// the picture size and the 16 colours, and every block is one subpicture unit.
class VobSubDecoder final : public BitmapSubDecoder
{
   public:
    VobSubDecoder(const uint8_t* idx, int idxSize, double videoFps);
    void decode(const uint8_t* spu, int size, int64_t pts, int64_t duration, std::vector<uint8_t>& out) override;

   private:
    uint32_t m_palette[16]{};  // 0xRRGGBB
    bool m_warned = false;
};

// The display sets of a DVB subtitle track (ETSI EN 300 743), S_DVBSUB in Matroska: the CodecPrivate holds the
// composition and ancillary page ids, and every block is one display set. Regions, colour tables and the page persist
// from one display set to the next, as the standard's page updates need.
class DvbSubDecoder final : public BitmapSubDecoder
{
   public:
    DvbSubDecoder(const uint8_t* priv, int privSize, double videoFps);
    void decode(const uint8_t* data, int size, int64_t pts, int64_t duration, std::vector<uint8_t>& out) override;

   private:
    struct Clut
    {
        uint32_t entries2[4];  // BGRA, 0 transparent
        uint32_t entries4[16];
        uint32_t entries8[256];
    };
    struct RegionObject
    {
        uint16_t id;
        int x, y;
    };
    struct Region
    {
        int width = 0, height = 0;
        int depth = 0;  // bits per pixel: 2, 4 or 8
        uint8_t clutId = 0;
        std::vector<uint8_t> pixels;  // CLUT entries
        std::vector<RegionObject> objects;
    };
    struct PageRegion
    {
        uint8_t id;
        int x, y;
    };
    double m_videoFps;
    int m_compositionPage = -1;
    int m_ancillaryPage = -1;
    bool m_canvasSet = false;
    int m_displayWidth = 720, m_displayHeight = 576;
    int m_windowX = 0, m_windowY = 0;
    int m_pageTimeOut = 0;  // seconds
    std::vector<PageRegion> m_page;
    std::map<uint8_t, Region> m_regions;
    std::map<uint8_t, Clut> m_cluts;
    bool m_warnedDamage = false, m_warnedMethod = false, m_warnedSize = false;

    Clut& clut(uint8_t id);
    void displayDefinition(const uint8_t* seg, int len);
    void pageComposition(const uint8_t* seg, int len);
    void regionComposition(const uint8_t* seg, int len);
    void clutDefinition(const uint8_t* seg, int len);
    void objectData(const uint8_t* seg, int len);
    void drawField(Region& region, const uint8_t* data, int len, int x0, int y, bool nonModifying) const;
    void render();
};

#endif
