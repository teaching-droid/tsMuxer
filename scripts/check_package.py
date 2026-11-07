#!/usr/bin/env python3
"""Lists every file of a built package and fails if any of it is FFmpeg, GStreamer or Qt Multimedia.

tsMuxeR ships no FFmpeg. Qt Multimedia, linked once for the sound at the end of a mux, brought Qt's FFmpeg plug-in
and the FFmpeg libraries into the macOS and Linux packages without anything noticing, so every package is checked
before it is published.

    python3 scripts/check_package.py <folder | .zip | .AppImage | .7z> [...]

A folder and a .zip are read here; anything else is listed with 7z, which reads an AppImage as well. The exit code is
0 when nothing was found, 1 when something was, and 2 when a package could not be read.
"""

import os
import re
import shutil
import subprocess
import sys
import zipfile

# The libraries of FFmpeg in their Linux, macOS and Windows names (libavcodec.so.61, libavcodec.61.dylib,
# avcodec-61.dll), anything named after FFmpeg or GStreamer, GStreamer's own libraries and plug-ins, and Qt
# Multimedia with its plug-ins.
FORBIDDEN = [
    re.compile(r"(^|/)(lib)?(avcodec|avformat|avutil|avfilter|avdevice|avresample|swscale|swresample|postproc)"
               r"([-.0-9]|$)[^/]*$", re.I),
    re.compile(r"ffmpeg", re.I),
    re.compile(r"gstreamer", re.I),
    re.compile(r"(^|/)libgst[^/]*$", re.I),
    re.compile(r"(^|/)(lib)?Qt6?Multimedia", re.I),
    re.compile(r"(^|/)(plugins|PlugIns)/multimedia(/|$)", re.I),
]


def list_folder(path):
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            yield os.path.relpath(os.path.join(root, name), path).replace(os.sep, "/")


def list_zip(path):
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            yield name.rstrip("/")


def list_7z(path):
    seven = shutil.which("7z") or shutil.which("7za") or shutil.which("7zz")
    if not seven:
        raise RuntimeError("7z is needed to list " + path)
    out = subprocess.run([seven, "l", "-ba", "-slt", path], capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        raise RuntimeError("7z could not list " + path + ": " + out.stderr.strip())
    for line in out.stdout.splitlines():
        if line.startswith("Path = "):
            yield line[len("Path = "):].replace("\\", "/")


def check(path):
    if os.path.isdir(path):
        names = list(list_folder(path))
    elif zipfile.is_zipfile(path):
        names = list(list_zip(path))
    else:
        names = list(list_7z(path))
    if not names:
        raise RuntimeError(path + " holds no files")
    hits = [n for n in names if any(p.search(n) for p in FORBIDDEN)]
    print(f"{path}: {len(names)} entries")
    for n in sorted(hits):
        print(f"  NOT ALLOWED: {n}")
    return hits


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    found = False
    for path in argv[1:]:
        try:
            found |= bool(check(path))
        except (OSError, RuntimeError, zipfile.BadZipFile) as e:
            print(f"{path}: cannot be read: {e}")
            return 2
    if found:
        print("FAIL: the package holds FFmpeg, GStreamer or Qt Multimedia, and tsMuxeR ships none of them.")
        return 1
    print("OK: no FFmpeg, GStreamer or Qt Multimedia in the package.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
