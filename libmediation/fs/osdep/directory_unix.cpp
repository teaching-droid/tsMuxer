// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#if !defined(_WIN32)

#include "../directory.h"
#include "../directory_priv.h"
#include "../discImage.h"
#include "../file.h"
#include "../nameAlias.h"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <set>
#include <utility>

using namespace std;

char getDirSeparator() { return '/'; }

string extractFileDir(const string& fileName)
{
    size_t index = fileName.find_last_of('/');
    if (index != string::npos)
        return fileName.substr(0, index + 1);
    return "";
}

// A path inside a disc image is asked of the image (discImage.h); stat knows only the image file.
bool fileExists(const string& name)
{
    const std::string fileName = nameAlias::resolve(name);  // a second name of a file (nameAlias.h)
    if (discImage::isInside(fileName))
        return discImage::exists(fileName);
    struct stat buf;
    return stat(fileName.c_str(), &buf) == 0;
}

uint64_t getFileSize(const std::string& name)
{
    const std::string fileName = nameAlias::resolve(name);  // a second name of a file (nameAlias.h)
    if (discImage::isInside(fileName))
    {
        File f;
        int64_t size = 0;
        return f.open(fileName.c_str(), File::ofRead | File::ofOpenExisting) && f.size(&size)
                   ? static_cast<uint64_t>(size)
                   : 0;
    }
    struct stat fileStat;
    auto res = stat(fileName.c_str(), &fileStat) == 0;
    return res ? static_cast<uint64_t>(fileStat.st_size) : 0;
}

bool createDir(const std::string& dirName, bool createParentDirs)
{
    auto ok = preCreateDir([](auto) { return false; },
                           [](auto&& parentDir)
                           {
                               if (mkdir(parentDir.c_str(), S_IREAD | S_IWRITE | S_IEXEC) == -1)
                               {
                                   if (errno != EEXIST)
                                       return false;
                               }
                               return true;
                           },
                           getDirSeparator(), dirName, createParentDirs);

    return ok ? mkdir(dirName.c_str(), S_IREAD | S_IWRITE | S_IEXEC) == 0 : false;
}

bool deleteFile(const string& fileName) { return unlink(fileName.c_str()) == 0; }

// What an entry of a directory listing is. A symbolic link reports itself as a link, and some file systems report
// every entry as unknown, so both are asked about what they point at. Only a regular file and a directory counted
// before, which made a font file or a font folder that is a link invisible, and a link inside a disc folder too.
static bool isEntryOfKind(const string& fullName, const dirent* entry, const bool wantDir)
{
    if (entry->d_type == DT_REG)
        return !wantDir;
    if (entry->d_type == DT_DIR)
        return wantDir;
    if (entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN)
        return false;
    struct stat st
    {
    };
    if (stat(fullName.c_str(), &st) != 0)
        return false;  // a dangling link
    return wantDir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}

bool findFiles(const string& path, const string& fileMask, vector<string>* fileList, bool savePaths)
{
    // Inside a disc image the names match as on Windows, not by case: a disc is made to be read on any system.
    if (discImage::split(path, nullptr, nullptr))
    {
        vector<discImage::Entry> entries;
        if (!discImage::list(path, &entries))
            return false;
        for (const auto& e : entries)
            if (!e.dir && discImage::matchMask(e.name, fileMask))
                fileList->push_back(savePaths ? path + e.name : e.name);
        return true;
    }

    dirent** namelist;

    int n = scandir(path.c_str(), &namelist, 0, 0);  // alphasort);

    if (n < 0)
    {
        return false;
    }
    else
    {
        while (n--)
        {
            if (isEntryOfKind(path + namelist[n]->d_name, namelist[n], false))
            {
                string fileName(namelist[n]->d_name);

                if (0 == fnmatch(fileMask.c_str(), fileName.c_str(), 0))
                    fileList->push_back(savePaths ? path + fileName : fileName);
            }
            free(namelist[n]);
        }
        free(namelist);
    }

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

    dirent** namelist;

    int n = scandir(path.c_str(), &namelist, 0, 0);  // alphasort);

    if (n < 0)
    {
        return false;
    }
    else
    {
        while (n--)
        {
            if (isEntryOfKind(path + namelist[n]->d_name, namelist[n], true))
            {
                string dirName(namelist[n]->d_name);

                // we save only normal ones
                if ("." != dirName && ".." != dirName)
                    dirsList->push_back(path + dirName + "/");
            }
            free(namelist[n]);
        }
        free(namelist);
    }

    return true;
}

// Each directory is entered once, by its device and inode: a link to a directory above it would otherwise send the
// search round in a circle now that links are followed.
static void findFilesOnce(const string& path, const string& mask, vector<string>* const fileList,
                          std::set<std::pair<dev_t, ino_t>>& visited)
{
    struct stat st
    {
    };
    if (stat(path.c_str(), &st) != 0 || !visited.insert({st.st_dev, st.st_ino}).second)
        return;
    findFiles(path, mask, fileList, true);
    vector<string> dirList;
    findDirs(path, &dirList);
    for (const auto& dir : dirList) findFilesOnce(dir, mask, fileList, visited);
}

bool findFilesRecursive(const string& path, const string& mask, vector<string>* const fileList)
{
    if (discImage::split(path, nullptr, nullptr))
    {
        recurseDirectorySearch(findFiles, findDirs, path, mask, fileList);
        return true;
    }
    std::set<std::pair<dev_t, ino_t>> visited;
    findFilesOnce(path, mask, fileList, visited);
    return true;
}

#endif