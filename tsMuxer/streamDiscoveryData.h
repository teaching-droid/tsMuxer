#ifndef STREAM_DISCOVERY_DATA_H_
#define STREAM_DISCOVERY_DATA_H_

#include <cstdint>
#include <string>
#include <vector>

/// Metadata collected during the discovery (probe) phase for a single track.
/// This is populated by probeStream() / discoverStreams() before the main
/// mux pipeline starts, so that stream readers have correct properties
/// (channels, sample rate, resolution, HDR, etc.) from the very first packet.
struct StreamDiscoveryData
{
    // Identity
    std::string codecName;  // e.g. "A_OPUS", "V_MPEGH/ISO/HEVC"
    int32_t trackID = 0;

    // Container-level codec private data (OpusHead, FLAC STREAMINFO, AVC SPS/PPS, etc.)
    std::vector<uint8_t> codecPrivate;

    // Audio properties (filled for audio codecs)
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int bitrate = 0;
    // True when an AC-3 track turned out to be TrueHD, i.e. an AC-3 core with a lossless MLP
    // substream behind it. Carried here for the same reason as the Dolby Vision flags below: it is
    // discovered while probing, with test mode on, and is NOT otherwise re-derived during muxing.
    bool isTrueHD = false;

    // Video properties (filled for video codecs)
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool interlaced = false;
    int hdrType = 0;  // maps to getStreamHDR() return values
    // Dolby Vision layer flags. Carried here because they are discovered while probing and are
    // NOT re-derived during muxing: the RPU sits at the end of an access unit, so by the time a
    // muxer writes its header the reader has not necessarily seen one yet.
    bool isDVRPU = false;
    bool isDVEL = false;
    // What the stream's first readable RPU says: 1 an enhancement layer is part of the picture (profile 7), 0 a
    // single layer, -1 no RPU read.
    int dvRpuElPresent = -1;
    int profile = 0;
    int level = 0;

    // Stream description string (from getStreamInfo())
    std::string streamDescr;

    // True if the probe phase succeeded for this track
    bool discovered = false;
};

#endif
