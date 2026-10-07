#!/usr/bin/env bash
# A full-featured Linux build of the single-process Wine (server as a thread,
# console memory rules), for working on windows, input and Vulkan on the PC.
#
#   tools/wine-ps5/build-desktop-host.sh [make-target...]
#
# Source: work/wine-desktop-src, a worktree of vendor/wine at the pinned
#   revision with runtime/wine-ps5/wine-ps5.patch and, when it exists,
#   runtime/wine-ps5/wine-ps5-desktop.patch. An existing worktree is used as it
#   is (it is where the desktop patch is developed); delete it to start again.
# Build: work/wine-desktop-host, configured with the defaults (X11, Wayland,
#   Vulkan, ...) and
#     CFLAGS="-g -O2 -DWINE_INPROC_SERVER -DWINE_PS5_MEMORY_MODEL"
#     x86_64_CFLAGS="-g -O2 -DWINE_PS5_MEMORY_MODEL"
#
# The in-process server goes into ntdll.so the way build-native-server.sh
# --host does it (that script is tied to vendor/wine): every server object is
# compiled with -fPIC, linked into one relocatable object whose only globals
# are the two entry points, and passed to the ntdll.so link through
# NTDLL_PLATFORM_LIBS. makedep expands that variable into the generated
# Makefile, so it is given to `make depend` and to every later make.
#
# With no target, everything is built. loader/wine-preloader is removed at the
# end: the console has no preloader and its reservations are 4 KiB-granular.
#
# JOBS (default 8) is the make parallelism.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source="$root/work/wine-desktop-src"
build="$root/work/wine-desktop-host"
runtime="$root/runtime/wine-ps5"
revision=455e3509b98a6919fd4ad1def4803e08c41c03b2
jobs="${JOBS:-8}"
entry_points=(wine_server_call_direct wine_server_inproc_main wine_server_thread_main)

fail() { echo "FAIL: $*" >&2; exit 1; }

if [[ ! -d $source ]]; then
    git -C "$root/vendor/wine" worktree add --detach "$source" "$revision"
    git -C "$source" apply "$runtime/wine-ps5.patch"
    [[ ! -f $runtime/wine-ps5-desktop.patch ]] || git -C "$source" apply "$runtime/wine-ps5-desktop.patch"
    skeleton="$root/work/claude-partner/desktop/minimal-driver/wineskel-drv.patch"
    [[ ! -f $skeleton ]] || git -C "$source" apply "$skeleton"
fi
[[ $(git -C "$source" rev-parse HEAD) == "$revision" ]] || fail "$source is not at $revision"
[[ -f $source/server/inproc.c ]] || fail "$source does not have wine-ps5.patch applied"
bash "$root/tools/wine-ps5/sync-memory-layer.sh" "$source"

mkdir -p "$build"
if [[ ! -f $build/Makefile ]]; then
    (cd "$build" && "$source/configure" --enable-win64 --enable-archs=x86_64 --disable-tests \
        CFLAGS="-g -O2 -DWINE_INPROC_SERVER -DWINE_PS5_MEMORY_MODEL" \
        x86_64_CFLAGS="-g -O2 -DWINE_PS5_MEMORY_MODEL" > configure.log 2>&1) \
        || { tail -30 "$build/configure.log" >&2; exit 1; }
fi

server_obj="$build/server/wineserver-inproc.o"
platform_libs="$server_obj"

# the object list is the server's own source list
mapfile -t objs < <(sed -n '/^SOURCES/,/^$/p' "$source/server/Makefile.in" \
                    | grep -o '[a-z0-9_]*\.c\b' | sed 's/\.c$/.o/; s#^#server/#')
(( ${#objs[@]} > 40 )) || fail "could not read the server source list"

mk() { make -C "$build" -j"$jobs" NTDLL_PLATFORM_LIBS="$platform_libs" "$@"; }

# 1. Makefile with NTDLL_PLATFORM_LIBS baked into the ntdll.so rule
#    The skeleton driver dlls/wineskel.drv (optional, not part of the desktop
#    patch) is not known to configure: its directory is added to the list the
#    generated Makefile gives makedep.
depend=0
grep -q 'wineserver-inproc.o' "$build/Makefile" || depend=1
if [[ -d $source/dlls/wineskel.drv ]] && ! grep -q 'dlls/wineskel.drv' "$build/Makefile"; then
    sed -i 's|^\tdlls/winewayland.drv \\$|&\n\tdlls/wineskel.drv \\|' "$build/Makefile"
    grep -q 'dlls/wineskel.drv' "$build/Makefile" || fail "could not add dlls/wineskel.drv to the Makefile"
    depend=1
fi
if (( depend )); then
    mk depend > "$build/desktop-depend.log" 2>&1 || { tail -20 "$build/desktop-depend.log" >&2; exit 1; }
    grep -q 'wineserver-inproc.o' "$build/Makefile" || fail "NTDLL_PLATFORM_LIBS did not reach the generated Makefile"
fi

# 2. position-independent server objects. A stamp records that the objects in
#    the tree were made by this step: a plain make would build them without -fPIC.
host_cflags="$(sed -n 's/^CFLAGS = //p' "$build/Makefile")"
[[ $host_cflags == *-DWINE_INPROC_SERVER* ]] || fail "$build is not configured with -DWINE_INPROC_SERVER"
[[ -f $build/server/.pic-objects ]] || rm -f "${objs[@]/#/$build/}"
mk "CFLAGS=$host_cflags -fPIC" "${objs[@]}" > "$build/desktop-server.log" 2>&1 \
    || { grep -nE 'error|undefined' "$build/desktop-server.log" | head -40 >&2; exit 1; }
touch "$build/server/.pic-objects"

# 3. one relocatable object, entry points only
newest=$(ls -t "${objs[@]/#/$build/}" | head -1)
if [[ ! -f $server_obj || $newest -nt $server_obj ]]; then
    ld -r -o "$server_obj.tmp" "${objs[@]/#/$build/}"
    keep=(); for e in "${entry_points[@]}"; do keep+=("--keep-global-symbol=$e"); done
    objcopy "${keep[@]}" "$server_obj.tmp" "$server_obj"
    rm -f "$server_obj.tmp"
fi
globals="$(nm -g --defined-only "$server_obj" | awk '{print $NF}' | sort | tr '\n' ' ')"
[[ $globals == "wine_server_call_direct wine_server_inproc_main wine_server_thread_main " ]] \
    || fail "unexpected global symbols in wineserver-inproc.o: $globals"

# 4. the rest. The server object is a library argument of the ntdll.so link, not
#    a prerequisite make knows about.
ntdll_so="$build/dlls/ntdll/ntdll.so"
[[ ! -f $ntdll_so || ! $server_obj -nt $ntdll_so ]] || rm -f "$ntdll_so"
mk "$@"
rm -f "$build/loader/wine-preloader"
nm --defined-only "$build/dlls/ntdll/ntdll.so" | grep ' wine_server_inproc_main$' > /dev/null \
    || fail "ntdll.so does not contain the in-process server"
echo "OK: $build"
