// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef TEXT_SUBTITLES_RENDER_FT_H_
#define TEXT_SUBTITLES_RENDER_FT_H_

#include <ft2build.h>

#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../textSubtitlesRender.h"

#include FT_FREETYPE_H
// #include FT_GLYPH_H

namespace text_subtitles
{
class TextSubtitlesRenderFT final : public TextSubtitlesRender
{
   public:
    TextSubtitlesRenderFT();
    ~TextSubtitlesRenderFT() override;
    void setRenderSize(int width, int height) override;
    void drawText(const std::string& text, RECT* rect) override;
    void setFont(const Font& font) override;
    void getTextSize(const std::string& text, SIZE* mSize) override;
    int getLineSpacing() override;
    int getBaseline() override;
    void flushRasterBuffer() override;

   private:
    static FT_Library library;
    static std::map<std::string, std::string> m_fontNameToFile;
    FT_Face m_face = nullptr;
    bool m_emulateItalic = false;
    bool m_emulateBold = false;
    // The font m_face was loaded for. m_font is set and restored by others (textFits) without loading anything, so a
    // font equal to m_font is not necessarily the one loaded.
    Font m_loadedFont;
    // The file of the first font loaded, the track's own, which a font a tag names and the system lacks falls back
    // to; and the names already said to be missing.
    std::string m_fallbackFontFile;
    std::set<std::string> m_missingFonts;
    int m_underline_position;
    int m_line_thickness;
    FT_Matrix italic_matrix;
    FT_Matrix bold_matrix;
    FT_Matrix italic_bold_matrix;
    void drawHorLine(int left, int right, int top, const RECT* rect) const;
    std::string findAdditionFontFile(const std::string& fontName, const std::string& fontExt, bool isBold,
                                     bool isItalic);
    int loadFont(const std::string& fontName, FT_Face& face);
    static void loadFontMap();
    // A font installed here for subtitles whose named font is not: its file, and its family in family
    static std::string fallbackFontFile(std::string& family);
    static std::vector<std::string> fontFolders();

    std::map<std::string, FT_Face> m_fontMap;

    // ** A GLYPH IS MEASURED ONCE AT EACH SIZE. ** getTextSize loaded and drew every glyph of the text each time it
    // measured it, to read two numbers: the advance, and for the last glyph its left edge. A subtitle that does not
    // fit the picture is measured at eight to ten sizes before it is drawn, so two lines of 100 letters cost some 1,800
    // glyphs loaded and drawn, most of them the same two dozen letters again. The two numbers depend on nothing but
    // the face, its size, the emulated bold or italic and the glyph, so they are kept for each of these and read back;
    // a glyph not yet kept is loaded and drawn exactly as before, so a glyph that fails still fails at the same place.
    struct GlyphMetrics
    {
        FT_Pos advanceX;
        FT_Int bitmapLeft;
    };
    using GlyphMetricsMap = std::unordered_map<FT_UInt, GlyphMetrics>;
    std::map<std::tuple<FT_Face, int, bool, bool>, GlyphMetricsMap> m_glyphMetrics;
    GlyphMetricsMap* m_curGlyphMetrics = nullptr;  // those of the face and size setFont loaded last
};

}  // namespace text_subtitles

#endif
