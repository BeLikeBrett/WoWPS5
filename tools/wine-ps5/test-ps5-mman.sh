#!/usr/bin/env bash
# Host test of the PS5 memory layer against the Linux model of the console's kernel.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
mkdir -p "$root/build/host"
cc -O1 -g -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=undefined -I"$root/runtime/wine-ps5" \
    "$root/tools/wine-ps5/test-ps5-mman.c" "$root/runtime/wine-ps5/ps5_mman.c" "$root/runtime/wine-ps5/ps5_mman_model.c" \
    -o "$root/build/host/test-ps5-mman"
exec "$root/build/host/test-ps5-mman"
