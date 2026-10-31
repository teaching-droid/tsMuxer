// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include <windows.h>

#include "../directory.h"
#include "../directory_priv.h"

#include "../discImage.h"
#include "../file.h"

using namespace std;

char getDirSeparator() { return '\\'; }

// Windows accepts a forward slash everywhere it accepts a backslash, and the other two path helpers
// in this project, extractFilePath and extractFileName, both take either. This one took only the
// backslash, so a path written with forward slashes looked to it like a bare name with no directory.
// Naming a playlist that way was enough to break it: the stream folder is derived from the playlist
// folder, so it came out empty and the clip was looked for in the working directory.
string extractFileDir(const string& fileName)
{
    const size_t index = fileName.find_last_of("\\/");
    if (index != string::npos)
        return fileName.substr(0, index + 1);

    return "";
}

bool fileExists(const string& fileName)
{
    File f;
    return f.open(fileName.c_str(), File::ofRead | File::ofOpenExisting);
}

uint64_t getFileSize(const std::string& fileName)
{
    File f;
    if (f.open(fileName.c_str(), File::ofRead | File::ofOpenExisting))
    {
        int64_t rv;
        return f.size(&rv) ? rv : 0;
    }
    return 0;
}

// The beginning of a path that is no folder of its own and cannot be created: the \\?\ prefix, and the server and the
// share of a network path, \\server\share or \\?\UNC\server\share. Creating the folders of a path from its first one
// stopped at the first of these, so no folder could be made on a network path or a path with the prefix, which is
// what a path longer than 260 characters needs: a demux or a Blu-ray folder there failed with "Can't create output
// file", and so did a file in a folder that did not exist yet.
static bool isPathRoot(const std::string& dir)
{
    if (dir == R"(\\?)" || dir == R"(\\?\UNC)")
        return true;
    size_t rest = 0;  // where the server name starts
    if (strStartWith(dir, R"(\\?\UNC\)"))
        rest = 8;
    else if (strStartWith(dir, R"(\\)") && !strStartWith(dir, R"(\\?\)") && !strStartWith(dir, R"(\\.\)"))
        rest = 2;
    else
        return false;
    const size_t sep = dir.find('\\', rest);
    return sep == std::string::npos || dir.find('\\', sep + 1) == std::string::npos;  // \\server or \\server\share
}

bool createDir(const std::string& dirNameAsGiven, const bool createParentDirs)
{
    const std::string dirName = windowsPath(dirNameAsGiven);
    const bool ok = preCreateDir(
        [](auto&& parentDir)
        {
            return parentDir.empty() || parentDir[parentDir.size() - 1] == ':' || parentDir == "\\\\." ||
                   isPathRoot(parentDir) || parentDir == R"(\\.\)" ||                               // UNC patch prefix
                   (strStartWith(parentDir, R"(\\.\)") && parentDir[parentDir.size() - 1] == '}');  // UNC patch prefix
        },
        [](auto&& parentDir)
        {
            if (CreateDirectory(toWide(parentDir).data(), nullptr) == 0)
            {
                if (GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
            return true;
        },
        getDirSeparator(), dirName, createParentDirs);
    return ok ? CreateDirectory(toWide(dirName).data(), nullptr) != 0 : false;
}

bool deleteFile(const string& fileName)
{
    if (DeleteFile(toWide(windowsPath(fileName)).data()))
    {
        return true;
    }
    // DWORD err = GetLastError();
    return false;
}

bool findFiles(const string& path, const string& fileMask, vector<string>* fileList, const bool savePaths)
{
    if (discImage::split(path, nullptr, nullptr))
    {
        vector<discImage::Entry> entries;
        if (!discImage::list(path, &entries))
            return false;
        for (const auto& e : entries)
            if (!e.dir && discImage::matchMask(e.name, fileMask))
                fileList->push_back(savePaths ? (path + '/' + e.name) : e.name);
        return true;
    }

    WIN32_FIND_DATA fileData;  // Data structure describes the file found
    // Search handle returned by FindFirstFile

    const auto searchStr = toWide(windowsPath(path + '/' + fileMask));
    const HANDLE hSearch = FindFirstFile(searchStr.data(), &fileData);
    if (hSearch == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        if (!(fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            auto fileName = toUtf8(fileData.cFileName);
            fileList->push_back(savePaths ? (path + '/' + fileName) : fileName);
        }
    } while (FindNextFile(hSearch, &fileData));

    FindClose(hSearch);

    return true;
}

bool findDirs(const string& path, vector<string>* dirsList)
{
    if (discImage::split(path, nullptr, nullptr))
    {
        vector<discImage::Entry> entries;
        if (!discImage::list(path, &entries))
            return false;
        for (const auto& e : entries)
            if (e.dir)
                dirsList->push_back(path + e.name + "/");
        return true;
    }

    WIN32_FIND_DATA fileData;  // Data structure describes the file found
    // Search handle returned by FindFirstFile

    const auto searchStr = toWide(windowsPath(path + "*"));
    const HANDLE hSearch = FindFirstFile(searchStr.data(), &fileData);
    if (hSearch == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        // Test the directory bit with AND, not XOR: on read-only media (a mounted ISO, an optical disc)
        // directories also carry FILE_ATTRIBUTE_READONLY/ARCHIVE, so the old XOR check skipped every
        // directory and the recursive scan found nothing.
        if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            auto dirName = toUtf8(fileData.cFileName);

            if ("." != dirName && ".." != dirName)
                dirsList->push_back(path + dirName + "/");
        }

    } while (FindNextFile(hSearch, &fileData));

    FindClose(hSearch);

    return true;
}

bool findFilesRecursive(const string& path, const string& mask, vector<string>* const fileList)
{
    recurseDirectorySearch(findFiles, findDirs, path, mask, fileList);
    return true;
}
