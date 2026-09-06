#ifndef SINGLE_FILE_MUXER_H_
#define SINGLE_FILE_MUXER_H_

#include <types/types.h>

#include <map>
#include <string>
#include <vector>

#include "abstractMuxer.h"
#include "avPacket.h"

class SingleFileMuxer final : public AbstractMuxer
{
   public:
    SingleFileMuxer(MuxerManager* owner);
    ~SingleFileMuxer() override;
    bool muxPacket(AVPacket& avPacket) override;
    void intAddStream(const std::string& streamName, const std::string& codecName, int streamIndex,
                      const std::map<std::string, std::string>& params, AbstractStreamReader* codecReader) override;
    bool doFlush() override;
    bool close() override;
    void openDstFile() override;

   protected:
    void parseMuxOpt(const std::string& opts) override;

   private:
    static constexpr int ADD_DATA_SIZE = 2048;
    struct StreamInfo
    {
        File m_file;
        std::string m_fileName;
        int64_t m_dts;
        int64_t m_pts;
        uint8_t* m_buffer;
        int m_part;
        int m_bufLen;
        uint64_t m_totalWrited;
        AbstractStreamReader* m_codecReader;
        // Which packets of its track this file takes. A disc TrueHD track is an AC-3 core
        // interleaved with the lossless part, so one track can be written out as up to three
        // files: the pair as they stand, the lossless part alone, and the core alone.
        enum class Part
        {
            Everything,    // the disc form, .ac3+thd
            LosslessOnly,  // drop-ac3-core, .thd
            CoreOnly       // the core on its own, .ac3
        };
        Part m_part_of = Part::Everything;
        [[nodiscard]] bool takes(const bool isCorePacket) const
        {
            return m_part_of == Part::Everything || (m_part_of == Part::CoreOnly) == isCorePacket;
        }
        StreamInfo(const int blockSize)
        {
            m_buffer = new uint8_t[blockSize + MAX_AV_PACKET_SIZE +
                                   ADD_DATA_SIZE];  // reserv extra ADD_DATA_SIZE bytes for stream additional data
            m_bufLen = 0;
            m_dts = -1;
            m_pts = -1;
            m_codecReader = nullptr;
            m_totalWrited = 0;
            m_part = 1;
        }
        ~StreamInfo() { delete[] m_buffer; }
    };
    int m_lastIndex;
    std::map<std::string, int> m_trackNameTmp;
    // std::map<int, std::string> m_fileNames;
    // std::map<int, File> m_file;
    // One track can write more than one file, so each index owns a list. Nearly every track has
    // exactly one entry in its list and behaves as it always did.
    std::map<int, std::vector<StreamInfo*>> m_streamInfo;
    void writeOutBuffer(StreamInfo* streamInfo);
    bool muxPacketTo(StreamInfo* streamInfo, AVPacket& avPacket);
};

class SingleFileMuxerFactory final : public AbstractMuxerFactory
{
   public:
    [[nodiscard]] std::unique_ptr<AbstractMuxer> newInstance(MuxerManager* owner) const override
    {
        return std::make_unique<SingleFileMuxer>(owner);
    }
};

#endif
