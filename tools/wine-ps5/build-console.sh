#!/usr/bin/env bash
# Everything from the patched Wine source to a deployed title with Wine inside.
#
#   tools/wine-ps5/build-console.sh [--no-deploy]
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
export PS5_VULKAN_DIR="$root/vendor/PS5_Vulkan" PS5_PAYLOAD_SDK_FORK="$root/vendor/PS5_PayloadSDK"
export PKG_CONFIG_PATH="$root/.deps/host-tools/usr/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$root/.deps/host-tools/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
bash "$root/tools/wine-ps5/build-tls.sh"
if ! grep -q 'SONAME_LIBGNUTLS "static-ps5"' "$root/work/wine-ps5-build-check/include/config.h"; then
    (
        cd "$root/work/wine-ps5-build-check"
        GNUTLS_CFLAGS="-I$root/build/ps5-tls/include" GNUTLS_LIBS="-L$root/build/ps5-tls/lib -lgnutls" \
            ac_cv_lib_soname_gnutls=static-ps5 ./config.status --recheck > "$root/work/wine-tls-configure.log" 2>&1
        ./config.status >> "$root/work/wine-tls-configure.log" 2>&1
    )
fi
bash "$root/tools/wine-ps5/sync-memory-layer.sh" "$root/vendor/wine"
bash "$root/tools/wine-ps5/build-native-server.sh" "$root/work/claude-partner/wineserver" | tail -3
# the other Unix libraries the title links (the list is in build-title-object.sh)
# (win32u's own .so link fails on -lm, which a title does not have: only its objects are wanted)
make -C "$root/work/wine-ps5-build-check" -j8 -k dlls/ws2_32/ws2_32.so dlls/win32u/win32u.so dlls/winevulkan/winevulkan.so dlls/crypt32/unixlib.o dlls/secur32/schannel_gnutls.o dlls/xinput1_3/unixlib.o include/audioclient.h include/mmdeviceapi.h > "$root/work/wine-unixlibs-build.log" 2>&1 || true
grep -qP "error: (?!unable to find library -lm|linker command failed)" "$root/work/wine-unixlibs-build.log" &&
    { grep -nE 'error|undefined' "$root/work/wine-unixlibs-build.log" | head -20; exit 1; }
bash "$root/tools/wine-ps5/build-title-object.sh"
bash "$root/app/ps5/tools/build.sh" > "$root/work/wine-title-build.log" 2>&1 || { grep -nE 'error|undefined' "$root/work/wine-title-build.log" | head -30; exit 1; }
mkdir -p "$root/app/dist/PPSA99220/licenses/tls"
cp -R "$root/build/ps5-tls/licenses/." "$root/app/dist/PPSA99220/licenses/tls/"
cp "$root/build/ps5-tls/PROVENANCE.txt" "$root/app/dist/PPSA99220/licenses/tls/"
cp "$root/tools/wine-ps5/build-tls.sh" "$root/app/dist/PPSA99220/licenses/tls/build-sources.sh"
bash "$root/tools/build-pe-smoke.sh"
cp "$root/build/windows/pe-smoke.exe" "$root/app/dist/PPSA99220/pe-smoke.exe"
sha256sum "$root/app/dist/PPSA99220/eboot.bin"
[[ ${1:-} == --no-deploy ]] || bash "$root/tools/console.sh" deploy | tail -1
