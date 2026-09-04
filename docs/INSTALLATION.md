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
