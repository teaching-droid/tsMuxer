#!/usr/bin/env bash
set -euo pipefail

# Build tsMuxer for Windows 64-bit with Qt6 and Windows 7 compatibility
# Uses pre-built qt6windows7 binaries for Windows 7 support

QT_VERSION="6.8.3"
QT_ARCH="x86_64"
WORK_DIR="$(mktemp -d)"
trap "rm -rf $WORK_DIR" EXIT

mkdir -p build
cd build || exit

# Download pre-built qt6windows7 binaries
echo "Downloading Qt6 Windows 7 compatible binaries..."
QT_URL="https://github.com/crystalidea/qt6windows7/releases/download/${QT_VERSION}/qt-${QT_VERSION}-win64-msvc2022-static.zip"
if ! curl -L -o "$WORK_DIR/qt6.zip" "$QT_URL" 2>/dev/null; then
  echo "Warning: Could not download qt6windows7 binaries, falling back to installed Qt6"
  QT_PATH="${Qt6_DIR:-.}"
else
  unzip -q "$WORK_DIR/qt6.zip" -d "$WORK_DIR"
  QT_PATH="$WORK_DIR/qt-${QT_VERSION}"
fi

# Build with Qt6
cmake ../ \
  -DCMAKE_PREFIX_PATH="$QT_PATH" \
  -DTSMUXER_GUI=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

mkdir -p ../bin
cp tsMuxer/Release/tsmuxer.exe ../bin/tsMuxeR.exe
if [ -f tsMuxerGUI/Release/tsmuxergui.exe ]; then
  cp tsMuxerGUI/Release/tsmuxergui.exe ../bin/tsMuxerGUI.exe
fi

cd .. || exit

echo "Build complete: bin/tsMuxeR.exe"
if [ -f bin/tsMuxerGUI.exe ]; then
  echo "GUI build complete: bin/tsMuxerGUI.exe"
fi
