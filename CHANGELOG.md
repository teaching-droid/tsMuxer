## tsMuxeR 2.18.7

A Dolby Vision MKV or MP4 remuxed to MKV now keeps its Dolby Vision. Before this it did not, and
nothing said so.

### Fixed

* **A Dolby Vision MKV or MP4 remuxed to MKV lost its Dolby Vision.** Such a file holds both layers
in one video track. Read back with `subTrack=` and written to Matroska, it gave two plain video
tracks with no Dolby Vision records on them. What went in as Dolby Vision came out as plain HDR.
Muxing the same source to a disc was never affected, and neither was any other output.

* **Nothing found about a subTrack stream reached it.** The cause was wider than Dolby Vision. The
probe looked for such a stream under a number the container never uses, so it was skipped and
everything it would have found was lost, the frame rate included.

A Dolby Vision MKV from this version is read back with `subTrack=1` and `subTrack=2`. One from an
earlier version still wants `track=1` and `track=2`, because it holds two separate tracks rather
than one.

## tsMuxeR 2.18.6

An mp4 read with `subTrack=` could crash the muxer. That is the only change.

### Fixed

* **An mp4 read with `subTrack=` could crash the muxer.** An mp4 whose video track holds a merged
Dolby Vision picture crashed when it was read back with `subTrack=` and no `fps=` on the meta line.
The frame rate lookup was handed a sub-track number and read past the end of its array. The mux
died with an access violation and wrote nothing. The same file in an mkv was never affected,
because the Matroska side of that lookup already checked its range.

Present in 2.18.4 and 2.18.5. Nothing outside that case behaves differently.

## tsMuxeR 2.18.5

A 3D disc muxed to MKV now keeps both views in one video track, so the file is read as 3D
instead of flat. No behaviour outside 3D changes.

### 3D to MKV: both views in one track

A 3D source has two video streams, the base view and the dependent view. Until now each became a
track of its own, and the second one carried a codec id that general software does not recognise.
The file rebuilt a disc correctly, but nothing else could see the second view, so it was read as
flat 2D everywhere.

Both views now travel in one track, which is the form other software writes:

- the dependent view's NAL units follow the base view's in the same block
- the track is marked StereoMode 13, both eyes in one block, left eye first
- the MVC configuration record is written as the "mvcC" block addition mapping, built from both
  views' parameter sets

There is no option to set. A source with no dependent view is muxed exactly as before.

A full length 3D source of 40 GB came out as a 34.1 GiB MKV of 2 h 5 min, read back as two views
at Stereo High, with its TrueHD and AC-3 tracks intact.

### Fixed: the last NAL of an access unit could be dropped

When a combined AVC and MVC track was split back into two views, a NAL unit of exactly four bytes
at the end of a packet was never read, because the loop stopped four bytes short of the end. An
access unit ending with the end of sequence NAL lost it.

This runs for combined AVC and MVC tracks only, so nothing outside 3D can reach it.

### Fixed: the subTrack documentation was the wrong way round

For a combined AVC and MVC track, subTrack=1 is the MVC part and subTrack=2 is the AVC part. The
usage text and the documentation both said the opposite, so a meta file written from them put each
view in the other's place.

They also both said subTrack was for combined AVC and MVC tracks only. That stopped being true when
dual layer Dolby Vision began using it, where 1 is the base layer and 2 the enhancement layer. The
two cases do not number their parts the same way, and the text now says so.

### How it was checked

A disc was built straight from the source, and a second one from the MKV, then compared file by
file:

```
before this release   12 of 12 files identical
after this release    12 of 12 files identical
```

The MKV changed completely in between and the disc did not move at all. The same comparison was
run against a 3D file written by other software, with the old build and the new one: 12 of 12
identical both ways.

## tsMuxeR 2.18.4

A 3D disc can now be wrapped into an ISO without writing its video twice.

### New: `--3d-single-copy`

A 3D Blu-ray stores its video ONCE and gives it three names. The base view and the dependent view
are cut into chunks and written alternately, and that single run of sectors is what the `.ssif`
names. The same sectors are addressed again as two `.m2ts`, one naming only the base chunks and one
only the dependent chunks, so a 2D player opens the base `.m2ts` and never sees the rest.

Copying all three by name writes the video twice. 2.18.3 started reporting that; this writes it
once instead, so all three names resolve to the same sectors again, as on the source disc.

```
tsMuxeR --bdmv-to-iso --3d-single-copy <BDMV_folder> out.iso
```

Both facts it needs are stated on the disc, so nothing is guessed: the clip info file lists where
the chunks divide, and the playlist names which two clips pair up. A group whose five files are not
all present, or whose two views are not cut into the same number of chunks, is left alone and copied
as found.

Off by default. Without it the image is byte for byte what it was before.

Measured on a six-group 3D structure, with 1, 2, 2, 4, 4 and 4 chunk pairs:

```
without   366,084,096 bytes, every chunk of video present TWICE
with      188,088,320 bytes, every chunk present ONCE, dependent view first
```

All 18 files read back byte for byte identical to the source, checked twice with two independent
readers: 7-Zip, and the operating system's own UDF driver, which is what a player uses.

Then on a full-size 3D structure, 852 files and 40.8 GiB, its main title interleaved in 1,774
chunk pairs:

```
image        43,796,791,296 bytes, against 43,800,002,560 for the source it was built from
a plain copy would have written 87.2 GB
files        852 of 852 byte for byte identical, 0 different, 0 missing, 0 added
```

The source stores its video once, and the rebuilt image lands 3.21 MB from the same size, which
is the difference in UDF overhead and nothing else. `0 added` says the source's own `BACKUP` came
through untouched rather than being regenerated.

### The 3D authoring path was verified, not changed

tsMuxeR has authored 3D discs from streams for years, and `createInterleavedFile` is shared between
that path and the new one, so it was run rather than assumed. Muxing the two views of a 3D source
to a Blu-ray ISO gives a base view and a dependent view whose sizes sum to the `.ssif` exactly,
inside an image too small to hold the video twice, with the dependent chunk first. Building a disc
that way and then copying it with `--3d-single-copy` returns every file byte for byte.

Note that only a Blu-ray ISO can hold this. Two files sharing one run of sectors is a property of
the UDF image; a folder on an ordinary filesystem cannot express it.

### Fixed: an interleaved 3D file could be built wrong instead of refused

`createInterleavedFile` guarded its two preconditions with assertions, which a release build
removes. Both mean the interleave cannot be described: unequal piece counts leave chunks with no
partner, and a part-sector piece cannot be addressed by a non-final allocation descriptor. Building
it anyway produces a file of exactly the right length with its two halves out of step, which no size
check catches and no reader reports. Both conditions now say what is wrong and refuse.

The 3D authoring path calls the same function and ignored its result, which was reasonable while
the only outcome was an assertion a release build deletes. It acts on a refusal now: carrying on
past one would end the build with "Mux successful complete" while leaving a zero-length `.ssif`
in the image, which is a 3D disc that cannot play, reported as a success.

### The BDMV folder to ISO tab has a checkbox for it

The option was reachable only from the command line, and that tab is where someone wrapping a 3D
disc actually is. One checkbox next to the other build options, off like the option itself, with a
tooltip explaining the three names. Translated into all eight interface languages.

### Fixed: leaving out the output path printed one character

`--bdmv-to-iso` was only routed to its own handler when at least four arguments came with it, so a
call that forgot the output path fell through to the ordinary muxing path, which tried to open
`--bdmv-to-iso` as a meta file and printed a single character of the resulting exception. The mode
is routed on its own name now, and all three of `--bdmv-to-iso`, one argument and three arguments
print what the mode expects.

### Fixed: the layer-break guard could move a UDF structure and corrupt files

**This is not a 3D fault.** It was found while testing the 3D authoring path, but it reproduces on
an ordinary copy with `--3d-single-copy` off and no 3D content involved.

**Who it reaches.** It needs a disc of roughly 7,000 files or more, with `--layer-break-guard` or
`--inner-only` in use. For scale, a full feature disc measured here holds 852 files, about a ninth
of that, so an ordinary disc was never at risk. If you built an image far larger than that with an
earlier version, a BD-J disc for instance, the result is worth checking. The damage is silent: the
build reports success and the files look normal.

The UDF unique-id mapping file lives at a fixed address just past the metadata partition, and it is
written at the very end of the build by seeking BACK to it. `FileEntryInfo::write` consults the
layer-break guard on every write, so a guard zone that reached back that far padded the mapping file
FORWARD, out of its own home and into the data area.

The guard exists to move file data off the sectors around a layer transition. A volume structure has
nowhere else it may live, so moving one does not protect it, it destroys the image. The guard is now
suppressed while such a structure is written.

The metadata region grows with the file count, so the two eventually meet. Measured on a 12,005 file
tree with `--inner-only` on a BD50, which is a supported combination and not a contrived one:

```
before   the mapping file's home is sector 12,928, the guard's before-zone begins at 8,704,
         so the structure was written at sector 24,429,792 and four navigation files
         (index.bdmv, MovieObject.bdmv and both .clpi) came back wrong
after    12,005 of 12,005 files byte for byte identical
```

Also visible on a small tree with a hand-set early break, where it silently replaced the first
sector of the first file with a descriptor: eight break positions from 700 to 2000 sectors are now
byte-exact where four of them were not.

### Fixed: a layer-break guard could land inside a 3D chunk

3D only, and only with a guard in use. The guard pad is laid down wherever the copy write that reaches the zone happens to begin, and a
chunk larger than the 16 MB copy buffer takes several writes, so the pad could fall inside a chunk.
The interrupted view then gained a piece its partner did not have and the group could not be
rebuilt at all. Reproduced on real chunk tables, which have 125 chunks over 16 MB in the main
title alone:

```
Can't interleave BDMV/STREAM/SSIF/00019.ssif: 5 pieces against 4
```

The pad is now placed at the boundary between one pair of chunks and the next, where both views are
between pieces and stay in step. Verified with the pad falling both before a group and inside one,
byte exact in each case.

## tsMuxeR 2.18.3

One fault in the interface that made four options do nothing, and a new message when a 3D disc's
video is being written into the image twice.

### Fixed: the dual-layer options never reached the muxer

The meta text is what the muxer is actually handed. Not one control in the Dual-Layer group was
connected to what rebuilds it, so ticking the layer-break guard, choosing a disc in Fit to disc, or
allowing oversize changed nothing at all: the mux ran with the text as it stood. The option only
appeared once something else rebuilt the meta, such as switching the output between ISO and folder,
which is why toggling that looked like a cure.

Four options were dead this way:

```
--allow-oversize      ticked, absent, and the mux refused anyway
--disc-size           chosen, absent, so no capacity check at all
--layer-break-guard   ticked, absent, so no guard was written
--layer-break-lbn     worked out from the chosen disc, absent with it
```

Eight controls are connected now, the pattern every other control in that window already uses.

Checked by driving the window itself, selecting Blu-ray ISO and never touching the output type
afterwards: on 2.18.2 ticking the guard leaves the meta line unchanged, and with this it gains
`--layer-break-guard=288`, and `--allow-oversize` with it.

Then every one of the 46 toggles in the window was tried, on all eight tabs and on two different
source files, to find out whether anything else was in the same state. Nothing was.

The BDMV to ISO tab was never affected, because it builds its command line directly and does not
use the meta. That is why that route kept working.

Reported by @DreckSoft, who also found that it explained a layer-break guard that appeared to do
nothing.

### New: it says when a 3D disc's video is being written twice

A 3D Blu-ray stores its video once and gives it three names. The base view and the dependent view
are cut into chunks and written alternately, and that run of sectors is what the `.ssif` names; the
same sectors are addressed again as two `.m2ts`, so a 2D player opens the base one and never sees
the rest.

BDMV to ISO keeps everything under BDMV, so all three are copied as ordinary files and the video
goes into the image twice. On a pressed 3D disc measured here, 32,586 MB of a 46 GB title, which
fits no disc, least of all the one it came from.

**This release only reports it**, naming the file and how much of the image is duplicated. Nothing
is written differently yet, so an image built with this version is exactly what 2.18.2 built.

It is here because it is the first step of a feature being built: writing the video once, so a 3D
disc taken to an image comes out the size it should be. That work is underway and the message is
the part that was ready. If you have been wondering why a 3D image came out roughly double, this
is the answer, and the fix is coming.

---

## tsMuxeR 2.18.2

Two fixes, and both are about the program saying what it did rather than doing something different.
Every image and every disc this version writes is byte for byte what 2.18.1 wrote.

### Fixed: a layer break guard that was asked for and never written

The guard fills the sectors around a layer break with zeros, so that no part of the film sits where a
burn is most likely to fail. It acts only when a write actually reaches the zone around the break,
and if the break falls past the end of the image, nothing is written. Nothing was said either.

That is the ordinary outcome of leaving **Fit to disc** at **Off**: with no disc chosen the break
falls back to half a BD50, sector 12,219,392 or about 25 GB, so an image smaller than that never
reaches it. The image then comes out the same size it would have been without the option, which from
outside looks exactly like the guard not working. Measured on one source, one option apart:

```
no guard                                        2,004,418,560 bytes
--layer-break-guard=288                         2,004,418,560 bytes, and silent
the same, with the break inside the content     2,327,052,288 bytes, 307.7 MiB of guard
```

The first two being identical is the whole report. It now says which sector the image ends at, which
sector the break is at, and the two ways to fix it: set Fit to disc to the disc you are burning, or
give the break directly with `--layer-break-lbn`. A mux where the guard does land is unaffected; the
warning is raised only when no zone was written at all.

Reported by @DreckSoft.

### Fixed: a layer break guard asked for with a folder destination

The guard reserves sector positions, and only the disc image writer places sectors, so a folder
destination has nothing for it to act on. The interface greys those controls out for folder output,
but a meta file or a command line could still pass the option, and it was then ignored without a
word. It now says so, and names the folder to ISO route, where the guard does apply.

---

## tsMuxeR 2.18.1

Everything here is long standing. Two faults are about a disc layout that no version has ever
handled correctly, one is a feature that has not worked in this fork at all, and the rest are in
what the interface shows and what the program reports about a file.

### Fixed: a disc that delivers its AC-3 core in groups

A TrueHD track carries an AC-3 core braided into the same PID. Every disc tested here delivers one
core frame at a time, between 38 and 39 TrueHD units apart. A disc reported in issue 10 delivers
them in groups of three, and on that layout two separate things went wrong.

**Most of the core was stamped from the wrong clock.**

A core frame was recognised by the state the decoder was left in after sizing it, and that state
only changes when the NEXT frame is not another core frame. So in a group of three, only the last
one was recognised. The other two were stamped from the TrueHD clock and advanced it by an access
unit they do not represent, while the core's own clock advanced once per group instead of once per
frame.

Measured on the reported title, a whole feature of 94 minutes:

- of 14,207 core frames in a 7.6 minute cut, 8,476 were stamped from the wrong clock
- the core clock ran at a third speed: 182 seconds of clock for 454 seconds of audio
- 70,803 timestamps went **backwards**, the worst by 3,486 seconds, which is 58 minutes
- the error grew with the length of the film, so a longer film was worse

Afterwards the worst backward step is 0.094 seconds and it no longer grows. What remains is the
disc's own grouping: three core frames written together span three frame times, so the TrueHD unit
after them is behind. The audio itself was never affected and is byte for byte identical to the
disc either way, all 2,408,473,724 bytes of it.

**And the last frame of the track kept a timestamp from an earlier one.**

The final frame arrives through the flush path, and on such a disc it can be a core frame. It got
there still carrying an old timestamp, and it also carries a flag that suppressed the replacement,
so the stale value was written out. It was the only backward step in that output the interleave does
not account for: two frames behind on a short cut, twelve over a whole feature.

Steps between core frames are now 2,880 throughout, with no repeated timestamp anywhere.

Verified unchanged where the core arrives one frame at a time, against the released 2.18.0: a
640 kbps core with AVC video, 214,745,088 bytes, and a 448 kbps core with HEVC video, 375,742,464
bytes, both byte for byte identical. So the core bitrate is not what matters, the grouping is.

Reported and confirmed on hardware by @TexasChainsaw83.

### Fixed: play sound at end

The option has never worked in this fork, on any platform, and does work in the last release built
with the old build system. Two separate reasons.

The code is guarded by a macro that the old build system defined by itself and the current one does
not, so on every platform the sound was compiled out and a system beep was used instead.

On Windows there was also nothing to compile against. That build compiles Qt from source from its
base package alone so that the result runs on Windows 7, and sound is a separate package that is not
built. The 2.18.0 package carries four Qt libraries and no sound library at all, so it could not have
played there whatever the macro said. Windows now uses the system's own call, which takes the sound
from memory, needs nothing that is not already on the machine, and works on Windows 7 as well.

Reported by @Nemesh64, and confirmed by @oniiz86 with the detail that made it findable: that it
worked in 2.7.0 and in nothing since.

### Fixed: the interface and the progress figure

**The Merge AC-3 track box asked for a number the track list never shows.** It was written into
the meta as it stood, and the meta wants the track number in the source file, while the only
numbers on screen are the row numbers of the list. Those are not the same once a Dolby Vision file
is loaded: its two layers take two rows and share one track, so every row below them is one ahead
of its track. Reading 4 off the AC-3 row named the track after it. The box is read as a row number
now, and it explains itself when hovered. Where there is no second video row the two are the same
number, so nothing changes for a file that has none.

**And the merged track was still written on a line of its own**, which the muxer refuses, so the
interface was producing a combination it had been told to reject. That line is left out now. The
row keeps its tick: it is still in the output, just inside the TrueHD track rather than beside it.

**The progress figure could never reach the end when a track was merged in.** Those bytes reach the
reader by a different path and were counted nowhere, so the bar stopped short by exactly the share
of the file that stream takes up. On a 60 GB file with a 339 MiB AC-3 track, 0.589 percent of it,
the bar stopped at 99.4 and stayed there while the mux finished normally. Nothing written to disk
changes: the same job through 2.18.0 and through this gives byte identical output.

**The enhancement layer of a Dolby Vision file was listed at the base layer's resolution.** A
profile 7 disc taken into a Matroska file keeps both layers in one track, which is shown as two
rows, and both rows were described from the base layer. So the enhancement layer read 3840x2160
where the disc it came from says 1920x1080, and a mismatched pair looked like a matched one. Only
the reported value was ever wrong, and that was checked before anything was changed: the layer
separates out at 1920x1080 and muxes back to a disc at 1920x1080, both matching the source. The
second row is described from the enhancement layer's own parameter sets now. Reported by a tester.

### Fixed: a mux that stopped for five minutes at 75 percent

Writing a Matroska file, the muxer held every packet in memory until every track had delivered one,
so that the codec readers were initialised before the track headers were written. Nothing bounded
that wait.

On a pressed disc whose third subtitle track has its first packet 74.99 percent of the way into a
72 GB title, that meant holding three quarters of the film in memory: 42 GB on a machine with 64 GB,
with the output file not created at all and not one byte written. It then spent 178 seconds writing
those packets out and 127 more freeing them. Nothing was read during either, so the progress figure
sat at 75.0 percent for five minutes and the program looked hung.

The percentage where it stops is not a threshold in the program. It is where that subtitle track
begins, so a different disc stops at a different number, and a machine with less memory stops
earlier still.

Subtitle tracks are no longer waited for. A subtitle track entry is complete before any packet
arrives, nothing in it is discovered by parsing, and it is the one kind of track that legitimately
carries nothing for a long stretch, because a forced subtitle track only has content in the scenes
that need it. Video and audio are still waited for, because their sample rate, channel count and
even their codec id are not known until the first frame.

The same title, the same meta file, before and after:

```
                    before          after
wall time           9 min 11 s      3 min 49 s
longest pause       305.4 s         1.0 s
peak memory         42.95 GB        0.054 GB
output              60,290,004,931 bytes, both
```

The output is unchanged. Five track combinations were muxed from the same source before and after:
identical file sizes, and identical block digests, every block of every track. A Matroska file
carries randomly generated identifiers, so two runs of any one build differ in a few bytes of the
header; the two builds differ in exactly those bytes and no others.

A track of another kind that started as late would still do this. That case needs a different
answer and has not been seen.

### Also

- **Where to get the library profile 8.1 needs is now written down.** It said only that a prebuilt
  one is published. The releases page it comes from carries the command line tool beside the
  library, under a more obvious name, and the release is numbered after the tool so the library
  inside has a different version on it. Somebody copied the tool in and it did not work, which is
  the mistake the page invites. The archive to take, the one file inside it and the two traps are
  named now, in the documentation in all three languages and in the refusal message itself.

### For anyone building from source

- A build can now be started by hand from the Actions tab against any branch, so someone testing a
  fix can be given a build without a release being published. The zips are the same, and no release
  is created.

---

## tsMuxeR 2.18.0

Most of this release is about output that was wrong, or data that was missing, **and the program
did not say so**. It finished, it said "Mux successful complete", and it returned success. Ten
separate faults did that.

If you only read one part of these notes, read the next one.

### A correction to 2.17.0, first

That release said that a dual layer Dolby Vision disc taken into a Matroska file and split back out gives a Blu-ray identical to the original. It was measured on one disc and then stated for all of them. On a disc that writes its start codes in the shorter form it was not true. The rebuilt video first differs 736 bytes in.

Only the framing differed. Every picture and every piece of Dolby Vision data came back exactly, even on that disc. Both forms are correct and play the same. So a disc made with 2.17.0 works, and nobody needs to rebuild anything.

The 2.17.0 entry is corrected in place, and so is the line in the README. It is repeated here because release notes get copied to places where they cannot be edited later. The claim now holds as it was written, and the entry below has the figures, measured on a whole film.

---

### If you used an earlier version, please check these files

These faults were silent. Your files may be affected and look fine.

**1. An ISO you built with `--keep-extra-files`.**
The last part of a file could be missing. The image opens, the file is listed at the right size,
and the end of it is gone. Only extra files you added yourself are affected. The disc's own video
files are never affected.

**2. A job where you asked for `--split-size=4GB` or larger.**
You got one big file instead of several parts. Splitting was turned off, and nothing said so.

**3. An AAC audio track you demuxed from a file with cover art.**
You may have got the picture instead of the sound. One test file gave 455,104 bytes where the
audio is only 194,146 bytes.

**4. A film you joined from three or more parts.**
If the first part ended exactly on a 2 MB boundary, everything after the second part was dropped.
In one test, 20,035,292 bytes went in and 2,097,152 bytes came out.

**5. A TrueHD track you merged with an AC-3 file, written to Matroska.**
The AC-3 sound was thrown away. The file looks normal and the AC-3 is simply not there.

---

### Fixed: your data was being lost, and nothing said so

**A joined film lost everything after the second part.**

When you join files, the reader moves to the next part when a read returns less data than asked
for. A part that ends exactly on a 2 MB boundary does not do that. It returns a full block, and
then it returns nothing. That took a different path, which opened the next part and then decided
the whole list was finished.

So any part shorter than 2 MB, or an empty part, ended the join. Every part after it was never
opened. Two parts always worked, which is why nobody noticed: you need a first part that ends on
the boundary and a third part before anything is lost.

Measured on three parts of one sound track: 20,035,292 bytes in, 2,097,152 bytes out, success
reported, no warning.

**A tag in the middle of an audio file destroyed a piece of the sound.**

A file made by sticking two audio files together has a tag where they join. The part of the program
that reads a frame of sound never checked that it was looking at one. Handed the first four bytes of
a tag, it read them as a frame 314 bytes long, wrote those 314 bytes into the output as if they were
sound, and landed inside the first real frame after the tag, which was then thrown away.

That is why a small tag did damage and a large one did not: with a large tag the imaginary frame
stayed inside it.

Measured on a clean recording with a tag pushed into the middle of it, twelve tag sizes from 10
bytes to 1024. Before, anything under 314 bytes cost a whole frame of sound, about a fortieth of a
second, and part of the tag came out in the audio. Now all twelve produce exactly what the same
recording produces with no tag at all, and none of them complains.

**And a tag that begins right at the edge of a read block was reported as lost.**

The program reads a file in 2 MB pieces. A tag whose first bytes fall at the end of one piece cannot
be recognised there, and by the time the next piece arrives the program has already read past the
start of it, so the whole tag was counted as lost data on a file whose sound is complete. Measured
on a 360,989 byte tag beginning five bytes before the boundary. It now carries those few bytes over
to the next piece.

**A picture in front of an AAC track was read as sound.**

Many audio files start with a tag that holds the cover art. The marker that starts an AAC frame is
only fourteen bits long and has no checksum, so random data matches it about once every sixteen
thousand bytes. Inside a picture, that random data is very common.

A file with 1.2 MB of cover art produced 455,104 bytes of "audio" where the real audio is 194,146
bytes. It started inside the picture. The same file could not be opened at all, and was refused
with "Can't detect stream type".

The reader may now distrust a marker **inside a tag it has read and checked**, and nowhere else.
That matters: refusing markers everywhere destroys real sound. One damaged byte cost 881 bytes of
good audio in a test, and a file with junk between its frames lost 193,449 of its 194,146 bytes.
Inside a tag there is no audio to lose, so it is safe there and only there.

**An AAC track lost one frame near a block boundary.**

The reader reads in 2 MB blocks. It used the same answer for "this is not a frame" and "I do not
have enough data yet to tell". So a frame header cut in half by the end of a block was treated as
rubbish and skipped.

Measured on a file whose audio starts six bytes before the end of the first block: 193,637 bytes
instead of 194,146. Exactly 509 bytes short, which is the length of the first frame.

The same fault also made these files very slow. Four test files took over two minutes. They now
take seventeen seconds.

**A metadata tag was written into demuxed MPEG audio.**

The tag at the start or end of an MP3 was copied into the audio file as if it were sound.

**The last part of a large file was written past the end of an ISO.**

A file that does not end exactly on a sector boundary keeps its last partial sector in a buffer and
writes it separately. The position for that write was measured from the start of the file, but
added to the start of the last piece of it. Those are the same place only while the file is in one
piece.

A file is only split into pieces past one gigabyte, or where a layer break guard splits it, which
is why this survived so long. The loud version of the fault was an image about twice its correct
size. **The quiet version is the one that matters**: those last bytes went into the padding at the
end of the image, and the sector they belonged in stayed empty. The image opens. Every file inside
reads at its correct size. The end of the file is gone.

Tested with a file whose every byte can be checked, because a file full of zeros cannot tell a hole
from correct data. Before: a 2.3 GB file read back off the image differs from the original, and its
last 4096 bytes differ. After: identical.

**Asking for a 4 GB split turned splitting off.**

The split size was held in a 32 bit number. 4 GB does not fit, so it became zero, and zero meant
"do not split". 4.5 GB became 205 MB parts.

There were two faults, not one: the running total overflowed as well. Proven by building a single
part of 4,405,683,744 bytes, which the old arithmetic could not even express.

A size of zero or less is now refused, instead of quietly switching the feature off.

**A merged AC-3 sound track was thrown away in Matroska.**

A Blu-ray carries TrueHD sound as the lossless stream **plus** a normal AC-3 version, so that a
player which cannot decode TrueHD still has sound. Matroska cannot hold both in one track, so this
program writes the AC-3 as its own track.

`merge-ac3-track` and `merge-ac3-file` build the same pair from two separate sources. They use a
different reader, so the test that asks "is there an AC-3 version here" said no, and every AC-3
frame was dropped.

Measured with a 4,261,662 byte TrueHD stream and a 1,120,000 byte AC-3 file:

- before: 4,397,657 bytes out, one track. That is **the same size, to the byte**, as the TrueHD
  muxed with no AC-3 file at all
- after: 5,514,890 bytes out, two tracks

The loss happened twice over. Turning that file back into a disc gave 4,323,248 bytes instead of
7,212,432, and said nothing then either.

Both reference tools write the same shape, and the AC-3 track now decodes identically to the one
another tool produces, over all 953,856 samples it holds.

**File names on an ISO were measured in the wrong units, and names were cut short.**

An ISO stores a file name either as one byte per letter, or as two bytes per letter when the name
needs letters that do not fit in one. The writer worked out how much room to leave from the name's
size in UTF-8, which is a different number, and four things went wrong because of it.

A Cyrillic or Greek name was cut short, because each plain letter in it takes one byte in UTF-8 and
two on the image. Three Cyrillic letters in front of `_document_v1.txt` left eleven letters of
nineteen. A `v2` file beside it was cut at the same place, so both files ended up with the same
name and one of the two was lost. The mux said it was complete.

A Japanese or Chinese name was cut in the middle of a letter, leaving half a character at the end.

A name like `cafe.txt` written with an accent got an invisible zero glued to the end of it.

And a name too long for the field wrapped round instead of being refused. A 150 letter Greek name
made an image that no program will open at all. 7-Zip answers "Cannot open the file as archive",
and the mux still said it was complete.

The name is now written first and measured afterwards, so the two always agree. A name that is
still too long is cut at a whole letter and the log says which name it was and how much of it went
on the image.

An image whose names are all plain English letters is exactly the same as before.

**An ISO with more than 4093 files and folders wrecked itself, and said nothing at all.**

An ISO keeps a small table with one line per file and per folder. The room for that table was a
fixed 64 KB, which holds 4093 lines and not one more, and nothing checked. So line 4094 was written
over memory that belonged to something else.

The program then died with no message on screen at all, and left an image behind that no program
will open. Counted exactly: 4068 extra files beside a BDMV folder is 4093 lines and works, 4069 is
4094 and dies.

The table is now made as big as it needs to be, and every part of the ISO writer refuses to write
past the end of its own space instead of writing over something else.

A folder of 6000 files now builds, and all 6000 come back out with the right names and the right
contents. 20,000 works too.

**And an ISO that was too big for its own file table said "complete" instead of saying no.**

Above about 23,000 files the ISO writer runs out of a different kind of room, and it has always had
a message for that. The message never appeared, because the writer only finished its work when it
was being thrown away, and a program cannot report a problem at that moment. It either stopped dead
with nothing said, or claimed success over an unusable image.

The image is now finished before the program reports anything, so the message arrives, and it names
the numbers.

**A Dolby Vision disc listed the wrong way round lost its Dolby Vision without a word.**

A Dolby Vision disc carries the picture on two video streams: a large base layer, and a smaller
enhancement layer that holds the Dolby Vision data. They have to be listed in that order. The
program worked out which was which from the order alone, so listing them the other way round meant
the two were never joined.

Without `--dv-profile` the mux said it worked. What came out was two separate tracks with no Dolby
Vision at all, and nothing in the log said so. With `--dv-profile=8.1` it was refused, and the
refusal said that both video streams must be listed, when both were listed.

The program now recognises the wrong order and says so plainly, and the message tells you to swap
the two video lines. Files that were already right are exactly the same as before, including a disc
with a picture in picture stream beside the main video.

**A file at the root of an ISO was marked as system data.**

A file placed at the top level of an image built with `--keep-extra-files` was marked as UDF
metadata and would not open. Files inside a folder were fine, which is why this was not noticed.
Reported by @DreckSoft.

**The last picture of a stream with no end marker was lost.**

On HEVC the last picture went missing. On H.264 it was written twice. One cause, in code that six readers share. Counted from the video itself rather than from the muxer's own report, and checked on a real 3D file as well as on plain streams.

**The last frame of the AC-3 core was dropped by every merge since the feature existed.**

The tolerance in the coverage warning was worked out again with it, so a file that is now complete is not reported as short.

**An AV1 file on its own lost the start of the file and counted no frames at all.**

An AV1 file on disk is a plain chain of blocks, each one carrying its own length. It has no start
markers. tsMuxeR handles AV1 inside itself the way it handles H.264 and HEVC, which do have start
markers, and the reader looked for them in the file.

There are none. So it threw away everything up to the first three bytes that looked like a start
marker by accident, found no block boundary after that, and wrote the whole rest of the file as one
packet. Then it said "Processed 0 video frames" next to "Mux successful complete".

Measured on a file of 51,960 bytes holding 50 pictures: the first accidental marker sits 10,992
bytes in, so 10,992 bytes were dropped, one packet came out instead of fifty, and the count was
zero. AV1 inside a container was never affected.

The reader now works out how the file is laid out and reads it either way. The same file gives 50
pictures and 50 packets, a 66.8 MB file gives its 3,000, and the pictures that come back out are
the same ones that went in, to the last pixel. A file that stops in the middle of a block, or whose
blocks do not follow on from each other, now says so and gives the number of bytes it skipped.
tsMuxeR can also recognise these files now, where it used to answer "Can't detect stream type".

**And the other way round: `--demux` wrote an AV1 file that no other program will open.**

The same difference, from the other side. When you pull an AV1 track out of a disc or a transport
stream, tsMuxeR wrote the file in its own internal arrangement with the start markers still in it.
ffmpeg refuses such a file outright, and refuses it even when told to expect AV1, while it reads a
file from any encoder without complaint. The mux said it was complete.

It writes the real thing now. There was a second half to it: an AV1 file is a chain of picture
groups and each one has to begin with a small marker of its own, with nothing in front of the very
first. A transport stream carries a header ahead of that marker, so writing the pieces out in the
order they arrive still produced a file ffmpeg would not open. That header is now written just
after the marker instead, so nothing is dropped.

Measured: ffmpeg reads the file now and gets all 50 pictures, and the pictures that come out are
the same ones that went in. Take away the one header the transport stream adds and the file is
byte for byte the encoder's original. Every other kind of track comes out of a demux exactly as
before.

---

### Fixed: a refusal that destroyed the file it refused to write

Opening a file for writing empties it at once. The program created the output file **before** it
knew the job could run. So when it then refused, your existing file at that path was already gone.

**A refused image.**

Measured: 2,490,368 bytes of a good disc image replaced by a 1,769,472 byte stub.

**A rate the program cannot use.**

A muxing rate of 90 kbps or below made the program **write until the disk was full**. 89 kbps
turned a two second, 258,500 byte job into 23.9 GB. Each one reported success. The range is refused
now, and nothing is written before the refusal.

**A refused Matroska file.**

The Dolby Vision check that decides this needs the video to be read first, so it cannot run before
the file is opened. An earlier fix moved a simpler check earlier, but that check only counts video
tracks, so two video tracks passed it and the file was emptied anyway.

The output file is now created only after the real check has passed. Tested with a 96,002 byte file
at the output path, across ten different refusals: **all ten now leave it untouched**. Two of them
used to leave a 308 byte stub.

A bad output path is still reported at once, before any reading starts.

---

**Setting the frame rate on an HEVC track read one part of the stream as if it were another.**

Two kinds of header carry the frame rate, and the code that writes a new one was handed either of
them but always treated it as the first kind. The two are unrelated, so on the second kind it read
a number out of the wrong place in memory. Running the same file through the same program eight
times gave three different answers.

Most of the time that number came out as zero, and the program said "cannot override FPS in stream"
and changed nothing. Occasionally it came out as something else, said nothing, and wrote four bytes
at whatever position that number pointed at.

That is fixed: only the header that can carry a new frame rate is written to, and the other is left
alone as it always should have been. The same file now gives the same answer every time, and the
warnings that were appearing for no reason are gone.

**And the gap it exposed is closed too: `fps=` now reaches a stream that keeps its timing in the
second header.** Nothing had ever written a frame rate there, so a file that declares its rate only
in that header kept the old one no matter what you asked for. Eight bytes change and the rest of the
header does not; the decoded picture is identical, because all that changes is what the file says
about its own rate.

### Fixed: crashes

**An option written with a space.**

Writing an option with a space instead of an equals sign, for example `--split-size 4GB`, read past
the end of a list and could crash.

**A joined file on a merge-ac3 line.**

A line that joins two files and also merges an AC-3 file walked past its safety check and crashed,
after printing "Mux successful complete". The check compared the name that was typed, not the file
the line really opens, so a joined line went straight past it.

**A crash and a half core, both around the AC-3 core merge.**

A merged track that was read back could produce only part of its AC-3 core, with no warning. That is
worse than the crash beside it, because the file looks finished.

---

### Fixed: the program stayed quiet when it should have spoken

**A track that lost data now says so.**

If a track loses data on the way through, you now get a warning with the number of bytes, instead
of success and silence.

Most of the work here is in what it does **not** report, because a warning on a perfectly good file
is worse than no warning. A metadata tag is not lost sound. Counting one as lost put a warning on
ordinary tagged music: two joined MP3s reported 4,239 bytes lost on a file that is identical to the
same join without tags.

So each kind of tag is now read properly and checked, not simply believed.

**A split that leaves parts you cannot play.**

H.264 video carries its setup data once, at the start. When you split the output, every part after
the first refers to setup data that is not in it.

Measured on an 11 MB file split at 3 MB, and decoded part by part: part 1 gave 203 frames, and
parts 2, 3 and 4 all failed. **On a Blu-ray that is three unplayable clips out of four**, written
while the program reported success.

The option that fixes this already existed. What was missing was anything telling you that this job
needed it. It now warns and names the option. No output byte changes.

**Splitting was ignored for Matroska.**

The same job produced nine parts as a transport stream and one 421 MB file as Matroska, both
reporting success. It now says so, and the interface greys the split controls for Matroska.

**A merge that produced no AC-3 core at all said nothing about it.**

The warning that reports a short core opened by excluding zero, so that an average below it would not divide by zero. That left the worst outcome as the only silent one. Measured on a Matroska source, the same line differing only in the track number: a track the file holds gives 7,212,432 bytes and names the track the core came from, and a track it does not hold gives 4,323,248 bytes, no core, no warning and exit 0. That is 2,889,184 bytes of compatibility audio gone in silence, and the lost data report cannot catch it either, because a track that is not in the file contributes no bytes read and so has nothing to charge. It is easy to reach: a remuxed file does not keep the track numbers of the file it was made from, so a meta copied from the original can name a track that is not there. It now says so, and says what to check.

**`--dv-profile` was accepted and ignored on every output but Matroska.**

The option is read by the Matroska muxer and by nothing else. On a disc, a transport stream or a demux it was accepted, did nothing, and said nothing. Measured on the same source twice, differing only in whether the option is present: both files have md5 65c5268e3c4c034aad92b6e898998368. It warns now, and names the output it is talking about. A warning and not a refusal, because on those outputs the option is not wrong so much as meaningless. A disc and a transport stream carry the two layers as two streams, the way the source disc does, so there is nothing to fold and nothing to convert.

**Asking for profile 8.1 with nothing to convert.**

This did nothing at all and said nothing. It is now refused, with the reason.

**The bitrate options did not match their help text.**

Three separate problems. `--maxbitrate` was described as a limit and is not one: a 1 Mbps ceiling on
a 30.77 Mbps track gives an identical file. `--bitrate` was documented in Mbps but is read as kbps,
so the example in the help was wrong by a factor of a thousand. And `--cbr` and `--vbr` were read by
nothing at all.

All three are corrected, and three combinations that cannot mean what they look like now say so.
No output byte changes.

---

**The Linux download had no GUI in it, and nothing said so.**

Every Linux release contained the command line tool on its own. The step that builds the download
looked for the interface under a name with different capital letters from the one it is built with,
found nothing, and quietly packed the zip without it. Windows was never affected, because Windows
does not care about capital letters in a file name.

It is fixed, and it can no longer happen quietly: if the interface is missing the build now stops
instead of publishing half a release. Reported by @Frankaboy7.

### New

**A disc rebuilt from a Matroska file is now identical to the original.**

A start code is the marker between pieces of video. It can be three bytes or four. Both are legal
and both decode the same, but discs differ, and Matroska does not store start codes at all. So a
disc rebuilt through Matroska always came out in the four byte form.

The program now notes which form the source used, for each kind of piece, and writes it back the
same way. A whole 73 GB film now comes back **byte for byte identical** on both video layers,
through both Dolby Vision profiles.

This is not only a Dolby Vision matter. Any single layer disc taken to Matroska and back now keeps
its original form too.

**Dolby Vision profile 8.1, with the disc still rebuildable.**

A dual layer Dolby Vision disc can now be stored as single layer profile 8.1, which many more
devices accept, and still be turned back into the disc it came from.

The conversion cannot be reversed by calculation, so the disc's own data travels with the file as
an attachment and is put back when the file is split again. Everything the file promises is checked
before a single picture is written.

**Please note: this does not make the file smaller.** The second layer is kept so that the disc can
be rebuilt. That is the point of it.

**Profile 8.1 needs libdovi**, which is not part of tsMuxeR and is not in the downloads. On 64 bit
Windows put `dovi.dll` beside `tsMuxeR.exe`; on Linux and macOS build it, or install a package if
your distribution has one. Without it `--dv-profile=8.1` is refused before muxing starts rather
than failing part way through a feature, and the message names the library. The table in
`docs/DOLBY_VISION.md` has the detail, in English, German and Japanese.

**The index is now written at the front of the file as well.**

The index has always been written after the video, because it is not known until the end. Nothing
at the front pointed to it. That is survivable for an index and not survivable for an attachment:
other software did not list the attachments at all, and copying such a file produced one with the
video intact and the attachments quietly gone.

**An image is now checked against the disc you are burning to.**

Writing a folder to an ISO never checked whether the result would fit. It now refuses before
writing anything when the content already cannot fit, warns when the answer is close, and measures
the finished image before the run ends. `--allow-oversize` turns both into warnings.

Making that check also found `--inner-only` going over the disc by 1.44 MB in silence. It now lands
22.56 MB inside it.

**Smaller additions.**

- A merged Dolby Vision track is listed as its two layers, so you can separate it in the interface
  instead of writing a meta file by hand.
- An E-AC-3 track with Atmos now says which kind it is and how many objects it holds.
- The H.264 reader no longer assumes a four byte start code. It read the type from a fixed place, which on a short framed stream is one byte inside the header, so the type came out wrong and the muxer added a second delimiter. Checked on a real stream reframed to three bytes: 841 delimiters in, 841 out.
- The stream notes attachment has an honest name now. A Matroska file made from a source with short start codes carries a small text file recording how that source was framed. It used to be called dv-manifest.txt, which says Dolby Vision, and that name turned up on files with no Dolby Vision in them at all. The file's own first line already said "tsMuxeR stream notes", so it is tsmuxer-manifest.txt now.

---

### Interface

- A Dolby Vision selector beside the output format. It is hidden, not greyed, unless it applies.
- The selector used to do nothing at all. Choosing profile 8.1 moved the control and changed
  nothing else.
- The Blu-ray tab took no notice of the chosen output. 23 controls, and not one of them changed.
  Nothing was ever muxed wrongly; the tab was simply saying something untrue about itself.
- The Merge AC-3 file box threw away what you typed into it.
- The Max bitrate box started at 99.99 kbps, which is a real limit of 99,990 bits per second. It is
  48000 now, which gives a file identical to not using the box at all.
- The bitrate boxes could ask for a rate the program refuses.
- The Dolby Vision row appeared on a freshly opened window with nothing loaded.
- The add, join and remove buttons and the file list now have help text. The join button explains
  what it is for, which is a film split across two discs.

---

### Known limits

These are real and they are not fixed. They are listed so you know where you stand.

- A dual layer Dolby Vision pair listed with the **enhancement layer first** is still not joined
  automatically. It now says so and tells you to swap the two lines, instead of saying something
  untrue or nothing at all. Listing the base layer first works.
- The lost data warning does **not** report data lost at the very start or the very end of a track.
- A companion file longer than the track it is merged into is counted in the total, although the
  extra could never have been used.
- Splitting is not supported for Matroska output. The program now says so instead of ignoring it.
- The tag checks defend against damage, which is what happens in practice. They do not defend
  against a deliberately crafted tag.
- An ISO with more than about **23,000 files and folders** is refused, because the space set aside
  for the file table is worked out from the file count alone and does not allow for the folder
  listings. The refusal names the numbers and `--extra-iso-space` makes the space bigger. An
  ordinary disc holds a few dozen files.

---

### For anyone building from source

The version line named the commit the build folder was **configured** with, not the one that was
built. A local build here reported a commit 24 behind the code it contained. Releases were never
affected. It now updates when the code moves.

## tsMuxeR 2.17.0

- The layer-break guard beside the normal Blu-ray output was placed at the BD-R DL break whatever disc was selected. The disc list there was a bare BD25/BD50/BD100/BD128 with no capacities behind it, and the break sectors were never passed to the muxer, so it fell back to half a 50 GB disc: on a 100 or 128 GB disc that sector is not a layer boundary at all, so the guard was placed at the wrong sector. Both parts of the interface now share one disc table, the one the BDMV to ISO tab uses, so the break is computed from the disc's real Free Sectors and the group shows where the guard will land before the mux runs.
- New "Also fill before the break" beside the normal Blu-ray output, matching the option the BDMV to ISO tab already had, for media that are weak just before the layer transition as well as after it. The muxer now accepts --layer-break-guard-before on an ordinary mux and not only on --bdmv-to-iso.
- The AC-3 core track of a TrueHD track is now written directly after the track it came from, rather than after every other stream in the file. That is where a disc remux normally carries it and where other muxers put it, so a file made here has the same track order as one made from the same disc elsewhere.
- The BDMV to ISO disc list now offers a 100 GB disc used across its first two layers only, and shows every disc's Free Sectors in its own entry. There is no 66 GB BD-R and many players cope with two layers better than three, so two thirds of a 100 GB disc is a common compromise; picking it puts the one break it needs on the disc's real first layer boundary and leaves the third layer unwritten, and the fit estimate, the layer-fit placement and --inner-only all treat the disc as 66 GB. It is still BD-R XL media, so the player-compatibility warning now follows the media rather than the layer count and still appears. Requested by @DreckSoft.
- The pre-filled Free Sectors for BD-R XL was the defect-managed capacity, which is not what a plain BD-R XL reports. The same disc size exists at two capacities, a defect-managed disc holding 1 GiB per layer back as spare area, and on a 100 GB disc that is 524,288 sectors: the calculated break lands a full GiB away from the real one, far outside any guard band, so the guard misses the break entirely. Both capacities have been measured here on real discs, so the entries now carry the full capacity, every entry shows its number so it can be matched against ImgBurn at a glance, and the difference and the defect-managed figures are spelled out where the value is chosen. A disc whose figure differs is still entered by hand, as before. Reported by @DreckSoft.

- A dual layer Dolby Vision disc is now carried in ONE Matroska track. Such a disc puts the picture on two video streams, a base layer plus a quarter resolution enhancement layer holding the Dolby Vision data, and Matroska needs both in a single track; until now a disc source produced two tracks and no player engaged Dolby Vision. Every enhancement layer NAL is wrapped in an unspecified NAL of type 63, which a decoder that knows nothing about Dolby Vision skips, and the RPU travels unchanged. The track carries the Dolby Vision configuration record describing profile 7 with base layer, enhancement layer and RPU all present, and beside it the enhancement layer's own HEVC configuration record. Nothing is re-encoded, re-ordered or re-escaped. Verified on a two hour feature, 166,928 frames all merged, against a file an independent muxer produced from the same disc: 166,927 of 166,928 blocks are byte identical, and the one that differs is the last, where the disc carries an end of stream marker on both layers that we keep and the other drops.
- The reverse direction as well: a merged track can be split back into its two layers for a disc, through the existing subTrack= mechanism, with the base layer on one PID and the enhancement layer on the other. A single layer file has no enhancement layer to separate and is refused with an explanation rather than producing an empty second track. CORRECTED AFTER RELEASE, see the 2.18.0 notes: this entry also said that two disc layers merged into one Matroska track and split again produce a Blu-ray byte identical to authoring it straight from those layers. That was not measured on every disc and it was not true on all of them. It held where a disc frames its start codes the way this muxer writes them, and not on a disc that uses the shorter conformant form, where the rebuilt stream first differs a few hundred bytes in. Every picture and every piece of Dolby Vision metadata did come back exactly even there, and the difference is legal either way and decodes identically, so no disc authored with 2.17.0 is defective. 2.18.0 carries the source framing through and the unqualified claim holds from that release.
- Fixed Matroska output keeping only the AC-3 core of a TrueHD track and throwing the lossless stream away. The result was a 448 kbps track labelled AC-3, with no warning and a successful mux, while the same source muxed to m2ts was intact. TrueHD detection only ever runs while probing, and the instance that does the muxing re-ran that probe on the Blu-ray path alone, so on every other output the reader believed it was plain AC-3 from beginning to end. The lossless stream is now carried and the track is declared A_TRUEHD, with the AC-3 core kept beside it on a track of its own (see below). Anyone who wants only the core can still ask for it with down-to-ac3. The other audio codecs were measured for the same fault rather than assumed safe: DTS-HD MA, DTS, AC-3 and E-AC3 all carry their full payload.
- The AC-3 core of a TrueHD track is no longer lost on the way into Matroska. A Blu-ray carries the lossless stream and a 448 kbps AC-3 core braided onto one PID, so a player that cannot decode the lossless part still has something to play; Matroska has no such arrangement, because a TrueHD track there holds the lossless stream alone. The core is therefore written as an ordinary AC-3 track beside it, which is the layout a disc remux normally has, and merge-ac3-track= braids the two back together when a disc is authored from that file. Without it the disc had no core, which the spec does not allow, and the audio could not be recovered from anywhere. Verified as a round trip from a pressed disc: the file comes back with the lossless track and a 448 kbps 5.1 track, the disc built from it reports "AC3 core + TRUE-HD + ATMOS" again, and the warning that MLP is not standard for BD disks is gone. The added track measures 449.4 kbps against the core's 448, the difference being one block header per frame, so it carries the core and nothing besides. Add drop-ac3-core to a track to leave it out.
- A DTS:X track using the IMAX variant of the marker is now named "+ DTS:X IMAX" instead of being shown identically to a plain DTS:X track. Measured over every frame of two tracks from pressed discs, 690,198 and 661,703 frames, all carrying the variant and none the plain marker. It names the audio encoding and nothing else: it does not say the title is an IMAX Enhanced presentation, which is a remaster and an expanded aspect ratio rather than anything in the bitstream.
- The level of an H.265/HEVC stream can now be changed without re-encoding, through the same level= track option the H.264 path has always had. general_level_idc is a single byte carried in both the VPS and the SPS, so raising it touches no picture data at all; a higher level is a superset of a lower one, so a stream that was conformant stays conformant. Until now the only way to alter that one number was to re-encode the entire track, which on a 4K feature is a full pass over the largest asset on the disc and a quality loss for a value in a header. Two things make it less trivial than it sounds and both are handled: the reserved bits immediately before the level are zeros, so emulation prevention bytes land in that exact region and the position has to be found by parsing rather than by a fixed offset; and the byte is written directly rather than through the general bit writer, whose safety margin refuses to reach a field 17 bytes into a 22 byte VPS and would otherwise have left the SPS updated and the VPS silently unchanged. Verified on a 3840x2160 Main10 stream: the output is byte identical apart from those single bytes, and asking for the level the stream already has produces a byte identical file and no log line.
- Access unit delimiters are now written for HEVC when a source has none, which the H.264 path has done for years. They are optional in HEVC itself, where the standard only requires that one come first in its access unit if present, but on a disc they are not optional in practice: measured across two pressed UHD titles, both video layers of each, every one carries exactly one per picture without exception. A source that omits them was previously authored to a disc without any. The pic_type written declares the set of slice types the picture may contain, so it is derived from the slices actually present rather than assumed; announcing that a picture may hold B slices when it holds only I slices would be wrong. Verified over 741 access units, every value correct, and a source that already carries delimiters is untouched.
- The HDR mastering display and content light level are now repeated at every random access point when authoring a Blu-ray. A pressed disc carries both at every one; material that states them once, at the head of the stream, leaves a player with no HDR10 metadata at all after a seek. Dolby Vision hides the problem, because its own metadata travels with every picture, which is why it is easy to miss. Nothing is fabricated: the bytes written are the stream's own, captured when they first went past, and a source that already repeats them gains nothing. They are inserted immediately before the first slice, by which point every other part of the access unit has been seen, so what the source already provided is known rather than guessed, and the order the standard requires is preserved. Applies to Blu-ray output only.

## tsMuxeR 2.16.0

- Matroska output was losing almost everything except the picture and the sound. Language was never written at all, and since a Matroska file that declares no language is English by definition, every track in every file tsMuxeR had ever written claimed to be English. Chapters were read, listed and then discarded, which loses the 200 to 250 marks of a feature playlist. Dolby Digital Plus was labelled as plain AC-3, and TrueHD as plain AC-3 as well, so a player that trusted the label tried to decode the wrong thing. HDR10 colour, the mastering display values and MaxCLL and MaxFALL were read for the disc path and thrown away on the Matroska one, so an HDR10 source came back as an untagged file. All of it is written now.
- A language the source container already knows is carried over without needing an explicit lang=, in both directions, so a disc authored from a named Matroska track now names it on the disc as well. An explicit lang= still wins, and demuxed file names are unaffected.
- New track-name= on a track, and "default" now takes effect on Matroska output. Without an explicit flag Matroska treats every track as a default track, so a file with two audio tracks declared both of them default. Exactly one track per type is marked now, and the interface can choose which one: its default audio and default subtitle controls previously reached the meta file for disc output only, so on an .mkv the checkbox was present, was settable, and silently did nothing, leaving every player to take the first track.
- Fixed a mux with two video tracks describing itself by whichever video the meta file happened to list last, because only one stream advanced the running duration and that duration becomes the playlist end. On a Dolby Vision disc the second track is the enhancement layer, and the natural listing order puts it second, so the disc claimed the enhancement layer's length and every chapter mark past that point was dropped as out of range, while the feature itself was written in full. Reported by @axledentaldj against 2.15.0. The main stream is now chosen by what the track is, a primary video outranking a secondary or an enhancement layer, rather than by where it sits in the file, and a title runs as long as its primary video does. Reproduced and fixed against a pressed dual layer Dolby Vision disc, where the natural order previously gave 24.8 seconds against a 60 second base layer. A mux with a single video track is byte for byte identical.
- Fixed the file list of a playlist built from several sections showing the second entry with two file names run together, the third with three, and so on. On a 57 section playlist from a pressed disc the last line ran to 602 characters naming 57 files. The GUI builds its file list from those lines.
- A DTS:X track is now named. It used to be shown exactly like a plain DTS-HD Master Audio track, because nothing in the extension substream header says DTS:X: the marker sits at the very end of the XLL band data and is only reachable by walking the asset descriptor, the XLL common header, the channel set sub-headers and the navigation table. A track carrying it now prints " + DTS:X" beside "DTS Master Audio", in the same place a TrueHD track prints "+ ATMOS". Confirmed by walking every frame of all three DTS tracks of a pressed disc: 857,001 of 857,001 frames of the 7.1 track carry the marker and neither 5.1 track carries it. A second marker value, used by some material and matched under a mask because its low nibble is not part of the sync word, rests on two short samples rather than a full disc. This is labelling only; the audio is untouched and produced files are byte-identical.
- Dolby Vision in Matroska, for sources with a SINGLE picture track only. Such a file now carries the configuration record a television reads to recognise Dolby Vision; without it the file played as HDR10 even though the data was there. That covers streaming material, profiles 5 and 8, and it was confirmed on hardware. A DISC IS NOT COVERED AND NOW DELIBERATELY CLAIMS NOTHING: a Dolby Vision disc carries the picture on two tracks, a base layer plus a quarter resolution enhancement layer holding the Dolby Vision data, and Matroska needs the two merged into one track. That merge is not built yet, so the record is not written when a second picture track is present, and the mux says so in its log. Writing it there would leave a 1080p enhancement layer announcing itself as complete Dolby Vision when it is neither complete nor playable on its own. A disc source produces a plain HDR10 file.
- Fixed a track delay of exactly the length of an open GOP's lead-in being reported as zero and then lost on a demux and remux. "Stream delay" was measured against the smallest timestamp on the stream rather than the first one, and on an open GOP the leading B pictures are presented before the first coded picture. Found on AVCHD clips from a Sony ZV-1, whose audio runs 80 ms ahead of the picture; clips from a Panasonic Lumix DMC-TZ110 never reproduced it because their first coded picture is also their first displayed one. Nothing about it is specific to AVCHD; any open-GOP source is affected.
- Fixed the reported and written picture height on sources that pad the coded picture to a round number of rows and mark the extra ones as not for display: a 2160 row picture was described as 2176, and a 1634 row one as 1664. The conformance window is now applied. Discs were always correct and are unchanged. VVC is deliberately untouched, as no test material was available for it.
- Fixed the Dolby Vision level written into a disc being one step too high for material at exactly 24.000 frames per second, caused by a rounding error putting the computed pixel rate three units past a boundary that is defined as an exact rate. Players that support the higher level were unaffected, which is why it went unnoticed, and 23.976 material was never affected.
- A mistyped per-track option in a meta file used to be accepted in silence, so langauge= instead of lang= did nothing and gave no clue why. Unknown option names now produce a warning and the mux continues, so existing meta files keep working.
- --bdmv-to-iso listed nine options in its usage error and only three in --help. The six that were missing (--keep-extra-files, --inner-only, --original-order, --no-layer-fit, --disc-capacity, --label) are now documented, each described from what it does rather than from its name. The GUI already offered them, so only command line users were short of the information.

## tsMuxeR 2.15.0

- Fixed the mastering-display luminance written into a UHD Blu-ray playlist having its maximum and minimum swapped. A disc mastered at 4000 nits was described to the player as 610 nits with a black level of 2.3040 instead of 0.0050. This affects every UHD Blu-ray tsMuxeR has authored from an HDR10 source, not just recent versions. A disc authored with this build now carries the same value as a pressed reference disc. The picture primaries, white point, MaxCLL and MaxFALL were always correct.
- The delay of a demuxed audio track is now recorded in its file name, using the long established DELAY convention: "00294.track_4353 DELAY -17ms.ac3". Muxing a file named that way applies the delay again. A raw elementary stream has nowhere to keep a timestamp, so a track that was offset against the video used to come back at zero after a demux and remux, which is a real loss of sync on discs that use one. An explicit timeshift= on the meta line still wins, and the demuxed audio itself is unchanged.
- Fixed a stream parsing fault that could make a whole H.264 track disappear on the Linux and macOS builds. Two values were read from the bitstream in an order the compiler was free to choose, and reversing them inverted a check that rejects the stream. The Windows builds were never affected.
- down-to-dts on a track with no DTS core, which is every DTS Express stream, wrote an empty file and reported success. In a mux with other tracks it left an entry in the stream table with nothing behind it. It now reports that there is no core to keep. DTS-HD Master Audio and High Resolution tracks are unaffected.
- down-to-ac3 on an A_MLP track was accepted and quietly did nothing. A standalone TrueHD stream has no AC-3 core to keep and tsMuxeR does not encode audio, so it now says so and names the two things that do work, while still muxing the track as before.
- Fixed LPCM tracks differing from their source by one byte. The first frame of every output set a flag that no pressed disc sets. A 16 bit stereo and a 24 bit 5.1 track now reproduce their source disc byte for byte.
- Tracks that cannot be muxed are now named in the interface as well as on the command line. Opening a Blu-ray stream that holds only the disc menu overlay used to add a blank row to the track list with no explanation, and a file containing nothing else reported that its stream type could not be detected, which was not true. The description is translated in all eight interface languages.

## tsMuxeR 2.14.0

- The mux start time is now guarded for Blu-ray outputs (#5, requested by oniiz86 based on Emulgator's doom9 analysis). Muxing has always defaulted to a safe 600 seconds when the start time is left alone, but an explicitly entered value below 524280 ticks of the 45 kHz clock (11.65 seconds) produces a disc on which players cannot navigate back to the start of the stream: they subtract the decoder preroll from timestamps near zero and their 32-bit 45 kHz registers underflow. Twelve commercial discs were measured (BD and UHD, three studios): none starts below 524280, and one studio masters at exactly that value. Lower values are now raised to 524280 with a log message, in the muxer and in the GUI field alike. This applies to the outputs that actually carry the offset, so plain *.ts, Matroska and demux mode are left alone. The 600 second default is unchanged, so existing workflows produce identical results.
- Fixed the TrueHD + AC-3 core merge writing its audio packets with PES stream id 0xBD instead of 0xFD. Blu-ray players route HD audio by the extended id 0xFD plus the extension byte, so strict hardware could fail to pick up a merged track. Plain TrueHD without a core keeps the private-stream id 0xBD it has always used, since it carries no such extension.
- Fixed the TrueHD + AC-3 core merge losing core frames on longer muxes. The merge read its side data misaligned by a reader buffer pad, so each delivered block began with pad garbage and lost its real tail: with merge-ac3-track the core silently stopped a few minutes in, and with merge-ac3-file about 1.6 percent of the core went missing. A full-length UHD test now delivers the complete core to the last frame (195,887 of 195,887 expected AC-3 frames on a 104 minute movie). The result was checked against a pressed disc rather than against earlier output: taking a commercial UHD that carries TrueHD with its AC-3 core natively, splitting the two apart and merging them back together reproduces the disc's own core bit for bit.
- Fixed TrueHD audio being silently thrown away when muxing from a raw TrueHD (.thd) elementary stream. Whenever an audio unit straddled the end of one of the muxer's 2 MiB input blocks, the reader could not tell "I need the rest of this unit" from "this unit is broken", so it reported a bad frame, skipped to the next sync point and discarded everything in between, leaving only a "bad frame detected ... Resync stream" line in the log. On a 20 MiB test slice that silently dropped 53,764 bytes of audio in two places, and the losses repeat about every 2 MiB. The reader now asks for the remaining data instead, and the same input comes back out byte for byte identical. This is the case you get from the usual demux-then-remux workflow.
- Fixed a start code scanner defect that could mis-detect frame boundaries on builds without SSE2, which includes the macOS arm64 binary published with each release. A payload byte 0x01 immediately before a four byte start code could make the scanner return a pointer one byte early, affecting every video codec. Builds with SSE2, which covers all Windows binaries, were never affected.
- Fixed the last frame of a down-converted DTS track carrying a leftover DTS-HD block. When a DTS-HD Master Audio or High Resolution track is stripped down to its DTS core, the final frame was written out with its HD extension still attached, because a stream ending exactly on a buffer boundary was mistaken for one that still had data to come. Muxing DTS tracks without down-converting was never affected.
- Down-converting a Dolby Digital Plus track that has no AC-3 core now fails with a clear message instead of writing an empty file and reporting success. Extracting the core works by keeping the AC-3 frames and discarding the E-AC-3 ones, so a DD+ stream with no core at all, which is what streaming sources normally provide, had nothing left to keep.
- Fixed a crash ("Bitstream exception") that could abort a mux, or an analyze run, at a fixed position in some HEVC streams. One malformed metadata (SEI) unit was enough to stop the whole run even though that data is only analyzed, never modified. Such units are now skipped with a warning.
- Dolby Vision is now reported against the track that actually carries it. On a dual layer Dolby Vision disc the two HEVC tracks looked identical apart from their resolution, with nothing to say which one held the Dolby Vision data. The track listing now marks the stream carrying the Dolby Vision RPU, while the base layer stays plain HDR10.
- Tracks that cannot be muxed are now named instead of reported as a blank failure. A Blu-ray stream containing an interactive menu used to print only "Can't detect stream type" against that track. It now says that the track is an Interactive Graphics stream, the disc's menu overlay, that muxing this stream type is not implemented, and that the track is skipped. Text subtitle tracks are named the same way, and any other unrecognised track from a transport stream at least reports the stream type the container declared. The GUI track list is unchanged: unsupported streams are still not offered for muxing.
- GUI: the Dual-layer (BD-R/RE DL) controls now have explanatory tooltips, translated in all eight interface languages, and they follow a runtime language change like the rest of the window.
- Internal rework of how streams are scanned and buffered, with no change to what comes out. The AC-3 side track of the TrueHD merge is no longer re-copied for every frame it hands over. The start code scanner that every video codec funnels through now uses the CPU's vector instructions where they are available, a redundant second scan of large video frames was removed, large video frames are packetized straight into the output buffer instead of passing through a staging buffer, and a few repeated per-packet lookups were hoisted out of the mux loop. Output is byte-for-byte identical to the previous release across the whole test set, including subtitles, split output, CBR and plain TS. No speed figure is quoted: how much difference this makes depends on whether the CPU or the drive is the limiting factor, and on some measurements the two builds sit within run-to-run noise of each other.
- Hardened several long-standing edge cases found while reviewing the above: a malformed stream can no longer drive the output buffer into an endless growth loop, a TrueHD stream that never decodes a sync header can no longer divide by zero, an MLP header field is now range checked before it is used to walk a buffer, and merging from an AC-3 file with no sync word in its first block no longer discards a whole block of audio while reporting success.

## tsMuxeR 2.13.3

- Fixed the interface language not fully switching on the BDMV to ISO tab. Changing the interface language at runtime did not re-translate the "Include all files from the folder", "Keep data on the inner disc area", and disc label controls, so they kept the previously shown language while the rest of the tab updated. They now switch with everything else. This became visible only once 2.13.2 fixed the embedded translations.

## tsMuxeR 2.13.2

- Fixed the interface translations. A build-pipeline issue had been embedding a stale compiled-translation file, so several BDMV to ISO options added since 2.11.0 - the disc label field, "Include all files from the folder", and "Keep data on the inner disc area" - appeared in English in every language. The build now always embeds the current translations, so all eight interface languages show correctly again.
- The "Include all files from the folder" tooltip now also explains that the full standard Blu-ray folder structure is completed automatically, translated in all eight languages.

## tsMuxeR 2.13.1

- BDMV to ISO: corrected the "Include all files from the folder" checkbox tooltip. It now states that the full standard Blu-ray folder structure (the empty AUXDATA, BDJO, JAR and META folders, a CERTIFICATE folder, and a populated BACKUP) is completed automatically by default, matching a real BD/UHD disc. No behaviour change; the 2.13.0 build already created these folders.

## tsMuxeR 2.13.0

- GUI (BDMV to ISO): new **Keep data on the inner disc area (pad the outer edge)** checkbox (CLI: `--inner-only`). The outer edge of an optical disc is the most error-prone part to burn, so this packs the movie onto the inner tracks of every layer and fills the outer/rim region with zeros, keeping the data off the weak outer edge. The layer-break guard is sized automatically from the disc type and the content, and the image is padded to the full disc. When it is on, the manual guard controls are greyed out. Verified on dual-layer BD-R DL. Requested by DreckSoft.
- Tested on marginal media: a BD-R DL that verified with a 41 MB uncorrectable defect right at the layer transition still played the movie flawlessly (checked across 1:29 to 1:31), because the defect fell entirely inside the guard's zero band, so no video data was lost.
- BDMV to ISO: the image now gets the full standard Blu-ray folder structure by default (the empty AUXDATA, BDJO, JAR and META folders, a CERTIFICATE folder, and a BACKUP populated with copies of index.bdmv, MovieObject.bdmv and the PLAYLIST/CLIPINF files), the same structure tsMuxeR already creates when it authors a disc, so players that expect the complete layout are satisfied. Only what the source is missing is added: a folder that already ships its own BACKUP is left untouched, and the large .m2ts streams are never duplicated into it. Requested by Oleekae.
- BDMV to ISO: images build faster and use far less scratch space. The layer-break and inner-only guard padding is now stored as a sparse region instead of writing gigabytes of literal zeros to disk, so a guarded image finishes much quicker while burning byte-for-byte identically (an 8 GB inner-only guard costs no disk space and no write time). The folder copy uses a larger buffer as well.

## tsMuxeR 2.12.0

- GUI (BDMV to ISO): new "Disc label (optional)" field (CLI: --label=<name>). The volume label is written into the ISO, the same as the direct ISO output already allows; leaving it empty keeps the previous behaviour. Requested by DreckSoft.
- GUI (BDMV to ISO): new "Include all files from the folder (not just BDMV)" checkbox (CLI: --keep-extra-files). By default only the disc-structure folders (BDMV, CERTIFICATE, AACS) are written; ticking this also adds every other file and folder next to BDMV (readme files, cover art, extra folders) to the image. Requested by DreckSoft.
- Both new options are off by default, so an unchanged workflow produces the same ISO as before, and both are translated in all eight interface languages.

## tsMuxeR 2.11.0

- GUI (BDMV to ISO): the layer-break guard now defaults to 288 MB. Reported real defect zones cluster around 35, 64 and 258 MB, and on a defect-managed disc the true layer switch can sit up to 128 MB after the calculated break; 288 covers all of these. The coloured hint under the field explains the choice at every value. Requested by Coopervid, based on his defect-size reports.
- The small guard before the break now scales proportionally with the main guard: one sixteenth of the after value (the 64:4 ratio of the original design), at least 4 MB, so 288 gives 18 MB before. The advanced field shows the live value and stops following the moment you set your own. Suggested by Coopervid.
- Layer-fit placement (automatic): when the file that would cross the break fits completely on the next layer, it is placed there whole, so the break falls cleanly between two files. Two big titles (theatrical and directors cut) get one layer each; on seamless-branching discs the break lands between segments, the same spot commercial authoring uses. A movie larger than a layer still straddles the break with the guard, as before. Disable with --no-layer-fit.
- New checkbox "Keep original file order (seamless branching)" (CLI: --original-order): keeps the many segment files of a branching disc in their playback order instead of largest-first.
- Layer break report: after every build, the log shows where each guard landed, and for the main movie the playback time of the break, so you know where to spot-check on a player. The same information is saved next to the image as name.iso.layerbreak.txt. On seamless-branching discs the time is relative to the named segment file.
- CLI: --disc-capacity=<sectors> lets the layer-fit placement respect the disc end; the GUI passes it automatically.
- Selecting the BDMV folder itself (instead of the disc root above it) now automatically steps up to the parent, in the GUI and the CLI. Reported by Coopervid.
- The folder row has a refresh button, and re-picking the same folder re-reads it, so a fit estimate can no longer go stale after files were removed. Reported by Coopervid.
- The suggested output ISO now goes to the parent of the source folder, and the build only ever includes the disc structure folders (BDMV, CERTIFICATE, AACS); anything else next to them, such as a previously built ISO, is skipped with a log line instead of being muxed in.
- Long tooltips now word-wrap instead of rendering as one endless line.
- The playback-time reader samples several positions and uses the median, so a small block with a foreign timeline inside a stream can no longer produce a wrong time.
- The disc-authoring guide (English, German, Japanese) covers all of the above, plus new sections on seamless branching and defect-managed discs (measured on real hardware).

## tsMuxeR 2.10.1

- GUI (BDMV to ISO): the layer-break guard field now accepts up to 9999 MB (was 1024), for media whose defect zone at the layer transition is much larger than the ~35 MB typical case. The size hint now says so and suggests raising the guard if a test burn fails just after the break. Reported by Coopervid.

## tsMuxeR 2.10.0

- GUI (BDMV to ISO): the disc-type list now has separate BD-R DL and BD-RE DL entries and pre-fills the disc's standard "Free Sectors" for the chosen media, so you no longer need to run ImgBurn for a standard disc. The field is locked to prevent accidental changes; tick "Enter Free Sectors manually (advanced)" to override it for a non-standard disc (a reformatted BD-RE, or a BDXL burned without defect management). The pre-filled capacities were read from real Verbatim discs and match the Blu-ray/BDXL spec. Requested by Coopervid.
- Windows: fixed the GUI failing to start on Windows 7 with "api-ms-win-core-winrt-l1-1-0.dll is missing". The release build was shipping an unpatched, Windows 8+ Qt6 because a stale build cache skipped the qt6windows7-patched rebuild; the cache is now keyed so the patched build runs, and CI fails if a WinRT (Windows 8+) import ever returns to the shipped Qt. Reported by Coopervid.
- CI: updated actions/checkout to v5 (Node.js 20 is deprecated).

## tsMuxeR 2.9.6

- BDMV to ISO: report copy progress so the GUI progress bar advances while the ISO builds. The build itself is unchanged; it can simply be slow when the source is an optical disc (a few MB/s), and previously the bar sat at 0.0% the whole time and looked hung. tsMuxeR now prints "percent complete" as it copies, so the bar sweeps from 0 to 100. Reported by Coopervid

## tsMuxeR 2.9.5

- GUI: the "BDMV folder to ISO" tab now shows immediate feedback when you pick a folder: whether a BDMV was found, and for a read-only disc (a mounted ISO or optical drive) a note that the output ISO must go to a writable location. The folder size is also read instantly for discs (from the volume) instead of walking every file, so selecting an optical drive no longer stalls. Reported by Coopervid

## tsMuxeR 2.9.4

- Fix reading a BDMV from a mounted ISO or optical disc. The recursive directory scan tested the directory attribute with XOR, so on read-only media (where folders also carry the read-only and archive attributes) it skipped every folder and reported no files. Reported by Coopervid
- GUI: when the input BDMV is on read-only media such as a mounted ISO, default the output ISO to a writable folder instead of the read-only drive, and warn before building if the chosen output is not writable. Reported by Coopervid

## tsMuxeR 2.9.3

- GUI: hide the mux controls (output type, file name, meta file, and the mux and meta buttons) while the "BDMV folder to ISO" tab is active. That tab is self-contained and uses its own Build ISO button, so the mux controls were irrelevant and confusing there. Reported by Coopervid

## tsMuxeR 2.9.2

- GUI: add a tooltip on the layer-break guard field explaining that the burned zone aligns to whole 2048-byte sectors and file boundaries, so it matches the entered value within about 1 MB
- CI: name the release assets tsMuxeR-<version>-<platform>.zip instead of w64.zip / lnx.zip / mac-<arch>.zip

## tsMuxeR 2.9.1

- GUI: move the dual-layer controls (Fit to disc, Layer-break guard, Allow oversize) out of the progress dialog into the main window Output group, and show them only for Blu-ray ISO or Blu-ray folder output (hidden for TS, M2TS, MKV, AVCHD and Demux)

## tsMuxeR 2.9.0

- `--layer-break-guard-before=<MB>`: size the guard zone before the layer break on its own, instead of the fixed 4 MB margin, for media that are defective on both sides of the transition. The default stays asymmetric (the measured defect sits at the start of the next layer)
- GUI: the "BDMV folder to ISO" tab gains an optional "fill before the break" control with a short note on when to use it, and a live fit estimate that shows the image size against the disc's Free Sectors and whether it will fit

## tsMuxeR 2.8.1

- GUI: show the version number in the window title (previously only the git commit was shown)
- GUI: stop the "#" column header from overlapping the select-all checkbox in the track list

## tsMuxeR 2.8.0

Multi-layer disc authoring, on top of jaminmc/tsMuxer. See docs/DISC_AUTHORING.md.

- `--bdmv-to-iso`: wrap an existing unprotected BDMV folder into a burnable BD-ROM ISO byte for byte, keeping the BD-J menus and all clip and playlist references intact (no re-mux, no re-numbering)
- `--layer-break-guard=<MB>`: zero-fill the defect-prone sectors around each layer transition of BD-R/RE DL and BD-R XL media, so the movie plays across the break. Validated on real hardware, where it absorbed a genuine layer-1 defect
- `--layer-break-lbn=<sector[,sector...]>`: set the layer break sector(s); takes a comma list for 100 GB (2 breaks) and 128 GB (3 breaks) BD-R XL
- `--disc-size` and `--allow-oversize`: abort, or warn, before muxing if the estimated image will not fit the target disc
- ISO writer: multi-sector directories and 32-bit object IDs, so full retail discs (hundreds of files) can be wrapped
- Propagate asynchronous write errors so a truncated disc is no longer reported as a successful mux; fix a buffer leak on the write path
- Skip unsupported subtitle coding types in the Blu-ray playlist with a warning instead of aborting a finished, multi-hour mux
- Guard a divide-by-zero in the discovery phase, and stop a bogus "MLP is not standard" warning when a TrueHD track is merged with an AC-3 core from a file
- GUI: a "BDMV folder to ISO" tab with a layer-break calculator (paste the disc's Free Sectors from ImgBurn and the break sectors are worked out), colour-coded guard hints, disc-type and divisibility sanity warnings, and a BD-R XL at-your-own-risk confirmation
- GUI: dual-layer capacity and guard controls on the Blu-ray outputs, and runtime language switching that also updates the hand-built widgets
- GUI: added Japanese, and completed German, Spanish, French, Hebrew, Russian and Chinese to full coverage
- Windows: the release binaries build Qt 6.8.3 statically with the qt6windows7 patches, so the standard 32-bit and 64-bit builds run on Windows 7 and newer. This replaces the earlier separate Qt5 build. The workflow builds Qt, qttools and a static zlib with Ninja

## tsMuxeR 2.7.2
- **Qt6 GUI:** The default GUI build uses Qt6. In this fork the Windows binaries are built with the qt6windows7 patches, so they still run on Windows 7 (see docs/COMPILING.md)
- Added language selection to the video track options in the GUI, matching the existing audio/subtitle language selector
- Fixed AAC audio not being detected in MP4/MOV containers (reported as "Can't detect stream type"), caused by missing ADTS header generation when ESDS parsing did not set the AAC flag, and by channel count corruption from the AudioSpecificConfig channel configuration index
- Fixed MKV and MOV/MP4 demuxers returning absolute timecodes instead of relative delays, causing audio/video sync offsets (e.g. delay stored in differing track start times or edit lists) to be lost during remuxing
- GUI: Make meta file editable with manual override support, allowing users to manually edit the meta file directly in the GUI and apply advanced options not available through the visual interface
- GUI: Added "Reset meta to auto-generated" button to revert manual meta edits
- GUI: Support for merge-ac3-file option for TrueHD tracks, enabling TrueHD+AC-3 merge in the GUI with external .ac3 file input
- GUI: Preserve custom Blu-ray chapters when input files change
- Introduced AV1 codec support in MPEG-TS, implementing the AOM "Carriage of AV1 in MPEG-2 TS" draft specification
- AV1 muxing from MKV and MP4/MOV containers into MPEG-TS with start-code based OBU format and emulation prevention bytes
- AV1 demuxing from MPEG-TS to raw .obu elementary stream files
- AV1 Sequence Header parsing for profile, level, resolution, bit depth, color primaries, transfer characteristics, and HDR/WCG detection
- AV1 PMT descriptors: Registration descriptor (format_identifier 'AV01') and AV1 video descriptor (tag 0x80) per AOM spec
- AV1 keyframe detection for random access point signaling in MPEG-TS adaptation field
- Introduced automatic FPS detection from container metadata (MKV default_duration, MP4/MOV timescale) for codecs that lack timing info in the elementary stream (e.g. AV1)
- Fixed MP4/MOV files larger than 4GB not being read correctly due to compact atom size overflow
- Fixed AAC MPEG-2 TS descriptor (was disabled with early return; now emits proper AAC descriptor tag 0x2B)
- Fixed TrueHD/MLP TS descriptor to emit Blu-ray compliant HDMV registration descriptor in Blu-ray mode, or SMPTE-RA 'mlpa' registration descriptor otherwise
- Updated GUI to recognize AV1 codec: file dialog filters (.obu), About dialog, BD V3 auto-selection, and video track settings
- Updated all GUI About dialog translations (EN, DE, ES, FR, HE, RU, ZH) to include H.266/VVC and AV1 in supported codecs list
- Introduced Matroska (MKV/MKA) muxing support with EBML writing, cluster-based output, Cues index, and SeekHead
- Supported codecs for MKV output: H.264, HEVC, VVC, AV1, VC-1, MPEG-2 (video); AAC, AC3, E-AC3, DTS, TrueHD, LPCM, MP3 (audio); SRT, PGS (subtitles)
- Added CodecPrivate generation for H.264 (AVCDecoderConfigurationRecord), HEVC (HEVCDecoderConfigurationRecord), AV1 (AV1CodecConfigurationRecord), AAC (AudioSpecificConfig), and VVC
- Added MKV radio button to the GUI output panel with save dialog filter and auto-detection of .mkv/.mka extensions
- Updated all GUI translations (DE, ES, FR, HE, RU, ZH) with MKV muxing strings
- Added missing Matroska codec ID constants (A_DTS, A_EAC3, A_TRUEHD, A_MPEG/L3, V_MPEG2)
- Fixed aspect ratio override (`ar` parameter in metafile) being ignored for MPEG-2 and other video streams, so the original stream aspect ratio was always retained
- Added Opus audio codec support for MPEG-TS and MKV muxing, including OpusHead codec-private handling and TS descriptor generation per RFC 7845
- Added FLAC audio codec support for MKV muxing and demux output, including STREAMINFO codec-private parsing
- Introduced a stream discovery phase that probes all tracks before muxing begins, collecting codec properties (channels, sample rate, resolution, HDR metadata, codec-private data) upfront to prevent late-initialization bugs in container headers
- Generalized early codec-private propagation from containers (MKV, MP4/MOV) to all stream readers via `applyDiscoveryData()`, replacing the previous Opus-specific workaround
- Added `getTrackCodecPrivate()` support for MP4/MOV containers
- Added FLAC and Opus codec entries to USAGE documentation

## tsMuxeR 2.7.1
- Fixed file dialogs not appearing on macOS with Qt6 by using non-native dialogs
- Fixed browse button for output folder passing wrong parameter to file dialog
- Consolidated file type filters in add dialog to reduce dropdown size
- Added automatic copying of tsMuxeR CLI into macOS app bundle during build

## tsMuxeR 2.7.0
- Fixed a an issue so that Dolby Vision EL stream type is now correct 
- Fixed a bug with HEVC streams when an HDR10+ SEI payload is too short
- Fixed a bug where the first 2 frames of the first video track are muxed before anything else
- Introduced an improvement so Single Track Double Layer files now can properly handled
- Fixed a bug so that if no MOOF atom is met we stop atom parsing at the next MDAT atom
- Fixed an issue with HDR flags, so we only set them if an HEVC stream is detected
- Introduced correct ATSC descriptor for pure EAC3 tracks
- Introduced correct HDMV TS descriptors for MPEG-2 streams
- Fixed an issue where Blu-Ray movies will loop rather than stopping after reading
- Introduced being able to include Dolby Vision descriptors in TS or M2TS mode
- Fixed the order of streams so that video streams always come first
- Introduced a GUI option for adjusting PIP transparency
- Fixed an issue where translated strings appeared in the meta file
- Fixed a bug in the output paths of the MXE build scripts
- Ensured we keep M2TS descriptors in TS files (temporary until a long-term solution can be found)
- Fixed a bug where filenames were being truncated prematurely if there were dots in the filename
- Introduced putting overnight builds into OBS, to build for various Linux platforms
- Improved the documentation to fix a broken URL for the test files
- Introduced a simplification of the method used to play sounds in the GUI
- Fixed an issue with broken ISO labels when using non-ASCII characters
- Introduced a refactoring that moved the About page into an external HTML file for the GUI
- Introduced a code cleanup that removed all usages of std::wstring
- Fixed an issue with incorrect subtitle spacing on Windows
- Introduced support for M4V files
- Fixed issue with subtitle timestamps when joining multiple M2TS files together
- Fixed incorrect usage of POSIX APIs in Windows builds
- Fixed a bug with encoding errors when dealing with SSIF files
- Fixed a bug where we could read over the end of an MP4 file
- Introduced keeping the track order when multiple video tracks are added
- Introduced support for reading fragmented MP4 files
- Introduced support for specific AVC and HEVC descriptors in TS files
- Introduced support for Dolby Vision atoms in AVC or HEVC streams
- Introduced a changelog and improved general documentation
- Fixed an issue with garbled subtitles being displayed
- Introduced translation support, as well as a full Russian translation of the GUI
- Introduced getting the HDR10 information from the SPS VUI in HEVC
- Introduced detection of UTF8 in subtitle files
- Fixed usage of WinMain, which lead to issues with console output on Windows
- Introduced converting meta files using active code page if UTF8 fails
- Improved the documentation for building with Msys2
- Fixed bugs in the handling of non-ASCII characters in paths on Windows
- Fixed bugs in subtitles PIDs for BD V3 M2TS with HDR
- Fixed bug with the display of bitrate and channel numbers for EAC3 and AC3 tracks
- Fixed bug with GUI not correctly allowing to select DTS Express 24-bit as a secondary track
- Introduced an error message when an output file longer than 255 characters and reduced overall file length
- Fixed bug where 3D plane information was showing for 2D BD-ROMs
- Fixed a bug with uneven width between characters in subtitles on Mac and Linux
- Introduced the ability to detect audio delays in MKV files
- Fixed a bug where the 3D planes were not detected in specific cases
- Fixed a bug with alignment of the subtitle tracks and 3D planes
- Removed unnecessary floating point conversion code from the GUI source tree
- Added support for frame rates of 50, 59.94 and 60
- Fixed an issue with HDR10 HEVC streams where the maxCLL and maxFALL values were set incorrectly
- Fixed typos and improved the clarity of certain wording in the GUI
- Fixed typos and grammar issues with the readme and usage information
- Introduced the git revision to the version string in the GUI and CLI
- Introduced automatic selection of BD V3 for HEVC in GUI
- Fixed an issue with compiling on Mac
- Fixed an issue with the handling of wav64
- Introduced a workaround for QTBUG-28893
- Performed another round of GUI code cleanup
- Introduced a uniform code formatting style
- Fixed a bug with reading the FPS information from certain streams
- Fixed a typo in the GUI settings for the font family setting
- Introduced a warning when a V2 video format is used for a V3 Blu-ray
- Fixed a bug with incorrect stream ID for TS stream
- Fixed typos in the source files
- Introduced UHD Blu-ray as an option in the GUI
- Fixed a bug where invalid font files could crash tsMuxer
- Fixed an issue with HEVC stream detection in the GUI
- Introduced reading the FPS info from VPS or SPS, rather than VPS only
- Fixed a bug with the CPI table I-frame thresholds with UHD
- Introduced Dolby Vision support
- Fixed compiler warnings on return value overflows
- Fixed an issue with the stream ID being incorrectly set for BD V3
- Fixed an issue when spaces where in the path to the temporary meta file in the GUI
- Fixed an issue with buffer overflows on HEVC streams
- Fixed an issue so that TS descriptors are the same as on commercial Blu-rays
- Fixed an issue where numbers were shown instead of language codes in the GUI
- Introduced nightly builds, hosted on Bintray
- Fixed a bug where the tsMuxer executable could not be found on Windows in the GUI
- Fixed a bug where muxing a SRT results in a segfault on Linux
- Introduced support for UHD HDR10 and HDR10+
- Introduced a migration from "override" to "virtual" keywords in the code to conform better to C++14
- Introduced a migration from "QObject::connect" syntax to Qt5 equivalent in the GUI
- Fixed an issue with the min and max functions when compiling on Windows
- Fixed an issue calculating the AAC frame size
- Introduced UHD (width >= 2600) support in the MPLS and CLPI
- Introduced a clean up and reformatting of the documentation
- Introduced UHD BD V3 support
- Fixed an issue with EAC3 bitrate, sampling rate and channel information not being set correctly
- Fixed a bug with parsing of AC3
- Fixed an issue with the stream type not being set correctly for H265
- Fixed an issue when parsing MP4 AAC 5.1 where the channel output is not read correctly
- Fixed an issue with parsing the AAC frame length
- Introduced an update of the C++ standard from 11 to 14
- Introduced a cleanup of precompiled headers
- Introduced using std::thread for the TerminatableThread in libmediation
- Introduced cross-platform CMake build system
- Introduced a cleanup of libmediation that removed condvar, mutex and time from the library
- Introduced a translation of comments from Russian to English
- Introduced a migration from Qt4 to Qt5

## tsMuxeR 2.6.15
- Fixed mkv parser a bit. I've got unparsed file example

## tsMuxeR 2.6.13
- update SEI correction: do not correct SPS/PPS if stream contains different PPS with same pps_id

## tsMuxeR 2.6.12
- several minor bugs fixed

## tsMuxeR 2.6.11
- fixed saving UI settings to a registry. Also, if file tsMuxerGUI.ini found, UI will switch settings to an ini file instead of registry
  (you can create empty ini file at the beginning).
- UI: change control for cut start/end time
- fixed SEI processing for 'force' mode ( it doesn't work correctly for some movies)
- fixed bug in the wav demuxer (first audio frame has mixed up channels)
- fixed timings for PG streams. Timings was inaccurate for amount of several ms (for some movies only, it depended of the first PTS of the file)

## tsMuxeR 2.6.9
- inserting SEI did not work for some H.264 stream at all
- add more correction for VUI parameters if option insert SEI is active (it helps to open some H.264 streams in the Scenarins
  and solve PS3 problem for some sources)
- fixed channels for 7.1 and 7.0 wav files
- fixed combined H.264 streams read from Elementary Stream
- BD Bitrate control improved a little bit

## tsMuxeR 2.6.4
- Add secondary video support
- fixed mp4 files with MPEG-DASH
- fixed SEI again
- fixed DTS-ES recognition
- fixed font renderer (a little bit wrong text position)
- several minor improvments and bug fixes

## tsMuxeR 2.5.7
- fixed bug with SEI messages for some movie
- fixed problem with some movies where problem occured during processing several last video frames
- several minor bug fixes

## tsMuxeR 2.5.5
- add HEVC video codec support
- UI improvment: Save settings for General tab, Subtitles tab and last output folder
- Fixed file duration detection for ssif and some m2ts files
- Fixed bug if mux playlist and several sup files (it is a very olg bug, but it became much more often since 2.4.x)
- Several minor bug fixes

## tsMuxeR 2.4.0
- Add secondary audio support for bluray muxing. Due to standart It is allowed only for DTS-Express and DD+ codecs.
- Filter out H.264 filler packets
- UI improvment: option for MPLS offset can be entered either as time or as 45Khz clock value
- UI improvment: UI displays opened file duration
- UI improvment: chapter list correctly updated if join several files. Also joining for MPLS is enabled.
- Add help if run tsMuxeR without parameters
- Fixed muxing for 96Khz TRUE-HD tracks
- PCM inside VOB was anonced before, but actually did not work. Fixed.
- UI fix: if open MPLS, then close, track list is not cleared. It is broken in previous build only.
- Subtitles renderer fixed (broken in previous build only after in/out effects)

## tsMuxeR 2.3.2
- Support PG subtitles inside MKV
- Support MKV tracks with zlib compression
- Support 3D MP4 and MOV files (combined AVC+MVC stream)
- Add option 'line spacing' to subtitles renderer
- Add fade in/out effect to subtitles renderer
- Fixed ability to drag&drop files directly to tsMuxerGUI shurtcut (it worked before in version 10.6)
- Fixed splitting operation if no video track present
- bug fixed: tsMuxeR can't create output directory for UNC path (for instance \\.\Volume{E5FB13D8-5096-11E3-B9C4-005056C00008}\folder1\test.ts)
- bug fixed: message "file already exist" appeared if open several files from a folder with '(' in the name

## tsMuxeR 2.2.3
- Add support for DTS-HD elementary stream with extra DTSHD headers
- Add support for mkv with 'Header Stripping' compression
- Add 3D MKV support
- Add PCM inside MKV support
- Add PCM inside VOB support
- Fixed option 'bind to video fps' for subtitles
- Improved font renderer quality
- Fixed file splitting option (it was disabled since v.1.11.x because of was not implemented for ISO and 3D-blurays)
- Several minor bug fixes

## tsMuxeR 2.1.8
- Fixed join files problem with True-HD track
- introduce MAC build

## tsMuxeR 2.1.6
- Add support for combined AVC+MVC streams
- Output file size slightly reduced
- Fixed bug if mux AVC+MVC tracks to m2ts file. Some 3d m2ts movies did not play on Samsung Smart TV
- Fixed minor bug in a SSIF interleaving for some movies
- introduce Linux build

## tsMuxeR 2.1.4
- Same problem fixed again. Sometimes tsMuxeR get access to file with wrong name during mpls processing.

## tsMuxeR 2.1.3
- Previous version introduce a new bug. Sometime tsMuxeR showed error message "file not found". Fixed.

## tsMuxeR 2.1.2(b);
- fixed bug in MVC stream recognition. MVC from Intel Media Encoder now work.
- SSIF files is not required any more if you open 3D MPLS file
- Add Stereo subtitles basic support. If source PG stream has stereo format, same stereo PG stream will be created in a output file
- Add tag <force> (or <f>) to srt parser. This tag force to show subtitle message. For instance:

	1
	00:00:10,440 --> 00:00:20,375
	<force>	
	<b>Senator</b>, we're making
	our final approach into Coruscant.

## tsMuxeR 2.0.8:
- fixed subtitles bug: "3d-plane" option was inaccessible for many disks

## tsMuxeR 2.0.7:
 improvments:
- add control for select/unselect all tracks at once
 bug fixes:
- extract ac3 core from e-ac3 track fixed
- fixed option --m2tsOffset (was broken in version 2.x.x)
- fixed 'bufer overflow' error message if simultaneously mux several m2ts files and one of them has PSG tracks only
- fixed problem with too long file names in demux mode for large mpls files

## tsMuxeR 2.0.6:
- bug fixed: removing overlapped frames for HD audio fixed

## tsMuxeR 2.0.5:
- add direct ISO output

## tsMuxeR 1.12.10:
- fixed H.264 stream parser. Same fix as in previous version but more careful
- fixed subtitles color selection in UI

## tsMuxeR 1.12.10:
- fixed H.264 stream parser. It cause video distortion for some movies.
- add DTS-express support. Is not fully complete yet, tsMuxeR doesn't produce subpath for secondary audio

## tsMuxeR 1.12.9:
- fixed file join for mov/mp4
- fixed bug in SEI unit processing (if enable options 'insert picture timing'). Bug may cause video distortion.
- fixed distortion for VC1 codec if join several files
- seamless audio fixed. Extra audio frame correctly removed.

## tsMuxeR 1.12.6:
- fixed 3d subtitles. Add ability to select 3D offset plane for subtitles
- add new parameter '--start-time'. This parameter define time for first video frame in output file. This parameter is filled automatically (too keep same input time) if open MPLS file.
- several more minor fixes in transport stream to improve Blu-ray compatibility
- fixed E-AC3 codec

## tsMuxeR 1.12.3:
- fixed problem with ssif muxing
- add addition check for 'insert picture timing' parameter. For MVC depended view used same value as for primary video stream
- add new parameter to GUI and tsMuxeR core: 'right-eye'. Parameter is used for 3D blurays only. If parameter is set then MPEG-4 MVC Base view video used for Right eye.
This parameter filled automatically in GUI if open MPLS file.

## tsMuxeR 1.12.2:
- add 3d bluray support. Bluray muxing activated automatically if MVC substream appears in input tracks.
To reduce HDD space, tsMuxeR doesn't produce ssif file, only a couple of .m2ts files. ssif files can be 
creted on the fly in DVD fab using "create mini iso" menu item.
- add ability to mux to ssif file directly. It is not supported in GUI, but you can provide .ssif file extension
- fixed bugs in SEI message processing and add MVC sei message support
- fixed several bugs in the Transport Stream to improve compatibility with Blu-ray standart.

## tsMuxeR 1.11.6:
- fixed bug in SSIF file demuxing. It cause a problem for subtitles tracks.

## tsMuxeR 1.11.5:
- added SSIF files support for blu-ray play lists (MPLS)

## tsMuxeR 1.11.4:
- detect language for audio/subtitle tracks fixed for SSIF files (it's work if ssif file is opened from Blu-ray disk structure)

## tsMuxeR 1.11.3:
- bug fixed in MVC parsing

## tsMuxeR 1.11.0:
- add support of SSIF files and MVC codec (3d Blu-ray compatibility)
