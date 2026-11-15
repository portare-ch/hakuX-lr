#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test that a write to NV_PFIFO_INTR_0 clears the bits it names.

A title acknowledges a PFIFO interrupt by writing its bit; the register must
read 0 afterwards (pbkit waits for that).
Usage: nv2a_pfifo_intr.py <xboxrun directory>
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    build = Path(sys.argv[1]).resolve()
    spec = importlib.util.spec_from_file_location('check', HERE / 'check.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    # Write ACQUIRE_TIMEOUT's bit (0x01000000) to the register and print what it reads then.
    code = (b'\xb8\x00\x21\x00\xfd'              # mov eax, 0xFD002100: NV_PFIFO_INTR_0
            b'\xc7\x00\x00\x00\x00\x01'          # mov dword ptr [eax], 0x01000000
            b'\x8b\x08'                          # mov ecx, [eax]
            b'\x51'                              # push ecx
            b'\x68\x20\x10\x01\x00'              # push the format at 0x11020
            b'\xff\x15\x00\x0f\x01\x00'          # call [thunk 0]: DbgPrint
            b'\x83\xc4\x08\xf4').ljust(0x20, b'\0') + b'kpfifo intr %08x\0'
    with tempfile.TemporaryDirectory(prefix='hakux-nv2a-pfifo-') as folder:
        root = Path(folder)
        check.disc(root / 'pfifo.iso', code, [8])
        out = subprocess.run([str(build / 'xboxrun'), '--headless', str(root / 'pfifo.iso'), str(root / 'data')],
                             env=dict(os.environ, NC_TIMEOUT='10'), capture_output=True, text=True, timeout=15)
        text = out.stdout + out.stderr
        words = [line.split('kpfifo intr ', 1)[1].split()[0] for line in text.splitlines() if 'kpfifo intr ' in line]
        if out.returncode or not words:
            print(text)
            raise SystemExit('FAIL: the title printed nothing')
        if words[-1] != '00000000':
            raise SystemExit(f'FAIL: NV_PFIFO_INTR_0 reads {words[-1]} after its bit was written (acknowledged)')
        print('PASS: a bit written to NV_PFIFO_INTR_0 is cleared')
    return 0


if __name__ == '__main__':
    sys.exit(main())
