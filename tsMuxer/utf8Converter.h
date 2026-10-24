#ifndef UTFCONVERTER_H_
#define UTFCONVERTER_H_

#include <types/types.h>

#include <string>

namespace UtfConverter
{
enum class SourceFormat
{
    sfUnknown,
#ifdef _WIN32
    sfANSI,  // currently active code page (CP_ACP)
#endif
    sfUTF8,
    sfUTF16le,
    sfUTF16be,
    sfUTF32le,
    sfUTF32be
};

// The text in UTF-8. A UTF-16 or UTF-32 code unit that is no character becomes U+FFFD, and *replaced, when given, says
// whether one did.
std::string toUtf8(const uint8_t* start, size_t numBytes, SourceFormat srcFormat, bool* replaced = nullptr);
}  // namespace UtfConverter

#endif
