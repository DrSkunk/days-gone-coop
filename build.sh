#!/usr/bin/env bash
# Cross-compiles the co-op plugin (Windows x64 DLL) with MinGW-w64.
set -euo pipefail
cd -- "$(dirname -- "$0")"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
mkdir -p package
version=$(cat dependencies/imgui.version)
archive=".deps/imgui-${version}.tar.gz"
source=".deps/imgui-${version#v}"
if [ ! -f "$source/imgui.cpp" ]; then
  mkdir -p .deps
  curl --fail --location --proto '=https' --tlsv1.2 \
    "https://github.com/ocornut/imgui/archive/refs/tags/${version}.tar.gz" -o "$archive"
  actual=$(shasum -a 256 "$archive" | cut -d ' ' -f1)
  test "$actual" = "$(cat dependencies/imgui.sha256)"
  tar -xzf "$archive" -C .deps
fi
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -shared -static -s \
  -I"$source" -o package/DaysGoneCoop.asi src/coop/*.cpp \
  "$source"/imgui.cpp "$source"/imgui_draw.cpp "$source"/imgui_tables.cpp "$source"/imgui_widgets.cpp \
  "$source"/backends/imgui_impl_dx11.cpp \
  -lws2_32 -lbcrypt -ld3d11 -ld3dcompiler -ldxgi -Wl,--subsystem,windows
cp config/DaysGoneCoop.ini package/DaysGoneCoop.ini
cp "$source/LICENSE.txt" package/LICENSE-Dear-ImGui.txt
echo "built package/DaysGoneCoop.asi"
