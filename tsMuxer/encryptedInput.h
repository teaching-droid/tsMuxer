// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef ENCRYPTED_INPUT_H_
#define ENCRYPTED_INPUT_H_

#include <string>

// A source file that is still encrypted was read as if it were not: a Blu-ray .m2ts gave "Invalid
// tsPacket->getHeaderSize", or its listing looked whole when the first part of it was clear, and a DVD VOB was listed
// with its video and "Can't detect stream type" for its sound, and muxed into noise. Units of the file are read from
// across it, and the file is refused when most of them are encrypted, with a warning when some are. Only the marks a
// file carries in the clear are looked at; nothing is decrypted.
//
// A .m2ts (BDAV, 192-byte packets) is read in aligned units of 6144 bytes, 32 packets: in an encrypted unit the first
// 16 bytes stay readable, so its first TS packet starts with its sync byte while the later ones do not, and the copy
// permission bits (the top 2 bits of the first packet's 4-byte header) are set. A clear unit that is damaged has its
// bits clear, and is not counted. A DVD VOB, MPEG program stream or EVO is read in sectors of 2048 bytes: an encrypted
// sector keeps its pack and PES headers readable, and PES_scrambling_control (bits 5-4 of PES byte 6) of its video
// and audio packets is set; a decrypted copy has them clear.
//
// Each file is tested once; m2ts: the file is read as a .m2ts, otherwise as a program stream.
void checkEncryptedInput(const std::string& fileName, bool m2ts);

#endif  // ENCRYPTED_INPUT_H_
