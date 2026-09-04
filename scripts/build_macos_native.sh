#!/usr/bin/env bash

set -ex

export MACOSX_DEPLOYMENT_TARGET=10.15

if [ "${CMAKE_OSX_ARCHITECTURES:-}" = "x86_64" ]; then
  # Install a separate x86_64 Homebrew at /usr/local for Intel libraries
  NONINTERACTIVE=1 arch -x86_64 /bin/bash -c \
    "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  X86_BREW=/usr/local/bin/brew
  arch -x86_64 $X86_BREW install freetype
  BREW_PREFIX=$($X86_BREW --prefix)
else
  brew install freetype
  BREW_PREFIX=$(brew --prefix)
fi

mkdir build

pushd build
CMAKE_ARCH_FLAG=""
if [ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]; then
  CMAKE_ARCH_FLAG="-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
fi

cmake -DCMAKE_BUILD_TYPE=Release -DTSMUXER_STATIC_BUILD=TRUE \
  "-DFREETYPE_LDFLAGS=bz2;${BREW_PREFIX}/lib/libpng.a" -DTSMUXER_GUI=TRUE \
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
