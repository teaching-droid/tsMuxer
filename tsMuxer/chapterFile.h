// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef CHAPTER_FILE_H_
#define CHAPTER_FILE_H_

#include <string>
#include <vector>

#include "avPacket.h"

// Chapter lists in the two formats other tools use.
//
// A list typed into a meta file or into the window carries times and nothing else, so a remux could
// never be given names. These two formats carry both, which is why every other tool reads and
// writes them.
//
// The format is told apart by looking at the text rather than at the extension, because a .txt with
// XML in it is a thing that happens.
//
//   the simple text format, which eac3to writes and MKVToolNix calls OGM:
//       CHAPTER01=00:00:00.000
//       CHAPTER01NAME=Opening
//
//   Matroska XML, which mkvextract chapters writes:
//       <Chapters><EditionEntry><ChapterAtom>
//         <ChapterTimeStart>00:00:00.000000000</ChapterTimeStart>
//         <ChapterDisplay><ChapterString>Opening</ChapterString>
//                         <ChapterLanguage>eng</ChapterLanguage></ChapterDisplay>
//       </ChapterAtom></EditionEntry></Chapters>
//
// Returns the chapters in the order the file lists them. Throws if the file cannot be read or holds
// nothing that looks like a chapter, because silently muxing without the chapters someone asked for
// is worse than stopping.
std::vector<AVChapter> readChapterFile(const std::string& fileName);

// Seconds from one chapter time, such as 01:02:03.456 (the hours and the minutes may be left out; an empty
// time is 0, as it always was). A time that is not written in digits, is negative, or is past 100,000
// hours is refused with a message that starts with where (a file and line, or an option name).
double chapterTimeToSeconds(const std::string& text, const std::string& where);

// Writes the Matroska XML form. Chosen for the written side because it is the one that carries names
// and languages and the one other tools read back without being told anything.
bool writeChapterXml(const std::string& fileName, const std::vector<AVChapter>& chapters);

#endif  // CHAPTER_FILE_H_
