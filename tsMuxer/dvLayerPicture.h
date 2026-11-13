// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef DV_LAYER_PICTURE_H_
#define DV_LAYER_PICTURE_H_

#include <cstdint>
#include <string>
#include <vector>

// The picture of a made Dolby Vision enhancement layer.
//
// A UHD Blu-ray player takes Dolby Vision from two layers: the film, and a second stream of a quarter of its size
// that carries the Dolby Vision data (RPU) of each picture. A single layer file (profile 8) has that data inside the
// film's own stream, and on a disc it plays without Dolby Vision. Its second layer can be made: in the kind of layer
// that adds nothing to the picture (MEL) only the RPU counts, and the picture it rides on can be the same black
// picture every time. This holds that one picture; HevcDolbyVisionFilter writes it once for every picture of the
// film, each time with the RPU of that picture behind it.
//
// tsMuxeR does not hold such a picture itself: it is given, as a file or made by an encoder the user has.
namespace dvLayer
{
// The picture as it is written for each picture of the film: its parameter sets and its slices, each behind a start
// code of four bytes. Empty when none was given.
const std::vector<uint8_t>& picture();

// Takes the picture from the bytes of an HEVC stream of one key picture. What is not parameter set or slice (an
// access unit delimiter, messages) is left out. Returns the reason when the bytes are not a picture the layer of a
// disc can be made of: one key picture, Main 10, 1920x1080. Empty when it was taken.
std::string take(const uint8_t* data, size_t size);

// The same from a file
std::string takeFile(const std::string& name);

// Has the picture made by an ffmpeg the user has, started once: the one given (--ffmpeg), else one beside tsMuxeR,
// else one on the search path. The encoders are tried in turn: libx265, then those of the graphics hardware
// (NVIDIA, Intel, AMD), which are told the colour of the picture by a filter. fps is the frame rate of the film.
// Returns the reason when no picture could be made; empty when it was.
std::string make(const std::string& ffmpegGiven, double fps);
}  // namespace dvLayer

#endif  // DV_LAYER_PICTURE_H_
