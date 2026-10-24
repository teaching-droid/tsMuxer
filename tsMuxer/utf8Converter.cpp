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
// valid UTF-8 does in a UTF-8 file (makeValidUtf8 in srtStreamReader.cpp).
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
