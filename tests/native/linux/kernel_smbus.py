#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test SMBus EEPROM access and persistence with xboxrun.

Covers frontend overrides, unchanged writes and encrypted-section changes.
Requires clang and ld.lld for i386; exits 77 when unavailable.
Usage: kernel_smbus.py <xboxrun directory> [clang]
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# Thunk order: DbgPrint, ExQueryNonVolatileSetting, HalReadSMBusValue, HalWriteSMBusValue,
# ExSaveNonVolatileSetting.
IMPORTS = [8, 24, 45, 50, 29]


def tool(*names):
    for name in names:
        if name and shutil.which(name):
            return shutil.which(name)
    return None


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
    with tempfile.TemporaryDirectory(prefix='hakux-kernel-smbus-') as folder:
        root = Path(folder)

        def iso(mode):
            o, image = root / f'k{mode}.o', root / f'k{mode}.bin'
            subprocess.run([cc, '-target', 'i386-linux-gnu', f'-Wa,-defsym,MODE={mode}', '-c',
                            HERE / 'kernel_smbus.s', '-o', o], check=True)
            subprocess.run([ld, '-m', 'elf_i386', '--image-base=0x10000', '-Ttext=0x11000', '-e', '_start',
                            '--oformat=binary', o, '-o', image], check=True)
            path = root / f'kernel_smbus{mode}.iso'
            check.disc(path, image.read_bytes(), IMPORTS)
            return path

        env = dict(os.environ, NC_TIMEOUT='10')

        def run(mode, data, *options, markers=()):
            check.run([build / 'xboxrun', '--headless', iso(mode), data, *options], 0,
                      ('ksmbus: all passed', 'guest executed HLT', *markers), env)
            path = data / 'hdd' / 'eeprom.bin'
            return path.read_bytes(), path.stat().st_ino

        def fail(what):
            raise SystemExit(f'FAIL: {what}')

        # Check that SMBus writes reach eeprom.bin.
        eeprom, _ = run(0, root / 'data')
        if len(eeprom) != 256 or eeprom[0xF0] != 0x5A or eeprom[0xF2:0xF4] != b'\x34\x12':
            fail(f'eeprom.bin after the run: 0xF0 {eeprom[0xF0:0xF1].hex()}, 0xF2 {eeprom[0xF2:0xF4].hex()}')
        print('PASS: the SMBus writes are in eeprom.bin')

        def settings(e):
            return e[0x90], int.from_bytes(e[0x94:0x98], 'little'), int.from_bytes(e[0x64:0x68], 'little', signed=True)

        # Create an EEPROM with the frontend settings.
        data = root / 'settings'
        first, ino = run(1, data, 'lang=2', 'video=0', 'tz=0', markers=('ksmbus: language 2, video 00000000, bias 0',))
        if settings(first) != (2, 0, 0):
            fail(f'a new console keeps {settings(first)}, not the front end\'s (2, 0, 0)')
        print('PASS: a new console keeps the front end\'s settings')
        # Check new settings over SMBus and on disk.
        changed, ino2 = run(1, data, 'lang=3', 'video=00420000', 'tz=60',
                            markers=('ksmbus: language 3, video 00420000, bias -60',))
        if settings(changed) != (3, 0x00420000, -60) or ino2 == ino:
            fail(f'eeprom.bin after new settings: {settings(changed)}, written again: {ino2 != ino}')
        print('PASS: the front end\'s new settings are on the SMBus and in eeprom.bin')
        # Reboot with the same settings and check the inode.
        same, ino3 = run(1, data, 'lang=3', 'video=00420000', 'tz=60')
        if same != changed or ino3 != ino2:
            fail('the same settings wrote eeprom.bin again')
        print('PASS: the same settings leave eeprom.bin alone')
        # Change one setting and check that the file is replaced.
        one, ino4 = run(1, data, 'lang=4', 'video=00420000', 'tz=60')
        if settings(one) != (4, 0x00420000, -60) or ino4 == ino3:
            fail(f'one changed setting: {settings(one)}, written again: {ino4 != ino3}')
        print('PASS: one changed setting is kept')
        # Write the same byte over SMBus and check the inode.
        unchanged, ino5 = run(2, data, 'lang=4', 'video=00420000', 'tz=60')
        if unchanged != one or ino5 != ino4:
            fail('an unchanged SMBus write wrote eeprom.bin again')
        print('PASS: an unchanged SMBus write leaves eeprom.bin alone')
        # Save a setting after an SMBus write to the encrypted section.
        saved, _ = run(3, data, 'lang=4', 'video=00420000', 'tz=60')
        if saved[0x1C] != 0xA5 or saved[0x90] != 5 or saved[0x00:0x1C] != one[0x00:0x1C]:
            fail(f'after a setting saved: 0x1C {saved[0x1C]:02x} (written a5), language {saved[0x90]}')
        print('PASS: saving a setting leaves the encrypted section written on the SMBus')
    return 0


if __name__ == '__main__':
    sys.exit(main())
