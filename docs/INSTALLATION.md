# tsMuxer : Installation Instructions

The following executables are created to be portable, so you can just save and extract the compressed package for your platform. 

Download a release from the [releases](https://github.com/teaching-droid/tsMuxer/releases) page.
Each package has a `.sha256` file beside it, to check the download with. Builds between releases are
on the [Actions](https://github.com/teaching-droid/tsMuxer/actions) tab, as the artifacts of the
newest run of each build.

| System | Package |
|--------|---------|
| Windows, 64-bit | `tsMuxeR-<version>-windows-x64.zip` |
| Windows, 32-bit | `tsMuxeR-<version>-windows-x86.zip` |
| Linux, any current distribution | `tsMuxeR-<version>-x86_64.AppImage` |
| Linux, with Qt 6.7 or newer installed | `tsMuxeR-<version>-linux-x64.zip` |
| macOS, Apple silicon | `tsMuxeR-<version>-macos-arm64.zip` |
| macOS, Intel | `tsMuxeR-<version>-macos-x86_64.zip` |

## Windows

The ZIP file for Windows can just be unzipped and the executables can be used straight away, there
are no dependencies. The Windows binaries are built with Qt 6.8 and the
[qt6windows7](https://github.com/crystalidea/qt6windows7) patches, so the standard builds run on
Windows 7 and newer. libass, which draws ASS and SSA subtitles with their styles, is in the package
too.

### If your antivirus flags it

Some products report the Windows binaries as something like `Win64:Evo-gen`. A name of that shape is
a generic heuristic. It says the file resembles a statistical pattern the engine has learned, not
that anything known was found inside it. A small program that is not code signed and writes very
large files is close enough to that pattern to trip it.

The binaries are not code signed. A certificate costs a fee every year, which this project does not
have, and being unsigned is one of the things the heuristic weighs.

What you can check for yourself, rather than taking anyone's word for it:

- Every Windows build is produced by GitHub Actions from the source in this repository. The workflow
  and its log are public, on the [Actions](https://github.com/teaching-droid/tsMuxer/actions) tab. A
  tagged build is attached to the release by that same workflow, so the file on the releases page is
  the file the runner built.
- Scan it with more than one engine. A single generic name from one engine, while the others stay
  quiet, is weak evidence.
- Build it yourself and compare. See [COMPILING.md](COMPILING.md).

If you would like to help, report it to your vendor as a false positive. Every vendor has a form for
that, and a report from someone who was actually affected carries more weight than one from the
project.

## Linux

### AppImage

The AppImage holds the window and the command line program in one file, with the Qt libraries the
window needs. It runs on most current distributions (glibc 2.35 or newer). Make it executable and
start it:
```
chmod +x tsMuxeR-<version>-x86_64.AppImage
./tsMuxeR-<version>-x86_64.AppImage
```
`./tsMuxeR-<version>-x86_64.AppImage tsMuxeR <arguments>` runs the command line program. An
AppImage needs FUSE to start; where it is missing, as in a container,
`--appimage-extract-and-run` before the other arguments runs it without. An update tool such as
AppImageUpdate or Gear Lever can fetch the next release, and an update downloads only the changed
parts.

### ZIP

The ZIP file holds the two programs on their own (glibc 2.34 or newer):
- `tsMuxeR`, the command line program, needs FreeType and zlib, which every desktop has.
- `tsMuxerGUI`, the window, needs the Qt 6.7 or newer libraries of the system (Qt Widgets and
  Qt Multimedia). Many distributions have an older Qt, such as Ubuntu 24.04 and Debian 12 (Qt 6.4),
  and there the window does not start (`version 'Qt_6.7' not found`): take the AppImage instead.

### Drawing ASS and SSA subtitles

On Linux, ASS and SSA subtitles are drawn with the libass of the system, with both packages. Install
it from the distribution (`libass9` on Debian and Ubuntu, `libass` on Fedora and Arch). Without it they are drawn as plain text, without their styles, and a warning says so.

## MacOS

The ZIP file for MacOS can just be unzipped. The window, `tsMuxerGUI.app`, holds the Qt frameworks
it needs, and `tsMuxeR` is the command line program; nothing else needs installing.

### The first time you open it

macOS will say the developer cannot be verified. Right-click the app and choose Open, and it will
start and be remembered. The app is signed, but with an ad-hoc signature rather than a paid Apple
developer certificate, and only notarisation removes that prompt.

If it says the app is **damaged** rather than unverified, the download is from 2.18.9 or earlier.
Every arm64 build before 2.18.10 was packaged without a signature and with the symbolic links in
its frameworks flattened, which macOS rejects outright.

### Drawing ASS and SSA subtitles

ASS and SSA subtitles are drawn with libass from [Homebrew](https://brew.sh), when it is installed:
```
brew install libass
```
Without it they are drawn as plain text, without their styles, and a warning says so.
