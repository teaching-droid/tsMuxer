#include "bdSsifImport.h"

#include <fs/directory.h>
#include <fs/file.h>
#include <fs/systemlog.h>

#include "vod_common.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace
{
// Everything below reads files off a pressed disc, so every field is treated as hostile: no read is
// made without checking it is inside the buffer first. A malformed disc must give an empty answer,
// never a crash and never a wrong number.

constexpr int64_t TS_PACKET_BYTES = 192;  // Blu-ray source packet: 4 byte header + 188 byte TS packet

bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    out.clear();
    File f;
    if (!f.open(path.c_str(), File::ofRead))
        return false;
    const uint64_t size = f.size();
    // Clip info and playlists are kilobytes. A megabyte is far above anything real and keeps a
    // damaged or mistyped path from being read into memory wholesale.
    if (size == 0 || size > 1024 * 1024)
    {
        f.close();
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const int got = f.read(out.data(), static_cast<uint32_t>(size));
    f.close();
    if (got != static_cast<int>(size))
    {
        out.clear();
        return false;
    }
    return true;
}

bool be16(const std::vector<uint8_t>& d, const size_t at, uint16_t& v)
{
    if (at + 2 > d.size())
        return false;
    v = static_cast<uint16_t>(d[at] << 8 | d[at + 1]);
    return true;
}

bool be32(const std::vector<uint8_t>& d, const size_t at, uint32_t& v)
{
    if (at + 4 > d.size())
        return false;
    v = static_cast<uint32_t>(d[at]) << 24 | static_cast<uint32_t>(d[at + 1]) << 16 |
        static_cast<uint32_t>(d[at + 2]) << 8 | static_cast<uint32_t>(d[at + 3]);
    return true;
}

// Both file types share one shape: a four byte type tag, a four byte version, then a table of
// offsets, the LAST of which points at an extension data block. The block is a length, the offset of
// its data area, an entry count, and then that many twelve byte descriptors of two ids, an offset
// and a length. This finds one entry by its id pair and hands back where its data starts.
bool findExtensionEntry(const std::vector<uint8_t>& d, const size_t extAt, const uint16_t wantId1,
                        const uint16_t wantId2, size_t& dataAt, uint32_t& dataLen)
{
    if (extAt == 0 || extAt + 12 > d.size())
        return false;
    uint32_t entries = 0;
    if (!be32(d, extAt + 8, entries) || entries == 0 || entries > 64)
        return false;
    for (uint32_t i = 0; i < entries; ++i)
    {
        const size_t at = extAt + 12 + static_cast<size_t>(i) * 12;
        uint16_t id1 = 0, id2 = 0;
        uint32_t off = 0, len = 0;
        if (!be16(d, at, id1) || !be16(d, at + 2, id2) || !be32(d, at + 4, off) || !be32(d, at + 8, len))
            return false;
        if (id1 != wantId1 || id2 != wantId2)
            continue;
        const size_t start = extAt + off;
        if (off == 0 || len == 0 || start > d.size() || start + len > d.size())
            return false;
        dataAt = start;
        dataLen = len;
        return true;
    }
    return false;
}
}  // namespace

bool bdReadClipChunkSizes(const std::string& clpiPath, std::vector<int64_t>& sizes)
{
    sizes.clear();
    std::vector<uint8_t> d;
    if (!readWholeFile(clpiPath, d) || d.size() < 28 || memcmp(d.data(), "HDMV", 4) != 0)
        return false;

    // Five offsets follow the eight byte tag: sequence info, program info, CPI, clip mark, extension.
    uint32_t extAt = 0;
    if (!be32(d, 24, extAt))
        return false;

    size_t at = 0;
    uint32_t len = 0;
    // id 2/4 is the extent start point table, which exists only on a stereoscopic clip.
    if (!findExtensionEntry(d, extAt, 2, 4, at, len))
        return false;

    uint32_t blockLen = 0, count = 0;
    // A count of one is legitimate and common on short clips: one start point means the whole
    // file is a single chunk, and the caller's tail step supplies it.
    if (!be32(d, at, blockLen) || !be32(d, at + 4, count) || count < 1)
        return false;
    if (at + 8 + static_cast<size_t>(count) * 4 > d.size())
        return false;

    // Start points are source packet numbers. The gap between two of them is a chunk, and the last
    // start point opens the final chunk, which runs to the end of the file: the caller adds it,
    // because only the caller knows the file size.
    uint32_t prev = 0;
    if (!be32(d, at + 8, prev))
        return false;
    sizes.reserve(count - 1);
    for (uint32_t i = 1; i < count; ++i)
    {
        uint32_t spn = 0;
        if (!be32(d, at + 8 + static_cast<size_t>(i) * 4, spn) || spn <= prev)
        {
            sizes.clear();  // a table that does not increase is not a table this understands
            return false;
        }
        sizes.push_back(static_cast<int64_t>(spn - prev) * TS_PACKET_BYTES);
        prev = spn;
    }
    return true;
}

bool bdReadPlaylistStereoPair(const std::string& mplsPath, std::string& baseClipId, std::string& depClipId)
{
    baseClipId.clear();
    depClipId.clear();
    std::vector<uint8_t> d;
    if (!readWholeFile(mplsPath, d) || d.size() < 20 || memcmp(d.data(), "MPLS", 4) != 0)
        return false;

    uint32_t plAt = 0, extAt = 0;
    if (!be32(d, 8, plAt) || !be32(d, 16, extAt))
        return false;

    // The base view is the first play item's clip. Play list: length, reserved, play item count,
    // sub path count, then the items, each a two byte length followed by a five character clip id
    // and a four character codec id.
    if (plAt + 12 > d.size())
        return false;
    const size_t firstItem = plAt + 10;
    if (firstItem + 11 > d.size())
        return false;
    baseClipId.assign(reinterpret_cast<const char*>(d.data() + firstItem + 2), 5);

    // The dependent view is named in extension data id 2/2, the sub path entries extension, which a
    // 2D player never reads. Inside it: length, count, then sub paths of a four byte length, a
    // reserved byte, the type, and at offset nine the sub play item count.
    size_t at = 0;
    uint32_t len = 0;
    if (!findExtensionEntry(d, extAt, 2, 2, at, len) || len < 6)
    {
        baseClipId.clear();
        return false;
    }
    uint16_t subPaths = 0;
    if (!be16(d, at + 4, subPaths) || subPaths == 0 || subPaths > 64)
    {
        baseClipId.clear();
        return false;
    }
    size_t o = at + 6;
    const size_t blockEnd = at + len;
    for (uint16_t i = 0; i < subPaths && o + 10 <= blockEnd; ++i)
    {
        uint32_t spLen = 0;
        if (!be32(d, o, spLen) || spLen < 6 || o + 4 + spLen > blockEnd)
            break;
        const uint8_t subPathType = d[o + 5];
        const uint8_t itemCount = d[o + 9];
        // Type 8 is the stereoscopic dependent view. Nothing else here is of interest.
        if (subPathType == 8 && itemCount > 0 && o + 17 <= blockEnd)
        {
            depClipId.assign(reinterpret_cast<const char*>(d.data() + o + 12), 5);
            return true;
        }
        o += 4 + spLen;
    }
    baseClipId.clear();
    return false;
}

namespace
{
std::string toDiscRel(const std::string& root, const std::string& full)
{
    std::string rel = full.size() > root.size() ? full.substr(root.size()) : full;
    for (char& ch : rel)
        if (ch == '\\')
            ch = '/';
    // The walk can hand back a path with a doubled separator where the root already ended in
    // one. Left in, it makes "BDMV/STREAM//00800.m2ts", which matches nothing later.
    for (size_t i = 1; i < rel.size();)
        if (rel[i] == '/' && rel[i - 1] == '/')
            rel.erase(i, 1);
        else
            ++i;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    return rel;
}

// The final chunk is not in the table: it runs from the last start point to the end of the file.
// Adding it here is also the check that the table describes THIS file, because the chunks before it
// must leave a positive remainder.
bool appendTailChunk(std::vector<int64_t>& chunks, const int64_t fileSize)
{
    int64_t sum = 0;
    for (const int64_t c : chunks) sum += c;
    const int64_t tail = fileSize - sum;
    if (tail <= 0)
        return false;
    chunks.push_back(tail);
    return true;
}
}  // namespace

std::string bdFindSsifForM2ts(const std::string& m2tsPath, bool& isBaseView)
{
    isBaseView = false;

    // It has to sit where a disc puts it, <root>/BDMV/STREAM/xxxxx.m2ts. Anything else is not
    // a disc layout and there is nothing to look up.
    std::string norm = m2tsPath;
    for (char& ch : norm)
        if (ch == '\\')
            ch = '/';
    std::string upper = norm;
    for (char& ch : upper) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    const size_t streamPos = upper.rfind("/BDMV/STREAM/");
    if (streamPos == std::string::npos || upper.size() < 5 || upper.compare(upper.size() - 5, 5, ".M2TS") != 0)
        return std::string();

    const std::string root = norm.substr(0, streamPos);
    const std::string leaf = norm.substr(norm.find_last_of('/') + 1);
    if (leaf.size() < 6)
        return std::string();
    const std::string myId = leaf.substr(0, 5);

    std::vector<std::string> playlists;
    if (!findFilesRecursive(root + "/BDMV/PLAYLIST/", "*.mpls", &playlists))
        return std::string();

    for (const auto& mpls : playlists)
    {
        std::string baseId, depId;
        if (!bdReadPlaylistStereoPair(mpls, baseId, depId))
            continue;  // an ordinary 2D playlist, which is most of them
        if (myId != baseId && myId != depId)
            continue;
        // The interleaved file is named after the BASE clip, whichever half was asked about.
        const std::string rel = "BDMV/STREAM/SSIF/" + baseId + ".ssif";
        if (getFileSize(root + "/" + rel) == 0)
            continue;  // the pairing is stated but the file is not there
        isBaseView = myId == baseId;
        return rel;
    }
    return std::string();
}

std::vector<BdSsifGroup> bdFindSsifGroups(const std::string& srcRoot)
{
    std::vector<BdSsifGroup> groups;
    std::string root = srcRoot;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();

    std::vector<std::string> files;
    if (!findFilesRecursive(root + getDirSeparator(), "*", &files) || files.empty())
        return groups;

    // One pass to index what is on the disc, so the playlists below cost no more searching.
    std::map<std::string, std::string> mplsByName, clpiById, m2tsById, ssifById;
    for (const auto& full : files)
    {
        const std::string rel = toDiscRel(root, full);
        std::string upper = rel;
        for (char& ch : upper) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        const size_t slash = upper.find_last_of('/');
        const std::string leaf = slash == std::string::npos ? upper : upper.substr(slash + 1);
        if (leaf.size() < 6)
            continue;
        const std::string id = leaf.substr(0, 5);
        if (upper.find("/PLAYLIST/") != std::string::npos && leaf.size() > 5 && leaf.substr(5) == ".MPLS")
            mplsByName[id] = full;
        else if (upper.find("/CLIPINF/") != std::string::npos && leaf.substr(5) == ".CLPI")
            clpiById[id] = full;
        else if (upper.find("/STREAM/SSIF/") != std::string::npos && leaf.substr(5) == ".SSIF")
            ssifById[id] = rel;
        else if (upper.find("/STREAM/") != std::string::npos && leaf.substr(5) == ".M2TS")
            m2tsById[id] = rel;
    }

    std::vector<std::string> seen;
    for (const auto& [id, mplsPath] : mplsByName)
    {
        std::string baseId, depId;
        if (!bdReadPlaylistStereoPair(mplsPath, baseId, depId))
            continue;  // an ordinary 2D playlist, which is most of them
        if (std::find(seen.begin(), seen.end(), baseId) != seen.end())
            continue;  // two playlists can point at one pair; the group is wanted once
        // The interleaved file is named after the BASE clip. Every member has to be present: a
        // group missing one of its three files is not something to rebuild, it is something to
        // leave alone and copy as found.
        const auto ssif = ssifById.find(baseId);
        const auto base = m2tsById.find(baseId);
        const auto dep = m2tsById.find(depId);
        const auto baseClpi = clpiById.find(baseId);
        const auto depClpi = clpiById.find(depId);
        if (ssif == ssifById.end() || base == m2tsById.end() || dep == m2tsById.end() || baseClpi == clpiById.end() ||
            depClpi == clpiById.end())
            continue;

        BdSsifGroup g;
        g.ssifRel = ssif->second;
        g.baseRel = base->second;
        g.depRel = dep->second;
        // A clip with ONE chunk has a table holding a single start point, so it yields no
        // differences and a legitimately empty list. Ask whether the TABLE was there.
        if (!bdReadClipChunkSizes(baseClpi->second, g.baseChunkBytes) ||
            !bdReadClipChunkSizes(depClpi->second, g.depChunkBytes))
            continue;
        if (!appendTailChunk(g.baseChunkBytes, static_cast<int64_t>(getFileSize(root + "/" + g.baseRel))) ||
            !appendTailChunk(g.depChunkBytes, static_cast<int64_t>(getFileSize(root + "/" + g.depRel))))
            continue;
        // The two views interleave one chunk for one. Unequal counts mean this is not the layout
        // this understands, and guessing at it would produce a file of exactly the right size with
        // the halves scrambled, which is the worst way to be wrong.
        if (g.baseChunkBytes.size() != g.depChunkBytes.size())
        {
            LTRACE(LT_WARN, 2,
                   "3D: " << g.ssifRel << " has " << g.baseChunkBytes.size() << " base chunks against "
                          << g.depChunkBytes.size() << " dependent, so it is copied as found rather than rebuilt");
            continue;
        }
        seen.push_back(baseId);
        groups.push_back(std::move(g));
    }
    return groups;
}
