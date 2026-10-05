#!/usr/bin/env python3
"""Copy the installed WoW client to the console.

The game is not part of this project: WOW_INSTALL names your own installation.

  tools/wine-ps5/stage-game.py [--dry-run] [--only PATTERN] [--shard I/N]

Uploads over the console's FTP server to drive C: of the console's prefix,
  /data/wowps5/prefix/dosdevices/c:/Games/World of Warcraft
which the client sees as C:\\Games\\World of Warcraft. The installation on the
PC is only read. A file whose size on the console already matches is skipped,
so an interrupted run is continued by running it again; a file cut short is
sent again from its start.

Left out: the other flavor, the launcher, and what belongs to the user or is
made again at run time (WTF, Interface, Logs, Errors, Cache, shader caches).
Nothing of the account is copied.

A title cannot ask the console how much space is free, and neither can this
script: a failed upload (a full disk, most likely) removes the file that
failed, stops, and says how much had been sent, so that it can be removed.
"""
import argparse
import fnmatch
import sys
import time
from ftplib import FTP, all_errors, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'vendor/PS5_Vulkan/tools'))
import ps5_console  # noqa: E402



def install_folder():
    """The game's folder on this PC: WOW_INSTALL, from the environment or the project's .env."""
    import os
    value = os.environ.get('WOW_INSTALL')
    env_file = ROOT / '.env'
    if not value and env_file.is_file():
        for line in env_file.read_text(encoding='utf-8').splitlines():
            if line.strip().startswith('WOW_INSTALL='):
                value = line.split('=', 1)[1].strip().strip('"\'')
    if not value or not (Path(value) / '_classic_beta_').is_dir():
        sys.exit('set WOW_INSTALL (environment or .env, see .env.example) to the folder that holds _classic_beta_ and Data')
    return Path(value)


INSTALL = install_folder()
TARGET = '/data/wowps5/prefix/dosdevices/c:/Games/World of Warcraft'
SKIP_TOP = {'_classic_', '.battle.net', 'World of Warcraft Launcher.exe'}
SKIP_FLAVOR = {'WTF', 'Interface', 'Logs', 'Errors', 'Cache', 'Screenshots',
               'vkd3d-proton.cache', 'vkd3d-proton.cache.write'}


def wanted():
    for path in sorted(INSTALL.rglob('*')):
        relative = path.relative_to(INSTALL)
        if relative.parts[0] in SKIP_TOP:
            continue
        if relative.parts[0] == '_classic_beta_' and len(relative.parts) > 1 and relative.parts[1] in SKIP_FLAVOR:
            continue
        if path.is_file() and not path.is_symlink():
            yield relative, path


def connect(settings):
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=600)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    ftp.voidcmd('TYPE I')
    return ftp


def ensure(ftp, directory, made):
    path = ''
    for part in directory.strip('/').split('/'):
        path += '/' + part
        if path in made:
            continue
        try:
            ftp.mkd(path)
        except error_perm:
            pass
        made.add(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dry-run', action='store_true', help='list what would be sent')
    parser.add_argument('--only', help='only files whose path matches this pattern, e.g. "Data/data/data.00*"')
    parser.add_argument('--shard', default='0/1', help='I/N: every Nth file starting at the Ith, to run N uploads side by side')
    args = parser.parse_args()
    shard, shards = (int(part) for part in args.shard.split('/'))
    files = [(relative, path) for relative, path in wanted()
             if not args.only or fnmatch.fnmatch(relative.as_posix(), args.only)]
    files = files[shard::shards]
    total = sum(path.stat().st_size for _, path in files)
    print(f'{len(files)} files, {total / 2**30:.1f} GiB, to {TARGET}', flush=True)
    if args.dry_run:
        for relative, path in files:
            print(f'{path.stat().st_size:>12}  {relative.as_posix()}')
        return 0
    settings = ps5_console.load_settings()
    ftp = connect(settings)
    made = set()
    sent = kept = sent_bytes = 0
    started = time.time()
    for relative, path in files:
        target = f'{TARGET}/{relative.as_posix()}'
        size = path.stat().st_size
        try:
            if ftp.size(target) == size:
                kept += 1
                continue
        except all_errors:
            pass
        ensure(ftp, target.rsplit('/', 1)[0], made)
        began = time.time()
        try:
            with path.open('rb') as stream:
                ftp.storbinary('STOR ' + target, stream, blocksize=4 * 1024 * 1024)
            if ftp.size(target) != size:
                raise OSError(f'the console has {ftp.size(target)} of {size} bytes')
        except all_errors + (OSError,) as error:
            print(f'FAILED at {relative.as_posix()}: {error}', flush=True)
            try:
                ftp = connect(settings)
                ftp.delete(target)
                print('the file that failed was removed from the console', flush=True)
            except all_errors as second:
                print(f'and it could not be removed: {second}', flush=True)
            print(f'sent before that: {sent} files, {sent_bytes / 2**30:.2f} GiB; already there: {kept}', flush=True)
            return 1
        sent += 1
        sent_bytes += size
        if size >= 64 << 20:
            seconds = max(time.time() - began, 0.001)
            print(f'{relative.as_posix()}: {size / 2**20:.0f} MiB at {size / 2**20 / seconds:.0f} MiB/s; '
                  f'{sent_bytes / 2**30:.1f} GiB sent in {(time.time() - started) / 60:.0f} min', flush=True)
    print(f'done: {sent} sent ({sent_bytes / 2**30:.2f} GiB), {kept} already there', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
