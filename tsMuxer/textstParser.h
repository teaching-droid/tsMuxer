#ifndef TEXTST_PARSER_H_
#define TEXTST_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

// A Blu-ray text subtitle track (TextST), stream coding type 0x92. It is a clip of its own: each PES of private stream
// 2 holds one segment, of a type (1 byte), a length (2 bytes) and a body. Segment 0x81 is the dialog style, one for the
// clip and first; each segment 0x82 is one subtitle: its start and end, and the text of its regions.
namespace textst
{
constexpr uint8_t SEGMENT_STYLE = 0x81;
constexpr uint8_t SEGMENT_PRESENTATION = 0x82;

struct Subtitle
{
    int64_t start = 0;  // 90 kHz, on the clock of the clip
    int64_t end = 0;
    std::string text;  // UTF-8, its lines parted by \n
    int regionStyle = 0;  // of its first region
};

// The payload of a PES, if it holds a whole segment: its type, 0 otherwise
uint8_t segmentType(const uint8_t* data, size_t size);

// One presentation segment, its 3 byte header included. False for one that is cut off or that this reader cannot follow.
bool parsePresentation(const uint8_t* data, size_t size, Subtitle& subtitle);

// The region styles of a dialog style segment that sit at the top of the picture
std::set<int> topRegionStyles(const uint8_t* data, size_t size);

// The record the SRT reader takes for a subtitle: its number, its times and its text, {\an8} in front for the top
std::string srtRecord(const Subtitle& subtitle, int number, bool top);
}  // namespace textst

#endif
