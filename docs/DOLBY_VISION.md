# Dolby Vision

tsMuxeR can store a dual layer Dolby Vision disc in a single Matroska file and build the disc again
from that file. This page explains what the options do and what is and is not preserved.

Updated through 2.18.

## What a dual layer Dolby Vision disc looks like

Such a disc puts the picture on **two** video streams:

- a **base layer**, the full resolution picture, which is ordinary HDR10
- an **enhancement layer**, a quarter resolution stream carrying the Dolby Vision data, together
  with one RPU per picture

Both are needed. A player that understands Dolby Vision reads them together; a player that does not
shows the base layer as HDR10.

Matroska has no place for a second video stream of that kind. A file with two video tracks is not
what a player expects, and no player engages Dolby Vision from it. So tsMuxeR carries the whole
thing in **one** track: the base layer, the enhancement layer wrapped so that a decoder which knows
nothing about Dolby Vision skips it, and the RPU. Nothing is re-encoded and nothing is re-ordered.

## Storing a disc as Matroska

Add the disc or its two video streams, choose **MKV muxing**, and mux. The two video streams are
recognised as the two layers of one picture and are merged automatically.

## Building the disc again

Open the Matroska file. A merged Dolby Vision track is listed as **two rows**, one marked
`(base layer)` and one `(enhancement layer)`. Select both, choose a Blu-ray output, and mux. The
layers are separated again and land on the right streams.

From the command line the same thing is written as two lines with `subTrack=`:

```
V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=1, fps=23.976
V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=2, fps=23.976
```

`subTrack=1` is the base layer and `subTrack=2` is the enhancement layer.

## The Dolby Vision profile

A dual layer disc is **profile 7**. Many devices do not accept profile 7 from a file, even though
they play the same disc happily. **Profile 8.1** is a single layer form that far more devices
accept, and which falls back to HDR10 on anything that does not.

When the source is a dual layer Dolby Vision disc and the output is Matroska, a **Dolby Vision**
selector appears beside the output format:

| choice | what it does |
|---|---|
| Profile 7, as on the disc | carries the disc unchanged. This is the default. |
| Profile 8.1, plays on more devices | converts the metadata so the file plays as single layer Dolby Vision |

On the command line this is `--dv-profile=7` or `--dv-profile=8.1`.

### Profile 8.1 does not make the file smaller

This is the part that surprises people. The usual way to convert a disc to profile 8.1 throws the
enhancement layer away, and that is where the space saving comes from.

tsMuxeR does not throw it away. The enhancement layer still travels inside the track, and the
disc's **own** Dolby Vision metadata is attached beside it, so the original disc can be built again
exactly. A converted file is therefore about the same size as a profile 7 one, and can be slightly
larger.

If you want a small profile 8.1 file and do not care about rebuilding the disc, this is not the
tool for that.

### Why the original metadata has to be kept

Converting profile 7 metadata to 8.1 is **many to one**: different originals produce the same
result. Measured on a pressed disc, two thirds of the converted metadata became indistinguishable,
with as many as 91 different originals landing on one result. Nothing can undo that afterwards, at
any quality of implementation.

So the originals are kept verbatim rather than reconstructed. That is what makes the round trip
possible, and it is why the file does not shrink.

### What profile 8.1 needs

Profile 8.1 uses the **libdovi** library, which is not part of tsMuxeR.

| platform | what to do |
|---|---|
| Windows 64-bit | put `dovi.dll` beside `tsMuxeR.exe`. A prebuilt one is published. |
| Linux, macOS | build libdovi from source, or install a package if your distribution has one |
| Windows 32-bit | not available. There is no 32-bit build of the library. |

**Where to get it.** The library is published on the [dovi_tool releases page](https://github.com/quietvoid/dovi_tool/releases), as
`libdovi-<version>-x86_64-pc-windows-msvc.zip`. That archive holds one file, `dovi.dll`, and that
is the file to place beside `tsMuxeR.exe`.

Do not take `dovi_tool-<version>-x86_64-pc-windows-msvc.zip` from the same page. That is the
command line tool and tsMuxeR cannot use it. Both sit side by side and the tool has the more
obvious name, which is the usual mistake.

The release is numbered after dovi_tool, so the library inside carries a different number:
`libdovi-3.4.0` is attached to release `2.3.3`. Searching the page for the library version will
find nothing. Any libdovi from 3.3.1 onward carries the four entry points tsMuxeR asks for.

If the library is missing, `--dv-profile=8.1` is refused **before** muxing starts, with a message
saying so, rather than failing part way through a feature.

## What is preserved

Measured on whole titles rather than short clips, because a clip cannot exercise the end of a
stream or a join between segments, and both turned out to matter:

- a whole feature, 73 GB, both layers: the rebuilt streams are **byte for byte identical** to the
  disc, 59.33 GB of base layer and 4.25 GB of enhancement layer with nothing left over on either
  side, and all 166,928 of the disc's original metadata records restored. Through profile 7 and
  through profile 8.1 alike, so the disc comes back exactly while its metadata is converted and
  put back
- a whole **seamless branching** title, 23 clips joined at 22 points: the profile 7 rebuild and the
  profile 8.1 rebuild are byte for byte identical on both layers

In plain terms: **the disc that comes out is the disc that went in.**

### How the start code framing is kept

A disc frames its start codes one way or the other, three bytes or four. Both are legal and decode
identically, but a given disc is consistent about it, and Matroska stores this video length prefixed
and holds no start codes at all. A disc rebuilt from Matroska therefore used to come out four byte
whatever the source did, so a disc using the shorter form never came back byte for byte, however
correct everything else was.

The source's own framing now travels with the file and is reproduced. It is recorded per NAL type,
because that is what discs actually do and a blanket three byte rule would not be conformant. A NAL
type framed both ways in one source records nothing rather than guessing, and a source that used
four bytes throughout records nothing either, because that is what tsMuxeR writes anyway. So a file
with nothing unusual to say carries nothing extra, and the worst case is the old behaviour rather
than a wrong one. None of this is specific to Dolby Vision: an ordinary single layer disc taken to
Matroska and authored back keeps its framing the same way.

### The rest of the disc

The statement above is about the video. Audio, subtitles and the disc structure are authored fresh,
as they are for any mux, and they were measured rather than assumed. The same disc authored two ways,
straight from its layers and by way of a Matroska file:

| | result |
| --- | --- |
| lossless audio | identical, byte for byte |
| the AC-3 compatibility core beside it | one frame short, see below |
| subtitles | identical, byte for byte |
| the clip information file, including its seek map | identical, byte for byte |
| the playlist, the index and the movie object | identical, byte for byte |
| the program map and the clock reference | identical in every field |
| the transport layout | identical apart from the last moment of a cut |

**The one blemish:** authoring a disc from a Matroska file loses the final frame of the AC-3
compatibility core, 32 ms of the fallback stream that exists for players which cannot decode the
lossless one. The lossless stream itself is complete and identical. This is not new and not specific
to Dolby Vision; it is the point where the two audio streams are braided back onto one stream, and it
happens because the last core frame has no lossless frame left to travel beside.

### Refusals rather than guesses

A file whose preserved metadata does not match what it claims is **refused**, before anything is
written, rather than producing a disc that looks right and is wrong. That covers a file that has
been cut, re-muxed, or had its attachments stripped by another tool.

If a file was written by tsMuxeR and then copied by other software that dropped the attachments,
the Dolby Vision data needed to rebuild the disc is gone. Keep the original.

## A single layer Dolby Vision file

A file that is already single layer, profile 5 or profile 8, has no enhancement layer to separate.
Asking for a split is refused with an explanation rather than producing an empty second track.
Mux it as one track instead.
