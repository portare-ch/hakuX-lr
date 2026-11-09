#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy pinned dsp56300 sources and apply local patches in order.

Usage: dsp-jit-source.py <source> <destination> [<patch>...]
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    source, dest = Path(sys.argv[1]), Path(sys.argv[2])
    patches = sorted(Path(p).resolve() for p in sys.argv[3:])
    if dest.exists():
        shutil.rmtree(dest)
    # Fresh timestamps ensure removed patches also trigger a Cargo rebuild.
    shutil.copytree(source, dest, ignore=shutil.ignore_patterns('.git', 'target'), copy_function=shutil.copy)
    # Keep git apply from using the enclosing hakuX-lr repository.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(dest.resolve().parent))
    env.pop('GIT_DIR', None)
    env.pop('GIT_WORK_TREE', None)
    for patch in patches:
        r = subprocess.run(['git', 'apply', '--verbose', '--whitespace=nowarn', str(patch)], cwd=dest,
                           env=env, capture_output=True, text=True)
        if r.returncode or 'Skipped' in r.stderr:
            sys.exit(f'dsp-jit-source: {patch.name} does not apply to {source}:\n{r.stderr}')
        print(f'dsp-jit-source: applied {patch.name}', flush=True)


if __name__ == '__main__':
    main()
