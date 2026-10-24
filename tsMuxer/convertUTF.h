#ifndef CONVERT_UTF_
#define CONVERT_UTF_

/*
 * Copyright 2001-2004 Unicode, Inc.
 *
 * Disclaimer
 *
 * This source code is provided as is by Unicode, Inc. No claims are
 * made as to fitness for any particular purpose. No warranties of any
 * kind are expressed or implied. The recipient agrees to determine
 * applicability of information provided. If this file has been
 * purchased on magnetic or optical media from Unicode, Inc., the
 * sole remedy for any claim will be exchange of defective media
 * within 90 days of receipt.
 *
 * Limitations on Rights to Redistribute This Code
 *
 * Unicode, Inc. hereby grants the right to freely use the information
 * supplied in this file in the creation of products supporting the
 * Unicode Standard, and to make copies of this file in any form
 * for internal or external distribution as long as this notice
 * remains attached.
 */

/* ---------------------------------------------------------------------

    Conversions between UTF32, UTF-16, and UTF-8.  Header file.

    Several funtions are included here, forming a complete set of
    conversions between the three formats.  UTF-7 is not included
    here, but is handled in a separate source file.

    Each of these routines takes pointers to input buffers and output
    buffers.  The input buffers are const.

    Each routine converts the text between *sourceStart and sourceEnd,
    putting the result into the buffer between *targetStart and
    targetEnd. Note: the end pointers are *after* the last item: e.g.
    *(sourceEnd - 1) is the last item.

    The return result indicates whether the conversion was successful,
    and if not, whether the problem was in the source or target buffers.
    (Only the first encountered problem is indicated.)

    After the conversion, *sourceStart and *targetStart are both
    updated to point to the end of last text successfully converted in
    the respective buffers.

    Input parameters:
        sourceStart - pointer to a pointer to the source buffer.
                The contents of this are modified on return so that
                it points at the next thing to be converted.
        targetStart - similarly, pointer to pointer to the target buffer.
        sourceEnd, targetEnd - respectively pointers to the ends of the
                two buffers, for overflow checking only.

    These conversion functions take a ConversionFlags argument. When this
    flag is set to strict, both irregular sequences and isolated surrogates
    will cause an error.  When the flag is set to lenient, both irregular
    sequences and isolated surrogates are converted.

    Whether the flag is strict or lenient, all illegal sequences will cause
    an error return. This includes sequences such as: <F4 90 80 80>, <C0 80>,
    or <A0> in UTF-8, and values above 0x10FFFF in UTF-32. Conformant code
    must check for illegal sequences.

    When the flag is set to lenient, characters over 0x10FFFF are converted
    to the replacement character; otherwise (when the flag is set to strict)
    they constitute an error.

    Output parameters:
        The value "sourceIllegal" is returned from some routines if the input
        sequence is malformed.  When "sourceIllegal" is returned, the source
        value will point to the illegal value that caused the problem. E.g.,
        in UTF-8 when a sequence is malformed, it points to the start of the
        malformed sequence.

    Author: Mark E. Davis, 1994.
    Rev History: Rick McGowan, fixes & updates May 2001.
                 Fixes & updates, Sept 2001.

------------------------------------------------------------------------ */

/* ---------------------------------------------------------------------
    The following 4 definitions are compiler-specific.
    The C standard does not guarantee that wchar_t has at least
    16 bits, so wchar_t is no less portable than unsigned short!
    All should be unsigned values to avoid sign extension during
    bit mask & shift operations.
------------------------------------------------------------------------ */

#include <cstdint>
#include <string>
#include <tuple>

namespace convertUTF
{
typedef std::uint32_t UTF32; /* at least 32 bits */
typedef std::uint16_t UTF16; /* at least 16 bits */
typedef std::uint8_t UTF8;   /* typically 8 bits */
typedef bool Boolean;        /* 0 or 1 */

/* Some fundamental constants */
#define UNI_REPLACEMENT_CHAR (UTF32)0x0000FFFD
#define UNI_MAX_BMP (UTF32)0x0000FFFF
#define UNI_MAX_UTF16 (UTF32)0x0010FFFF
#define UNI_MAX_UTF32 (UTF32)0x7FFFFFFF
#define UNI_MAX_LEGAL_UTF32 (UTF32)0x0010FFFF

enum class ConversionResult
{
    conversionOK,    /* conversion successful */
    sourceExhausted, /* partial character in source, but hit end */
    targetExhausted, /* insuff. room in target for conversion */
    sourceIllegal    /* source sequence is illegal/malformed */
};

enum class ConversionFlags
{
    strictConversion = 0,
    lenientConversion
};

ConversionResult ConvertUTF8toUTF16(const UTF8** sourceStart, const UTF8* sourceEnd, UTF16** targetStart,
                                    const UTF16* targetEnd, ConversionFlags flags);

ConversionResult ConvertUTF16toUTF8(const UTF16** sourceStart, const UTF16* sourceEnd, UTF8** targetStart,
                                    const UTF8* targetEnd, ConversionFlags flags);

ConversionResult ConvertUTF8toUTF32(const UTF8** sourceStart, const UTF8* sourceEnd, UTF32** targetStart,
                                    const UTF32* targetEnd, ConversionFlags flags);

ConversionResult ConvertUTF32toUTF8(const UTF32** sourceStart, const UTF32* sourceEnd, UTF8** targetStart,
                                    const UTF8* targetEnd, ConversionFlags flags);

ConversionResult ConvertUTF16toUTF32(const UTF16** sourceStart, const UTF16* sourceEnd, UTF32** targetStart,
                                     const UTF32* targetEnd, ConversionFlags flags);

ConversionResult ConvertUTF32toUTF16(const UTF32** sourceStart, const UTF32* sourceEnd, UTF16** targetStart,
                                     const UTF16* targetEnd, ConversionFlags flags);

std::tuple<UTF16, UTF16> ConvertUTF32toUTF16(UTF32);

Boolean isLegalUTF8Sequence(const UTF8* source, const UTF8* sourceEnd);

Boolean isLegalUTF8String(const UTF8* string, size_t length);

/*
 * Index into the table below with the first byte of a UTF-8 sequence to
 * get the number of trailing bytes that are supposed to follow it.
 * Note that *legal* UTF-8 values can't have 4 or 5-bytes. The table is
 * left as-is for anyone who may want to do such conversion, which was
 * allowed in earlier algorithms.
 */
const char trailingBytesForUTF8[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};

/*
 * Magic values subtracted from a buffer value during UTF8 conversion.
 * This table contains as many values as there might be trailing bytes
 * in a UTF-8 sequence.
 */
constexpr UTF32 offsetsFromUTF8[6] = {0x00000000UL, 0x00003080UL, 0x000E2080UL,
                                      0x03C82080UL, 0xFA082080UL, 0x82082080UL};

// Every character of the text in turn, as a code point, until f returns false.
//
// ** TEXT THAT IS NOT VALID UTF-8 IS NEVER READ PAST ITS END. ** The lead byte of a sequence used to say how many bytes
// followed it, and that many were read, whatever they were and wherever the text ended. A subtitle in a legacy code
// page is such text: on Linux and macOS an SRT file that is neither UTF-8 nor UTF-16 nor UTF-32 is read as UTF-8
// ("Failed to auto-detect SRT encoding : falling back to UTF-8"), and its letters beyond ASCII are single bytes from
// 0x80 up. In "Caf\xE9 und Gr\xFC\xDFe", cp1252 for "Cafe und Grusse" with their accents, 0xFC claims five more bytes
// where two are left, so the read went past the end of the text. The loop stops only where it meets the end exactly,
// so nothing stopped it after that: every byte of the memory after the string was measured as a character until one
// could not be read, and drawing such subtitles ended with a segmentation fault in getTextSize, for files in cp1252,
// cp1251, GBK, Shift-JIS and Big5 alike. Where no letter sat that near the end of a line, each one took the bytes
// after it into one wrong character, and the subtitle was drawn with letters missing.
//
// A byte that does not begin a valid sequence (RFC 3629: no overlong form, no surrogate, nothing above U+10FFFF), or
// begins one the text cuts short, is now one U+FFFD REPLACEMENT CHARACTER, and the next byte is read. That is the rule
// a subtitle kept as text is written by (makeValidUtf8 in srtStreamReader.cpp), so a subtitle drawn and the same one
// kept as text show the same characters. Valid UTF-8 reads exactly as before. The replacement character, not each
// byte taken as a letter of some code page: Linux and macOS have no code page to take, a guess such as cp1252 would
// draw Cyrillic or CJK text as other letters with no sign that anything is wrong, and the mark shows at a glance that
// the file is not UTF-8. Windows reads such an SRT file in the active code page before it is drawn.
template <typename Fn>
void IterateUTF8Chars(const std::string& utf8String, Fn f)
{
    const auto* p = reinterpret_cast<const unsigned char*>(utf8String.data());
    const auto* const end = p + utf8String.size();
    bool keep_going = true;
    while (keep_going && p != end)
    {
        const unsigned char lead = *p;
        size_t len = 0;           // the bytes of the sequence this one begins, 0 for none
        unsigned char lo = 0x80;  // the allowed range of the second byte
        unsigned char hi = 0xBF;
        if (lead < 0x80)
            len = 1;
        else if (lead >= 0xC2 && lead <= 0xDF)
            len = 2;
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            len = 3;
            if (lead == 0xE0)
                lo = 0xA0;  // no overlong form
            else if (lead == 0xED)
                hi = 0x9F;  // no surrogate
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            len = 4;
            if (lead == 0xF0)
                lo = 0x90;
            else if (lead == 0xF4)
                hi = 0x8F;  // nothing above U+10FFFF
        }
        bool ok = len > 0 && static_cast<size_t>(end - p) >= len;
        for (size_t k = 1; ok && k < len; ++k) ok = k == 1 ? p[k] >= lo && p[k] <= hi : p[k] >= 0x80 && p[k] <= 0xBF;
        UTF32 ch = UNI_REPLACEMENT_CHAR;
        if (ok)
        {
            ch = static_cast<UTF32>(len == 1 ? lead : lead & (0x7F >> len));
            for (size_t k = 1; k < len; ++k) ch = ch << 6 | static_cast<UTF32>(p[k] & 0x3F);
            p += len;
        }
        else
            ++p;
        keep_going = f(ch);
    }
}

}  // namespace convertUTF

#endif
/* --------------------------------------------------------------------- */
