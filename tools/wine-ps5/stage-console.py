#!/usr/bin/env python3
"""Stage Wine's Windows-side files and test programs on the console.

  tools/wine-ps5/stage-console.py [--all | --core] [--tree work/wine-memsim-host] [--system32] [program.exe ...]

Uploads over the console's FTP server, only what has changed, to
/data/wowps5/wine (the layout Wine's loader is given on the console): the PE
DLLs built in a host tree with the console's syscall thunk address, stripped,
and the NLS tables. Programs go to /data/wowps5/test. Nothing is deleted.

What has changed: a file is sent when its size on the console differs, or when
its contents differ from what this script last sent there (hashes kept in
work/console-staged.json). Size alone is not enough: a rebuilt DLL is often
the size it was, and a console with two builds of one DLL (Wine's own folder
and the prefix's system folder) fails in ways that look like anything else.
--force sends everything.
"""
import argparse
import hashlib
import io
import json
import subprocess
import sys
import tempfile
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'vendor/PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

DLLS = ['ntdll', 'kernel32', 'kernelbase']


def ensure(ftp, directory):
    path = ''
    for part in directory.strip('/').split('/'):
        path += '/' + part
        try:
            ftp.mkd(path)
        except error_perm:
            pass


MANIFEST = ROOT / 'work/console-staged.json'


def send(ftp, data, target, always, manifest, digest=None):
    # (a module's digest is of the file it was stripped from: strip stamps its output with the time)
    digest = digest or hashlib.sha256(data).hexdigest()
    try:
        if not always and manifest.get(target) == digest and ftp.size(target) == len(data):
            return False
    except error_perm:
        pass
    ftp.storbinary('STOR ' + target, io.BytesIO(data), blocksize=1024 * 1024)
    manifest[target] = digest
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tree', default=str(ROOT / 'work/wine-memsim-host'))
    parser.add_argument('--all', action='store_true', help='every PE module the tree built (DLLs, drivers, programs)')
    parser.add_argument('--module', action='append', default=[], help='stage one named PE DLL (repeatable)')
    parser.add_argument('--core', action='store_true', help='only ntdll, kernel32 and kernelbase from the tree')
    parser.add_argument('--prefix', help='a prefix made on the host by the same Wine: its registry and drive_c skeleton go to /data/wowps5/prefix')
    parser.add_argument('--force', action='store_true', help='send every file, whatever was sent before')
    parser.add_argument('--ca-bundle', help='stage a PEM trust bundle for console certificate validation')
    parser.add_argument('--system32', action='store_true',
                        help="also put each module in the prefix's system folder, replacing the copy there: a program "
                             'that reads a system DLL from disk (a protected game restoring ntdll) must find the build that is loaded')
    parser.add_argument('--keep', default='d3d12.dll,d3d12core.dll,dxgi.dll',
                        help='system folder files --system32 leaves alone (the DirectX translation libraries put there by hand)')
    parser.add_argument('programs', nargs='*')
    args = parser.parse_args()
    tree = Path(args.tree)
    settings = ps5_console.load_settings()
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    ftp.voidcmd('TYPE I')
    sent = kept = 0
    files = []
    if args.ca_bundle:
        bundle = Path(args.ca_bundle).read_bytes()
        if b'-----BEGIN CERTIFICATE-----' not in bundle:
            raise ValueError('CA bundle must contain PEM certificates')
        files.append((bundle, '/data/wowps5/ca-certificates.crt'))
    # Modules are sent only when asked for. Sending the core DLLs with every test
    # program, from whichever tree was the default, left the console with ntdll
    # from one build in Wine's folder and from another in the system folder.
    modules = [tree / f'dlls/{name}/x86_64-windows/{name}.dll' for name in DLLS] if args.core else []
    modules.extend(tree / f'dlls/{name}/x86_64-windows/{name}.dll' for name in args.module)
    if args.all:
        # one flat directory, as an installed Wine has: lib/wine/x86_64-windows
        kinds = {'.dll', '.exe', '.sys', '.drv', '.ocx', '.cpl', '.acm', '.ax', '.tlb', '.ds', '.com', '.msstyles'}
        modules = sorted(path for top in ('dlls', 'programs') for path in (tree / top).glob('*/x86_64-windows/*')
                         if path.suffix in kinds)
    with tempfile.TemporaryDirectory() as scratch:
        for source in modules:
            stripped = Path(scratch) / source.name
            done = subprocess.run(['x86_64-w64-mingw32-strip', '--strip-debug', '-o', str(stripped), str(source)],
                                  capture_output=True)
            data = stripped.read_bytes() if done.returncode == 0 else source.read_bytes()   # .tlb and the like are not PE
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            files.append((data, f'/data/wowps5/wine/lib/wine/x86_64-windows/{source.name}', digest))
            if args.system32 and source.name not in args.keep.split(','):
                files.append((data, f'/data/wowps5/prefix/dosdevices/c:/windows/system32/{source.name}', digest))
    for table in sorted((tree / 'nls').glob('*.nls')):
        files.append((table.read_bytes(), f'/data/wowps5/wine/share/wine/nls/{table.name}'))
    programs = set()   # always sent: a rebuilt test program is often the size it was
    for program in args.programs:
        files.append((Path(program).read_bytes(), f'/data/wowps5/test/{Path(program).name}'))
        programs.add(files[-1][1])
    directories = set()
    if args.prefix:
        # The console cannot run wineboot (no child processes), so the prefix is
        # made on the host and shipped whole: the registry, every directory, the
        # data files, and the builtin modules wineboot copied into system32
        # (stripped). Those are what a program's imports are found through:
        # names on drive C: match whatever their case, while the installation
        # directory is searched by exact name only. Drive C: is a real directory
        # on the console, where symbolic links cannot be made.
        prefix = Path(args.prefix)
        binaries = {'.dll', '.exe', '.sys', '.drv', '.ocx', '.cpl', '.acm', '.ax', '.ds', '.com'}
        for name in ['system.reg', 'user.reg', 'userdef.reg']:
            files.append(((prefix / name).read_bytes(), f'/data/wowps5/prefix/{name}'))
        drive = prefix / 'drive_c'
        with tempfile.TemporaryDirectory() as scratch:
            for path in sorted(drive.rglob('*')):
                relative = path.relative_to(drive)
                if relative.parts[:2] == ('windows', 'syswow64'):
                    continue                 # 32-bit modules: this Wine is 64-bit only
                target = '/data/wowps5/prefix/dosdevices/c:/' + relative.as_posix()
                if path.is_dir():            # a link to a host directory becomes a plain directory
                    directories.add(target)
                elif path.is_file() and not path.is_symlink():
                    data = None
                    if path.suffix.lower() in binaries:
                        stripped = Path(scratch) / 'module'
                        if subprocess.run(['x86_64-w64-mingw32-strip', '--strip-debug', '-o', str(stripped), str(path)],
                                          capture_output=True).returncode == 0:
                            data = stripped.read_bytes()
                    files.append((data if data is not None else path.read_bytes(), target))
    for directory in sorted(directories | {entry[1].rsplit('/', 1)[0] for entry in files}):
        ensure(ftp, directory)
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
    try:
        for data, target, *digest in files:
            if send(ftp, data, target, args.force or target in programs, manifest, *digest):
                sent += 1
            else:
                kept += 1
    finally:
        MANIFEST.write_text(json.dumps(manifest, indent=0, sort_keys=True) + '\n')
    ftp.quit()
    print(f'staged: {sent} sent, {kept} already there')


if __name__ == '__main__':
    main()
