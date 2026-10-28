#include "bitmapSubtitles.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "textSubtitles.h"
#include "vod_common.h"

using text_subtitles::CompositionMode;
using text_subtitles::TextToPGSConverter;
using text_subtitles::YUVQuad;
#ifndef _WIN32
using text_subtitles::RGBQUAD;  // windows.h has it on Windows
#endif

double nearestPgsFrameRate(const double fps)
{
    static constexpr double RATES[] = {24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60};
    for (const double rate : RATES)
        if (fabs(fps - rate) < rate * 0.005)
            return rate;
    return 0;
}

BitmapToPgs::BitmapToPgs() : m_pgs(std::make_unique<TextToPGSConverter>(false)) {}

BitmapToPgs::~BitmapToPgs() = default;

void BitmapToPgs::setCanvas(const uint16_t width, const uint16_t height, const double fps)
{
    m_width = width;
    m_height = height;
    m_canvas.assign(static_cast<size_t>(width) * height, 0);
    m_pgs->setCanvas(width, height, fps);
    m_pgs->setImageBuffer(reinterpret_cast<uint8_t*>(m_canvas.data()));
}

void BitmapToPgs::clearCanvas() { std::fill(m_canvas.begin(), m_canvas.end(), 0); }

void BitmapToPgs::addClear(const double atTimeSec, std::vector<uint8_t>& out, const int64_t latestDts)
{
    uint32_t len = 0;
    const uint8_t* data = m_pgs->doClear(atTimeSec, len, latestDts);
    out.insert(out.end(), data, data + len);
    m_shown = false;
    m_lastPts = llround(m_pgs->alignToGrid(atTimeSec) * 90000.0);
}

bool BitmapToPgs::addPicture(const int64_t showTime, const int64_t endTime, const bool forced,
                             std::vector<uint8_t>& out)
{
    const double inSec = m_pgs->alignToGrid(static_cast<double>(showTime) / INTERNAL_PTS_FREQ);
    // The end of the picture before, when it comes before this one. When it does not, or when the two fall on the
    // same frame, the new picture replaces the old one on the screen, as a text subtitle does that follows the one
    // before without a gap.
    const auto endBefore = [&](const int64_t latestDts)
    {
        if (m_shown && m_heldEnd >= 0)
        {
            const double outSec = m_pgs->alignToGrid(static_cast<double>(m_heldEnd) / INTERNAL_PTS_FREQ);
            if (outSec < inSec)
                addClear(outSec, out, latestDts);
        }
        m_heldEnd = -1;
    };

    // Fewer colours until the picture packs: the same steps as for a text subtitle.
    uint32_t mask = 0;
    int step = 0;
    while (!m_pgs->rlePack(mask))
    {
        if (++step > 32)
        {
            if (!m_warnedColours)
                LTRACE(LT_WARN, 2,
                       "Warning: a subtitle picture at " << floatToTime(inSec)
                                                         << " could not be packed as PGS and was left out.");
            m_warnedColours = true;
            endBefore(INT64_MAX);
            return true;
        }
        const auto tmp = reinterpret_cast<uint8_t*>(&mask);
        const int idx = (step - 1) % 4;
        tmp[idx] <<= 1;
        tmp[idx]++;
    }
    if (m_pgs->m_rleLen == 0)
    {
        // Nothing to show: the picture empties the screen. Where such ends are held, that end is held as the end of a
        // picture is, unless the picture ends earlier: the next picture says when the clear is decoded.
        if (m_shown && m_holdEmptyEnds && (m_heldEnd < 0 || m_heldEnd > showTime))
            m_heldEnd = showTime;
        else
        {
            endBefore(INT64_MAX);
            if (m_shown)
                addClear(inSec, out);
        }
        return true;
    }

    // As for a text subtitle (TextToPGSConverter::doConvert): the presentation, the window, the palette and the
    // object, each with the times a player needs to decode and draw it, then the end of the display set. The window
    // and the object are the box around what the picture draws, at the place it has in the picture (packDrawnBox);
    // they were as wide as the picture. Every picture starts an epoch, which only its clear continues, so the window
    // may change from one picture to the next: also on each frame of a fade, a move or karaoke that libass draws.
    const uint16_t top = m_pgs->padToLeastHeight(m_pgs->minLine());
    const auto lines = static_cast<uint16_t>(FFMAX(8, m_pgs->renderedHeight()));
    const uint16_t left = m_pgs->m_objectLeft;
    const uint16_t width = m_pgs->m_objectWidth;
    const double objectPixels = static_cast<double>(lines) * width;
    const int64_t decodeTime = TextToPGSConverter::objectDecodeTime(objectPixels);
    const int64_t drawTime = TextToPGSConverter::windowDrawTime(objectPixels);
    const int64_t decodeDuration =
        FFMAX(TextToPGSConverter::windowDrawTime(static_cast<double>(m_width) * m_height), decodeTime) + drawTime;
    const int64_t pts = llround(inSec * 90000.0);
    const int64_t dts = pts - decodeDuration;
    if (m_waitUntilShown)
    {
        // ** A DISPLAY SET IS DECODED AFTER THE ONE BEFORE IT IS SHOWN. ** Every picture starts an epoch, and a player
        // decodes it (clearing the whole screen first) only once the display set before is on the screen. A picture
        // that would have to be decoded earlier waits: a later frame of the fade or move takes its place.
        if (m_lastPts >= 0 && dts <= m_lastPts)
            return false;
        // The same for the clear of a picture that ended a few frames before this one: the clear is left out, and
        // this picture replaces the one before at its own time, which stays on the screen until then.
        if (m_shown && m_heldEnd >= 0 &&
            dts <= llround(m_pgs->alignToGrid(static_cast<double>(m_heldEnd) / INTERNAL_PTS_FREQ) * 90000.0))
            m_heldEnd = -1;
    }
    // ** THE CLEAR BEFORE A PICTURE IS DECODED NO LATER THAN THE PICTURE. ** A picture is decoded for as long as the
    // whole screen takes to clear, and more, before it is shown: where the one before ended less than that before
    // it, its clear was decoded after the new picture, and the decoding times of the stream went back. The clear is
    // then decoded with the picture, still shown at its own time.
    endBefore(dts);
    m_pgs->m_lastWindowLeft = left;
    m_pgs->m_lastWindowTop = top;
    m_pgs->m_lastWindowWidth = width;
    m_pgs->m_lastWindowHeight = lines;
    m_pgs->m_lastWindowDrawTime = drawTime;

    const int64_t odfPts = dts + decodeTime;
    m_pgs->palette_update_flag = false;
    m_pgs->m_paletteID = 0;
    m_pgs->m_paletteVersion = 0;

    // A display set needs at most its picture packed, which rlePack keeps within (width + 16) bytes a line, and the
    // headers of its segments.
    std::vector<uint8_t> buf(static_cast<size_t>(m_pgs->m_rleLen) + (m_pgs->m_rleLen / 65000 + 1) * 32 + 2048);
    uint8_t* cur = buf.data();
    cur += m_pgs->composePresentationSegment(cur, CompositionMode::Start, pts, dts, left, top, true, forced);
    cur += m_pgs->composeWindowDefinition(cur, pts - drawTime, dts, left, top, width, lines);
    cur += m_pgs->composePaletteDefinition(m_pgs->m_paletteByColor, cur, dts, dts);
    cur += m_pgs->composeObjectDefinition(cur, odfPts, dts, 0, lines - 1, true);
    cur += m_pgs->composeEnd(cur, odfPts, odfPts);
    out.insert(out.end(), buf.data(), cur);
    m_shown = true;
    m_heldEnd = endTime;
    m_lastPts = pts;
    return true;
}

void BitmapToPgs::finish(std::vector<uint8_t>& out)
{
    if (m_shown && m_heldEnd >= 0)
        addClear(m_pgs->alignToGrid(static_cast<double>(m_heldEnd) / INTERNAL_PTS_FREQ), out);
    m_heldEnd = -1;
}

void BitmapSubDecoder::setUp(const int width, const int height, const double videoFps, const double guessedFps)
{
    double fps = nearestPgsFrameRate(videoFps);
    if (fps == 0)
    {
        fps = guessedFps;
        m_rateGuessed = true;
    }
    m_pgs.setCanvas(static_cast<uint16_t>(width), static_cast<uint16_t>(height), fps);
}

// ------------ DVD subpictures ---------------

VobSubDecoder::VobSubDecoder(const uint8_t* idx, const int idxSize, const double videoFps)
{
    // The .idx text: "size: 720x576" and "palette: rrggbb, rrggbb, ..." are the lines that matter here.
    int width = 720;
    int height = 576;
    bool haveSize = false;
    std::istringstream lines(std::string(reinterpret_cast<const char*>(idx), idx ? static_cast<size_t>(idxSize) : 0));
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("size:", 0) == 0)
        {
            int w = 0, h = 0;
            if (sscanf(line.c_str() + 5, " %dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 1920 && h <= 1080)
            {
                width = w;
                height = h;
                haveSize = true;
            }
        }
        else if (line.rfind("palette:", 0) == 0)
        {
            std::istringstream colours(line.substr(8));
            std::string colour;
            for (int i = 0; i < 16 && std::getline(colours, colour, ','); ++i)
                m_palette[i] = static_cast<uint32_t>(strtoul(colour.c_str(), nullptr, 16)) & 0xFFFFFF;
        }
    }
    if (!haveSize)
        LTRACE(LT_WARN, 2, "Warning: the DVD subtitle track gives no picture size; 720x576 is used.");
    // Without a video track in the file, the rate of the DVD standard for the picture height: 25 for 576 lines, 29.97
    // for the others.
    setUp(width, height, videoFps, height == 576 ? 25.0 : 30000.0 / 1001);
}

namespace
{
// The pixels of one field of a subpicture: run lengths in 4 bit steps (DVD-Video, subpicture unit).
class NibbleReader
{
   public:
    NibbleReader(const uint8_t* data, const int start, const int end) : m_data(data), m_pos(start * 2), m_end(end * 2)
    {
    }
    [[nodiscard]] bool atEnd() const { return m_pos >= m_end; }
    int nibble()
    {
        if (m_pos >= m_end)
            return 0;
        const int v = m_pos & 1 ? m_data[m_pos / 2] & 0x0F : m_data[m_pos / 2] >> 4;
        ++m_pos;
        return v;
    }
    // 1 to 4 nibbles: the run length in the upper bits, the colour index in the lowest 2. A length of 0 runs to
    // the end of the line.
    int code()
    {
        int v = nibble();
        if (v < 0x4)
        {
            v = v << 4 | nibble();
            if (v < 0x10)
            {
                v = v << 4 | nibble();
                if (v < 0x40)
                    v = v << 4 | nibble();
            }
        }
        return v;
    }
    void alignToByte() { m_pos = (m_pos + 1) & ~1; }

   private:
    const uint8_t* m_data;
    int m_pos;  // in nibbles
    int m_end;
};
}  // namespace

void VobSubDecoder::decode(const uint8_t* spu, const int size, const int64_t pts, const int64_t duration,
                           std::vector<uint8_t>& out)
{
    const auto bad = [this]()
    {
        if (!m_warned)
            LTRACE(LT_WARN, 2, "Warning: a damaged DVD subtitle picture was left out.");
        m_warned = true;
    };
    if (size < 4)
        return bad();
    const int unitSize = (std::min)(static_cast<int>(AV_RB16(spu)), size);
    const int firstSequence = AV_RB16(spu + 2);
    if (firstSequence < 4 || firstSequence + 4 > unitSize)
        return bad();

    // The control sequences: each starts after a delay from the unit's time, in units of 1024 ticks of 90 kHz, and
    // holds commands until 0xFF.
    int startDate = -1, stopDate = -1;
    bool forced = false;
    int colour[4] = {0, 1, 2, 3};
    int alpha[4] = {0, 15, 15, 15};
    int x1 = 0, x2 = -1, y1 = 0, y2 = -1;
    int fieldStart[2] = {0, 0};
    int pos = firstSequence;
    for (int sequences = 0; sequences < 64 && pos + 4 <= unitSize; ++sequences)
    {
        const int date = AV_RB16(spu + pos);
        const int next = AV_RB16(spu + pos + 2);
        int p = pos + 4;
        bool end = false;
        while (!end && p < unitSize)
        {
            switch (spu[p++])
            {
            case 0x00:  // forced start of display
                forced = true;
                [[fallthrough]];
            case 0x01:  // start of display
                if (startDate < 0)
                    startDate = date;
                break;
            case 0x02:  // stop of display
                if (stopDate < 0)
                    stopDate = date;
                break;
            case 0x03:  // the palette entry of each of the 4 pixel values, the last one first
                if (p + 2 > unitSize)
                    return bad();
                colour[3] = spu[p] >> 4;
                colour[2] = spu[p] & 0x0F;
                colour[1] = spu[p + 1] >> 4;
                colour[0] = spu[p + 1] & 0x0F;
                p += 2;
                break;
            case 0x04:  // the opacity of each, 0 to 15, in the same order
                if (p + 2 > unitSize)
                    return bad();
                alpha[3] = spu[p] >> 4;
                alpha[2] = spu[p] & 0x0F;
                alpha[1] = spu[p + 1] >> 4;
                alpha[0] = spu[p + 1] & 0x0F;
                p += 2;
                break;
            case 0x05:  // the area: first and last column, first and last line, 12 bits each
                if (p + 6 > unitSize)
                    return bad();
                x1 = spu[p] << 4 | spu[p + 1] >> 4;
                x2 = (spu[p + 1] & 0x0F) << 8 | spu[p + 2];
                y1 = spu[p + 3] << 4 | spu[p + 4] >> 4;
                y2 = (spu[p + 4] & 0x0F) << 8 | spu[p + 5];
                p += 6;
                break;
            case 0x06:  // where the lines of the top and of the bottom field start
                if (p + 4 > unitSize)
                    return bad();
                fieldStart[0] = AV_RB16(spu + p);
                fieldStart[1] = AV_RB16(spu + p + 2);
                p += 4;
                break;
            case 0x07:  // colour and contrast changes within the area: its size word counts itself
            {
                if (p + 2 > unitSize || AV_RB16(spu + p) < 2)
                {
                    end = true;
                    break;
                }
                p += AV_RB16(spu + p);
                break;
            }
            default:  // 0xFF ends the sequence, and an unknown command cannot be stepped over
                end = true;
            }
        }
        if (next <= pos)
            break;  // the last sequence points to itself
        pos = next;
    }

    const auto toInternal = [](const int date)
    { return static_cast<int64_t>(date) * 1024 * INTERNAL_PTS_FREQ / 90000; };
    const int64_t showTime = pts + (startDate > 0 ? toInternal(startDate) : 0);
    int64_t endTime = -1;
    if (stopDate > (std::max)(startDate, 0))
        endTime = pts + toInternal(stopDate);
    else if (duration > 0)
        endTime = pts + duration;

    // A unit that only stops the display has nothing to draw, and its empty picture ends what is shown.
    m_pgs.clearCanvas();
    if (startDate >= 0 && x2 >= x1 && y2 >= y1 && fieldStart[0] >= 4 && fieldStart[1] >= 4)
    {
        const int w = x2 - x1 + 1;
        const int h = y2 - y1 + 1;
        uint32_t argb[4];
        for (int i = 0; i < 4; ++i)
            argb[i] = alpha[i] == 0 ? 0 : static_cast<uint32_t>(alpha[i] * 17) << 24 | m_palette[colour[i]];
        // The pixel data ends where the control sequences begin.
        NibbleReader field[2] = {NibbleReader(spu, fieldStart[0], firstSequence),
                                 NibbleReader(spu, fieldStart[1], firstSequence)};
        uint32_t* canvas = m_pgs.pixels();
        for (int y = 0; y < h; ++y)
        {
            NibbleReader& reader = field[y & 1];
            const int canvasY = y1 + y;
            for (int x = 0; x < w && !reader.atEnd();)
            {
                const int code = reader.code();
                int run = code >> 2;
                if (run == 0 || run > w - x)
                    run = w - x;
                const uint32_t pixel = argb[code & 3];
                if (pixel && canvasY < m_pgs.height())
                    for (int i = 0; i < run; ++i)
                        if (x1 + x + i < m_pgs.width())
                            canvas[static_cast<size_t>(canvasY) * m_pgs.width() + x1 + x + i] = pixel;
                x += run;
            }
            reader.alignToByte();
        }
    }
    m_pgs.addPicture(showTime, endTime, forced, out);
}

// ------------ DVB subtitles ---------------

namespace
{
// The bits of a pixel code string, first bit first, never past the end of its field.
class FieldBits
{
   public:
    FieldBits(const uint8_t* data, const int len, const int pos) : m_data(data), m_bitEnd(len * 8), m_bit(pos * 8) {}
    [[nodiscard]] bool atEnd() const { return m_bit >= m_bitEnd; }
    int bits(const int n)
    {
        int v = 0;
        for (int i = 0; i < n; ++i, ++m_bit)
            v = v << 1 | (m_bit < m_bitEnd ? m_data[m_bit >> 3] >> (7 - (m_bit & 7)) & 1 : 0);
        return v;
    }
    [[nodiscard]] int bytePos() const { return (m_bit + 7) >> 3; }

   private:
    const uint8_t* m_data;
    int m_bitEnd;
    int m_bit;
};

uint32_t bgraFromYCrCbT(const int y, const int cr, const int cb, const int t)
{
    // Y = 0 is full transparency, and T counts from opaque (0) towards fully transparent.
    if (y == 0 || t >= 255)
        return 0;
    YUVQuad yuv;
    yuv.Y = static_cast<uint8_t>(y);
    yuv.Cr = static_cast<uint8_t>(cr);
    yuv.Cb = static_cast<uint8_t>(cb);
    yuv.alpha = static_cast<uint8_t>(255 - t);
    const RGBQUAD rgb = TextToPGSConverter::YUVAToRGBA(yuv);
    return static_cast<uint32_t>(rgb.rgbReserved) << 24 | static_cast<uint32_t>(rgb.rgbRed) << 16 |
           static_cast<uint32_t>(rgb.rgbGreen) << 8 | rgb.rgbBlue;
}

uint32_t bgra(const double r, const double g, const double b, const double transparency)
{
    if (transparency >= 1.0)
        return 0;
    const auto c = [](const double v) { return static_cast<uint32_t>(lround(v * 255)); };
    return c(1.0 - transparency) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}
}  // namespace

DvbSubDecoder::DvbSubDecoder(const uint8_t* priv, const int privSize, const double videoFps) : m_videoFps(videoFps)
{
    // composition_page_id, ancillary_page_id, subtitling_type (Matroska codec mapping, S_DVBSUB)
    if (priv && privSize >= 4)
    {
        m_compositionPage = AV_RB16(priv);
        m_ancillaryPage = AV_RB16(priv + 2);
    }
}

// The colour table family, at its default contents (EN 300 743, clause 10) until a CLUT definition changes them.
DvbSubDecoder::Clut& DvbSubDecoder::clut(const uint8_t id)
{
    const auto found = m_cluts.find(id);
    if (found != m_cluts.end())
        return found->second;
    Clut& c = m_cluts[id];
    // 4 entries: transparent, white, black, grey
    c.entries2[0] = 0;
    c.entries2[1] = bgra(1, 1, 1, 0);
    c.entries2[2] = bgra(0, 0, 0, 0);
    c.entries2[3] = bgra(0.5, 0.5, 0.5, 0);
    // 16 entries, bits b1..b4 from the most significant
    for (int i = 0; i < 16; ++i)
    {
        const int b1 = i >> 3 & 1, b2 = i >> 2 & 1, b3 = i >> 1 & 1, b4 = i & 1;
        if (b1 == 0)
            c.entries4[i] = i == 0 ? 0 : bgra(b4, b3, b2, 0);
        else
            c.entries4[i] = bgra(0.5 * b4, 0.5 * b3, 0.5 * b2, 0);
    }
    // 256 entries, bits b1..b8 from the most significant
    for (int i = 0; i < 256; ++i)
    {
        const int b1 = i >> 7 & 1, b2 = i >> 6 & 1, b3 = i >> 5 & 1, b4 = i >> 4 & 1;
        const int b5 = i >> 3 & 1, b6 = i >> 2 & 1, b7 = i >> 1 & 1, b8 = i & 1;
        uint32_t v;
        if (b1 == 0 && b5 == 0)
        {
            if (b2 == 0 && b3 == 0 && b4 == 0)
                v = b6 == 0 && b7 == 0 && b8 == 0 ? 0 : bgra(b8, b7, b6, 0.75);
            else
                v = bgra(b8 / 3.0 + b4 * 2 / 3.0, b7 / 3.0 + b3 * 2 / 3.0, b6 / 3.0 + b2 * 2 / 3.0, 0);
        }
        else if (b1 == 0)
            v = bgra(b8 / 3.0 + b4 * 2 / 3.0, b7 / 3.0 + b3 * 2 / 3.0, b6 / 3.0 + b2 * 2 / 3.0, 0.5);
        else if (b5 == 0)
            v = bgra(b8 / 6.0 + b4 / 3.0 + 0.5, b7 / 6.0 + b3 / 3.0 + 0.5, b6 / 6.0 + b2 / 3.0 + 0.5, 0);
        else
            v = bgra(b8 / 6.0 + b4 / 3.0, b7 / 6.0 + b3 / 3.0, b6 / 6.0 + b2 / 3.0, 0);
        c.entries8[i] = v;
    }
    return c;
}

void DvbSubDecoder::displayDefinition(const uint8_t* seg, const int len)
{
    if (len < 5)
        return;
    const bool window = seg[0] >> 3 & 1;
    const int width = AV_RB16(seg + 1) + 1;
    const int height = AV_RB16(seg + 3) + 1;
    if (m_canvasSet && (width != m_displayWidth || height != m_displayHeight))
    {
        if (!m_warnedSize)
            LTRACE(LT_WARN, 2,
                   "Warning: the DVB subtitle display size changes to " << width << "x" << height
                                                                        << "; the first size is kept.");
        m_warnedSize = true;
        return;
    }
    if (width > 1920 || height > 1080)
        return;
    m_displayWidth = width;
    m_displayHeight = height;
    m_windowX = m_windowY = 0;
    if (window && len >= 13)
    {
        m_windowX = AV_RB16(seg + 5);
        m_windowY = AV_RB16(seg + 9);
    }
}

void DvbSubDecoder::pageComposition(const uint8_t* seg, const int len)
{
    if (len < 2)
        return;
    m_pageTimeOut = seg[0];
    // A mode change starts a new epoch: what the regions and colour tables held before goes.
    if ((seg[1] >> 2 & 3) == 2)
    {
        m_regions.clear();
        m_cluts.clear();
    }
    m_page.clear();
    for (int p = 2; p + 6 <= len; p += 6) m_page.push_back({seg[p], AV_RB16(seg + p + 2), AV_RB16(seg + p + 4)});
}

void DvbSubDecoder::regionComposition(const uint8_t* seg, const int len)
{
    if (len < 10)
        return;
    Region& region = m_regions[seg[0]];
    const bool fill = seg[1] >> 3 & 1;
    const int width = AV_RB16(seg + 2);
    const int height = AV_RB16(seg + 4);
    const int depthCode = seg[6] >> 2 & 7;
    const int depth = depthCode == 1 ? 2 : depthCode == 2 ? 4 : depthCode == 3 ? 8 : 0;
    region.clutId = seg[7];
    const uint8_t background = depth == 8 ? seg[8] : depth == 4 ? seg[9] >> 4 : seg[9] >> 2 & 3;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || depth == 0)
    {
        region.width = region.height = 0;
        region.pixels.clear();
        return;
    }
    if (width != region.width || height != region.height || depth != region.depth)
    {
        region.width = width;
        region.height = height;
        region.depth = depth;
        region.pixels.assign(static_cast<size_t>(width) * height, fill ? background : 0);
    }
    else if (fill)
        std::fill(region.pixels.begin(), region.pixels.end(), background);
    region.objects.clear();
    for (int p = 10; p + 6 <= len;)
    {
        const int type = seg[p + 2] >> 6;
        const int provider = seg[p + 2] >> 4 & 3;
        if (type == 0 && provider == 0)  // a bitmap carried in the stream
            region.objects.push_back(
                {AV_RB16(seg + p), (seg[p + 2] & 0x0F) << 8 | seg[p + 3], (seg[p + 4] & 0x0F) << 8 | seg[p + 5]});
        p += type == 1 || type == 2 ? 8 : 6;
    }
}

void DvbSubDecoder::clutDefinition(const uint8_t* seg, const int len)
{
    if (len < 2)
        return;
    Clut& c = clut(seg[0]);
    for (int p = 2; p + 4 <= len;)
    {
        const int entry = seg[p];
        const int flags = seg[p + 1];
        int y, cr, cb, t;
        if (flags & 1)  // full range
        {
            if (p + 6 > len)
                break;
            y = seg[p + 2];
            cr = seg[p + 3];
            cb = seg[p + 4];
            t = seg[p + 5];
            p += 6;
        }
        else  // the most significant bits only: 6 of Y, 4 of Cr and of Cb, 2 of T
        {
            y = seg[p + 2] >> 2 << 2;
            cr = ((seg[p + 2] & 3) << 2 | seg[p + 3] >> 6) << 4;
            cb = (seg[p + 3] >> 2 & 0x0F) << 4;
            t = (seg[p + 3] & 3) << 6;
            p += 4;
        }
        const uint32_t colour = bgraFromYCrCbT(y, cr, cb, t);
        if (flags & 0x80 && entry < 4)
            c.entries2[entry] = colour;
        if (flags & 0x40 && entry < 16)
            c.entries4[entry] = colour;
        if (flags & 0x20)
            c.entries8[entry] = colour;
    }
}

// One field of an object, its pixel code strings, into the region from line y on, every second line.
void DvbSubDecoder::drawField(Region& region, const uint8_t* data, const int len, const int x0, int y,
                              const bool nonModifying) const
{
    uint8_t map2to4[4] = {0x0, 0x7, 0x8, 0xF};
    uint8_t map2to8[4] = {0x00, 0x77, 0x88, 0xFF};
    uint8_t map4to8[16];
    for (int i = 0; i < 16; ++i) map4to8[i] = static_cast<uint8_t>(i * 0x11);
    int x = x0;
    // A pixel code of codeBits bits as an entry of the region's table: the map tables to more bits, the reductions of
    // clause 9 to fewer.
    const auto put = [&](const int run, const int code, const int codeBits)
    {
        int entry = code;
        if (region.depth == 2)
        {
            if (codeBits == 4)
                entry = (code >> 3) << 1 | ((code & 7) != 0);
            else if (codeBits == 8)
                entry = (code >> 7) << 1 | ((code & 0x70) != 0);
        }
        else if (region.depth == 4)
        {
            if (codeBits == 2)
                entry = map2to4[code];
            else if (codeBits == 8)
                entry = code >> 4;
        }
        else if (codeBits == 2)
            entry = map2to8[code];
        else if (codeBits == 4)
            entry = map4to8[code];
        if (!(nonModifying && entry == 1) && y >= 0 && y < region.height)
            for (int i = (std::max)(0, -x); i < run && x + i < region.width; ++i)
                region.pixels[static_cast<size_t>(y) * region.width + x + i] = static_cast<uint8_t>(entry);
        x += run;
    };
    int pos = 0;
    while (pos < len)
    {
        const int type = data[pos++];
        if (type == 0x10 || type == 0x11 || type == 0x12)
        {
            FieldBits b(data, len, pos);
            if (type == 0x10)
            {
                while (!b.atEnd())
                {
                    if (const int c = b.bits(2))
                        put(1, c, 2);
                    else if (b.bits(1))
                    {
                        const int run = b.bits(3) + 3;
                        put(run, b.bits(2), 2);
                    }
                    else if (b.bits(1))
                        put(1, 0, 2);
                    else
                    {
                        const int s = b.bits(2);
                        if (s == 0)
                            break;
                        if (s == 1)
                            put(2, 0, 2);
                        else
                        {
                            const int run = s == 2 ? b.bits(4) + 12 : b.bits(8) + 29;
                            put(run, b.bits(2), 2);
                        }
                    }
                }
            }
            else if (type == 0x11)
            {
                while (!b.atEnd())
                {
                    if (const int c = b.bits(4))
                        put(1, c, 4);
                    else if (!b.bits(1))
                    {
                        const int n = b.bits(3);
                        if (n == 0)
                            break;
                        put(n + 2, 0, 4);
                    }
                    else if (!b.bits(1))
                    {
                        const int run = b.bits(2) + 4;
                        put(run, b.bits(4), 4);
                    }
                    else
                    {
                        const int s = b.bits(2);
                        if (s < 2)
                            put(s + 1, 0, 4);
                        else
                        {
                            const int run = s == 2 ? b.bits(4) + 9 : b.bits(8) + 25;
                            put(run, b.bits(4), 4);
                        }
                    }
                }
            }
            else
            {
                while (!b.atEnd())
                {
                    if (const int c = b.bits(8))
                        put(1, c, 8);
                    else if (!b.bits(1))
                    {
                        const int n = b.bits(7);
                        if (n == 0)
                            break;
                        put(n, 0, 8);
                    }
                    else
                    {
                        const int run = b.bits(7);
                        put(run, b.bits(8), 8);
                    }
                }
            }
            pos = b.bytePos();
        }
        else if (type == 0x20 && pos + 2 <= len)
        {
            for (int i = 0; i < 4; ++i) map2to4[i] = static_cast<uint8_t>(data[pos + i / 2] >> (i % 2 ? 0 : 4) & 0x0F);
            pos += 2;
        }
        else if (type == 0x21 && pos + 4 <= len)
        {
            memcpy(map2to8, data + pos, 4);
            pos += 4;
        }
        else if (type == 0x22 && pos + 16 <= len)
        {
            memcpy(map4to8, data + pos, 16);
            pos += 16;
        }
        else if (type == 0xF0)  // the end of a line
        {
            x = x0;
            y += 2;
        }
        else
            break;  // unknown, or cut short
    }
}

void DvbSubDecoder::objectData(const uint8_t* seg, const int len)
{
    if (len < 3)
        return;
    const uint16_t id = AV_RB16(seg);
    const int method = seg[2] >> 2 & 3;
    const bool nonModifying = seg[2] >> 1 & 1;
    if (method != 0)
    {
        // 1: characters from a table the standard does not define; 2: zlib compressed progressive pictures
        if (!m_warnedMethod)
            LTRACE(LT_WARN, 2,
                   "Warning: DVB subtitle objects coded " << (method == 1 ? "as characters" : "progressively")
                                                          << " cannot be read and were left out.");
        m_warnedMethod = true;
        return;
    }
    if (len < 7)
        return;
    const int topLength = AV_RB16(seg + 3);
    const int bottomLength = AV_RB16(seg + 5);
    if (7 + topLength + bottomLength > len)
    {
        if (!m_warnedDamage)
            LTRACE(LT_WARN, 2, "Warning: a damaged DVB subtitle object was left out.");
        m_warnedDamage = true;
        return;
    }
    const uint8_t* top = seg + 7;
    // A bottom field without data of its own repeats the top field.
    const uint8_t* bottom = bottomLength ? top + topLength : top;
    const int bottomLen = bottomLength ? bottomLength : topLength;
    for (auto& [regionId, region] : m_regions)
        for (const RegionObject& object : region.objects)
            if (object.id == id && !region.pixels.empty())
            {
                drawField(region, top, topLength, object.x, object.y, nonModifying);
                drawField(region, bottom, bottomLen, object.x, object.y + 1, nonModifying);
            }
}

// The page as it stands: its regions, each in the colours of its table, on the canvas.
void DvbSubDecoder::render()
{
    m_pgs.clearCanvas();
    uint32_t* canvas = m_pgs.pixels();
    const int canvasWidth = m_pgs.width();
    const int canvasHeight = m_pgs.height();
    for (const PageRegion& placed : m_page)
    {
        const auto found = m_regions.find(placed.id);
        if (found == m_regions.end() || found->second.pixels.empty())
            continue;
        const Region& region = found->second;
        const Clut& c = clut(region.clutId);
        const uint32_t* table = region.depth == 2 ? c.entries2 : region.depth == 4 ? c.entries4 : c.entries8;
        for (int y = 0; y < region.height; ++y)
        {
            const int cy = m_windowY + placed.y + y;
            if (cy < 0 || cy >= canvasHeight)
                continue;
            for (int x = 0; x < region.width; ++x)
            {
                const int cx = m_windowX + placed.x + x;
                if (cx >= 0 && cx < canvasWidth)
                    canvas[static_cast<size_t>(cy) * canvasWidth + cx] =
                        table[region.pixels[static_cast<size_t>(y) * region.width + x]];
            }
        }
    }
}

void DvbSubDecoder::decode(const uint8_t* data, const int size, const int64_t pts, const int64_t duration,
                           std::vector<uint8_t>& out)
{
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    // The PES framing, the data identifier and the stream id, where a writer left them in.
    if (size >= 3 && p[0] == 0x20 && p[1] == 0x00 && p[2] == 0x0F)
        p += 2;
    bool page = false;
    while (end - p >= 6 && p[0] == 0x0F)
    {
        const int type = p[1];
        const int pageId = AV_RB16(p + 2);
        const int len = AV_RB16(p + 4);
        const uint8_t* seg = p + 6;
        if (len > end - seg)
        {
            if (!m_warnedDamage)
                LTRACE(LT_WARN, 2, "Warning: a damaged DVB subtitle display set was cut short.");
            m_warnedDamage = true;
            break;
        }
        if (m_compositionPage < 0 || pageId == m_compositionPage || pageId == m_ancillaryPage)
        {
            switch (type)
            {
            case 0x14:
                displayDefinition(seg, len);
                break;
            case 0x10:
                pageComposition(seg, len);
                page = true;
                break;
            case 0x11:
                regionComposition(seg, len);
                break;
            case 0x12:
                clutDefinition(seg, len);
                break;
            case 0x13:
                objectData(seg, len);
                break;
            default:  // the end of the display set, and what this reader does not use
                break;
            }
        }
        p = seg + len;
    }
    if (!m_canvasSet)
    {
        // Without a display definition, 720x576 (EN 300 743, 7.2.1). Without a video track in the file, 29.97 for
        // 480 lines and 25 for any other size.
        setUp(m_displayWidth, m_displayHeight, m_videoFps, m_displayHeight == 480 ? 30000.0 / 1001 : 25.0);
        m_canvasSet = true;
    }
    // A new page instance is shown when the display set has a page composition. It ends after the block's duration,
    // or, for a block without one, after the page's time-out.
    if (!page)
        return;
    render();
    int64_t endTime = -1;
    if (duration > 0)
        endTime = pts + duration;
    else if (m_pageTimeOut > 0)
        endTime = pts + m_pageTimeOut * INTERNAL_PTS_FREQ;
    m_pgs.addPicture(pts, endTime, false, out);
}
