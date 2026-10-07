#!/usr/bin/env bash
# Wine's PS5 audio driver on the PC: the driver itself, under Wine's own
# mmdevapi, winmm and a Windows test program, with a stand-in for the console's
# audio port that records what it is given.
#
#   tools/wine-ps5/test-audio-host.sh [output-dir]
#
# Default output: work/audio-partner/host. Nothing is written anywhere else:
# the driver is built there as wineps5.so against the single-process Linux
# build (work/wine-desktop-host, read only) and found through WINEDLLPATH, and
# the prefix is a copy of work/console-prefix made on the first run.
#
# What it shows: that mmdevapi loads the driver by name, that every WASAPI and
# winmm call tools/win32-audio-test.c makes succeeds, and that what reaches the
# port is the three tones (frequency, level, length, left and right, no gaps).
# What it cannot show: the console's port (sceAudioOut), the title's table of
# Unix libraries that replaces dlopen there, and what anything sounds like.
#
# Four runs:
#   block    the port takes one grain per grain of time, as the console's does
#   noblock  the port returns at once: the driver must still not outrun the clock
#   refuse   the port fails every grain: the driver must keep time without it
#   off      WOWPS5_AUDIO=0: Windows must be told there is no audio device
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:-$root/work/audio-partner/host}
source="$root/work/wine-desktop-src"
build="$root/work/wine-desktop-host"
wine="$build/loader/wine"
[[ -x $wine && -f $build/dlls/ntdll/ntdll.so ]] || { echo "build first: tools/wine-ps5/build-desktop-host.sh" >&2; exit 1; }
mkdir -p "$out/x86_64-unix" "$out/logs"

# the driver as Wine builds a Unix library (the flags of the build tree's own rules), with the stand-in port
gcc -m64 -c -o "$out/wineps5_audio.o" "$root/runtime/wine-ps5/wineps5_audio.c" \
    -I"$source/dlls/mmdevapi" -I"$build/include" -I"$source/include" -D__WINESRC__ -DWINE_UNIX_LIB \
    -Wall -Wextra -Wno-unused-parameter -pipe -fcf-protection=none -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing \
    -Wdeclaration-after-statement -Wstrict-prototypes -Wvla -Wwrite-strings -fPIC -fasynchronous-unwind-tables \
    -g -O2 -DWINE_INPROC_SERVER -DWINE_PS5_MEMORY_MODEL || exit 1
gcc -m64 -c -O2 -g -fPIC -Wall -Wextra -o "$out/audio-host-sink.o" "$root/tools/wine-ps5/audio-host-sink.c" || exit 1
gcc -m64 -o "$out/x86_64-unix/wineps5.so" -shared -Wl,-Bsymbolic -Wl,-soname,wineps5.so -Wl,-z,defs \
    "$out/wineps5_audio.o" "$out/audio-host-sink.o" "$build/dlls/ntdll/ntdll.so" -lpthread || exit 1
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$root/tools/win32-audio-test.c" -lole32 -lwinmm -lntdll -o "$out/win32-audio-test.exe" || exit 1

prefix="$out/prefix"
if [[ ! -d $prefix ]]; then
    cp -a "$root/work/console-prefix" "$prefix" || exit 1
    fresh=1
else
    fresh=0
fi
export WINEPREFIX="$prefix" WINEDLLPATH="$out" DISPLAY= WAYLAND_DISPLAY=
USER=$(ls "$prefix/drive_c/users" | grep -v '^Public$' | head -1)
export USER
if (( fresh )); then
    # on the console the server supplies this default (server/registry.c); a PC prefix has to name the driver
    timeout 120 "$wine" 'C:\windows\system32\reg.exe' add 'HKCU\Software\Wine\Drivers' /v Audio /d ps5 /f > "$out/logs/reg.log" 2>&1 \
        || { echo "could not name the audio driver, see $out/logs/reg.log" >&2; exit 1; }
fi

failed=0
run() {  # <mode> <expected exit status: 0, or "nonzero">
    local mode=$1 expected=$2 status verdict
    rm -f "$out/capture-$mode.raw"
    if [[ $mode == off ]]; then
        WOWPS5_AUDIO=0 WOWPS5_AUDIO_CAPTURE="$out/capture-$mode.raw" \
            timeout 120 "$wine" "$out/win32-audio-test.exe" > "$out/logs/$mode.out" 2> "$out/logs/$mode.err"
    else
        WOWPS5_AUDIO_HOST_PORT="$mode" WOWPS5_AUDIO_CAPTURE="$out/capture-$mode.raw" \
            timeout 120 "$wine" "$out/win32-audio-test.exe" > "$out/logs/$mode.out" 2> "$out/logs/$mode.err"
    fi
    status=$?
    if [[ $expected == nonzero ]]; then verdict=$(( status != 0 && status != 124 && status != 98 )); else verdict=$(( status == expected )); fi
    printf '%-8s exit=%-3s %s  %s\n' "$mode" "$status" "$( ((verdict)) && echo ok || echo UNEXPECTED )" "$(tail -1 "$out/logs/$mode.out" | tr -d '\r')"
    (( verdict )) || failed=1
    grep -h 'wineps5:\|host audio\|err:mmdevapi' "$out/logs/$mode.err" | sed 's/^/         /'
}
run block 0
run noblock 0
run refuse 0
run off nonzero
grep -q 'GetDefaultAudioEndpoint(eRender' "$out/logs/off.out" && grep -q '^FAIL IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender' "$out/logs/off.out" \
    || { echo "off: the render endpoint was not refused"; failed=1; }

# what reached the port in the first two runs
python3 - "$out/capture-block.raw" "$out/capture-noblock.raw" <<'EOF' || failed=1
import sys
import numpy as np

RATE = 48000
def analyse(path):
    data = np.fromfile(path, dtype='<i2')
    frames = data.reshape(-1, 2).astype(np.float64)
    # The port is given nothing while no stream plays, so the tones follow each other
    # with only the silence a stream plays before it starts and after it ends: a tone is
    # a stretch of sound with no run of 64 silent frames in it (a 440 Hz tone has runs of 1).
    sound = np.flatnonzero((frames != 0).any(axis=1))
    tones = []
    if len(sound):
        breaks = np.flatnonzero(np.diff(sound) > 64)
        starts = np.concatenate(([sound[0]], sound[breaks + 1]))
        ends = np.concatenate((sound[breaks], [sound[-1]]))
        tones = list(zip(starts, ends + 1))
    print(f"{path}: {len(frames)} frames ({len(frames) / RATE:.2f} s), {len(tones)} tones")
    expected = [(440.0, 1.0, 0.25, 0.125), (660.0, 0.5, 0.25, 0.25), (880.0, 0.3, 0.25, 0.25)]
    good = len(tones) == len(expected)
    for (start, end), (hertz, seconds, left, right) in zip(tones, expected):
        part = frames[start:end]
        length = len(part) / RATE
        spectrum = np.abs(np.fft.rfft(part[:, 0] * np.hanning(len(part))))
        peak = np.argmax(spectrum) * RATE / len(part)
        levels = np.abs(part).max(axis=0) / 32767.0
        # a gap is a run of exact zeros in both channels longer than a tone ever has at a zero crossing
        silent = (part == 0).all(axis=1)
        runs = np.diff(np.flatnonzero(np.diff(np.concatenate(([0], silent.astype(np.int8), [0])))))[::2]
        gap = int(runs.max()) if len(runs) else 0
        # a pure tone: nearly all of the power within 2% of the peak
        power = spectrum ** 2
        bins = np.arange(len(power)) * RATE / len(part)
        purity = power[np.abs(bins - peak) < peak * 0.02 + 5].sum() / power.sum()
        ok = (abs(peak - hertz) < 3 and abs(length - seconds) < 0.02 and abs(levels[0] - left) < 0.01
              and abs(levels[1] - right) < 0.01 and gap < 8 and purity > 0.98)
        good &= bool(ok)
        print(f"  {'ok  ' if ok else 'FAIL'} {peak:7.1f} Hz for {length:.3f} s, left {levels[0]:.3f} right {levels[1]:.3f}, "
              f"longest gap {gap} frames, {purity * 100:.2f}% of the power at the tone "
              f"(expected {hertz:.0f} Hz, {seconds} s, {left} and {right})")
    return good

results = [analyse(path) for path in sys.argv[1:]]
sys.exit(0 if all(results) else 1)
EOF
echo "$( ((failed)) && echo FAIL || echo PASS ): PS5 audio driver on the PC; logs in $out/logs"
exit "$failed"
