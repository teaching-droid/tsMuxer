#!/usr/bin/env bash

set -ex

# Run from the repository root whatever directory this was invoked from. Without this the
# script only works when the working directory happens to be the root: "mkdir build" lands
# wherever you stood, and the "cmake .." below then points at a directory with no
# CMakeLists.txt in it. Upstream issue 906 is that failure, reported as a wrong cmake call.
cd "$(dirname "$0")/.."

export MACOSX_DEPLOYMENT_TARGET=10.15

FREETYPE_EXTRA_LDFLAGS=""

if [ "${CMAKE_OSX_ARCHITECTURES:-}" = "x86_64" ]; then
  # Homebrew will not install on Intel macOS at all any more. Its installer does not fail while
  # fetching a package: it refuses outright, printing
  #
  #   Homebrew on macOS is only supported on Apple Silicon processors!
  #
  # and exiting 1. So the second Homebrew at /usr/local, which this used to install under Rosetta,
  # is gone for good and no amount of retrying brings it back. 2.18.14 was the last release with an
  # Intel package; the September runs still had an Intel path in that installer.
  #
  # freetype is the only thing the Intel build was getting from it, so it is built from source here
  # instead, cross compiled for x86_64 on the arm64 runner. Nothing x86_64 is ever RUN during the
  # build, only compiled and linked, so this needs no Rosetta either.
  #
  # PNG, BZip2 and Brotli support are switched OFF deliberately. They exist in freetype for colour
  # bitmap glyphs inside fonts, bzip2 compressed PCF fonts and WOFF2, and subtitle text rendering
  # uses none of the three. Turning them off removes libpng, libbz2 and libbrotli from the picture
  # altogether, which is three fewer things to cross compile and three fewer ways to end up with an
  # undefined symbol at link time.
  #
  # FT_DISABLE_ZLIB does NOT remove zlib: freetype's own wording is "disable use of system zlib and
  # use internal zlib library instead", so it keeps the feature and drops the dependency. That
  # matters here because freetype links ZLIB::ZLIB as PRIVATE, and a PRIVATE dependency of a STATIC
  # library is not carried to whoever links it, so a system zlib would leave tsMuxeR needing an
  # explicit -lz. With the internal copy the archive needs nothing outside libSystem.
  FREETYPE_VERSION=2.14.3
  DEPS="${PWD}/deps-x86_64"
  mkdir -p "${DEPS}/src"
  pushd "${DEPS}/src"
  curl -fsSL -o freetype.tar.xz \
    "https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.xz"
  tar xf freetype.tar.xz
  cmake -S "freetype-${FREETYPE_VERSION}" -B ft-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DCMAKE_INSTALL_PREFIX="${DEPS}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_ZLIB=TRUE \
    -DFT_DISABLE_PNG=TRUE \
    -DFT_DISABLE_BZIP2=TRUE \
    -DFT_DISABLE_BROTLI=TRUE \
    -DFT_DISABLE_HARFBUZZ=TRUE
  cmake --build ft-build --config Release
  cmake --install ft-build
  popd

  # A known answer, because the way this goes wrong is silent: CMake happily produces an arm64
  # library if the architecture flag does not reach it, and the only symptom is an undefined symbol
  # a hundred lines into the tsMuxeR link, which reads as a tsMuxeR fault.
  if ! lipo -info "${DEPS}/lib/libfreetype.a" | grep -q 'x86_64'; then
    echo "ERROR: the freetype built here is not x86_64:" >&2
    lipo -info "${DEPS}/lib/libfreetype.a" >&2
    exit 1
  fi
  echo "freetype for x86_64: $(lipo -info "${DEPS}/lib/libfreetype.a")"

  BREW_PREFIX="${DEPS}"
else
  brew install freetype
  BREW_PREFIX=$(brew --prefix)
  # Homebrew's freetype is built against libpng and bzip2, so a static link needs both named.
  FREETYPE_EXTRA_LDFLAGS="bz2;${BREW_PREFIX}/lib/libpng.a"
fi

mkdir build

pushd build
CMAKE_ARCH_FLAG=""
if [ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]; then
  CMAKE_ARCH_FLAG="-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
fi

cmake -DCMAKE_BUILD_TYPE=Release -DTSMUXER_STATIC_BUILD=TRUE \
  "-DFREETYPE_LDFLAGS=${FREETYPE_EXTRA_LDFLAGS}" -DTSMUXER_GUI=TRUE \
  -DWITHOUT_PKGCONFIG=TRUE ${CMAKE_ARCH_FLAG} \
  -DCMAKE_PREFIX_PATH="${BREW_PREFIX}" \
  -DFREETYPE_LIBRARY="${BREW_PREFIX}/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIR_freetype2="${BREW_PREFIX}/include/freetype2" \
  -DFREETYPE_INCLUDE_DIR_ft2build="${BREW_PREFIX}/include/freetype2" ..

if ! num_cores=$(sysctl -n hw.logicalcpu); then
  num_cores=1
fi

make -j${num_cores}

pushd tsMuxerGUI
pushd tsMuxerGUI.app/Contents
# avoid permission denied errors with Info.plist
chmod 664 "$PWD/Info.plist"
defaults write "$PWD/Info.plist" NSPrincipalClass -string NSApplication
defaults write "$PWD/Info.plist" NSHighResolutionCapable -string True
plutil -convert xml1 Info.plist
popd
macdeployqt tsMuxerGUI.app
popd

mkdir bin
pushd bin
mv ../tsMuxer/tsmuxer tsMuxeR
mv ../tsMuxerGUI/tsMuxerGUI.app .
cp tsMuxeR tsMuxerGUI.app/Contents/MacOS/

# Sign HERE, not right after macdeployqt. The copy above adds a file to the bundle, and adding
# anything to a signed bundle invalidates the signature, so signing earlier would be undone.
#
# An ad-hoc signature, which is what "-" means, is not a Developer ID and does not remove the
# unidentified developer prompt. What it does remove is "the application is damaged and cannot be
# opened", which is what an unsigned bundle produces on Apple Silicon. On arm64 every Mach-O must
# carry a signature, so the linker gives each binary an ad-hoc one, but the BUNDLE has none and
# that is what macOS objects to.
codesign --force --deep --sign - tsMuxerGUI.app
codesign --verify --deep --strict tsMuxerGUI.app

# -y stores symlinks as symlinks. Without it zip follows them and writes copies, which breaks a
# framework: Versions/Current has to be a link to Versions/A, and QtCore has to be a link to
# Versions/Current/QtCore. Written as real files the bundle is structurally invalid, which is a
# second route to "damaged", and it also stored every Qt library three times. Measured on the
# 2.18.9 arm64 package: 127 entries, zero symlinks, and 98 MB of duplication.
zip -9 -r -y mac.zip tsMuxeR tsMuxerGUI.app

# Check the package rather than trusting the flags, because both faults this replaces were
# silent: the build succeeded, the zip uploaded, and the app would not open. zipinfo marks a
# symlink with l in the first column.
links=$(zipinfo mac.zip | grep -c '^l' || true)
echo "symlinks stored in package: ${links}"
if [ "${links}" -eq 0 ]; then
  echo "ERROR: no symlinks in the package, so the .app frameworks are invalid" >&2
  exit 1
fi
if ! unzip -l mac.zip | grep -q '_CodeSignature/CodeResources'; then
  echo "ERROR: the .app is not signed, which reads as damaged on Apple Silicon" >&2
  exit 1
fi
popd
popd
