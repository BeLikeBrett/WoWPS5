#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build/windows"
x86_64-w64-mingw32-gcc -O2 -nostdlib -fno-stack-protector \
    -Wl,--entry,WoWPS5Smoke -Wl,--enable-reloc-section \
    "$root/tools/pe-smoke.c" -lkernel32 -o "$root/build/windows/pe-smoke.exe"
x86_64-w64-mingw32-gcc -O2 -nostdlib -fno-stack-protector \
    -Wl,--entry,WoWPS5Win32Test -Wl,--enable-reloc-section \
    "$root/tools/win32-runtime-test.c" -lkernel32 -o "$root/build/windows/win32-runtime-test.exe"
x86_64-w64-mingw32-gcc -O2 -nostdlib -fno-stack-protector -fno-builtin \
    -Wl,--entry,WoWPS5MemoryTest -Wl,--enable-reloc-section \
    "$root/tools/win32-memory-test.c" -lkernel32 -lgcc -o "$root/build/windows/win32-memory-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-crt-test.c" -ladvapi32 -o "$root/build/windows/win32-crt-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-net-test.c" -lws2_32 -o "$root/build/windows/win32-net-test.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$root/tools/win32-tls-test.c" -lwinhttp -o "$root/build/windows/win32-tls-test.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$root/tools/win32-input-test.c" -luser32 -o "$root/build/windows/win32-input-test.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$root/tools/win32-selfconnect-test.c" -lws2_32 -o "$root/build/windows/win32-selfconnect-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-gdi-test.c" -lgdi32 -luser32 -o "$root/build/windows/win32-gdi-test.exe"
# Vulkan headers are any platform's copy; only vulkan/ is put on the include path
vulkan_headers="${VULKAN_HEADERS:-/usr/include}"
mkdir -p "$root/build/windows/.vulkan-include"
ln -sfn "$vulkan_headers/vulkan" "$root/build/windows/.vulkan-include/vulkan"
[[ ! -d $vulkan_headers/vk_video ]] || ln -sfn "$vulkan_headers/vk_video" "$root/build/windows/.vulkan-include/vk_video"
x86_64-w64-mingw32-gcc -O2 -isystem "$root/build/windows/.vulkan-include" "$root/tools/win32-vulkan-noscreen-test.c" -o "$root/build/windows/win32-vulkan-noscreen-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-d3d12-test.c" -ldxguid -o "$root/build/windows/win32-d3d12-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-d3d12-present-test.c" -ld3d12 -ldxgi -ldxguid -luser32 -o "$root/build/windows/win32-d3d12-present-test.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$root/tools/win32-audio-test.c" -lole32 -lwinmm -lntdll -o "$root/build/windows/win32-audio-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-file-test.c" -o "$root/build/windows/win32-file-test.exe"
x86_64-w64-mingw32-gcc -O2 "$root/tools/win32-sync-bench.c" -o "$root/build/windows/win32-sync-bench.exe"
