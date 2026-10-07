#!/usr/bin/env bash
# Regenerate runtime/wine-ps5/wine-ps5.patch from the vendor/wine working tree
# and check that it reproduces that tree from the pinned revision.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
wine="$root/vendor/wine"
patch="$root/runtime/wine-ps5/wine-ps5.patch"
layer=(':!dlls/ntdll/unix/ps5_mman.c' ':!dlls/ntdll/unix/ps5_mman.h' ':!dlls/ntdll/unix/ps5_mman_model.c')
mapfile -t added < <(git -C "$wine" ls-files --others --exclude-standard -- . "${layer[@]}")
(( ${#added[@]} )) && git -C "$wine" add -N -- "${added[@]}"
git -C "$wine" diff -- . "${layer[@]}" > "$patch"
(( ${#added[@]} )) && git -C "$wine" reset -q -- "${added[@]}"
check=$(mktemp -d)
trap 'git -C "$wine" worktree remove --force "$check/tree" 2>/dev/null; rm -rf "$check"' EXIT
git -C "$wine" worktree add --detach "$check/tree" HEAD > /dev/null 2>&1
git -C "$check/tree" apply "$patch"
bash "$root/tools/wine-ps5/sync-memory-layer.sh" "$check/tree"
if diff -rq -x .git "$check/tree" "$wine" | grep .; then echo "the patch does not reproduce vendor/wine" >&2; exit 1; fi
echo "$patch: $(grep -c '^diff --git' "$patch") files, reproduces vendor/wine"
