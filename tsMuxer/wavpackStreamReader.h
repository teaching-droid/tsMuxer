// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef WAVPACK_STREAM_READER_H_
#define WAVPACK_STREAM_READER_H_

#include <vector>

#include "simplePacketizerReader.h"

// WavPack audio, read from Matroska (A_WAVPACK4) and copied as it is into a Matroska output; nothing is decoded. A
// Matroska block holds one WavPack frame without the parts of its block headers Matroska leaves out: block_samples,
// flags and crc, then the data, and for more than 2 channels the further blocks of the frame each with flags, crc and
// their size (the codec mapping of Matroska, A_WAVPACK4). The Matroska parser (ParsedMarkedTrackData) puts a 4 byte
// big endian length in front of each frame, and in front of the first the mark "WVPKCPRV" with this track's
// CodecPrivate (the 2 byte version of the WavPack headers) followed by its sample rate (4 bytes), channels and bits per
// sample from the track header, little endian, which the frames do not all carry.
//
// WavPack has no form in a transport stream or on a Blu-ray disc: a Matroska output is the only one that carries it
// (MuxerManager::checkMatroskaOnlyCodecs). A demux writes a .wv file, its block headers made anew from the frames.
class WavpackStreamReader final : public SimplePacketizerReader
{
   public:
    WavpackStreamReader() = default;
    ~WavpackStreamReader() override = default;

    int getFreq() override { return m_sampleRate; }
    // A demux writes a .wv file: each frame back into WavPack blocks with their 32 byte headers
    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    uint8_t getChannels() override { return m_channels; }
    [[nodiscard]] int getBitDepth() const { return m_bitDepth; }
    // The 2 byte version, as Matroska's CodecPrivate holds it
    [[nodiscard]] const std::vector<uint8_t>& getCodecPrivate() const { return m_codecPrivate; }

   protected:
    int getHeaderLen() override { return 4; }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override { return 0; }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override;
    const std::string getStreamInfo() override;

   private:
    bool setPrivate(const uint8_t* data, int size);
    void toWvBlocks(AVPacket& avPacket);
    std::vector<uint8_t> m_codecPrivate;
    int m_sampleRate = 0;
    uint8_t m_channels = 0;
    int m_bitDepth = 0;
    uint32_t m_flags = 0;  // of the first frame: hybrid, float
    bool m_haveFlags = false;
    int m_curFrameSamples = 0;
    int64_t m_demuxIndex = 0;         // the block_index of the next frame a demux writes
    std::vector<uint8_t> m_wvBlocks;  // that frame as WavPack blocks
};

#endif  // WAVPACK_STREAM_READER_H_
