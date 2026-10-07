#!/usr/bin/env bash
# Wine's native side as one object for the title's link.
#
#   tools/wine-ps5/build-title-object.sh
#
# A title has no general dlopen for Wine's Unix libraries, so they are linked
# into the title itself. ntdll's Unix objects, the memory layer and the
# in-process server become one relocatable object, wine-unix.o, whose only
# global symbols are __wine_main, the memory layer (ps5w_*) and what the other
# Unix libraries import from ntdll. Each other Unix library (UNIXLIBS below)
# becomes an object whose only global is its call table, renamed after the
# library; build/ps5-wine/unixlibs.c lists them for ntdll's loader, which looks
# a library up there instead of calling dlopen. Build the objects first
# (tools/wine-ps5/build-native-server.sh, and make <dir>/<name>.so in the cross tree).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build="$root/work/wine-ps5-build-check"
sdk="$root/app/.deps/native/ps5-payload-sdk"
out="$root/build/ps5-wine"
mkdir -p "$out"
# name:directory of each Unix library linked into the title besides ntdll (objects in
# the directory and its subdirectories). A library offers a call table, an init
# entry, or both.
UNIXLIBS=(ws2_32:dlls/ws2_32 win32u:dlls/win32u winevulkan:dlls/winevulkan crypt32:dlls/crypt32 secur32:dlls/secur32 xinput1_3:dlls/xinput1_3 wineps5:dlls/wineps5)

# The audio driver is not in Wine's makefiles (mmdevapi loads a driver's Unix
# library by name, and this one exists only for the title): compiled here into
# the directory the loop below scans.
mkdir -p "$build/dlls/wineps5"
"$sdk/bin/prospero-clang" -m64 -c -o "$build/dlls/wineps5/wineps5_audio.o" "$root/runtime/wine-ps5/wineps5_audio.c" \
    -I"$root/vendor/wine/dlls/mmdevapi" -I"$build/include" -I"$root/vendor/wine/include" -D__WINESRC__ -DWINE_UNIX_LIB \
    -Wall -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -fPIC -fasynchronous-unwind-tables -g -O2

# Calls no system module gives a title go to the platform layer's own (ps5_*)
# or to this project's (wowps5_*, app/runtime/wine_title.c). Renamed in the
# objects, so the title defines no name a system stub library also has.
redefine=()
for name in umask getpwuid if_nametoindex times; do redefine+=("--redefine-sym=$name=ps5_$name"); done
for name in clock_gettime write read pread pwrite lseek fstat ftruncate fsync mmap close fchdir chdir getcwd realpath open openat fstatat stat lstat access mkdir rmdir unlink rename opendir fopen \
        fstatfs getfsstat isatty getmntinfo cfgetospeed cfsetispeed cfsetospeed extattr_get_fd extattr_get_file \
        extattr_set_fd extattr_delete_fd thr_set_name pipe2 dup fcntl ioctl __h_errno gethostbyaddr gethostbyname getnameinfo \
        getaddrinfo freeaddrinfo gethostname; do redefine+=("--redefine-sym=$name=wowps5_$name"); done
redefine+=("--redefine-sym=exit=wowps5_exit" "--redefine-sym=_exit=wowps5_exit")

# First each library as one raw object, and what all of them import. Then each is
# localised: only its entries stay global, and what another library imports from it
# (winevulkan calls win32u, as their shared objects would).
: > "$out/unixlib-imports.txt"
for entry in "${UNIXLIBS[@]}"; do
    name=${entry%%:*} directory=${entry#*:}
    mapfile -t parts < <(find "$build/$directory" -name '*.o' ! -path '*/x86_64-windows/*' ! -path '*/tests/*' | LC_ALL=C sort)
    (( ${#parts[@]} )) || { echo "no objects in $build/$directory: make $directory/$name.so there first" >&2; exit 1; }
    "$sdk/bin/ld.lld" -r -o "$out/$name-unix.tmp.o" "${parts[@]}"
    "$sdk/bin/llvm-nm" -u "$out/$name-unix.tmp.o" | awk '{print $NF}' | LC_ALL=C sort -u > "$out/$name-unix.imports.txt"
    cat "$out/$name-unix.imports.txt" >> "$out/unixlib-imports.txt"
done
LC_ALL=C sort -u -o "$out/unixlib-imports.txt" "$out/unixlib-imports.txt"

library_objects=()
declarations=() rows=()
for entry in "${UNIXLIBS[@]}"; do
    name=${entry%%:*}
    # read whole before searching: grep -q closing the pipe early fails the pipeline under pipefail
    globals=$("$sdk/bin/llvm-nm" -g --defined-only "$out/$name-unix.tmp.o")
    grep -qE ' (__wine_unix_call_funcs|__wine_unix_lib_init)$' <<<"$globals" ||
        { echo "$name has neither __wine_unix_call_funcs nor __wine_unix_lib_init" >&2; exit 1; }
    keep=(--keep-global-symbol=__wine_unix_call_funcs --keep-global-symbol=__wine_unix_lib_init)
    while read -r symbol; do
        [[ $symbol == __wine_unix_call_funcs || $symbol == __wine_unix_lib_init ]] || keep+=("--keep-global-symbol=$symbol")
    done < <(LC_ALL=C comm -12 "$out/unixlib-imports.txt" <(awk '{print $NF}' <<<"$globals" | LC_ALL=C sort -u))
    # two passes: localise, then give the two entries the library's name
    "$sdk/bin/llvm-objcopy" "${keep[@]}" "${redefine[@]}" "$out/$name-unix.tmp.o" "$out/$name-unix.o"
    "$sdk/bin/llvm-objcopy" --redefine-sym=__wine_unix_call_funcs="wowps5_${name}_unix_call_funcs" \
        --redefine-sym=__wine_unix_lib_init="wowps5_${name}_unix_lib_init" "$out/$name-unix.o"
    rm -f "$out/$name-unix.tmp.o"
    library_objects+=("$out/$name-unix.o")
    # the table names only what the library has: the title converter refuses an undefined name, weak or not
    funcs=0 init=0
    if grep -q ' __wine_unix_call_funcs$' <<<"$globals"; then
        funcs="wowps5_${name}_unix_call_funcs"; declarations+=("extern const char $funcs[];")
    fi
    if grep -q ' __wine_unix_lib_init$' <<<"$globals"; then
        init="wowps5_${name}_unix_lib_init"; declarations+=("extern int $init(void);")
    fi
    rows+=("    { \"$name.so\", $funcs, $init },")
done
{
    echo "/* Generated by tools/wine-ps5/build-title-object.sh: the Unix libraries linked into the title. */"
    echo "struct ps5_unixlib { const char *name; const void *funcs; int (*init)(void); };"
    printf '%s\n' "${declarations[@]}"
    echo "const struct ps5_unixlib wowps5_unixlibs[] = {"
    printf '%s\n' "${rows[@]}"
    # All XInput versions share this native call table; their PE exports differ.
    for version in 1_1 1_2 1_4; do
        echo "    { \"xinput${version}.so\", wowps5_xinput1_3_unix_call_funcs, 0 },"
    done
    echo "    { 0, 0, 0 }"
    echo "};"
} > "$out/unixlibs.c"

objects=()
for object in "$build"/dlls/ntdll/unix/*.o; do
    case "$object" in */ps5_mman_model.o|*/signal_arm.o|*/signal_arm64.o|*/signal_i386.o) continue ;; esac
    objects+=("$object")
done
objects+=("$build/server/wineserver-inproc.o")
"$sdk/bin/ld.lld" -r -o "$out/wine-unix.tmp.o" "${objects[@]}"
# what the other Unix libraries import from ntdll stays global
"$sdk/bin/llvm-nm" -g --defined-only "$out/wine-unix.tmp.o" | awk '{print $NF}' | LC_ALL=C sort -u > "$out/ntdll-globals.txt"
keep=()
while read -r name; do keep+=("--keep-global-symbol=$name"); done < <(LC_ALL=C comm -12 "$out/unixlib-imports.txt" "$out/ntdll-globals.txt")
echo "ntdll symbols kept for the other Unix libraries: ${#keep[@]}"
"$sdk/bin/llvm-objcopy" --wildcard --keep-global-symbol=__wine_main --keep-global-symbol="ps5w_*" "${keep[@]}" "${redefine[@]}" \
    "$out/wine-unix.tmp.o" "$out/wine-unix.o"
rm -f "$out/wine-unix.tmp.o"
# every library in one object for the title's link; the imports are what the console must provide
"$sdk/bin/ld.lld" -r -o "$out/wine-unix.ntdll.o" "$out/wine-unix.o"
tls="$root/build/ps5-tls/lib"
"$sdk/bin/prospero-clang" -O2 -fPIC -I"$root/build/ps5-tls/include" \
    -c "$root/app/runtime/wine_input.c" -o "$out/wine-input.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$root/app/runtime/wine_devices.c" -o "$out/wine-devices.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$root/app/runtime/wine_audio.c" -o "$out/wine-audio.o"
"$sdk/bin/ld.lld" -r -o "$out/wine-unix.o" "$out/wine-unix.ntdll.o" "${library_objects[@]}" \
    "$out/wine-input.o" "$out/wine-devices.o" "$out/wine-audio.o" "$tls/libgnutls.a" "$tls/libhogweed.a" "$tls/libnettle.a" "$tls/libgmp.a"
# Apply the filesystem wrappers to the TLS dependencies as well as Wine.
"$sdk/bin/llvm-objcopy" "${redefine[@]}" "$out/wine-unix.o"
rm -f "$out/wine-unix.ntdll.o"
"$sdk/bin/llvm-nm" -u "$out/wine-unix.o" | awk '{print $NF}' | LC_ALL=C sort -u > "$out/wine-unix.imports.txt"
echo "$out/wine-unix.o: $(stat -c %s "$out/wine-unix.o") bytes, $(wc -l < "$out/wine-unix.imports.txt") imports"
# The stub libraries list what newer firmware exports. A name this console's
# modules lack is left pointing nowhere and faults at its first call, so the
# title checks every one of these at start (app/runtime/wine_contract_probe.c).
{
    echo "/* Generated by tools/wine-ps5/build-title-object.sh: Wine's imports, for the console audit. */"
    echo "#include <stddef.h>"
    index=0
    while read -r name; do
        echo "extern char wowps5_import_$index __asm__(\"$name\");"
        index=$((index + 1))
    done < "$out/wine-unix.imports.txt"
    echo "const struct { const char *name; const void *address; } wowps5WineImports[] = {"
    index=0
    while read -r name; do
        echo "    { \"$name\", &wowps5_import_$index },"
        index=$((index + 1))
    done < "$out/wine-unix.imports.txt"
    echo "};"
    echo "const size_t wowps5WineImportCount = sizeof(wowps5WineImports) / sizeof(*wowps5WineImports);"
} > "$out/import-audit.c"
