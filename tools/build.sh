#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PS5_VULKAN_DIR="$root/vendor/PS5_Vulkan"
export PS5_PAYLOAD_SDK_FORK="$root/vendor/PS5_PayloadSDK"
export PKG_CONFIG_PATH="$root/.deps/host-tools/usr/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$root/.deps/host-tools/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[[ -d $PS5_VULKAN_DIR ]] || { echo 'Run python3 tools/bootstrap.py first.' >&2; exit 2; }
bash "$PS5_VULKAN_DIR/tools/setup-native-dependencies.sh"
bash "$PS5_VULKAN_DIR/tools/build-radv.sh" release
bash "$PS5_VULKAN_DIR/tools/rebuild-libc.sh"
mkdir -p "$PS5_VULKAN_DIR/build/host"
cp "$PS5_VULKAN_DIR/build/runtime-shim/ps5-native-tool" "$PS5_VULKAN_DIR/build/host/ps5-native-tool"
bash "$root/app/ps5/tools/compile-shaders.sh" wowps5probe
bash "$root/app/ps5/tools/build.sh"
bash "$root/tools/build-pe-smoke.sh"
cp "$root/build/windows/pe-smoke.exe" "$root/app/dist/PPSA99220/pe-smoke.exe"
