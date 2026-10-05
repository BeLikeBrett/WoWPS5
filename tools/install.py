#!/usr/bin/env python3
"""Install WoW Forever for PS5 on a console, and copy your own game to it.

  python3 install.py --host 192.168.1.50 --wow "/path/to/World of Warcraft"

Run it from the unpacked release bundle (the folder that holds title/ and
runtime/), or name that folder with --bundle. It uploads over the console's
FTP server:

  title/PPSA99220      ->  /data/homebrew/PPSA99220      the app (ShadowMount shows it on the home screen)
  runtime/wowps5       ->  /data/wowps5                  Wine's Windows side and an empty Windows prefix
                                                         (prefix/drive_c goes to prefix/dosdevices/c:)
  your game            ->  /data/wowps5/prefix/dosdevices/c:/Games/World of Warcraft   (about 64 GiB)

Your installation is only read. A file already on the console with the same
size is skipped, so an interrupted run is continued by running it again.
Nothing of an account is copied: you sign in on the console.
"""
import argparse
import sys
import time
from ftplib import FTP, all_errors, error_perm
from pathlib import Path

GAME_TARGET = 'prefix/dosdevices/c:/Games/World of Warcraft'
SKIP_TOP = {'_classic_', '_retail_', '.battle.net', 'World of Warcraft Launcher.exe'}
SKIP_FLAVOR = {'WTF', 'Interface', 'Logs', 'Errors', 'Cache', 'Screenshots',
               'vkd3d-proton.cache', 'vkd3d-proton.cache.write'}


class Console:
    def __init__(self, args):
        self.args = args
        self.made = set()
        self.connect()

    def connect(self):
        self.ftp = FTP()
        self.ftp.connect(self.args.host, self.args.port, timeout=600)
        self.ftp.login(self.args.user, self.args.password)
        self.ftp.voidcmd('TYPE I')

    def size(self, path):
        try:
            return self.ftp.size(path)
        except all_errors:
            return None

    def ensure(self, directory):
        path = ''
        for part in directory.strip('/').split('/'):
            path += '/' + part
            if path not in self.made:
                try:
                    self.ftp.mkd(path)
                except error_perm:
                    pass
                self.made.add(path)

    def send(self, local, remote):
        """True when sent, False when it was there already."""
        size = local.stat().st_size
        # The console's FTP servers answer for an executable with another size than
        # the file's (they read it as the system would): those are sent every time.
        signed = local.name == 'eboot.bin' or local.suffix in ('.prx', '.sprx', '.self', '.elf')
        if not signed and self.size(remote) == size:
            return False
        self.ensure(remote.rsplit('/', 1)[0])
        problem = 'the size on the console does not match'
        for attempt in (1, 2, 3):
            try:
                with local.open('rb') as stream:
                    self.ftp.storbinary('STOR ' + remote, stream, blocksize=1024 * 1024)
                if signed or self.size(remote) == size:
                    return True
            except all_errors as error:
                problem = str(error) or type(error).__name__
            time.sleep(2)
            try:
                self.connect()
            except all_errors as error:
                problem = f'the console stopped answering ({error})'
        sys.exit(f'could not send {local.name} to {remote}: {problem}\n'
                 'Is the console still on, with free space? If the app is running on the console, close it first.')


def upload_tree(console, source, target, what):
    files = sorted(p for p in source.rglob('*') if p.is_file())
    total = sum(p.stat().st_size for p in files)
    print(f'{what}: {len(files)} files, {total / 2**20:.0f} MiB, to {target}', flush=True)
    sent = done = 0
    began = time.time()
    for path in files:
        relative = path.relative_to(source).as_posix()
        if relative.startswith('prefix/drive_c/'):       # the console's name for it, which a Windows PC cannot hold
            relative = 'prefix/dosdevices/c:/' + relative[len('prefix/drive_c/'):]
        sent += console.send(path, f'{target}/{relative}')
        done += path.stat().st_size
        if time.time() - began > 15:
            began = time.time()
            print(f'  {done * 100 // max(total, 1)}%', flush=True)
    print(f'  done: {sent} sent, {len(files) - sent} already there', flush=True)


def game_files(install):
    for path in sorted(install.rglob('*')):
        relative = path.relative_to(install)
        if relative.parts[0] in SKIP_TOP:
            continue
        if relative.parts[0] == '_classic_beta_' and len(relative.parts) > 1 and relative.parts[1] in SKIP_FLAVOR:
            continue
        if path.is_file() and not path.is_symlink():
            yield relative, path


def upload_game(console, install, target):
    if not (install / '_classic_beta_' / 'WowB.exe').is_file():
        sys.exit(f'{install} does not hold _classic_beta_/WowB.exe: --wow is the folder with _classic_beta_ and Data in it')
    files = list(game_files(install))
    total = sum(path.stat().st_size for _, path in files)
    print(f'the game: {len(files)} files, {total / 2**30:.1f} GiB, to {target}', flush=True)
    sent_bytes = sent = 0
    started = time.time()
    for relative, path in files:
        if console.send(path, f'{target}/{relative.as_posix()}'):
            sent += 1
            sent_bytes += path.stat().st_size
            if path.stat().st_size >= 64 << 20:
                minutes = (time.time() - started) / 60
                print(f'  {relative.as_posix()}: {sent_bytes / 2**30:.1f} GiB sent in {minutes:.0f} min', flush=True)
    print(f'  done: {sent} sent ({sent_bytes / 2**30:.1f} GiB), {len(files) - sent} already there', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--host', required=True, help="the console's address on your network")
    parser.add_argument('--port', type=int, default=2121, help="the console's FTP port (default 2121)")
    parser.add_argument('--user', default='anonymous')
    parser.add_argument('--password', default='')
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent,
                        help='the unpacked release bundle (default: where this script is)')
    parser.add_argument('--wow', type=Path, help='your World of Warcraft folder (the one with _classic_beta_ and Data)')
    parser.add_argument('--skip-app', action='store_true', help='only copy the game')
    parser.add_argument('--title-dir', default='/data/homebrew/PPSA99220', help=argparse.SUPPRESS)
    parser.add_argument('--root', default='/data/wowps5', help=argparse.SUPPRESS)
    args = parser.parse_args()
    title, runtime = args.bundle / 'title' / 'PPSA99220', args.bundle / 'runtime' / 'wowps5'
    if not args.skip_app and not ((title / 'eboot.bin').is_file() and runtime.is_dir()):
        sys.exit(f'{args.bundle} is not an unpacked release bundle (no title/PPSA99220/eboot.bin and runtime/wowps5)')
    try:
        console = Console(args)
    except all_errors as error:
        sys.exit(f'no FTP server at {args.host}:{args.port} ({error}): is the console on, jailbroken, and its FTP server running?')
    if not args.skip_app:
        upload_tree(console, title, args.title_dir, 'the app')
        upload_tree(console, runtime, args.root, 'Wine and the Windows prefix')
    if args.wow:
        upload_game(console, args.wow, f'{args.root}/{GAME_TARGET}')
    else:
        print('no --wow given: the game was not copied')
    print('finished. On the console: "WoW Forever" on the home screen (ShadowMount adds it within a minute).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
