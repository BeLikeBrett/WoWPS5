#!/usr/bin/env bash
# Rebuild Wine's native (Unix-side) ntdll.so for the PS5 in the existing
# cross-build tree and report what the ELF needs at load time.
#
#   tools/wine-ps5/build-native-ntdll.sh [--apply-patch] [log-dir]
#
# --apply-patch applies runtime/wine-ps5/*.patch to vendor/wine first (skipped
# if already applied). The build tree is work/wine-ps5-build-check, configured
# by the parent for --host=x86_64-unknown-freebsd9 with prospero-clang; this
# script does not reconfigure it.
#
# NTDLL_PLATFORM_LIBS must be passed on every make run: makedep expands it into
# the generated Makefile, and a regeneration without it would drop the unwinder.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WINE_SRC="${ROOT}/vendor/wine"
BUILD="${ROOT}/work/wine-ps5-build-check"
RUNTIME="${ROOT}/runtime/wine-ps5"
SDK="${ROOT}/app/.deps/native/ps5-payload-sdk"
EXPECTED_REV=455e3509b98a6919fd4ad1def4803e08c41c03b2

apply=0
if [[ "${1:-}" == "--apply-patch" ]]; then apply=1; shift; fi
LOGDIR="${1:-${BUILD}}"
mkdir -p "${LOGDIR}"

rev="$(git -C "${WINE_SRC}" rev-parse HEAD)"
[[ "${rev}" == "${EXPECTED_REV}" ]] || { echo "vendor/wine is at ${rev}, expected ${EXPECTED_REV}" >&2; exit 1; }

if (( apply )); then
    for p in "${RUNTIME}"/*.patch; do
        if git -C "${WINE_SRC}" apply --reverse --check "${p}" 2>/dev/null; then
            echo "already applied: ${p##*/}"
        else
            git -C "${WINE_SRC}" apply "${p}"
            echo "applied: ${p##*/}"
        fi
    done
fi

# The SDK's libunwind.a is single-image (bounds from linker symbols); the
# script gives it ntdll.so's own .eh_frame, and --exclude-libs keeps its
# _Unwind_* entry points out of ntdll.so's dynamic symbol table.
NTDLL_PLATFORM_LIBS="-Wl,-T,${RUNTIME}/ntdll-eh-frame.ld -Wl,--exclude-libs,libunwind.a -lunwind"

# With wine-ps5.patch applied the server is part of the same
# image: ntdll.so calls wine_server_inproc_main, which lives in the object that
# build-native-server.sh makes. The value must match that script's exactly.
if [[ -f "${WINE_SRC}/server/inproc.c" ]]; then
    SERVER_OBJ="${BUILD}/server/wineserver-inproc.o"
    [[ -f "${SERVER_OBJ}" ]] || { echo "missing ${SERVER_OBJ}: run tools/wine-ps5/build-native-server.sh first" >&2; exit 1; }
    NTDLL_PLATFORM_LIBS="${NTDLL_PLATFORM_LIBS} ${SERVER_OBJ}"
fi

log="${LOGDIR}/native-ntdll-build-ps5.log"
set +e
make -C "${BUILD}" -j"$(nproc)" dlls/ntdll/ntdll.so NTDLL_PLATFORM_LIBS="${NTDLL_PLATFORM_LIBS}" >"${log}" 2>&1
rc=$?
set -e
echo "make rc=${rc} (log: ${log})"
(( rc == 0 )) || { tail -30 "${log}"; exit "${rc}"; }

so="${BUILD}/dlls/ntdll/ntdll.so"
report="${LOGDIR}/ntdll-elf-report.txt"
{
    echo "## file"; file "${so}"; sha256sum "${so}"
    echo "## program headers"; readelf -lW "${so}"
    echo "## dynamic section"; readelf -dW "${so}"
    echo "## unwind bounds (must bracket .eh_frame_hdr/.eh_frame)"
    readelf -SW "${so}" | grep -E '\.eh_frame'
    readelf -sW "${so}" | grep -E ' __eh_frame(_hdr)?_(start|end)$'
    echo "## undefined dynamic symbols (resolved from NEEDED sprx at load)"
    readelf --dyn-syms -W "${so}" | awk '$7=="UND" && $8!="" {print $8}' | sort
    echo "## exported dynamic symbols"
    readelf --dyn-syms -W "${so}" | awk '$7!="UND" && $5=="GLOBAL" {print $8}' | sort
    echo "## libc.a members linked statically (defined locally, also in SDK libc.a)"
    comm -12 <("${SDK}/bin/llvm-nm" --defined-only "${SDK}/target/lib/libc.a" 2>/dev/null | awk 'NF==3{print $3}' | sort -u) \
             <(readelf -sW "${so}" | awk '$7!="UND" && $4=="FUNC" {print $8}' | sort -u)
} >"${report}"
echo "report: ${report}"

# The five symbols this workstream addresses must no longer be imported.
missing=0
for s in _Unwind_Find_FDE amd64_get_gsbase amd64_set_gsbase amd64_get_fsbase getfsent; do
    if readelf --dyn-syms -W "${so}" | awk '$7=="UND"{print $8}' | grep -qx "${s}"; then
        echo "still imported: ${s}"; missing=1
    fi
done
readelf --dyn-syms -W "${so}" | awk '$7=="UND"{print $8}' | grep -qx sysarch \
    && echo "imports sysarch (libkernel) for GS/FS base"
(( missing == 0 )) && echo "OK: no import of the five formerly missing symbols"
exit "${missing}"
