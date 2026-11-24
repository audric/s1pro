#!/bin/sh
# Build s1pro for the machine this runs on: Linux PC (x86_64) or Raspberry Pi OS 64-bit (arm64).
set -e
cd "$(dirname "$0")"

missing=""
need() { command -v "$1" >/dev/null 2>&1 || missing="$missing $2"; }
need g++ g++
need cmake cmake
need ninja ninja-build
# Qt6 development files (Widgets and DBus come together in qt6-base-dev)
if ! find /usr/lib /usr/lib64 /usr/local/lib -name Qt6WidgetsConfig.cmake 2>/dev/null | grep -q . \
   && [ -z "$CMAKE_PREFIX_PATH" ] && [ -z "$Qt6_DIR" ]; then
    missing="$missing qt6-base-dev"
fi
if [ -n "$missing" ]; then
    echo "Missing build dependencies:$missing" >&2
    echo "On Debian, Ubuntu or Raspberry Pi OS install them with:" >&2
    echo "    sudo apt install$missing" >&2
    echo "(other distributions: the equivalent packages for a C++ compiler, CMake, Ninja and Qt 6 base)" >&2
    exit 1
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo "built for $(uname -m): $(pwd)/build/s1pro"
command -v bluetoothctl >/dev/null 2>&1 || echo "note: BlueZ (package 'bluez') is needed to run it and was not found." >&2
