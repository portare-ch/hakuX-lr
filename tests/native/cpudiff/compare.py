#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare two runs of the CPU differential tests (tests/native/cpudiff/README.md).

  compare.py <test dir> <reference outputs> <outputs under test> [options]

For a test tagged "vendor" (its result depends on the CPU's maker) the
reference is the Xbox model, <test dir>/<name>.model, when there is one;
without a model the test is listed and not judged.

  --expect FILE   the differences that are known: lines "<name> <cases>".
                  Exit 1 when a test differs in more cases than listed;
                  fewer is reported as a gain.

A test without an output file, or with one of the wrong size, always fails.
  --write FILE    write the current differences in that format
  --show N        print the first N differing cases of every differing test
"""
import argparse
import os
import sys


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tests')
    ap.add_argument('ref')
    ap.add_argument('out')
    ap.add_argument('--expect')
    ap.add_argument('--write')
    ap.add_argument('--show', type=int, default=0)
    args = ap.parse_args()

    expect = {}
    if args.expect:
        for line in open(args.expect):
            p = line.split('#')[0].split()
            if len(p) == 2:
                expect[p[0]] = int(p[1])

    rows, bad, gains, unjudged, broken = [], [], [], [], []
    by_tag = {}
    for line in open(os.path.join(args.tests, 'manifest.txt')):
        p = line.split()
        if not p:
            continue
        name, tags = p[0], p[1:]
        rec = 0
        for cfg in open(os.path.join(args.tests, name + '.cfg')):
            c = cfg.split()
            if c and c[0] == 'out':
                rec = int(c[3])
        got = read(os.path.join(args.out, name + '.out'))
        want = read(os.path.join(args.tests, name + '.model')) if 'vendor' in tags else None
        if want is None:
            if 'vendor' in tags:
                unjudged.append(name)
                continue
            want = read(os.path.join(args.ref, name + '.out'))
        if want is None:
            print('no reference for %s' % name)
            bad.append(name)
            continue
        cases = len(want) // rec
        if got is None or len(got) != len(want):
            # no output, or a short one: the run failed, which no listed difference covers
            print('%s: %s' % (name, 'no output' if got is None else 'output of %d bytes, %d expected' % (len(got), len(want))))
            broken.append(name)
            continue
        else:
            first = [i for i in range(cases) if got[i * rec:(i + 1) * rec] != want[i * rec:(i + 1) * rec]]
            diff = len(first)
        rows.append((name, diff))
        key = ' '.join(tags)
        t = by_tag.setdefault(key, [0, 0, 0])
        t[0] += 1
        t[1] += diff > 0
        t[2] += diff
        if diff > expect.get(name, 0) and args.expect:
            bad.append(name)
        elif diff < expect.get(name, 0):
            gains.append((name, expect[name], diff))
        if diff and (args.show or name in bad):
            print('%s: %d of %d cases differ' % (name, diff, cases))
            for i in first[:args.show]:
                print('  case %d: reference %s, got %s' % (i, want[i * rec:(i + 1) * rec].hex(), got[i * rec:(i + 1) * rec].hex()))

    print('%-28s %6s %8s %10s' % ('tags', 'tests', 'differ', 'cases'))
    for key in sorted(by_tag):
        t = by_tag[key]
        print('%-28s %6d %8d %10d' % (key, t[0], t[1], t[2]))
    print('%d tests compared, %d differ' % (len(rows), sum(1 for _, d in rows if d)))
    if unjudged:
        print('not judged (vendor-dependent, no model): ' + ' '.join(unjudged))
    for name, was, now in gains:
        print('gain: %s differs in %d cases, %d listed' % (name, now, was))
    if args.write:
        with open(args.write, 'w') as f:
            f.write('# Known differences between FEX and the x86 reference: <test> <differing cases>.\n')
            f.write('# Written by compare.py --write; see README.md.\n')
            for name, diff in rows:
                if diff:
                    f.write('%s %d\n' % (name, diff))
    if broken:
        print('FAIL: no (or a short) output from: ' + ' '.join(broken))
    if bad:
        print('FAIL: more differences than listed in: ' + ' '.join(bad))
    return 1 if bad or broken else 0


if __name__ == '__main__':
    sys.exit(main())
