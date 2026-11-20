# Compiling

The release binaries are made by the workflows under `.github/workflows/`. The sections below describe what they do.

## Requirements

- **CMake**: 3.12 or later, and Ninja where the scripts use it
- **C++ Compiler**: C++20 support required
  - GCC 10 or later (the Linux release builds use GCC 11 on Ubuntu 22.04)
  - Clang 11 or later
  - MSVC 2019 or later (the Windows release builds use Visual Studio 2022)
- **FreeType** and **zlib** for the command line program
- **Qt**: Qt 6.7 or later for the window, Qt Widgets only. The sound at the end of a mux is played by the system's
  own player, so no Qt Multimedia is needed. The
  Windows release builds use Qt 6.8.3 with the [qt6windows7](https://github.com/crystalidea/qt6windows7) patches,
  built from source with **Ninja** (the Visual Studio generator cannot build Qt, because Qt uses per-configuration
  `EXCLUDE_FROM_ALL` targets that multi-config generators reject) and shipped as DLLs beside the window.
- **libass** is not linked: the program loads it at run time when it is there, to draw ASS and SSA subtitles.

**Note on Windows Compatibility:** with the [qt6windows7](https://github.com/crystalidea/qt6windows7) patches, the
32-bit and 64-bit builds run on Windows 7 and newer. See the Windows (native) section below.

## Linux

The release builds run on Ubuntu 22.04, so that the programs also start on older systems: a program needs the glibc of
the system it was built on, or a newer one. On Debian or Ubuntu, install the tools and the libraries:

```
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build libfreetype-dev zlib1g-dev
```

The window needs Qt 6.7 or later (Qt Widgets). Take it from the distribution where it has one that new, or
install it with [aqtinstall](https://github.com/miurahr/aqtinstall), as the release builds do:

```
pip install aqtinstall
aqt install-qt linux desktop 6.7.3 linux_gcc_64 -O ~/Qt
export CMAKE_PREFIX_PATH=~/Qt/6.7.3/gcc_64
```

Then build from the repository folder:

```
# the command line program and the window, in build/tsMuxer/tsmuxer and build/tsMuxerGUI/tsMuxerGUI
./scripts/rebuild_linux_with_gui.sh
# or the command line program alone, as bin/tsMuxeR
./scripts/rebuild_linux.sh
```

A build without a build type is made as Release.

## Linux AppImage

The AppImage is made by `.github/workflows/linux_appimage.yml` on Ubuntu 22.04. It builds the two programs as above,
gathers the window's Qt libraries and plugins with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and its
Qt plugin, and packs the result with [appimagetool](https://github.com/AppImage/appimagetool), with the update
information an update tool reads. Each of these tools is a pinned release, checked against its SHA-256 before it is
run. The workflow then starts the AppImage in clean Debian, Fedora, Arch and Ubuntu containers. To make one, run the
workflow from the Actions tab of a fork, or follow its steps on Ubuntu 22.04.

## Windows (native, Visual Studio)

You need Visual Studio 2022 with the C++ workload, CMake, and Ninja.

The release binaries are produced by `.github/workflows/windows_64.yml` and `windows_32.yml`. They build Qt 6.8.3
from source with the [qt6windows7](https://github.com/crystalidea/qt6windows7) patches, which takes roughly 40
minutes, and the libass that draws ASS and SSA subtitles with vcpkg. The package holds the two programs, the Qt and
libass DLLs and the Visual C++ runtime beside them, and runs on Windows 7 and newer. Qt must be configured with the
Ninja generator, from a Visual Studio developer command prompt (so `cl.exe` is on `PATH`); the Visual Studio
generator cannot build Qt. The workflows are the reference if you want to reproduce it locally.

For day-to-day development it is much quicker to configure tsMuxer against a regular Qt 6.7 or later installation
(for example one installed with aqtinstall). Run `windeployqt` to gather its DLLs beside the window; a build with
those runs on Windows 10 and later.

```
cmake -B build -DTSMUXER_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
```

## MacOS (Native)

You need the Xcode command line tools, CMake, and Qt 6.7 or later (Qt Widgets). Qt from Homebrew has issues
with `macdeployqt` and is *not* supported; install the official Qt package with
[aqtinstall](https://github.com/miurahr/aqtinstall), as the release builds do:

```
pip3 install aqtinstall
aqt install-qt mac desktop 6.7.3 clang_64 -O ~/Qt
export PATH=~/Qt/6.7.3/macos/bin:$PATH
```

Then run `./scripts/build_macos_native.sh` from the repository folder. It builds FreeType 2.14.3 from its source
release, checked against its SHA-256, and links it statically, so nothing has to be installed with Homebrew; zlib is
the one that comes with macOS. `CMAKE_OSX_ARCHITECTURES=arm64` or `x86_64` chooses the architecture, and
`TSMUXER_FREETYPE_FROM_BREW=1` takes Homebrew's FreeType instead. The result is `build/bin/mac.zip`, with `tsMuxeR`
and `tsMuxerGUI.app`, signed ad hoc.
