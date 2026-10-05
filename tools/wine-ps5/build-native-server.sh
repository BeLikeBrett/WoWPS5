#!/usr/bin/env bash
# Build Wine's server as an in-process object (wineserver-inproc.o) and link it
# into the native ntdll.so, for the PS5 or for a Linux test tree.
#
#   tools/wine-ps5/build-native-server.sh [--apply-patch] [--host <build-dir>] [log-dir]
#
# Default: the PS5 cross-build tree work/wine-ps5-build-check (prospero-clang).
# --host <build-dir>: a native Linux tree configured with CFLAGS containing
#   -DWINE_INPROC_SERVER (see work/claude-partner/wineserver/HANDOFF.md); the
#   server objects are rebuilt with -fPIC there so they can go into ntdll.so.
# --apply-patch applies runtime/wine-ps5/*.patch to vendor/wine first, in order
#   (each is skipped if already applied).
#
# What it does, failing loudly at the first problem:
#   1. regenerates the tree's Makefile (server/inproc.c is a new source file),
#   2. compiles every server object,
#   3. links them into one relocatable object whose only global symbols are the
#      entry points wine_server_inproc_main and wine_server_thread_main, so that
#      nothing in it can clash with ntdll's Unix objects in the same image,
#   4. PS5 only: checks every symbol it imports against the SDK. An import must
#      be exported by a system module stub; one that only the SDK's libc.a
#      syscalls.o supplies is a bare syscall instruction the title's libkernel
#      does not export (and it returns the errno value instead of -1), so it
#      fails the build. Then links a probe executable; an unresolved symbol
#      fails that link.
#   5. links ntdll.so with the server object and -z defs.
#
# With runtime/wine-ps5/wine-ps5.patch applied the PS5 ntdll.so needs the server object
# (server_start_inproc calls wine_server_inproc_main), and NTDLL_PLATFORM_LIBS
# is expanded by makedep into the generated Makefile: always build ntdll.so
# through this script or build-native-ntdll.sh, which pass the same value.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WINE_SRC="${ROOT}/vendor/wine"
RUNTIME="${ROOT}/runtime/wine-ps5"
SDK="${ROOT}/app/.deps/native/ps5-payload-sdk"
EXPECTED_REV=455e3509b98a6919fd4ad1def4803e08c41c03b2
ENTRY_POINTS=(wine_server_call_direct wine_server_inproc_main wine_server_thread_main)

apply=0
host=0
BUILD="${ROOT}/work/wine-ps5-build-check"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply-patch) apply=1; shift ;;
        --host) host=1; BUILD="$(cd "$2" && pwd)"; shift 2 ;;
        *) break ;;
    esac
done
LOGDIR="${1:-${BUILD}}"
mkdir -p "${LOGDIR}"
LOGDIR="$(cd "${LOGDIR}" && pwd)"
JOBS="${JOBS:-$(( $(nproc) > 8 ? 8 : $(nproc) ))}"

fail() { echo "FAIL: $*" >&2; exit 1; }

rev="$(git -C "${WINE_SRC}" rev-parse HEAD)"
[[ "${rev}" == "${EXPECTED_REV}" ]] || fail "vendor/wine is at ${rev}, expected ${EXPECTED_REV}"

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
[[ -f "${WINE_SRC}/server/inproc.c" ]] || fail "runtime/wine-ps5/wine-ps5.patch is not applied (use --apply-patch)"

if (( host )); then
    tag=host
    LD=ld; OBJCOPY=objcopy; NM=nm
    PLATFORM_LIBS=""
    # objects that go into a shared object must be position independent
    host_cflags="$(sed -n 's/^CFLAGS = //p' "${BUILD}/Makefile")"
    [[ "${host_cflags}" == *-DWINE_INPROC_SERVER* ]] || fail "${BUILD} is not configured with -DWINE_INPROC_SERVER in CFLAGS"
    SERVER_MAKE_ARGS=("CFLAGS=${host_cflags} -fPIC")
else
    tag=ps5
    LD="${SDK}/bin/ld.lld"; OBJCOPY="${SDK}/bin/llvm-objcopy"; NM="${SDK}/bin/llvm-nm"
    # same unwinder arrangement as build-native-ntdll.sh
    PLATFORM_LIBS="-Wl,-T,${RUNTIME}/ntdll-eh-frame.ld -Wl,--exclude-libs,libunwind.a -lunwind"
    SERVER_MAKE_ARGS=()
fi

SERVER_OBJ="${BUILD}/server/wineserver-inproc.o"
# EXTRA_NTDLL_PLATFORM_LIBS: more objects for ntdll.so, e.g. the memory layer
# (runtime/wine-ps5/ps5_mman.c) that the patched virtual.c calls.
NTDLL_PLATFORM_LIBS="${PLATFORM_LIBS:+${PLATFORM_LIBS} }${SERVER_OBJ}${EXTRA_NTDLL_PLATFORM_LIBS:+ ${EXTRA_NTDLL_PLATFORM_LIBS}}"

# the object list is the server's own source list
mapfile -t objs < <(sed -n '/^SOURCES/,/^$/p' "${WINE_SRC}/server/Makefile.in" \
                    | grep -o '[a-z0-9_]*\.c\b' | sed 's/\.c$/.o/; s#^#server/#')
(( ${#objs[@]} > 40 )) || fail "could not read the server source list"

run_make() {  # <log> <make args...>
    local log="$1"; shift
    set +e
    make -C "${BUILD}" -j"${JOBS}" "$@" NTDLL_PLATFORM_LIBS="${NTDLL_PLATFORM_LIBS}" >"${log}" 2>&1
    local rc=$?
    set -e
    echo "make $(printf '%s ' "$@" | cut -c1-60)... rc=${rc} (log: ${log})"
    (( rc == 0 )) || { grep -nE 'error|undefined|multiple definition' "${log}" | head -40; tail -5 "${log}"; exit "${rc}"; }
}

# 1. Makefile (new source file, new header, NTDLL_PLATFORM_LIBS)
run_make "${LOGDIR}/native-server-depend-${tag}.log" depend
grep -q 'server/inproc.o' "${BUILD}/Makefile" || fail "regenerated Makefile has no rule for server/inproc.o"

# 2. objects. On a host tree they may have been built without -fPIC by a plain
#    make: rebuild them all so that the flags are known.
(( host )) && rm -f "${objs[@]/#/${BUILD}/}"
run_make "${LOGDIR}/native-server-build-${tag}.log" "${SERVER_MAKE_ARGS[@]}" "${objs[@]}"
if grep -nE 'warning:' "${LOGDIR}/native-server-build-${tag}.log" | head -20 | grep .; then
    echo "note: compiler warnings above"
fi

# 3. one relocatable object, entry points only
tmp="${BUILD}/server/wineserver-inproc.tmp.o"
"${LD}" -r -o "${tmp}" "${objs[@]/#/${BUILD}/}"
keep=(); for e in "${ENTRY_POINTS[@]}"; do keep+=("--keep-global-symbol=${e}"); done
"${OBJCOPY}" "${keep[@]}" "${tmp}" "${SERVER_OBJ}"
rm -f "${tmp}"
globals="$("${NM}" -g --defined-only "${SERVER_OBJ}" | awk '{print $NF}' | sort | tr '\n' ' ')"
[[ "${globals}" == "wine_server_call_direct wine_server_inproc_main wine_server_thread_main " ]] \
    || fail "unexpected global symbols in wineserver-inproc.o: ${globals}"
echo "wineserver-inproc.o: globals = ${globals}"

"${NM}" -u "${SERVER_OBJ}" | awk '{print $NF}' | LC_ALL=C sort -u > "${LOGDIR}/native-server-imports-${tag}.txt"
echo "wineserver-inproc.o: $(wc -l < "${LOGDIR}/native-server-imports-${tag}.txt") imported symbols (${LOGDIR}/native-server-imports-${tag}.txt)"

# An import that an ntdll Unix object defines would silently bind to ntdll's
# function instead of libc's once both are in one image.
ntdll_defs="${LOGDIR}/native-server-ntdll-globals-${tag}.txt"
"${NM}" -g --defined-only "${BUILD}"/dlls/ntdll/unix/*.o 2>/dev/null | awk 'NF>=3{print $NF}' | LC_ALL=C sort -u > "${ntdll_defs}"
if [[ -s "${ntdll_defs}" ]]; then
    # (the memory layer's calls, ps5w_*, are meant to bind there: the server declares its sections to it)
    clash="$(LC_ALL=C comm -12 "${LOGDIR}/native-server-imports-${tag}.txt" "${ntdll_defs}" | grep -v '^ps5w_' | tr '\n' ' ' || true)"
    [[ -z "${clash}" ]] || fail "server imports that ntdll's Unix objects define: ${clash}"
    echo "no server import is defined by ntdll's Unix objects"
else
    echo "note: ntdll Unix objects not built yet, name clash check done after the ntdll.so link"
fi

# 4. PS5: where does each import come from?
if (( ! host )); then
    stubs="${LOGDIR}/native-server-sdk-exports.txt"
    : > "${stubs}"
    for lib in libkernel_web.so libSceLibcInternal.so libSceNet.so; do
        "${NM}" -D --defined-only "${SDK}/target/lib/${lib}" 2>/dev/null | awk -v l="${lib}" '{print $NF, l}' >> "${stubs}"
    done
    libc_members="${LOGDIR}/native-server-libc-members.txt"
    "${NM}" -A --defined-only "${SDK}/target/lib/libc.a" 2>/dev/null \
        | awk '{n=split($1,a,":"); print $NF, a[n-1]}' | LC_ALL=C sort -u > "${libc_members}"
    report="${LOGDIR}/native-server-import-report-${tag}.txt"
    bad=0
    {
        echo "# symbol <- provider.   raw-syscall = SDK libc.a syscalls.o (bare syscall instruction, not a libkernel export)"
        while read -r sym; do
            member="$(awk -v s="${sym}" '$1==s{print $2; exit}' "${libc_members}")"
            stub="$(awk -v s="${sym}" '$1==s{print $2; exit}' "${stubs}")"
            # lld extracts an archive member as soon as a reference meets its lazy symbol,
            # and libc.a precedes the stubs on the link line: libc.a wins when both define it
            # The title does not link libc.a at all (PS5_Vulkan's recipe: the system
            # stubs and the platform layer), so a name a system module exports is that
            # module's there, whatever libc.a also has.
            if [[ ${sym} == ps5w_* ]]; then echo "${sym} <- the memory layer, linked with ntdll's Unix objects"
            elif [[ -n "${stub}" ]]; then echo "${sym} <- ${stub}${member:+ (libc.a(${member}) in an SDK-libc link)}"
            elif [[ "${member}" == "syscalls.o" ]]; then echo "${sym} <- raw-syscall libc.a(syscalls.o)"
            elif [[ -n "${member}" ]]; then echo "${sym} <- libc.a(${member})${stub:+ (also ${stub})}"
            elif [[ -n "${stub}" ]]; then echo "${sym} <- ${stub}"
            else echo "${sym} <- UNRESOLVED"
            fi
        done < "${LOGDIR}/native-server-imports-${tag}.txt"
    } > "${report}"
    if grep -v '^#' "${report}" | grep -E 'raw-syscall|UNRESOLVED'; then bad=1; fi
    echo "import report: ${report}"
    (( bad == 0 )) || fail "the server imports symbols the PS5 title cannot get from a system module (listed above)"
    echo "OK: every server import is a system module export or a non-syscall libc.a member"

    # probe executable: the linker itself must resolve everything
    probe_c="${BUILD}/server/inproc-link-probe.c"
    cat > "${probe_c}" <<'EOF'
/* link probe only: never run */
extern int wine_server_thread_main( int client_fd );
/* the memory layer's calls, which the title gets from ntdll's Unix objects */
int ps5w_section( int fd ) { return fd; }
void ps5w_section_closed( int fd ) { (void)fd; }
int ps5w_wake_flags[16384];
int main( void ) { return wine_server_thread_main( -1 ); }
EOF
    set +e
    "${SDK}/bin/prospero-clang" -m64 -o "${BUILD}/server/inproc-link-probe" "${probe_c}" "${SERVER_OBJ}" \
        -Wl,-Map="${LOGDIR}/native-server-link-probe.map" -Wl,--why-extract="${LOGDIR}/native-server-why-extract.txt" \
        > "${LOGDIR}/native-server-link-probe.log" 2>&1
    rc=$?
    set -e
    echo "probe executable link rc=${rc} (log: ${LOGDIR}/native-server-link-probe.log)"
    (( rc == 0 )) || { cat "${LOGDIR}/native-server-link-probe.log" | head -40; fail "unresolved symbols linking the server"; }
    if grep -F 'libc.a(syscalls.o)' "${LOGDIR}/native-server-why-extract.txt" | grep -F 'wineserver-inproc.o'; then
        echo "note: in this SDK-libc probe link the names above come from libc.a(syscalls.o); the title takes them from the system stubs"
    fi
    # a libc.a member the server uses may itself be built on them: not fatal, but say so
    indirect="$(awk -F'\t' '$2 ~ /libc\.a\(syscalls\.o\)/ {n=split($1,a,"/"); print a[n] " needs " $3}' \
                "${LOGDIR}/native-server-why-extract.txt" | tr '\n' ';')"
    [[ -z "${indirect}" ]] || echo "WARNING: raw syscall wrappers reach the link through libc.a itself: ${indirect}"
fi

# 5. ntdll.so with the server inside
run_make "${LOGDIR}/native-server-ntdll-${tag}.log" dlls/ntdll/ntdll.so
so="${BUILD}/dlls/ntdll/ntdll.so"
# (to a file first: grep -q closing the pipe early would fail the pipeline under pipefail)
"${NM}" --defined-only "${so}" | awk '{print $NF}' > "${LOGDIR}/native-server-ntdll-symbols-${tag}.txt"
grep -qx wine_server_inproc_main "${LOGDIR}/native-server-ntdll-symbols-${tag}.txt" \
    || fail "ntdll.so does not contain wine_server_inproc_main"
echo "OK: ${so} links with the in-process server (-z defs)"

if (( ! host )); then
    report="${LOGDIR}/native-server-elf-report.txt"
    {
        echo "## files"; file "${SERVER_OBJ}" "${so}"; sha256sum "${SERVER_OBJ}" "${so}"
        echo "## ntdll.so dynamic section"; readelf -dW "${so}"
        echo "## ntdll.so undefined dynamic symbols (resolved from NEEDED sprx at load)"
        readelf --dyn-syms -W "${so}" | awk '$7=="UND" && $8!="" {print $8}' | sort
        echo "## SDK libc.a syscalls.o (raw syscall) symbols that ntdll's own Unix objects import"
        echo "## (client-side hazards, not the server's: see HANDOFF.md)"
        "${NM}" -u "${BUILD}"/dlls/ntdll/unix/*.o | awk 'NF==2{print $2}' | LC_ALL=C sort -u \
            | while read -r sym; do
                  awk -v s="${sym}" '$1==s && $2=="syscalls.o"{print s}' "${LOGDIR}/native-server-libc-members.txt"
              done
    } > "${report}"
    echo "report: ${report}"
fi
echo "done (${tag})"
