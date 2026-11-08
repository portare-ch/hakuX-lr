#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Update the bundled dashboard from a hakux-lr-dash CI run.

Usage: dash-update.py <run-id>

Download the artifact with gh, verify its certificate, and update the XBE,
provenance pin, and licence bundle.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

REPO = 'portare-ch/hakux-lr-dash'
ROOT = Path(__file__).resolve().parents[2]
TITLE = 'hakux-dash'
VERSION = 0x7FFFFFFF


def certificate(xbe):
    """The XBE certificate's title name and version."""
    if xbe[:4] != b'XBEH':
        sys.exit('not an XBE')
    base, cert = struct.unpack_from('<I', xbe, 0x104)[0], struct.unpack_from('<I', xbe, 0x118)[0]
    c = cert - base
    title = xbe[c + 0xC:c + 0xC + 80].decode('utf-16-le').split('\0')[0]
    return title, struct.unpack_from('<I', xbe, c + 0xAC)[0]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    run = sys.argv[1]
    commit = subprocess.run(['gh', 'run', 'view', run, '-R', REPO, '--json', 'headSha', '--jq', '.headSha'],
                            check=True, capture_output=True, text=True).stdout.strip()
    with tempfile.TemporaryDirectory() as folder:
        art = Path(folder)
        subprocess.run(['gh', 'run', 'download', run, '-R', REPO, '-n', 'hakux-dash', '-D', str(art)], check=True)
        xbe = (art / 'default.xbe').read_bytes()
        title, version = certificate(xbe)
        if title != TITLE or version != VERSION:
            sys.exit(f'run {run}: title {title!r}, certificate version {version:08x}; '
                     f'want {TITLE!r}, {VERSION:08x}')
        (ROOT / 'native/dash/hakux-dash.xbe').write_bytes(xbe)
        pin = {'repository': REPO, 'commit': commit, 'run': int(run),
               'sha256': hashlib.sha256(xbe).hexdigest(), 'size': len(xbe)}
        (ROOT / 'native/dash/hakux-dash.json').write_text(json.dumps(pin, indent=2) + '\n')
        parts = [f'The hakuX-lr dashboard (native/dash/hakux-dash.xbe), built from\n'
                 f'https://github.com/{REPO} at {commit}.\n',
                 (art / 'LICENSE').read_text(), (art / 'THIRD_PARTY_NOTICES.md').read_text()]
        for text in sorted((art / 'licenses').rglob('*.txt')):
            parts.append(f'==== {text.relative_to(art)} ====\n\n{text.read_text()}')
        (ROOT / 'licenses/hakux-dash.license.txt').write_text('\n\n'.join(p.rstrip('\n') for p in parts) + '\n')
    print(f'native/dash/hakux-dash.xbe: {REPO}@{commit[:12]}, run {run}, {len(xbe)} bytes')


if __name__ == '__main__':
    main()
