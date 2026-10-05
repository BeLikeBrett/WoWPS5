#!/usr/bin/env bash
# Build the Windows test programs of the single-process desktop work (MinGW).
#
#   tools/wine-ps5/build-desktop-tests.sh [output-dir]
#
# Default output: work/claude-partner/desktop/bin
#   win32-desktop-crt-test.exe     C runtime: stdio, heap, files, time, threads
#   win32-window-test.exe          window, message loop, GDI, timer, input
#   win32-vulkan-test.exe          Vulkan surface and swapchain on a window
#   win32-desktop-edge-test.exe    desktop corner cases; needs win32-desktop-edge-dll.dll beside it
#   win32-helper-probe.exe         what the missing helper processes cost (a measurement, not a test)
#
# The Vulkan test only needs the Vulkan headers, which are the same for every
# platform: the host's copy is used through a directory that holds nothing else,
# so no other host header can reach the MinGW compiler.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out="${1:-$root/work/claude-partner/desktop/bin}"
mkdir -p "$out"
cc=x86_64-w64-mingw32-gcc
flags=(-O2 -Wall)

"$cc" "${flags[@]}" "$root/tools/win32-desktop-crt-test.c" -o "$out/win32-desktop-crt-test.exe"
"$cc" "${flags[@]}" "$root/tools/win32-window-test.c" -luser32 -lgdi32 -o "$out/win32-window-test.exe"

vulkan_headers="${VULKAN_HEADERS:-/usr/include}"
[[ -f $vulkan_headers/vulkan/vulkan.h ]] || { echo "no vulkan/vulkan.h under $vulkan_headers (set VULKAN_HEADERS)" >&2; exit 1; }
mkdir -p "$out/.vulkan-include"
ln -sfn "$vulkan_headers/vulkan" "$out/.vulkan-include/vulkan"
[[ ! -d $vulkan_headers/vk_video ]] || ln -sfn "$vulkan_headers/vk_video" "$out/.vulkan-include/vk_video"
"$cc" "${flags[@]}" -isystem "$out/.vulkan-include" "$root/tools/win32-vulkan-test.c" -luser32 -o "$out/win32-vulkan-test.exe"

if [[ -f $root/tools/win32-desktop-edge-test.c ]]; then
    "$cc" "${flags[@]}" -shared -DEDGE_DLL "$root/tools/win32-desktop-edge-test.c" -luser32 -o "$out/win32-desktop-edge-dll.dll"
    "$cc" "${flags[@]}" "$root/tools/win32-desktop-edge-test.c" "$out/win32-desktop-edge-dll.dll" -luser32 -lgdi32 -o "$out/win32-desktop-edge-test.exe"
fi
"$cc" "${flags[@]}" "$root/tools/win32-helper-probe.c" -lws2_32 -liphlpapi -lole32 -lshell32 -luuid -lxinput -lbcrypt -ladvapi32 -luser32 \
    -o "$out/win32-helper-probe.exe"
ls -l "$out"/*.exe "$out"/*.dll 2>/dev/null
