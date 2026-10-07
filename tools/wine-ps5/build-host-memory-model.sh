#!/usr/bin/env bash
# A Linux build of Wine that runs under the console's memory rules.
#
#   tools/wine-ps5/build-host-memory-model.sh [make-target...]
#
# Wine's virtual.c is built with WINE_PS5_MEMORY_MODEL: runtime/wine-ps5/ps5_mman.c
# over the Linux model of the console's kernel (ps5_mman_model.c), 16 KiB host
# pages, nothing at or above 1 TiB, executable memory only through the layer.
# The source is a separate worktree of vendor/wine at the pinned revision with
# runtime/wine-ps5/wine-ps5.patch, so vendor/wine and the PS5 cross-build tree
# are left alone. Without -DWINE_INPROC_SERVER the server stays a separate
# program here, so Wine can start child processes and make a whole prefix. This shows how Windows programs behave
# under those rules on the PC. It establishes nothing about the console.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source="$root/work/wine-virtual-src"
build="$root/work/wine-memsim-host"
runtime="$root/runtime/wine-ps5"
revision=455e3509b98a6919fd4ad1def4803e08c41c03b2

[[ -d $source ]] || git -C "$root/vendor/wine" worktree add --detach "$source" "$revision"
[[ $(git -C "$source" rev-parse HEAD) == "$revision" ]] || { echo "$source is not at $revision" >&2; exit 1; }
if ! git -C "$source" apply --reverse --check "$runtime/wine-ps5.patch" 2>/dev/null; then
    # an older or partial patch state: start again from the pinned revision
    git -C "$source" checkout -q -- . && git -C "$source" clean -fdq
    git -C "$source" apply "$runtime/wine-ps5.patch"
fi
bash "$root/tools/wine-ps5/sync-memory-layer.sh" "$source"

mkdir -p "$build"
if [[ ! -f $build/Makefile ]]; then
    (cd "$build" && "$source/configure" --enable-win64 --enable-archs=x86_64 --disable-tests \
        CFLAGS="-g -O2 -DWINE_PS5_MEMORY_MODEL" x86_64_CFLAGS="-g -O2 -DWINE_PS5_MEMORY_MODEL" > configure.log 2>&1) || { tail -30 "$build/configure.log" >&2; exit 1; }
fi
make -C "$build" -j"$(nproc)" "$@"
# The console has no preloader, and the areas it reserves are 4 KiB-granular:
# without the binary Wine's loader runs directly, as it will there.
rm -f "$build/loader/wine-preloader"
