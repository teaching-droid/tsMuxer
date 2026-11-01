#ifndef MEMORY_MATROSKA_H_
#define MEMORY_MATROSKA_H_

#include <types/types.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// The pieces of a Matroska file built in memory, for a source read as the Matroska file that holds it (assFile):
// EBML (RFC 8794) and Matroska (RFC 9559). An element is its ID, its size as a variable length number, its
// body.
namespace memoryMatroska
{
inline void putId(std::vector<uint8_t>& out, const uint32_t id)
{
    for (int shift = id > 0xFFFFFF ? 24 : id > 0xFFFF ? 16 : id > 0xFF ? 8 : 0; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(id >> shift));
}

inline void putSize(std::vector<uint8_t>& out, const uint64_t size)
{
    // eight bytes: one size form for every element, the largest a file of 2^56 bytes needs
    out.push_back(0x01);
    for (int shift = 48; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(size >> shift));
}

inline void element(std::vector<uint8_t>& out, const uint32_t id, const std::vector<uint8_t>& body)
{
    putId(out, id);
    putSize(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

inline void uintElement(std::vector<uint8_t>& out, const uint32_t id, const uint64_t value)
{
    std::vector<uint8_t> body;
    int bytes = 1;
    while (bytes < 8 && (value >> (8 * bytes)) != 0) ++bytes;
    for (int i = bytes - 1; i >= 0; --i) body.push_back(static_cast<uint8_t>(value >> (8 * i)));
    element(out, id, body);
}

inline void intElement(std::vector<uint8_t>& out, const uint32_t id, const int64_t value)
{
    std::vector<uint8_t> body;
    for (int shift = 56; shift >= 0; shift -= 8) body.push_back(static_cast<uint8_t>(value >> shift));
    element(out, id, body);
}

inline void stringElement(std::vector<uint8_t>& out, const uint32_t id, const std::string& value)
{
    element(out, id, std::vector<uint8_t>(value.begin(), value.end()));
}

inline void floatElement(std::vector<uint8_t>& out, const uint32_t id, const double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    std::vector<uint8_t> body;
    for (int shift = 56; shift >= 0; shift -= 8) body.push_back(static_cast<uint8_t>(bits >> shift));
    element(out, id, body);
}

// The EBML header of a Matroska file
inline void ebmlHeader(std::vector<uint8_t>& out)
{
    std::vector<uint8_t> ebml;
    uintElement(ebml, 0x4286, 1);
    uintElement(ebml, 0x42F7, 1);
    uintElement(ebml, 0x42F2, 4);
    uintElement(ebml, 0x42F3, 8);
    stringElement(ebml, 0x4282, "matroska");
    uintElement(ebml, 0x4287, 4);
    uintElement(ebml, 0x4285, 2);
    element(out, 0x1A45DFA3, ebml);
}

// A UTF-8 path for the file streams. On Windows a long one gets the \\?\ prefix (windowsPath), as every file tsMuxeR
// opens does.
inline std::filesystem::path sourcePath(const std::string& utf8)
{
#ifdef _WIN32
    const std::string p = windowsPath(utf8);
#else
    const std::string& p = utf8;
#endif
    return {std::u8string(p.begin(), p.end())};
}
}  // namespace memoryMatroska

#endif
