#!/bin/zsh
# Build FrameLift and its installer (dist/FrameLift-Setup-<version>.exe). Needs: mingw-w64, cmake, ninja, makensis.
set -e
cd "${0:A:h}/.."
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C build-mingw
ver=$(sed -n 's/.*project(FrameLift VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
mkdir -p dist
(cd installer && makensis -V2 -DVERSION=$ver framelift.nsi)
ls -la dist/FrameLift-Setup-$ver.exe
