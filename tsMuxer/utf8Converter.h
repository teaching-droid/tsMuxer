// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
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
// Valid UTF-8: a byte that does not begin or continue a valid sequence (RFC 3629) becomes U+FFFD. Returns whether a
// byte was replaced.
bool makeValidUtf8(std::string& text);
// A text from the meta file in valid UTF-8, read in the active code page on Windows. Returns whether it was not UTF-8.
bool metaTextToUtf8(std::string& text);
// A line of a source file as a message quotes it: a control character or a byte that is not UTF-8 as \xNN, and at
// most maxBytes of it, then "...".
std::string quotedForMessage(const std::string& text, size_t maxBytes = 120);
}  // namespace UtfConverter

#endif
