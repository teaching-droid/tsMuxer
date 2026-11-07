// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#if !defined(_WIN32) || defined(WIN32_DEBUG_FREETYPE)
#ifdef _WIN32
#pragma comment(lib, "../../freetype/lib/freetype.lib")
#endif

#include "textSubtitlesRenderFT.h"

#include "../convertUTF.h"
#include "../vodCoreException.h"
#include "../vod_common.h"
// #include "../math.h"
#include <fs/directory.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <map>

#if defined(_WIN32)
static constexpr char FONT_ROOT[] = "c:/WINDOWS/Fonts";  // for debug only
#elif __linux__ == 1
const static char FONT_ROOT[] = "/usr/share/fonts/";
#elif defined(__APPLE__) && defined(__MACH__)
const static char FONT_ROOT[] = "/System/Library/Fonts/";
#elif defined(__NetBSD__)
const static char FONT_ROOT[] = "/usr/X11R7/lib/X11/fonts/";
#endif

#include <freetype/ftstroke.h>
#include <fs/systemlog.h>

using namespace std;

namespace tsmuxer
{
std::string realpath(const std::string& path)
{
    const std::unique_ptr<char, decltype(::free)*> resolved{::realpath(path.c_str(), nullptr), ::free};
    return resolved ? resolved.get() : std::string();
}
}  // namespace tsmuxer

namespace text_subtitles
{
FT_Library TextSubtitlesRenderFT::library;
std::map<std::string, std::string> TextSubtitlesRenderFT::m_fontNameToFile;

constexpr double PI = 3.1415926f;
constexpr double angle = -PI / 10.0f;

TextSubtitlesRenderFT::TextSubtitlesRenderFT() : TextSubtitlesRender()
{
    static bool initialized = false;
    if (!initialized)
    {
        int error = FT_Init_FreeType(&library);
        if (error)
            THROW(ERR_COMMON, "Can't initialize freeType font library");
        loadFontMap();
        initialized = true;
    }
    m_pData = nullptr;
    italic_matrix.xx = static_cast<FT_Fixed>(cos(angle) * 0x10000L);
    italic_matrix.xy = static_cast<FT_Fixed>(-sin(angle) * 0x10000L);
    italic_matrix.yx = 0 * 0x10000L;
    italic_matrix.yy = 1 * 0x10000L;

    bold_matrix.xx = static_cast<FT_Fixed>(1.5 * 0x10000L);
    bold_matrix.xy = static_cast<FT_Fixed>(0.0 * 0x10000L);
    bold_matrix.yx = static_cast<FT_Fixed>(0.0 * 0x10000L);
    bold_matrix.yy = static_cast<FT_Fixed>(1.0 * 0x10000L);

    italic_bold_matrix.xx = static_cast<FT_Fixed>(cos(angle) * 1.5 * 0x10000L);
    italic_bold_matrix.xy = static_cast<FT_Fixed>(-sin(angle) * 1.5 * 0x10000L);
    italic_bold_matrix.yx = 0 * 0x10000L;
    italic_bold_matrix.yy = 1 * 0x10000L;
}

// Every place a font can be installed, and every extension one can arrive in.
//
// Only FONT_ROOT was searched before, and only for *.ttf, so a font a user installed for
// themselves was invisible however they installed it, and so was every .otf and .ttc wherever it
// sat. On a plain Ubuntu that is the difference between the thirty faces the distribution ships
// and everything the user has added since.
//
// A tilde is not a path. The macOS entry here was written as "~/Library/Fonts/" and no part of
// this code expands one, so that folder has never been searched at all; HOME is read instead.
std::vector<std::string> TextSubtitlesRenderFT::fontFolders()
{
    std::vector<std::string> folders{FONT_ROOT};
    const char* home = getenv("HOME");
#if defined(__APPLE__) && defined(__MACH__)
    folders.emplace_back("/Library/Fonts/");
    if (home)
        folders.emplace_back(std::string(home) + "/Library/Fonts/");
#elif !defined(_WIN32)
    folders.emplace_back("/usr/local/share/fonts/");
    if (home)
    {
        folders.emplace_back(std::string(home) + "/.fonts/");
        folders.emplace_back(std::string(home) + "/.local/share/fonts/");
    }
#endif
    return folders;
}

void TextSubtitlesRenderFT::loadFontMap()
{
    vector<string> fileList;
    // A .ttc holds several faces and only the first is taken, which is what happens for a .ttf
    // with more than one face too. Better than the file being invisible.
    static const char* masks[] = {"*.ttf", "*.otf", "*.ttc"};
    for (const auto& folder : fontFolders())
        for (const char* mask : masks)
        {
            vector<string> found;
            findFilesRecursive(folder.c_str(), mask, &found);
            fileList.insert(fileList.end(), found.begin(), found.end());
        }

    for (auto& fontFile : fileList)
    {
        // LTRACE(LT_INFO, 2, "before loading font " << fileList[i].c_str());
        if (strEndWith(fontFile, "AppleMyungjo.ttf"))
            continue;

        FT_Face font;
        const int error = FT_New_Face(library, fontFile.c_str(), 0, &font);
        if (error == 0)
        {
            string fontFamily = strToLowerCase(font->family_name);

            auto itr = m_fontNameToFile.find(fontFamily);

            if (itr == m_fontNameToFile.end() || fontFile.length() < itr->second.length())
            {
                m_fontNameToFile[fontFamily] = tsmuxer::realpath(fontFile);
            }
            FT_Done_Face(font);
        }
        // LTRACE(LT_INFO, 2, "after loading font " << fileList[i].c_str());
    }
}

// ** A FONT THAT IS NOT INSTALLED IS REPLACED, AS ON WINDOWS. ** The window writes font-name="Arial" unless another
// font is chosen, and Linux has no Arial: such a meta, and any meta written on Windows, ended with "Can't find a font
// file for Arial" there, while Windows draws a font it lacks with a sans serif font of its own and says so. The
// subtitles are drawn with a sans serif font that is installed: one made to Arial's measures first, then those most
// systems have, then the one fontconfig gives for sans-serif. Only when none is there does the job end.
std::string TextSubtitlesRenderFT::fallbackFontFile(std::string& family)
{
#if defined(__APPLE__) && defined(__MACH__)
    static const char* const candidates[] = {"Helvetica", "Helvetica Neue", "Arial", "Geneva"};
#else
    static const char* const candidates[] = {"Liberation Sans", "Arimo",  "DejaVu Sans", "Noto Sans", "FreeSans",
                                             "Nimbus Sans",     "Ubuntu", "Cantarell",   "Arial"};
#endif
    for (const char* name : candidates)
        if (const auto itr = m_fontNameToFile.find(strToLowerCase(name)); itr != m_fontNameToFile.end())
        {
            family = name;
            return itr->second;
        }
#if !defined(_WIN32)
    // The font fontconfig takes for sans-serif, where its program is installed; a file FreeType opens as a whole font
    if (FILE* pipe = popen("fc-match -f '%{family[0]}|%{file}' sans-serif 2>/dev/null", "r"))
    {
        std::string answer;
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) answer += buf;
        pclose(pipe);
        const size_t bar = answer.find('|');
        if (bar != std::string::npos)
        {
            const std::string file = trimStr(answer.substr(bar + 1));
            const std::string lower = strToLowerCase(file);
            if ((strEndWith(lower, ".ttf") || strEndWith(lower, ".otf") || strEndWith(lower, ".ttc")) &&
                fileExists(file))
            {
                family = answer.substr(0, bar);
                return file;
            }
        }
    }
#endif
    return std::string();
}

// delete[], because the buffer below is new[]. Both sites had plain delete, which is the same
// fault reported against the Matroska demuxer, at a place the sweep for it did not reach: it
// needs a subtitle track AND a font the machine actually has, or the render never allocates.
TextSubtitlesRenderFT::~TextSubtitlesRenderFT() { delete[] m_pData; }

void TextSubtitlesRenderFT::setRenderSize(int width, int height)
{
    m_width = width;
    m_height = height;
    delete[] m_pData;
    const int size = width * height * 4;
    m_pData = new uint8_t[size];  // 32bpp ARGB buffer
}

string TextSubtitlesRenderFT::findAdditionFontFile(const string& fontName, const string& fontExt, bool isBold,
                                                   bool isItalic)
{
    m_emulateItalic = false;
    m_emulateBold = false;
    if (isBold && isItalic)
    {
        if (fileExists(fontName + string("BI") + fontExt))
            return fontName + string("BI") + fontExt;
        if (fileExists(fontName + string("Bi") + fontExt))
            return fontName + string("Bi") + fontExt;
        if (fileExists(fontName + string("Bd") + fontExt))
        {
            m_emulateItalic = true;
            return fontName + string("Bd") + fontExt;
        }
        if (fileExists(fontName + string("B") + fontExt))
        {
            m_emulateItalic = true;
            return fontName + string("B") + fontExt;
        }
        if (fileExists(fontName + string("It") + fontExt))
        {
            m_emulateBold = true;
            return fontName + string("It") + fontExt;
        }
        if (fileExists(fontName + string("I") + fontExt))
        {
            m_emulateBold = true;
            return fontName + string("I") + fontExt;
        }
        m_emulateBold = true;
        m_emulateItalic = true;
        return fontName + fontExt;
    }
    if (isBold)
    {
        if (fileExists(fontName + string("Bd") + fontExt))
        {
            return fontName + string("Bd") + fontExt;
        }
        if (fileExists(fontName + string("B") + fontExt))
        {
            return fontName + string("B") + fontExt;
        }
        m_emulateBold = true;
        return fontName + fontExt;
    }
    if (isItalic)
    {
        if (fileExists(fontName + string("It") + fontExt))
        {
            return fontName + string("It") + fontExt;
        }
        if (fileExists(fontName + string("I") + fontExt))
        {
            return fontName + string("I") + fontExt;
        }
        m_emulateItalic = true;
        return fontName + fontExt;
    }

    return fontName + fontExt;
}

int TextSubtitlesRenderFT::loadFont(const string& fontName, FT_Face& face)
{
    const auto itr = m_fontMap.find(fontName);
    if (itr == m_fontMap.end())
    {
        const int error = FT_New_Face(library, fontName.c_str(), 0, &face);
        if (error)
            return error;
        // m_fontMap.insert(make_pair<string, FT_Face>(fontName, face));
        m_fontMap.insert(make_pair(fontName, face));
    }
    else
        face = itr->second;
    return 0;
}

void TextSubtitlesRenderFT::setFont(const Font& font)
{
    // Loaded when the face is not this font's, not when m_font differs: m_font can already hold the font with no face
    // loaded at all, and the first text measured (textFits) then went to FreeType with no face, which crashed.
    m_font = font;
    if (m_face == nullptr || m_loadedFont != font)
    {
        // Windows draws with a font of its own choosing when none is named; here there is none to fall back on
        if (font.m_name.empty())
            THROW(ERR_COMMON,
                  "No font is named for the subtitles. Give the track font-name=, the name of a font "
                  "installed here or the path of a .ttf, .otf or .ttc file.")
        string fontName = font.m_name;
        // A name that ends in a font file's extension is a path and is used as it stands;
        // anything else is a family name and is looked up. The test used to be ".ttf" alone and
        // case sensitive, so a path ending .otf, .ttc or .TTF was taken for a family name and
        // reported missing.
        const std::string fontLower = strToLowerCase(fontName);
        if (!strEndWith(fontLower, string(".ttf")) && !strEndWith(fontLower, string(".otf")) &&
            !strEndWith(fontLower, string(".ttc")))
        {
            auto itr = m_fontNameToFile.find(fontLower);
            if (itr != m_fontNameToFile.end())
                fontName = itr->second;
            else if (!m_fallbackFontFile.empty())
            {
                // A font a tag names (<font face=...>) that is not installed: Windows draws such a piece with
                // another font, here the job ended. It is drawn with the track's font, and said once per name.
                if (m_missingFonts.insert(fontLower).second)
                    LTRACE(LT_WARN, 2,
                           "Warning: the font \"" << font.m_name
                                                  << "\" is not installed, and the subtitles that name it "
                                                  << "are drawn with " << m_fallbackFontFile << ".");
                fontName = m_fallbackFontFile;
            }
            else
            {
                std::string family;
                const std::string file = fallbackFontFile(family);
                if (file.empty())
                    THROW(ERR_COMMON, "Can't find a font file for "
                                          << fontName
                                          << ", and no sans serif font that could take its place is installed. Give "
                                             "the track font-name= with a font installed here, or the path of a "
                                             ".ttf, .otf or .ttc file.")
                // Once per font in a job: the track is drawn by a renderer of its own when it is checked before the
                // mux and again in the mux, and each said it
                static std::set<std::string> replacedFonts;
                if (replacedFonts.insert(fontLower).second)
                    LTRACE(LT_WARN, 2,
                           "Warning: the font \"" << font.m_name
                                                  << "\" is not installed, so the subtitles are drawn with " << family
                                                  << " (" << file << ").");
                fontName = file;
            }
        }
        if (m_fallbackFontFile.empty())
            m_fallbackFontFile = fontName;
        string fileExt = extractFileExt(fontName);
        if (fileExt.length() > 0)
            fileExt = string(".") + fileExt;
        fontName = fontName.substr(0, fontName.length() - fileExt.length());
        string postfix;
        string modFontName =
            findAdditionFontFile(fontName, fileExt, font.m_opts & Font::BOLD, font.m_opts & Font::ITALIC);
        int error = loadFont(modFontName, m_face);
        if (error == FT_Err_Unknown_File_Format)
        {
            THROW(ERR_COMMON, "Unknown font format for file " << modFontName.c_str())
        }
        else if (error)
        {
            THROW(ERR_COMMON, "Can't load font file " << modFontName.c_str())
        }

        // No piece of a subtitle is measured or drawn above 65,535 (browserSizeToRealSize holds every size to it, and
        // drawNext measures nothing above 16,384), but the track's own font is loaded once at the start with the size
        // its line gives, and FreeType refuses a face of more than 65,535 pixels per em: font-size=74000 or more
        // ended the mux with "Invalid font size" before a subtitle was read. The face is set to 65,535 at most; the
        // font keeps its size, from which the size of every piece is worked out.
        const int faceSize = (std::min)(font.m_size, 65535);
        error = FT_Set_Char_Size(m_face,        /* handle to face object */
                                 0,             /* char_width in 1/64th of points */
                                 faceSize * 64, /* char_height in 1/64th of points */
                                 64,            /* horizontal device resolution */
                                 64);           /* vertical device resolution */
        if (error)
            THROW(ERR_COMMON, "Invalid font size " << font.m_size)

        m_line_thickness = FT_MulFix(m_face->underline_thickness, m_face->size->metrics.y_scale) >> 6;
        if (m_line_thickness < 1)
            m_line_thickness = 1;
        m_underline_position = -FT_MulFix(m_face->underline_position, m_face->size->metrics.y_scale);
        m_underline_position >>= 6;

        if (m_font.m_charset)
        {
            auto encoding = reinterpret_cast<FT_Encoding*>(&m_font.m_charset);
            error = FT_Select_Charmap(m_face, *encoding);
            if (error)
                THROW(ERR_COMMON, "Can't load charset " << m_font.m_charset << " for font " << modFontName.c_str())
        }
        // A track asks for few faces and sizes, but a search for the size that fits tries new ones for each
        // subtitle: the kept measurements are let go now and then, so that they cannot grow without end.
        if (m_glyphMetrics.size() >= 256)
            m_glyphMetrics.clear();
        m_curGlyphMetrics = &m_glyphMetrics[std::make_tuple(m_face, faceSize, m_emulateBold, m_emulateItalic)];
        m_loadedFont = font;
    }
}

uint32_t grayColorToARGB(unsigned char gray, uint32_t color)
{
    uint32_t rez = color;
    const auto bPtr = reinterpret_cast<uint8_t*>(&rez);
    gray &= 0xF0;
    for (int i = 0; i < 4; ++i)
        bPtr[i] = static_cast<uint8_t>(lround(static_cast<float>(bPtr[i]) * static_cast<float>(gray) / 240.0f));
    return rez;
}

uint32_t clrMax(uint32_t first, uint32_t second)
{
    const auto rgba = reinterpret_cast<RGBQUAD*>(&first);
    const auto rgba2 = reinterpret_cast<RGBQUAD*>(&second);
    const int first_yuv = static_cast<int>(0.257 * rgba->rgbRed + 0.504 * rgba->rgbGreen + 0.098 * rgba->rgbBlue) + 16;
    const int second_yuv =
        static_cast<int>(0.257 * rgba2->rgbRed + 0.504 * rgba2->rgbGreen + 0.098 * rgba2->rgbBlue) + 16;
    return first_yuv > second_yuv ? first : second;
}

void TextSubtitlesRenderFT::drawHorLine(int left, int right, int top, const RECT* rect) const
{
    if (top < rect->top || top >= rect->bottom)
        return;
    // rect->right is the width of the picture, the first column outside it, as it is for the glyphs (x < width). The
    // line is drawn up to and including right, which was held to rect->right itself, so a line that reached the edge
    // (an underlined or struck-out text wider than the picture, which a text too long to fit at any size is) wrote the
    // pixel x = width: the first pixel of the next row, or, on the last row, 4 bytes past the end of the picture.
    if (left < rect->left)
        left = rect->left;
    if (right > rect->right - 1)
        right = rect->right - 1;
    const int offset = m_width * top + left;
    auto dst = reinterpret_cast<uint32_t*>(m_pData) + offset;
    // The picture holds each pixel as 0xAARRGGBB, the form of the font colour itself. The colour used to have its red
    // and blue exchanged on the way, so an underline was drawn blue where the text asked for red.
    for (int x = left; x <= right; ++x) *dst++ = m_font.m_color;
}

struct Pixel32
{
    Pixel32(const uint32_t val = 0)
        : b(static_cast<uint8_t>(val)),
          g(static_cast<uint8_t>(val >> 8)),
          r(static_cast<uint8_t>(val >> 16)),
          a(static_cast<uint8_t>(val >> 24))
    {
    }
    Pixel32(const uint8_t bi, const uint8_t gi, const uint8_t ri, const uint8_t ai = 255)
    {
        b = bi;
        g = gi;
        r = ri;
        a = ai;
    }

    uint8_t b, g, r, a;
};

struct Vec2
{
    Vec2() = default;
    Vec2(const int a, const int b) : x(a), y(b) {}

    int x, y;
};

struct Rect
{
    Rect() : xmin(0), xmax(0), ymin(0), ymax(0) {}

    Rect(const int left, const int top, const int right, const int bottom)
        : xmin(left), xmax(right), ymin(top), ymax(bottom)
    {
    }

    void Include(const Vec2& r)
    {
        xmin = FFMIN(xmin, r.x);
        ymin = FFMIN(ymin, r.y);
        xmax = FFMAX(xmax, r.x);
        ymax = FFMAX(ymax, r.y);
    }

    int Width() const { return xmax - xmin + 1; }
    int Height() const { return ymax - ymin + 1; }

    int xmin, xmax, ymin, ymax;
};

struct Span
{
    Span() : x(0), y(0), width(0), coverage(0) {}

    Span(int _x, int _y, int _width, uint8_t _coverage) : x(_x), y(_y), width(_width), coverage(_coverage) {}

    int x, y, width;
    uint8_t coverage;
};

typedef std::vector<Span> Spans;

void RasterCallback(const int y, const int count, const FT_Span* const spans, void* const user)
{
    const auto sptr = static_cast<Spans*>(user);
    for (int i = 0; i < count; ++i) sptr->push_back(Span(spans[i].x, y, spans[i].len, spans[i].coverage));
}

void RenderSpans(const FT_Library& library, FT_Outline* const outline, Spans* spans)
{
    FT_Raster_Params params = {};
    params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT;
    params.gray_spans = RasterCallback;
    params.user = spans;

    FT_Outline_Render(library, outline, &params);
}

void RenderGlyph(const FT_Library& library, const uint32_t ch, const FT_Face& face, const Pixel32& fontCol,
                 const Pixel32 outlineColOut, const Pixel32 outlineColIner, const float outlineWidth, int left, int top,
                 const int width, const int height, uint32_t* dstData)
{
    // Load the glyph we are looking for.
    const FT_UInt gindex = FT_Get_Char_Index(face, ch);
    // Need an outline for this to work.
    if (FT_Load_Glyph(face, gindex, FT_LOAD_NO_BITMAP) != 0 || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return;

    // Render the basic glyph to a span list.
    Spans spans;
    RenderSpans(library, &face->glyph->outline, &spans);

    // Next we need the spans for the outline.
    Spans outlineSpansOut;
    Spans outlineSpansIner;

    FT_Glyph glyph;
    if (FT_Get_Glyph(face->glyph, &glyph) == 0)
    {
        FT_Stroker strokerOut;
        FT_Stroker_New(library, &strokerOut);
        FT_Stroker_Set(strokerOut, static_cast<int>(outlineWidth * 64), FT_STROKER_LINECAP_ROUND,
                       FT_STROKER_LINEJOIN_ROUND, 0);
        FT_Glyph_StrokeBorder(&glyph, strokerOut, 0, 1);
        // Again, this needs to be an outline to work. Without a border (font-border=0) the stroke of width 0 is the
        // glyph's own shape: drawn in black under a half-transparent text, it made the text more opaque than asked.
        // It is then left out; under an opaque colour it is drawn as it always was.
        const bool border = outlineWidth > 0 || fontCol.a == 255;
        if (glyph->format == FT_GLYPH_FORMAT_OUTLINE && border)
        {
            // Render the outline spans to the span list
            FT_Outline* o = &reinterpret_cast<FT_OutlineGlyph>(glyph)->outline;
            RenderSpans(library, o, &outlineSpansOut);
        }
        FT_Stroker_Done(strokerOut);  // Clean up afterwards.
        FT_Done_Glyph(glyph);

        // The second glyph was taken but never given back, so every glyph of every subtitle
        // rendered leaked one, along with the outline it owns. And the result was not checked:
        // on a failure glyph still held the pointer freed on the line above, which the stroker
        // below would then have written through.
        if (FT_Get_Glyph(face->glyph, &glyph) == 0)
        {
            FT_Stroker strokerIn;
            FT_Stroker_New(library, &strokerIn);
            FT_Stroker_Set(strokerIn, static_cast<int>(outlineWidth * 32), FT_STROKER_LINECAP_ROUND,
                           FT_STROKER_LINEJOIN_ROUND, 0);
            FT_Glyph_StrokeBorder(&glyph, strokerIn, 0, 1);
            if (glyph->format == FT_GLYPH_FORMAT_OUTLINE && border)
            {
                FT_Outline* o = &reinterpret_cast<FT_OutlineGlyph>(glyph)->outline;
                RenderSpans(library, o, &outlineSpansIner);
            }
            FT_Stroker_Done(strokerIn);
            FT_Done_Glyph(glyph);
        }

        // Now we need to put it all together.
        if (!spans.empty())
        {
            // Figure out what the bounding rect is for both the span lists.
            Rect rect(spans[0].x, spans[0].y, spans[0].x, spans[0].y);
            for (const auto& span : spans)
            {
                rect.Include(Vec2(span.x, span.y));
                rect.Include(Vec2(span.x + span.width - 1, span.y));
            }
            for (const auto& span : outlineSpansOut)
            {
                rect.Include(Vec2(span.x, span.y));
                rect.Include(Vec2(span.x + span.width - 1, span.y));
            }
            for (const auto& span : outlineSpansIner)
            {
                rect.Include(Vec2(span.x, span.y));
                rect.Include(Vec2(span.x + span.width - 1, span.y));
            }

            // This is unused in this test but you would need this to draw
            // more than one glyph.
            const int bearingX = face->glyph->metrics.horiBearingX >> 6;
            const int bearingY = face->glyph->metrics.horiBearingY >> 6;

            // Get some metrics of our image.
            const int imgHeight = rect.Height();

            top += (face->size->metrics.ascender >> 6) - bearingY;
            left += bearingX;

            // Loop over the outline spans and just draw them into the image.
            for (const auto& span : outlineSpansOut)
            {
                for (int w = 0; w < span.width; ++w)
                {
                    const int y = imgHeight - 1 - (span.y - rect.ymin) + top;
                    const int x = span.x + w + left;
                    if (y >= 0 && y < height && x >= 0 && x < width)
                    {
                        const int offset = y * width + x;
                        const auto dst = reinterpret_cast<Pixel32*>(dstData) + offset;
                        if (*reinterpret_cast<uint32_t*>(dst) == 0)
                            *dst = Pixel32(
                                outlineColOut.b, outlineColOut.g, outlineColOut.r,
                                static_cast<uint8_t>(static_cast<float>(span.coverage * outlineColOut.a) / 255.0F));
                    }
                }
            }

            // Loop over the outline spans and just draw them into the image.
            for (const auto& span : outlineSpansIner)
            {
                for (int w = 0; w < span.width; ++w)
                {
                    const int y = imgHeight - 1 - (span.y - rect.ymin) + top;
                    const int x = span.x + w + left;
                    if (y >= 0 && y < height && x >= 0 && x < width)
                    {
                        const int offset = y * width + x;
                        Pixel32* dst = reinterpret_cast<Pixel32*>(dstData) + offset;
                        *dst = Pixel32(outlineColIner.b, outlineColIner.g, outlineColIner.r,
                                       static_cast<uint8_t>(span.coverage * outlineColIner.a / 255));
                    }
                }
            }

            // Then loop over the regular glyph spans and blend them into the image.
            for (const auto& span : spans)
            {
                for (int w = 0; w < span.width; ++w)
                {
                    const int y = imgHeight - 1 - (span.y - rect.ymin) + top;
                    const int x = span.x + w + left;

                    if (y >= 0 && y < height && x >= 0 && x < width)
                    {
                        const int offset = y * width + x;
                        const auto dst = reinterpret_cast<Pixel32*>(dstData) + offset;

                        // Pixel32 takes blue first. The colours were passed red first, so red and blue changed
                        // places: a subtitle asked for in red came out blue on Linux and macOS.
                        // The alpha of the font colour counts as well as the coverage: it was left out, so a
                        // colour such as 0x80FFFFFF was drawn fully opaque, here and in the border. A
                        // half-transparent text is laid over the border as one layer over another; the alphas of an
                        // opaque one are added, as they always were.
                        const auto src = Pixel32(fontCol.b, fontCol.g, fontCol.r,
                                                 static_cast<uint8_t>(span.coverage * fontCol.a / 255));
                        dst->r = static_cast<uint8_t>(static_cast<float>(dst->r + (src.r - dst->r) * src.a) / 255.0f);
                        dst->g = static_cast<uint8_t>(static_cast<float>(dst->g + (src.g - dst->g) * src.a) / 255.0f);
                        dst->b = static_cast<uint8_t>(static_cast<float>(dst->b + (src.b - dst->b) * src.a) / 255.0f);
                        if (fontCol.a == 255)
                            dst->a = FFMIN(255, dst->a + src.a);
                        else
                            dst->a = static_cast<uint8_t>(src.a + dst->a * (255 - src.a) / 255);
                    }
                }
            }
        }
    }
}

void TextSubtitlesRenderFT::drawText(const string& text, RECT* rect)
{
    FT_Vector pen;
    pen.x = rect->left;
    pen.y = rect->top;
    if (rect->left < 0)
        rect->left = 0;
    if (rect->top < 0)
        rect->top = 0;

    int maxX = 0;

    if (m_emulateItalic && m_emulateBold)
        FT_Set_Transform(m_face, &italic_bold_matrix, &pen);
    else if (m_emulateItalic)
        FT_Set_Transform(m_face, &italic_matrix, &pen);
    else if (m_emulateBold)
        FT_Set_Transform(m_face, &bold_matrix, &pen);
    else
        FT_Set_Transform(m_face, nullptr, &pen);

    const uint8_t alpha = m_font.m_color >> 24;
    const auto outColor = static_cast<uint8_t>(lround(static_cast<float>(alpha) / 255.0 * 48.0));
    convertUTF::IterateUTF8Chars(text,
                                 [&](auto c)
                                 {
                                     RenderGlyph(library, c, m_face, m_font.m_color, Pixel32(0, 0, 0, outColor),
                                                 Pixel32(0, 0, 0, alpha), m_font.m_borderWidth, pen.x, pen.y,
                                                 rect->right, rect->bottom, reinterpret_cast<uint32_t*>(m_pData));
                                     pen.x += m_face->glyph->advance.x >> 6;
                                     pen.x += lround(m_font.m_borderWidth / 2.0F);
                                     if (m_emulateBold || m_emulateItalic)
                                         pen.x += m_line_thickness - 1;
                                     maxX = pen.x + m_face->glyph->bitmap_left;
                                     return true;
                                 });
    if (m_font.m_opts & Font::UNDERLINE || m_font.m_opts & Font::STRIKE_OUT)
    {
        if (m_font.m_opts & Font::UNDERLINE)
        {
            const int y = pen.y + (m_face->size->metrics.ascender >> 6) + m_underline_position - m_line_thickness / 2;
            for (int i = 0; i < m_line_thickness; ++i)
                drawHorLine(rect->left, /*rect->left + size.cx*/ maxX, y + i, rect);
        }
        if (m_font.m_opts & Font::STRIKE_OUT)
        {
            const int y =
                pen.y + static_cast<int>((m_face->size->metrics.ascender >> 6) * 2.0 / 3.0) - m_line_thickness / 2;
            for (int i = 0; i < m_line_thickness; ++i)
                drawHorLine(rect->left, /*rect->left + size.cx*/ pen.x, y + i, rect);
        }
    }
}

void TextSubtitlesRenderFT::getTextSize(const string& text, SIZE* mSize)
{
    FT_Vector pen;
    pen.x = 0;
    pen.y = 0;
    mSize->cy = mSize->cx = 0;

    if (m_emulateItalic && m_emulateBold)
        FT_Set_Transform(m_face, &italic_bold_matrix, &pen);
    else if (m_emulateItalic)
        FT_Set_Transform(m_face, &italic_matrix, &pen);
    else if (m_emulateBold)
        FT_Set_Transform(m_face, &bold_matrix, &pen);
    else
        FT_Set_Transform(m_face, nullptr, &pen);

    convertUTF::IterateUTF8Chars(text,
                                 [&](auto c)
                                 {
                                     const FT_UInt glyph_index = FT_Get_Char_Index(m_face, c);
                                     const auto [kept, isNew] = m_curGlyphMetrics->try_emplace(glyph_index);
                                     if (isNew)
                                     {
                                         // ** MEASURED WITHOUT DRAWING. ** The glyph was also drawn here, only to
                                         // read its left edge: bold or italic text is drawn through a transform,
                                         // and from a size of about 1800 the drawing ran out of room (raster
                                         // overflow) and stopped the mux with "Can't render symbol code", where the
                                         // text itself is drawn without it. Loading the glyph sets its left edge as
                                         // drawing it would (FreeType 2.10 and later), so it is only loaded.
                                         int error = FT_Load_Glyph(m_face, glyph_index, 0);
                                         if (error)
                                         {
                                             m_curGlyphMetrics->erase(kept);
                                             THROW(ERR_COMMON, "Can't load symbol code '" << c << "' from font")
                                         }
#if FREETYPE_MAJOR * 100 + FREETYPE_MINOR < 210  // an older FreeType sets the left edge only when it draws
                                         error = FT_Render_Glyph(m_face->glyph, FT_RENDER_MODE_NORMAL);
                                         if (error)
                                         {
                                             m_curGlyphMetrics->erase(kept);
                                             THROW(ERR_COMMON, "Can't render symbol code '" << c << "' from font")
                                         }
#endif
                                         kept->second =
                                             GlyphMetrics{m_face->glyph->advance.x, m_face->glyph->bitmap_left};
                                     }
                                     pen.x += kept->second.advanceX >> 6;
                                     if (m_emulateBold || m_emulateItalic)
                                         pen.x += m_line_thickness - 1;
                                     pen.x += lround(m_font.m_borderWidth / 2.0F);
                                     mSize->cy = m_face->size->metrics.height >> 6;
                                     mSize->cx = pen.x + kept->second.bitmapLeft;
                                     return true;
                                 });
}

int TextSubtitlesRenderFT::getLineSpacing() { return m_face->size->metrics.height >> 6; }

int TextSubtitlesRenderFT::getBaseline() { return abs(m_face->size->metrics.descender) >> 6; }

void TextSubtitlesRenderFT::flushRasterBuffer() { return; }

}  // namespace text_subtitles

#endif
