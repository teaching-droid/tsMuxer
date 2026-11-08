// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef WAVPACK_FILE_H_
#define WAVPACK_FILE_H_

#include <string>

// A WavPack file (.wv) is read as the Matroska file that holds the same track, as a Matroska A_WAVPACK4 track is stored
// (the block format of WavPack 4 and 5; the Matroska codec mapping of A_WAVPACK4): each frame, the blocks of one
// block_index from the one marked initial to the one marked final, is a block at the time its block_index gives, its
// block headers cut to what Matroska keeps (block_samples once, then flags, crc and, where the frame has more than one
// block, the size of each). So everything that reads WavPack from a Matroska file reads it: the listing, a Matroska
// output and a join (wavpackStreamReader.h).
//
// The correction file (.wvc) of a hybrid file is read when it is beside it. DSD is refused.
//
// For a file of such a name: reads it and puts the Matroska file in its place (discImage::addMemoryFile), once.
// Throws with the reason when it holds nothing tsMuxeR can read. False for any other name.
bool prepareWavpackFile(const std::string& fileName);

// The extension is that of a WavPack file (in capitals)
bool isWavpackFileExt(const std::string& upperExt);

#endif
