// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef ALAC_STREAM_READER_H_
#define ALAC_STREAM_READER_H_

#include <vector>

#include "simplePacketizerReader.h"

// Apple Lossless (ALAC) audio, read from Matroska. A frame has no sync code: the Matroska parser
// (ParsedAlacTrackData) puts a 4 byte big endian length in front of each, and the track's CodecPrivate (the 24 byte
// ALACSpecificConfig) in front of the first, behind the mark "ALACCPRV" and its own length.
//
// A frame holds the configuration's frame length in samples, except a partial frame, the last of a stream as a rule,
// which says its own count in its header.
//
// ALAC has no form in a transport stream or on a Blu-ray disc, so a Matroska output is the only one that can carry it
// (MuxerManager::checkTrackList).
class AlacStreamReader final : public SimplePacketizerReader
{
   public:
    AlacStreamReader();
    ~AlacStreamReader() override = default;

    int getFreq() override { return m_sampleRate; }
    uint8_t getChannels() override { return m_channels; }
    [[nodiscard]] int getBitDepth() const { return m_bitDepth; }

    // The 24 byte ALACSpecificConfig, as Matroska's CodecPrivate holds it.
    [[nodiscard]] const std::vector<uint8_t>& getCodecPrivate() const { return m_codecPrivate; }
    bool setCodecPrivate(const uint8_t* data, int size);

   protected:
    int getHeaderLen() override { return 4; }
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override { return 0; }
    int decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes) override;
    uint8_t* findFrame(uint8_t* buff, uint8_t* end) override;
    double getFrameDuration() override;
    const CodecInfo& getCodecInfo() override;
    const std::string getStreamInfo() override;

   private:
    std::vector<uint8_t> m_codecPrivate;
    uint32_t m_frameLength;
    int m_bitDepth;
    uint8_t m_channels;
    int m_sampleRate;
    int m_curFrameSamples;
};

#endif  // ALAC_STREAM_READER_H_
