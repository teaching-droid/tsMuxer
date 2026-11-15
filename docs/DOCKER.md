# tsMuxeR in Docker

The image holds the command line program, tsMuxeR, for x86-64 Linux, such as a NAS. The window is
not part of it. It is published with each release as `ghcr.io/teaching-droid/tsmuxer`, tagged with the version and
`latest`.

## Running a job

The folder you mount at `/storage` is the working folder: the files you give, and the paths in a meta file, are
taken from there, and the output is written there.

    docker run --rm -v "$PWD:/storage" ghcr.io/teaching-droid/tsmuxer film.mkv
    docker run --rm -v "$PWD:/storage" ghcr.io/teaching-droid/tsmuxer film.meta film.ts
    docker run --rm -v "$PWD:/storage" ghcr.io/teaching-droid/tsmuxer film.meta disc_folder

The first lists the tracks of `film.mkv`. The others run the job of `film.meta`, to a transport stream or to a
Blu-ray folder (with `--blu-ray` on its MUXOPT line). A path in a meta file is a path inside the container: give it
relative to `/storage`, or mount the folder it names at the same path (`-v /volume1/video:/volume1/video`). Every option of the command line works as in
[USAGE.md](USAGE.md), and the exit code of the container is tsMuxeR's (0 done; the others are in the table of exit
codes in [USAGE.md](USAGE.md)).

## Who owns the files

The program runs as user 1000, group 1000, so the files it writes belong to that user on the host. Give your own
numbers with `USER_ID` and `GROUP_ID`:

    docker run --rm -e USER_ID=1026 -e GROUP_ID=100 -v /volume1/video:/storage ghcr.io/teaching-droid/tsmuxer film.meta film.ts

`--user 1026:100` does the same. The program has no other groups: where a folder can only be reached through a
second group, give it with `--user 1026:100 --group-add 1001`. `USER_ID=0 GROUP_ID=0` runs it as root, which rootless
Docker and Podman map to your own user.

## Stopping a job

`docker stop`, or Ctrl+C in a container started with `-it`, asks the program to stop: the job ends with exit code 4
(130 for Ctrl+C), and its unfinished output is removed.

## Your own ffmpeg

ffmpeg is not part of the image. The audio conversion (`convert=`, `--convert-audio`) and the made Dolby Vision
layer use an ffmpeg of your own: put a static build of ffmpeg for Linux in a folder, mount it at `/config`, and
give it with `--ffmpeg=/config/ffmpeg` on the MUXOPT line or the command line. It must be built for x86-64, and USER_ID must be allowed to run it.

    docker run --rm -v "$PWD:/storage" -v "$HOME/tools:/config" ghcr.io/teaching-droid/tsmuxer conv.meta disc_folder

The made Dolby Vision layer also needs libdovi, which is not part of the image either: put `libdovi.so.3` in the same
folder and add `-e LD_LIBRARY_PATH=/config`.

## Subtitles

ASS and SSA subtitles are drawn with libass, and subtitles turned into pictures use the Liberation and DejaVu
fonts of the image. Fonts attached to a Matroska file are used as well, and so are fonts in a `.fonts` folder inside
the folder mounted at `/config`.

## Building the image

From the folder of the source:

    docker build -f packaging/docker/Dockerfile -t tsmuxer .

`--build-arg TSMUXER_GIT_REV=<commit>` names the commit in the version, as the build has no `.git` folder.

## Licenses

The license of tsMuxeR and the notices of what the image includes are in `/usr/share/doc/tsmuxer` inside the image
(LICENSE, NOTICE, THIRD-PARTY-NOTICES.txt, the license texts, and PACKAGES.txt, the list of its packages).
