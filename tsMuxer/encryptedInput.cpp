#include "encryptedInput.h"

#include <fs/file.h>
#include <fs/systemlog.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
struct Sampled
{
    int tested = 0;
    int encrypted = 0;
};

// The blocks read: the first one, and the others spread evenly over the file. An encrypted file is encrypted
// throughout, so a few tell; more for a large file, where a part may differ. Few for a small one: a playlist can have
// hundreds of short clips, and each place read is a seek on a hard disk or an optical drive.
std::vector<int64_t> spread(const int64_t blocks, const int64_t fileSize)
{
    const int64_t samples = fileSize < (int64_t{256} << 20) ? 4 : fileSize < (int64_t{4} << 30) ? 16 : 32;
    std::vector<int64_t> at;
    if (blocks <= samples)
        for (int64_t i = 0; i < blocks; ++i) at.push_back(i);
    else
        for (int64_t i = 0; i < samples; ++i) at.push_back(i * blocks / samples);
    return at;
}

bool readBlock(const File& file, const int64_t offset, uint8_t* buffer, const uint32_t size)
{
    return file.seek(offset) == offset && file.read(buffer, size) == static_cast<int>(size);
}

// .m2ts: aligned units of 6144 bytes; encrypted when the first packet starts with its sync byte, a later one does not,
// and the copy permission bits of the first header are set
Sampled sampleM2ts(const File& file, const int64_t size)
{
    constexpr uint32_t UNIT = 6144;
    Sampled s;
    std::vector<uint8_t> unit(UNIT);
    for (const int64_t u : spread(size / UNIT, size))
    {
        if (!readBlock(file, u * UNIT, unit.data(), UNIT) || unit[4] != 0x47)
            continue;  // not a unit that starts as one: nothing to tell
        ++s.tested;
        bool allSync = true;
        for (uint32_t k = 1; k < 32 && allSync; ++k) allSync = unit[4 + 192 * k] == 0x47;
        if (!allSync && (unit[0] & 0xC0) != 0)
            ++s.encrypted;
    }
    return s;
}

// A program stream: sectors of 2048 bytes; encrypted when a video or audio PES packet of the sector is marked
// scrambled. A sector without such a packet (a NAV pack, padding) or with an MPEG-1 pack, which has no such mark, is
// not counted.
Sampled sampleProgramStream(const File& file, const int64_t size)
{
    constexpr uint32_t SECTOR = 2048;
    Sampled s;
    std::vector<uint8_t> sector(SECTOR);
    for (const int64_t at : spread(size / SECTOR, size))
    {
        if (!readBlock(file, at * SECTOR, sector.data(), SECTOR) || sector[0] != 0 || sector[1] != 0 ||
            sector[2] != 1 || sector[3] != 0xBA || (sector[4] & 0xC0) != 0x40)
            continue;
        uint32_t o = 14 + (sector[13] & 7);
        bool testable = false, scrambled = false;
        while (o + 9 <= SECTOR && sector[o] == 0 && sector[o + 1] == 0 && sector[o + 2] == 1)
        {
            const uint8_t id = sector[o + 3];
            const uint32_t length = static_cast<uint32_t>(sector[o + 4]) << 8 | sector[o + 5];
            if (id == 0xBD || (id >= 0xC0 && id <= 0xEF))
            {
                testable = true;
                scrambled |= (sector[o + 6] & 0x30) != 0;
            }
            o += 6 + length;
        }
        if (testable)
        {
            ++s.tested;
            s.encrypted += scrambled ? 1 : 0;
        }
    }
    return s;
}
}  // namespace

void checkEncryptedInput(const std::string& fileName, const bool m2ts)
{
    static std::mutex mutex;
    static std::set<std::string> checked;
    {
        std::lock_guard lock(mutex);
        if (!checked.insert(fileName).second)
            return;
    }
    const std::string name = unquoteStr(fileName);
    File file;
    if (!file.open(name.c_str(), File::ofRead | File::ofOpenExisting))
        return;  // the reader says why it cannot be opened
    const int64_t size = file.size();
    const Sampled s = m2ts ? sampleM2ts(file, size) : sampleProgramStream(file, size);
    file.close();
    if (s.encrypted == 0)
        return;
    const std::string what = m2ts ? " units of 6144 bytes tested" : " sectors tested";
    if (s.encrypted * 2 >= s.tested)
        THROW(ERR_COMMON, "\"" << name << "\" is still encrypted: " << s.encrypted << " of the " << s.tested << what
                               << (m2ts ? " cannot be read past their first 16 bytes"
                                        : (s.encrypted == 1 ? " is marked scrambled" : " are marked scrambled"))
                               << ". tsMuxeR cannot read an encrypted file; it needs to be decrypted before muxing.")
    LTRACE(LT_WARN, 2,
           "Warning: " << s.encrypted << " of the " << s.tested << what << " of \"" << name
                       << "\" are still encrypted. What they hold cannot be read, and the pictures and sound there "
                          "are damaged.");
}
