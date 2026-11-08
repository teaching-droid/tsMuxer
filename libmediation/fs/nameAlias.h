// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef LIBMEDIATION_NAME_ALIAS_H
#define LIBMEDIATION_NAME_ALIAS_H

#include <string>

// ** A FILE CAN BE OPENED UNDER A SECOND NAME. ** tsMuxeR chooses the reader of a source by the extension of its name,
// in many places. A source whose content is another container than its name says, as a transport stream that a
// recorder saved as .mpg, is given a second name with the right extension (metaDemuxer.cpp, readAsName), and every
// open, size and existence check of that name is done on the file itself.
namespace nameAlias
{
void add(const std::string& alias, const std::string& real);
// The file an alias stands for, or the name itself when it is no alias
std::string resolve(const std::string& name);
// A text with each second name in it replaced by the name of its file, for what tsMuxeR prints: the second name is no
// file a user can find
std::string shown(std::string text);
}  // namespace nameAlias

#endif
