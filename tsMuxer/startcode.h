// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef START_CODE_H_
#define START_CODE_H_

#include <cstdint>
#include <cstring>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define TSMUXER_STARTCODE_SSE2 1
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace startcode_detail
{
inline unsigned ctz32(const unsigned v)
{
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanForward(&idx, v);
    return idx;
#else
    return static_cast<unsigned>(__builtin_ctz(v));
#endif
}

inline unsigned ctz64(const uint64_t v)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long idx;
    _BitScanForward64(&idx, v);
    return idx;
#elif defined(_MSC_VER)
    const auto lo = static_cast<unsigned>(v);
    return lo ? ctz32(lo) : 32 + ctz32(static_cast<unsigned>(v >> 32));
#else
    return static_cast<unsigned>(__builtin_ctzll(v));
#endif
}
}  // namespace startcode_detail

// Position of the 01 byte of the first 00 00 01 pattern whose 01 lies in [buffer + 2, end),
// or nullptr when there is none. Shared primitive behind every start-code scanner; the
// callers add their own return conventions (marker+1, marker-2, 4-byte back-off) on top.
// Candidates are located by the 01 byte, so runs of zeros (the scalar loop's slow case)
// are skipped a whole block at a time and no pattern can straddle a block boundary.
inline uint8_t* findStartCodeMarker(uint8_t* buffer, uint8_t* end)
{
    if (end - buffer < 3)
        return nullptr;
    uint8_t* const first = buffer + 2;  // lowest valid 01 position
    uint8_t* const last = end - 1;      // highest valid 01 position

#ifdef TSMUXER_STARTCODE_SSE2
    // Blocks of 16 bytes, all inside [first, end), then the rest byte by byte. The blocks were aligned loads that
    // began before `first` and ran past `end`: they could not fault, an aligned block never crossing a page, and the
    // bytes outside were rejected, but they were read all the same, outside the buffer. An address sanitizer reported
    // it as an overflow of the buffer on every call near its end, which hid any real fault behind it. The unaligned
    // load is as fast, and the first match is the same.
    const __m128i ones = _mm_set1_epi8(1);
    uint8_t* q = first;
    for (; end - q >= 16; q += 16)
    {
        unsigned mask = static_cast<unsigned>(
            _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(q)), ones)));
        while (mask)
        {
            uint8_t* c = q + startcode_detail::ctz32(mask);
            mask &= mask - 1;
            if (c[-1] == 0 && c[-2] == 0)
                return c;
        }
    }
    for (; q <= last; ++q)
        if (*q == 1 && q[-1] == 0 && q[-2] == 0)
            return q;
    return nullptr;
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // Big endian: the SWAR bit order would visit candidates out of order; use the
    // plain scalar loop.
    for (uint8_t* c = first; c <= last; ++c)
        if (*c == 1 && c[-1] == 0 && c[-2] == 0)
            return c;
    return nullptr;
#else
    // SWAR fallback (little endian): high bit marks each byte equal to 1.
    uint8_t* c = first;
    while (c <= last && (reinterpret_cast<uintptr_t>(c) & 7) != 0)
    {
        if (*c == 1 && c[-1] == 0 && c[-2] == 0)
            return c;
        ++c;
    }
    while (c + 7 <= last)
    {
        uint64_t x;
        memcpy(&x, c, 8);
        const uint64_t t = x ^ 0x0101010101010101ull;
        uint64_t hit = (t - 0x0101010101010101ull) & ~t & 0x8080808080808080ull;
        while (hit)
        {
            uint8_t* cc = c + (startcode_detail::ctz64(hit) >> 3);
            hit &= hit - 1;
            // The value test is NOT redundant. The borrow chain in (t - ones) & ~t & high
            // propagates out of a matching byte and marks the bytes that follow it, so a
            // payload 0x01 ahead of a 4-byte start code (... 01 | 00 00 00 01) also flags the
            // zeros. Without re-checking the byte we would return a pointer to a 0x00 and
            // start the NAL one byte early. Every other branch in this function tests it too.
            if (*cc == 1 && cc[-1] == 0 && cc[-2] == 0)
                return cc;
        }
        c += 8;
    }
    for (; c <= last; ++c)
        if (*c == 1 && c[-1] == 0 && c[-2] == 0)
            return c;
    return nullptr;
#endif
}

#endif  // START_CODE_H_
