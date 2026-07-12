#ifndef MUXER_MANAGER_H_
#define MUXER_MANAGER_H_

#include <memory>

#include "abstractMuxer.h"
#include "bufferedFileWriter.h"
#include "bufferedReaderManager.h"
#include "metaDemuxer.h"

class FileFactory;

class MuxerManager final
{
   public:
    static constexpr int32_t PHYSICAL_SECTOR_SIZE =
        2048;  // minimum write align requirement. Should be readed from OS in a next version
    static constexpr int BLURAY_SECTOR_SIZE =
        PHYSICAL_SECTOR_SIZE * 3;  // real sector size is 2048, but M2TS frame required addition rounding by 3 blocks

    MuxerManager(const BufferedReaderManager& readManager, AbstractMuxerFactory& factory);
    ~MuxerManager();

    void setAsyncMode(const bool val) { m_asyncMode = val; }

    [[nodiscard]] bool isAsyncMode() const { return m_asyncMode; }

    bool openMetaFile(const std::string& fileName);
    int addStream(const std::string& codecName, const std::string& fileName,
                  const std::map<std::string, std::string>& addParams);

    void doMux(const std::string& outFileName, FileFactory* fileFactory);

    void setCutStart(const int64_t value) { m_cutStart = value; }
    [[nodiscard]] int64_t getCutStart() const { return m_cutStart; }

    void setCutEnd(const int64_t value) { m_cutEnd = value; }
    [[nodiscard]] int64_t getCutEnd() const { return m_cutEnd; }

    void waitForWriting() const;

    void asyncWriteBuffer(const AbstractMuxer* muxer, uint8_t* buff, int len, AbstractOutputStream* dstFile);
    int syncWriteBuffer(AbstractMuxer* muxer, const uint8_t* buff, int len, AbstractOutputStream* dstFile) const;
    void muxBlockFinished(const AbstractMuxer* muxer);

    void parseMuxOpt(const std::string& opts);
    int getTrackCnt() { return static_cast<int>(m_metaDemuxer.getCodecInfo().size()); }
    [[nodiscard]] bool getHevcFound() const { return m_metaDemuxer.m_HevcFound; }
    [[nodiscard]] AbstractMuxer* getMainMuxer() const;
    [[nodiscard]] AbstractMuxer* getSubMuxer() const;
    [[nodiscard]] bool isStereoMode() const;

    void setAllowStereoMux(bool value);

    [[nodiscard]] bool isMvcBaseViewR() const { return m_mvcBaseViewR; }
    [[nodiscard]] int64_t totalSize() const { return m_metaDemuxer.totalSize(); }
    [[nodiscard]] int getExtraISOBlocks() const { return m_extraIsoBlocks; }

    [[nodiscard]] bool useReproducibleIsoHeader() const { return m_reproducibleIsoHeader; }

    // Fit-to-disc guard: target disc capacity in bytes (0 = disabled) + oversize override.
    [[nodiscard]] int64_t getDiscSizeLimit() const { return m_discSizeLimit; }
    [[nodiscard]] bool getAllowOversize() const { return m_allowOversize; }

    // Dual-layer guard band: MB of zero-fill on each side of the BD-R DL layer break (-1 = off),
    // plus an optional break-sector override for testing / non-standard geometries.
    [[nodiscard]] int getLayerBreakGuardMB() const { return m_layerBreakGuardMB; }
    [[nodiscard]] const std::vector<int>& getLayerBreakLbns() const { return m_layerBreakLbns; }

    enum class SubTrackMode
    {
        All,
        Forced
    };

    [[nodiscard]] int getDefaultAudioTrackIdx() const;
    int getDefaultSubTrackIdx(SubTrackMode& mode) const;

   private:
    void preinitMux(const std::string& outFileName, FileFactory* fileFactory);
    std::unique_ptr<AbstractMuxer> createMuxer();
    void asyncWriteBlock(const WriterData& data) const;
    void checkTrackList(const std::vector<StreamInfo>& ci) const;

    std::unique_ptr<AbstractMuxer> m_mainMuxer;
    std::unique_ptr<AbstractMuxer> m_subMuxer;

    bool m_asyncMode;
    // int32_t m_fileBlockSize;
    std::string m_outFileName;
    std::condition_variable reinitCond;
    METADemuxer m_metaDemuxer;
    int64_t m_cutStart;
    int64_t m_cutEnd;
    std::unique_ptr<BufferedFileWriter> m_fileWriter;
    AbstractMuxerFactory& m_factory;
    bool m_allowStereoMux;
    std::set<int> m_subStreamIndex;
    std::string m_muxOpts;
    bool m_interleave;

    std::vector<WriterData> m_delayedData;  // ssif interlieave
    bool m_subBlockFinished;
    bool m_mainBlockFinished;
    bool m_mvcBaseViewR;
    int64_t m_ptsOffset;
    int m_extraIsoBlocks;
    bool m_bluRayMode;
    bool m_demuxMode;
    bool m_reproducibleIsoHeader = false;
    int64_t m_discSizeLimit = 0;   // --disc-size: target disc capacity in bytes (0 = guard off)
    bool m_allowOversize = false;  // --allow-oversize: warn instead of abort on an over-capacity image
    int m_layerBreakGuardMB = -1;  // --layer-break-guard: MB of zero-fill each side of the DL break (-1 = off)
    std::vector<int> m_layerBreakLbns;  // --layer-break-lbn: break sector(s); empty = BD-R/RE DL default

    /// Results of the discovery (probe) phase, indexed by stream index.
    std::vector<StreamDiscoveryData> m_discoveryData;
};

#endif  // _MUXER_MANAGER_H_
