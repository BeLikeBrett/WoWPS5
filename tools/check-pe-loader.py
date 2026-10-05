#!/usr/bin/env python3
"""Exercise actual relocated Windows code and reject malformed PE fixtures."""
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/windows'
WORK = ROOT / 'work/pe-loader-check'
WORK.mkdir(parents=True, exist_ok=True)
subprocess.run(['bash', str(ROOT / 'tools/build-pe-smoke.sh')], check=True)
subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-DWOWPS5_PE_HOST_TEST',
                str(ROOT / 'app/runtime/pe_probe.cpp'), '-o', str(BUILD / 'pe-host-test')], check=True)
original = (BUILD / 'pe-smoke.exe').read_bytes()
pe = struct.unpack_from('<I', original, 0x3c)[0]
opt = pe + 24
sections = opt + struct.unpack_from('<H', original, pe + 20)[0]
cases = [('valid', original, True), ('truncated', original[:80], False)]
bad = bytearray(original)
struct.pack_into('<H', bad, pe + 4, 0x14c)
cases.append(('wrong-architecture', bad, False))
bad = bytearray(original)
struct.pack_into('<I', bad, sections + 12, 0xffff0000)
cases.append(('section-outside-image', bad, False))
bad = bytearray(original)
struct.pack_into('<I', bad, sections + 20, len(bad) - 1)
cases.append(('section-outside-file', bad, False))
bad = bytearray(original)
struct.pack_into('<I', bad, sections + 40 + 12, struct.unpack_from('<I', bad, sections + 12)[0])
cases.append(('overlapping-sections', bad, False))
results = []
for name, data, expected in cases:
    exe, report = WORK / (name + '.exe'), WORK / (name + '.json')
    exe.write_bytes(data)
    result = subprocess.run([str(BUILD / 'pe-host-test'), str(exe), str(WORK / 'absent-client'), str(report)],
                            capture_output=True, text=True, timeout=10)
    actual = json.loads(report.read_text())['smokePassed']
    if actual != expected or result.returncode != (0 if expected else 1):
        raise SystemExit(f'{name}: unexpected result {result.returncode}, smokePassed={actual}')
    results.append({'case': name, 'passed': True})
print(json.dumps({'checks': results, 'platform': 'Linux host; PS5 tested separately'}, indent=2))
