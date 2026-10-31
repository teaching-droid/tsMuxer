# tsMuxer : Testing

There is no automatic test suite. A build can be checked by hand with the steps below, on any short file of your
own: a minute or two of H.264 or HEVC video with AC-3 sound is enough.

## The programs start

`tsMuxeR` with no arguments prints its version on the first line, such as `tsMuxeR version 3.0.0 (git-1234abc)`.
`tsMuxeR source.mkv` lists the tracks of a file: the Stream ID and the Track ID of each are what its line in a meta
file needs.

## A mux

Write a meta file, `test.meta`, from the listing:

```
MUXOPT --no-pcr-on-video-pid --new-audio-pes --vbr --vbv-len=500
V_MPEG4/ISO/AVC, "source.mkv", track=1
A_AC3, "source.mkv", track=2
```

(`V_MPEGH/ISO/HEVC` for HEVC video.) A file named without its folder is looked for in the current folder, so run
the commands from the folder that holds it. Then mux it to each kind of output:

```
tsMuxeR test.meta out.m2ts
tsMuxeR test.meta out.mkv
```

and, with `--blu-ray` added to the MUXOPT line, to a Blu-ray folder and an image:

```
tsMuxeR test.meta out_folder
tsMuxeR test.meta out.iso
```

Each ends with `Mux successful complete` and exit code 0. Then check that:

1. each output plays, and seeking works in it
2. `tsMuxeR out.m2ts` (or `out.mkv`, or the `.m2ts` in `out_folder/BDMV/STREAM`) lists the same tracks as the source
3. the log has no warning that you do not expect

## Two builds compared

The same meta file and source give the same output byte for byte, run after run, for a TS, an M2TS and a Blu-ray
folder, so two builds can be compared with a checksum of each file. An ISO image is the same when `--constant-iso-hdr`
is on the MUXOPT line. A Matroska file is not: its unique IDs are new on every run, so two Matroska files differ in
a few bytes even from the same build.

## The window

Start `tsMuxerGUI`, add the file, choose an output and press Start muxing. The meta file box shows what is muxed;
Save meta file writes it, and the command line gives the same output from it.

## If something fails

Please report it with the bug report form on GitHub, with the version line and the full log, the meta file, what
you expected and what happened instead. A short sample of the source helps most.
