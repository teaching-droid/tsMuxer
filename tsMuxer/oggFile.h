#ifndef OGG_FILE_H_
#define OGG_FILE_H_

#include <string>

// An Ogg file (.ogg, .oga, .opus) of Opus or Vorbis sound is read as the Matroska file that holds the same tracks, as
// a Matroska Opus or Vorbis track is stored (RFC 7845 for Opus in Ogg, the Vorbis I specification for Vorbis in Ogg,
// RFC 3533 for the pages): each logical stream a track, numbered from 1 in the order of the file, its header packets
// as the track's CodecPrivate, each audio packet a block at the time its granule positions give. The pre-skip of Opus
// is its CodecDelay, and the samples its last page cuts off its last packet the DiscardPadding of the last block. So
// everything that reads Opus and Vorbis from a Matroska file reads it: the listing, a Matroska output and a demux.
//
// For a file of such a name: reads it and puts the Matroska file in its place (discImage::addMemoryFile), once.
// Throws with the reason when it holds nothing tsMuxeR can read. False for any other name.
bool prepareOggFile(const std::string& fileName);

// The extension is that of an Ogg file (in capitals)
bool isOggFileExt(const std::string& upperExt);

// A source read as a Matroska file built in memory: an ASS or SSA script (assFile.h) or an Ogg file
bool isMemorySourceExt(const std::string& upperExt);
bool prepareMemorySource(const std::string& fileName);

#endif
