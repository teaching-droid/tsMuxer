#ifndef TEXT_SUBTITLES_RENDER_WIN_H_
#define TEXT_SUBTITLES_RENDER_WIN_H_

#include <map>
#include <string>
#include <vector>

#include "../textSubtitlesRender.h"
#include "windows.h"

namespace text_subtitles
{
class GdiPlusPriv;

class TextSubtitlesRenderWin32 final : public TextSubtitlesRender
{
   public:
    TextSubtitlesRenderWin32();
    ~TextSubtitlesRenderWin32() override;
    void setRenderSize(int width, int height) override;
    void drawText(const std::string& text, RECT* rect) override;
    void setFont(const Font& font) override;
    void getTextSize(const std::string& text, SIZE* mSize) override;
    int getTextAdvance(const std::string& text) override;
    int getLineSpacing() override;
    int getBaseline() override;
    void flushRasterBuffer() override;

   private:
    HBITMAP m_hbmp;
    BITMAPINFO* m_pbmpInfo;
    HDC m_hdcScreen;
    HDC m_dc;
    HFONT m_hfont;
#ifndef OLD_WIN32_RENDERER
    GdiPlusPriv* m_gdiPriv;
    // The family GDI+ draws the font of m_font with, by font name: the name itself, or the system's sans serif font
    // when GDI+ has no family of that name
    std::map<std::string, std::vector<wchar_t>> m_families;
    const wchar_t* family();
#else
#endif
};

}  // namespace text_subtitles

#endif
