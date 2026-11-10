# Compiling

The release binaries are made by the workflows under `.github/workflows/`. The sections Linux, Linux AppImage, MacOS (Native) and Windows (native, Visual Studio) describe what they do. The other sections come from earlier versions; the release builds do not use them, and they are not checked.

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

## Docker (Linux build)

*This way is not used by the release builds, and it is not checked for this version. The workflows under `.github/workflows/` show how the release binaries are made.*

You can use the [Docker container](https://github.com/jaminmc/tsmuxer_build) to build the Linux
binaries. Browse to the tsMuxer repository and run:

```
docker run -it --rm -v $(pwd):/workdir -w="/workdir" jaminmc/tsmuxer_build bash -c ". scripts/rebuild_linux_with_gui_docker.sh"
```

The executables will be saved to the "bin" folder. Windows and Mac builds do not use Docker in
this repository; see their own sections below.

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

## Windows (MXE on Linux)

*This way is not used by the release builds, and it is not checked for this version. The workflows under `.github/workflows/` show how the release binaries are made.*

To compile tsMuxer and tsMuxerGUI for Windows using MXE on Linux you must follow the steps below on Ubuntu:

```
# setup pre-reqs
sudo apt-get install -y software-properties-common
sudo apt-get install -y apt-transport-https
sudo apt-get install -y checkinstall
sudo apt-key adv --keyserver keyserver.ubuntu.com --recv-keys C6BF758A33A3A276

# add MXE repo
sudo add-apt-repository -y 'deb https://mirror.mxe.cc/repos/apt stretch main'
sudo apt-get update

# install necessary MXE components for building tsmuxer
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-zlib
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-harfbuzz
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-freetype
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-cmake
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-ccache
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-autotools
sudo apt-get install -y mxe-x86-64-pc-linux-gnu-autotools
sudo apt-get install -y mxe-x86-64-pc-linux-gnu-ccache
sudo apt-get install -y mxe-x86-64-pc-linux-gnu-cc
sudo apt-get install -y mxe-x86-64-pc-linux-gnu-cmake

# manually fix some weird symlinks
sudo rm /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/x86_64-w64-mingw32.static-g++
sudo rm /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/x86_64-w64-mingw32.static-gcc
sudo ln -s /usr/lib/mxe/usr/bin/x86_64-w64-mingw32.static-g++ /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/x86_64-w64-mingw32.static-g++
sudo ln -s /usr/lib/mxe/usr/bin/x86_64-w64-mingw32.static-gcc /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/x86_64-w64-mingw32.static-gcc
sudo rm /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/g++
sudo rm /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/gcc
sudo ln -s /usr/lib/mxe/usr/bin/x86_64-w64-mingw32.static-g++ /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/g++
sudo ln -s /usr/lib/mxe/usr/bin/x86_64-w64-mingw32.static-gcc /usr/lib/mxe/usr/x86_64-pc-linux-gnu/bin/gcc
```

If you want to compile the GUI as well you also need to install Qt6 for MXE:
```
sudo apt-get install -y mxe-x86-64-w64-mingw32.static-qt6
```

With all the dependencies set up we can now actually compile the code.

Open the folder where the git repo is stored in a terminal and run the following to build just the command-line program:

```
# build the project
./scripts/rebuild_mxe.sh
```

Or run the following to build the GUI as well:

```
# build the project
./scripts/rebuild_mxe_with_gui.sh
```

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

## Windows (Msys2)

*This way is not used by the release builds, and it is not checked for this version. The workflows under `.github/workflows/` show how the release binaries are made.*

To compile tsMuxer and tsMuxerGUI on Windows with Msys2, you must download and install [Msys2](https://www.msys2.org/). Once you have Msys2 fully configured, open an Msys2 prompt and run the following commands, depending on which build you require:

Common:
```
pacman -Syu
pacman -Sy --needed base-devel \
flex \
zlib-devel \
git
```

Or just run:
```
./scripts/rebuild_msys2.sh
```

Close the Msys2 prompt and then open either a Mingw32 or a Mingw64 prompt, depending on whether you want to build for 32 or 64 bit.
```
pacman -Sy --needed $MINGW_PACKAGE_PREFIX-toolchain \
$MINGW_PACKAGE_PREFIX-cmake \
$MINGW_PACKAGE_PREFIX-freetype \
$MINGW_PACKAGE_PREFIX-zlib \
$MINGW_PACKAGE_PREFIX-ninja
```

If you intend to build the GUI as well you need to also install Qt6:
```
pacman -Sy --needed $MINGW_PACKAGE_PREFIX-qt6-static
```

Download tsMuxer repo and browse to its location:
```
cd ~
git clone https://github.com/teaching-droid/tsMuxer.git
cd tsMuxer
```

Then run:
```
./scripts/rebuild_msys2.sh
```

This will create in tsMuxer/bin statically compiled versions of tsMuxer and tsMuxerGUI - so no external DLL files are required.

## MacOS (osxcross on Linux)

*This way is not used by the release builds, and it is not checked for this version. The workflows under `.github/workflows/` show how the release binaries are made.*

To use osxcross on Ubuntu to compile for OSX, first run the following commands:

```
# setup pre-reqs
sudo apt-get install -y clang
sudo apt-get install -y patch lzma-dev libxml2-dev libssl-dev python curl
```

Next you have a choice - compile osxcross from source or use a prepared package. To compile osxcross from source:

```
# set up a new osxcross installation
cd /tmp
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
curl -sLo tarballs/MacOSX10.10.sdk.tar.xz "https://s3.eu.cloud-object-storage.appdomain.cloud/justdan96-public/MacOSX10.10.sdk.tar.xz"

export SDK_VERSION="10.10"
export OSX_VERSION_MIN="10.6"
UNATTENDED=1 ./build.sh

rm -rf "target/SDK/MacOSX10.10.sdk/usr/share/man"

# copy to permanent home as root
sudo su -
cp -r target /usr/lib/osxcross                                                                                    
cp -r tools /usr/lib/osxcross/ 
exit

# remove temporary folder
cd ..
rm -rf osxcross
```

To install osxcross from a pre-compiled package for Ubuntu 19 x86_64:

```
# download the package to /tmp
curl -sLo /tmp/osxcross-6acb50-20191025.tgz "https://s3.eu.cloud-object-storage.appdomain.cloud/justdan96-public/osxcross-6acb50-20191025.tgz"

# extract to correct location
sudo su -
cd /tmp
mkdir /usr/lib/osxcross
tar -xzf osxcross-6acb50-20191025.tgz --strip-components=1 -C /usr/lib/osxcross
exit

# remove tar file
rm -f osxcross-6acb50-20191025.tgz
```

Now to setup the tsMuxer build dependencies for osxcross:

```
# install freetype and zlib in root session with osxcross-macports, dependencies for tsMuxer build
sudo su -
export MACOSX_DEPLOYMENT_TARGET=10.10
export PATH=/usr/lib/osxcross/bin:/usr/lib/osxcross/tools:$PATH
/usr/lib/osxcross/bin/osxcross-conf
/usr/lib/osxcross/bin/osxcross-macports
/usr/lib/osxcross/bin/osxcross-macports install freetype
/usr/lib/osxcross/bin/osxcross-macports install zlib
exit
```

With all the dependencies set up we can now actually compile the code.

Open the folder where the git repo is stored in a terminal and run the following to build just the command-line program:

```
# build the project
./scripts/rebuild_osxcross.sh
```

Or run the following to build the GUI as well:

```
# build the project
./scripts/rebuild_osxcross_with_gui.sh
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
