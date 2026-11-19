#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate Linux release notes from CHANGELOG.md, with install and build links.
Fail if the version has no changelog section.
    python3 tools/native/linux-release-notes.py <version> <commit> [<previous tag> [<run url>]]
"""
from pathlib import Path
import re
import sys

if not 3 <= len(sys.argv) <= 5:
    sys.exit(__doc__)
version, commit = sys.argv[1], sys.argv[2]
previous = sys.argv[3] if len(sys.argv) > 3 else ''
run = sys.argv[4] if len(sys.argv) > 4 else ''
repo = 'https://github.com/portare-ch/hakuX-lr'
tag = f'linux-v{version}'

changelog = (Path(__file__).resolve().parents[2] / 'CHANGELOG.md').read_text()
section = re.search(rf'^## {re.escape(tag)}(?:[ \t][^\n]*)?\n(.*?)(?=^## |\Z)', changelog, re.M | re.S)
if not section:
    sys.exit(f'CHANGELOG.md has no "## {tag}" section')

built = f'Built from [{commit[:10]}]({repo}/commit/{commit})'
built += f' by [CI]({run}).' if run else '.'
print(f'hakuX-lr {version} for Linux ARM64, Linux x86-64 and Android ARM64 (experimental).\n')
print(section.group(1).strip() + '\n')
print('### Install\n')
print(f'[PortareOS core override]({repo}/blob/{tag}/USER_GUIDE.md#portareos)\n')
print('For other ARM64 Linux systems, follow the '
      f'[RetroArch installation guide]({repo}/blob/{tag}/README.md#play-on-other-aarch64-linux-systems).\n')
print('For AMD64 (x86-64) Linux, download the `x86_64` archive and follow the '
      f'[KVM setup and RetroArch installation guide]({repo}/blob/{tag}/README.md#play-on-amd64-x86-64-linux-systems).\n')
print(f'[Android ARM64 installation guide]({repo}/blob/{tag}/README.md#play-on-android)\n')
print(f'[Upgrade from the old core name]({repo}/blob/{tag}/USER_GUIDE.md#upgrading-from-the-old-core-name)\n')
print(built + ' `BUILD-INFO` lists the source revision and pinned dependencies.')
if previous:
    print(f'\n[Full changelog]({repo}/compare/{previous}...{tag})')
