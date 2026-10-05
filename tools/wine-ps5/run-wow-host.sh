#!/usr/bin/env bash
# Start the installed WoW client on the PC under one of this project's Wine
# builds, without writing to the installation.
#
#   tools/wine-ps5/run-wow-host.sh <wine-build-dir> <seconds> [NAME=value ...]
#
# The installation is seen through an overlay (bubblewrap): everything the
# client writes (settings, cache, logs, its data container) goes to
# work/wow-host/upper, never to the real folder. vkd3d-proton and DXVK's DXGI are installed in the
# test prefix's system folder. The client is given <seconds>, then this build's
# Wine processes for this prefix are ended. Nothing signs in.
#
# HEADLESS=1 runs the client inside a headless gamescope instead of on the
# desktop: nothing appears on screen, and what the client draws is saved as
# <log>.shot-<second>.png every SHOT_EVERY seconds (default 20).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(cd -- "$1" && pwd); seconds=$2; shift 2
# The game is not part of this project: WOW_INSTALL (environment or the project's .env) names your installation.
install=${WOW_INSTALL:-$(sed -n 's/^WOW_INSTALL=//p' "$root/.env" 2>/dev/null | head -1 | sed 's/^"//; s/"$//')}
[[ -d $install/_classic_beta_ ]] || { echo "set WOW_INSTALL (environment or .env, see .env.example) to the folder that holds _classic_beta_ and Data" >&2; exit 2; }
flavor="$install/_classic_beta_"
state="$root/work/wow-host"
name=$(basename "$build")
mkdir -p "$state/upper" "$state/work" "$state/logs"
if ps -eo args | grep -q '^[A-Za-z]:.*WowB\.exe'; then echo "the game is already running: not starting a second copy" >&2; exit 3; fi
[[ -x $build/loader/wine ]] || { echo "no Wine in $build" >&2; exit 2; }
log="$state/logs/$name-$(date +%Y%m%d-%H%M%S)"
# a build with the server inside the process (WINE_INPROC_SERVER): no wineboot, no wineserver to end
single=; grep -qs -- -DWINE_INPROC_SERVER "$build/config.log" && single=1
wrap=; [[ -z ${HEADLESS:-} ]] || wrap="gamescope --backend headless -W ${WIDTH:-1920} -H ${HEIGHT:-1080} -r 60 --"
shot_every=${SHOT_EVERY:-20}
# CONSOLE=1 (single-process builds): the run is given what the console's title has, which is
# no display driver (win32u's null driver, Vulkan without a window system) and only the Unix
# libraries linked into the title. Its prefix is separate: the driver choice is a registry value.
console=${CONSOLE:-}
[[ -z $console ]] || { name="$name-console"; export DISPLAY= WAYLAND_DISPLAY= WINEPS5_NO_NTSYNC=1 WINEPS5_UNIXLIBS="${WINEPS5_UNIXLIBS:-ntdll,ws2_32,win32u,winevulkan,crypt32}"; }
export WINEPREFIX="$state/prefix-$name" USER=wowps5 LOGNAME=wowps5
# DX11=1 adds DXVK's Direct3D 11 (the client then has both; it is the fallback should the
# console's DirectX 12 feature level, 11_1, not be enough for it)
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi${DX11:+,d3d11,d3d10core}=n;winemenubuilder.exe=d"
export WINEDEBUG="${WINEDEBUG:-err+all,fixme-all}" VKD3D_DEBUG="${VKD3D_DEBUG:-warn}" DXVK_LOG_LEVEL=warn
for setting in "$@"; do export "$setting"; done
# The whole installation is an overlay. The client needs to write to its data
# container (mounted read-only it reports "container locked"), and the overlay
# copies each 1 GiB archive it opens for writing: up to the size of the
# installation (64 GiB) in work/wow-host/upper, copied once and reused by later
# runs. Delete work/wow-host when done.
# gamescope goes outside the overlay's namespace: inside it Xwayland cannot own its socket folder
[[ -z $wrap ]] || exec 2> "$log.gamescope"     # where gamescope says which display it made
$wrap bwrap --dev-bind / / --overlay-src "$install" --overlay "$state/upper" "$state/work" "$install" \
    bash -c '
        set -u
        # vkd3d-proton and DXVK DXGI go where Proton puts them: the prefix system folder.
        # The client loads d3d12.dll from there, and its own D3D12/ folder holds
        # Microsoft Agility SDK files that vkd3d-proton passes over.
        if [ ! -d "$WINEPREFIX/drive_c/windows/system32" ]; then
            if [ -n "'"$single"'" ]; then    # one process cannot run wineboot: the prefix of the build that can
                cp -a "'"$state"'/prefix-wine-memsim-host" "$WINEPREFIX" || exit 2
                [ -z "'"$console"'" ] || "'"$build"'/loader/wine" "C:\\windows\\system32\\reg.exe" add "HKCU\\Software\\Wine\\Drivers" /v Graphics /d null /f > "'"$log"'.reg" 2>&1
            else
                "'"$build"'/loader/wine" wineboot -u > "'"$log"'.wineboot" 2>&1
                "'"$build"'/server/wineserver" -w
            fi
        fi
        cp -f "'"$root"'/work/d3d12-test/d3d12.dll" "'"$root"'/work/d3d12-test/d3d12core.dll" "'"$root"'/work/d3d12-test/dxgi.dll" "$WINEPREFIX/drive_c/windows/system32/"
        [ -z "${DX11:-}" ] || cp -f "'"$root"'/work/d3d12-test/d3d11.dll" "'"$root"'/work/d3d12-test/d3d10core.dll" "$WINEPREFIX/drive_c/windows/system32/"
        cd "'"$flavor"'"
        "'"$build"'/loader/wine" "'"$flavor"'/WowB.exe" > "'"$log"'.out" 2> "'"$log"'.err" &
        started=$(date +%s) shot=0
        while (( (now = $(date +%s) - started) < '"$seconds"' )) && kill -0 $! 2>/dev/null; do
            sleep 2
            if [ -n "'"$wrap"'" ] && (( now >= shot + '"$shot_every"' )); then
                shot=$now
                socket=$(sed -n "s/.*Running compositor on wayland display .\(gamescope-[0-9]*\).*/\1/p" "'"$log"'.gamescope" | head -1)
                [ -z "$socket" ] || GAMESCOPE_WAYLAND_DISPLAY=$socket timeout 10 gamescopectl screenshot "'"$log"'.shot-$now.png" > /dev/null 2>&1
            fi
        done
        # what the run cost: processor seconds and voluntary context switches of the one process
        if [ -n "'"$single"'" ] && kill -0 $! 2>/dev/null; then
            awk -v hz=$(getconf CLK_TCK) "{printf \"cpu seconds: user %.1f system %.1f\\n\", \$14/hz, \$15/hz}" /proc/$!/stat > "'"$log"'.cpu"
            cat /proc/$!/task/*/status 2>/dev/null | awk "/^voluntary_ctxt/{v+=\$2} /^nonvoluntary/{n+=\$2} END{print \"context switches: voluntary \" v \", involuntary \" n}" >> "'"$log"'.cpu"
            ls /proc/$!/task | wc -l | sed "s/^/threads: /" >> "'"$log"'.cpu"
        fi
        if kill -0 $! 2>/dev/null; then echo "still running after '"$seconds"' s: ending it"; else wait $!; echo "the client exited by itself with status $?"; fi
        if [ -n "'"$single"'" ]; then      # the one process is the one this script started
            pkill -P $! 2>/dev/null; kill $! 2>/dev/null; sleep 3; pkill -9 -P $! 2>/dev/null; kill -9 $! 2>/dev/null
        else
            "'"$build"'/server/wineserver" -k 2>/dev/null
        fi
        sleep 1
        mkdir -p "'"$log"'.client"; cp -r "'"$flavor"'/Logs/." "'"$log"'.client/" 2>/dev/null
    '
echo "wine output: $log.out $log.err; the client's own logs: $log.client/"
