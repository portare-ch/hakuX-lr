#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the console memory test on xboxrun: 64, 128 and 256 MiB, and a title
whose XBE limits it to 64 MiB on a console with more, as it is and with the
limit patched out as it loads.

Assembles kernel_ram.s for i386 with clang and ld.lld; skips (exit 77) if
either tool is unavailable.
Usage: kernel_ram.py <xboxrun's directory> [clang]
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# Thunk order: DbgPrint, MmAllocateContiguousMemory, MmQueryStatistics, NtAllocateVirtualMemory,
# MmClaimGpuInstanceMemory.
IMPORTS = [8, 165, 181, 184, 168]
XBE = 34 * 2048         # default.xbe in check.disc's image
INIT_FLAGS = 0x124
LIMIT_64MB = 0x4
GPU_REACH = 128


def tool(*names):
    for name in names:
        if name and shutil.which(name):
            return shutil.which(name)
    return None


def boot(build, iso, data, mib, unlimit=False):
    """The test's totals on a console of mib: (total pages, the NV2A's memory, the lower and the higher of its
    two contiguous blocks (the second allocated after the GPU's instance memory was claimed), pages committed).
    unlimit: the titles' 64 MiB limit patched out as they load."""
    env = dict(os.environ, NC_TIMEOUT='30', NC_RAM_MB=str(mib), NC_RAM_UNLIMIT='1' if unlimit else '0')
    result = subprocess.run([str(build / 'xboxrun'), '--headless', str(iso), str(data)], env=env,
                            capture_output=True, text=True, timeout=60)
    out = result.stdout + result.stderr
    m = re.search(r'kram: total (\d+) pages, gpu ([0-9a-f]{8}), block ([0-9a-f]{8}), after the instance claim ([0-9a-f]{8}), '
                  r'committed (\d+) pages', out)
    if result.returncode or not m or f'RAM {mib} MiB' not in out:
        print(out[-3000:])
        raise SystemExit(f'FAIL: {iso.name} on {mib} MiB: exit {result.returncode}')
    return int(m[1]), int(m[2], 16), min(int(m[3], 16), int(m[4], 16)), max(int(m[3], 16), int(m[4], 16)), int(m[5]), out


def main():
    build = Path(sys.argv[1]).resolve()
    cc = tool(sys.argv[2] if len(sys.argv) > 2 else None, os.environ.get('CC'), 'clang', 'clang-19')
    ld = tool('ld.lld', 'ld.lld-19')
    if not cc or not ld:
        print('SKIP: no clang or ld.lld for i386')
        return 77
    spec = importlib.util.spec_from_file_location('check', HERE / 'check.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    with tempfile.TemporaryDirectory(prefix='hakux-kernel-ram-') as folder:
        root = Path(folder)
        subprocess.run([cc, '-target', 'i386-linux-gnu', '-c', HERE / 'kernel_ram.s', '-o', root / 'k.o'], check=True)
        subprocess.run([ld, '-m', 'elf_i386', '--image-base=0x10000', '-Ttext=0x11000', '-e', '_start',
                        '--oformat=binary', root / 'k.o', '-o', root / 'k.bin'], check=True)
        check.disc(root / 'kernel_ram.iso', (root / 'k.bin').read_bytes(), IMPORTS)
        limited = bytearray((root / 'kernel_ram.iso').read_bytes())
        struct.pack_into('<I', limited, XBE + INIT_FLAGS, LIMIT_64MB)
        (root / 'kernel_ram_64.iso').write_bytes(limited)

        for mib in (64, 128, 256):
            total, gpu, block, high, committed, out = boot(build, root / 'kernel_ram.iso', root / 'data', mib)
            reach = min(mib, GPU_REACH) << 20
            # nearly all of it is the title's: the kernel's own and the spare are a few MiB
            ok = (total == mib * 256 and gpu == reach and 0x80000000 <= block and high + 0x1000 <= 0x80000000 + reach
                  and committed >= (mib - 8) * 256 and 'limits itself' not in out)
            if not ok:
                raise SystemExit(f'FAIL: {mib} MiB: total {total} pages, gpu {gpu:08x}, block {block:08x}, '
                                 f'committed {committed} pages')
            print(f'PASS: {mib} MiB: {total} pages, the NV2A has {gpu >> 20} MiB, a contiguous block at {block:08x}, '
                  f'{committed} pages committed and written')
        for mib in (64, 128, 256):
            total, gpu, block, high, committed, out = boot(build, root / 'kernel_ram_64.iso', root / 'data', mib)
            ok = (total == 64 * 256 and high + 0x1000 <= 0x80000000 + (64 << 20) and (64 - 8) * 256 <= committed < 64 * 256
                  and ('limits itself' in out) == (mib > 64))
            if not ok:
                raise SystemExit(f'FAIL: limited to 64 MiB on {mib} MiB: total {total} pages, block {block:08x}, '
                                 f'committed {committed} pages')
            print(f'PASS: a title limited to 64 MiB on {mib} MiB: {total} pages, {committed} committed and written')
        for mib in (64, 128, 256):
            total, gpu, block, high, committed, out = boot(build, root / 'kernel_ram_64.iso', root / 'data', mib, True)
            ok = (total == mib * 256 and committed >= (mib - 8) * 256 and 'limits itself' not in out
                  and ('patched out' in out) == (mib > 64))
            if not ok:
                raise SystemExit(f'FAIL: the limit patched out on {mib} MiB: total {total} pages, '
                                 f'committed {committed} pages')
            print(f'PASS: a title limited to 64 MiB, the limit patched out, on {mib} MiB: {total} pages, '
                  f'{committed} committed and written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
