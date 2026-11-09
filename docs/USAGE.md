# tsMuxer : Usage Instructions

## GUI

The simplest thing to do is to use the tsMuxerGUI. A screenshot of that can be seen below:

![tsMuxerGUI_Screenshot](https://user-images.githubusercontent.com/122434/148913872-b7af9caa-c2ca-4892-8853-06fd6329a15a.png)

## Multi-layer disc authoring (this fork)

This fork adds a "BDMV folder to ISO" tab and the `--bdmv-to-iso`, `--layer-break-guard`,
`--layer-break-lbn`, `--disc-size`, `--allow-oversize` and `--inner-only` options for authoring dual-layer and
BD-R XL discs. See [DISC_AUTHORING.md](DISC_AUTHORING.md) (also available in German as
[DISC_AUTHORING_DE.md](DISC_AUTHORING_DE.md) and in Japanese as [DISC_AUTHORING_JA.md](DISC_AUTHORING_JA.md)) for
the details and the layer-break calculator.

An ISO image whose data crosses a layer break gets a layer-break guard by default: 288 MB after the break and
18 MB before it (306 MB) are left empty, as a burnt dual-layer disc often reads badly at the layer change and a
player then stalls there. The break is the one of `--layer-break-lbn`, else of `--disc-size` (`--disc-capacity` for
`--bdmv-to-iso`), else that of a BD-R DL. An image that would not fit the disc with the guard is written without it,
with a warning. `--no-layer-break-guard` leaves the guard out; `--layer-break-guard=<MB>` sets its size (the zone
before the break is a sixteenth of it, at least 4 MB, unless `--layer-break-guard-before` gives it). Folder, TS and
MKV output, images that end before the break and `--inner-only` are not affected.

A step-by-step guide to the "BDMV folder to ISO" tab, with screenshots of every stage, is in
[BDMV_TO_ISO.md](BDMV_TO_ISO.md) (also available in German as
[BDMV_TO_ISO_DE.md](BDMV_TO_ISO_DE.md) and in Japanese as
[BDMV_TO_ISO_JA.md](BDMV_TO_ISO_JA.md)).

For an illustrated explanation of how the layer-break guard protects the movie, and why a block
of zeros in the middle of the image never damages the video, see
[The layer-break buffer](LAYER_BREAK_BUFFER.md) (also available in German as
[LAYER_BREAK_BUFFER_DE.md](LAYER_BREAK_BUFFER_DE.md) and in Japanese as
[LAYER_BREAK_BUFFER_JA.md](LAYER_BREAK_BUFFER_JA.md)).

### Editing the Meta File in the GUI

The tsMuxerGUI displays an auto-generated meta file in the "Meta file" section. This preview shows the exact content that will be used when you start muxing. 

You can now **manually edit this meta file** directly in the GUI to use advanced options not available through the visual interface. This is useful when you need to:
- Combine options that the GUI doesn't provide
- Apply specialized parameters for particular encoding scenarios
- Test different muxing configurations without creating separate project files

**How to edit:**
1. Click in the "Meta file" text area to position your cursor
2. Edit the meta file content directly (add/remove/modify lines)
3. The GUI will remember your changes and use the edited version when you click "Start muxing"

**Reverting changes:**
- If you've manually edited the meta file and want to go back to the auto-generated version, click the **"Reset meta to auto-generated"** button
- This will discard your manual edits and regenerate the meta file based on current GUI settings

**Saving edited meta files:**
- Use the **"Save meta file"** button to save your edited meta file as a `.meta` project file
- This allows you to reuse complex configurations later
- When you load the saved `.meta` file from the command line, it will use your custom settings

**Note:** Once you manually edit the meta file, subsequent GUI changes will no longer automatically update it. You can reset it at any time using the "Reset meta to auto-generated" button, or make additional manual edits as needed.

### TrueHD + AC-3 Compatibility Core Merge in the GUI

When working with Dolby TrueHD and AC-3 tracks in Matroska files, the GUI provides options to automatically merge them into a Blu-ray compatible format.

**GUI usage:**

Select the **TRUE-HD** track, then set:
- `track=` to the TrueHD Matroska track number
- **Merge AC-3 track** to the AC-3 Matroska track number (when the AC-3 exists as a track in the MKV), or
- **Merge AC-3 file** to a standalone `.ac3` file (when the MKV does not contain an AC-3 track)

The GUI will emit either `merge-ac3-track=<n>` or `merge-ac3-file="path"` into the meta preview.

On a disc output the window also asks about a TrueHD track that has no core when you press **Start muxing**. It offers the AC-3 tracks of the same file, a new core, or no core. A new core needs an ffmpeg of your own that reads TrueHD and writes AC-3, beside tsMuxeR, on the PATH, or chosen with **Choose ffmpeg...** in the question. tsMuxeR does not include ffmpeg. The command line is not changed: use `merge-ac3-file=` there.

**If the MKV has no AC-3 track:**

Create an AC-3 compatibility stream from the TrueHD audio with FFmpeg (choose the correct audio index for `0:a:N`):

```
ffmpeg -i input.mkv -map 0:a:0 -c:a ac3 -b:a 640k -ac 6 compat.ac3
```

Then either:
- use **Merge AC-3 file** / `merge-ac3-file="compat.ac3"` directly, or
- **remux** `compat.ac3` back into the MKV as a separate audio track (e.g. with `mkvmerge`) and use **Merge AC-3 track** / `merge-ac3-track` with the new AC-3 track number.

### Preview in the GUI

Set a player for previews on the General tab, or leave it empty for the system's default player. `{file}` in its arguments stands for what is played; without it, it comes last. A player can also be given for Blu-ray titles and one for DVD titles; left empty, they use the player above. Preview in Choose a title plays the title chosen, in its angle; a click on the clip of an angle under it plays that clip; Play in the context menu of the input list plays an item. A clip of a disc folder, or a file, is opened as it is. For a title, or a clip inside an image, tsMuxeR first makes a 30 second preview (from where the angles differ, or a tenth of the way in, at most 2 minutes) into a temporary folder, which is removed when the window closes. mpv opens a Blu-ray title directly in its angle (mpv 0.41 or later), VLC a DVD title in its first angle. A 3D title has no angle choice; its clip played directly shows the 2D base view.

## Command Line

Alternatively you can use tsMuxer via the command-line. 

Examples:
```
    tsMuxeR <media file name>
    tsMuxeR <meta file name> <out file/dir name>
```

tsMuxeR can be run in track detection mode or muxing mode. If tsMuxeR is run with only one argument, then the program displays track information required to construct a meta file. When running with two arguments, tsMuxeR starts the muxing or demuxing process.

A file inside a disc image (.iso) is read without mounting the image: give the path of the file after the name of the image, for example `tsMuxeR film.iso/BDMV/PLAYLIST/00011.mpls`, and the same in a meta file. Blu-ray, UHD Blu-ray and DVD images are read, and the stream files are read in place, without a copy. The names inside an image are found without regard to case.

A Blu-ray disc folder or image given alone lists its titles instead of tracks: `tsMuxeR film.iso` or `tsMuxeR D:/discs/film`. Each title is shown with its playlist, length, chapters, clips and audio languages, the main film first. Each title also says what its picture is: SDR, HDR10, HDR10+, Dolby Vision with its profile where it is known, or a combination such as "HDR10, Dolby Vision profile 7". The playlist gives SDR, HDR10 and the Dolby Vision enhancement layer of a disc (profile 7). HDR10+ and single layer Dolby Vision are read from the start of the main clip of each main title, when it is there; a disc of control files alone does not show them. A DVD title is SDR. A disc of episodes or of several films has more than one main title: a title that plays footage of its own for at least 75 % of the main film's footage and at least 15 minutes is one too. The main film comes first and the other main titles follow in the order of the disc, each with its versions after it; where they are all of one length (within 3 minutes), as episodes are, they are all in the order of the disc. `title=main` takes title 1 and says how many main titles the disc has. The main film is a best guess: the title that plays the most footage. A disc with several main titles says so, and `title=main` takes the first. It can miss on some discs, as with an HDR and an SDR version of one length, so check the list before a long mux. Any title can be chosen by its number. Playlists that are no title are listed under "Left out" with the reason (one clip repeated, many short pieces, a copy of another playlist, the same clips in another order, shorter than 2 minutes). Options before the name: `--all-playlists` lists every playlist as a title, `--min-title-length=<seconds>` sets the length a title needs (120 by default). Give the playlist of a title as the source to list its tracks, or name the disc on a meta line with `title=main` or `title=<number>`:
```
MUXOPT --blu-ray
V_MPEG4/ISO/AVC, "film.iso", title=main, track=4113
A_AC3, "film.iso", title=main, track=4352
```

A DVD-Video folder (the folder holding VIDEO_TS, or VIDEO_TS itself) or image is listed the same way. On a meta line, `title=main` or `title=<number>` reads the title's cells in play order from its VOB files. Where the timestamps start again, the parts are joined as a "+" join is. A title with several angles is read in its first angle, or in the angle `angle=<n>` gives. Audio tracks take the disc's language unless the line gives `lang=`, and a Matroska output gets the title's chapters. A disc output takes chapters only from `--custom-chapters`: `tsMuxeR --show-title=main dvd.iso` lists the title's tracks, its audio languages and its chapters as a `--custom-chapters=` value to copy into MUXOPT. On a Blu-ray it lists the tracks of the title's playlist. Track numbers on a DVD: video 224; audio 128 and up for AC-3, 136 for DTS, 160 for LPCM and 192 for MPEG; subtitles 32 and up. `dvd-title=<number>` names a DVD title by the disc's own number (the "DVD title" column of the listing) instead of its place in the listing: it stays the same in a saved meta file, and it also names the titles the listing leaves out. DVD subtitles are read as `S_HDMV/PGS`: their pictures are drawn as PGS with the colours of the disc's IFO file and placed on the time line of the video, also across the parts of a title; they take the disc's language as the audio does. A VOB file given on its own takes the colours from the IFO file of its title set next to it, and without one they are drawn in grey, with a warning. A title whose video is still encrypted is refused.
```
MUXOPT --vbr
V_MPEG-2, "dvd.iso", title=main, track=224
A_AC3, "dvd.iso", title=main, track=128
```

A title of several angles is listed with the number of its angles and, under it, where they differ: the times, and on a Blu-ray the clip each angle plays there. Take the angle with `angle=<n>` on the meta line; the window's Choose a title shows a row for each angle, with these lines under it.
```
Title 1  00011.mpls    1:52:14   24 chapters     3 clips  eng deu spa  2 angles  SDR  (main film)
    angles differ at 0:00:00-0:02:10, 1:49:30-1:52:14
    angle 1: 00021.m2ts, 00023.m2ts
    angle 2: 00022.m2ts, 00024.m2ts
```

In the window, a click on such a clip plays it in the player for previews (General tab).

A version of a main title that plays clips of its own, another cut, gets two lines under it: where in its own time it differs, and the clips it plays there (several in a row joined with +). A version with the same clips and other audio or subtitles gets none.

```
Title 2  00012.mpls    2:21:47   64 chapters    31 clips  eng deu fra  SDR  (version of title 1, 2 of 31 clips differ)
    differs from title 1 at 0:31:12-0:31:58, 1:47:40-1:48:20
    its own clips: 00031.m2ts, 00035.m2ts
```

In the window, such a version has an arrow: it opens a row for each place, and a click on the clip plays it in the player for previews.

A demux writes DVD subtitles as `name.idx` and `name.sub`. Such a pair is read as a source when the .idx file is beside the .sub file: `S_HDMV/PGS, "name.sub", track=32`. The track number is 32 plus the `index:` of the .idx file. Such pairs are not joined: give one pair on each line.

A Blu-ray plays text subtitles (TextST) from clips of their own. The listing of a playlist gives each one with `Stream file:` and `Stream delay:`. The meta line names that clip, with the delay as `timeshift=`:
`S_TEXT/UTF8, "BDMV/STREAM/00013.m2ts", track=6144, timeshift=-10000ms, lang=fra`
The window does this by itself.

A folder that holds several disc folders or images, such as a box set copied disc by disc, given alone names them: `tsMuxeR D:/discs/box` prints each Blu-ray and DVD disc folder and image in it, to be listed with its own path.

Supported output formats:
* **TS** - MPEG Transport Stream
* **M2TS** - Blu-ray MPEG-2 Transport Stream
* **MKV/MKA** - Matroska Video/Audio container
* **WebM** - the Matroska form for VP8, VP9 and AV1 video, Opus and Vorbis audio, and WebVTT subtitles
* **ISO** - Blu-ray disc image
* **Blu-ray folder structure** - BDMV directory layout
* **AVCHD folder structure** - with `--avchd` in the meta file
* **Demux** - each track as a file of its own, with `--demux`

The output format is determined by the file extension of the output file name (e.g. `.ts`, `.m2ts`, `.mts`, `.m2t`, `.mkv`, `.mka`, `.mks`, `.mk3d`, `.webm`, `.iso`).

The output of the program is encoded in UTF-8, which means that non-ASCII characters will not show up properly in the Windows console by default. If you want to see the output properly, run `chcp 65001` before running tsMuxeR.

### Ogg files

An Ogg file (`.ogg`, `.oga`, `.opus`) of Opus or Vorbis sound is read as a source: each of its streams is a track, numbered from 1 in the order of the file, so `A_OPUS, "song.opus", track=1`; without `track=` the first one is taken. The pre-skip of Opus and the samples the last page cuts off are kept as in a Matroska file, and a LANGUAGE comment of three letters gives the track's language. Other streams in an Ogg file (Theora, FLAC, Speex) are left out with a warning, and a chained Ogg file, of several streams one after the other, is refused: split it and join the parts with `+`. Opus and Vorbis go to a Matroska output or a demux, and Opus to a TS as well; a Blu-ray disc has no form for either. A damaged page is left out with a warning; in an Opus track of one or two channels the time it held is filled with empty frames, which a decoder fills in by itself, so the sound after it stays at its time.

### Report for a problem report

A problem with a mux can often be looked into without the source file, which may be too big to send. `--report=<file>` writes a text file of the run to attach to the problem report instead:

```
tsMuxeR --report=report.txt film.meta film.ts
```

It holds the tsMuxeR version, the system, the command line, the meta file, the size and first 32 bytes of each source and its listing, every message of the run with its time in seconds, the lines tsMuxeR does not show on the screen as well (a lower-case letter marks them), what each track read, used and left out, with where the left-out bytes lie, and the files written with their sizes. Folders are left out of every path, and the names of files, titles, track names and chapter names are replaced by placeholders: a source is `<source1>.mkv`, a disc `<disc1>`, the output `<output>.ts`, a track name `<track name, 14 characters>`. A section Names gives what each placeholder keeps of its name: its length in characters, whether it is plain ASCII, whether it has spaces, the language or delay tsMuxeR takes from it, and the length of its path. The names of the files of a disc, such as `00011.mpls` or `VTS_01_1.VOB`, are kept. `--report-keep-names` keeps the names, and `--report-full-paths` keeps the folders and the names. Nothing of the sound or the pictures is in it. A run that stops with an error, by Ctrl+C or with a crash still leaves the report up to that point.

`--report-frames` adds a second file, `<file>.frames.tsv`, tab-separated for a spreadsheet: a line for each PES a transport stream mux writes, with its track, PTS and DTS, size, the output file and packet it begins in, the PCR there and how long before its DTS it was written; and for MPEG-2 video a line for each picture as the decoder's buffer (the T-STD of H.222.0) takes it out, with how full the buffer is and whether the picture is late. A film gives millions of lines, so a range of the output's time keeps the file small: `--report-frames=0:10:00-0:12:00` or `--report-frames=600-720`.

The report cannot take the place of a file of the job: a source, the meta file, the output and the report of `--add-report` are refused as its name.

In the window, "Write a report of each mux for a problem report" in the options does the same, and "Save report..." in the progress window keeps a copy when the job has ended.

The steps for the window and the command line, and what to attach, are in [Reporting a problem](REPORTING_A_PROBLEM.md).

### Sample for a problem report

When a problem needs the source itself, a part of it is enough, and an issue takes a file of 25 MB at most. `--make-sample` writes a zip with a part of the source, the report of making it and, with `--add-report`, the report of the job that went wrong:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

The part is copied byte for byte, so whatever the source does wrong the sample does too. It begins where a reader can begin and keeps what a reader needs before that:

Source | Where the sample begins | What is kept
---|---|---
TS, M2TS | the PAT before a key picture | the PAT and PMT, which come again every few tenths of a second
MKV, WebM | a cluster | the EBML header, Info, Tracks and Chapters; the title and the names of the tracks and chapters are blanked to "x", each as long as it was. Tags, attachments and the index are left out.
VOB, MPG | a pack with a sequence header | the IFO file of the title set of a VOB, beside the sample
Ogg | a page | the header pages
WavPack (.wv) | the initial block of a frame | nothing more is needed; whole frames are taken
WAV, RF64, BW64 | a sample of sound | the header, its sizes set to the sample
MP4, MOV | the start only, when the index comes first | the index; the frames past the end of the sample are not there
A video or sound stream on its own | a sequence header or a frame | nothing more is needed

`--from-start` takes the start (the default), `--from-end` the end, and `--at=<time>` the part around a time of the source, as h:mm:ss or seconds. `--at` finds the time by the clock of the source: the PCR of a transport stream, the timestamps of the clusters of a Matroska file, the SCR of a program stream, the granule positions of Ogg, the block_index of WavPack, and the rate of a WAV file or of AC-3, DTS and MPEG audio. It is refused for an MP4 file, whose index would have to be made anew, and for a video stream on its own, which holds no times. `--size=<MB>` sets the size of the sample, 1 to 24 MB, 20 when not given; the reports take their room from it so that the zip stays under 25 MB.

The names are replaced as in the report: the sample is `source1.mkv` in the zip, the source `<source1>.mkv` in the report. `--report-keep-names` keeps them, and the title and names in a Matroska file too.

A source that fits in the sample is taken whole, whatever part is asked for, and a time past the end takes the end. A file inside a disc image can be named as the source, as `film.iso/BDMV/STREAM/00001.m2ts`.

In the window, "Make a sample..." on the Input tab makes one of the file chosen there, and the progress window offers it when a job ends with an error.

## Meta file format
File MUST have the .meta extension. This file defines files you want to multiplex. The first line of a meta file contains additional parameters that apply to all tracks. In this case the first line should begin with the word MUXOPT.

### Encoding
The file should be encoded with UTF-8. However, since older versions of the GUI saved the file in the "active code page" encoding on Windows, it is used as a fallback on this platform. In the very rare event of the program not being able to open some files referenced by a meta file saved with an older GUI version, please convert it to UTF-8 manually by opening it in Notepad and selecting `ANSI` as the encoding, and then saving it via "Save As", but this time selecting `UTF-8` as the encoding.

### Syntax

The following lines form a list of tracks and their parameters.  The format is as follows: `<code name>,   <file name>,   <parameters>`. Parameters are separated with commas, with each parameter consisting of a name and a value, separated with an equals sign.
Example of META file:
```
MUXOPT --blu-ray
V_MPEG4/ISO/AVC, D:/media/test/stream.h264, fps=25
A_AC3, D:/media/test/stream.ac3, timeshift=-10000ms
```
In this example one AC3 audio stream and one H264 video stream are multiplexed into BD disc. The input file name can reference an elementary stream or a track located inside a container.

Example of META file for MKV output:
```
MUXOPT --vbr
V_MPEG4/ISO/AVC, D:/media/test/stream.h264, fps=25
A_AC3, D:/media/test/stream.ac3, timeshift=-10000ms
```
When the output file has an `.mkv`, `.mka`, `.mks`, `.mk3d` or `.webm` extension, tsMuxeR automatically uses the Matroska muxer. A `.webm` output is WebM, the subset of Matroska that web browsers play: it takes VP8, VP9 and AV1 video and Opus and Vorbis audio only, and holds no attachments. Most MUXOPT parameters related to TS/Blu-ray (such as `--blu-ray`, `--new-audio-pes`, `--no-pcr-on-video-pid`) are not applicable to MKV output and will be ignored.

Supported input containers:
* TS/M2TS/MTS
* EVO/VOB/MPG/MPEG
* MKV
* MOV/MP4/M4A/M4V
* MPLS (Blu-ray media play list file)

Names of codecs in the meta file:

Meta File Code    | Description 
---               | --- 
V_MPEGI/ISO/VVC   | H.266/VVC 
V_MPEGH/ISO/HEVC  | H.265/HEVC 
V_MPEG4/ISO/AVC   | H.264/AVC 
V_MPEG4/ISO/MVC   | H.264/MVC 
V_MS/VFW/WVC1     | VC1 
V_MPEG-2          | MPEG2 
V_AV1             | AV1 (read from MKV, WebM, MP4 and .ivf files, and from .obu files in the low overhead form or the length delimited form of Annex B; not on a disc) 
V_VP8             | VP8 (read from MKV, WebM and .ivf files; MKV output, or demuxed to an .ivf file)
V_VP9             | VP9 (read from MKV, WebM, MP4 and .ivf files; MKV output, or demuxed to an .ivf file)
V_MPEG4/ISO/ASP   | MPEG-4 Part 2 video (read from MP4 and MOV; copied into MKV output only)
V_PRORES          | Apple ProRes (read from MOV; copied into MKV output only)
A_AC3             | AC3/AC3+/TRUE-HD 
A_AAC             | AAC. AAC in LATM from a transport stream is read and written as ADTS.
A_DTS             | DTS/DTS-Express/DTS-HD 
A_MP3             | MPEG audio layer 1/2/3 
A_LPCM            | raw pcm data or PCM WAV file: 8, 16, 20, 24 or 32 bit integers, or 32 or 64 bit floating point. A disc and a TS carry 16, 20 or 24 bit, so 8 bit samples become 16 bit and 32 bit and floating point ones 24 bit there; a Matroska output and a demux keep the samples as they are. 
A_FLAC            | FLAC (MKV and demux output only)
A_OPUS            | Opus (also read from an Ogg file, .opus or .ogg; TS, MKV, and demux output only)
A_VORBIS          | Vorbis (read from MKV, WebM, MP4 and Ogg; MKV output, or demuxed to an .ogg file)
A_ALAC            | Apple Lossless (read from MKV and MP4; MKV output only)
A_WAVPACK4        | WavPack (read from MKV and from .wv files; MKV output, or demuxed to .wv). Hybrid WavPack: the correction part (from the MKV, or the .wvc file beside the .wv) is kept in a Matroska output; a demux writes the lossy part only.
S_HDMV/PGS        | Presentation graphic stream (BD subtitle format) 
S_TEXT/UTF8       | SRT subtitle format.  Encoding MUST be  UTF-8/UTF-16/UTF-32 
S_ARIBSUB         | ARIB captions of an ISDB recording. Matroska output only.

Each track may have additional parameters. Track parameters do not have dashes. If a parameter's value consists of several words, it must be enclosed in quotes.

Common additional parameters for any type of track:

Parameter         | Description 
---               | --- 
track             | track number if input file is a container.
lang              | track language: three letters, an ISO 639-2 code such as eng or deu, or a BCP 47 tag such as de, pt-BR or zh-Hant. A tag becomes its ISO 639-2 code (in the form --lang-codes asks for) on a disc, in a transport stream and in a demuxed file name; a Matroska output writes the tag as well. 
track-name        | **MKV output only.** The name of the track, shown by players in the track list. Put it in quotes when it has a comma: `track-name="Director, commentary"`.
forced, hearing-impaired, visual-impaired, text-descriptions, original, commentary | **MKV output only.** The Matroska track flags: forced subtitles, made for the hearing or the visually impaired, text descriptions of the picture, the original language, commentary. `=1` (or the name alone) sets the flag, `=0` clears it. A Matroska source's own flags are kept otherwise.

Additional parameters for audio tracks:

Parameter         | Description 
---               | --- 
timeshift         | Shift the track by the given time: a number followed by ns, us, ms or s, such as -120ms or 1.5s. A number alone is in milliseconds. Can be negative. Without it, the start of a track against the picture is taken from the source; a Matroska file without video has no picture to measure against, so its tracks start together unless timeshift= says otherwise. 
angle=<n>         | The angle of a Blu-ray playlist or of a disc title (`title=`, `dvd-title=`) with several angles, from 1. Every line of one playlist or disc title takes the same angle. The listing says how many angles a title has.
down-to-dts       | Available only for DTS-HD tracks. Filter out HD part. 
down-to-ac3       | For TRUE-HD and E-AC3 (DD+) tracks. Keep the AC-3 core, drop the HD part. 
drop-ac3-core     | **Matroska output or demux, TrueHD only.** A Blu-ray TrueHD stream carries an AC-3 core beside its lossless part. Matroska writes the core as a track of its own; with drop-ac3-core it is left out and the lossless stream is kept alone. A demux then writes `.thd` instead of `.ac3+thd`. A TS or a Blu-ray keeps the core, so it is ignored there with a warning.
split-ac3-core    | **Demux only, disc TrueHD with its core only** (an A_AC3 line, without drop-ac3-core or down-to-ac3). Writes the two halves as `.thd` and `.ac3` files of their own beside the `.ac3+thd` file that holds the pair, in one pass. Anywhere else it is ignored with a warning.
secondary         | Mux as secondary audio.  Available for DD+ and DTS-Express. 
default           | Mark this track as the default one. `none`, `no`, `0`, `false` or `off` say that it is not.
w64               | **Demux only, LPCM only.** Write the track as Wave64 (`.w64`) instead of WAV. A WAV header states its sizes in 32 bits and so cannot describe a file past 4 GiB, which is why a long track is otherwise written as two; Wave64 is the same audio with 64 bit sizes and stays one file.
stretch           | Stretch audio by a given factor. Can be a decimal value or a fraction (e.g. 25/24), from 0.5 up to 2. Useful for fixing A/V sync issues caused by frame rate discrepancies. Audio tracks only: on a video or subtitle track it is ignored with a warning.
merge-ac3-track   | **MKV only, A_MLP only.** Matroska track number of a classic AC-3 stream to interleave with this TrueHD track for Blu-ray-style muxing. Requires `track=<TrueHD track number>`. Do not add the AC-3 track as a separate meta line.
merge-ac3-file    | **A_MLP only.** Path to an external classic AC-3 (`.ac3`) file to interleave with a standalone TrueHD (`.thd`) stream for Blu-ray-style muxing.

### TrueHD + AC-3 Compatibility Core Merge

Some remuxed Blu-ray MKVs store Dolby TrueHD and the AC-3 compatibility track as **separate tracks**. For Blu-ray output, tsMuxer can merge them into the Blu-ray style interleaved TrueHD+AC-3 stream during muxing.

- **Note**: You can merge either:
  - an **AC-3 Matroska track** from the same MKV (`merge-ac3-track`), or
  - an external **AC-3 file** (`merge-ac3-file`).

#### Meta file example

```
MUXOPT --blu-ray
V_MPEG4/ISO/AVC, "movie.mkv", track=1
A_MLP, "movie.mkv", track=2, merge-ac3-track=3
```

### TrueHD (`.thd`) + AC-3 (`.ac3`) Merge (Elementary Streams)

If you have standalone TrueHD and AC-3 files, you can merge them directly:

```
MUXOPT --blu-ray
A_MLP, "audio.thd", merge-ac3-file="compat.ac3"
```

Additional parameters for video tracks:

Parameter         | Description 
---               | --- 
fps               | The number of frames per second. If not defined, the value is auto detected from the source stream or the container metadata (e.g. MKV default_duration, MP4 timescale), or from the times of its pictures in a TS, an M2TS or a Blu-ray playlist. If none of them gives it, it defaults to 25. The GUI then writes no fps either; 23.976 is the value its fps box offers when it is ticked. 
delPulldown       | Remove pulldown from the track, if it exists. If the pulldown is present, the FPS value is changed from 30 to 24. 
ar                | Override video aspect ratio: 1:1, 4:3, 16:9 or 2.21:1 (also written with x, such as 16x9). Any other value is refused. 

Additional parameters for H.264 video tracks:

Parameter         | Description 
---               | --- 
level             | Overwrite the level in the H264 or HEVC stream (level=4.1, level=5.1). Do note that this option only updates the headers and does not reencode the stream, which may not meet the requirements for a lower  level. Ignored, with a warning, for any other codec (VVC, AV1, VP9 and so on). 
insertSEI         | If the original stream does not contain SEI picture timing, SEI buffering period or VUI parameters, add this data to the stream. This option is recommended for BD muxing. 
forceSEI          | Add SEI picture timing, buffering period and VUI parameters to the stream and rebuild this data if it already exists. 
contSPS           | If the original video doesn't contain repetitive SPS/PPS, then SPS/PPS will be added to the stream before each key frame. This option is recommended for BD muxing. 
hdr10-override    | **UHD Blu-ray only, HEVC only.** For a stream that says SDR: the disc marks it as HDR10 (in its playlist, clip information and index). The picture is not changed, so a player switches the screen to HDR while the picture is coded for SDR; a warning says so. Ignored, with a warning, for any other output. A stream that is already HDR10, HDR10+ or Dolby Vision is marked as it is.
subTrack          | Selects one half of a track that carries two streams. The listing gives such a track as two lines, one for each part, and the GUI writes both. A dual layer Dolby Vision track given on one line without subTrack= goes to a disc as one stream, which plays as HDR10; a warning says so. For a combined AVC/MVC track, 1 is the MVC part and 2 is the AVC part. For a dual layer Dolby Vision track, 1 is the base layer and 2 is the enhancement layer. The two cases do not number their parts the same way. 
secondary         | Mux as secondary video (PIP). 
pipCorner         | Corner for PIP video. Allowed values: "TopLeft","TopRight", "BottomRight", "BottomLeft". 
pipHOffset        | PIP window horizontal offset from the corner in pixels. 
pipVOffset        | PIP window vertical offset from the corner in pixels. 
pipScale          | PIP window scale factor. Allowed values: "1", "1/2", "1/4", "1.5", "fullScreen". 
pipLumma          | Allow the PIP window to be transparent. Transparent colors are lumma colors in range [0..pipLumma].

Additional parameters for PG and SRT tracks:

Parameter         | Description 
---               | ---
video-width       | The width of the video in pixels. 
video-height      | The height of the video in pixels. 
default           | Mark this track as the default one. `all` (or `yes`, `1`, `true`, `on`) shows all subtitles, and `forced` shows only elements marked as "forced" in the subtitle stream; in a Matroska output the track is then marked forced as well. `none`, `no`, `0`, `false` or `off` say that it is not the default. `default` with no value is the same as `all`. The values are read in any case, and any other value is refused.
fps               | Video fps. It is recommended to define this parameter in order to enable more careful timing processing. 
3d-plane          | Defines the number of the '3D offset track' which is placed inside the MVC track. Each message has an individual 3D offset. This information is stored inside 3D offset track. A whole number from 0 up to one less than the number of planes of the MVC track (its listing gives that number as `3d-pg-planes`), or 255 for none: the subtitles are then shown at the depth of the screen. Any other value is refused. For a PG track and for text subtitles drawn into pictures; it applies to a 3D Blu-ray only.

Additional parameters for SRT tracks:

Parameter         | Description 
---               | --- 
font-name         | Font name to render. 
font-color        | Font color, defined as a hexadecimal or decimal number. 24-bit long numbers (for instance 0xFF00FF) define RGB components, while 32-bit long ones (for instance 0x80FF00FF) define ARGB components. 
font-size         | Font size in pixels: a whole number from 1. 
font-italic       | Italic display text. 
font-bold         | Bold display text. 
font-underline    | Underlined text. 
font-strike-out   | Strikethrough text.
font-charset      | Font character set (numeric). Allows selection of a specific character set for font rendering.
bottom-offset     | Distance from the lower edge while displaying text.
font-border       | Outline width: a number from 0. 
fadein-time       | Time in seconds for smooth subtitle appearance, for example 0.25.
fadeout-time      | Time in seconds for smooth subtitle disappearance, for example 0.25.
line-spacing      | Interval between subtitle lines: a number from 0. Default value is 1.0.
keep-text         | Keep the subtitles as text instead of drawing them into pictures. Each subtitle is copied into the track as text, with its tags and its own start and end, and the font and picture parameters are not needed. For Matroska output, and for a demux, which then writes an SRT file (UTF-8, numbered from 1); ASS, SSA and WebVTT subtitles from a Matroska file are written as .ass, .ssa and .vtt files instead, in their own form. Other outputs draw the subtitles as usual, with a warning, which needs `video-width`, `video-height` and `fps`.
save-srt          | `save-srt="name.srt"`: while the subtitles are drawn into pictures, also write them as text to that SRT file (UTF-8, numbered from 1), as a demux with `keep-text` would write them. Ignored, with a warning, on a line with `keep-text`. In the window: "Also save drawn SRT subtitles as .srt files" on the Subtitles tab.
fonts-dir         | `fonts-dir="folder"`: a folder of fonts that ASS and SSA subtitles drawn with libass may use, besides the fonts attached to the file and the fonts installed in the system (see below). In the window: "Fonts folder for ASS and SSA subtitles" on the Subtitles tab.
ass-as-srt        | Draw ASS and SSA subtitles as plain text with the font parameters above, as SRT subtitles are drawn, instead of with libass. In the window: "Draw ASS and SSA subtitles as plain text, without their styles" on the Subtitles tab.
keep-textst       | **Text subtitles of a Blu-ray only.** With keep-text into a Matroska file: keep the track as TextST (S_HDMV/TEXTST). Without it the track is written as SRT.

On Linux and Mac, a font can be named either by its family name or by the full path to the file. Named by family, tsMuxeR searches `/usr/share/fonts/`, `/usr/local/share/fonts/`, `~/.fonts` and `~/.local/share/fonts` on Linux, and `/Library/Fonts/` and `~/Library/Fonts` on Mac, and it accepts `.ttf`, `.otf` and `.ttc`. Note that the search is case sensitive on the extension, so a file copied from Windows as `Arial.TTF` is found only by its full path.

#### ASS and SSA subtitles drawn with libass

ASS and SSA subtitles from a Matroska file are listed as `S_TEXT/UTF8`. Whenever they are drawn into pictures, that is unless `keep-text` keeps them as text in a Matroska output or a demux, tsMuxeR draws them with [libass](https://github.com/libass/libass), as their script says: with their styles, colours, positions, rotations, fades, moves and karaoke, at the picture size given by `video-width` and `video-height`, and frame by frame at `fps` wherever the picture changes. The pictures become a PGS track, as DVD and DVB subtitles do. libass uses the fonts attached to the Matroska file, the fonts in the script's own `[Fonts]` section, the fonts in `fonts-dir`, and the fonts installed in the system. `save-srt` writes the text of every subtitle as well, without its styles.

An ASS or SSA script as a file of its own (`.ass`, `.ssa`, UTF-8) is read the same way, as track 1 of the file: `S_TEXT/UTF8, "film.ass", video-width=1920, video-height=1080, fps=23.976` draws it, and with `keep-text` a Matroska output keeps it as ASS or SSA and a demux writes it as a script again. Its Dialogue lines are taken in the order of their start times; Comment lines are kept in its header.

libass is not built into tsMuxeR. It is loaded when such a track is drawn:

* Windows: the packages hold `ass-9.dll` and the libraries it needs beside `tsMuxeR.exe`. Their licenses are in `THIRD-PARTY-NOTICES.txt` and the `licenses` folder.
* Linux: the distribution's libass package (`libass9` on Debian and Ubuntu, `libass` on Fedora and Arch).
* Mac: libass from Homebrew (`brew install libass`).

Without libass, or with `ass-as-srt`, the subtitles are drawn as plain text with the font parameters above, without their styles, and tsMuxeR says so in a warning when libass is missing.

tsMuxeR supports additional tags inside SRT tracks.  The syntax  and parameters coincide with HTML: `<b>, <i>, <u>, <strike>, <font>`. Default relative font size (used in these tags) is 3.  For example:
```
<b><font size=5 color="deepskyblue" name="Arial"><u>Test</u>
<font size= 4 color="#806040">colored</font>text</font>
</b>
```

Global additional parameters are placed in the first line of the META file, which must begin with the MUXOPT token. All parameters in this group start with two dashes:

Parameter           | Description 
---                 | --- 
--no-pcr-on-video-pid | Allocate a separate PID for PCR and do not use the existing video PID. Activated automatically for BD and AVCHD muxing, and for a mux without video.
--new-audio-pes     | Use bytes 0xfd instead of 0xbd for AC3, True-HD, DTS and DTS-HD. Activated automatically for BD and AVCHD muxing. 
--no-hdmv-descriptors | Use ITU-T H.222.0 / ISO/IEC 13818-1 descriptors instead of HDMV descriptors. Not activated for BD or AVCHD muxing.
--vbr               | Use variable bitrate. This is the default mode, so on its own the word changes nothing.
--minbitrate        | Sets the lower limit of the muxing rate, in kbps. If the streams together come to less, NULL packets are inserted to make up the difference. Has no effect unless --bitrate or --maxbitrate is given as well.
--maxbitrate        | Sets a ceiling on the muxing rate, in kbps: the stream is paced so that it never asks to be read faster than that. The rate counts the transport stream. In an .m2ts file and on a disc the 4-byte timestamp of each packet is not counted, as in the Blu-ray read rate, so the same number allows about 2 percent more there than with --bitrate. Nothing is dropped, so the output is no smaller: where the video needs more than the ceiling, its pictures reach the player after they are due, and a warning says how many and by how much. For a fixed rate use --bitrate. A --bitrate or --minbitrate above it is lowered to it, with a warning. Must be above 90.
--cbr               | Ask for a fixed bitrate. The rate comes from --bitrate, so --cbr on its own does nothing. --vbr and --cbr ask for opposite things: given both, a warning says so, and --bitrate decides as always. 
--bitrate           | Set a fixed bitrate in kbps, so --bitrate=35000 means 35 Mbps. This sets both the minimum and the maximum to that value. Where the streams need less, NULL packets pad the difference; where they need more, the data waits for the rate, and a warning counts the pictures that arrive after they are due. The rate counts everything written, in an .m2ts file and on a disc the timestamp of each packet too. Must be above 90.
--vbv-len           | The  length  of the  virtual  buffer  in milliseconds.  The default value  is 500.  Typically, this  option  is used together with --cbr. The parameter is similar to  the value of  vbv-buffer-size  in  the  x264  codec,  but  defined in milliseconds instead of kbit. 
--no-asyncio        | Do not  create  a separate thread  for writing. This option also disables the FILE_FLAG_NO_BUFFERING flag on Windows when writing. This option is deprecated. 
--auto-chapters     | Insert a chapter every <n> minutes. Used only in BD/AVCHD mode; on any other output it is ignored with a warning. 
--custom-chapters   | A semicolon delimited list of hh:mm:ss.zzz strings, representing the chapters' start times. On a disc the title always begins with a chapter: when the list has no time at the start, a mark is added there, so 5;10 gives three chapters. 
--chapters-file     | Read the chapter list from a file instead. Two formats are accepted and are told apart by what is in them: the simple text form, with CHAPTER01= and CHAPTER01NAME= lines, and Matroska XML as mkvextract writes it. Unlike --custom-chapters these carry the chapter NAMES, which Matroska output then keeps. 
--no-chapters       | Do not take over the chapters of the source. A Matroska output then has none, and a demux writes no chapter files. Chapters given with --custom-chapters or --chapters-file still go into a Matroska output. 
--demux             | Run in demux mode : the selected audio and video tracks are stored as separate files. The output name must be a folder name. All selected effects (such as changing the level of a H264 stream) are processed. When demuxing, certain types of tracks are always changed : - Subtitles in a Presentation Graphic Stream are converted into sup format. - PCM audio is saved as WAV files. A chapters.txt is written beside the tracks when the source carries chapter marks, one time per line as hh:mm:ss.mmm, which is the form the GUI's chapter box takes. Note that --custom-chapters wants those same times separated by semicolons rather than by lines. 
--blu-ray           | Mux as a BD disc. If the output file name is a folder, a Blu-Ray folder structure is created inside that folder. SSIF files for BD3D discs are not created in this case. If the output name has an .iso extension, then the disc is created directly as an image file. A Blu-ray player plays MPEG-2, H.264 (AVC) and VC-1 video, MVC for 3D, and HEVC on a UHD disc (--blu-ray-v3), with LPCM, AC-3, E-AC-3, TrueHD with its AC-3 core, DTS and DTS-HD sound. Other codecs, such as AAC, MP2, MP3 and VVC, are muxed with a warning, and such a disc does not play in a Blu-ray player. Opus, AV1, FLAC, Vorbis, VP8 and VP9 are refused on a disc. 
--blu-ray-v3        | As above - except mux to UHD BD discs. If you're using the GUI, this will be automatically set if one of the streams is HEVC.
--avchd             | Mux to AVCHD disc. An AVCHD player plays H.264 (AVC) video with AC-3 or LPCM sound. Other codecs that a Blu-ray disc takes (MPEG-2, VC-1 and HEVC video, DTS, DTS-HD, E-AC-3 and TrueHD sound) are muxed with a warning, and such a disc does not play in an AVCHD player. 
--cut-start         | Trim the beginning of the file. The value is a number followed by the time unit "ms" (milliseconds), "s" (seconds) or "min" (minutes), such as 90s, or a clock time such as 00:01:30. A start at or past the end of the sources is an error. 
--cut-end           | Trim the end of the file. Same rules as --cut-start apply. 
--cut-pictures      | Which pictures of the video a cut keeps. A video that is not encoded again can only begin at a key picture, and the pictures shown just before an end can be made from one shown after it. `cover`, the default, keeps every picture the cut needs: the video begins at the key picture at or before --cut-start, and on a Blu-ray or AVCHD disc it ends with the last picture decoded before --cut-end. The disc playlist plays only from --cut-start to --cut-end, from the first picture shown at or after each, so no picture outside the cut is shown, and a film cut in two loses no picture between the parts. A TS, M2TS or Matroska output shows up to one group of pictures before the sound at the start, and it ends as with `inside`, as no playlist stops it at the cut: the pictures kept past --cut-end would be shown, after a gap where the left out pictures were. A film cut in two then loses a picture between the parts only when the cut is exactly on a key picture: the few pictures shown just before it and made from it are in no part. A demux says how much later the sound starts than the picture, as the timeshift to give it when the files are muxed together. `inside` keeps only the pictures shown inside the cut, as earlier versions did: the video begins at the first key picture at or after --cut-start, and the pictures before that key picture, and those shown just before --cut-end, are in no part. The sound and subtitles start and stop at the cut either way. 
--split-duration    | Split the output into several files, with each of them being <n> seconds long. A unit may follow: ms, s, min or h, such as 10min. The length must be above zero. 
--split-size        | Split the output into several files of about the given size each: a part ends at the first key picture after the size is reached, so it can be larger by up to one group of pictures. KB, KiB, MB, MiB, GB, GiB, TB and TiB are accepted as size units, and K, M, G and T as KB, MB, GB and TB; a number alone is in bytes. Any other unit is refused. 
--split-chapters    | Split the output so that each file begins at a chapter. The marks come from a Blu-ray playlist or from a Matroska's own chapters, and --custom-chapters overrides both. As with the other split options the cut lands on the first key frame at or after the mark. A Matroska output is split by all three options the same way, into name.split.1.mkv, name.split.2.mkv and on. 
--keep-editions     | Matroska output only, from one Matroska file: keep its chapters exactly as they are, every edition of them and every link to another file, and its segment identity (SegmentUID, PrevUUID, NextUUID), so that ordered editions and linked files still work after a remux. The file is read as it is stored, not as its ordered edition plays it. Remux each file of a linked set this way. Not with a join, a cut, a split, --custom-chapters, --chapters-file, --no-chapters or a .webm output.
--title             | Matroska output: the title of the file. Without it a Matroska source's own title is kept.
--lang-codes        | Which form a language read from a disc or a container is reported in. Twenty languages have two codes in ISO 639-2, Greek being ell or gre. Takes source (no conversion either way), terminological (ell, deu, nld, the default and what earlier versions did) or bibliographic (gre, ger, dut, what eac3to shows). Unlike the other options this is an argument rather than a MUXOPT entry, so it also applies when simply listing a file. A language typed into a file name is left exactly as typed. 
--playlist-clips    | How the clips of a Blu-ray playlist are muxed. `played`, the default, takes from each clip only the part its play item plays: from the key picture at or before the point where a player starts to the one at or after the point where it stops. `whole` takes every clip whole, as earlier versions did. It is a MUXOPT entry and also an argument, as --lang-codes is, so that a listing of a playlist gives the lengths and marks of the same parts. 
--report            | `--report=<file>` writes a report of the run to the file, to attach to a problem report in place of the source: see [Report for a problem report](#report-for-a-problem-report). A command line argument, not a MUXOPT entry. 
--make-sample       | Separate mode: `tsMuxeR --make-sample [options] <source> <out.zip>`. A part of a source with the report of making it, in a zip under 25 MB for a problem report. Options: `--from-start`, `--from-end`, `--at=<time>`, `--size=<MB>`, `--add-report=<file>`, `--report-keep-names`. See "Sample for a problem report" above.
--report-keep-names | Keeps the names of files, titles, track names and chapter names in the report, which are replaced by placeholders otherwise. 
--report-full-paths | Keeps the folders in the paths of the report, which are left out otherwise, and the names as `--report-keep-names` does. 
--report-frames     | With --report, a file of every PES and of the decoder's buffer for MPEG-2 video, beside the report. `--report-frames=<from>-<to>` takes only that time range of the output. 
--right-eye         | Use base video stream for right eye. Used for 3DBD only.
--start-time        | Timestamp of the first video frame. May be defined as 45 kHz clock (just a number) or as time in hh:mm:ss.zzz format. If not set, muxing starts at 600 s (27000000 ticks). For every output except plain \*.ts, values below 524280 ticks (11.65 s) are raised to 524280: below that point Blu-ray players cannot navigate back to the start of the disc (their 32-bit 45 kHz registers underflow).
--mplsOffset        | The number of the first MPLS file. Used for BD disc mode.
--m2tsOffset        | The number of the first M2TS file. Used for BD disc mode. With this option or --mplsOffset, a job into a disc folder that exists adds its title to the folder. A number that is already there is refused.
--insertBlankPL     | Add an additional short playlist.  Used for cropped video muxed to BD disc.
--blankOffset       | Blank playlist number, 0 to 1999, 1900 if not set. Its clip gets the same number, so it must differ from --mplsOffset and from the main title's clips (from --m2tsOffset on, one a part when the output is split).
--label             | Disk label when muxing to ISO.
--meta-dir          | Blu-ray: the disc's title and the pictures a player such as the PS3 shows (BDMV/META/DL), from this folder: bdmt_eng.xml and the like, with the pictures they name. A folder that holds DL, META/DL or BDMV/META/DL is taken too. Without it, a disc muxed from a Blu-ray disc keeps that disc's own. Only the bdmt files and the pictures they name are copied. --meta-dir=none writes none. Also an option of --bdmv-to-iso, where it replaces the folder's own. Ignored, with a warning, when no Blu-ray disc is made.
--extra-iso-space   | Allocate extra space in 64K units for ISO metadata (file and directory names). Normally, tsMuxeR allocates this space automatically, and for a split it counts the parts from the split size, the split length or the chapters. A split it cannot count, such as --split-duration on a source that states no length, may need this option. When the space is too small, the mux ends with a message saying so. `--bdmv-to-iso` takes it too, and adds it to the room counted from the files and names of the folder.
--constant-iso-hdr  | Generates an ISO header that does not depend on the program version or the current time. Normally, the ISO header's "application ID", "implementation ID", and "volume ID" fields are set to strings containing the program version and/or a random number, while the access/modification/creation times of the files in the image are set to the current time. This option disables this behaviour by filling these fields with hardcoded values and setting the file times to `1 Jan 1970 00:00:01 UTC`, written as UTC, so the header is the same in every time zone. Using this option is not recommended for normal usage, as it is meant only for testing ISO output validity.
--inner-only        | ISO output with --disc-size: keep the data on the inner part of every layer and fill the outer edge of the disc with zeros. The disc is muxed into a folder beside the image first and the image is then laid out from it, as --bdmv-to-iso --inner-only lays it out, so the drive needs room for both while it runs. Replaces --layer-break-guard; a MUXOPT line with both is refused. See [DISC_AUTHORING.md](DISC_AUTHORING.md).
--no-layer-break-guard | ISO output: leave out the layer-break guard an image crossing a layer break gets by default (288 MB after the break, 18 MB before it). A guard given with --layer-break-guard is still written.
--disc-size         | Blu-ray or ISO output: refuse the job before muxing when the estimated image does not fit the target disc. Takes `bd25`, `bd50`, `bd100`, `bd128`, or a byte count with an optional k, m, g (decimal) or ki, mi, gi (binary) suffix, such as `25000000000`, `24g` or `23gib`. It also chooses the layer breaks of the image.
--allow-oversize    | With --disc-size, an image too large for the disc is a warning instead of an error, and it is built anyway.
--allow-extra-tracks | Writes more than 32 tracks of one kind (audio, subtitles, secondary audio, secondary video) to a disc, with a warning. A disc has numbers for 32 of each kind; some players show the tracks past the 32nd, others leave them out. Without it, such a disc is refused.
--layer-break-guard | ISO output: the size in MB of the zeros AFTER each layer break of a BD-R/RE DL or BD-R XL, where a burnt disc is most often defective, plus a sixteenth of it (at least 4 MB) before the break. The film crossing the break stays one file and plays without a pause. 0 aligns to the break without zeros. Without the option an image crossing a break gets 288 MB after it and 18 MB before it. Not with --inner-only.
--layer-break-guard-before | ISO output: the size in MB of the zeros BEFORE the break, in place of a sixteenth of --layer-break-guard.
--layer-break-lbn   | The layer break sector(s), in 2048 byte sectors: the total sectors of the disc divided by its layers. Write the number without grouping: a comma separates breaks, so `12,219,392` is refused. One value for a BD-R/RE DL (`12219392` for a full 50 GB disc, the default), two for a 100 GB BD-R XL, three for 128 GB. Work them out from the "Free Sectors" your own disc reports when it is fully formatted. See [DISC_AUTHORING.md](DISC_AUTHORING.md).
--dv-profile        | **MKV output only, from a dual layer Dolby Vision source.** `7`, the default, keeps the disc as it is. `8.1` converts the Dolby Vision metadata so the file plays as single layer profile 8.1, while the disc can still be built again from it; the file is not smaller. Needs the libdovi library (dovi.dll on Windows) beside tsMuxeR. See [DOLBY_VISION.md](DOLBY_VISION.md).

### BDMV folder to ISO

A mode of its own, without a meta file:
```
    tsMuxeR --bdmv-to-iso [options] <BDMV_folder> <out.iso>
```
It wraps an existing BDMV folder into a UDF 2.50 BD-ROM ISO image byte for byte, with no remux and no renumbering, so BD-J menus and every clip and playlist reference stay valid, and lays the image out for the layer break of the disc. The "BDMV folder -> ISO" tab of the window does the same. Its options:

Option              | Description
---                 | ---
--layer-break-guard, --layer-break-guard-before, --no-layer-break-guard, --layer-break-lbn, --allow-oversize, --inner-only, --constant-iso-hdr, --label, --extra-iso-space | As in the MUXOPT line above.
--disc-capacity     | The total sectors of the target disc, from its full formatted capacity. Needed by --inner-only; with it the image is also checked against the disc before and after it is built.
--keep-extra-files  | Copy everything in the source folder, not only BDMV, CERTIFICATE and AACS.
--3d-single-copy    | Store a 3D disc's video once instead of twice, as the source disc does.
--original-order    | Write the files in disc order instead of the largest .m2ts first. Better for seamless branching titles.
--no-layer-fit      | Do not move a file that would cross the layer break to after it; split it instead.
--meta-dir          | The disc's title and pictures from this folder, in place of the source folder's own BDMV/META/DL; `none` leaves them out.

The source can also be a disc image, `tsMuxeR --bdmv-to-iso film.iso new.iso`, which is read without mounting it, for a copy with a new layout, a guard or other disc pictures.
