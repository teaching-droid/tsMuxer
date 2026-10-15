#include "textSubtitlesRender.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "utf8Converter.h"
#include "vod_common.h"

using namespace std;

namespace
{
// The colour names of CSS, which an HTML font tag in a subtitle uses. Five names were misspelt here (tea, cora,
// lightcora, seashel, mediumauqamarine) and three values were wrong (mediumpurple, olivedrab, palevioletred); the
// names CSS spells two ways are there both ways, and cyan, lightgray and rebeccapurple were missing.
const std::unordered_map<std::string, uint32_t> defaultPallette = {
    make_pair("black", 0x000000),
    make_pair("cyan", 0x00ffff),
    make_pair("lightgray", 0xd3d3d3),
    make_pair("grey", 0x808080),
    make_pair("darkgrey", 0xa9a9a9),
    make_pair("dimgrey", 0x696969),
    make_pair("slategrey", 0x708090),
    make_pair("lightslategrey", 0x778899),
    make_pair("darkslategrey", 0x2f4f4f),
    make_pair("rebeccapurple", 0x663399),
    make_pair("aqua", 0x00ffff),
    make_pair("red", 0xff0000),
    make_pair("green", 0x008000),
    make_pair("blue", 0x0000ff),
    make_pair("fuchsia", 0xff00ff),
    make_pair("gray", 0x808080),
    make_pair("lime", 0x00ff00),
    make_pair("maroon", 0x800000),
    make_pair("navy", 0x000080),
    make_pair("olive", 0x808000),
    make_pair("purple", 0x800080),
    make_pair("silver", 0xc0c0c0),
    make_pair("teal", 0x008080),
    make_pair("white", 0xffffff),
    make_pair("yellow", 0xffff00),
    make_pair("orange", 0xffa500),
    make_pair("violet", 0xEE82EE),

    make_pair("aliceblue", 0xf0f8ff),
    make_pair("antiquewhite", 0xfaebd7),
    make_pair("aquamarine", 0x7fffd4),
    make_pair("azure", 0xf0ffff),
    make_pair("beige", 0xf5f5dc),
    make_pair("bisque", 0xffe4c4),
    make_pair("blanchedalmond", 0xffebcd),
    make_pair("blueviolet", 0x8a2be2),
    make_pair("brown", 0xa52a2a),
    make_pair("burlywood", 0xdeb887),
    make_pair("cadetblue", 0x5f9ea0),
    make_pair("chartreuse", 0x7fff00),
    make_pair("chocolate", 0xd2691e),
    make_pair("coral", 0xff7f50),
    make_pair("cornflowerblue", 0x6495ed),
    make_pair("cornsilk", 0xfff8dc),
    make_pair("crimson", 0xdc143c),
    make_pair("darkblue", 0x00008b),
    make_pair("darkcyan", 0x008b8b),
    make_pair("darkgoldenrod", 0xb8860b),
    make_pair("darkgray", 0xa9a9a9),
    make_pair("darkgreen", 0x006400),
    make_pair("darkkhaki", 0xbdb76b),
    make_pair("darkmagenta", 0x8b008b),
    make_pair("darkolivegreen", 0x556b2f),
    make_pair("darkorange", 0xff8c00),
    make_pair("darkorchid", 0x9932cc),
    make_pair("darkred", 0x8b0000),
    make_pair("darksalmon", 0xe9967a),
    make_pair("darkseagreen", 0x8fbc8f),
    make_pair("darkslateblue", 0x483d8b),
    make_pair("darkslategray", 0x2f4f4f),
    make_pair("darkturquoise", 0x00ced1),
    make_pair("darkviolet", 0x9400d3),
    make_pair("deeppink", 0xff1493),
    make_pair("deepskyblue", 0x00bfff),
    make_pair("dimgray", 0x696969),
    make_pair("dodgerblue", 0x1e90ff),
    make_pair("firebrick", 0xb22222),
    make_pair("floralwhite", 0xfffaf0),
    make_pair("forestgreen", 0x228b22),
    make_pair("gainsboro", 0xdcdcdc),
    make_pair("ghostwhite", 0xf8f8ff),
    make_pair("gold", 0xffd700),
    make_pair("goldenrod", 0xdaa520),
    make_pair("greenyellow", 0xadff2f),
    make_pair("honeydew", 0xf0fff0),
    make_pair("hotpink", 0xff69b4),
    make_pair("indianred", 0xcd5c5c),
    make_pair("indigo", 0x4b0082),
    make_pair("ivory", 0xfffff0),
    make_pair("khaki", 0xf0e68c),
    make_pair("lavender", 0xe6e6fa),
    make_pair("lavenderblush", 0xfff0f5),
    make_pair("lawngreen", 0x7cfc00),
    make_pair("lemonchiffon", 0xfffacd),
    make_pair("lightblue", 0xadd8e6),
    make_pair("lightcoral", 0xf08080),
    make_pair("lightcyan", 0xe0ffff),
    make_pair("lightgoldenrodyellow", 0xfafad2),
    make_pair("lightgreen", 0x90ee90),
    make_pair("lightgrey", 0xd3d3d3),
    make_pair("lightpink", 0xffb6c1),
    make_pair("lightsalmon", 0xffa07a),
    make_pair("lightseagreen", 0x20b2aa),
    make_pair("lightskyblue", 0x87cefa),
    make_pair("lightslategray", 0x778899),
    make_pair("lightsteelblue", 0xb0c4de),
    make_pair("lightyellow", 0xffffe0),
    make_pair("limegreen", 0x32cd32),
    make_pair("linen", 0xfaf0e6),
    make_pair("magenta", 0xff00ff),
    make_pair("mediumaquamarine", 0x66cdaa),
    make_pair("mediumblue", 0x0000cd),
    make_pair("mediumorchid", 0xba55d3),
    make_pair("mediumpurple", 0x9370db),
    make_pair("mediumseagreen", 0x3cb371),
    make_pair("mediumslateblue", 0x7b68ee),
    make_pair("mediumspringgreen", 0x00fa9a),
    make_pair("mediumturquoise", 0x48d1cc),
    make_pair("mediumvioletred", 0xc71585),
    make_pair("midnightblue", 0x191970),
    make_pair("mintcream", 0xf5fffa),
    make_pair("mistyrose", 0xffe4e1),
    make_pair("moccasin", 0xffe4b5),
    make_pair("navajowhite", 0xffdead),
    make_pair("oldlace", 0xfdf5e6),
    make_pair("olivedrab", 0x6b8e23),
    make_pair("orangered", 0xff4500),
    make_pair("orchid", 0xda70d6),
    make_pair("palegoldenrod", 0xeee8aa),
    make_pair("palegreen", 0x98fb98),
    make_pair("paleturquoise", 0xafeeee),
    make_pair("palevioletred", 0xdb7093),
    make_pair("papayawhip", 0xffefd5),
    make_pair("peachpuff", 0xffdab9),
    make_pair("peru", 0xcd853f),
    make_pair("pink", 0xffc0cb),
    make_pair("plum", 0xdda0dd),
    make_pair("powderblue", 0xb0e0e6),
    make_pair("rosybrown", 0xbc8f8f),
    make_pair("royalblue", 0x4169e1),
    make_pair("saddlebrown", 0x8b4513),
    make_pair("salmon", 0xfa8072),
    make_pair("sandybrown", 0xf4a460),
    make_pair("seagreen", 0x2e8b57),
    make_pair("seashell", 0xfff5ee),
    make_pair("sienna", 0xa0522d),
    make_pair("skyblue", 0x87ceeb),
    make_pair("slateblue", 0x6a5acd),
    make_pair("slategray", 0x708090),
    make_pair("snow", 0xfffafa),
    make_pair("springgreen", 0x00ff7f),
    make_pair("steelblue", 0x4682b4),
    make_pair("tan", 0xd2b48c),
    make_pair("thistle", 0xd8bfd8),
    make_pair("tomato", 0xff6347),
    make_pair("turquoise", 0x40e0d0),
    make_pair("wheat", 0xf5deb3),
    make_pair("whitesmoke", 0xf5f5f5),
    make_pair("yellowgreen", 0x9acd32),
};

// Whether s is a number, in hexadecimal or decimal digits.
bool isNumber(const string& s, const bool hex)
{
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [hex](const unsigned char c) { return hex ? isxdigit(c) != 0 : isdigit(c) != 0; });
}

size_t findUnquotedStr(const string& str, const string& substr)
{
    if (substr.empty())
        return string::npos;
    bool quote = false;
    for (size_t i = 0; i < str.size(); i++)
    {
        if (str[i] == '\"' || str[i] == '\'')
            quote = !quote;
        else if (!quote && str[i] == substr[0])
        {
            bool found = true;
            for (size_t j = 1; j < substr.size(); j++)
            {
                if (i + j >= str.size() || str[i + j] != substr[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
                return i;
        }
    }
    return string::npos;
}
}  // namespace

namespace text_subtitles
{
TextSubtitlesRender::TextSubtitlesRender() : m_width(0), m_height(0)
{
    m_pData = nullptr;
    m_borderWidth = 0;
}

TextSubtitlesRender::~TextSubtitlesRender() = default;

// The value after the '=' of a font attribute: up to the next space, or, in quotes of either kind, up to the closing
// quote, so face="Times New Roman" is a name of three words. With its quotes; unquoteArg takes them off.
string findFontArg(const string& text, const size_t pos)
{
    bool delFound = false;
    size_t firstPos = 0;
    for (size_t i = pos; i < text.size(); i++)
    {
        if (text[i] == '=')
            delFound = true;
        else if (delFound)
        {
            if (firstPos == 0 && (text[i] == '"' || text[i] == '\''))
            {
                const size_t close = text.find(text[i], i + 1);
                return close == string::npos ? text.substr(i) : text.substr(i, close - i + 1);
            }
            if (text[i] == ' ')
            {
                if (firstPos != 0)
                    return text.substr(firstPos, i - firstPos);
            }
            else if (firstPos == 0)
                firstPos = i;
        }
    }
    if (firstPos != 0)
        return text.substr(firstPos, text.size() - firstPos);
    return "";
}

// A value in double or single quotes, without them: color='red' was taken as the colour 'red', with its quotes, and
// left the colour as it was.
string unquoteArg(const string& value)
{
    if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0])
        return value.substr(1, value.size() - 2);
    if (!value.empty() && (value[0] == '"' || value[0] == '\''))
        return value.substr(1);
    return value;
}

uint32_t rgbSwap(uint32_t color)
{
    const auto rgb = reinterpret_cast<uint8_t*>(&color);
    const uint8_t tmp = rgb[0];
    rgb[0] = rgb[2];
    rgb[2] = tmp;
    return color;
}

int TextSubtitlesRender::browserSizeToRealSize(const int bSize, double rSize)
{
    // Each step away from the default size is a factor of the square root of 2, so 64 steps are a factor of
    // 2^32: the font is then under one pixel or far larger than any frame, and further steps change nothing
    // that can be shown. A size near 2^31 used to run two billion steps for every piece of text, which took
    // minutes per subtitle on the way down.
    const int steps = std::clamp(bSize, DEFAULT_BROWSER_STYLE_FS - 64, DEFAULT_BROWSER_STYLE_FS + 64);
    if (steps > DEFAULT_BROWSER_STYLE_FS)
        for (int i = DEFAULT_BROWSER_STYLE_FS; i < steps; i++) rSize *= BROWSER_FONT_STYLE_INC_COEFF;
    else if (steps < DEFAULT_BROWSER_STYLE_FS)
        for (int i = steps; i < DEFAULT_BROWSER_STYLE_FS; i++) rSize /= BROWSER_FONT_STYLE_INC_COEFF;
    // A subtitle picture is at most 65,535 pixels on a side, so no larger font can be shown, and a value
    // beyond the int range has no defined conversion.
    return static_cast<int>(std::clamp(rSize, -65535.0, 65535.0));
}

vector<pair<Font, string>> TextSubtitlesRender::processTxtLine(const std::string& line, vector<Font>& fontStack) const
{
    if (fontStack.empty())
    {
        fontStack.push_back(m_font);
        fontStack[0].m_size = DEFAULT_BROWSER_STYLE_FS;
    }
    Font curFont = fontStack[fontStack.size() - 1];

    vector<pair<Font, string>> rez;
    size_t prevTextPos = 0;
    size_t bStartPos = 0;
    bool bStartFound = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '<')
        {
            bStartPos = i;
            bStartFound = true;
        }
        else if (line[i] == '>' && bStartFound)
        {
            bool isTag = false;
            bool endTag = false;
            string tagStr = trimStr(line.substr(bStartPos + 1, i - bStartPos - 1));
            string ltagStr = tagStr;
            for (auto& j : ltagStr) j = static_cast<char>(towlower(j));
            if (ltagStr == "i" || ltagStr == "italic")
            {
                curFont.m_opts |= Font::ITALIC;
                isTag = true;
            }
            else if (ltagStr == "b" || ltagStr == "bold")
            {
                curFont.m_opts |= Font::BOLD;
                isTag = true;
            }
            else if (ltagStr == "u" || ltagStr == "underline")
            {
                curFont.m_opts |= Font::UNDERLINE;
                isTag = true;
            }
            else if (ltagStr == "f" || ltagStr == "force")
            {
                curFont.m_opts |= Font::FORCED;
                isTag = true;
            }
            else if (ltagStr == "strike")
            {
                curFont.m_opts |= Font::STRIKE_OUT;
                isTag = true;
            }
            else if ((ltagStr == "/i" || ltagStr == "/italic") || (ltagStr == "/b" || ltagStr == "/bold") ||
                     (ltagStr == "/u" || ltagStr == "/underline") || (ltagStr == "/f" || ltagStr == "/force") ||
                     ltagStr == "/strike" || strStartWith(ltagStr, "/font"))  // </FONT> as well
            {
                endTag = true;
            }
            else if (strStartWith(ltagStr, "font "))
            {
                size_t fontNamePos = findUnquotedStr(ltagStr, "name");  // ltagStr.find("name");
                if (fontNamePos == string::npos)
                    fontNamePos = findUnquotedStr(ltagStr, "face");
                if (fontNamePos != string::npos)
                    curFont.m_name = unquoteArg(findFontArg(tagStr, fontNamePos /*, lastIndexPos*/));

                size_t colorPos = findUnquotedStr(ltagStr, "color");
                if (colorPos != string::npos)
                {
                    // A name, #RRGGBB (or xRRGGBB, 0xRRGGBB), or a decimal number. The number was read only when the
                    // value was a known name, and then replaced the name's colour: color="red" drew black, and
                    // color="#806040", the form the help shows, was never read at all. A value that is none of these
                    // leaves the colour as it was.
                    auto arg = unquoteArg(findFontArg(ltagStr, colorPos));
                    // #RGB stands for #RRGGBB, as in HTML: #f00 was read as 0x000f00, a dark green.
                    if (arg.size() == 4 && arg[0] == '#' && isNumber(arg.substr(1), true))
                        arg = std::string("#") + arg[1] + arg[1] + arg[2] + arg[2] + arg[3] + arg[3];
                    const auto it = defaultPallette.find(arg);
                    if (it != std::end(defaultPallette))
                        curFont.m_color = it->second;
                    else if (!arg.empty() && (arg[0] == '#' || arg[0] == 'x') && isNumber(arg.substr(1), true))
                        curFont.m_color = strToInt32u(arg.substr(1).c_str(), 16);
                    else if (arg.size() > 2 && arg[0] == '0' && arg[1] == 'x' && isNumber(arg.substr(2), true))
                        curFont.m_color = strToInt32u(arg.substr(2).c_str(), 16);
                    else if (isNumber(arg, false))
                        curFont.m_color = strToInt32u(arg.c_str(), 10);
                    if ((curFont.m_color & 0xff000000u) == 0)
                        curFont.m_color |= 0xff000000u;
                }
                size_t fontSizePos = findUnquotedStr(ltagStr, "size");
                if (fontSizePos != string::npos)
                {
                    string arg = unquoteArg(findFontArg(tagStr, fontSizePos));
                    if (!arg.empty())
                    {
                        // In 64 bits and held to the int range: a relative size such as +2147483647
                        // overflowed the sum. The value is held first, so that even a 19-digit one
                        // cannot overflow the sum in 64 bits either.
                        const int64_t value =
                            std::clamp<int64_t>(std::strtoll(arg.c_str(), nullptr, 10), INT_MIN, INT_MAX);
                        const int64_t size = (arg[0] == '+' || arg[0] == '-') ? curFont.m_size + value : value;
                        curFont.m_size = static_cast<int>(std::clamp<int64_t>(size, INT_MIN, INT_MAX));
                    }
                }
                isTag = true;
            }
            if (isTag || endTag)
            {
                if (bStartPos > prevTextPos)
                {
                    string msg = line.substr(prevTextPos, bStartPos - prevTextPos);
                    rez.emplace_back(fontStack[fontStack.size() - 1], msg);
                }
                if (isTag)
                    fontStack.push_back(curFont);
                else if (fontStack.size() > 1)
                {
                    fontStack.resize(fontStack.size() - 1);
                    curFont = fontStack[fontStack.size() - 1];
                }
                prevTextPos = i + 1;
            }
            bStartFound = false;
        }
    }
    if (line.size() > static_cast<unsigned>(prevTextPos))
        rez.emplace_back(curFont, line.substr(prevTextPos, line.size() - prevTextPos));
    double rSize = m_initFont.m_size;
    for (auto& i : rez)
    {
        i.first.m_size = browserSizeToRealSize(i.first.m_size, rSize);
        if (m_fitScale != 1.0)
            i.first.m_size = FFMAX(1, static_cast<int>(i.first.m_size * m_fitScale));
    }
    return rez;
}

int TextSubtitlesRender::getTextAdvance(const std::string& text)
{
    SIZE size;
    getTextSize(text, &size);
    return size.cx;
}

std::vector<int> TextSubtitlesRender::fittedSizes(const std::string& text)
{
    vector<Font> fontStack;
    const vector<string> lines = splitStr(text.c_str(), '\n');
    m_initFont = m_font;
    std::vector<int> sizes;
    for (auto& line : lines)
    {
        for (const auto& part : processTxtLine(line, fontStack)) sizes.push_back(part.first.m_size);
        sizes.push_back(0);  // the end of a line: two lines are not the same as one line with both pieces
    }
    return sizes;
}

bool TextSubtitlesRender::textFits(const std::string& text)
{
    vector<Font> fontStack;
    const vector<string> lines = splitStr(text.c_str(), '\n');
    const Font saved = m_font;
    m_initFont = m_font;
    bool fits = true;
    int curY = 0;
    for (auto& line : lines)
    {
        int ySize = 0;
        int tWidth = 0;
        const vector<pair<Font, string>> parts = processTxtLine(line, fontStack);
        for (size_t k = 0; k < parts.size(); ++k)
        {
            setFont(parts[k].first);
            SIZE mSize;
            getTextSize(parts[k].second, &mSize);
            ySize = FFMAX(ySize, mSize.cy);
            // A piece with another after it moves the next one on by its advance; the last piece counts by its size,
            // as a line of one piece always did, so that the line is still centred on what is drawn.
            tWidth += k + 1 == parts.size() ? mSize.cx : getTextAdvance(parts[k].second);
        }
        if (tWidth > m_width)
            fits = false;
        curY += lround(static_cast<float>(ySize) * m_font.m_lineSpacing);
    }
    m_font = saved;
    return fits && curY <= m_height;
}

bool TextSubtitlesRender::rasterText(const std::string& text)
{
    bool forced = false;
    memset(m_pData, 0, static_cast<size_t>(m_width) * m_height * 4);
    vector<Font> fontStack;
    const vector<string> lines = splitStr(text.c_str(), '\n');
    int curY = 0;
    m_initFont = m_font;
    for (auto& i : lines)
    {
        vector<pair<Font, string>> txtParts = processTxtLine(i, fontStack);
        for (const auto& j : txtParts)
        {
            if (j.first.m_opts & Font::FORCED)
                forced = true;
        }

        int ySize = 0;
        int tWidth = 0;
        int maxHeight = 0;
        int maxBaseLine = 0;
        vector<int> xSize;
        for (size_t k = 0; k < txtParts.size(); ++k)
        {
            setFont(txtParts[k].first);
            SIZE mSize;
            getTextSize(txtParts[k].second, &mSize);
            ySize = FFMAX(ySize, mSize.cy);
            maxHeight = FFMAX(maxHeight, getLineSpacing());
            maxBaseLine = FFMAX(maxBaseLine, getBaseline());
            // as in textFits: the advance of a piece another follows, the size of the last
            xSize.push_back(k + 1 == txtParts.size() ? mSize.cx : getTextAdvance(txtParts[k].second));
            tWidth += xSize.back();
        }
        const int xOffs = (m_width - tWidth) / 2;
        int curX = 0;
        for (size_t j = 0; j < txtParts.size(); j++)
        {
            Font font(txtParts[j].first);
            setFont(font);

            RECT rect = {curX + xOffs, curY + (maxHeight - getLineSpacing()) - (maxBaseLine - getBaseline()), m_width,
                         m_height};
            drawText(txtParts[j].second, &rect);

            curX += xSize[j];
        }
        curY += lround(static_cast<float>(ySize) * m_font.m_lineSpacing);
    }
    flushRasterBuffer();
    m_font = m_initFont;

    return forced;
}

static constexpr uint32_t BORDER_COLOR = 0xff020202;
static constexpr uint32_t BORDER_COLOR_TMP = RGB(0x1, 0x1, 0x1);

inline void setBPoint(uint32_t* addr)
{
    if (*addr == 0)
        *addr = BORDER_COLOR_TMP;
}

void TextSubtitlesRender::addBorder(const int borderWidth, uint8_t* data, const int width, const int height)
{
    // add black border
    for (int i = 0; i < borderWidth; ++i)
    {
        auto dst = reinterpret_cast<uint32_t*>(data);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (*dst != 0 && *dst != BORDER_COLOR_TMP)
                {
                    if (x > 0)
                    {
                        setBPoint(dst - 1);
                        if (y > 0)
                            setBPoint(dst - 1 - width);
                        if (y < height - 1)
                            setBPoint(dst - 1 + width);
                    }
                    if (y > 0)
                        setBPoint(dst - width);
                    if (y < height - 1)
                        setBPoint(dst + width);
                    if (x < width - 1)
                    {
                        setBPoint(dst + 1);
                        if (y > 0)
                            setBPoint(dst + 1 - width);
                        if (y < height - 1)
                            setBPoint(dst + 1 + width);
                    }
                }
                dst++;
            }
        }
        dst = reinterpret_cast<uint32_t*>(data);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                if (*dst == BORDER_COLOR_TMP)
                    *dst = BORDER_COLOR;
                dst++;
            }
    }
}

}  // namespace text_subtitles
