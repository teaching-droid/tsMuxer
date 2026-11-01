#ifndef PICTURE_END_BITS_H_
#define PICTURE_END_BITS_H_

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

// The bit reading shared by the picture-end readers of H.264 and HEVC (pictureEnd.h)
namespace picture_end
{
// Thrown when the slice cannot be followed: outOfData when the data ends inside it, otherwise its bits break the syntax
struct Stop
{
    bool outOfData;
};

// The bits of a NAL unit with its emulation prevention bytes removed. A read past the end throws: a whole slice never
// reads past its last bit (its rbsp_stop_one_bit), so that is the data ending inside it.
class Rbsp
{
   public:
    explicit Rbsp(std::vector<uint8_t> data) : m_d(std::move(data)), m_bits(static_cast<int64_t>(m_d.size()) * 8)
    {
        m_lastOne = m_bits - 1;
        while (m_lastOne >= 0 && !bitAt(m_lastOne)) --m_lastOne;
    }
    uint32_t bit()
    {
        if (m_pos >= m_bits)
            throw Stop{true};
        return bitAt(m_pos++);
    }
    uint32_t get(int n)
    {
        uint32_t v = 0;
        while (n-- > 0) v = v << 1 | bit();
        return v;
    }
    [[nodiscard]] uint32_t peek(const int n) const
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = m_pos + i;
            v = v << 1 | (p < m_bits ? bitAt(p) : 0);
        }
        return v;
    }
    void skip(const int64_t n)
    {
        if (m_pos + n > m_bits)
            throw Stop{true};
        m_pos += n;
    }
    uint32_t ue()
    {
        int zeros = 0;
        while (!bit())
            if (++zeros > 31)
                throw Stop{false};
        return zeros ? (1u << zeros) - 1 + get(zeros) : 0;
    }
    int32_t se()
    {
        const uint32_t k = ue();
        return k & 1 ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    // a code of the table, sorted by length; none: past the end when the longest code would reach past it
    template <typename Code, size_t N>
    const Code& vlc(const Code (&table)[N])
    {
        for (const Code& v : table)
            if (peek(v.len) == v.code)
            {
                skip(v.len);
                return v;
            }
        throw Stop{m_pos + table[N - 1].len > m_bits};
    }
    [[nodiscard]] bool moreRbspData() const { return m_pos < m_lastOne; }
    [[nodiscard]] int64_t pos() const { return m_pos; }
    [[nodiscard]] int64_t lastOne() const { return m_lastOne; }
    [[nodiscard]] bool aligned() const { return (m_pos & 7) == 0; }
    // every byte after the one with the last bit read is 0: cabac_zero_words, trailing zero bytes. The bits after the
    // stop bit in its own byte are not looked at: an encoder may leave other bits there.
    [[nodiscard]] bool restZeroAfterByte() const { return m_lastOne < ((m_pos + 7) & ~int64_t{7}); }
    [[nodiscard]] const std::vector<uint8_t>& data() const { return m_d; }

   private:
    // a bit inside the data (its position is below m_bits, so its byte is an index of m_d)
    [[nodiscard]] uint32_t bitAt(const int64_t p) const
    {
        return m_d[static_cast<size_t>(p >> 3)] >> (7 - (p & 7)) & 1;
    }
    std::vector<uint8_t> m_d;
    int64_t m_bits;
    int64_t m_pos = 0;
    int64_t m_lastOne;
};

inline std::vector<uint8_t> unescape(const uint8_t* p, const uint8_t* end)
{
    std::vector<uint8_t> out;
    out.reserve(end - p);
    int zeros = 0;
    for (; p < end; ++p)
    {
        if (zeros >= 2 && *p == 3)
        {
            zeros = 0;
            continue;
        }
        zeros = *p == 0 ? zeros + 1 : 0;
        out.push_back(*p);
    }
    return out;
}

// The arithmetic decoding engine of CABAC, the same in H.264 (9.3.3.2) and HEVC (9.3.4.3); its tables are those of
// H.264 Tables 9-44 and 9-45, which HEVC repeats as Tables 9-52 and 9-53 (read from both texts and compared)
inline const uint8_t rangeTabLps[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205}, {116, 142, 169, 195},
    {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166}, {95, 116, 137, 158},  {90, 110, 130, 150},
    {85, 104, 123, 142},  {81, 99, 117, 135},   {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},
    {66, 80, 95, 110},    {62, 76, 90, 104},    {59, 72, 86, 99},     {56, 69, 81, 94},     {53, 65, 77, 89},
    {51, 62, 73, 85},     {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},     {41, 50, 59, 69},
    {39, 48, 56, 65},     {37, 45, 54, 62},     {35, 43, 51, 59},     {33, 41, 48, 56},     {32, 39, 46, 53},
    {30, 37, 43, 50},     {29, 35, 41, 48},     {27, 33, 39, 45},     {26, 31, 37, 43},     {24, 30, 35, 41},
    {23, 28, 33, 39},     {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},     {19, 23, 27, 31},
    {18, 22, 26, 30},     {17, 21, 25, 28},     {16, 20, 23, 27},     {15, 19, 22, 25},     {14, 18, 21, 24},
    {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},     {12, 14, 17, 20},     {11, 14, 16, 19},
    {11, 13, 15, 18},     {10, 12, 15, 17},     {10, 12, 14, 16},     {9, 11, 13, 15},      {9, 11, 12, 14},
    {8, 10, 12, 14},      {8, 9, 11, 13},       {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},         {2, 2, 2, 2}};
inline const uint8_t transIdxLps[64] = {0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
                                        13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
                                        24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
                                        33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};
}  // namespace picture_end

#endif
