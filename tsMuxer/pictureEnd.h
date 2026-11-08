// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef PICTURE_END_H_
#define PICTURE_END_H_

#include <cstdint>
#include <vector>

// Whether the last picture of a raw video stream is whole, read from its slices as the syntax of its standard gives
// them (no picture is decoded). A raw file cut at a byte position ends inside a picture, and that part was muxed as the
// last picture without a word (K19).
//
// It says "cut" only when that is certain: the data of the last slice ends inside a macroblock or coding tree unit, or
// the slices stop before the end of the picture, which every profile these readers follow rules out for a whole
// picture. Anything it cannot follow (a profile or tool it does not read, data that breaks the syntax before its end,
// a picture larger than the data given) gives "unknown", never "cut".
struct PictureEnd
{
    enum class State
    {
        whole,
        cut,
        unknown
    };
    State state = State::unknown;
    int row = 0;   // cut: the row of macroblocks (coding tree units) the data ends in, counted from 1; 0 before any
    int rows = 0;  // the rows of the picture (of a field, for a field picture)
    // cut: the data ends after a whole slice, and the slices of the rest of the picture are not there
    bool betweenSlices = false;
};

// The end of an MPEG-2 video stream (H.262), from at least the start of its last picture (its picture header, or a
// sequence header or GOP header before it) to the end of the file. width, height, chromaFormat (1 4:2:0, 2 4:2:2,
// 3 4:4:4) and progressiveSequence come from the sequence header and extension in force; a sequence header in the
// data replaces them.
PictureEnd mpeg2PictureEnd(const uint8_t* data, const uint8_t* end, int width, int height, int chromaFormat,
                           bool progressiveSequence);

// The end of an H.264 stream in the byte stream format of Annex B, from at least the first slice of its last picture to
// the end of the file. paramSets: the sequence and picture parameter sets in force before the data (each a NAL unit
// without its start code, emulation prevention bytes removed); those in the data replace them.
PictureEnd h264PictureEnd(const uint8_t* data, const uint8_t* end, const std::vector<std::vector<uint8_t>>& paramSets);

// The end of an HEVC stream in the byte stream format of Annex B, from at least the first slice segment of its last
// picture to the end of the file. paramSets: the sequence and picture parameter sets in force before the data (each a
// NAL unit as the stream carries it, without its start code); those in the data replace them.
PictureEnd hevcPictureEnd(const uint8_t* data, const uint8_t* end, const std::vector<std::vector<uint8_t>>& paramSets);

#endif
