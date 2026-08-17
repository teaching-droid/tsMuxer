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

If the library is missing, `--dv-profile=8.1` is refused **before** muxing starts, with a message
saying so, rather than failing part way through a feature.

## What is preserved

Measured on whole titles rather than short clips, because a clip cannot exercise the end of a
stream or a join between segments, and both turned out to matter:

- a whole feature, both layers: every one of 2,064,653 units identical, and every one of the
  disc's 166,928 original metadata records restored
- a whole segment, both layers: every unit identical, and byte for byte identical over 7 GB
- a whole **seamless branching** title, 23 clips joined at 22 points: the profile 7 rebuild and the
  profile 8.1 rebuild are byte for byte identical on both layers

In plain terms: **every picture and every piece of Dolby Vision metadata comes back exactly.**

### One honest limit

A rebuilt stream is byte for byte identical to the source wherever the disc uses the same start
code form tsMuxeR writes, which is most discs but not all. Where a disc differs, the difference is
the start code length only, which is legal either way and decodes identically; the pictures and the
metadata are unaffected. This is not specific to Dolby Vision and applies to profile 7 in the same
way.

Audio, subtitles and the disc structure are authored fresh, as they are for any mux. The statement
above is about the video.

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
