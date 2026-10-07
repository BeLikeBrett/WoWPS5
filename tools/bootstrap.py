#!/usr/bin/env python3
"""Fetch pinned upstream sources and isolated host packages. No global installs."""
import hashlib
import json
import os
import shutil
import subprocess
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PINS = json.loads((ROOT / 'dependencies.json').read_text())

def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)

def main():
    for tool in ['git', 'clang', 'clang++', 'cmake', 'ninja', 'meson',
                 'pkg-config', 'glslangValidator', 'bsdtar']:
        if not shutil.which(tool):
            raise SystemExit('Missing host tool: ' + tool)
    # These LLVM-linked package pins are for the verified LLVM 23 host.
    if 'version 23.' not in subprocess.check_output(['clang', '--version'], text=True):
        raise SystemExit('Host LLVM differs from the tested LLVM 23 package pins.')
    vendor = ROOT / 'vendor'
    vendor.mkdir(exist_ok=True)
    for name in ['PS5_Vulkan', 'PS5_Mesa', 'PS5_PayloadSDK', 'klogsrv']:
        owner = 'ps5-payload-dev' if name == 'klogsrv' else 'mihawk-99'
        target = vendor / name
        new = not target.exists()
        if new:
            run('git', 'clone', '--depth', '1', '--filter=blob:none',
                f'https://github.com/{owner}/{name}.git', str(target))
        required = [PINS[name]]
        if name == 'PS5_PayloadSDK':
            required.append(PINS['driverSDK'])
        for revision in required:
            check = subprocess.run(['git', '-C', str(target), 'cat-file', '-e',
                                    revision + '^{commit}'], capture_output=True)
            if check.returncode:
                run('git', '-C', str(target), 'fetch', '--depth', '1', 'origin', revision)
        if new:
            run('git', '-C', str(target), 'checkout', '--detach', PINS[name])
    cache = ROOT / '.deps/host-tools'
    downloads = cache / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    for package in PINS['hostPackages']:
        path = downloads / package['url'].rsplit('/', 1)[1]
        if not path.exists():
            urllib.request.urlretrieve(package['url'], path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != package['sha256']:
            raise SystemExit('Package checksum mismatch: ' + path.name)
        marker = cache / (path.name + '.installed')
        if not marker.exists():
            run('bsdtar', '-xf', str(path), '-C', str(cache))
            marker.touch()
    for file in (cache / 'usr/lib/pkgconfig').glob('*.pc'):
        file.write_text(file.read_text().replace('prefix=/usr', 'prefix=' + str(cache / 'usr')))
    print('Pinned sources and isolated host tools ready. Run bash tools/build.sh.')

if __name__ == '__main__':
    main()
