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

If you receive missing dependency errors you may need to install a couple of dependencies, using the commands below in the Terminal:
```
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)" < /dev/null 2> /dev/null
brew install freetype
brew install zlib
```
