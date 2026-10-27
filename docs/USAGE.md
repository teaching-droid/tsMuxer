# tsMuxer : Usage Instructions

## GUI

The simplest thing to do is to use the tsMuxerGUI. A screenshot of that can be seen below:

![tsMuxerGUI_Screenshot](https://user-images.githubusercontent.com/122434/148913872-b7af9caa-c2ca-4892-8853-06fd6329a15a.png)

## Multi-layer disc authoring (this fork)

This fork adds a "BDMV folder to ISO" tab and the `--bdmv-to-iso`, `--layer-break-guard`,
`--layer-break-lbn`, `--disc-size`, `--allow-oversize` and `--inner-only` options for authoring dual-layer and
BD-R XL discs. See [DISC_AUTHORING.md](DISC_AUTHORING.md) (also available in German as
[DISC_AUTHORING_DE.md](DISC_AUTHORING_DE.md)) for the details and the layer-break calculator.

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

**If the MKV has no AC-3 track:**

Create an AC-3 compatibility stream from the TrueHD audio with FFmpeg (choose the correct audio index for `0:a:N`):

```
ffmpeg -i input.mkv -map 0:a:0 -c:a ac3 -b:a 640k -ac 6 compat.ac3
```

Then either:
- use **Merge AC-3 file** / `merge-ac3-file="compat.ac3"` directly, or
- **remux** `compat.ac3` back into the MKV as a separate audio track (e.g. with `mkvmerge`) and use **Merge AC-3 track** / `merge-ac3-track` with the new AC-3 track number.

## Command Line

Alternatively you can use tsMuxer via the command-line. 

Examples:
```
    tsMuxeR <media file name>
    tsMuxeR <meta file name> <out file/dir name>
```

tsMuxeR can be run in track detection mode or muxing mode. If tsMuxeR is run with only one argument, then the program displays track information required to construct a meta file. When running with two arguments, tsMuxeR starts the muxing or demuxing process.

Supported output formats:
* **TS** - MPEG Transport Stream
* **M2TS** - Blu-ray MPEG-2 Transport Stream
* **MKV/MKA** - Matroska Video/Audio container
* **ISO** - Blu-ray disc image
* **Blu-ray folder structure** - BDMV directory layout

The output format is determined by the file extension of the output file name (e.g. `.ts`, `.m2ts`, `.mts`, `.m2t`, `.mkv`, `.mka`, `.mks`, `.mk3d`, `.webm`, `.iso`).

The output of the program is encoded in UTF-8, which means that non-ASCII characters will not show up properly in the Windows console by default. If you want to see the output properly, run `chcp 65001` before running tsMuxeR.

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
V_AV1             | AV1 
V_VP8             | VP8 (read from MKV, WebM and .ivf files; MKV output, or demuxed to an .ivf file)
V_VP9             | VP9 (read from MKV, WebM, MP4 and .ivf files; MKV output, or demuxed to an .ivf file)
V_MPEG4/ISO/ASP   | MPEG-4 Part 2 video (read from MP4 and MOV; copied into MKV output only)
V_PRORES          | Apple ProRes (read from MOV; copied into MKV output only)
A_AC3             | AC3/AC3+/TRUE-HD 
A_AAC             | AAC 
A_DTS             | DTS/DTS-Express/DTS-HD 
A_MP3             | MPEG audio layer 1/2/3 
A_LPCM            | raw pcm data or PCM WAV file 
A_FLAC            | FLAC (MKV and demux output only)
A_OPUS            | Opus (TS, MKV, and demux output only)
A_VORBIS          | Vorbis (read from MKV, WebM and MP4; MKV output, or demuxed to an .ogg file)
A_ALAC            | Apple Lossless (read from MKV and MP4; MKV output only)
S_HDMV/PGS        | Presentation graphic stream (BD subtitle format) 
S_TEXT/UTF8       | SRT subtitle format.  Encoding MUST be  UTF-8/UTF-16/UTF-32 

Each track may have additional parameters. Track parameters do not have dashes. If a parameter's value consists of several words, it must be enclosed in quotes.

Common additional parameters for any type of track:

Parameter         | Description 
---               | --- 
track             | track number if input file is a container.
lang              | track language: three letters, an ISO 639-2 code such as eng or deu, or a BCP 47 tag such as de, pt-BR or zh-Hant. A tag becomes its ISO 639-2 code (in the form --lang-codes asks for) on a disc, in a transport stream and in a demuxed file name; a Matroska output writes the tag as well. 

Additional parameters for audio tracks:

Parameter         | Description 
---               | --- 
timeshift         | Shift the track by the given time: a number followed by ns, us, ms or s, such as -120ms or 1.5s. A number alone is in milliseconds. Can be negative. 
down-to-dts       | Available only for DTS-HD tracks. Filter out HD part. 
down-to-ac3       | For TRUE-HD and E-AC3 (DD+) tracks. Keep the AC-3 core, drop the HD part. 
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
level             | Overwrite the level in the H264 stream. Do note that this option only updates the headers and does not reencode the stream, which may not meet the requirements for a lower  level. 
insertSEI         | If the original stream does not contain SEI picture timing, SEI buffering period or VUI parameters, add this data to the stream. This option is recommended for BD muxing. 
forceSEI          | Add SEI picture timing, buffering period and VUI parameters to the stream and rebuild this data if it already exists. 
contSPS           | If the original video doesn't contain repetitive SPS/PPS, then SPS/PPS will be added to the stream before each key frame. This option is recommended for BD muxing. 
hdr10-override    | **UHD Blu-ray only, HEVC only.** For a stream that says SDR: the disc marks it as HDR10 (in its playlist, clip information and index). The picture is not changed, so a player switches the screen to HDR while the picture is coded for SDR; a warning says so. Ignored, with a warning, for any other output. A stream that is already HDR10, HDR10+ or Dolby Vision is marked as it is.
subTrack          | Selects one half of a track that carries two streams. TsMuxeR always splits such a track into its two parts. For a combined AVC/MVC track, 1 is the MVC part and 2 is the AVC part. For a dual layer Dolby Vision track, 1 is the base layer and 2 is the enhancement layer. The two cases do not number their parts the same way. 
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

On Linux and Mac, a font can be named either by its family name or by the full path to the file. Named by family, tsMuxeR searches `/usr/share/fonts/`, `/usr/local/share/fonts/`, `~/.fonts` and `~/.local/share/fonts` on Linux, and `/Library/Fonts/` and `~/Library/Fonts` on Mac, and it accepts `.ttf`, `.otf` and `.ttc`. Note that the search is case sensitive on the extension, so a file copied from Windows as `Arial.TTF` is found only by its full path.

#### ASS and SSA subtitles drawn with libass

ASS and SSA subtitles from a Matroska file are listed as `S_TEXT/UTF8`. Whenever they are drawn into pictures, that is unless `keep-text` keeps them as text in a Matroska output or a demux, tsMuxeR draws them with [libass](https://github.com/libass/libass), as their script says: with their styles, colours, positions, rotations, fades, moves and karaoke, at the picture size given by `video-width` and `video-height`, and frame by frame at `fps` wherever the picture changes. The pictures become a PGS track, as DVD and DVB subtitles do. libass uses the fonts attached to the Matroska file, the fonts in the script's own `[Fonts]` section, the fonts in `fonts-dir`, and the fonts installed in the system. `save-srt` writes the text of every subtitle as well, without its styles.

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
--no-pcr-on-video-pid | Allocate a separate PID for PCR and do not use the existing video PID. Activated automatically for BD and AVCHD muxing.
--new-audio-pes     | Use bytes 0xfd instead of 0xbd for AC3, True-HD, DTS and DTS-HD. Activated automatically for BD and AVCHD muxing. 
--no-hdmv-descriptors | Use ITU-T H.222.0 / ISO/IEC 13818-1 descriptors instead of HDMV descriptors. Not activated for BD or AVCHD muxing.
--vbr               | Use variable bitrate. This is the default mode, so on its own the word changes nothing.
--minbitrate        | Sets the lower limit of the muxing rate, in kbps. If the streams together come to less, NULL packets are inserted to make up the difference. Has no effect unless --bitrate or --maxbitrate is given as well.
--maxbitrate        | Sets a ceiling on the muxing rate, in kbps: the stream is paced so that it never asks to be read faster than that. Nothing is dropped, so the output is no smaller: where the video needs more than the ceiling, its pictures reach the player after they are due, and a warning says how many and by how much. For a fixed rate use --bitrate. A --bitrate or --minbitrate above it is lowered to it, with a warning. Must be above 90.
--cbr               | Ask for a fixed bitrate. The rate comes from --bitrate, so --cbr on its own does nothing. --vbr and --cbr ask for opposite things: given both, a warning says so, and --bitrate decides as always. 
--bitrate           | Set a fixed bitrate in kbps, so --bitrate=35000 means 35 Mbps. This sets both the minimum and the maximum to that value. Where the streams need less, NULL packets pad the difference; where they need more they are still written in full, so the output can come out above the figure asked for. Must be above 90.
--vbv-len           | The  length  of the  virtual  buffer  in milliseconds.  The default value  is 500.  Typically, this  option  is used together with --cbr. The parameter is similar to  the value of  vbv-buffer-size  in  the  x264  codec,  but  defined in milliseconds instead of kbit. 
--no-asyncio        | Do not  create  a separate thread  for writing. This option also disables the FILE_FLAG_NO_BUFFERING flag on Windows when writing. This option is deprecated. 
--auto-chapters     | Insert a chapter every <n> minutes. Used only in BD/AVCHD mode. 
--custom-chapters   | A semicolon delimited list of hh:mm:ss.zzz strings, representing the chapters' start times. On a disc the title always begins with a chapter: when the list has no time at the start, a mark is added there, so 5;10 gives three chapters. 
--chapters-file     | Read the chapter list from a file instead. Two formats are accepted and are told apart by what is in them: the simple text form, with CHAPTER01= and CHAPTER01NAME= lines, and Matroska XML as mkvextract writes it. Unlike --custom-chapters these carry the chapter NAMES, which Matroska output then keeps. 
--no-chapters       | Do not take over the chapters of the source. A Matroska output then has none, and a demux writes no chapter files. Chapters given with --custom-chapters or --chapters-file still go into a Matroska output. 
--demux             | Run in demux mode : the selected audio and video tracks are stored as separate files. The output name must be a folder name. All selected effects (such as changing the level of a H264 stream) are processed. When demuxing, certain types of tracks are always changed : - Subtitles in a Presentation Graphic Stream are converted into sup format. - PCM audio is saved as WAV files. A chapters.txt is written beside the tracks when the source carries chapter marks, one time per line as hh:mm:ss.mmm, which is the form the GUI's chapter box takes. Note that --custom-chapters wants those same times separated by semicolons rather than by lines. 
--blu-ray           | Mux as a BD disc. If the output file name is a folder, a Blu-Ray folder structure is created inside that folder. SSIF files for BD3D discs are not created in this case. If the output name has an .iso extension, then the disc is created directly as an image file. A Blu-ray player plays MPEG-2, H.264 (AVC) and VC-1 video, MVC for 3D, and HEVC on a UHD disc (--blu-ray-v3), with LPCM, AC-3, E-AC-3, TrueHD with its AC-3 core, DTS and DTS-HD sound. Other codecs, such as AAC, MP2, MP3 and VVC, are muxed with a warning, and such a disc does not play in a Blu-ray player. Opus, AV1, FLAC, Vorbis, VP8 and VP9 are refused on a disc. 
--blu-ray-v3        | As above - except mux to UHD BD discs. If you're using the GUI, this will be automatically set if one of the streams is HEVC.
--avchd             | Mux to AVCHD disc. An AVCHD player plays H.264 (AVC) video with AC-3 or LPCM sound. 
--cut-start         | Trim the beginning of the file. The value is a number followed by the time unit "ms" (milliseconds), "s" (seconds) or "min" (minutes), such as 90s, or a clock time such as 00:01:30. A start at or past the end of the sources is an error. 
--cut-end           | Trim the end of the file. Same rules as --cut-start apply. 
--cut-pictures      | Which pictures of the video a cut keeps. A video that is not encoded again can only begin at a key picture, and the pictures shown just before an end can be made from one shown after it. `cover`, the default, keeps every picture the cut needs: the video begins at the key picture at or before --cut-start, and on a Blu-ray or AVCHD disc it ends with the last picture decoded before --cut-end. The disc playlist plays only from --cut-start to --cut-end, from the first picture shown at or after each, so no picture outside the cut is shown, and a film cut in two loses no picture between the parts. A TS, M2TS or Matroska output shows up to one group of pictures before the sound at the start, and it ends as with `inside`, as no playlist stops it at the cut: the pictures kept past --cut-end would be shown, after a gap where the left out pictures were. A film cut in two then loses a picture between the parts only when the cut is exactly on a key picture: the few pictures shown just before it and made from it are in no part. A demux says how much later the sound starts than the picture, as the timeshift to give it when the files are muxed together. `inside` keeps only the pictures shown inside the cut, as earlier versions did: the video begins at the first key picture at or after --cut-start, and the pictures before that key picture, and those shown just before --cut-end, are in no part. The sound and subtitles start and stop at the cut either way. 
--split-duration    | Split the output into several files, with each of them being <n> seconds long. A unit may follow: ms, s, min or h, such as 10min. The length must be above zero. 
--split-size        | Split the output into several files, with each of them having a given maximum size. KB, KiB, MB, MiB, GB, GiB, TB and TiB are accepted as size units, and K, M, G and T as KB, MB, GB and TB; a number alone is in bytes. Any other unit is refused. 
--split-chapters    | Split the output so that each file begins at a chapter. The marks come from a Blu-ray playlist or from a Matroska's own chapters, and --custom-chapters overrides both. As with the other split options the cut lands on the first key frame at or after the mark. A Matroska output is split by all three options the same way, into name.split.1.mkv, name.split.2.mkv and on. 
--keep-editions     | Matroska output only, from one Matroska file: keep its chapters exactly as they are, every edition of them and every link to another file, and its segment identity (SegmentUID, PrevUUID, NextUUID), so that ordered editions and linked files still work after a remux. The file is read as it is stored, not as its ordered edition plays it. Remux each file of a linked set this way. Not with a join, a cut, a split, --custom-chapters, --chapters-file or a .webm output.
--title             | Matroska output: the title of the file. Without it a Matroska source's own title is kept.
--lang-codes        | Which form a language read from a disc or a container is reported in. Twenty languages have two codes in ISO 639-2, Greek being ell or gre. Takes source (no conversion either way), terminological (ell, deu, nld, the default and what earlier versions did) or bibliographic (gre, ger, dut, what eac3to shows). Unlike the other options this is an argument rather than a MUXOPT entry, so it also applies when simply listing a file. A language typed into a file name is left exactly as typed. 
--playlist-clips    | How the clips of a Blu-ray playlist are muxed. `played`, the default, takes from each clip only the part its play item plays: from the key picture at or before the point where a player starts to the one at or after the point where it stops. `whole` takes every clip whole, as earlier versions did. It is a MUXOPT entry and also an argument, as --lang-codes is, so that a listing of a playlist gives the lengths and marks of the same parts. 
--right-eye         | Use base video stream for right eye. Used for 3DBD only.
--start-time        | Timestamp of the first video frame. May be defined as 45Khz clock (just a number) or as time in hh:mm:ss.zzz format. If not set, muxing starts at 600 s (27000000 ticks). For every output except plain \*.ts, values below 524280 ticks (11.65 s) are raised to 524280: below that point Blu-ray players cannot navigate back to the start of the disc (their 32-bit 45 Khz registers underflow), and commercial discs never start lower.
--mplsOffset        | The number of the first MPLS file. Used for BD disc mode.
--m2tsOffset        | The number of the first M2TS file. Used for BD disc mode.
--insertBlankPL     | Add an additional short playlist.  Used for cropped video muxed to BD disc.
--blankOffset       | Blank playlist number, 0 to 1999, 1900 if not set. Its clip gets the same number, so it must differ from --mplsOffset and from the main title's clips (from --m2tsOffset on, one a part when the output is split).
--label             | Disk label when muxing to ISO.
--extra-iso-space   | Allocate extra space in 64K units for ISO metadata (file and directory names). Normally, tsMuxeR allocates this space automatically, and for a split it counts the parts from the split size, the split length or the chapters. A split it cannot count, such as --split-duration on a source that states no length, may need this option. When the space is too small, the mux ends with a message saying so.
--constant-iso-hdr  | Generates an ISO header that does not depend on the program version or the current time. Normally, the ISO header's "application ID", "implementation ID", and "volume ID" fields are set to strings containing the program version and/or a random number, while the access/modification/creation times of the files in the image are set to the current time. This option disables this behaviour by filling these fields with hardcoded values and setting the file times to `1 Jan 1970 00:00:01 UTC`, written as UTC, so the header is the same in every time zone. Using this option is not recommended for normal usage, as it is meant only for testing ISO output validity.
--inner-only        | ISO output with --disc-size: keep the data on the inner part of every layer and fill the outer edge of the disc with zeros. The disc is muxed into a folder beside the image first and the image is then laid out from it, as --bdmv-to-iso --inner-only lays it out, so the drive needs room for both while it runs. Replaces --layer-break-guard; a MUXOPT line with both is refused. See [DISC_AUTHORING.md](DISC_AUTHORING.md).
--no-layer-break-guard | ISO output: leave out the layer-break guard an image crossing a layer break gets by default (288 MB after the break, 18 MB before it). A guard given with --layer-break-guard is still written.
