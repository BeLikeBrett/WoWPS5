#!/usr/bin/env python3
"""Run a Windows program through Wine in the title, and keep what it said.

  tools/wine-ps5/run-console.py /data/wowps5/test/program.exe [NAME=value | +argument ...] [--until REGEX] [--timeout S]

Writes the run request the title reads at start (app/runtime/wine_title.c),
launches the title, and captures klog until a line matches --until, the title
exits or crashes, or the timeout passes.
"""
import argparse
import datetime
import io
import json
import re
import sys
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOOLS = ROOT / 'vendor/PS5_Vulkan/tools'
sys.path.insert(0, str(TOOLS))
import ps5_console  # noqa: E402
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location('run_title', TOOLS / 'run-title.py')
run_title = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_title)
# Klog is shared with system services. A SceJSCd timeout must not close a
# healthy WoW title. This runner polls the application's process lifecycle;
# retain global crash records as evidence, but do not end on another PID's log.
run_title.CRASH = re.compile(r'(?!)')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('program')
    parser.add_argument('settings', nargs='*', help='NAME=value environment for Wine, or +argument for the program')
    parser.add_argument('--until', default=r'the Windows program ended with status')
    parser.add_argument('--timeout', type=float, default=90.0)
    args = parser.parse_args()
    param = json.loads((ROOT / 'app/ps5/sce_sys/param.json').read_text())
    title = param['titleId']
    settings = ps5_console.load_settings()
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    # The title ends itself a little before this script would ask the shell to:
    # a hung title that only the shell can close has frozen the shell before.
    limit = [] if any(setting.startswith('WOWPS5_DEADLINE=') for setting in args.settings) \
        else [f'WOWPS5_DEADLINE={max(10, int(args.timeout) - 25)}']
    request = '\n'.join([args.program, *args.settings, *limit]) + '\n'
    ftp.storbinary(f'STOR /data/homebrew/{title}/wine-run.txt', io.BytesIO(request.encode()))
    ftp.quit()
    run = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    output = ROOT / f'app/klog/wine-{run}/klog.log'
    output.parent.mkdir(parents=True, exist_ok=True)
    ended = 'not started'
    try:
        ended, _, _ = run_title.run_title(title, args.until, args.timeout, str(output), [], r'.', str(ROOT / 'app/build/ps5/link/llvm-pie.elf'),
                                          exit_grace=5.0)
    finally:
        # The title removes the request when it reads it. One left behind (the
        # launch failed) would be read by the title's next start, whatever that
        # start was for: on 2026-10-04 a probe run started the game client that way.
        try:
            ftp = FTP()
            ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
            ftp.login(settings['ftp_user'], settings['ftp_password'])
            ftp.delete(f'/data/homebrew/{title}/wine-run.txt')
            print('the run request was still there (the title never read it): removed')
        except Exception:
            pass
    print(f'run: {ended}; klog: {output}')
    return 0 if ended == 'finished' else 3


if __name__ == '__main__':
    sys.exit(main())
