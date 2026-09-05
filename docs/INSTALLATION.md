# tsMuxer : Installation Instructions

The following executables are created to be portable, so you can just save and extract the compressed package for your platform. 

Builds are produced by GitHub Actions. Download them from the newest run's artifacts on the
[Actions](https://github.com/teaching-droid/tsMuxer/actions) tab, or, for tagged versions, from the
[releases](https://github.com/teaching-droid/tsMuxer/releases) page.

## Windows

The ZIP file for Windows can just be unzipped and the executables can be used straight away, there
are no dependencies. The Windows binaries are built with Qt 6.8 and the
[qt6windows7](https://github.com/crystalidea/qt6windows7) patches, so the standard builds run on
Windows 7 and newer:
- `tsMuxer-w64*.zip` - 64-bit
- `tsMuxer-w32*.zip` - 32-bit

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

The ZIP file for Linux can just be unzipped and the executables can be used straight away - there are no dependencies.

## MacOS

The ZIP file for MacOS can just be unzipped, as the executables should be relocatable. 

### The first time you open it

macOS will say the developer cannot be verified. Right-click the app and choose Open, and it will
start and be remembered. The app is signed, but with an ad-hoc signature rather than a paid Apple
developer certificate, and only notarisation removes that prompt.

If it says the app is **damaged** rather than unverified, the download is from 2.18.9 or earlier.
Every arm64 build before 2.18.10 was packaged without a signature and with the symbolic links in
its frameworks flattened, which macOS rejects outright.

### Missing dependencies

If you receive missing dependency errors you may need to install a couple of dependencies, using
the commands below in the Terminal:
```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install freetype
brew install zlib
```
