#!/usr/bin/env python3
"""Make the release bundle from a console where the port works, and this tree's title.

  tools/make-bundle.py [--out build/release]

The console's /data/wowps5 is the tested state of Wine's Windows side and of
the prefix (parts of it were put there by hand, the DirectX translation
libraries among them), so the bundle is a copy of it without the game, the
shader caches, the test programs and anything a session wrote:

  WoWForever-PS5/title/PPSA99220     app/dist/PPSA99220 of this tree
  WoWForever-PS5/runtime/wowps5      /data/wowps5: wine/, prefix/ (no Games), ca-certificates.crt;
                                     the console's prefix/dosdevices/c: is prefix/drive_c here, a name
                                     every PC can hold (install.py puts it back)
  WoWForever-PS5/install.py          tools/install.py
  WoWForever-PS5/README.txt, MANIFEST.sha256

Close the game first: the prefix's registry is written when it ends.
"""
import argparse
import hashlib
import shutil
import sys
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'vendor/PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

REMOTE = '/data/wowps5'
TOP = {'wine', 'prefix', 'ca-certificates.crt'}                  # not client/, test/, launch.txt
SKIP_PATHS = {'prefix/dosdevices/c:/Games'}                      # the game is the user's own
SKIP_NAMES = {'vkd3d-proton.cache', 'vkd3d-proton.cache.write'}


def walk(ftp, remote, relative=''):
    rows = []
    ftp.retrlines('LIST ' + remote, rows.append)
    for row in rows:
        fields = row.split(None, 8)
        name = fields[8]
        if name in ('.', '..') or name in SKIP_NAMES:
            continue
        path = f'{relative}/{name}'.lstrip('/')
        if not relative and name not in TOP or path in SKIP_PATHS:
            continue
        if row.startswith('d'):
            yield from walk(ftp, f'{remote}/{name}', path)
        else:
            yield path, int(fields[4])


def local_name(path):
    return path.replace('prefix/dosdevices/c:/', 'prefix/drive_c/', 1)


def sanitize(runtime):
    """Take out what a session, an account or the building machine left in the prefix.

    The game keeps its Battle.net identity and installation ids in the registry
    and under the user's AppData; the host Wine that made the prefix wrote paths
    of the machine it ran on. None of it belongs in a bundle for other people.
    """
    import re
    users = runtime / 'prefix' / 'drive_c' / 'users'
    for path in sorted(p for p in users.rglob('*') if p.is_file()):
        relative = path.relative_to(users).as_posix()
        if '/AppData/Local/Temp/' in relative or '/Blizzard Entertainment/' in relative or '/Battle.net/' in relative:
            path.unlink()
    for directory in sorted((p for p in users.rglob('*') if p.is_dir()), reverse=True):
        if 'Blizzard Entertainment' in directory.as_posix() and not any(directory.iterdir()):
            directory.rmdir()
    for user in (p for p in users.iterdir() if p.is_dir()):
        temp = user / 'AppData' / 'Local' / 'Temp'           # must exist: nothing on the console creates it
        temp.mkdir(parents=True, exist_ok=True)
        (temp / '.keep').write_text('')
    for name in ('system.reg', 'user.reg', 'userdef.reg'):
        path = runtime / 'prefix' / name
        kept, skipping = [], False
        for line in path.read_text(encoding='utf-8', errors='surrogateescape').split('\n'):
            if line.startswith('['):
                skipping = bool(re.match(r'\[Software\\\\(Wow6432Node\\\\)?Blizzard Entertainment', line))
            if skipping:
                continue
            if re.search(r'[A-Za-z]:\\\\home\\\\', line) or '/home/' in line:   # a path of the machine the prefix was made on
                continue
            kept.append(line)
        path.write_text('\n'.join(kept), encoding='utf-8', errors='surrogateescape')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--no-fetch', action='store_true', help="keep the runtime already in --out (the console is not read): only the title and the archive are made again")
    args = parser.parse_args()
    bundle = args.out / 'WoWForever-PS5'
    runtime = bundle / 'runtime' / 'wowps5'
    title = ROOT / 'app/dist/PPSA99220'
    if not (title / 'eboot.bin').is_file():
        sys.exit('build the title first (tools/wine-ps5/build-console.sh)')
    fetched = 0
    if not args.no_fetch:
        settings = ps5_console.load_settings()
        ftp = FTP()
        ftp.connect(settings['host'], settings['ftp_port'], timeout=600)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        ftp.voidcmd('TYPE I')
        files = list(walk(ftp, REMOTE))
        print(f'{len(files)} files, {sum(size for _, size in files) / 2**20:.0f} MiB on the console', flush=True)
        for path, size in files:
            local = runtime / local_name(path)
            if local.is_file() and local.stat().st_size == size and not path.endswith('.reg'):
                continue
            local.parent.mkdir(parents=True, exist_ok=True)
            with local.open('wb') as stream:
                ftp.retrbinary(f'RETR {REMOTE}/{path}', stream.write, blocksize=1 << 20)
            if local.stat().st_size != size and not path.endswith('.reg'):
                sys.exit(f'{path}: {local.stat().st_size} of {size} bytes')
            fetched += 1
        wanted = {runtime / local_name(path) for path, _ in files}
        for local in sorted(p for p in runtime.rglob('*') if p.is_file() and p not in wanted):
            local.unlink()                                       # gone from the console since the last bundle
        for name in ('system.reg', 'user.reg', 'userdef.reg'):
            text = (runtime / 'prefix' / name).read_bytes()
            if not text.startswith(b'WINE REGISTRY') or not text.endswith(b'\n'):
                sys.exit(f'{name} is not whole: close the game and run this again')
        sanitize(runtime)
    elif not (runtime / 'prefix' / 'user.reg').is_file():
        sys.exit('--no-fetch needs a runtime from an earlier run')
    shutil.rmtree(bundle / 'title', ignore_errors=True)
    shutil.copytree(title, bundle / 'title' / 'PPSA99220')
    shutil.copy(ROOT / 'tools/install.py', bundle / 'install.py')
    (bundle / 'README.txt').write_text(
        'WoW Forever for PS5: Wine, vkd3d-proton and RADV in one PS5 app. The game is not included.\n\n'
        'Install, with the console on, jailbroken, its FTP server running and ShadowMount loaded:\n\n'
        '  python3 install.py --host <the console\'s address> --wow "<your World of Warcraft folder>"\n\n'
        'The guide, the source of everything here and the licences: see the project page.\n'
        'Licence notices of what the app contains: title/PPSA99220/licenses/.\n')
    lines = []
    for path in sorted(p for p in bundle.rglob('*') if p.is_file() and p.name != 'MANIFEST.sha256'):
        lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(bundle).as_posix()}')
    (bundle / 'MANIFEST.sha256').write_text('\n'.join(lines) + '\n')
    archive = args.out / 'WoWForever-PS5.zip'
    import zipfile
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle_zip:
        for path in sorted(p for p in bundle.rglob('*') if p.is_file()):
            bundle_zip.write(path, Path('WoWForever-PS5') / path.relative_to(bundle))
    print(f'{fetched} fetched; {len(lines)} files in the bundle; {archive} is {archive.stat().st_size / 2**20:.0f} MiB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
