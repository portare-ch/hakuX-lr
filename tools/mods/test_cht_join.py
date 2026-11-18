#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for cht-join.py: entries renumbered, notes kept, conflicting XBE patches refused."""
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / 'cht-join.py'
failed = 0


def check(ok, what):
    global failed
    print(('PASS: ' if ok else 'FAIL: ') + what)
    failed += not ok


def cht(desc, code, note):
    return (f'# {note}\ncheats = 1\ncheat0_desc = "{desc}"\ncheat0_handler = 0\n'
            f'cheat0_code = "{code}"\ncheat0_enable = false\n')


def join(*args):
    r = subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def main():
    wide = 'xbe-patch-v1 1A2B3C4D:00200000+00001234:9090 screen=widescreen'
    fps = 'xbe-patch-v1 1A2B3C4D:00200000+00001235:90+00004321:EB'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'w.cht').write_text(cht('Widescreen [restart]', wide, 'by someone'))
        (tmp / 'f.cht').write_text(cht('60 fps [restart]', fps, 'tested'))
        (tmp / 'x.cht').write_text(cht('Other [restart]', 'xbe-patch-v1 1A2B3C4D:00200000+00001234:91', 'clashes'))
        (tmp / 'o.cht').write_text(cht('Other disc [restart]', 'xbe-patch-v1 99999999:00200000+00001234:91', 'fine'))
        (tmp / 'c.cht').write_text(cht('Infinite lives', 'w8 0x100 1', 'a cheat'))
        rc, out, err = join('--title', 'Game (USA)', tmp / 'w.cht', tmp / 'f.cht')
        check(rc == 0, f'two entries join ({err.strip()})')
        check(out.startswith('# Game (USA)\n#\n# Widescreen [restart]\n#   by someone\n#\n# 60 fps [restart]\n#   tested\n')
              and '\ncheats = 2\n' in out, 'the title, each entry\'s notes under its name, the count')
        check(f'cheat0_code = "{wide}"' in out and f'cheat1_code = "{fps}"' in out and
              'cheat1_desc = "60 fps [restart]"' in out and out.count('_enable = false') == 2,
              'entries renumbered in order, codes unchanged (a byte both write the same is fine)')
        (tmp / 'j.cht').write_text(out)
        rc, again, err = join(tmp / 'j.cht', tmp / 'o.cht', tmp / 'c.cht')
        check(rc == 0 and '\ncheats = 4\n' in again and 'cheat3_code = "w8 0x100 1"' in again and
              f'cheat1_code = "{fps}"' in again, f'a joined file joins again, with another disc\'s patch and a cheat ({err.strip()})')
        rc, _, err = join(tmp / 'w.cht', tmp / 'x.cht')
        check(rc != 0 and 'write different bytes at 00001234' in err, f'a conflicting pair is refused: {err.strip()}')
        (tmp / 'bad.cht').write_text('cheats = 1\ncheat0_desc = "x"\n')
        rc, _, err = join(tmp / 'bad.cht')
        check(rc != 0 and 'no desc or code' in err, f'refused: {err.strip()}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
