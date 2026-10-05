#!/usr/bin/env bash
# Run the desktop test programs in the single-process Linux build.
#
#   tools/wine-ps5/run-desktop-tests.sh [--driver x11|wayland|null|skel] [--xvfb] [--seconds N]
#
# --driver   the graphics driver the in-process desktop loads (default x11):
#              x11      winex11.drv on $DISPLAY (or on a private Xvfb with --xvfb)
#              wayland  winewayland.drv on $WAYLAND_DISPLAY ($DISPLAY is cleared so that x11 fails first)
#              null     win32u's null driver: windows without any output, Vulkan on a headless surface
#              skel     dlls/wineskel.drv, the skeleton fullscreen driver, if it is built
# --xvfb     start an Xvfb for the run and stop it afterwards (x11 only); without DRI3 only a
#            software Vulkan device can present there
# --seconds  how long the window and Vulkan tests run (default 3)
#
# Each driver has its own prefix, work/claude-partner/desktop/prefix-<driver>, copied from
# work/console-prefix the first time (the single-process build cannot run wineboot). The
# prefix's user name is used for the run: nothing would create a profile for another user.
# Programs are passed by path: a bare name would go through start.exe, a second process.
# Logs go to work/claude-partner/desktop/logs/<driver>/.
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
wine="$root/work/wine-desktop-host/loader/wine"
bin="$root/work/claude-partner/desktop/bin"
driver=x11 xvfb=0 seconds=3
while [[ $# -gt 0 ]]; do
    case "$1" in
        --driver) driver="$2"; shift 2 ;;
        --xvfb) xvfb=1; shift ;;
        --seconds) seconds="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
[[ -x $wine ]] || { echo "build first: tools/wine-ps5/build-desktop-host.sh" >&2; exit 1; }
[[ -f $bin/win32-window-test.exe ]] || "$root/tools/wine-ps5/build-desktop-tests.sh" > /dev/null || exit 1

prefix="$root/work/claude-partner/desktop/prefix-$driver"
logs="$root/work/claude-partner/desktop/logs/$driver"
mkdir -p "$logs"
fresh=0
if [[ ! -d $prefix ]]; then
    cp -a "$root/work/console-prefix" "$prefix"
    fresh=1
fi
export WINEPREFIX="$prefix"
USER=$(ls "$prefix/drive_c/users" | grep -v '^Public$' | head -1)
export USER

xvfb_pid=
cleanup() { [[ -z $xvfb_pid ]] || kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT

case "$driver" in
    x11)
        if (( xvfb )); then
            for n in 99 98 97 96; do [[ -e /tmp/.X11-unix/X$n ]] || break; done
            Xvfb ":$n" -screen 0 1280x720x24 -nolisten tcp > "$logs/xvfb.log" 2>&1 &
            xvfb_pid=$!
            sleep 1
            export DISPLAY=":$n"
        fi
        unset WAYLAND_DISPLAY ;;
    wayland) export DISPLAY= ;;
    null|skel) export DISPLAY= WAYLAND_DISPLAY= ;;
    *) echo "unknown driver $driver" >&2; exit 2 ;;
esac
if (( fresh )) && [[ $driver == null || $driver == skel ]]; then
    "$wine" 'C:\windows\system32\reg.exe' add 'HKCU\Software\Wine\Drivers' /v Graphics /d "$driver" /f > "$logs/reg.log" 2>&1 \
        || { echo "could not set the graphics driver, see $logs/reg.log" >&2; exit 1; }
fi
if [[ $driver == skel ]]; then
    built="$root/work/wine-desktop-host/dlls/wineskel.drv/x86_64-windows/wineskel.drv"
    [[ -f $built ]] || { echo "dlls/wineskel.drv is not built" >&2; exit 1; }
    # a builtin is found through its file in system32 (or with WINEBOOTSTRAPMODE=1)
    cp "$built" "$prefix/drive_c/windows/system32/"
    export WINESKEL_INPUT=1
fi

failed=0
run() {  # <log name> <program> [args...]
    local name="$1"; shift
    timeout 120 "$wine" "$@" > "$logs/$name.out" 2> "$logs/$name.err"
    local rc=$?
    printf '%-22s rc=%-3s %s\n' "$name" "$rc" "$(tail -1 "$logs/$name.out")"
    (( rc == 0 )) || failed=1
}
echo "driver $driver, display '${DISPLAY-}' '${WAYLAND_DISPLAY-}', prefix $prefix, user $USER"
run crt "$bin/win32-desktop-crt-test.exe"
run window "$bin/win32-window-test.exe" "$seconds"
run vulkan "$bin/win32-vulkan-test.exe" "$seconds"
run edge "$bin/win32-desktop-edge-test.exe"
WOWPS5_EDGE=dllmain run edge-dllmain "$bin/win32-desktop-edge-test.exe"
WOWPS5_EDGE=exitthread run edge-exitthread "$bin/win32-desktop-edge-test.exe"
run helper-probe "$bin/win32-helper-probe.exe"
exit "$failed"
