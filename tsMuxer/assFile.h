#ifndef ASS_FILE_H_
#define ASS_FILE_H_

#include <string>

// An ASS or SSA subtitle script as a file of its own (.ass, .ssa) is read as the Matroska file that holds it as its one
// track (track 1, S_TEXT/ASS or S_TEXT/SSA), as a Matroska ASS track is stored: the script's header as the track's
// CodecPrivate, each Dialogue line as a block of its time and duration. Everything that reads a Matroska ASS track
// reads it so: the listing, a Matroska output and a demux, which keep it as it is, and a disc, for which libass draws
// it.
//
// For a file named .ass or .ssa: reads it and puts the Matroska file in its place (discImage::addMemoryFile), once.
// Throws with the reason when it is no script tsMuxeR can read. False for any other name.
bool prepareAssFile(const std::string& fileName);

// The extension is that of a script file (ASS or SSA, in capitals)
bool isAssFileExt(const std::string& upperExt);

#endif
