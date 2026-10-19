#include "textSubtitlesRenderWin32.h"

#include <gdiplus.h>
#include <cmath>

#include <fs/systemlog.h>

#include "../vodCoreException.h"
#include "../vod_common.h"
#include "types/types.h"

using namespace std;
using namespace Gdiplus;

namespace text_subtitles
{
#ifndef OLD_WIN32_RENDERER
class GdiPlusPriv
{
   public:
    GdiPlusPriv() { GdiplusStartup(&m_gdiplusToken, &m_gdiplusStartupInput, nullptr); }

    ~GdiPlusPriv() { GdiplusShutdown(m_gdiplusToken); }

    GdiplusStartupInput m_gdiplusStartupInput;
    ULONG_PTR m_gdiplusToken;
};
#endif

TextSubtitlesRenderWin32::TextSubtitlesRenderWin32() : m_hbmp(nullptr)
{
    m_hfont = nullptr;
#ifndef OLD_WIN32_RENDERER
    m_gdiPriv = new GdiPlusPriv();
#endif

    m_pbmpInfo = nullptr;
    m_hdcScreen = CreateDC(TEXT("DISPLAY"), nullptr, nullptr, nullptr);
    m_dc = CreateCompatibleDC(m_hdcScreen);
}

TextSubtitlesRenderWin32::~TextSubtitlesRenderWin32()
{
    delete[] m_pbmpInfo;
    if (m_hbmp)
        DeleteObject(m_hbmp);
    DeleteDC(m_dc);
    DeleteDC(m_hdcScreen);

#ifndef OLD_WIN32_RENDERER
    delete m_gdiPriv;
#endif
}

void TextSubtitlesRenderWin32::setRenderSize(const int width, const int height)
{
    delete[] m_pbmpInfo;
    m_width = width;
    m_height = height;
    m_pbmpInfo = reinterpret_cast<BITMAPINFO*>(new uint8_t[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)]);
    m_pbmpInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    m_pbmpInfo->bmiHeader.biWidth = m_width;
    m_pbmpInfo->bmiHeader.biHeight = -m_height;
    m_pbmpInfo->bmiHeader.biPlanes = 1;
    m_pbmpInfo->bmiHeader.biBitCount = 32;
    m_pbmpInfo->bmiHeader.biCompression = BI_RGB;
    m_pbmpInfo->bmiHeader.biClrUsed = 256 * 256 * 256;
    m_hbmp = CreateDIBSection(m_dc, m_pbmpInfo, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_pData), nullptr, 0);
    if (m_hbmp == nullptr)
        THROW(ERR_COMMON, "Can't initialize graphic subsystem for render text subtitles")
    SelectObject(m_dc, m_hbmp);
    SetBkColor(m_dc, RGB(0, 0, 0));
    SetBkMode(m_dc, TRANSPARENT);
}

void TextSubtitlesRenderWin32::setFont(const Font& font)
{
    m_font = font;
#ifdef OLD_WIN32_RENDERER
    if (m_hfont)
        ::DeleteObject(m_hfont);
    m_hfont = CreateFont(font.m_size,                                        // height of font
                         0,                                                  // average character width
                         0,                                                  // angle of escapement
                         0,                                                  // base-line orientation angle
                         (font.m_opts & Font::BOLD) ? FW_BOLD : FW_REGULAR,  // font weight
                         font.m_opts & Font::ITALIC,                         // italic attribute option
                         font.m_opts & Font::UNDERLINE,                      // underline attribute option
                         font.m_opts & Font::STRIKE_OUT,                     // strikeout attribute option
                         font.m_charset,                                     // character set identifier
                         OUT_DEFAULT_PRECIS,                                 // output precision
                         CLIP_DEFAULT_PRECIS,                                // clipping precision
                         ANTIALIASED_QUALITY,  // DEFAULT_QUALITY,          // output quality
                         DEFAULT_PITCH,        // pitch and family
                         font.m_name.c_str()   // typeface name
    );
    if (m_hfont == 0)
        THROW(ERR_COMMON, "Can't create font " << font.m_name.c_str());
    ::SelectObject(m_dc, m_hfont);
    ::SetTextColor(m_dc, font.m_color);
#endif
}

#ifndef OLD_WIN32_RENDERER
// GDI+ draws nothing with a family it does not have. A track line without font-name=, or naming a font that is not
// installed, gave a subtitle track without pictures, a warning that the font size was too large and "Mux successful":
// the FontFamily of such a name fails, AddString adds no path, and GetEmHeight gives 0, which the line spacing is
// divided by. Such a name is drawn with the system's sans serif font instead, and a warning names it once.
const wchar_t* TextSubtitlesRenderWin32::family()
{
    auto it = m_families.find(m_font.m_name);
    if (it == m_families.end())
    {
        std::vector<wchar_t> name = toWide(m_font.m_name);
        if (const FontFamily wanted(name.data()); m_font.m_name.empty() || wanted.GetLastStatus() != Ok)
        {
            WCHAR fallback[LF_FACESIZE] = {};
            FontFamily::GenericSansSerif()->GetFamilyName(fallback);
            name.assign(fallback, fallback + wcslen(fallback) + 1);
            LTRACE(
                LT_WARN, 2,
                "Warning: " << (m_font.m_name.empty() ? std::string("no font is named for the subtitles (font-name=)")
                                                      : "the font \"" + m_font.m_name + "\" is not installed")
                            << ", and the subtitles are drawn with " << toUtf8(fallback) << ".");
        }
        it = m_families.emplace(m_font.m_name, std::move(name)).first;
    }
    return it->second.data();
}
#endif

void TextSubtitlesRenderWin32::drawText(const std::string& text, RECT* rect)
{
#ifdef OLD_WIN32_RENDERER
    ::DrawText(m_dc, text.c_str(), text.length(), rect, DT_NOPREFIX);
#else
    Graphics graphics(m_dc);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    const FontFamily fontFamily(family());
    const StringFormat strformat;
    GraphicsPath path;

    const auto text_wide = toWide(text);
    path.AddString(text_wide.data(), static_cast<int>(text_wide.size()) - 1, &fontFamily, m_font.m_opts & 0xf,
                   static_cast<float>(m_font.m_size), Point(rect->left, rect->top), &strformat);

    // GDI+ draws a pen of width 0 one pixel wide, so font-border=0 still gave the text a black hairline, and a
    // half-transparent colour laid over it came out more opaque than asked (alpha 181 for 0x80) at every edge.
    // Without a border the outline is left out under a half-transparent colour; under an opaque one it is drawn as
    // it always was.
    const uint8_t alpha = m_font.m_color >> 24;
    if (m_font.m_borderWidth > 0 || alpha == 255)
    {
        const uint8_t outColor = (alpha * 48 + 128) / 255;
        Pen pen(Color(outColor, 0, 0, 0), m_font.m_borderWidth * 2.0F);
        pen.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&pen, &path);

        Pen penInner(Color(alpha, 0, 0, 0), m_font.m_borderWidth);
        penInner.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&penInner, &path);
    }

    const SolidBrush brush(Color(m_font.m_color));
    graphics.FillPath(&brush, &path);
#endif
}

void TextSubtitlesRenderWin32::getTextSize(const std::string& text, SIZE* mSize)
{
#ifdef OLD_WIN32_RENDERER
    ::GetTextExtentPoint32(m_dc, text.c_str(), text.size(), mSize);
#else
    const int opts = m_font.m_opts & 0xf;
    const FontFamily fontFamily(family());
    const ::Font font(&fontFamily, static_cast<float>(m_font.m_size), opts, UnitPoint);

    const int lineSpacing = fontFamily.GetLineSpacing(FontStyleRegular);
    const int lineSpacingPixel =
        lround(font.GetSize() * static_cast<double>(lineSpacing) / fontFamily.GetEmHeight(opts));

    const StringFormat strformat;
    GraphicsPath path;
    const auto text_wide = toWide(text);
    path.AddString(text_wide.data(), static_cast<int>(text_wide.size()) - 1, &fontFamily, opts,
                   static_cast<float>(m_font.m_size), Point(0, 0), &strformat);
    RectF rect;
    Pen pen(Color(0x30, 0, 0, 0), m_font.m_borderWidth * 2.0f);
    pen.SetLineJoin(LineJoinRound);
    path.GetBounds(&rect, nullptr, &pen);
    mSize->cx = static_cast<int>(rect.Width);
    mSize->cy = lineSpacingPixel;
#endif
}

// The size above is the ink of the outline, so a space at either end of a piece has no width in it: "and <b>bold</b>"
// was drawn "andbold", and a piece that starts with a space overlapped the one before. The advance is measured with a
// bar set after the text: the right edges of the two outlines differ by the text's advance, whatever the layout puts
// in front of the first character.
int TextSubtitlesRenderWin32::getTextAdvance(const std::string& text)
{
#ifdef OLD_WIN32_RENDERER
    return TextSubtitlesRender::getTextAdvance(text);
#else
    const int opts = m_font.m_opts & 0xf;
    const FontFamily fontFamily(family());
    const StringFormat strformat;
    const auto rightEdge = [&](const std::string& s)
    {
        GraphicsPath path;
        const auto wide = toWide(s);
        path.AddString(wide.data(), static_cast<int>(wide.size()) - 1, &fontFamily, opts,
                       static_cast<float>(m_font.m_size), Point(0, 0), &strformat);
        RectF rect;
        path.GetBounds(&rect);
        return static_cast<double>(rect.X) + rect.Width;
    };
    return static_cast<int>(lround(rightEdge(text + "|") - rightEdge("|")));
#endif
}

int TextSubtitlesRenderWin32::getLineSpacing()
{
#ifdef OLD_WIN32_RENDERER
    TEXTMETRIC tm;
    ::GetTextMetrics(m_dc, &tm);
    return tm.tmAscent;
#else
    const int opts = m_font.m_opts & 0xf;
    const FontFamily fontFamily(family());
    const ::Font font(&fontFamily, static_cast<float>(m_font.m_size), opts, UnitPoint);

    const int lineSpacing = fontFamily.GetLineSpacing(opts);
    const int lineSpacingPixel =
        lround(font.GetSize() * static_cast<double>(lineSpacing) / fontFamily.GetEmHeight(opts));
    return lineSpacingPixel;
#endif
}

int TextSubtitlesRenderWin32::getBaseline()
{
#ifdef OLD_WIN32_RENDERER
    TEXTMETRIC tm;
    ::GetTextMetrics(m_dc, &tm);
    return tm.tmAscent;
#else
    const int opts = m_font.m_opts & 0xf;
    const FontFamily fontFamily(family());
    const ::Font font(&fontFamily, static_cast<float>(m_font.m_size), opts, UnitPoint);

    const int descentOffset = fontFamily.GetCellDescent(opts);
    const int descentPixel = lround(font.GetSize() * static_cast<double>(descentOffset) / fontFamily.GetEmHeight(opts));
    return descentPixel;
#endif
}

void TextSubtitlesRenderWin32::flushRasterBuffer() { GdiFlush(); }

}  // namespace text_subtitles
