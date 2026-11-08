// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef directory_h
#define directory_h

#include <string>

#include "types/types.h"

bool createDir(const std::string& dirName, bool createParentDirs = false);

std::string extractFileDir(const std::string& fileName);

char getDirSeparator();

bool fileExists(const std::string& fileName);

uint64_t getFileSize(const std::string& fileName);

/** remove file. cerr contains error code */
bool deleteFile(const std::string& fileName);

bool findFilesRecursive(const std::string& path, const std::string& mask, std::vector<std::string>* fileList);

// The files of one folder that match the mask, its subfolders left out; with their folder in front when savePaths.
bool findFiles(const std::string& path, const std::string& mask, std::vector<std::string>* fileList, bool savePaths);

#endif  // directory_h
