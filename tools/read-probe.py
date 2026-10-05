#!/usr/bin/env python3
"""Produce a shareable feature summary from this title's klog lines only."""
import argparse
import json
import re
from pathlib import Path

def parse(text):
    extensions = []
    features, limits = {}, {}
    version = None
    identity = None
    compute = False
    for line in text.splitlines():
        prefix = '[WoWPS5 Probe] '
        if not line.startswith(prefix):
            continue
        line = line[len(prefix):]
        if line.startswith('probe extension '):
            extensions.append(line.split()[-1])
        elif line.startswith('probe feature '):
            features.update({k: bool(int(v)) for k, v in re.findall(r'(\w+)=(\d+)', line)})
        elif line.startswith('probe limit '):
            limits.update({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)})
        elif line.startswith('probe identity '):
            identity = line.split(';')[0].split()[-1]
            match = re.search(r'Vulkan (\d+\.\d+\.\d+)', line)
            version = match.group(1) if match else None
        elif line.startswith('probe RGBA16F compute readback PASS; mismatches=0'):
            compute = True
    if not identity:
        raise ValueError('No WoWPS5 probe identity in the log')
    return {
        'schema': 1, 'build': identity, 'vulkanVersion': version,
        'rgba16fComputePassed': compute,
        'features': features, 'limits': limits,
        'extensions': sorted(set(extensions)),
        'vkd3dExtensionPresence': {
            name: name in extensions for name in ['VK_EXT_robustness2', 'VK_KHR_push_descriptor']
        },
        'scope': 'Native driver queries and RGBA16F compute only; feature list is a subset. Windows/D3D12/WoW runtime compatibility untested.',
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = parse(args.log.read_text(errors='replace'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    if not report['rgba16fComputePassed']:
        parser.exit(1, 'RGBA16F compute did not pass.\n')
    print('Native probe summary saved:', args.output)

if __name__ == '__main__':
    main()
