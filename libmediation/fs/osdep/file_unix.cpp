/***********************************************************************
 *	File: file.cpp
 *	Author: Andrey Kolesnikov
 *	Date: 13 oct 2006
 ***********************************************************************/

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>

#include "../directory.h"
#include "../file.h"

namespace
{
int to_fd(void* impl) { return static_cast<int>(reinterpret_cast<std::intptr_t>(impl)); }
void* from_fd(int fd) { return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)); }

int makeUnixOpenFlags(unsigned int oflag)
{
    int sysFlags = 0;
    if (oflag & File::ofRead)
        sysFlags = O_RDONLY;
    if (oflag & File::ofWrite)
    {
        if (oflag & File::ofRead)
            sysFlags = O_RDWR | O_TRUNC;
        else
            sysFlags = O_WRONLY | O_TRUNC;
        if (!(oflag & File::ofOpenExisting))
            sysFlags |= O_CREAT;
        if (oflag & File::ofNoTruncate)
        {
            sysFlags &= ~O_TRUNC;
        }
        // Not O_APPEND: that sends every write to the end of the file, whatever the position, and a WAV or W64
        // file gets its sizes written into its header at the start once its data is written. On Linux and macOS
        // that patch landed after the audio: the header kept its placeholder sizes and 8 bytes followed the
        // audio. The file is opened at its end instead (openAtEnd), as on Windows, and a seek then works.
        if (oflag & File::ofAppend)
            sysFlags &= ~O_TRUNC;
    }
    if (oflag & File::ofCreateNew)
        sysFlags |= O_CREAT | O_EXCL;
    return sysFlags;
}
}  // namespace

// An append opens at the end of the file; a seek can still move away from it.
static void openAtEnd(const int fd, const unsigned int oflag)
{
    if (fd != -1 && (oflag & File::ofAppend))
        lseek(fd, 0, SEEK_END);
}

File::File() : m_impl(from_fd(-1)), m_pos(0) {}

File::File(const char* fName, unsigned int oflag, unsigned int systemDependentFlags) : m_name(fName), m_pos(0)
{
    int sysFlags = makeUnixOpenFlags(oflag);
    auto fd = ::open(fName, sysFlags | systemDependentFlags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    openAtEnd(fd, oflag);
    if (fd == -1)
    {
        std::ostringstream ss;
        ss << "Error opening file " << fName << ": " << strerror(errno) << "(" << errno << ")";
        throw std::runtime_error(ss.str());
    }
    m_impl = from_fd(fd);
}

File::~File()
{
    // Use qualified call to bypass virtual dispatch, ensuring File::close()
    // is called even if a derived class overrides close().
    if (isOpen())
        File::close();
}

bool File::open(const char* fName, unsigned int oflag, unsigned int systemDependentFlags)
{
    m_name = fName;
    // The position counts from the start of THIS file, as on Windows. It ran on from the file opened before, so a split
    // Matroska output, which opens each part with the same File, wrote the sizes and the index of every part after the
    // first at the wrong places.
    m_pos = 0;

    if (isOpen())
        close();

    int sysFlags = makeUnixOpenFlags(oflag);
    // The folder is made only for a file the open may create, as on Windows: a source that does not exist got its
    // folders made, on a drive that is only read from as well.
    if (sysFlags & O_CREAT)
        createDir(extractFileDir(fName), true);
    auto fd = ::open(fName, sysFlags | systemDependentFlags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    openAtEnd(fd, oflag);
    m_impl = from_fd(fd);
    return fd != -1;
}

bool File::close()
{
    if (::close(to_fd(m_impl)) == 0)
    {
        m_impl = from_fd(-1);
        return true;
    }

    return false;
}

int File::read(void* buffer, uint32_t count) const
{
    if (!isOpen())
        return -1;
    m_pos += count;
    return ::read(to_fd(m_impl), buffer, count);
}

// A failed or short write was returned as it came, the position moved on regardless, and none of the writers
// looked. On a full drive a TS or Matroska output was cut short under "Mux successful", and an ISO never finished:
// its padding loop waited for a file that could not grow. As on Windows, a write now completes or throws, with the
// reason, and the job stops on it.
int File::write(const void* buffer, uint32_t count)
{
    if (!isOpen())
        return -1;
    const auto* data = static_cast<const char*>(buffer);
    uint32_t left = count;
    while (left > 0)
    {
        const ssize_t written = ::write(to_fd(m_impl), data, left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
        {
            std::ostringstream ss;
            ss << "Error writing file " << m_name << ": "
               << (written < 0 ? strerror(errno) : "nothing could be written (is the drive full?)");
            if (written < 0)
                ss << " (" << errno << ")";
            throw std::runtime_error(ss.str());
        }
        data += written;
        left -= static_cast<uint32_t>(written);
        m_pos += written;
    }
    return static_cast<int>(count);
}

bool File::isOpen() const { return to_fd(m_impl) != -1; }

bool File::size(int64_t* const fileSize) const
{
    bool res = false;

    struct stat buf;

    if (isOpen() && (fstat(to_fd(m_impl), &buf) == 0))
    {
        *fileSize = buf.st_size;
        res = true;
    }

    return res;
}

int64_t File::seek(const int64_t offset, const SeekMethod whence) const
{
    if (!isOpen())
        return UINT64_C(-1);

    int sWhence = 0;
    switch (whence)
    {
    case SeekMethod::smBegin:
        sWhence = SEEK_SET;
        break;
    case SeekMethod::smCurrent:
        sWhence = SEEK_CUR;
        break;
    case SeekMethod::smEnd:
        sWhence = SEEK_END;
        break;
    }
    m_pos = offset;
    return lseek(to_fd(m_impl), offset, sWhence);
}

bool File::truncate(const uint64_t newFileSize) const { return ftruncate(to_fd(m_impl), newFileSize) == 0; }

// POSIX files are sparse by default: extending with ftruncate() leaves a hole, and writing past a gap
// keeps earlier gaps as holes. Nothing to enable, so just report that sparse behaviour is available.
bool File::setSparse() const { return isOpen(); }

// Only Windows writes unbuffered (FILE_FLAG_NO_BUFFERING), so only there does the sector size matter.
uint32_t File::volumeSectorSize(const char*) { return 0; }

void File::sync() { ::sync(); }

#endif