#!/usr/bin/env bash
# Put the PS5 memory layer where Wine's build expects its sources.
#
#   tools/wine-ps5/sync-memory-layer.sh <wine-source-tree>
#
# runtime/wine-ps5/ps5_mman.{c,h} and ps5_mman_model.c are the originals;
# wine-ps5.patch lists them in dlls/ntdll/Makefile.in, and
# Wine's makedep needs them inside the tree, marked as Unix-side sources.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
target="$1/dlls/ntdll/unix"
[[ -d $target ]] || { echo "not a Wine source tree: $1" >&2; exit 2; }
for file in ps5_mman.c ps5_mman_model.c; do
    { printf '#if 0\n#pragma makedep unix\n#endif\n'; cat "$root/runtime/wine-ps5/$file"; } > "$target/$file.new"
    cmp -s "$target/$file.new" "$target/$file" && rm "$target/$file.new" || mv "$target/$file.new" "$target/$file"
done
cmp -s "$root/runtime/wine-ps5/ps5_mman.h" "$target/ps5_mman.h" || cp "$root/runtime/wine-ps5/ps5_mman.h" "$target/ps5_mman.h"
