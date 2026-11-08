// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "utf8Converter.h"

#include <types/types.h>

#include "convertUTF.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace convertUTF;

namespace
{
uint16_t read_be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t read_be32(const uint8_t* p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t read_le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24; }

template <typename R, typename... A>
R get_fn_ret_type(R (*)(A...));

template <typename F>
auto make_vector(const uint8_t* start, size_t numBytes, F func) -> std::vector<decltype(get_fn_ret_type(func))>
{
    std::vector<decltype(get_fn_ret_type(func))> rv;
    const auto elem_size = sizeof(typename decltype(rv)::value_type);
    if (numBytes % elem_size)
    {
        THROW(ERR_COMMON, "Cannot convert string : size " << numBytes << " should be divisible by " << elem_size)
    }
    rv.reserve(numBytes);
    const auto end = (start + numBytes);
    while (start != end)
    {
        rv.emplace_back(func(start));
        start += elem_size;
    }
    return rv;
}

// The characters of UTF-16 or UTF-32 text. A code unit that is no character, a surrogate without its other half or a
// value above U+10FFFF, is U+FFFD REPLACEMENT CHARACTER, and replaced is set. Such a unit was refused, and the
// refusal ended the mux with "Cannot convert string : invalid source text", naming neither the file nor the line: an
// SRT file in UTF-16 with one lone surrogate in a subtitle, such as a text editor leaves when it cuts a pair in two,
// gave no output at all, drawn or kept as text. One bad character now costs that character, as a byte that is not
// valid UTF-8 does in a UTF-8 file (makeValidUtf8 below).
std::vector<UTF32> codePoints(const std::vector<UTF16>& units, bool& replaced)
{
    std::vector<UTF32> rv;
    rv.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i)
    {
        const UTF32 u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF)
            rv.push_back(0x10000 + ((u - 0xD800) << 10) + (units[++i] - 0xDC00U));
        else if (u >= 0xD800 && u <= 0xDFFF)
        {
            rv.push_back(UNI_REPLACEMENT_CHAR);
            replaced = true;
        }
        else
            rv.push_back(u);
    }
    return rv;
}

std::vector<UTF32> codePoints(std::vector<UTF32> units, bool& replaced)
{
    for (auto& u : units)
        if ((u >= 0xD800 && u <= 0xDFFF) || u > UNI_MAX_LEGAL_UTF32)
        {
            u = UNI_REPLACEMENT_CHAR;
            replaced = true;
        }
    return units;
}

template <typename InputType>
std::string from_utf_nn(std::vector<InputType>&& vec, bool& replaced)
{
    const std::vector<UTF32> chars = codePoints(std::move(vec), replaced);
    std::string rv;
    rv.resize(chars.size() * 4);
    const UTF32* sourceStart = chars.data();
    const auto sourceEnd = sourceStart + chars.size();
    const auto targetStart = reinterpret_cast<UTF8*>(rv.data());
    auto targetStart_out = targetStart;
    const auto targetEnd = targetStart + rv.size();
    // Every character is valid now, and 4 bytes are room for any: the conversion cannot fail
    if (ConvertUTF32toUTF8(&sourceStart, sourceEnd, &targetStart_out, targetEnd, ConversionFlags::strictConversion) !=
        ConversionResult::conversionOK)
        THROW(ERR_COMMON, "Cannot convert string : invalid source text")
    rv.resize(targetStart_out - targetStart);
    return rv;
}
}  // namespace

namespace UtfConverter
{

// The length of the valid UTF-8 sequence (RFC 3629) that begins at text[i], or 0 where none does
static size_t validSequenceAt(const std::string& text, const size_t i)
{
    const auto b = static_cast<uint8_t>(text[i]);
    size_t len = 0;
    uint8_t lo = 0x80;  // the allowed range of the second byte
    uint8_t hi = 0xBF;
    if (b < 0x80)
        len = 1;
    else if (b >= 0xC2 && b <= 0xDF)
        len = 2;
    else if (b >= 0xE0 && b <= 0xEF)
    {
        len = 3;
        if (b == 0xE0)
            lo = 0xA0;  // no overlong form
        else if (b == 0xED)
            hi = 0x9F;  // no surrogate
    }
    else if (b >= 0xF0 && b <= 0xF4)
    {
        len = 4;
        if (b == 0xF0)
            lo = 0x90;
        else if (b == 0xF4)
            hi = 0x8F;  // nothing above U+10FFFF
    }
    bool ok = len > 0 && i + len <= text.size();
    for (size_t k = 1; ok && k < len; ++k)
    {
        const auto c = static_cast<uint8_t>(text[i + k]);
        ok = k == 1 ? c >= lo && c <= hi : c >= 0x80 && c <= 0xBF;
    }
    return ok ? len : 0;
}

// Valid UTF-8, which a Matroska text track must hold: a byte that does not begin or continue a valid
// sequence (RFC 3629) becomes U+FFFD. The text has already been converted from the file's encoding;
// this catches a file whose encoding was judged from its first part and changes later, and a file
// that is not UTF-8 on a system with no code page to fall back to. Returns whether a byte was replaced.
bool makeValidUtf8(std::string& text)
{
    std::string out;
    bool replaced = false;
    size_t i = 0;
    while (i < text.size())
    {
        if (const size_t len = validSequenceAt(text, i))
        {
            out.append(text, i, len);
            i += len;
            continue;
        }
        out += static_cast<char>(0xEF);
        out += static_cast<char>(0xBF);
        out += static_cast<char>(0xBD);
        replaced = true;
        ++i;
    }
    if (replaced)
        text.swap(out);
    return replaced;
}

// A line of a source file quoted in a message. Random data on a subtitle line was quoted whole, several hundred bytes
// with an ESC and bytes that are not UTF-8 among them: the message was not valid UTF-8, and a terminal took escape
// sequences from the file. A control character or a byte that is not valid UTF-8 is now written as \xNN, and a line
// longer than maxBytes is cut short with "...".
std::string quotedForMessage(const std::string& text, const size_t maxBytes)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    size_t i = 0;
    while (i < text.size() && out.size() < maxBytes)
    {
        const auto b = static_cast<uint8_t>(text[i]);
        const size_t len = validSequenceAt(text, i);
        if (len == 0 || b < 0x20 || b == 0x7F)
        {
            out += "\\x";
            out += HEX[b >> 4];
            out += HEX[b & 15];
            ++i;
            continue;
        }
        out.append(text, i, len);
        i += len;
    }
    if (i < text.size())
        out += "...";
    return out;
}

// A meta file is read as UTF-8. One saved in another encoding, as Latin-1 on Linux or the ANSI code page of an older
// GUI, gave a --title= or a track-name= whose bytes were written into the Matroska file as they were: K\xE4se for
// "Kaese" with an umlaut, which no player reads as a character. On Windows such a text is read in the active code page,
// as a file name in the same meta file is (toWide); elsewhere there is no code page to read it in, and a byte that is
// not valid UTF-8 becomes U+FFFD (makeValidUtf8). Returns whether the text was not valid UTF-8.
bool metaTextToUtf8(std::string& text)
{
    std::string probe = text;
    if (!makeValidUtf8(probe))
        return false;
#ifdef _WIN32
    text = ::toUtf8(fromAcp(text.c_str(), static_cast<int>(text.size())).data());
#else
    text.swap(probe);
#endif
    return true;
}

std::string toUtf8(const uint8_t* start, const size_t numBytes, const SourceFormat srcFormat, bool* replaced)
{
    bool anyReplaced = false;
    if (replaced)
        *replaced = false;
    std::string rv;
    switch (srcFormat)
    {
    case SourceFormat::sfUTF8:
        return {start, start + numBytes};
    case SourceFormat::sfUTF16be:
        rv = from_utf_nn(make_vector(start, numBytes, read_be16), anyReplaced);
        break;
    case SourceFormat::sfUTF16le:
        rv = from_utf_nn(make_vector(start, numBytes, read_le16), anyReplaced);
        break;
    case SourceFormat::sfUTF32be:
        rv = from_utf_nn(make_vector(start, numBytes, read_be32), anyReplaced);
        break;
    case SourceFormat::sfUTF32le:
        rv = from_utf_nn(make_vector(start, numBytes, read_le32), anyReplaced);
        break;
#ifdef _WIN32
    case SourceFormat::sfANSI:
    {
        return ::toUtf8(fromAcp(reinterpret_cast<const char*>(start), static_cast<int>(numBytes)).data());
    }
#endif
    default:
        THROW(ERR_COMMON, "Unknown parameter to UtfConverter::toUtf8")
    }
    if (replaced)
        *replaced = anyReplaced;
    return rv;
}
}  // namespace UtfConverter
