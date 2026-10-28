#include "textSubtitles.h"

#ifdef _WIN32
// #include <winerror.h>

#include "osdep/textSubtitlesRenderWin32.h"
#ifdef WIN32_DEBUG_FREETYPE
#include "osdep/textSubtitlesRenderFT.h"
#endif
#else
#include "osdep/textSubtitlesRenderFT.h"
#endif

#include <algorithm>
#include <cassert>
#include <cmath>

#include <fs/systemlog.h>
#include "bitmapSubtitles.h"
#include "pgsStreamReader.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;
using namespace text_subtitles;

static constexpr float FLOAT_EPS = 1e-6f;
static constexpr int PG_BUFFER_SIZE = 1024 * 1024 * 2;

// const static int BORDER_WIDTH = 4;
// const RGBQUAD RESERVED_BACKGROUND_COLOR = {0x00, 0x00, 0x00, 0x00};

///////////////////////////////////////////
TextToPGSConverter::TextToPGSConverter(const bool sourceIsText)
    : /* TextSubtitlesRenderWin32(), */
      m_rleLen(0),
      m_composition_number(0),
      m_videoWidth(0),
      m_videoHeight(0),
      m_videoFps(0),
      m_imageBuffer()
{
    m_bottomOffset = 0;
    m_renderedData = nullptr;
    m_textRender = nullptr;
    palette_update_flag = false;
    m_paletteID = 0;
    m_paletteVersion = 0;
    m_pgsBuffer = new uint8_t[PG_BUFFER_SIZE];
    m_minLine = 0;
    m_maxLine = 0;
    if (sourceIsText)
    {
#ifdef _WIN32
#ifdef WIN32_DEBUG_FREETYPE
        m_textRender = new TextSubtitlesRenderFT;
#else
        m_textRender = new TextSubtitlesRenderWin32;
#endif
#else
        m_textRender = new TextSubtitlesRenderFT;
#endif
    }
}

TextToPGSConverter::~TextToPGSConverter()
{
    delete[] m_pgsBuffer;
    delete[] m_renderedData;
    delete m_textRender;
}

void TextToPGSConverter::enlargeCrop(const uint16_t width, const uint16_t height, const double fps, uint16_t* newWidth,
                                     uint16_t* newHeight)
{
    *newWidth = width;
    *newHeight = height;
    if (width > 1280)
    {
        *newWidth = 1920;
        *newHeight = 1080;
    }
    else if (width > 720)
    {
        *newWidth = 1280;
        *newHeight = 720;
    }
    else if (fabs(fps - 25.0) < 0.1 || fabs(fps - 50.0) < 0.1)
    {
        *newWidth = 720;
        *newHeight = 576;
    }
    else
    {
        *newWidth = 720;
        *newHeight = 480;
    }
}

// The frame rate decides the size for a width of 720 or less. It was stored only after the size had been chosen, so
// that the size was chosen at a rate of 0: video of 720x576 at 25 or 50 fps had its subtitles drawn for 720x480.
void TextToPGSConverter::setVideoInfo(const uint16_t width, const uint16_t height, const double fps)
{
    enlargeCrop(width, height, fps, &m_videoWidth, &m_videoHeight);
    // ** THE TIMES ARE ON THE FRAMES OF THE VIDEO. ** fps=23.976 was taken as written, and every time was put on a grid
    // of 1/23.976 s, while the video runs at 24000/1001: the grid left its frames by a tick every 267 frames, 324 ticks
    // (0.09 of a frame) an hour, so that a subtitle came later than its frame (29.97 and 59.94 alike). A standard rate
    // is taken at its exact value, as the bitmap path takes it (nearestPgsFrameRate); any other rate as given.
    const double exactFps = nearestPgsFrameRate(fps);
    m_videoFps = exactFps != 0.0 ? exactFps : fps;
    m_bt709 = m_videoHeight > 576;
    if (m_textRender)
        m_textRender->setRenderSize(m_videoWidth, m_videoHeight);
    delete[] m_renderedData;
    const int len = (m_videoWidth + 16) * m_videoHeight;
    m_renderedData = new uint8_t[len];
}

void TextToPGSConverter::setCanvas(const uint16_t width, const uint16_t height, const double fps)
{
    m_videoWidth = width;
    m_videoHeight = height;
    m_videoFps = fps;
    m_bt709 = m_videoHeight > 576;
    delete[] m_renderedData;
    m_renderedData = new uint8_t[(m_videoWidth + 16) * m_videoHeight];
}

double TextToPGSConverter::alignToGrid(const double value) const
{
    // How many frames have passed until this moment in time, rounded to the nearest frame, a time
    // exactly half way going to the later one. A subtitle time is a whole number of milliseconds
    // and cannot be held exactly in a double: 2.26 s is stored a hair below 2.26, so at 25 fps the
    // exact half way point 56.5 came out as 56.4999... and went to the EARLIER frame. The millionth
    // of a frame added here only decides such ties. It never moves any other millisecond time:
    // at the NTSC rates their fractions stay at least 1/2002 of a frame away from a half.
    const double frameCnt = round(value * m_videoFps + 1e-6);
    return frameCnt / m_videoFps;
}

// The renderers draw with premultiplied alpha: a colour's red, green and blue are already scaled by its alpha. A
// palette entry holds the colour and its alpha apart, so half-transparent white 0x80FFFFFF was written as grey (Y 126
// at alpha 128) and every antialiased edge was darker than its colour. Drawn text is turned back into colour and
// alpha; a picture from a bitmap subtitle holds them apart already.
uint32_t TextToPGSConverter::straightColor(const uint32_t premultiplied) const
{
    const uint32_t a = premultiplied >> 24;
    if (m_textRender == nullptr || a == 0 || a == 255)
        return premultiplied;
    uint32_t rez = premultiplied & 0xff000000;
    for (int shift = 0; shift < 24; shift += 8)
        rez |= (std::min)(255u, ((premultiplied >> shift & 0xff) * 255 + a / 2) / a) << shift;
    return rez;
}

// The colour a pixel is packed with. colorMask sets the lowest bits of each channel to merge colours when a picture
// has too many, so that each colour stands for those that differ from it only in these bits. The alphas that are
// merged with alpha 0 stand for nothing drawn: such a pixel is fully transparent. It was given the alpha of the mask,
// so that a subtitle with many colours lay on a white box of alpha 1 of 255 or more, as wide as the picture.
static uint32_t maskedColor(const uint32_t color, const uint32_t colorMask)
{
    return ((color >> 24) & ~(colorMask >> 24)) ? color | colorMask : 0;
}

uint8_t TextToPGSConverter::color32To8(const uint32_t* buff, const uint32_t colorMask)
{
    YUVQuad yuv = RGBAToYUVA(straightColor(maskedColor(*buff, colorMask)), m_bt709);
    const auto itr = m_paletteYUV.find(yuv);
    if (itr == m_paletteYUV.end())
    {
        if (m_paletteYUV.size() < 255)
        {
            const pair<map<YUVQuad, uint8_t>::iterator, bool> rez =
                m_paletteYUV.insert(std::make_pair(yuv, static_cast<uint8_t>(m_paletteYUV.size())));
            return rez.first->second;
        }
        // find nearest color
        // if (m_paletteYUV.size() >= 256)
        THROW(ERR_COMMON, "Can't transform image to YUV: too many colors are used.")
        // return m_paletteYUV[yuv];
    }
    return itr->second;
}

int TextToPGSConverter::getRepeatCnt(const uint32_t* pos, const uint32_t* end, const uint32_t colorMask) const
{
    int rez = 1;
    if (maskedColor(*pos, colorMask) == 0)
    {
        for (const uint32_t* cur = pos + 1; cur < end; cur++)
        {
            if (maskedColor(*cur, colorMask) == 0)
                rez++;
            else
                break;
        }
    }
    else
    {
        const uint32_t rgbColor = *pos | colorMask;
        const YUVQuad color = RGBAToYUVA(straightColor(rgbColor), m_bt709);
        for (const uint32_t* cur = pos + 1; cur < end && maskedColor(*cur, colorMask) != 0; cur++)
        {
            const uint32_t newRGBColor = *cur | colorMask;
            if (newRGBColor == rgbColor || RGBAToYUVA(straightColor(newRGBColor), m_bt709) == color)
                rez++;
            else
                break;
        }
    }
    return rez;
}

// Studio range (Y 16-235, Cb and Cr 16-240), in 1/256: BT.601 with Kr = 0.299, Kb = 0.114, BT.709 with Kr = 0.2126,
// Kb = 0.0722
YUVQuad TextToPGSConverter::RGBAToYUVA(uint32_t data, const bool bt709)
{
    const auto rgba = reinterpret_cast<RGBQUAD*>(&data);
    YUVQuad rez;
    if (bt709)
    {
        rez.Y = static_cast<uint8_t>(
            lround(46.742 * rgba->rgbRed + 157.243 * rgba->rgbGreen + 15.874 * rgba->rgbBlue + 4224.0) >> 8);
        rez.Cr = static_cast<uint8_t>(
            lround(112.439 * rgba->rgbRed - 102.129 * rgba->rgbGreen - 10.310 * rgba->rgbBlue + 32896.0) >> 8);
        rez.Cb = static_cast<uint8_t>(
            lround(-25.765 * rgba->rgbRed - 86.674 * rgba->rgbGreen + 112.439 * rgba->rgbBlue + 32896.0) >> 8);
        rez.alpha = rgba->rgbReserved;
        return rez;
    }
    rez.Y = static_cast<uint8_t>(
        lround(65.738 * rgba->rgbRed + 129.057 * rgba->rgbGreen + 25.064 * rgba->rgbBlue + 4224.0) >> 8);
    rez.Cr = static_cast<uint8_t>(
        lround(112.439 * rgba->rgbRed - 94.154 * rgba->rgbGreen - 18.285 * rgba->rgbBlue + 32896.0) >> 8);
    rez.Cb = static_cast<uint8_t>(
        lround(-37.945 * rgba->rgbRed - 74.494 * rgba->rgbGreen + 112.439 * rgba->rgbBlue + 32896.0) >> 8);
    rez.alpha = rgba->rgbReserved;
    return rez;
}

RGBQUAD TextToPGSConverter::YUVAToRGBA(const YUVQuad& yuv, const bool bt709)
{
    RGBQUAD rez;
    if (bt709)
    {
        int c = lround(298.082 * yuv.Y + 540.775 * yuv.Cb - 73860.497) >> 8;
        rez.rgbBlue = static_cast<uint8_t>(FFMAX(FFMIN(c, 255L), 0));
        c = lround(298.082 * yuv.Y - 136.425 * yuv.Cr - 54.592 * yuv.Cb + 19808.788) >> 8;
        rez.rgbGreen = static_cast<uint8_t>(FFMAX(FFMIN(c, 255L), 0));
        c = lround(298.082 * yuv.Y + 458.942 * yuv.Cr - 63385.855) >> 8;
        rez.rgbRed = static_cast<uint8_t>(FFMAX(FFMIN(c, 255L), 0));
        rez.rgbReserved = yuv.alpha;
        return rez;
    }
    int tmp = lround(298.082 * yuv.Y + 516.412 * yuv.Cb - 70742.016) >> 8;
    rez.rgbBlue = static_cast<uint8_t>(FFMAX(FFMIN(tmp, 255L), 0));
    tmp = lround(298.082 * yuv.Y - 208.120 * yuv.Cr - 100.291 * yuv.Cb + 34835.456) >> 8;
    rez.rgbGreen = static_cast<uint8_t>(FFMAX(FFMIN(tmp, 255L), 0));
    tmp = lround(298.082 * yuv.Y + 408.583 * yuv.Cr - 56939.776) >> 8;
    rez.rgbRed = static_cast<uint8_t>(FFMAX(FFMIN(tmp, 255L), 0));
    rez.rgbReserved = yuv.alpha;
    return rez;
}

void TextToPGSConverter::reduceColors(uint8_t mask) const
{
    mask = ~mask;
    const uint32_t val = (mask << 24) + (mask << 16) + (mask << 8) + mask;
    auto dst = reinterpret_cast<uint32_t*>(m_textRender ? m_textRender->m_pData : m_imageBuffer);
    const int len = m_videoWidth * m_videoHeight;
    const uint32_t* end = dst + len;
    for (; dst < end; ++dst) *dst &= val;
}

bool TextToPGSConverter::rlePack(const uint32_t colorMask)
{
    try
    {
        m_paletteYUV.clear();
        const auto srcData = reinterpret_cast<uint32_t*>(m_textRender ? m_textRender->m_pData : m_imageBuffer);
        assert(srcData);
        m_rleLen = 0;
        m_objectLeft = 0;
        m_objectWidth = m_videoWidth;
        packDrawnBox(srcData, colorMask);
        sortPalette();
        return true;
    }
    catch (VodCoreException&)
    {
        return false;
    }
}

// One run of pixels of one colour, packed as PGS packs it
uint8_t* TextToPGSConverter::packRun(uint8_t* dst, const uint32_t* src, const int repCnt, const uint32_t colorMask)
{
    const uint8_t srcColor = color32To8(src, colorMask);
    assert(repCnt < 16384);
    if (srcColor)  // color exists
    {
        if (repCnt > 2)
        {
            *dst++ = 0;
            if (repCnt <= 63)
                *dst++ = static_cast<uint8_t>(0x80 + repCnt);
            else
            {
                *dst++ = static_cast<uint8_t>(0xc0 + (repCnt >> 8));
                *dst++ = repCnt & 0xff;
            }
            *dst++ = srcColor;
        }
        else
        {  // simple color
            *dst++ = srcColor;
            if (repCnt == 2)
                *dst++ = srcColor;
        }
    }
    else  // zero color
    {
        *dst++ = 0;
        if (repCnt <= 63)
            *dst++ = repCnt & 0xff;
        else
        {
            *dst++ = static_cast<uint8_t>(0x40 | repCnt >> 8);
            *dst++ = repCnt & 0xff;
        }
    }
    return dst;
}

// ** A SUBTITLE PICTURE IS PACKED ONLY WHERE IT IS DRAWN. ** The object, and the window it is shown in, are the box
// around the pixels that are not fully transparent, at the columns they are drawn at. They were as wide as the picture,
// even for one word: for drawn text, for DVD, DVB and ASS subtitles, and for PGS drawn again at another size. A box
// narrower than 8 pixels, the least an object may have, is widened inside the picture. Transparent is colour 0,
// whether or not the box begins with it.
void TextToPGSConverter::packDrawnBox(const uint32_t* picture, const uint32_t colorMask)
{
    int left = m_videoWidth;
    int right = -1;
    int top = -1;
    int bottom = -1;
    for (int y = 0; y < m_videoHeight; ++y)
    {
        const uint32_t* line = picture + static_cast<size_t>(y) * m_videoWidth;
        int first = 0;
        while (first < m_videoWidth && maskedColor(line[first], colorMask) == 0) ++first;
        if (first == m_videoWidth)
            continue;
        int last = m_videoWidth - 1;
        while (maskedColor(line[last], colorMask) == 0) --last;
        left = FFMIN(left, first);
        right = FFMAX(right, last);
        if (top < 0)
            top = y;
        bottom = y;
    }
    m_minLine = m_maxLine = 0;
    if (bottom < 0)
        return;  // nothing is drawn: m_rleLen stays 0
    if (right - left + 1 < 8)
    {
        right = FFMIN(m_videoWidth - 1, left + 7);
        left = FFMAX(0, right - 7);
    }
    const int width = right - left + 1;

    m_paletteYUV.emplace(RGBAToYUVA(0, m_bt709), static_cast<uint8_t>(0));
    uint8_t* curPtr = m_renderedData;
    const uint8_t* bufEnd = m_renderedData + static_cast<size_t>(m_videoWidth + 16) * m_videoHeight;
    for (int y = top; y <= bottom; ++y)
    {
        const uint32_t* src = picture + static_cast<size_t>(y) * m_videoWidth + left;
        const uint32_t* lineEnd = src + width;
        while (src < lineEnd)
        {
            // a run takes at most 4 bytes, and the end of the line 2 more
            if (bufEnd - curPtr < 6)
                THROW(ERR_COMMON, "Not enough RLE buffer for encoding picture")
            const int repCnt = getRepeatCnt(src, lineEnd, colorMask);
            curPtr = packRun(curPtr, src, repCnt, colorMask);
            src += repCnt;
        }
        *curPtr++ = 0;  // end of line signal
        *curPtr++ = 0;  // end of line signal
    }
    m_minLine = static_cast<uint16_t>(top);
    m_maxLine = static_cast<uint16_t>(bottom);
    m_objectLeft = static_cast<uint16_t>(left);
    m_objectWidth = static_cast<uint16_t>(width);
    m_rleLen = static_cast<int>(curPtr - m_renderedData);
}

// Lines of nothing but transparent pixels above and below the packed picture, for an object of fewer lines than PGS
// allows. Colour 0 is transparent, as packDrawnBox has it.
void TextToPGSConverter::padLines(const int above, const int below)
{
    uint8_t line[5];
    int len = 0;
    line[len++] = 0;
    if (m_objectWidth <= 63)
        line[len++] = static_cast<uint8_t>(m_objectWidth);
    else
    {
        line[len++] = static_cast<uint8_t>(0x40 | m_objectWidth >> 8);
        line[len++] = m_objectWidth & 0xff;
    }
    line[len++] = 0;  // end of line signal
    line[len++] = 0;  // end of line signal
    memmove(m_renderedData + above * len, m_renderedData, m_rleLen);
    for (int i = 0; i < above; ++i) memcpy(m_renderedData + i * len, line, len);
    uint8_t* end = m_renderedData + above * len + m_rleLen;
    for (int i = 0; i < below; ++i) memcpy(end + i * len, line, len);
    m_rleLen += (above + below) * len;
}

// An object of fewer than 8 lines, the least PGS allows, gets transparent lines above the drawn lines, or below them at
// the top of the picture. drawnTop is the line the drawn lines are shown at; the object's first line is returned, and
// it has (std::max)(8, renderedHeight()) lines. Where nothing is drawn, the object is 8 lines of nothing.
uint16_t TextToPGSConverter::padToLeastHeight(const int drawnTop)
{
    const int drawnHeight = m_rleLen ? renderedHeight() : 0;
    const int padHeight = FFMAX(0, 8 - drawnHeight);
    const int padAbove = FFMIN(padHeight, drawnTop);
    padLines(padAbove, padHeight - padAbove);
    return static_cast<uint16_t>(drawnTop - padAbove);
}

void TextToPGSConverter::sortPalette()
{
    // sort by colors indexes
    m_paletteByColor.clear();
    for (auto [fst, snd] : m_paletteYUV) m_paletteByColor.insert(std::make_pair(snd, fst));
    assert(m_paletteByColor.size() == m_paletteYUV.size());
}

uint16_t TextToPGSConverter::renderedHeight() const { return m_maxLine - m_minLine + 1; }

uint16_t TextToPGSConverter::maxLine() const { return m_maxLine; }

uint16_t TextToPGSConverter::minLine() const { return m_minLine; }

TextToPGSConverter::Palette TextToPGSConverter::buildPalette(const float opacity)
{
    if (opacity == 1.0)
        return m_paletteByColor;
    Palette result = m_paletteByColor;
    for (auto& [fst, snd] : result) snd.alpha = static_cast<uint8_t>(FFMIN(255L, lround(snd.alpha * opacity)));
    return result;
}

float toCurve(const float value)
{
    // float result = pow(value, 1.5f);
    const float result = value * sqrt(value);  // same as pow 1.5, reduce binary size
    return result;
}

int64_t TextToPGSConverter::objectDecodeTime(const double pixels)
{
    return static_cast<int64_t>(ceil(90000.0 * pixels / PIXEL_DECODING_RATE));
}

int64_t TextToPGSConverter::windowDrawTime(const double pixels)
{
    return static_cast<int64_t>(ceil(90000.0 * pixels / PIXEL_COMPOSITION_RATE));
}

int64_t TextToPGSConverter::longestDecodeLead() const
{
    const double picture = static_cast<double>(m_videoWidth) * m_videoHeight;
    return FFMAX(windowDrawTime(picture), objectDecodeTime(picture)) + windowDrawTime(picture);
}

// The segments of PGS data as writePGHeader writes them: "PG", the presentation time, the decoding time (0 where it is
// the presentation time), the segment type, its length and its data
int64_t TextToPGSConverter::firstDecodingTime(uint8_t* data, const uint32_t len)
{
    if (len < 10)
        return 0;
    const uint32_t dts = AV_RB32(data + 6);
    return dts != 0 ? dts : AV_RB32(data + 2);
}

void TextToPGSConverter::capDecodingTimes(uint8_t* data, const uint32_t len, const int64_t latestDts)
{
    const auto latest = static_cast<uint32_t>(latestDts);
    uint32_t from = 0;  // the decoding time of the display set the segment belongs to
    uint32_t to = 0;
    for (uint32_t pos = 0; pos + 13 <= len; pos += 13 + AV_RB16(data + pos + 11))
    {
        uint8_t* seg = data + pos;
        const uint32_t pts = AV_RB32(seg + 2);
        const uint32_t dts = AV_RB32(seg + 6) != 0 ? AV_RB32(seg + 6) : pts;
        if (seg[10] == PCS_DEF_SEGMENT)
        {
            from = dts;
            to = static_cast<int32_t>(dts - latest) > 0 ? latest : dts;
        }
        if (from == to)
            continue;
        const uint32_t newPts = pts == from ? to : pts;
        const uint32_t newDts = dts == from ? to : dts;
        AV_WB32(seg + 2, newPts);
        AV_WB32(seg + 6, newDts != newPts ? newDts : 0);
    }
}

uint8_t* TextToPGSConverter::doClear(const double atTimeSec, uint32_t& dstBufSize, const int64_t latestDts)
{
    // The same three segments that end a text in doConvert, for a text whose end could not be
    // written with it because the composition meant to replace it turned out to draw nothing
    uint8_t* curPos = m_pgsBuffer;
    palette_update_flag = false;
    const int64_t pts = llround(alignToGrid(atTimeSec) * 90000.0);
    const int64_t dts = FFMIN(pts - m_lastWindowDrawTime - 1, latestDts);
    m_displaySets.assign(1, {0, dts});
    curPos += composePresentationSegment(curPos, CompositionMode::Finish, pts, dts, m_lastWindowLeft, m_lastWindowTop,
                                         true, false);
    curPos += composeWindowDefinition(curPos, pts - m_lastWindowDrawTime, dts, m_lastWindowLeft, m_lastWindowTop,
                                      m_lastWindowWidth, m_lastWindowHeight);
    curPos += composeEnd(curPos, dts, dts);
    dstBufSize = static_cast<uint32_t>(curPos - m_pgsBuffer);
    return m_pgsBuffer;
}

uint8_t* TextToPGSConverter::doConvert(const std::string& text, const TextAnimation& animation, double inTimeSec,
                                       double outTimeSec, uint32_t& dstBufSize, const bool hide)
{
    const bool forced = m_textRender->rasterText(text);
    inTimeSec = alignToGrid(inTimeSec);
    outTimeSec = alignToGrid(outTimeSec);

    const double inTimePTS = inTimeSec * 90000.0;
    const double outTimePTS = outTimeSec * 90000.0;

    uint32_t mask = 0;
    int step = 0;
    while (!rlePack(mask))
    {
        // reduce colors
        const auto tmp = reinterpret_cast<uint8_t*>(&mask);
        const int idx = step++ % 4;
        tmp[idx] <<= 1;
        tmp[idx]++;
    }

    if (m_rleLen == 0)
        return nullptr;  // empty text

    // The drawn lines are placed bottom-offset lines above the bottom of the picture, and inside it: a negative offset
    // put them partly below it. An object of fewer than 8 lines, the least PGS allows, gets transparent lines above
    // them, or below them at the top of the picture. The columns are those packDrawnBox kept.
    const int drawnHeight = renderedHeight();
    const int drawnTop =
        FFMAX(0, FFMIN(m_textRender->m_height - drawnHeight - m_bottomOffset, m_textRender->m_height - drawnHeight));
    const uint16_t objectWindowTop = padToLeastHeight(drawnTop);
    const auto objectWindowHeight = static_cast<uint16_t>(FFMAX(8, drawnHeight));
    const uint16_t objectWindowLeft = m_objectLeft;
    const uint16_t objectWindowWidth = m_objectWidth;

    int fadeInFrames = lround(animation.fadeInDuration * m_videoFps);
    int fadeOutFrames = lround(animation.fadeOutDuration * m_videoFps);
    fadeInFrames++;
    fadeOutFrames++;

    // ** FADES LONGER THAN THE SUBTITLE ARE FITTED INTO IT. ** A fade-in longer than the subtitle ended with the
    // subtitle at the opacity it had reached (none to see, for a fade of minutes on a subtitle of a second), and with
    // both fades the fade-in took all the time, so the text left at its peak without a fade-out. When the two do not
    // fit into the frames the subtitle is shown for, those frames are shared between them in the ratio of their
    // lengths: the text still reaches full opacity, and then fades out to nothing.
    {
        const double framePts = 90000.0 / m_videoFps;
        const double firstStep = alignToGrid(inTimePTS + framePts);
        const double lastStep = alignToGrid(outTimePTS - framePts);
        const int shown =
            lastStep + FLOAT_EPS >= firstStep ? static_cast<int>((lastStep - firstStep) / framePts + 1.5) : 0;
        const int64_t wantIn = fadeInFrames - 1;
        const int64_t wantOut = hide ? fadeOutFrames - 1 : 0;
        if (shown > 0 && wantIn + wantOut > shown)
        {
            const int inSteps = wantIn > 0 ? FFMAX(1, static_cast<int>(shown * wantIn / (wantIn + wantOut))) : 0;
            const int outSteps = wantOut > 0 ? FFMAX(0, shown - inSteps) : 0;
            if (wantIn > inSteps)
                fadeInFrames = inSteps + 1;
            if (wantOut > outSteps)
                fadeOutFrames = outSteps + 1;
        }
    }

    float opacityInDelta = 1.0F / static_cast<float>(fadeInFrames);
    float opacityOutDelta = 1.0F / static_cast<float>(fadeOutFrames);
    float opacity = opacityInDelta;

    // ** THE TIMES OF THE PGS DECODER MODEL. ** The plane is cleared while the object is decoded, then the window is
    // drawn: the composition is decoded that long before it is shown, and the window is drawn in the time before it. A
    // display set without an object (a fade step, or the clear at the end) takes the drawing of the window and one
    // tick, and ends with its decoding. Each time is rounded up on its own. They were rounded up and then to the
    // nearest tick, so that a time without a fraction (297 ticks for a window of 1920x55) came out a tick longer and
    // the window began before the composition allowed; the end of a display set without an object came at 90 ticks
    // before its presentation, after the decoding time.
    const double objectPixels = static_cast<double>(objectWindowWidth) * objectWindowHeight;
    const int64_t decodeTime = objectDecodeTime(objectPixels);
    const int64_t drawTime = windowDrawTime(objectPixels);
    const int64_t decodeDuration =
        FFMAX(windowDrawTime(static_cast<double>(m_videoWidth) * m_videoHeight), decodeTime) + drawTime;
    m_lastWindowLeft = objectWindowLeft;
    m_lastWindowTop = objectWindowTop;
    m_lastWindowWidth = objectWindowWidth;
    m_lastWindowHeight = objectWindowHeight;
    m_lastWindowDrawTime = drawTime;

    // 1. show text
    uint8_t* curPos = m_pgsBuffer;
    palette_update_flag = false;
    m_paletteID = 0;
    m_paletteVersion = 0;
    int64_t pts = llround(inTimePTS);
    int64_t dts = pts - decodeDuration;
    m_displaySets.assign(1, {0, dts});

    curPos += composePresentationSegment(curPos, CompositionMode::Start, pts, dts, objectWindowLeft, objectWindowTop,
                                         true, forced);
    curPos += composeWindowDefinition(curPos, pts - drawTime, dts, objectWindowLeft, objectWindowTop, objectWindowWidth,
                                      objectWindowHeight);
    curPos += composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, dts, dts);
    const int64_t odfPTS = dts + decodeTime;
    curPos += composeObjectDefinition(curPos, odfPTS, dts, 0, objectWindowHeight - 1, true);
    curPos += composeEnd(curPos, odfPTS, odfPTS);

    // Each fade step is a presentation segment, a palette of up to 256 colours and an end: under 1.4 KB. A fade of
    // minutes on a long subtitle, at 59.94 fps, wrote past the end of the buffer (only a debug check looked). The fade
    // now stops where the next step would not fit, with room left for hiding the text. The size of one step is
    // measured by writing one into the free space, which is then written over: the steps are counted with it.
    static constexpr ptrdiff_t FADE_STEP_MAX = 2048;
    static constexpr ptrdiff_t HIDE_RESERVE = 1024;
    ptrdiff_t stepBytes = FADE_STEP_MAX;
    if (curPos - m_pgsBuffer + FADE_STEP_MAX <= PG_BUFFER_SIZE - HIDE_RESERVE)
    {
        const auto savedNumber = m_composition_number;
        const bool savedFlag = palette_update_flag;
        palette_update_flag = true;
        uint8_t* p = curPos;
        p += composePresentationSegment(p, CompositionMode::Update, pts, dts, objectWindowLeft, objectWindowTop, true,
                                        forced);
        p += composePaletteDefinition(buildPalette(toCurve(1.0F)), p, dts, dts);
        p += composeEnd(p, dts, dts);
        m_composition_number = savedNumber;
        palette_update_flag = savedFlag;
        if (p - curPos > 0 && p - curPos <= FADE_STEP_MAX)
            stepBytes = p - curPos;
    }
    const auto stepFits = [&]
    {
        if (curPos - m_pgsBuffer + stepBytes <= PG_BUFFER_SIZE - HIDE_RESERVE)
            return true;
        if (!m_fadeCutSaid)
            LTRACE(LT_WARN, 2, "Warning: a fade of a subtitle is too long to be drawn in full; it is shortened.");
        m_fadeCutSaid = true;
        return false;
    };

    // 2.1 fade in palette
    const double fpsPts = 90000.0 / m_videoFps;
    double updateTime = alignToGrid(inTimePTS + fpsPts);

    const double lastAnimateTime = alignToGrid(outTimePTS - fpsPts);

    // ** A FADE THAT DOES NOT FIT IS SHORTENED, NOT CUT OFF. ** The loops below stopped where the next step did not
    // fit, so a long fade-in left the text at the opacity it had reached (alpha 5 of 255 for a fade of minutes) and a
    // long fade-out never reached nothing. The steps that fit are counted first and shared between the two fades;
    // each fade then takes fewer, larger steps, so it still ends at full opacity, or at none.
    {
        const ptrdiff_t room = PG_BUFFER_SIZE - HIDE_RESERVE - (curPos - m_pgsBuffer);
        const int budget = room > 0 ? static_cast<int>(room / stepBytes) : 0;
        const int framesShown = lastAnimateTime + FLOAT_EPS >= updateTime
                                    ? static_cast<int>((lastAnimateTime - updateTime) / fpsPts + 1.5)
                                    : 0;
        const int wantIn = FFMIN(fadeInFrames - 1, framesShown);
        const int wantOut = hide ? FFMIN(fadeOutFrames - 1, framesShown) : 0;
        if (wantIn + wantOut > budget)
        {
            if (!m_fadeCutSaid)
                LTRACE(LT_WARN, 2, "Warning: a fade of a subtitle is too long to be drawn in full; it is shortened.");
            m_fadeCutSaid = true;
            const int inSteps =
                wantIn > 0 ? FFMAX(1, static_cast<int>(static_cast<int64_t>(budget) * wantIn / (wantIn + wantOut))) : 0;
            const int outSteps = FFMAX(0, budget - inSteps);
            if (wantIn > inSteps)
            {
                fadeInFrames = inSteps + 1;
                opacityInDelta = 1.0F / static_cast<float>(fadeInFrames);
            }
            if (wantOut > outSteps)
            {
                fadeOutFrames = outSteps + 1;
                opacityOutDelta = 1.0F / static_cast<float>(fadeOutFrames);
            }
        }
    }
    opacity += opacityInDelta;
    while (updateTime <= lastAnimateTime + FLOAT_EPS && opacity <= 1.0 + FLOAT_EPS && stepFits())
    {
        palette_update_flag = true;
        m_paletteVersion++;
        pts = llround(updateTime);
        dts = pts - drawTime - 1;
        m_displaySets.emplace_back(static_cast<uint32_t>(curPos - m_pgsBuffer), dts);
        curPos += composePresentationSegment(curPos, CompositionMode::Update, pts, dts, objectWindowLeft,
                                             objectWindowTop, true, forced);
        curPos += composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, dts, dts);
        curPos += composeEnd(curPos, dts, dts);

        updateTime = alignToGrid(updateTime + fpsPts);
        opacity += opacityInDelta;
    }

    // 2.2 fade out palette, only where the text then leaves the screen: text that is replaced at
    //     outTimeSec by the next composition must stay fully visible until then
    updateTime = alignToGrid(FFMAX(updateTime, outTimePTS - (fadeOutFrames - 1) * fpsPts));
    opacity = 1.0f - opacityOutDelta;
    while (hide && updateTime < outTimePTS - FLOAT_EPS && stepFits())
    {
        palette_update_flag = true;
        m_paletteVersion++;
        pts = llround(updateTime);
        dts = pts - drawTime - 1;
        m_displaySets.emplace_back(static_cast<uint32_t>(curPos - m_pgsBuffer), dts);
        curPos += composePresentationSegment(curPos, CompositionMode::Update, pts, dts, objectWindowLeft,
                                             objectWindowTop, true, forced);
        curPos += composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, dts, dts);
        curPos += composeEnd(curPos, dts, dts);

        updateTime = alignToGrid(updateTime + fpsPts);
        opacity -= opacityOutDelta;
    }

    // 3. hide text, unless the caller replaces it at outTimeSec with a new composition: then the
    //    screen goes straight from this text to the next, with nothing empty in between
    palette_update_flag = false;
    if (hide)
    {
        pts = llround(outTimePTS);
        dts = pts - drawTime - 1;
        m_displaySets.emplace_back(static_cast<uint32_t>(curPos - m_pgsBuffer), dts);
        curPos += composePresentationSegment(curPos, CompositionMode::Finish, pts, dts, objectWindowLeft,
                                             objectWindowTop, true, false);
        curPos += composeWindowDefinition(curPos, pts - drawTime, dts, objectWindowLeft, objectWindowTop,
                                          objectWindowWidth, objectWindowHeight);
        curPos += composeEnd(curPos, dts, dts);
    }

    assert(curPos - m_pgsBuffer < PG_BUFFER_SIZE);

    dstBufSize = static_cast<uint32_t>(curPos - m_pgsBuffer);
    return m_pgsBuffer;
}

long TextToPGSConverter::composePresentationSegment(uint8_t* buff, const CompositionMode mode, const int64_t pts,
                                                    const int64_t dts, const uint16_t left, const uint16_t top,
                                                    const bool needPgHeader, const bool forced)
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = PCS_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;

    curPos += composeVideoDescriptor(curPos);
    curPos += composeCompositionDescriptor(curPos, m_composition_number++,
                                           mode == CompositionMode::Start ? EPOTH_START : EPOTH_NORMAL);
    *curPos++ = static_cast<uint8_t>(palette_update_flag << 7);  // palette_update_flag = 0 and 7 reserved bits
    *curPos++ = m_paletteID;                                     // paletteID ref
    *curPos++ = mode != CompositionMode::Finish ? 1 : 0;         // number_of_composition_objects
    // composition object
    if (mode != CompositionMode::Finish)
    {
        *curPos++ = 0;  // objectID ref
        *curPos++ = 0;  // objectID ref
        *curPos++ = 0;  // windowID ref
        if (forced)
            *curPos++ = 0x40;  // object_cropped_flag = false, forced_on_flag = true
        else
            *curPos++ = 0;  // object_cropped_flag = false, forced_on_flag = false
        AV_WB16(curPos, left);
        curPos += 2;  // object horizontal position
        AV_WB16(curPos, top);
        curPos += 2;  // object vertical position
    }

    AV_WB16(startPos - 2, static_cast<uint16_t>(curPos - startPos));
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composeVideoDescriptor(uint8_t* buff) const
{
    uint8_t* curPos = buff;
    AV_WB16(curPos, m_videoWidth);
    curPos += 2;
    AV_WB16(curPos, m_videoHeight);
    curPos += 2;
    *curPos++ = static_cast<uint8_t>(PGSStreamReader::calcFpsIndex(m_videoFps) << 4);
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composeWindowDefinition(uint8_t* buff, const int64_t pts, const int64_t dts,
                                                 const uint16_t left, const uint16_t top, const uint16_t width,
                                                 const uint16_t height, const bool needPgHeader) const
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = WINDOWS_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    *curPos++ = 1;  // number of windows
    curPos += composeWindow(curPos, left, top, width, height);
    AV_WB16(startPos - 2, static_cast<uint16_t>(curPos - startPos));
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composeCompositionDescriptor(uint8_t* buff, const uint16_t number, const uint8_t state)
{
    uint8_t* curPos = buff;
    AV_WB16(curPos, number);
    curPos += 2;
    *curPos++ = static_cast<uint8_t>(state << 6);
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composeWindow(uint8_t* buff, const uint16_t left, const uint16_t top, const uint16_t width,
                                       const uint16_t height)
{
    uint8_t* curPos = buff;
    *curPos++ = 0;  // window ID
    AV_WB16(curPos, left);
    curPos += 2;
    AV_WB16(curPos, top);
    curPos += 2;
    AV_WB16(curPos, width);
    curPos += 2;
    AV_WB16(curPos, height);
    curPos += 2;
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composePaletteDefinition(const Palette& palette, uint8_t* buff, const int64_t pts,
                                                  const int64_t dts, const bool needPgHeader) const
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = PALETTE_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    *curPos++ = m_paletteID;       // palette ID
    *curPos++ = m_paletteVersion;  // palette version number
    for (const auto [fst, snd] : palette)
    {
        *curPos++ = fst;
        *curPos++ = snd.Y;
        *curPos++ = snd.Cr;
        *curPos++ = snd.Cb;
        *curPos++ = snd.alpha;
    }
    AV_WB16(startPos - 2, static_cast<uint16_t>(curPos - startPos));  // correct length field
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::composeObjectDefinition(uint8_t* buff, const int64_t pts, const int64_t dts,
                                                 const int firstLine, const int lastLine, const bool needPgHeader) const
{
    std::vector<uint8_t*> seqPos;

    const uint8_t* srcData = m_renderedData;
    int srcProcessed = 0;
    int blocks = 0;
    uint8_t* curPos = buff;
    uint8_t* sizePos = buff;
    do
    {
        if (needPgHeader)
            curPos += writePGHeader(curPos, pts, dts);
        *curPos++ = OBJECT_DEF_SEGMENT;
        curPos += 2;  // skip length field
        uint8_t* fragmentStart = curPos;
        *curPos++ = 0;  // objectID
        *curPos++ = 0;  // objectID
        *curPos++ = 0;  // object version number
        seqPos.push_back(curPos);
        *curPos++ = 0;  // 0xc0; // sequence descriptor: first=true, last=true

        if (blocks == 0)
        {
            sizePos = curPos;
            // object data header
            curPos += 3;  // skip total size
            AV_WB16(curPos, m_objectWidth);
            curPos += 2;
            AV_WB16(curPos, static_cast<uint16_t>(lastLine - firstLine + 1));
            curPos += 2;
        }

        int MAX_PG_PACKET = 65515;
        if (blocks == 0)
            MAX_PG_PACKET -= 7;
        const int size = FFMIN(m_rleLen - srcProcessed, MAX_PG_PACKET);
        memcpy(curPos, srcData + srcProcessed, size);
        srcProcessed += size;
        curPos += size;

        AV_WB16(fragmentStart - 2, static_cast<uint16_t>(curPos - fragmentStart));  // correct length field
        blocks++;
    } while (srcProcessed < m_rleLen);
    AV_WB24(sizePos, m_rleLen + 4);  // object len
    if (!seqPos.empty())
    {
        *(seqPos[0]) |= 0x80;
        *(seqPos[seqPos.size() - 1]) |= 0x40;
    }
    return static_cast<long>(curPos - buff);
}

// ReSharper disable once CppMemberFunctionMayBeStatic
long TextToPGSConverter::composeEnd(uint8_t* buff, const int64_t pts, const int64_t dts, const bool needPgHeader)
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = END_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    AV_WB16(startPos - 2, static_cast<uint16_t>(curPos - startPos));
    return static_cast<long>(curPos - buff);
}

long TextToPGSConverter::writePGHeader(uint8_t* buff, const int64_t pts, int64_t dts)
{
    if (dts > pts)
        dts = pts;
    *buff++ = 'P';
    *buff++ = 'G';
    auto data = UnalignedPtr<uint32_t>(buff);
    *data++ = my_htonl(static_cast<uint32_t>(pts));
    if (dts != pts)
        *data = my_htonl(static_cast<uint32_t>(dts));
    else
        *data = 0;
    return 10;
}
