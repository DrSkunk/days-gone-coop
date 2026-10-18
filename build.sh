#!/usr/bin/env bash
# Cross-compiles the co-op plugin (Windows x64 DLL) with MinGW-w64.
set -euo pipefail
cd -- "$(dirname -- "$0")"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
mkdir -p package
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -shared -static -s \
  -o package/DaysGoneCoop.asi src/coop/*.cpp \
  -lws2_32 -Wl,--subsystem,windows
cp config/DaysGoneCoop.ini package/DaysGoneCoop.ini
echo "built package/DaysGoneCoop.asi"
