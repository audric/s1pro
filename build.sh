#!/bin/sh
# Build s1pro on Linux (PC or Raspberry Pi OS, 64-bit). Needs BlueZ, which both ship with.
#   sudo apt install g++ cmake ninja-build qt6-base-dev
set -e
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo "built: $(pwd)/build/s1pro"
