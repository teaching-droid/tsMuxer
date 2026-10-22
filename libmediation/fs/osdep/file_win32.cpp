#include <io.h>
#include <windows.h>

#include <winioctl.h>  // FSCTL_SET_SPARSE
#include <stdexcept>
#include <string>

#include "../directory.h"
#include "../file.h"

// FormatMessage is FormatMessageW in this build, so the text came back in UTF-16 and was read as 8-bit
// characters: every message stopped after its first letter, and a write Windows refused said only "T"
// (W1-6(d)). The text is asked for in UTF-16 on purpose now and turned into UTF-8, which is what tsMuxeR
// prints everywhere, and the error number goes with it. The buffer Windows allocates is given back.
[[noreturn]] void throwFileError()
{
    const DWORD dw = GetLastError();
    LPWSTR msgBuf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&msgBuf), 0, nullptr);
    std::string text;
    if (len > 0 && msgBuf)
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, msgBuf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        if (bytes > 0)
        {
            text.resize(bytes);
            WideCharToMultiByte(CP_UTF8, 0, msgBuf, static_cast<int>(len), text.data(), bytes, nullptr, nullptr);
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    }
    if (msgBuf)
        LocalFree(msgBuf);
    if (text.empty())
        text = "Windows could not complete a file operation.";
    throw std::runtime_error(text + " (Windows error " + std::to_string(dw) + ")");
}

void makeWin32OpenFlags(const unsigned int oflag, DWORD* const dwDesiredAccess, DWORD* const dwCreationDisposition,
                        DWORD* const dwShareMode)
{
    *dwDesiredAccess = 0;
    *dwCreationDisposition = CREATE_ALWAYS;
    if (oflag & File::ofRead)
    {
        *dwDesiredAccess = GENERIC_READ & ~SYNCHRONIZE;
        *dwCreationDisposition = OPEN_EXISTING;
        *dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }
    if (oflag & File::ofWrite)
    {
        *dwDesiredAccess |= GENERIC_WRITE & ~SYNCHRONIZE;
        *dwCreationDisposition = CREATE_ALWAYS;
        *dwShareMode = FILE_SHARE_READ;
    }
    if (oflag & File::ofAppend)
        *dwCreationDisposition = OPEN_ALWAYS;
    if (oflag & File::ofNoTruncate)
        *dwCreationDisposition = OPEN_ALWAYS;
    if (oflag & File::ofOpenExisting)
        *dwCreationDisposition = OPEN_EXISTING;
    if (oflag & File::ofCreateNew)
        *dwCreationDisposition = CREATE_NEW;
}

File::File() : AbstractOutputStream(), m_impl(INVALID_HANDLE_VALUE), m_pos(0) {}

File::File(const char* fName, const unsigned int oflag,
           unsigned int systemDependentFlags) /* throw ( std::runtime_error ) */
    : AbstractOutputStream(), m_impl(INVALID_HANDLE_VALUE), m_name(fName), m_pos(0)
{
    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = CREATE_ALWAYS;
    DWORD dwShareMode = 0;
    makeWin32OpenFlags(oflag, &dwDesiredAccess, &dwCreationDisposition, &dwShareMode);
    if (!systemDependentFlags)
    {
        if (((oflag & ofRead) && !(oflag & ofWrite)) || (oflag & ofSequential))
            systemDependentFlags = FILE_FLAG_SEQUENTIAL_SCAN;
        else
            systemDependentFlags = FILE_FLAG_RANDOM_ACCESS;
    }
    m_impl = CreateFile(toWide(windowsPath(fName)).data(), dwDesiredAccess, dwShareMode, nullptr, dwCreationDisposition,
                        systemDependentFlags, nullptr);
    if (m_impl == INVALID_HANDLE_VALUE)
    {
        throwFileError();
    }
    else
    {
        if (oflag & ofAppend)
        {
            long hiword = 0;
            const DWORD newPointerLow = SetFilePointer(m_impl, 0, &hiword, FILE_END);
            if (newPointerLow == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
                throwFileError();
        }
    }
}

File::~File()
{
    // Use qualified call to bypass virtual dispatch, ensuring File::close()
    // is called even if a derived class overrides close().
    if (isOpen())
        File::close();
}

bool File::open(const char* fName, const unsigned int oflag, unsigned int systemDependentFlags)
{
    m_name = fName;
    m_pos = 0;

    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = CREATE_ALWAYS;
    DWORD dwShareMode = 0;
    makeWin32OpenFlags(oflag, &dwDesiredAccess, &dwCreationDisposition, &dwShareMode);
    if (!systemDependentFlags)
    {
        if (((oflag & ofRead) && !(oflag & ofWrite)) || (oflag & ofSequential))
            systemDependentFlags = FILE_FLAG_SEQUENTIAL_SCAN;
        else
            systemDependentFlags = FILE_FLAG_RANDOM_ACCESS;
    }

    if ((oflag & ofOpenExisting) == 0)
    {
        createDir(extractFileDir(fName), true);
    }
    m_impl = CreateFile(toWide(windowsPath(fName)).data(), dwDesiredAccess, dwShareMode, nullptr, dwCreationDisposition,
                        systemDependentFlags, nullptr);
    if (m_impl == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    if (oflag & ofAppend)
    {
        long hiword = 0;
        const DWORD newPointerLow = SetFilePointer(m_impl, 0, &hiword, FILE_END);
        if (newPointerLow == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
            throwFileError();
    }

    return true;
}

bool File::close()
{
    // sync();
    const BOOL res = CloseHandle(m_impl);
    m_impl = INVALID_HANDLE_VALUE;
    return res != 0;
}

int File::read(void* buffer, const uint32_t count) const
{
    if (!isOpen())
        return -1;

    DWORD bytesRead = 0;
    const BOOL res = ReadFile(m_impl, buffer, count, &bytesRead, nullptr);
    if (!res)
        return -1;

    m_pos += bytesRead;

    return static_cast<int>(bytesRead);
}

int File::write(const void* buffer, const uint32_t count)
{
    if (!isOpen())
        return -1;

    DWORD bytesWritten = 0;
    const BOOL res = WriteFile(m_impl, buffer, count, &bytesWritten, nullptr);
    if (!res)
    {
        throwFileError();
    }
    if (!res)
        return -1;

    m_pos += bytesWritten;

    return static_cast<int>(bytesWritten);
}

void File::sync() { FlushFileBuffers(m_impl); }

bool File::isOpen() const { return m_impl != INVALID_HANDLE_VALUE; }

bool File::size(int64_t* const fileSize) const
{
    DWORD highDw;
    const DWORD lowDw = GetFileSize(m_impl, &highDw);
    if ((lowDw == INVALID_FILE_SIZE) && (GetLastError() != NO_ERROR))
        return false;
    *fileSize = highDw;
    *fileSize <<= 32;
    *fileSize |= lowDw;
    return true;
}

int64_t File::seek(const int64_t offset, const SeekMethod whence) const
{
    if (!isOpen())
        return -1;

    DWORD moveMethod = FILE_BEGIN;
    switch (whence)
    {
    case SeekMethod::smBegin:
        moveMethod = FILE_BEGIN;
        break;
    case SeekMethod::smCurrent:
        moveMethod = FILE_CURRENT;
        break;
    case SeekMethod::smEnd:
        moveMethod = FILE_END;
        break;
    }

    const LONG distanceToMoveLow = static_cast<LONG>(offset);
    LONG distanceToMoveHigh = static_cast<LONG>(offset >> 32);

    const DWORD newPointerLow = SetFilePointer(m_impl, distanceToMoveLow, &distanceToMoveHigh, moveMethod);
    if (newPointerLow == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return -1;

    m_pos = static_cast<int64_t>(distanceToMoveHigh) << 32 | newPointerLow;

    return m_pos;
}

bool File::truncate(const uint64_t newFileSize) const
{
    const LONG distanceToMoveLow = static_cast<LONG>(newFileSize);
    LONG distanceToMoveHigh = static_cast<LONG>(newFileSize >> 32);
    const DWORD newPointerLow = SetFilePointer(m_impl, distanceToMoveLow, &distanceToMoveHigh, FILE_BEGIN);
    const DWORD errCode = GetLastError();
    if (newPointerLow == INVALID_SET_FILE_POINTER && errCode != NO_ERROR)
        throwFileError();

    return SetEndOfFile(m_impl) > 0;
}

bool File::setSparse() const
{
    if (!isOpen())
        return false;
    DWORD bytesReturned = 0;
    return DeviceIoControl(m_impl, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) != 0;
}

// The volume a folder really lies on, as \\?\Volume{...}\, asked of the folder itself. A junction or a symbolic link
// on the way, or a drive mounted in a folder, leads to another volume than the one the path names.
static bool realVolumeOfFolder(const wchar_t* folder, wchar_t* volume, const DWORD size)
{
    const HANDLE dir = CreateFileW(folder, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE)
        return false;
    const DWORD len = GetFinalPathNameByHandleW(dir, volume, size, VOLUME_NAME_GUID | FILE_NAME_NORMALIZED);
    CloseHandle(dir);
    if (len == 0 || len >= size || wcsncmp(volume, L"\\\\?\\Volume{", 11) != 0)
        return false;
    wchar_t* const end = wcschr(volume + 11, L'\\');  // the volume's root ends at the first separator after its name
    if (end == nullptr)
        return false;
    end[1] = L'\0';
    return true;
}

// The volume of a path and the size of that volume's sectors as the file system uses them: the logical size, 512 on
// most drives, 4,096 on a drive with native 4 KB sectors. The volume was found from the path as written, and for a
// drive letter made with subst that gave no sector size, so Windows refused the write. The folder that receives the
// file is asked first, and the path as written is the fallback when that fails.
uint32_t File::volumeSectorSize(const char* fName)
{
    const auto name = toWide(windowsPath(fName));
    wchar_t full[MAX_PATH * 4];
    wchar_t volume[MAX_PATH * 4];
    wchar_t* filePart = nullptr;
    const DWORD len = GetFullPathNameW(name.data(), MAX_PATH * 4, full, &filePart);
    if (len == 0 || len >= MAX_PATH * 4)
        return 0;
    bool found = false;
    if (filePart != nullptr && filePart > full)
    {
        const wchar_t keep = *filePart;
        *filePart = L'\0';  // the folder, with its trailing separator
        found = realVolumeOfFolder(full, volume, MAX_PATH * 4);
        *filePart = keep;
    }
    if (!found && !GetVolumePathNameW(full, volume, MAX_PATH * 4))
        return 0;
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    if (!GetDiskFreeSpaceW(volume, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
        return 0;
    return bytesPerSector;
}
