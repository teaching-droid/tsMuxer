#ifndef LIBMEDIATION_DISC_IMAGE_H
#define LIBMEDIATION_DISC_IMAGE_H

// Reading inside a disc image (.iso) without mounting it: the UDF file system of a DVD (UDF 1.02) and of a Blu-ray or
// UHD Blu-ray (UDF 2.50 and 2.60, with the metadata partition). A path that goes on after the name of an image file,
// film.iso/BDMV/PLAYLIST/00001.mpls, names the file inside it. File and the folder functions take such paths and
// read through the file's places in the image, so a stream file is never copied out.

#include <cstdint>
#include <string>
#include <vector>

namespace discImage
{
// Where a part of a file's bytes is in the image, in the order of the file. A hole is space the file has but no data
// is written for; it reads as zeros.
struct Extent
{
    uint64_t offset = 0;
    uint64_t length = 0;
    bool hole = false;
};

// A file inside an image as File reads it: its parts in the image, or its bytes when they are kept in its entry.
struct FileView
{
    std::string imageFile;
    uint64_t size = 0;
    std::vector<Extent> extents;
    std::vector<uint8_t> data;
    bool inEntry = false;
};

struct Entry
{
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

// Splits a path into the image file and the path inside it ('/' separated, "" for the image's root). False when no
// part of the path ends with .iso and names an existing file: a plain path, which every function below leaves alone.
bool split(const std::string& path, std::string* imageFile, std::string* inner);

// True for a path that goes on inside an image (film.iso/BDMV, not film.iso itself).
bool isInside(const std::string& path);

// Fills `view` for a file inside an image. False when the image has no such file. Throws std::runtime_error with the
// reason when the image cannot be read: not a UDF image, or damaged where the file is.
bool openFile(const std::string& path, FileView* view);

// Why an image file cannot be read (not a UDF image, a kind of UDF not read); empty when it can.
std::string unreadableReason(const std::string& imageFile);

// For a path inside an image: whether it names a folder or a file there.
bool isDir(const std::string& path);
bool exists(const std::string& path);

// The entries of a folder inside an image, by name. False when it is no folder there.
bool list(const std::string& dir, std::vector<Entry>* entries);

// The file name match of the folder functions on Windows: '*' and '?', not case sensitive.
bool matchMask(const std::string& name, const std::string& mask);
}  // namespace discImage

#endif  // LIBMEDIATION_DISC_IMAGE_H
