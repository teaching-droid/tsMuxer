// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "discImage.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

#include "file.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace discImage
{
namespace
{
using Bytes = std::vector<uint8_t>;

uint16_t u16(const Bytes& b, const size_t o)
{
    return o + 2 <= b.size() ? static_cast<uint16_t>(b[o] | b[o + 1] << 8) : 0;
}

uint32_t u32(const Bytes& b, const size_t o)
{
    return o + 4 <= b.size() ? static_cast<uint32_t>(b[o]) | static_cast<uint32_t>(b[o + 1]) << 8 |
                                   static_cast<uint32_t>(b[o + 2]) << 16 | static_cast<uint32_t>(b[o + 3]) << 24
                             : 0;
}

uint64_t u64(const Bytes& b, const size_t o)
{
    return static_cast<uint64_t>(u32(b, o)) | static_cast<uint64_t>(u32(b, o + 4)) << 32;
}

// A name in OSTA compressed Unicode (CS0): 8 is one byte per character, 16 is UTF-16 big endian. The result is UTF-8;
// a surrogate pair becomes one character, a lone half U+FFFD.
std::string cs0(const Bytes& b, const size_t o, const size_t len)
{
    if (len == 0 || o + len > b.size())
        return {};
    std::string out;
    const auto put = [&](const uint32_t c)
    {
        if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | c >> 18);
            out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    };
    if (b[o] == 8)
    {
        for (size_t i = 1; i < len; ++i) put(b[o + i]);
    }
    else if (b[o] == 16)
    {
        for (size_t i = 1; i + 1 < len; i += 2)
        {
            uint32_t c = static_cast<uint32_t>(b[o + i]) << 8 | b[o + i + 1];
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < len)
            {
                const uint32_t lo = static_cast<uint32_t>(b[o + i + 2]) << 8 | b[o + i + 3];
                if (lo >= 0xDC00 && lo < 0xE000)
                {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (c >= 0xD800 && c < 0xE000)
                c = 0xFFFD;
            put(c);
        }
    }
    return out;
}

// A dstring: CS0 bytes with the length used in the last byte of the field.
std::string dstring(const Bytes& b, const size_t o, const size_t field)
{
    if (field == 0 || o + field > b.size())
        return {};
    const size_t used = b[o + field - 1];
    return used ? cs0(b, o, (std::min)(used, field - 1)) : std::string();
}

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A name from the image that can be one part of a path: not empty, no separator, drive colon or control character, and
// not made of dots and spaces only, which would climb out of its folder.
bool safeName(const std::string& s)
{
    if (s.find_first_not_of(". ") == std::string::npos)
        return false;
    for (const unsigned char c : s)
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':')
            return false;
    return true;
}

bool isRegularFile(const std::string& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(toWide(windowsPath(path)).data());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st
    {
    };
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

// The parts of a path inside an image; "." is left out and ".." goes up one.
std::vector<std::string> pathParts(const std::string& inner)
{
    std::vector<std::string> parts;
    std::string part;
    const auto take = [&]
    {
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
            parts.push_back(part);
        part.clear();
    };
    for (const char c : inner)
    {
        if (c == '/' || c == '\\')
            take();
        else
            part += c;
    }
    take();
    return parts;
}

class Image
{
   public:
    struct Node
    {
        std::string name;
        bool dir = false;
        uint64_t size = 0;
        std::vector<Extent> extents;
        Bytes data;  // the bytes of a file kept in its entry
        bool inEntry = false;
        std::string damaged;  // why its data cannot be read, empty when it can
        bool listed = false;
        std::vector<std::shared_ptr<Node>> children;
    };

    explicit Image(const std::string& path);

    std::shared_ptr<Node> find(const std::string& inner);
    const std::vector<std::shared_ptr<Node>>& children(Node& node);

    int64_t fileSize() const { return m_fileSize; }
    std::mutex& lock() { return m_lock; }

   private:
    struct Partition
    {
        uint16_t number = 0;
        uint32_t start = 0;          // a physical partition: its first sector
        bool metadata = false;       // a metadata partition: its blocks are those of the metadata file
        uint16_t physical = 0;       // the physical partition the metadata file is in
        std::vector<Extent> blocks;  // where the metadata file is
    };

    Bytes readAt(uint64_t offset, size_t n) const;
    uint64_t toOffset(uint16_t ref, uint32_t lbn) const;
    std::shared_ptr<Node> loadNode(uint32_t lbn, uint16_t ref, const std::string& name) const;
    Bytes readExtents(const std::vector<Extent>& extents, uint64_t limit) const;

    std::string m_path;
    File m_file;
    int64_t m_fileSize = 0;
    uint32_t m_block = 2048;
    std::vector<Partition> m_parts;
    std::shared_ptr<Node> m_root;
    std::set<uint64_t> m_folders;  // where the entry of each folder listed so far is
    std::mutex m_lock;             // the image file is read and the folders are filled in by one thread at a time
};

Image::Image(const std::string& path) : m_path(path)
{
    if (!m_file.open(path.c_str(), File::ofRead | File::ofOpenExisting) || !m_file.size(&m_fileSize))
        throw std::runtime_error(path + ": it cannot be opened");
    // The anchor at sector 256 points to the volume descriptors.
    const Bytes anchor = readAt(256ull * 2048, 2048);
    if (u16(anchor, 0) != 2)
        throw std::runtime_error(path + ": it has no UDF file system, which every Blu-ray and DVD has");
    const uint32_t vdsLength = u32(anchor, 16);
    const uint32_t vdsLocation = u32(anchor, 20);
    struct Map
    {
        int type;
        uint16_t number;
        uint32_t metadataFile;
    };
    std::vector<Map> maps;
    std::vector<std::pair<uint16_t, uint32_t>> starts;  // partition number and its first sector
    uint32_t fsdLbn = 0;
    uint16_t fsdRef = 0;
    bool haveLogicalVolume = false;
    for (uint32_t i = 0; i < (std::max)(1u, vdsLength / 2048) && i < 64; ++i)
    {
        const Bytes d = readAt((static_cast<uint64_t>(vdsLocation) + i) * 2048, 2048);
        const uint16_t tag = u16(d, 0);
        if (tag == 8)  // the terminating descriptor
            break;
        if (tag == 5)
            starts.emplace_back(u16(d, 22), u32(d, 188));
        if (tag == 6 && !haveLogicalVolume)
        {
            haveLogicalVolume = true;
            m_block = u32(d, 212);
            fsdLbn = u32(d, 248 + 4);
            fsdRef = u16(d, 248 + 8);
            const uint32_t count = u32(d, 268);
            size_t p = 440;
            for (uint32_t k = 0; k < count && p + 2 <= d.size(); ++k)
            {
                const uint8_t type = d[p];
                const uint8_t len = d[p + 1];
                if (len == 0 || p + len > d.size())
                    break;  // a table cut off
                if (type == 1)
                    maps.push_back({1, u16(d, p + 4), 0});
                else if (type == 2)
                {
                    if (p + 44 > d.size())
                        break;
                    const std::string id(reinterpret_cast<const char*>(&d[p + 5]), 23);
                    if (id.rfind("*UDF Metadata Partition", 0) == 0)
                        maps.push_back({2, u16(d, p + 38), u32(d, p + 40)});
                    else
                        maps.push_back({3, u16(d, p + 38), 0});  // virtual or sparable: written discs only
                }
                p += len;
            }
        }
    }
    if (!haveLogicalVolume)
        throw std::runtime_error(path + ": its UDF file system has no logical volume");
    if (m_block != 2048)
        throw std::runtime_error(path + ": its UDF blocks have " + std::to_string(m_block) +
                                 " bytes, where a disc has blocks of 2048 bytes");
    const auto startOf = [&](const uint16_t number)
    {
        for (const auto& [n, s] : starts)
            if (n == number)
                return s;
        throw std::runtime_error(path + ": its UDF partition " + std::to_string(number) + " is not described");
    };
    for (const auto& map : maps)
    {
        Partition p;
        p.number = map.number;
        if (map.type == 3)
            throw std::runtime_error(
                path + ": it has a UDF partition of a written disc (virtual or sparable), which is not read");
        p.start = startOf(p.number);
        if (map.type == 2)
        {
            p.metadata = true;
            for (size_t k = 0; k < maps.size(); ++k)
                if (maps[k].type == 1 && maps[k].number == p.number)
                    p.physical = static_cast<uint16_t>(k);
        }
        m_parts.push_back(p);
    }
    if (m_parts.empty())
        throw std::runtime_error(path + ": its UDF file system has no partitions");
    // A metadata partition maps its blocks through the metadata file, which is a file of the physical partition.
    for (size_t i = 0; i < m_parts.size(); ++i)
    {
        if (!m_parts[i].metadata)
            continue;
        uint32_t location = 0;
        for (const auto& map : maps)
            if (map.type == 2 && map.number == m_parts[i].number)
                location = map.metadataFile;
        const auto file = loadNode(location, m_parts[i].physical, "metadata");
        if (!file || file->extents.empty() || !file->damaged.empty())
            throw std::runtime_error(path + ": its UDF metadata file cannot be read");
        m_parts[i].blocks = file->extents;
    }
    const Bytes fsd = readAt(toOffset(fsdRef, fsdLbn), 2048);
    if (u16(fsd, 0) != 256)
        throw std::runtime_error(path + ": its UDF file system has no file set");
    m_root = loadNode(u32(fsd, 400 + 4), u16(fsd, 400 + 8), "");
    if (!m_root || !m_root->dir)
        throw std::runtime_error(path + ": its root folder cannot be read");
    m_folders.insert(toOffset(u16(fsd, 400 + 8), u32(fsd, 400 + 4)));
}

Bytes Image::readAt(const uint64_t offset, size_t n) const
{
    if (offset >= static_cast<uint64_t>(m_fileSize))
        return {};
    n = static_cast<size_t>((std::min)(static_cast<uint64_t>(n), static_cast<uint64_t>(m_fileSize) - offset));
    Bytes b(n);
    if (m_file.seek(static_cast<int64_t>(offset)) != static_cast<int64_t>(offset))
        return {};
    size_t done = 0;
    while (done < n)
    {
        const int r = m_file.read(b.data() + done, static_cast<uint32_t>(n - done));
        if (r <= 0)
            break;
        done += static_cast<size_t>(r);
    }
    b.resize(done);
    return b;
}

uint64_t Image::toOffset(const uint16_t ref, const uint32_t lbn) const
{
    if (ref >= m_parts.size())
        throw std::runtime_error("a UDF partition that is not there");
    const Partition& p = m_parts[ref];
    if (!p.metadata)
        return (static_cast<uint64_t>(p.start) + lbn) * m_block;
    uint64_t want = static_cast<uint64_t>(lbn) * m_block;
    for (const auto& e : p.blocks)
    {
        if (want < e.length)
        {
            if (e.hole)
                throw std::runtime_error("a block of the UDF metadata partition that holds no data");
            return e.offset + want;
        }
        want -= e.length;
    }
    throw std::runtime_error("a block outside the UDF metadata partition");
}

std::shared_ptr<Image::Node> Image::loadNode(const uint32_t lbn, const uint16_t ref, const std::string& name) const
{
    const Bytes d = readAt(toOffset(ref, lbn), m_block);
    const uint16_t tag = u16(d, 0);
    if (tag != 261 && tag != 266)  // a file entry or an extended file entry
        return nullptr;
    auto node = std::make_shared<Node>();
    node->name = name;
    node->dir = d.size() > 27 && d[27] == 4;
    const uint16_t flags = u16(d, 34);
    node->size = u64(d, 56);
    const size_t ea = tag == 261 ? u32(d, 168) : u32(d, 208);
    const size_t adLength = tag == 261 ? u32(d, 172) : u32(d, 212);
    const size_t ad = (tag == 261 ? 176 : 216) + ea;
    if (ad + adLength > d.size())
        return nullptr;
    switch (flags & 7)
    {
    case 3:  // the data is kept in the entry
        node->inEntry = true;
        node->data.assign(
            d.begin() + static_cast<std::ptrdiff_t>(ad),
            d.begin() + static_cast<std::ptrdiff_t>(ad + (std::min)(adLength, static_cast<size_t>(node->size))));
        if (node->data.size() < node->size)
            node->damaged = "its entry holds " + std::to_string(node->data.size()) + " of its " +
                            std::to_string(node->size) + " bytes";
        break;
    case 0:  // short descriptors: in the entry's own partition
    case 1:  // long descriptors: in any partition
    {
        // Kind 0 is recorded data; 1 and 2 are space without data, read as zeros; 3 says the list goes on in another
        // block, which is followed with a limit and a guard against a list that goes round.
        const bool isLong = (flags & 7) == 1;
        const size_t step = isLong ? 16 : 8;
        std::set<uint64_t> seen;
        int blocks = 0;
        std::function<void(const Bytes&, size_t, size_t)> descriptors =
            [&](const Bytes& b, const size_t at, const size_t len)
        {
            for (size_t p = at; p + step <= at + len && p + step <= b.size(); p += step)
            {
                const uint32_t length = u32(b, p) & 0x3FFFFFFF;
                const uint32_t kind = u32(b, p) >> 30;
                const uint32_t block = u32(b, p + 4);
                const uint16_t partRef = isLong ? u16(b, p + 8) : ref;
                if (length == 0)
                    break;
                if (kind == 3)
                {
                    const uint64_t offset = toOffset(partRef, block);
                    if (!seen.insert(offset).second || ++blocks > 4096)
                    {
                        node->damaged = "its list of places goes round in a circle";
                        return;
                    }
                    const Bytes next = readAt(offset, m_block);
                    if (next.size() < 24 || u16(next, 0) != 258)
                    {
                        node->damaged = "the rest of its list of places is missing";
                        return;
                    }
                    descriptors(next, 24, (std::min)(static_cast<size_t>(u32(next, 20)), next.size() - 24));
                    return;
                }
                if (kind == 0)
                    node->extents.push_back({toOffset(partRef, block), length, false});
                else
                    node->extents.push_back({0, length, true});
            }
        };
        try
        {
            descriptors(d, ad, adLength);
        }
        catch (const std::exception& e)
        {
            node->damaged = e.what();
        }
        uint64_t total = 0;
        for (const auto& e : node->extents) total += e.length;
        if (node->damaged.empty() && total < node->size)
            node->damaged =
                "its places hold " + std::to_string(total) + " of its " + std::to_string(node->size) + " bytes";
        for (const auto& e : node->extents)
            if (node->damaged.empty() && !e.hole && e.offset + e.length > static_cast<uint64_t>(m_fileSize))
                node->damaged = "a part of it lies past the end of the image file";
        break;
    }
    default:
        return nullptr;  // extended descriptors are not used on discs
    }
    return node;
}

Bytes Image::readExtents(const std::vector<Extent>& extents, const uint64_t limit) const
{
    Bytes out;
    for (const auto& e : extents)
    {
        if (out.size() >= limit)
            break;
        const auto n = static_cast<size_t>((std::min)(e.length, limit - out.size()));
        if (e.hole)
        {
            out.insert(out.end(), n, static_cast<uint8_t>(0));
            continue;
        }
        const Bytes part = readAt(e.offset, n);
        out.insert(out.end(), part.begin(), part.end());
        if (part.size() < n)
            break;
    }
    return out;
}

const std::vector<std::shared_ptr<Image::Node>>& Image::children(Node& node)
{
    if (node.listed)
        return node.children;
    node.listed = true;
    if (!node.damaged.empty())
        return node.children;
    Bytes d =
        node.inEntry ? node.data : readExtents(node.extents, (std::min)(node.size, static_cast<uint64_t>(16u << 20)));
    if (d.size() > node.size)
        d.resize(static_cast<size_t>(node.size));
    for (size_t p = 0; p + 38 <= d.size();)
    {
        if (u16(d, p) != 257)  // a file identifier descriptor
            break;
        const uint8_t characteristics = d[p + 18];
        const uint8_t nameLength = d[p + 19];
        const uint16_t useLength = u16(d, p + 36);
        const uint32_t lbn = u32(d, p + 20 + 4);
        const uint16_t ref = u16(d, p + 20 + 8);
        const size_t total = (38 + useLength + nameLength + 3) & ~static_cast<size_t>(3);
        // Not the parent, not deleted. A hidden file is listed, as Windows lists it: it is on the disc, and a copy of
        // the disc keeps it.
        if (!(characteristics & 0x08) && !(characteristics & 0x04))
        {
            const std::string name = cs0(d, p + 38 + useLength, nameLength);
            if (safeName(name))
            {
                try
                {
                    // A folder is in one place of the tree. One that is there already, the folder itself or one
                    // above it in a damaged image, is left out: a search through the folders would never end.
                    auto child = loadNode(lbn, ref, name);
                    if (child && (!child->dir || m_folders.insert(toOffset(ref, lbn)).second))
                        node.children.push_back(child);
                }
                catch (const std::exception&)
                {
                    // an entry that points outside its partition: left out, as an unreadable one
                }
            }
        }
        p += total;
    }
    std::sort(node.children.begin(), node.children.end(),
              [](const std::shared_ptr<Node>& a, const std::shared_ptr<Node>& b) { return a->name < b->name; });
    return node.children;
}

std::shared_ptr<Image::Node> Image::find(const std::string& inner)
{
    auto current = m_root;
    const auto parts = pathParts(inner);
    if (parts.size() > 64)
        return nullptr;  // a folder inside itself in a damaged image: a search through it ends here
    for (const auto& part : parts)
    {
        if (!current->dir)
            return nullptr;
        std::shared_ptr<Node> next;
        const std::string want = lower(part);
        for (const auto& child : children(*current))
            if (lower(child->name) == want)
            {
                next = child;
                break;
            }
        if (!next)
            return nullptr;
        current = next;
    }
    return current;
}

struct CachedImage
{
    int64_t fileSize = -1;
    std::shared_ptr<Image> image;
    std::string error;
    uint64_t used = 0;
};

std::mutex cacheLock;
std::map<std::string, CachedImage>& cache()
{
    static std::map<std::string, CachedImage> images;
    return images;
}

// The image of a path, read once and kept: its folders are listed again and again (fileExists, findFiles). Read anew
// when its size changed. Throws the reason when it is no UDF image. The 8 used last are kept, each with the image file
// open; a file still read from keeps its image while it is open.
std::shared_ptr<Image> imageOf(const std::string& imageFile)
{
    int64_t size = -1;
    {
        File f;
        if (!f.open(imageFile.c_str(), File::ofRead | File::ofOpenExisting) || !f.size(&size))
            throw std::runtime_error(imageFile + ": it cannot be opened");
    }
    std::lock_guard<std::mutex> guard(cacheLock);
    static uint64_t uses = 0;
    auto& images = cache();
    if (images.size() >= 8 && !images.count(imageFile))
        images.erase(std::min_element(images.begin(), images.end(),
                                      [](const auto& a, const auto& b) { return a.second.used < b.second.used; }));
    auto& entry = images[imageFile];
    entry.used = ++uses;
    if (entry.fileSize != size)
    {
        entry = CachedImage();
        entry.fileSize = size;
        entry.used = uses;
        try
        {
            entry.image = std::make_shared<Image>(imageFile);
        }
        catch (const std::exception& e)
        {
            // Said with the name of the image; a place that is not there, found on the way, makes it a damaged one.
            entry.error = e.what();
            if (entry.error.rfind(imageFile + ": ", 0) != 0)
                entry.error = imageFile + ": it is damaged: " + entry.error;
        }
    }
    if (!entry.image)
        throw std::runtime_error(entry.error);
    return entry.image;
}

bool endsWithIso(const std::string& part) { return part.size() > 4 && lower(part.substr(part.size() - 4)) == ".iso"; }
}  // namespace

bool split(const std::string& path, std::string* imageFile, std::string* inner)
{
    // A part ending with .iso is looked at only when the path goes on after it or is that file itself, from the left:
    // the first such part that is a file is the image.
    size_t begin = 0;
    while (begin <= path.size())
    {
        size_t end = path.find_first_of("/\\", begin);
        if (end == std::string::npos)
            end = path.size();
        if (end > begin && endsWithIso(path.substr(begin, end - begin)))
        {
            const std::string candidate = path.substr(0, end);
            if (isRegularFile(candidate))
            {
                if (imageFile)
                    *imageFile = candidate;
                if (inner)
                    *inner = end < path.size() ? path.substr(end + 1) : std::string();
                return true;
            }
        }
        begin = end + 1;
    }
    return false;
}

std::mutex virtualLock;
std::map<std::string, std::vector<Piece>>& virtualFiles()
{
    static std::map<std::string, std::vector<Piece>> files;
    return files;
}

std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>& memoryFiles()
{
    static std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files;
    return files;
}

std::shared_ptr<const std::vector<uint8_t>> memoryFile(const std::string& name)
{
    std::lock_guard<std::mutex> guard(virtualLock);
    const auto it = memoryFiles().find(name);
    return it == memoryFiles().end() ? nullptr : it->second;
}

bool virtualFile(const std::string& name, std::vector<Piece>* pieces)
{
    std::lock_guard<std::mutex> guard(virtualLock);
    if (memoryFiles().count(name))
    {
        if (pieces)
            pieces->clear();
        return true;
    }
    const auto it = virtualFiles().find(name);
    if (it == virtualFiles().end())
        return false;
    if (pieces)
        *pieces = it->second;
    return true;
}

void addMemoryFile(const std::string& name, std::vector<uint8_t> bytes)
{
    std::lock_guard<std::mutex> guard(virtualLock);
    memoryFiles()[name] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

void addVirtualFile(const std::string& name, const std::vector<Piece>& pieces)
{
    std::lock_guard<std::mutex> guard(virtualLock);
    virtualFiles()[name] = pieces;
}

bool isInside(const std::string& path)
{
    if (virtualFile(path, nullptr))
        return true;
    // Cheap first: most paths have no .iso part at all.
    if (lower(path).find(".iso") == std::string::npos)
        return false;
    std::string inner;
    return split(path, nullptr, &inner) && !pathParts(inner).empty();
}

bool openFile(const std::string& path, FileView* view)
{
    if (const auto bytes = memoryFile(path))
    {
        view->imageFile.clear();
        view->files.clear();
        view->extents.clear();
        view->data = bytes;
        view->size = bytes->size();
        view->inEntry = true;
        return true;
    }
    if (std::vector<Piece> pieces; virtualFile(path, &pieces))
    {
        view->imageFile.clear();
        view->files.clear();
        view->size = 0;
        view->inEntry = false;
        for (const Piece& p : pieces)
        {
            auto at = std::find(view->files.begin(), view->files.end(), p.file);
            if (at == view->files.end())
                at = view->files.insert(view->files.end(), p.file);
            Extent e;
            e.offset = p.offset;
            e.length = p.length;
            e.file = static_cast<int>(at - view->files.begin());
            view->extents.push_back(e);
            view->size += p.length;
        }
        return true;
    }
    std::string imageFile, inner;
    if (!split(path, &imageFile, &inner))
        return false;
    const auto image = imageOf(imageFile);
    std::lock_guard<std::mutex> guard(image->lock());
    const auto node = image->find(inner);
    if (!node || node->dir)
        return false;
    if (!node->damaged.empty())
        throw std::runtime_error("The disc image " + imageFile + " is damaged at " + inner + ": " + node->damaged);
    view->imageFile = imageFile;
    view->files = {imageFile};
    view->size = node->size;
    view->inEntry = node->inEntry;
    view->data = node->inEntry ? std::make_shared<const std::vector<uint8_t>>(node->data) : nullptr;
    view->extents = node->extents;
    return true;
}

std::string unreadableReason(const std::string& imageFile)
{
    try
    {
        imageOf(imageFile);
        return {};
    }
    catch (const std::exception& e)
    {
        // The reason without the name of the image, which the caller says
        const std::string reason = e.what();
        return reason.rfind(imageFile + ": ", 0) == 0 ? reason.substr(imageFile.size() + 2) : reason;
    }
}

bool isDir(const std::string& path)
{
    std::string imageFile, inner;
    if (!split(path, &imageFile, &inner))
        return false;
    try
    {
        const auto image = imageOf(imageFile);
        std::lock_guard<std::mutex> guard(image->lock());
        const auto node = image->find(inner);
        return node && node->dir;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool exists(const std::string& path)
{
    if (virtualFile(path, nullptr))
        return true;
    std::string imageFile, inner;
    if (!split(path, &imageFile, &inner))
        return false;
    try
    {
        const auto image = imageOf(imageFile);
        std::lock_guard<std::mutex> guard(image->lock());
        return image->find(inner) != nullptr;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool list(const std::string& dir, std::vector<Entry>* entries)
{
    std::string imageFile, inner;
    if (!split(dir, &imageFile, &inner))
        return false;
    try
    {
        const auto image = imageOf(imageFile);
        std::lock_guard<std::mutex> guard(image->lock());
        const auto node = image->find(inner);
        if (!node || !node->dir)
            return false;
        for (const auto& child : image->children(*node)) entries->push_back({child->name, child->dir, child->size});
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool matchMask(const std::string& name, const std::string& mask)
{
    const std::string n = lower(name), m = lower(mask);
    size_t i = 0, j = 0, star = std::string::npos, mark = 0;
    while (i < n.size())
    {
        if (j < m.size() && (m[j] == '?' || m[j] == n[i]))
        {
            ++i;
            ++j;
        }
        else if (j < m.size() && m[j] == '*')
        {
            star = j++;
            mark = i;
        }
        else if (star != std::string::npos)
        {
            j = star + 1;
            i = ++mark;
        }
        else
            return false;
    }
    while (j < m.size() && m[j] == '*') ++j;
    return j == m.size();
}
}  // namespace discImage
