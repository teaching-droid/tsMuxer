#ifndef LATM_H_
#define LATM_H_

#include <cstdint>
#include <string>
#include <vector>

// AAC in LATM inside LOAS (ISO/IEC 14496-3, 1.7), stream type 0x11 of a transport stream, as a DVB broadcast sends it
// (ETSI TS 101 154, 6.4.1 and 6.5.1.1): each frame the 11 bit sync word 0x2B7, its length, and an AudioMuxElement
// whose StreamMuxConfig gives the AudioSpecificConfig. Such a track could not be read at all: "Can't detect stream
// type". The frames are turned into ADTS frames, which the AAC reader reads, so the track can be listed and muxed.
//
// Read is the form DVB allows: audioMuxVersion 0, one program, one layer, one access unit per frame, frame lengths in
// bytes (frameLengthType 0), and an AAC core (Main, LC, SSR or LTP) of 1024 samples a frame, also under explicit
// HE-AAC or HE-AAC v2, whose core goes into the ADTS header (the SBR then is implicit, as a decoder reads it). Any
// other form is said once (problem()) and gives no frames.
class LatmToAdts
{
   public:
    // Appends the ADTS frames of the bytes given to out. A frame the bytes end inside waits for the next call; the
    // frames before the first StreamMuxConfig cannot be read and are left out.
    void convert(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

    // What the stream uses that is not read, as a sentence; empty while it is read.
    [[nodiscard]] const std::string& problem() const { return m_problem; }

   private:
    struct Config
    {
        int profile = 0;     // the ADTS profile: object type of the core - 1
        int rateIndex = 0;   // the core's samplingFrequencyIndex
        int channelCfg = 0;  // channelConfiguration; 0 with a program config element in the frames
    };

    void frame(const uint8_t* data, int size, std::vector<uint8_t>& out);
    bool readStreamMuxConfig(const uint8_t* data, int size, int& bitPos);

    std::vector<uint8_t> m_pending;  // a frame not yet whole
    Config m_config;
    bool m_haveConfig = false;
    std::string m_problem;
};

#endif  // LATM_H_
