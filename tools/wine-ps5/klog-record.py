#!/usr/bin/env python3
"""Keep the console's kernel log on the PC while the game is played from the home screen.

  tools/wine-ps5/klog-record.py [directory]

A session started from the console's home screen has no runner on the PC, so a
crash would leave nothing to read. This follows the console's klog service and
writes it to work/klog-live/klog-<start>.log (a new file every 64 MiB),
reconnecting when the console restarts. It only listens.
"""
import datetime
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'vendor/PS5_Vulkan/tools'))
import ps5_console  # noqa: E402


def main():
    directory = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'work/klog-live'
    directory.mkdir(parents=True, exist_ok=True)
    settings = ps5_console.load_settings()
    while True:
        try:
            with socket.create_connection((settings['host'], settings['klog_port']), timeout=15) as klog:
                klog.settimeout(60)
                output = None
                quiet = 0
                times = None
                written = 0
                while True:
                    try:
                        data = klog.recv(65536)
                        quiet = 0
                    except socket.timeout:
                        # A console that was restarted leaves the connection open and silent
                        # for good: after three quiet minutes, connect again.
                        quiet += 1
                        if quiet >= 3:
                            break
                        continue
                    if not data:
                        break
                    if output is None or written > 64 << 20:
                        if output:
                            output.close()
                        name = directory / f'klog-{datetime.datetime.now():%Y%m%d-%H%M%S}.log'
                        output = open(name, 'wb')
                        if times:
                            times.close()
                        # when each piece arrived: "<seconds since the epoch> <its offset in the log>"
                        times = open(str(name) + '.times', 'w')
                        written = 0
                    times.write(f'{time.time():.3f} {written}\n')
                    times.flush()
                    output.write(data)
                    output.flush()
                    written += len(data)
        except OSError:
            pass
        time.sleep(5)


if __name__ == '__main__':
    main()
