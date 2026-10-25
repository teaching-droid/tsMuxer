#include "blurayHelper.h"

#include <algorithm>
#include <memory>
#include <set>
#include <type_traits>

#include <fs/directory.h>
#include <fs/systemlog.h>
#include <array>

#include "iso_writer.h"
#include "muxerManager.h"
#include "pgsStreamReader.h"
#include "tsMuxer.h"
#include "vodCoreException.h"

using namespace std;

uint8_t bdIndexData[] = {
    0x49, 0x4e, 0x44, 0x58, 0x30, 0x32, 0x30, 0x30, 0x00, 0x00, 0x00, 0x4e, 0x00, 0x00, 0x00, 0x00,  // 0x78,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    0x00, 0x00, 0x01, 0x92, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x01, 0x7e, 0x49, 0x44, 0x45, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x62, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x36, 0x10, 0x13, 0x00, 0x01, 0x54, 0x52, 0x20, 0x30,
    0x2e, 0x30, 0x2e, 0x31, 0x2e, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xff, 0xff, 0x42, 0x20, 0x07, 0x08, 0x08, 0x00,
    0x54, 0x53, 0x00, 0x90, 0x0a, 0x54, 0x52, 0x20, 0x30, 0x2e, 0x30, 0x2e, 0x31, 0x2e, 0x38, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x18, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

namespace
{
template <typename Container>
void push_back_raw(std::vector<std::uint8_t>& byteVector, const Container& c, std::false_type)
{
    std::copy(std::begin(c), std::end(c), std::back_inserter(byteVector));
}
template <typename PODType>
void push_back_raw(std::vector<std::uint8_t>& byteVector, const PODType& value, std::true_type)
{
    const auto start = reinterpret_cast<const std::uint8_t*>(&value);
    const auto finish = start + sizeof(value);
    std::copy(start, finish, std::back_inserter(byteVector));
}
template <typename T>
void push_back_raw(std::vector<std::uint8_t>& byteVector, const T& value)
{
    // What std::is_pod said, which C++20 deprecates: trivial, and standard layout
    push_back_raw(byteVector, value, std::bool_constant < std::is_trivial_v<T> && std::is_standard_layout_v < T >> {});
}
using NavigationCommand = std::array<std::uint8_t, 12>;
struct MovieObject
{
    std::vector<NavigationCommand> navigationCommands;
    bool resumeIntentionFlag = true;
    bool menuCallMask = false;
    bool titleSearchMask = false;

    [[nodiscard]] std::vector<std::uint8_t> serialize() const
    {
        std::uint16_t flags = 0;
        flags |= (resumeIntentionFlag << 15);
        flags |= (menuCallMask << 14);
        flags |= (titleSearchMask << 13);

        const auto numOfNavCommands = static_cast<std::uint16_t>(navigationCommands.size());

        std::vector<std::uint8_t> rv;
        rv.reserve(serializedSize());

        push_back_raw(rv, my_htons(flags));
        push_back_raw(rv, my_htons(numOfNavCommands));
        for (auto&& navCmd : navigationCommands)
        {
            std::copy(std::begin(navCmd), std::end(navCmd), std::back_inserter(rv));
        }
        return rv;
    }

    [[nodiscard]] size_t serializedSize() const
    {
        const auto numOfNavCommands = static_cast<std::uint16_t>(navigationCommands.size());
        return (2 * sizeof(std::uint16_t)) /* flags + numOfNavCommands */
               + (numOfNavCommands * std::tuple_size_v<decltype(navigationCommands)::value_type>);
    }
};
enum class BDMV_VersionNumber
{
    Version1,
    Version2,
    Version3
};
std::array<std::uint8_t, 4> getBDMV_VersionNumber(const BDMV_VersionNumber version)
{
    std::array<std::uint8_t, 4> rv = {0x30, 0x31, 0x30, 0x30};  // "0100"
    switch (version)
    {
    case BDMV_VersionNumber::Version2:
        rv[1] = 0x32;
        return rv;
    case BDMV_VersionNumber::Version3:
        rv[1] = 0x33;
        return rv;
    default:
        return rv;
    }
}
std::vector<std::uint8_t> makeBdMovieObjectData(const BDMV_VersionNumber version,
                                                const std::vector<MovieObject>& movieObjects)
{
    constexpr std::array<std::uint8_t, 4> type_indicator = {0x4D, 0x4F, 0x42, 0x4A};  // "MOBJ"
    const auto version_number = getBDMV_VersionNumber(version);
    constexpr std::uint32_t extension_data_start_addr = 0;
    constexpr std::uint32_t reserved_after_length = 0;
    constexpr auto header_reserved_bytes = 28u;

    std::vector<std::uint8_t> rv;
    const auto num_movie_objects = static_cast<std::uint16_t>(movieObjects.size());
    auto payload_length = static_cast<std::uint32_t>(sizeof(reserved_after_length) + sizeof(num_movie_objects));
    constexpr auto header_size = type_indicator.size() + version_number.size() + sizeof(extension_data_start_addr) +
                                 header_reserved_bytes + sizeof(payload_length);

    for (auto&& movieObj : movieObjects)
    {
        payload_length += static_cast<uint32_t>(movieObj.serializedSize());
    }

    rv.reserve(header_size + payload_length);
    push_back_raw(rv, type_indicator);
    push_back_raw(rv, version_number);
    push_back_raw(rv, my_htonl(extension_data_start_addr));
    rv.resize(rv.size() + header_reserved_bytes, 0);
    push_back_raw(rv, my_htonl(payload_length));
    push_back_raw(rv, my_htonl(reserved_after_length));
    push_back_raw(rv, my_htons(num_movie_objects));
    for (auto&& movieObj : movieObjects)
    {
        push_back_raw(rv, movieObj.serialize());
    }

    return rv;
}

NavigationCommand makeBlankPLCommand(std::uint32_t blankPlNum)
{
    NavigationCommand cmd = {0x42, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
    blankPlNum = my_htonl(blankPlNum);
    memcpy(cmd.data() + 4, &blankPlNum, sizeof(blankPlNum));
    return cmd;
}

NavigationCommand makeMplsCommand(std::uint32_t mplsNum)
{
    NavigationCommand cmd = {0x42, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A};
    mplsNum = my_htonl(mplsNum);
    memcpy(cmd.data() + 4, &mplsNum, sizeof(mplsNum));
    return cmd;
}

NavigationCommand makeNoBlankCommand()
{
    return {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

NavigationCommand makeDefaultTrackCommand(int audioTrackIdx, int subTrackIdx,
                                          const MuxerManager::SubTrackMode subTrackMode)
{
    NavigationCommand cmd = {0x51, 0xC0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (audioTrackIdx >= 0)
    {
        ++audioTrackIdx;  // stream indices in this command are 1-based.
        audioTrackIdx = my_ntohs(static_cast<std::uint16_t>(audioTrackIdx));
        memcpy(cmd.data() + 4, &audioTrackIdx, sizeof(audioTrackIdx));
        cmd[4] &= 0x0f;
        cmd[4] |= 0x80;
    }
    if (subTrackIdx >= 0)
    {
        ++subTrackIdx;
        subTrackIdx = my_ntohs(static_cast<std::uint16_t>(subTrackIdx));
        memcpy(cmd.data() + 6, &subTrackIdx, sizeof(subTrackIdx));
        cmd[6] &= 0x0f;
        cmd[6] |= 0x80;
        // Shown from the start, rather than only its forced captions. Only with a subtitle track to show: without
        // one, this flag set on its own asked a player to switch subtitles on at the start of the disc.
        if (subTrackMode == MuxerManager::SubTrackMode::All)
            cmd[6] |= 0x40;
    }
    return cmd;
}

bool writeBdMovieObjectData(const MuxerManager& muxer, AbstractOutputStream* file, const std::string& prefix,
                            const DiskType diskType, const bool usedBlankPL, const int mplsNum, const int blankNum)
{
    std::vector<MovieObject> movieObjects = {
        {{{0x50, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          usedBlankPL ? makeBlankPLCommand(blankNum) : makeNoBlankCommand(),
          makeMplsCommand(mplsNum),
          {0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}},
        {{{0x50, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x03},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0xFF, 0xFF},
          {0x48, 0x40, 0x03, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0xFF, 0xFF},
          {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00},
          {0x50, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x04},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00},
          {0x48, 0x40, 0x03, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00},
          {0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00},
          {0x21, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}}},
        {{{0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0xFF, 0xFF},
          {0x50, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00},
          {0x21, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}}};

    BDMV_VersionNumber num = BDMV_VersionNumber::Version1;
    if (diskType == DiskType::BLURAY)
    {
        num = isV3() ? BDMV_VersionNumber::Version3 : BDMV_VersionNumber::Version2;
    }
    const auto defaultAudioIdx = muxer.getDefaultAudioTrackIdx();
    // Set only when a subtitle track is marked default. It used to be read uninitialised when none was, with only an
    // audio track marked: whatever the stack held decided whether the disc switched subtitles on at its start.
    MuxerManager::SubTrackMode mode = MuxerManager::SubTrackMode::Forced;
    const auto defaultSubIdx = muxer.getDefaultSubTrackIdx(mode);
    if (defaultAudioIdx != -1 || defaultSubIdx != -1)
    {
        auto&& navCmds = movieObjects[0].navigationCommands;
        movieObjects[0].navigationCommands.insert(std::begin(navCmds) + 2,
                                                  makeDefaultTrackCommand(defaultAudioIdx, defaultSubIdx, mode));
    }
    const auto objectData = makeBdMovieObjectData(num, movieObjects);
    if (!file->open((prefix + "BDMV/MovieObject.bdmv").c_str(), File::ofWrite))
    {
        delete file;
        THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + "BDMV/MovieObject.bdmv"))
    }

    file->write(objectData);
    file->close();
    if (!file->open((prefix + "BDMV/BACKUP/MovieObject.bdmv").c_str(), File::ofWrite))
    {
        delete file;
        THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + "BDMV/BACKUP/MovieObject.bdmv"))
    }
    file->write(objectData);
    file->close();
    delete file;
    return true;
}
}  // namespace

// ------------------------- BlurayHelper ---------------------------

BlurayHelper::BlurayHelper() : m_dt(), m_isoWriter(nullptr) {}

// The image is finished by close(), which the mux calls before it reports success. Here it is only a last resort,
// as when the mux fails part way: a refusal while finishing cannot leave a destructor, and it used to end the
// program on the spot without a word.
BlurayHelper::~BlurayHelper()
{
    try
    {
        close();
    }
    catch (const std::exception& e)
    {
        LTRACE(LT_ERROR, 2, e.what());
    }
    catch (const VodCoreException& e)
    {
        LTRACE(LT_ERROR, 2, e.m_errStr);
    }
}

void BlurayHelper::close()
{
    if (m_isoWriter)
    {
        LTRACE(LT_INFO, 2, "Finalize ISO disk");
        // Finalise through close() rather than leaving it to the destructor. Finalising can refuse,
        // and a refusal thrown out of a destructor cannot be reported: it either ends the process
        // where it stands, with nothing on stdout or stderr, or it is swallowed and the run says it
        // succeeded over an image no reader will open. Both were measured. The writer is handed to
        // a unique_ptr first, so it is destroyed either way.
        const std::unique_ptr<IsoWriter> writer(m_isoWriter);
        m_isoWriter = nullptr;
        writer->close();
    }
}

bool BlurayHelper::open(const string& dst, const DiskType dt, const int64_t diskSize, const int extraISOBlocks,
                        const bool useReproducibleIsoHeader, const int layerBreakGuardMB,
                        const std::vector<int>& layerBreakLbns, const int layerBreakGuardBeforeMB,
                        const int64_t discBytes)
{
    m_dstPath = toNativeSeparators(dst);

    m_dt = dt;
    string fileExt = extractFileExt(m_dstPath);
    fileExt = unquoteStr(strToUpperCase(fileExt));
    if (fileExt == "ISO")
    {
        m_isoWriter = new IsoWriter(useReproducibleIsoHeader ? IsoHeaderData::reproducible() : IsoHeaderData::normal());
        // --layer-break-guard-before on its own asks for a guard too: one that is only before the break. It was
        // ignored without a word, as only --layer-break-guard switched the guard on.
        const int afterMB = layerBreakGuardMB < 0 && layerBreakGuardBeforeMB >= 0 ? 0 : layerBreakGuardMB;
        // Layer break(s) in absolute image sectors: the ones given, or those of the disc --disc-size names (its
        // sectors split over its layers, as the window's disc table has them), or the break of a BD-R DL. With
        // --disc-size=bd100 and no breaks given the guard went at half a BD50, which is not a layer change on a
        // 100 GB disc.
        std::vector<int> breaks = layerBreakLbns;
        if (afterMB >= 0 && breaks.empty())
        {
            std::string warning, error;
            breaks = defaultLayerBreaks(discBytes, warning, error);
            if (!error.empty())
                THROW(ERR_COMMON, error)
            if (!warning.empty())
                LTRACE(LT_WARN, 2, warning);
            std::string list;
            for (const int b : breaks) list += (list.empty() ? "" : ",") + std::to_string(b);
            // The option given is named: with --layer-break-guard-before alone, the warning of a single layer disc
            // named the other one.
            if (breaks.empty())
                LTRACE(LT_WARN, 2,
                       "Warning: a single layer disc has no layer break, so "
                           << (layerBreakGuardMB < 0 ? "--layer-break-guard-before" : "--layer-break-guard")
                           << " is not applied.");
            else
                LTRACE(LT_INFO, 2,
                       "Layer break guard at sector(s) " << list
                                                         << (discBytes > 0 ? ", the break(s) of the disc size"
                                                                           : ", the break of a BD-R DL (50 GB); "
                                                                             "give --disc-size or "
                                                                             "--layer-break-lbn for another "
                                                                             "disc")
                                                         << ".");
        }
        if (afterMB >= 0 && !breaks.empty())
        {
            m_isoWriter->setLayerBreakPoints(breaks);
            const int afterSectors = static_cast<int>(static_cast<int64_t>(afterMB) * 1024 * 1024 / SECTOR_SIZE);
            if (layerBreakGuardBeforeMB >= 0)
                m_isoWriter->setLayerBreakGuard(
                    static_cast<int>(static_cast<int64_t>(layerBreakGuardBeforeMB) * 1024 * 1024 / SECTOR_SIZE),
                    afterSectors);
            else
                m_isoWriter->setLayerBreakGuard(afterSectors);
        }
        return m_isoWriter->open(m_dstPath, diskSize, extraISOBlocks);
    }
    // A FOLDER destination. The guard works by writing zeros at particular sector positions and only
    // the ISO writer places sectors, so there is nothing here for it to act on. That is the shape of
    // the feature rather than a defect. The defect was staying silent about it: the option parses, the
    // mux succeeds, and the disc is burned without the guard the user asked for. The interface already
    // greys these out for folder output, so this catches a meta file or a command line.
    if (layerBreakGuardMB >= 0 || layerBreakGuardBeforeMB >= 0)
        LTRACE(LT_WARN, 2,
               "Warning: --layer-break-guard applies to an .iso destination and is being ignored, because "
               "this mux writes a folder and only a disc image has sector positions to reserve. Author the "
               "folder as now, then run it through BDMV folder to ISO, where the guard is applied.");
    m_dstPath = closeDirPath(m_dstPath, getDirSeparator());
    return true;
}

void BlurayHelper::createBluRayDirs() const
{
    if (m_dt == DiskType::BLURAY)
    {
        if (m_isoWriter)
        {
            m_isoWriter->createDir("BDMV/META");
            m_isoWriter->createDir("BDMV/BDJO");
            m_isoWriter->createDir("BDMV/JAR");
            m_isoWriter->createDir("BDMV/BACKUP/BDJO");
            m_isoWriter->createDir("BDMV/BACKUP/JAR");
        }
        else
        {
            createDir(m_dstPath + toNativeSeparators("BDMV/META"), true);
            createDir(m_dstPath + toNativeSeparators("BDMV/BDJO"), true);
            createDir(m_dstPath + toNativeSeparators("BDMV/JAR"), true);
            createDir(m_dstPath + toNativeSeparators("BDMV/BACKUP/BDJO"), true);
            createDir(m_dstPath + toNativeSeparators("BDMV/BACKUP/JAR"), true);
        }
    }

    if (m_isoWriter)
    {
        m_isoWriter->createDir("CERTIFICATE/BACKUP");
        m_isoWriter->createDir("BDMV/AUXDATA");

        m_isoWriter->createDir("BDMV/CLIPINF");
        m_isoWriter->createDir("BDMV/PLAYLIST");
        m_isoWriter->createDir("BDMV/STREAM");
        m_isoWriter->createDir("BDMV/BACKUP/CLIPINF");
        m_isoWriter->createDir("BDMV/BACKUP/PLAYLIST");
    }
    else
    {
        createDir(m_dstPath + toNativeSeparators("CERTIFICATE/BACKUP"), true);
        createDir(m_dstPath + toNativeSeparators("BDMV/AUXDATA"), true);

        createDir(m_dstPath + toNativeSeparators("BDMV/CLIPINF"), true);
        createDir(m_dstPath + toNativeSeparators("BDMV/PLAYLIST"), true);
        createDir(m_dstPath + toNativeSeparators("BDMV/STREAM"), true);
        createDir(m_dstPath + toNativeSeparators("BDMV/BACKUP/CLIPINF"), true);
        createDir(m_dstPath + toNativeSeparators("BDMV/BACKUP/PLAYLIST"), true);
    }
}

bool BlurayHelper::writeBluRayFiles(const MuxerManager& muxer, const bool usedBlankPL, const int mplsNum,
                                    const int blankNum, const bool stereoMode) const
{
    int fileSize = sizeof(bdIndexData);
    const string prefix = m_isoWriter ? "" : m_dstPath;
    AbstractOutputStream* file;
    if (m_isoWriter)
        file = m_isoWriter->createFile();
    else
        file = new File();

    if (m_dt == DiskType::BLURAY)
    {
        if (isV3())
        {
            bdIndexData[5] = '3';
            fileSize = 0x9C;         // add 36 bytes for UHD data extension
            bdIndexData[15] = 0x78;  // start address of UHD data extension

            // UHD data extension
            uint8_t* V3metaData = bdIndexData + 0x78;
            static constexpr char metaData[37] =
                "\x00\x00\x00\x20\x00\x00\x00\x18\x00\x00\x00\x01"
                "\x00\x03\x00\x01\x00\x00\x00\x18\x00\x00\x00\x0C"
                "\x00\x00\x00\x08\x20\x00\x00\x00\x00\x00\x00\x00";
            for (int i = 0; i < 36; i++) V3metaData[i] = metaData[i];

            // 4K => 66/100 GB Disk, 109 MB/s Recording_Rate
            if (is4K())
                bdIndexData[0x94] = 0x51;
            // include HDR flags
            bdIndexData[0x96] = (V3_flags & 0x1e);
            // no HDR10 detected => SDR flag
            if (bdIndexData[0x96] == 0)
                bdIndexData[0x96] = 1;
        }
        else  // V2 Blu-ray
        {
            bdIndexData[5] = '2';
            fileSize = 0x78;
        }
    }
    else
    {
        bdIndexData[5] = '1';
        bdIndexData[15] = 0x78;
    }
    bdIndexData[0x2c] = stereoMode ? 0x60 : 0;  // set initial_output_mode_preference and SS_content_exist_flag

    if (!file->open((prefix + "BDMV/index.bdmv").c_str(), AbstractOutputStream::ofWrite))
    {
        delete file;
        THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + "BDMV/index.bdmv"))
    }
    file->write(bdIndexData, fileSize);
    file->close();

    if (!file->open((prefix + "BDMV/BACKUP/index.bdmv").c_str(), File::ofWrite))
    {
        delete file;
        THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + "BDMV/BACKUP/index.bdmv"))
    }
    file->write(bdIndexData, fileSize);
    file->close();

    return writeBdMovieObjectData(muxer, file, prefix, m_dt, usedBlankPL, mplsNum, blankNum);
}

bool BlurayHelper::createCLPIFile(TSMuxer* muxer, int clpiNum, bool doLog) const
{
    static constexpr int CLPI_BUFFER_SIZE = 1024 * 1024;
    auto clpiBuffer = new uint8_t[CLPI_BUFFER_SIZE];
    CLPIParser clpiParser;
    string version_number;
    clpiParser.version_number[0] = '0';
    clpiParser.version_number[1] = m_dt == DiskType::BLURAY ? (isV3() ? '3' : '2') : '1';
    clpiParser.version_number[2] = '0';
    clpiParser.version_number[3] = '0';
    clpiParser.version_number[4] = 0;

    clpiParser.clip_stream_type = 1;  // AV stream
    clpiParser.isDependStream = muxer->isSubStream();
    if (clpiParser.isDependStream)
        clpiParser.application_type = 8;  // Sub TS for a sub-path of MPEG-4 MVC Dependent view
    else
        clpiParser.application_type = 1;  // 1 - Main TS for a main-path of Movie
    clpiParser.is_ATC_delta = false;
    clpiParser.pcr_pid = static_cast<uint16_t>(muxer->getPcrPid());
    if (doLog)
    {
        if (m_dt == DiskType::BLURAY)
        {
            LTRACE(LT_INFO, 2, "Creating Blu-ray stream info and seek index");
        }
        else
        {
            LTRACE(LT_INFO, 2, "Creating AVCHD stream info and seek index");
        }
    }

    PIDListMap pidList = muxer->getPidList();
    for (const auto& [pid, si] : pidList)
    {
        CLPIStreamInfo streamInfo(si);
        clpiParser.m_streamInfo.insert(make_pair(streamInfo.streamPID, streamInfo));
    }
    vector<uint32_t> packetCount = muxer->getMuxedPacketCnt();
    vector<int64_t> firstPts = muxer->getFirstPts();
    vector<int64_t> lastPts = muxer->getLastPts();

    for (size_t i = 0; i < muxer->splitFileCnt(); i++)
    {
        if (muxer->isInterleaveMode())
            clpiParser.interleaveInfo = muxer->getInterleaveInfo(i);

        if (muxer->isSubStream())
            clpiParser.TS_recording_rate = MAX_SUBMUXER_RATE / 8;
        else
        {
            if (is4K())
                clpiParser.TS_recording_rate = MAX_4K_MUXER_RATE / 8;
            else
                clpiParser.TS_recording_rate = MAX_MAIN_MUXER_RATE / 8;
        }

        clpiParser.number_of_source_packets = packetCount[i];
        clpiParser.presentation_start_time = static_cast<uint32_t>(firstPts[i] / 2);
        clpiParser.presentation_end_time = static_cast<uint32_t>(lastPts[i] / 2);
        clpiParser.m_clpiNum = i;

        int fileLen = clpiParser.compose(clpiBuffer, CLPI_BUFFER_SIZE);

        string prefix = m_isoWriter ? "" : m_dstPath;
        AbstractOutputStream* file;
        if (m_isoWriter)
            file = m_isoWriter->createFile();
        else
            file = new File();

        string dstDir = string("BDMV") + getDirSeparator() + string("CLIPINF") + getDirSeparator();
        string clipName = extractFileName(muxer->getFileNameByIdx(i));
        if (!file->open((prefix + dstDir + clipName + ".clpi").c_str(), File::ofWrite))
        {
            delete[] clpiBuffer;
            delete file;
            THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + dstDir + clipName + ".clpi"))
        }
        if (file->write(clpiBuffer, fileLen) != fileLen)
        {
            file->close();
            delete[] clpiBuffer;
            delete file;
            THROW(ERR_CANT_CREATE_FILE, "Can't write file " << (prefix + dstDir + clipName + ".clpi"))
        }
        file->close();

        dstDir = string("BDMV") + getDirSeparator() + string("BACKUP") + getDirSeparator() + string("CLIPINF") +
                 getDirSeparator();
        if (!file->open((prefix + dstDir + clipName + ".clpi").c_str(), File::ofWrite))
        {
            delete[] clpiBuffer;
            delete file;
            THROW(ERR_CANT_CREATE_FILE, "Can't create file " << (prefix + dstDir + clipName + ".clpi"))
        }
        if (file->write(clpiBuffer, fileLen) != fileLen)
        {
            file->close();
            delete[] clpiBuffer;
            delete file;
            THROW(ERR_CANT_CREATE_FILE, "Can't write file " << (prefix + dstDir + clipName + ".clpi"))
        }
        file->close();
        delete file;
    }
    delete[] clpiBuffer;
    return true;
}

const PMTStreamInfo* streamByIndex(const int index, const PIDListMap& pidList)
{
    for (const auto& [pid, si] : pidList)
    {
        const PMTStreamInfo& stream = si;
        if (stream.m_codecReader->getStreamIndex() == index)
            return &stream;
    }
    return nullptr;
}

bool BlurayHelper::createMPLSFile(TSMuxer* mainMuxer, TSMuxer* subMuxer, int autoChapterLen,
                                  const vector<double>& customChapters, DiskType dt, int mplsOffset,
                                  bool isMvcBaseViewR) const
{
    int64_t firstPts = *(mainMuxer->getFirstPts().begin());
    int64_t lastPts = *(mainMuxer->getLastPts().rbegin());
    // --cut-pictures=cover: the clip of a cut begins at the key picture before --cut-start and ends with the pictures
    // decoded before --cut-end, and the playlist plays it from the first picture shown at or after the one to the first
    // shown at or after the other, as a Blu-ray playlist plays a part of a clip. The chapters of the cut are times from
    // --cut-start, which is after the start of the clip.
    if (mainMuxer->getCutPlayFromPts() > firstPts && mainMuxer->getCutPlayFromPts() < lastPts)
        firstPts = mainMuxer->getCutPlayFromPts();
    if (mainMuxer->getCutPlayToPts() > firstPts && mainMuxer->getCutPlayToPts() < lastPts)
        lastPts = mainMuxer->getCutPlayToPts();
    const int64_t chapterOrigin = mainMuxer->getChapterOriginPts() >= 0 ? mainMuxer->getChapterOriginPts() : firstPts;

    int bufSize = 1024 * 100;
    auto mplsBuffer = new uint8_t[bufSize];
    MPLSParser mplsParser;
    mplsParser.m_m2tsOffset = mainMuxer->getFirstFileNum();
    mplsParser.PlayList_playback_type = 1;
    mplsParser.ref_to_STC_id = 0;
    mplsParser.IN_time = static_cast<uint32_t>(firstPts / 2);
    mplsParser.OUT_time = static_cast<uint32_t>(lastPts / 2);
    mplsParser.mvc_base_view_r = isMvcBaseViewR;

    if (customChapters.empty())
    {
        mplsParser.m_chapterLen = autoChapterLen;
    }
    else
    {
        for (auto& i : customChapters)
        {
            // Checked before it becomes a 32-bit count of 1/45,000 s. A time of 26.5 hours or more is past
            // the end of any playlist, and it used to wrap round to a mark near the start (26:30:48.718
            // became 0:05). A time before the start, or one that is not a number, gets no mark either.
            const double ticks = i * 45000.0;
            if (!(ticks >= 0.0 && ticks < 4294967296.0))
                continue;
            // On the clip's clock; one before the start of the playlist (the chapter playing at a --cut-start, when the
            // playlist starts after it) is at the start
            const int64_t mark = (std::max)(static_cast<int64_t>(ticks) + chapterOrigin / 2,
                                            static_cast<int64_t>(mplsParser.IN_time));
            // The list is in time order with no time twice, but two times a few microseconds apart become one tick of
            // the playlist's clock: they gave two marks at one time, which a player shows as an empty chapter
            if (mark <= mplsParser.OUT_time &&
                (mplsParser.m_marks.empty() || mplsParser.m_marks.back().m_markTime != mark))
                mplsParser.m_marks.emplace_back(-1, static_cast<uint32_t>(mark));
        }
    }
    // Counted before the mux from the length the sources state (refuseTooManyMarks in main); a length stated wrong
    // still stops here with the reason, not in the playlist's buffer with "Bitstream exception"
    if (mplsParser.m_marks.size() > 999)
    {
        delete[] mplsBuffer;
        THROW(ERR_COMMON, "A Blu-ray playlist holds at most 999 chapter marks, and the chapters within the title are "
                              << mplsParser.m_marks.size() << ". Give fewer chapters.")
    }
    mplsParser.PlayItem_random_access_flag = false;
    PIDListMap pidList = mainMuxer->getPidList();
    PIDListMap pidListMVC;
    if (subMuxer)
        pidListMVC = subMuxer->getPidList();

    // The eyes of a stereo subtitle and the aligned subtitles are muxed as streams of their own, and are named only by
    // the 3D table entry of the subtitle they belong to: they are no entries of the PG list
    std::set<int> hangingStreams;
    bool hasSecondaryVideo = false;
    for (auto& [index, si] : pidList)
    {
        if (const auto pg = dynamic_cast<PGSStreamReader*>(si.m_codecReader))
        {
            if (pg->isSSPG)
                hangingStreams.insert({pg->leftEyeSubStreamIdx, pg->rightEyeSubStreamIdx});
            hangingStreams.insert({pg->alignedSubStreamIdx[0], pg->alignedSubStreamIdx[1]});
        }
        if (si.isSecondary && si.m_codecReader && si.m_codecReader->getCodecInfo().programName[0] == 'V')
            hasSecondaryVideo = true;
    }
    hangingStreams.erase(-1);
    for (auto& [index, si] : pidList)
    {
        mplsParser.m_streamInfo.emplace_back(si);
        MPLSStreamInfo& info = *mplsParser.m_streamInfo.rbegin();
        auto pgStream = dynamic_cast<PGSStreamReader*>(si.m_codecReader);
        if (pgStream)
        {
            info.hangsOffEntry = hangingStreams.count(pgStream->getStreamIndex()) > 0;
            info.isPiP = pgStream->isPiP && !info.hangsOffEntry;
            if (info.isPiP && !hasSecondaryVideo)
                LTRACE(LT_WARN, 2,
                       "Warning: the picture-in-picture subtitle written with PID 0x"
                           << std::hex << si.m_pid << std::dec
                           << " is left out of the playlist. The picture-in-picture video it belongs to is not "
                              "muxed, so no player could show it.");
            for (int k = 0; k < 2; ++k)
            {
                bool isSecondary = false;
                const PMTStreamInfo* as = streamByIndex(pgStream->alignedSubStreamIdx[k], pidList);
                if (!as)
                {
                    as = streamByIndex(pgStream->alignedSubStreamIdx[k], pidListMVC);
                    isSecondary = as != nullptr;
                }
                if (as)
                {
                    MPLSStreamInfo::AlignedRef& ref = k == 0 ? info.topAS : info.bottomAS;
                    ref.pid = as->m_pid;
                    ref.type = isSecondary ? 2 : 1;
                    ref.offsetId = pgStream->alignedOffset[k];
                }
            }
            info.offsetId = pgStream->getOffsetId();
            info.SS_PG_offset_sequence_id = pgStream->ssPGOffset;
            info.isSSPG = pgStream->isSSPG;
            if (info.isSSPG)
            {
                bool isSecondary = false;
                const PMTStreamInfo* left = streamByIndex(pgStream->leftEyeSubStreamIdx, pidList);
                if (!left)
                {
                    left = streamByIndex(pgStream->leftEyeSubStreamIdx, pidListMVC);
                    isSecondary = true;
                }
                if (left)
                {
                    info.leftEye = new MPLSStreamInfo(*left);
                    if (isSecondary)
                        info.leftEye->type = 2;  // ref to subclip
                }

                isSecondary = false;
                const PMTStreamInfo* right = streamByIndex(pgStream->rightEyeSubStreamIdx, pidList);
                if (!right)
                {
                    right = streamByIndex(pgStream->rightEyeSubStreamIdx, pidListMVC);
                    isSecondary = true;
                }
                if (right)
                {
                    info.rightEye = new MPLSStreamInfo(*right);
                    if (isSecondary)
                        info.rightEye->type = 2;  // ref to subclip
                }
            }
        }
    }
    // A second video without "secondary" on its line is written as a second primary video, which a player does not
    // show over the first, while a picture-in-picture video of a disc playlist is listed as "Secondary: 1". That was
    // said nowhere. The first (PID 0x1011) is the main video; the PIDs of the others are named.
    {
        std::string extra;
        bool first = true;
        for (const auto& [index, si] : pidList)
        {
            if (!si.m_codecReader || si.m_codecReader->getCodecInfo().programName[0] != 'V' ||
                si.m_streamType == StreamType::VIDEO_MVC || si.isSecondary || si.isDvEnhancement)
                continue;
            if (!first)
            {
                std::ostringstream pid;
                pid << "0x" << std::hex << si.m_pid;
                extra += (extra.empty() ? "" : ", ") + pid.str();
            }
            first = false;
        }
        if (!extra.empty())
            LTRACE(LT_WARN, 2,
                   "Warning: the video with PID " << extra
                                                  << " is muxed as a second primary video, which a player does not "
                                                     "show as a picture-in-picture. Give \"secondary\" on its line "
                                                     "to mux it as a picture-in-picture video.");
    }
    if (subMuxer)
    {
        mplsParser.isDependStreamExist = true;
        for (const auto& [pid, si] : pidListMVC)
        {
            MPLSStreamInfo data(si);
            data.type = 2;  // Identify an elementary stream of the Clip used by a SubPath with SubPath_type set to
                            // 2,3,4,5,6,8 or 9
            mplsParser.m_streamInfoMVC.push_back(data);
        }
    }

    if (dt == DiskType::BLURAY)
    {
        LTRACE(LT_INFO, 2, "Creating Blu-ray playlist");
    }
    else
    {
        LTRACE(LT_INFO, 2, "Creating AVCHD playlist");
    }
    int fileLen = mplsParser.compose(mplsBuffer, bufSize, dt);

    string prefix = m_isoWriter ? "" : m_dstPath;
    AbstractOutputStream* file;
    if (m_isoWriter)
        file = m_isoWriter->createFile();
    else
        file = new File();

    string dstDir = string("BDMV") + getDirSeparator() + string("PLAYLIST") + getDirSeparator();
    if (!file->open((prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")).c_str(),
                    File::ofWrite))
    {
        delete[] mplsBuffer;
        delete file;
        THROW(ERR_CANT_CREATE_FILE,
              "Can't create file " << (prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")))
    }
    if (file->write(mplsBuffer, fileLen) != fileLen)
    {
        file->close();
        delete[] mplsBuffer;
        delete file;
        THROW(ERR_CANT_CREATE_FILE,
              "Can't write file " << (prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")))
    }
    file->close();

    dstDir = string("BDMV") + getDirSeparator() + string("BACKUP") + getDirSeparator() + string("PLAYLIST") +
             getDirSeparator();
    if (!file->open((prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")).c_str(),
                    File::ofWrite))
    {
        delete[] mplsBuffer;
        delete file;
        THROW(ERR_CANT_CREATE_FILE,
              "Can't create file " << (prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")))
    }
    if (file->write(mplsBuffer, fileLen) != fileLen)
    {
        file->close();
        delete[] mplsBuffer;
        delete file;
        THROW(ERR_CANT_CREATE_FILE,
              "Can't write file " << (prefix + dstDir + strPadLeft(int32ToStr(mplsOffset), 5, '0') + string(".mpls")))
    }
    file->close();
    delete[] mplsBuffer;
    delete file;
    return true;
}

string BlurayHelper::m2tsFileName(const int num) const
{
    const string prefix = m_isoWriter ? "" : m_dstPath;
    const char separator = m_isoWriter ? '/' : getDirSeparator();
    return prefix + string("BDMV") + separator + string("STREAM") + separator + strPadLeft(int32ToStr(num), 5, '0') +
           string(".m2ts");
}

string BlurayHelper::ssifFileName(const int num) const
{
    const string prefix = m_isoWriter ? "" : m_dstPath;
    const char separator = m_isoWriter ? '/' : getDirSeparator();
    return prefix + string("BDMV") + separator + string("STREAM") + separator + string("SSIF") + separator +
           strPadLeft(int32ToStr(num), 5, '0') + string(".ssif");
}

IsoWriter* BlurayHelper::isoWriter() const { return m_isoWriter; }

AbstractOutputStream* BlurayHelper::createFile()
{
    if (m_isoWriter)
        return m_isoWriter->createFile();
    return new File();
}

bool BlurayHelper::isVirtualFS() const { return m_isoWriter != nullptr; }

void BlurayHelper::setVolumeLabel(const std::string& label) const
{
    if (m_isoWriter)
        m_isoWriter->setVolumeLabel(unquoteStr(label));
}
