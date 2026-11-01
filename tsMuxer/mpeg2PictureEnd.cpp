#include "pictureEnd.h"

#include <cstddef>
#include <vector>

// The tables are those of H.262 Annex B. Codes are given MSB first, with their length in bits.

namespace
{
struct Vlc
{
    uint32_t code;
    uint8_t len;
    int8_t value;
};

// B.1 macroblock_address_increment, 1 to 33 (macroblock_escape is read before it)
const Vlc addressIncrement[] = {{0b1, 1, 1},
                                {0b011, 3, 2},
                                {0b010, 3, 3},
                                {0b0011, 4, 4},
                                {0b0010, 4, 5},
                                {0b00011, 5, 6},
                                {0b00010, 5, 7},
                                {0b0000111, 7, 8},
                                {0b0000110, 7, 9},
                                {0b00001011, 8, 10},
                                {0b00001010, 8, 11},
                                {0b00001001, 8, 12},
                                {0b00001000, 8, 13},
                                {0b00000111, 8, 14},
                                {0b00000110, 8, 15},
                                {0b0000010111, 10, 16},
                                {0b0000010110, 10, 17},
                                {0b0000010101, 10, 18},
                                {0b0000010100, 10, 19},
                                {0b0000010011, 10, 20},
                                {0b0000010010, 10, 21},
                                {0b00000100011, 11, 22},
                                {0b00000100010, 11, 23},
                                {0b00000100001, 11, 24},
                                {0b00000100000, 11, 25},
                                {0b00000011111, 11, 26},
                                {0b00000011110, 11, 27},
                                {0b00000011101, 11, 28},
                                {0b00000011100, 11, 29},
                                {0b00000011011, 11, 30},
                                {0b00000011010, 11, 31},
                                {0b00000011001, 11, 32},
                                {0b00000011000, 11, 33}};

// macroblock_type: the value holds the flags below
constexpr int MB_QUANT = 16, MB_FORWARD = 8, MB_BACKWARD = 4, MB_PATTERN = 2, MB_INTRA = 1;
// B.2 I-pictures
const Vlc typeI[] = {{0b1, 1, MB_INTRA}, {0b01, 2, MB_QUANT | MB_INTRA}};
// B.3 P-pictures
const Vlc typeP[] = {{0b1, 1, MB_FORWARD | MB_PATTERN},
                     {0b01, 2, MB_PATTERN},
                     {0b001, 3, MB_FORWARD},
                     {0b00011, 5, MB_INTRA},
                     {0b00010, 5, MB_QUANT | MB_FORWARD | MB_PATTERN},
                     {0b00001, 5, MB_QUANT | MB_PATTERN},
                     {0b000001, 6, MB_QUANT | MB_INTRA}};
// B.4 B-pictures
const Vlc typeB[] = {{0b10, 2, MB_FORWARD | MB_BACKWARD},
                     {0b11, 2, MB_FORWARD | MB_BACKWARD | MB_PATTERN},
                     {0b010, 3, MB_BACKWARD},
                     {0b011, 3, MB_BACKWARD | MB_PATTERN},
                     {0b0010, 4, MB_FORWARD},
                     {0b0011, 4, MB_FORWARD | MB_PATTERN},
                     {0b00011, 5, MB_INTRA},
                     {0b00010, 5, MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN},
                     {0b000011, 6, MB_QUANT | MB_FORWARD | MB_PATTERN},
                     {0b000010, 6, MB_QUANT | MB_BACKWARD | MB_PATTERN},
                     {0b000001, 6, MB_QUANT | MB_INTRA}};

// B.9 coded_block_pattern, cbp 0 to 63
const Vlc codedBlockPattern[] = {
    {0b111, 3, 60},       {0b1101, 4, 4},       {0b1100, 4, 8},       {0b1011, 4, 16},      {0b1010, 4, 32},
    {0b10011, 5, 12},     {0b10010, 5, 48},     {0b10001, 5, 20},     {0b10000, 5, 40},     {0b01111, 5, 28},
    {0b01110, 5, 44},     {0b01101, 5, 52},     {0b01100, 5, 56},     {0b01011, 5, 1},      {0b01010, 5, 61},
    {0b01001, 5, 2},      {0b01000, 5, 62},     {0b001111, 6, 24},    {0b001110, 6, 36},    {0b001101, 6, 3},
    {0b001100, 6, 63},    {0b0010111, 7, 5},    {0b0010110, 7, 9},    {0b0010101, 7, 17},   {0b0010100, 7, 33},
    {0b0010011, 7, 6},    {0b0010010, 7, 10},   {0b0010001, 7, 18},   {0b0010000, 7, 34},   {0b00011111, 8, 7},
    {0b00011110, 8, 11},  {0b00011101, 8, 19},  {0b00011100, 8, 35},  {0b00011011, 8, 13},  {0b00011010, 8, 49},
    {0b00011001, 8, 21},  {0b00011000, 8, 41},  {0b00010111, 8, 14},  {0b00010110, 8, 50},  {0b00010101, 8, 22},
    {0b00010100, 8, 42},  {0b00010011, 8, 15},  {0b00010010, 8, 51},  {0b00010001, 8, 23},  {0b00010000, 8, 43},
    {0b00001111, 8, 25},  {0b00001110, 8, 37},  {0b00001101, 8, 26},  {0b00001100, 8, 38},  {0b00001011, 8, 29},
    {0b00001010, 8, 45},  {0b00001001, 8, 53},  {0b00001000, 8, 57},  {0b00000111, 8, 30},  {0b00000110, 8, 46},
    {0b00000101, 8, 54},  {0b00000100, 8, 58},  {0b000000111, 9, 31}, {0b000000110, 9, 47}, {0b000000101, 9, 55},
    {0b000000100, 9, 59}, {0b000000011, 9, 27}, {0b000000010, 9, 39}, {0b000000001, 9, 0}};

// B.10 motion_code: only whether it is 0 matters here (a motion_residual follows any other), so the 33 codes are
// given as the code for 0 and, for 1 to 16, the code of +n; -n is the same code with its last bit 1.
const uint32_t motionCodePositive[17][2] = {{0b1, 1},
                                            {0b010, 3},
                                            {0b0010, 4},
                                            {0b00010, 5},
                                            {0b0000110, 7},
                                            {0b00001010, 8},
                                            {0b00001000, 8},
                                            {0b00000110, 8},
                                            {0b0000010110, 10},
                                            {0b0000010100, 10},
                                            {0b0000010010, 10},
                                            {0b00000100010, 11},
                                            {0b00000100000, 11},
                                            {0b00000011110, 11},
                                            {0b00000011100, 11},
                                            {0b00000011010, 11},
                                            {0b00000011000, 11}};

// B.11 dmvector
const Vlc dmVector[] = {{0b11, 2, -1}, {0b0, 1, 0}, {0b10, 2, 1}};

// B.12 dct_dc_size_luminance, B.13 dct_dc_size_chrominance
const Vlc dcSizeLuma[] = {{0b100, 3, 0},     {0b00, 2, 1},       {0b01, 2, 2},         {0b101, 3, 3},
                          {0b110, 3, 4},     {0b1110, 4, 5},     {0b11110, 5, 6},      {0b111110, 6, 7},
                          {0b1111110, 7, 8}, {0b11111110, 8, 9}, {0b111111110, 9, 10}, {0b111111111, 9, 11}};
const Vlc dcSizeChroma[] = {{0b00, 2, 0},       {0b01, 2, 1},        {0b10, 2, 2},           {0b110, 3, 3},
                            {0b1110, 4, 4},     {0b11110, 5, 5},     {0b111110, 6, 6},       {0b1111110, 7, 7},
                            {0b11111110, 8, 8}, {0b111111110, 9, 9}, {0b1111111110, 10, 10}, {0b1111111111, 10, 11}};

// B.14 and B.15, DCT coefficients: only how many bits each code takes and whether it ends the block or is the escape
// (the run and level it stands for are not needed to find where the block ends). The code is given without its sign
// bit; len counts it. kind: 0 a coefficient, 1 end of block, 2 escape (6 bits run and 12 bits level follow), 3 the
// '1s' code of the first coefficient of a block of a non-intra macroblock (Table zero only).
struct DctCode
{
    uint32_t code;
    uint8_t len;
    uint8_t kind;
};
#include "mpeg2DctTables.inc"

class Bits
{
   public:
    Bits(const uint8_t* data, const uint8_t* end) : m_data(data), m_bits(static_cast<int64_t>(end - data) * 8) {}
    // false once a read went past the end of the data
    [[nodiscard]] bool ok() const { return m_pos <= m_bits; }
    [[nodiscard]] int64_t left() const { return m_bits - m_pos; }
    uint32_t peek(const int n) const
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = m_pos + i;
            v = v << 1 | (p < m_bits ? m_data[p >> 3] >> (7 - (p & 7)) & 1 : 0);
        }
        return v;
    }
    uint32_t get(const int n)
    {
        const uint32_t v = peek(n);
        m_pos += n;
        return v;
    }
    void skip(const int n) { m_pos += n; }
    // whether every bit from here to the end of the data is 0: the stuffing before a start code, or nothing at all
    [[nodiscard]] bool onlyZerosLeft() const
    {
        for (int64_t p = m_pos; p < m_bits; ++p)
            if (m_data[p >> 3] >> (7 - (p & 7)) & 1)
                return false;
        return true;
    }

   private:
    const uint8_t* m_data;
    int64_t m_bits;
    int64_t m_pos = 0;
};

// The value of the code that starts here, or -128 when none of the table does
template <size_t N>
int readVlc(Bits& bits, const Vlc (&table)[N])
{
    for (const Vlc& v : table)
        if (bits.peek(v.len) == v.code)
        {
            bits.skip(v.len);
            return v.value;
        }
    return -128;
}

// -1 when no code matches, else the kind
template <size_t N>
int readDct(Bits& bits, const DctCode (&table)[N], const bool first)
{
    for (const DctCode& c : table)
    {
        if (c.kind == 3 && !first)
            continue;
        if (c.kind == 1 && first)
            continue;  // end of block is not the first code of a block (Note 2)
        // with the sign bit after the code where it has one
        const int codeLen = c.kind == 0 || c.kind == 3 ? c.len - 1 : c.len;
        if (bits.peek(codeLen) == c.code)
        {
            bits.skip(c.len);
            if (c.kind == 2)
                bits.skip(18);
            return c.kind;
        }
    }
    return -1;
}

// What the picture header and its coding extension say, as far as the slices need it
struct Picture
{
    int codingType = 0;  // 1 I, 2 P, 3 B
    int fCode[2][2] = {};
    int structure = 3;  // 1 top field, 2 bottom field, 3 frame
    bool framePredFrameDct = false;
    bool concealmentVectors = false;
    bool intraVlcFormat = false;
};

enum class Mb
{
    done,
    outOfData,  // the data ended inside the macroblock
    broken      // the bits do not follow the syntax
};

// A code that matched no entry of its table: where fewer bits are left than the longest code takes, the data ends
// inside it (the bits past the end read as 0); elsewhere the bits do not follow the syntax.
Mb failed(const Bits& bits) { return bits.left() < 32 ? Mb::outOfData : Mb::broken; }

bool motionVector(Bits& bits, const Picture& pic, const int s, const bool dmv)
{
    for (int t = 0; t < 2; ++t)
    {
        bool zero = false, found = false;
        for (int n = 0; n <= 16 && !found; ++n)
        {
            const uint32_t code = motionCodePositive[n][0];
            const int len = static_cast<int>(motionCodePositive[n][1]);
            const uint32_t next = bits.peek(len);
            if (next == code || (n > 0 && next == (code | 1)))
            {
                bits.skip(len);
                zero = n == 0;
                found = true;
            }
        }
        if (!found)
            return false;
        if (pic.fCode[s][t] != 1 && !zero)
            bits.skip(pic.fCode[s][t] - 1);
        if (dmv && readVlc(bits, dmVector) == -128)
            return false;
    }
    return true;
}

bool motionVectors(Bits& bits, const Picture& pic, const int s, const int count, const bool fieldFormat, const bool dmv)
{
    if (count == 1)
    {
        if (fieldFormat && !dmv)
            bits.skip(1);  // motion_vertical_field_select[0][s]
        return motionVector(bits, pic, s, dmv);
    }
    bits.skip(1);
    if (!motionVector(bits, pic, s, dmv))
        return false;
    bits.skip(1);
    return motionVector(bits, pic, s, dmv);
}

bool block(Bits& bits, const Picture& pic, const bool intra, const bool luma)
{
    if (intra)
    {
        const int size = readVlc(bits, luma ? dcSizeLuma : dcSizeChroma);
        if (size < 0)
            return false;
        bits.skip(size);
    }
    const bool table1 = intra && pic.intraVlcFormat;
    for (bool first = !intra;; first = false)
    {
        const int kind = table1 ? readDct(bits, dctTable1, false) : readDct(bits, dctTable0, first);
        if (kind < 0)
            return false;
        if (kind == 1)
            return true;
        if (!bits.ok())
            return true;  // the caller sees the end of the data
    }
}

Mb macroblock(Bits& bits, const Picture& pic, const int chromaFormat, int& increment)
{
    increment = 0;
    while (bits.peek(11) == 0b00000001000)
    {
        bits.skip(11);
        increment += 33;
    }
    const int inc = readVlc(bits, addressIncrement);
    if (!bits.ok())
        return Mb::outOfData;
    if (inc < 0)
        return failed(bits);
    increment += inc;
    const int type = pic.codingType == 1   ? readVlc(bits, typeI)
                     : pic.codingType == 2 ? readVlc(bits, typeP)
                                           : readVlc(bits, typeB);
    if (!bits.ok())
        return Mb::outOfData;
    if (type < 0)
        return failed(bits);
    const bool intra = type & MB_INTRA, pattern = type & MB_PATTERN;
    const bool forward = type & MB_FORWARD, backward = type & MB_BACKWARD;
    const bool frame = pic.structure == 3;
    // 6.3.17.1, Tables 6-17 and 6-18: the motion vectors of the macroblock
    int count = 1;
    bool fieldFormat = !frame, dmv = false;
    if (forward || backward)
    {
        int motionType = 2;  // frame-based (frame_pred_frame_dct) or field-based in a field picture
        if (!frame || !pic.framePredFrameDct)
            motionType = static_cast<int>(bits.get(2));
        if (frame)
        {
            count = motionType == 1 ? 2 : 1;
            fieldFormat = motionType != 2;
            dmv = motionType == 3;
        }
        else
        {
            count = motionType == 2 ? 2 : 1;
            dmv = motionType == 3;
        }
        if (motionType == 0)
            return bits.ok() ? failed(bits) : Mb::outOfData;  // reserved
    }
    if (frame && !pic.framePredFrameDct && (intra || pattern))
        bits.skip(1);  // dct_type
    if (type & MB_QUANT)
        bits.skip(5);
    if (forward || (intra && pic.concealmentVectors))
        if (!motionVectors(bits, pic, 0, count, fieldFormat, dmv))
            return bits.ok() ? failed(bits) : Mb::outOfData;
    if (backward && !motionVectors(bits, pic, 1, count, fieldFormat, dmv))
        return bits.ok() ? failed(bits) : Mb::outOfData;
    if (intra && pic.concealmentVectors)
        bits.skip(1);  // marker_bit
    const int blocks = chromaFormat == 1 ? 6 : chromaFormat == 2 ? 8 : 12;
    uint32_t coded = intra ? 0xFFFu : 0;
    if (pattern && !intra)
    {
        const int cbp = readVlc(bits, codedBlockPattern);
        if (cbp < 0)
            return bits.ok() ? failed(bits) : Mb::outOfData;
        // block i is coded when bit (blocks - 1 - i) is set, the first six from cbp, the rest from the extra bits
        coded = static_cast<uint32_t>(cbp) << (blocks - 6);
        if (chromaFormat == 2)
            coded |= bits.get(2);
        else if (chromaFormat == 3)
            coded |= bits.get(6);
    }
    for (int i = 0; i < blocks; ++i)
        if (coded >> (blocks - 1 - i) & 1)
        {
            if (!block(bits, pic, intra, i < 4))
                return bits.ok() ? failed(bits) : Mb::outOfData;
            if (!bits.ok())
                return Mb::outOfData;
        }
    return bits.ok() ? Mb::done : Mb::outOfData;
}
}  // namespace

PictureEnd mpeg2PictureEnd(const uint8_t* data, const uint8_t* end, int width, int height, int chromaFormat,
                           bool progressiveSequence)
{
    PictureEnd result;
    // The start codes of the data: 00 00 01 and the code after it
    std::vector<const uint8_t*> units;
    for (const uint8_t* p = data; p + 3 <= end; ++p)
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
            units.push_back(p);
            p += 2;
        }
    Picture pic;
    bool haveExtension = false;
    bool inPicture = false;  // after a picture header: its slices follow
    bool sequenceEnd = false;
    std::vector<std::pair<const uint8_t*, const uint8_t*>> slices;  // each from its start code to the next start code
    for (size_t i = 0; i < units.size(); ++i)
    {
        const uint8_t* u = units[i];
        const uint8_t* uEnd = i + 1 < units.size() ? units[i + 1] : end;
        if (u + 4 > end)
            continue;  // a start code cut after its prefix: nothing of it
        const uint8_t code = u[3];
        Bits bits(u + 4, uEnd);
        if (code == 0xB7)
        {
            // sequence_end_code after a picture: that picture is whole. One comes between the cells of a DVD too.
            if (inPicture)
                sequenceEnd = true;
            continue;
        }
        if (code == 0xB3)
        {
            width = static_cast<int>(bits.get(12));
            height = static_cast<int>(bits.get(12));
        }
        else if (code == 0xB5)
        {
            const uint32_t id = bits.get(4);
            if (id == 1)  // sequence_extension
            {
                bits.skip(8);  // profile_and_level_indication
                progressiveSequence = bits.get(1);
                chromaFormat = static_cast<int>(bits.get(2));
                width = (width & 0xFFF) | static_cast<int>(bits.get(2)) << 12;
                height = (height & 0xFFF) | static_cast<int>(bits.get(2)) << 12;
            }
            else if (id == 5 || id == 9 || id == 10)
                return result;              // scalable: slices with more syntax than followed here
            else if (id == 8 && inPicture)  // picture_coding_extension
            {
                for (auto& s : pic.fCode)
                    for (int& f : s) f = static_cast<int>(bits.get(4));
                bits.skip(2);  // intra_dc_precision
                pic.structure = static_cast<int>(bits.get(2));
                bits.skip(1);  // top_field_first
                pic.framePredFrameDct = bits.get(1);
                pic.concealmentVectors = bits.get(1);
                bits.skip(1);  // q_scale_type
                pic.intraVlcFormat = bits.get(1);
                haveExtension = bits.ok();
            }
        }
        else if (code == 0x00)
        {
            inPicture = true;
            sequenceEnd = false;
            haveExtension = false;
            slices.clear();
            bits.skip(10);
            pic.codingType = static_cast<int>(bits.get(3));
        }
        else if (code >= 0x01 && code <= 0xAF && inPicture)
            slices.emplace_back(u, uEnd);
    }
    if (sequenceEnd)
    {
        result.state = PictureEnd::State::whole;
        return result;
    }
    if (!inPicture || !haveExtension || width <= 0 || height <= 0 || chromaFormat < 1 || chromaFormat > 3 ||
        pic.codingType < 1 || pic.codingType > 3 || pic.structure == 0)
        return result;  // MPEG-1, or not enough of the headers to follow
    const int mbWidth = (width + 15) / 16;
    int mbHeight = progressiveSequence ? (height + 15) / 16 : 2 * ((height + 31) / 32);
    if (pic.structure != 3)
        mbHeight /= 2;
    result.rows = mbHeight;
    if (slices.empty())
    {
        // the data ends after the headers of the picture: nothing of it is there
        result.state = PictureEnd::State::cut;
        return result;
    }
    // Only the last slice can be cut. It runs to the next start code, or to the end of the data, where a whole picture
    // has at most stuffing; a start code cut after 00 00 01 ends it as well.
    const auto [s, sliceEnd] = slices.back();
    Bits bits(s + 4, sliceEnd);
    int row = s[3] - 1;
    if (height > 2800)
        row += static_cast<int>(bits.get(3)) << 7;  // slice_vertical_position_extension
    if (row >= mbHeight)
        return result;
    bits.skip(5);  // quantiser_scale_code
    if (bits.peek(1))
    {
        bits.skip(9);                                    // intra_slice_flag, intra_slice, reserved_bits
        while (bits.peek(1) && bits.ok()) bits.skip(9);  // extra_bit_slice, extra_information_slice
    }
    bits.skip(1);  // extra_bit_slice
    int address = row * mbWidth - 1;
    const int total = mbWidth * mbHeight;
    for (bool firstMb = true;; firstMb = false)
    {
        if (!bits.ok())
        {
            result.state = PictureEnd::State::cut;
            result.row = row + 1;
            return result;
        }
        if (!firstMb && bits.onlyZerosLeft())
            break;  // the slice ends: a start code would follow, or nothing does
        int increment = 0;
        const Mb mb = macroblock(bits, pic, chromaFormat, increment);
        if (mb == Mb::broken)
            return result;
        if (mb == Mb::outOfData)
        {
            // where the macroblock would have been is not known: its address increment may be cut as well
            result.state = PictureEnd::State::cut;
            result.row = (firstMb ? row : (address + 1) / mbWidth) + 1;
            return result;
        }
        const int at = address + increment;
        if (at >= total || (firstMb && at / mbWidth != row))
            return result;
        address = at;
    }
    if (address == total - 1)
        result.state = PictureEnd::State::whole;
    else
    {
        // the slices stop before the last macroblock: rows or the end of a row are not there
        result.state = PictureEnd::State::cut;
        result.betweenSlices = true;
        result.row = address / mbWidth + 1;
    }
    return result;
}
