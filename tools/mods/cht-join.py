#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Join RetroArch .cht files into one file with an entry per patch.

  cht-join.py --title "Game (USA), default.xbe" Widescreen.cht "60 fps.cht" > "Game (USA).cht"

Each input's entries keep their name, code and state and are numbered in
the order given. An input's comment lines go above the file's entries,
under the name of its first entry, so each patch keeps its author, source
and test notes. hakuX-lr applies the enabled XBE patches of one XBE
together unless two of them write different bytes to the same offset;
this tool refuses such a pair (xbe-patch-v1 codes for the same CRC32:SIZE).
"""
import argparse
import re
import sys
from pathlib import Path


def fail(why):
    sys.exit(f'cht-join: {why}')


def read(path):
    """A .cht's comment lines and its entries as dicts of the cheatN_ keys."""
    notes, keys = [], {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith('#'):
            notes.append(s[1:].strip())
            continue
        k, eq, v = s.partition('=')
        if not eq:
            fail(f'{path}: not a key = value line: {s}')
        keys[k.strip()] = v.strip()
    n = int(keys.get('cheats', '0').strip('"') or 0)
    if n < 1:
        fail(f'{path}: no entries (cheats = {keys.get("cheats")})')
    entries = []
    for i in range(n):
        e = {m.group(1): v for k, v in keys.items() if (m := re.fullmatch(rf'cheat{i}_(\w+)', k))}
        if 'code' not in e or 'desc' not in e:
            fail(f'{path}: entry {i} has no desc or code')
        entries.append(e)
    return notes, entries


def patch_bytes(code):
    """An xbe-patch-v1 code's XBE (CRC32:SIZE) and its bytes by offset; None for another kind of code."""
    m = re.fullmatch(r'\s*xbe-patch-v1\s+([0-9A-Fa-f]{8}):([0-9A-Fa-f]{8})((?:\+[0-9A-Fa-f]{8}:(?:[0-9A-Fa-f]{2})+)+)'
                     r'(?:\s+screen=widescreen)?\s*', code)
    if not m:
        return None
    out = {}
    for off, data in re.findall(r'\+([0-9A-Fa-f]{8}):([0-9A-Fa-f]+)', m.group(3)):
        for i, b in enumerate(bytes.fromhex(data)):
            out[int(off, 16) + i] = b
    return (m.group(1).upper(), m.group(2).upper()), out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cht', nargs='+', help='the .cht files, in the order of their entries')
    ap.add_argument('--title', action='append', default=[], help='a comment line at the very top; repeatable')
    args = ap.parse_args()
    lines = [f'# {t}' for t in args.title]
    entries, seen = [], []
    for path in args.cht:
        notes, es = read(path)
        name = es[0]['desc'].strip('"')
        if lines:
            lines.append('#')
        lines.append(f'# {name}')
        lines += [f'#   {n}' for n in notes]
        for e in es:
            got = patch_bytes(e['code'].strip('"'))
            if got:
                for other_name, (xbe, other) in seen:
                    clash = [o for o in got[1] if xbe == got[0] and o in other and other[o] != got[1][o]]
                    if clash:
                        fail(f'"{e["desc"].strip(chr(34))}" and "{other_name}" write different bytes at '
                             f'{min(clash):08X}: hakuX-lr applies neither when both are on')
                seen.append((e['desc'].strip('"'), got))
            entries.append(e)
    lines += ['', f'cheats = {len(entries)}']
    for i, e in enumerate(entries):
        lines.append('')
        for k in ('desc', 'handler', 'code', 'enable'):
            if k in e:
                lines.append(f'cheat{i}_{k} = {e[k]}')
        lines += [f'cheat{i}_{k} = {v}' for k, v in e.items() if k not in ('desc', 'handler', 'code', 'enable')]
    sys.stdout.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
