// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef MKV_COPY_STREAM_READER_H_
#define MKV_COPY_STREAM_READER_H_

#include <string>
#include <vector>

#include "abstractStreamReader.h"

// A Matroska track copied block for block into a Matroska output, in the form the source has it: ASS, SSA and WebVTT
// subtitles kept as text, and VobSub and DVB subtitles, which every other output gets as PGS. The demuxer hands each
// block on as a record (ParsedCopyTrackData); this reader turns each record back into one packet:
//
//     4 bytes   the length of the block's BlockAdditions body (0 without)
//     ...       that body
//     ...       the block's data
//
// with the block's own time and duration. The CodecID, CodecPrivate and BlockAdditionMapping come from the source's
// header and are written as they are.
class MatroskaCopyStreamReader final : public AbstractStreamReader
{
   public:
    MatroskaCopyStreamReader(const std::string& codecId, std::vector<uint8_t> codecPrivate,
                             std::vector<std::vector<uint8_t>> addMappings, uint64_t maxBlockAddId);
    ~MatroskaCopyStreamReader() override = default;

    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void setBuffer(uint8_t* data, uint32_t dataLen, bool lastBlock = false) override;
    int64_t getProcessedSize() override { return m_processedSize; }
    [[nodiscard]] int64_t getDeliveredFrames() const override { return m_frames; }
    void frameLeftOut() { --m_frames; }  // a block read that a cut leaves out
    const CodecInfo& getCodecInfo() override { return m_codecInfo; }

    [[nodiscard]] const std::string& sourceCodecId() const { return m_sourceCodecId; }
    [[nodiscard]] const std::vector<uint8_t>& codecPrivate() const { return m_codecPrivate; }
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& addMappings() const { return m_addMappings; }
    [[nodiscard]] uint64_t maxBlockAddId() const { return m_maxBlockAddId; }

    // For a demux of ASS, SSA or WebVTT: each packet is the text of the subtitle file instead, its header first, then
    // one Dialogue line or one cue for each block (textFor)
    void setTextFileOutput(const bool on) { m_textFile = on; }

    // For a demux of DVD subtitles: each packet is one sub-picture unit, which the demux writes as the packs of the .sub
    // file (packVobSub), with the .idx file gathered beside it (lang: its id: line)
    void setVobSubFileOutput(const std::string& lang)
    {
        m_vobSubFile = true;
        m_vobSubLang = lang;
    }
    [[nodiscard]] bool vobSubFileOutput() const { return m_vobSubFile; }
    [[nodiscard]] std::string vobSubIndex() const;
    // A unit that goes into the .sub file: its packs in outData, their size returned, and its line of the .idx file
    int packVobSub(int64_t packetPts, const uint8_t* data, size_t size, const uint8_t** outData);

   private:
    // Called S_MKVCOPY, a subtitle by the first letter as the code everywhere asks; the source's CodecID beside it
    CodecInfo m_codecInfo;
    std::string m_sourceCodecId;
    std::vector<uint8_t> m_codecPrivate;
    std::vector<std::vector<uint8_t>> m_addMappings;
    uint64_t m_maxBlockAddId;
    bool m_vobSubFile = false;
    std::string m_vobSubLang;
    std::string m_vobSubTimes;  // the timestamp lines of the .idx file
    int64_t m_vobSubPos = 0;    // where the next unit starts in the .sub file
    std::vector<uint8_t> m_vobSubPacks;  // the packs of the unit last written
    std::vector<uint8_t> m_input;   // records not yet turned into packets
    size_t m_inputPos = 0;          // where the next record starts in m_input
    std::vector<uint8_t> m_packet;  // the data of the packet last handed out
    bool m_textFile = false;
    bool m_headerWritten = false;
    std::string textFor(int64_t pts, int64_t duration, const uint8_t* add, size_t addSize, const uint8_t* data,
                        size_t dataSize);
    int64_t m_processedSize = 0;
    int64_t m_frames = 0;
};

#endif
