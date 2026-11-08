// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "textstParser.h"

#include <cstdio>

namespace textst
{
namespace
{
int64_t readPts(const uint8_t* p)
{
    return static_cast<int64_t>(p[0] & 1) << 32 | static_cast<int64_t>(p[1]) << 24 | static_cast<int64_t>(p[2]) << 16 |
           static_cast<int64_t>(p[3]) << 8 | p[4];
}

std::string timeText(const int64_t pts)
{
    const int64_t ms = (pts + 45) / 90;
    char text[32];
    snprintf(text, sizeof text, "%02d:%02d:%02d,%03d", static_cast<int>(ms / 3600000),
             static_cast<int>(ms / 60000 % 60), static_cast<int>(ms / 1000 % 60), static_cast<int>(ms % 1000));
    return text;
}

// The inline entries of a region: 0x1B, a type, a length and that many bytes. Text is type 1, a line break type 0x0A;
// the other types change the look of the text, which the text itself does not need.
void readRegion(const uint8_t* data, const size_t size, std::string& text)
{
    size_t pos = 0;
    while (pos + 3 <= size)
    {
        if (data[pos] != 0x1B)
        {
            ++pos;
            continue;
        }
        const uint8_t type = data[pos + 1];
        const size_t length = data[pos + 2];
        pos += 3;
        if (pos + length > size)
            return;
        if (type == 0x01)
            text.append(reinterpret_cast<const char*>(data + pos), length);
        else if (type == 0x0A && !text.empty() && text.back() != '\n')
            text += '\n';
        pos += length;
    }
}
}  // namespace

uint8_t segmentType(const uint8_t* data, const size_t size)
{
    if (size < 3 || 3 + (static_cast<size_t>(data[1]) << 8 | data[2]) > size)
        return 0;
    return data[0];
}

bool parsePresentation(const uint8_t* data, const size_t size, Subtitle& subtitle)
{
    if (segmentType(data, size) != SEGMENT_PRESENTATION)
        return false;
    const size_t end = 3 + (static_cast<size_t>(data[1]) << 8 | data[2]);
    // start and end (5 bytes each), a flag byte and the number of regions. The flag byte is 0 on all the discs read;
    // with it set a palette follows, whose length is not known here.
    if (end < 3 + 12 || data[13] != 0)
        return false;
    subtitle.start = readPts(data + 3);
    subtitle.end = readPts(data + 8);
    subtitle.text.clear();
    const int regions = data[14];
    size_t pos = 15;
    for (int i = 0; i < regions; ++i)
    {
        // flags, the region style and the length of the data
        if (pos + 4 > end)
            return false;
        const size_t length = static_cast<size_t>(data[pos + 2]) << 8 | data[pos + 3];
        if (i == 0)
            subtitle.regionStyle = data[pos + 1];
        pos += 4;
        if (pos + length > end)
            return false;
        if (!subtitle.text.empty() && subtitle.text.back() != '\n')
            subtitle.text += '\n';
        readRegion(data + pos, length, subtitle.text);
        pos += length;
    }
    while (!subtitle.text.empty() && subtitle.text.back() == '\n') subtitle.text.pop_back();
    return subtitle.end >= subtitle.start;
}

std::set<int> topRegionStyles(const uint8_t* data, const size_t size)
{
    // 3 byte header, 2 flag bytes, the number of region styles and of user styles, then 29 bytes for each region
    // style: its id, x, y, width, height, background, text box, flow, horizontal and vertical alignment and the font.
    std::set<int> top;
    constexpr size_t RECORD = 29;
    if (size < 7)
        return top;
    for (size_t i = 0, pos = 7; i < data[5] && pos + RECORD <= size; ++i, pos += RECORD)
    {
        const int y = data[pos + 3] << 8 | data[pos + 4];
        const int height = data[pos + 7] << 8 | data[pos + 8];
        if (data[pos + 21] == 1 && y + height / 2 < 540)  // aligned to its top, in the upper half of 1080 lines
            top.insert(data[pos]);
    }
    return top;
}

std::string srtRecord(const Subtitle& subtitle, const int number, const bool top)
{
    std::string rez = number == 1 ? "\xEF\xBB\xBF" : "";
    rez += std::to_string(number) + "\n" + timeText(subtitle.start) + " --> " + timeText(subtitle.end) + "\n";
    return rez + (top ? "{\\an8}" : "") + subtitle.text + "\n\n";
}
}  // namespace textst
