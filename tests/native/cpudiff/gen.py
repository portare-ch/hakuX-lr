#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the CPU differential tests (tests/native/cpudiff/README.md).

Every test is one 32-bit program: a loop that runs one instruction form over
a table of inputs and stores each result in the output region, then HLT.
The same <name>.bin runs on an x86 CPU (cpudiff-ref, under KVM) and under
FEX (fexrun --dump); the output regions are compared byte for byte.

Writes <out>/<name>.bin, <name>.cfg, manifest.txt and, for the forms whose
result depends on the CPU's vendor, <name>.model: the Xbox's expected output
(RSQRT from Intel's table, native/cpu/intel_rsqrt_table.inc).

Usage: gen.py --out build/cpudiff [--count 2048] [--seed 1]
Needs GNU as, ld and objcopy for i386 (binutils on an x86 host).
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys

CODE = 0x10000
OUT = 0x00400000      # output region
STACK = 0x00300000    # one page, ESP starts at its end
HERE = os.path.dirname(os.path.abspath(__file__))


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f64(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


SPECIAL32 = [0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x00800001,
             0x3F800000, 0xBF800000, 0x3F7FFFFF, 0x3F800001, 0x40000000, 0x40800000, 0x3F000000,
             0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001,
             0xFFA00000, 0x7FFFFFFF, 0x4EFFFFFF, 0x4F000000, 0x4F000001, 0xCF000000, 0xCF000001,
             0x5F000000, 0xDF000000, 0xDF000001, 0x46FFFE00, 0x47000000, 0xC7000000, 0xC7000100,
             0x3EFFFFFF, 0x3F000001, 0x3FC00000, 0x40200000, 0xBFC00000, 0xC0200000]

SPECIAL64 = [0, 1 << 63, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, f64(1.0), f64(-1.0), f64(0.5),
             f64(1.5), f64(2.5), f64(-1.5), f64(-2.5), f64(2147483647.0), f64(2147483648.0),
             f64(-2147483648.0), f64(-2147483649.0), f64(2147483647.5), f64(32767.0), f64(32768.0),
             f64(-32768.0), f64(-32769.0), f64(9223372036854775807.0), f64(-9223372036854775808.0),
             f64(1e300), f64(-1e300), 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000,
             0xFFF8000000000000, 0x7FF0000000000001, 0x7FEFFFFFFFFFFFFF, f64(0.1), f64(1.0 / 3.0)]

SPECIALI64 = [0, 1, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x8000000000000001,
              (1 << 53) - 1, 1 << 53, (1 << 53) + 1, (1 << 53) + 3, (1 << 54) + 1, (1 << 62) + 1,
              0x0123456789ABCDEF, 0xFEDCBA9876543210, (1 << 63) - 1025, 0x00000000FFFFFFFF,
              0x0000000100000000, 0xFFFFFFFF00000001]


def floats32(rng, n):
    """Special values, uniform bit patterns and values of ordinary size."""
    v = list(SPECIAL32)
    while len(v) < n:
        k = len(v) % 3
        if k == 0:
            v.append(rng.getrandbits(32))
        elif k == 1:
            v.append(f32(rng.uniform(-1000.0, 1000.0)))
        else:
            v.append(f32(rng.uniform(0.0, 4.0) * 2.0 ** rng.randint(-40, 40)))
    return v[:n]


def floats64(rng, n):
    v = list(SPECIAL64)
    while len(v) < n:
        k = len(v) % 3
        if k == 0:
            v.append(rng.getrandbits(64))
        elif k == 1:
            v.append(f64(rng.uniform(-100000.0, 100000.0)))
        else:
            v.append(f64(rng.uniform(-4.0, 4.0) * 2.0 ** rng.randint(-70, 70)))
    return v[:n]


def ints64(rng, n):
    v = list(SPECIALI64)
    while len(v) < n:
        v.append(rng.getrandbits(64) >> rng.choice([0, 0, 0, 1, 8, 11, 20, 40]))
    return v[:n]


def angles64(rng, n):
    v = [f64(x) for x in (0.0, 0.5, 1.0, -1.0, 3.141592653589793, 1.5707963267948966, 0.1, 1e-10, 100.0)]
    while len(v) < n:
        v.append(f64(rng.uniform(-8.0, 8.0)))
    return v[:n]


class Test:
    """in_fmt / out_bytes: per case, the input record (struct format) and the
    bytes the body stores at [edi]. body: asm with ESI at the input record
    and EDI at the output record; EAX, EDX, x87 and XMM are free, the x87
    stack is empty on entry and must be on exit. setup: asm run once."""

    def __init__(self, name, inputs, in_fmt, out_bytes, body, setup='', tags='', env=(), single=False):
        self.name, self.inputs, self.in_fmt, self.out_bytes = name, inputs, in_fmt, out_bytes
        self.body, self.setup, self.tags, self.env, self.single = body, setup, tags, env, single


def cw(pc, rc=0):
    """An x87 control word: all exceptions masked, precision and rounding as given."""
    return 0x003F | (pc << 8) | (rc << 10)


PCS = [('s', 0), ('d', 2), ('e', 3)]
RCS = ['n', 'd', 'u', 'z']
REDUCED = (('FEX_X87REDUCEDPRECISION', '1'), ('FEX_X87REDUCEDPRECISIONCONTROL', '1'))


def build(rng, n):
    t = []
    a32 = floats32(rng, n)
    b32 = floats32(random.Random(rng.random()), n)
    b32.reverse()
    a64 = floats64(rng, n)
    b64 = floats64(random.Random(rng.random()), n)
    b64.reverse()
    i64 = ints64(rng, n)
    pair32 = list(zip(a32, b32))
    pair64 = list(zip(a64, b64))
    quad32 = [tuple(a32[(i + k * 7) % n] for k in range(4)) for i in range(n)]

    # 1. Reciprocal estimates: the result is the CPU's table (vendor).
    t.append(Test('rsqrtss', [(x,) for x in a32], '<I', 4,
                  'movss xmm0, [esi]\nrsqrtss xmm1, xmm0\nmovss [edi], xmm1', tags='vendor rsqrt'))
    t.append(Test('rsqrtss_mem', [(x,) for x in a32], '<I', 4,
                  'xorps xmm1, xmm1\nrsqrtss xmm1, [esi]\nmovss [edi], xmm1', tags='vendor rsqrt'))
    t.append(Test('rsqrtps', quad32, '<4I', 16,
                  'movups xmm0, [esi]\nrsqrtps xmm1, xmm0\nmovups [edi], xmm1', tags='vendor rsqrt'))
    t.append(Test('rsqrtss_upper', pair32, '<2I', 16,
                  'movss xmm0, [esi]\nmovss xmm1, [esi+4]\nshufps xmm1, xmm1, 0x15\nrsqrtss xmm1, xmm0\nmovups [edi], xmm1',
                  tags='vendor rsqrt'))
    t.append(Test('rcpss', [(x,) for x in a32], '<I', 4,
                  'movss xmm0, [esi]\nrcpss xmm1, xmm0\nmovss [edi], xmm1', tags='vendor rcp'))
    t.append(Test('rcpps', quad32, '<4I', 16,
                  'movups xmm0, [esi]\nrcpps xmm1, xmm0\nmovups [edi], xmm1', tags='vendor rcp'))

    # None of these touch the arithmetic flags: a compare before, the flags read after.
    FLAGS = 'pushfd\npop eax\nand eax, 0x8D5\nmov [edi], eax'
    t.append(Test('rsqrtss_flags', pair32, '<2I', 4,
                  'movss xmm0, [esi]\nmov eax, [esi]\ncmp eax, [esi+4]\nrsqrtss xmm1, xmm0\n' + FLAGS, tags='flags'))
    t.append(Test('rsqrtps_flags', pair32, '<2I', 4,
                  'movss xmm0, [esi]\nshufps xmm0, xmm0, 0\nmov eax, [esi]\ncmp eax, [esi+4]\nrsqrtps xmm1, xmm0\n' + FLAGS,
                  tags='flags'))
    for width, ptr in ((2, 'word'), (4, 'dword'), (8, 'qword')):
        t.append(Test('fistp%d_flags' % (width * 8), pair64, '<2Q', 4 + width,
                      'fld qword ptr [esi]\nmov eax, [esi+4]\ncmp eax, [esi+12]\nfistp %s ptr [edi+4]\n' % ptr + FLAGS,
                      env=REDUCED, tags='flags'))
    t.append(Test('fild64_fistp64_flags', [(x, y) for x, y in zip(i64, reversed(i64))], '<2Q', 12,
                  'mov eax, [esi]\ncmp eax, [esi+8]\nfild qword ptr [esi]\nfistp qword ptr [edi+4]\n' + FLAGS,
                  env=REDUCED, tags='flags'))

    # 2. 64-bit integers through the x87: exact at every precision setting.
    for pn, pc in PCS:
        t.append(Test('fild64_fistp64_' + pn, [(x,) for x in i64], '<Q', 8,
                      'fild qword ptr [esi]\nfistp qword ptr [edi]',
                      setup='fldcw [cw]\n.data\ncw: .word %d\n.text' % cw(pc), env=REDUCED, single=pc == 0,
                      tags='x87 int64' + (' extended' if pc == 3 else '')))
    t.append(Test('fild64_fstp80', [(x,) for x in i64], '<Q', 10,
                  'fild qword ptr [esi]\nfstp tbyte ptr [edi]',
                  setup='fldcw [cw]\n.data\ncw: .word %d\n.text' % cw(2), env=REDUCED, tags='x87 int64'))

    # 3. Float to integer: out of range and NaN store the integer indefinite.
    for rn, rc in zip(RCS, range(4)):
        s = 'fldcw [cw]\n.data\ncw: .word %d\n.text' % cw(2, rc)
        t.append(Test('fist32_f64_' + rn, [(x,) for x in a64], '<Q', 4,
                      'fld qword ptr [esi]\nfistp dword ptr [edi]', setup=s, env=REDUCED, tags='x87 f2i'))
        t.append(Test('fist16_f64_' + rn, [(x,) for x in a64], '<Q', 2,
                      'fld qword ptr [esi]\nfistp word ptr [edi]', setup=s, env=REDUCED, tags='x87 f2i'))
        t.append(Test('fist64_f64_' + rn, [(x,) for x in a64], '<Q', 8,
                      'fld qword ptr [esi]\nfistp qword ptr [edi]', setup=s, env=REDUCED, tags='x87 f2i'))
        t.append(Test('fist32_f32_' + rn, [(x,) for x in a32], '<I', 4,
                      'fld dword ptr [esi]\nfistp dword ptr [edi]', setup=s, env=REDUCED, tags='x87 f2i'))
        m = 'ldmxcsr [mx]\n.data\nmx: .long %d\n.text' % (0x1F80 | (rc << 13))
        t.append(Test('cvtss2si_' + rn, [(x,) for x in a32], '<I', 4,
                      'cvtss2si eax, dword ptr [esi]\nmov [edi], eax', setup=m, tags='sse f2i'))
        t.append(Test('cvtps2pi_' + rn, pair32, '<2I', 8,
                      'movlps xmm0, [esi]\ncvtps2pi mm0, xmm0\nmovq [edi], mm0\nemms', setup=m, tags='sse f2i'))
    t.append(Test('cvttss2si', [(x,) for x in a32], '<I', 4,
                  'cvttss2si eax, dword ptr [esi]\nmov [edi], eax', tags='sse f2i'))
    t.append(Test('cvttps2pi', pair32, '<2I', 8,
                  'movlps xmm0, [esi]\ncvttps2pi mm0, xmm0\nmovq [edi], mm0\nemms', tags='sse f2i'))

    # x87 arithmetic at each precision setting, stored as float and as double.
    for pn, pc in PCS:
        s = 'fldcw [cw]\n.data\ncw: .word %d\n.text' % cw(pc)
        tag = 'x87 arith' + (' extended' if pc == 3 else '')
        for op in ('fadd', 'fsub', 'fmul', 'fdiv'):
            t.append(Test('%s_f32_%s' % (op, pn), pair32, '<2I', 12,
                          'fld dword ptr [esi]\n%s dword ptr [esi+4]\nfst dword ptr [edi]\nfstp qword ptr [edi+4]' % op,
                          setup=s, env=REDUCED, single=pc == 0, tags=tag))
            t.append(Test('%s_f64_%s' % (op, pn), pair64, '<2Q', 12,
                          'fld qword ptr [esi]\n%s qword ptr [esi+8]\nfst dword ptr [edi]\nfstp qword ptr [edi+4]' % op,
                          setup=s, env=REDUCED, single=pc == 0, tags=tag))
        t.append(Test('fsqrt_f64_' + pn, [(x,) for x in a64], '<Q', 12,
                      'fld qword ptr [esi]\nfsqrt\nfst dword ptr [edi]\nfstp qword ptr [edi+4]',
                      setup=s, env=REDUCED, single=pc == 0, tags=tag))
        # a product kept on the stack and used again: where single precision rounds matters
        t.append(Test('fmul_fadd_f32_' + pn, pair32, '<2I', 12,
                      'fld dword ptr [esi]\nfmul dword ptr [esi+4]\nfadd dword ptr [esi]\nfmul st, st(0)\n'
                      'fst dword ptr [edi]\nfstp qword ptr [edi+4]',
                      setup=s, env=REDUCED, single=pc == 0, tags=tag))
    # x87 compares: the status word's condition codes
    t.append(Test('fcom_f64', pair64, '<2Q', 2,
                  'fld qword ptr [esi]\nfcomp qword ptr [esi+8]\nfnstsw ax\nand ax, 0x4500\nmov [edi], ax',
                  env=REDUCED, tags='x87'))

    # The transcendental functions: double results under the reduced x87 (known).
    for op, pre in (('fsin', ''), ('fcos', ''), ('fptan', 'fstp st(0)\n'), ('f2xm1', ''), ('fpatan', None),
                    ('fyl2x', None)):
        if pre is None:
            body = 'fld qword ptr [esi]\nfld qword ptr [esi+8]\nfabs\n%s\nfstp qword ptr [edi]' % op
            ins, fmt = [(a, b) for a, b in zip(angles64(rng, n), angles64(random.Random(7), n)[::-1])], '<2Q'
        else:
            scale = 'fmul qword ptr [eighth]\n' if op == 'f2xm1' else ''
            body = 'fld qword ptr [esi]\n%s%s\n%sfstp qword ptr [edi]' % (scale, op, pre)
            ins, fmt = [(x,) for x in angles64(rng, n)], '<Q'
        t.append(Test(op, ins, fmt, 8, body,
                      setup='fldcw [cw]\n.data\ncw: .word %d\neighth: .double 0.125\n.text' % cw(2),
                      env=REDUCED, tags='x87 transcendental'))

    # SSE scalar and packed arithmetic at each rounding mode, flush-to-zero included.
    for rn, rc in zip(RCS, range(4)):
        for fz in (0, 1):
            m = 'ldmxcsr [mx]\n.data\nmx: .long %d\n.text' % (0x1F80 | (rc << 13) | (fz << 15))
            sfx = rn + ('_fz' if fz else '')
            for op in ('addss', 'subss', 'mulss', 'divss', 'minss', 'maxss'):
                if fz and op in ('minss', 'maxss'):
                    continue
                t.append(Test('%s_%s' % (op, sfx), pair32, '<2I', 4,
                              'movss xmm0, [esi]\n%s xmm0, dword ptr [esi+4]\nmovss [edi], xmm0' % op,
                              setup=m, tags='sse arith'))
            t.append(Test('sqrtss_' + sfx, [(x,) for x in a32], '<I', 4,
                          'sqrtss xmm0, dword ptr [esi]\nmovss [edi], xmm0', setup=m, tags='sse arith'))
    for op in ('addps', 'mulps', 'divps', 'minps', 'maxps', 'sqrtps'):
        t.append(Test(op, [q + quad32[(i + 5) % n] for i, q in enumerate(quad32)], '<8I', 16,
                      'movups xmm0, [esi]\nmovups xmm1, [esi+16]\n%s xmm0, xmm1\nmovups [edi], xmm0' % op,
                      tags='sse arith'))
    for imm in range(8):
        t.append(Test('cmpss_%d' % imm, pair32, '<2I', 4,
                      'movss xmm0, [esi]\ncmpss xmm0, dword ptr [esi+4], %d\nmovss [edi], xmm0' % imm, tags='sse'))
    for op in ('comiss', 'ucomiss'):
        t.append(Test(op, pair32, '<2I', 4,
                      'movss xmm0, [esi]\n%s xmm0, dword ptr [esi+4]\npushfd\npop eax\nand eax, 0x8D5\nmov [edi], eax' % op,
                      tags='sse'))
    t.append(Test('cvtsi2ss', [(x & 0xFFFFFFFF,) for x in i64], '<I', 4,
                  'cvtsi2ss xmm0, dword ptr [esi]\nmovss [edi], xmm0', tags='sse'))

    # 4. Self-modifying code: the immediate of an instruction that has already
    # run is rewritten, with no call to anything; the new value must be used.
    t.append(Test('smc_imm', [(x & 0xFFFFFFFF,) for x in i64], '<I', 4,
                  'mov eax, [esi]\nmov dword ptr [patch+1], eax\ncall patch\nmov [edi], eax',
                  setup='jmp 9f\npatch: mov eax, 0x11111111\nret\n9:', tags='smc'))
    t.append(Test('smc_far', [(x & 0xFFFFFFFF,) for x in i64], '<I', 4,
                  'mov eax, [esi]\nmov dword ptr [0x00500001], eax\nmov eax, 0x00500000\ncall eax\nmov [edi], eax',
                  setup='mov byte ptr [0x00500000], 0xB8\nmov byte ptr [0x00500005], 0xC3', tags='smc'))
    return t


ASM = """.intel_syntax noprefix
.code32
.text
.globl _start
_start:
  mov esp, {stack_top}
  fninit
{setup}
  mov esi, offset inputs
  mov edi, {out}
  mov ecx, {count}
1:
{body}
  add esi, {in_size}
  add edi, {out_size}
  dec ecx
  jnz 1b
  hlt
.data
.balign 16
inputs:
"""


def rsqrt_table():
    text = open(os.path.join(HERE, '..', '..', '..', 'native', 'cpu', 'intel_rsqrt_table.inc')).read()
    text = text[text.index('{') + 1:text.index('}')]
    table = [int(x) for x in re.findall(r'\d+', text)]
    assert len(table) == 2048
    return table


def rsqrt_intel(bits, table):
    """nc_rsqrt_intel (native/cpu/kvm_rsqrt.c)."""
    mag = bits & 0x7FFFFFFF
    if mag > 0x7F800000:
        return bits | 0x00400000
    if mag < 0x00800000:
        return (bits & 0x80000000) | 0x7F800000
    if bits & 0x80000000:
        return 0xFFC00000
    if mag == 0x7F800000:
        return 0
    return (((380 - (bits >> 23)) >> 1) << 23) | (table[(bits >> 13) & 2047] << 11)


def model(test, table):
    if 'rsqrt' not in test.tags.split():
        return None
    out = b''
    for rec in test.inputs:
        if test.name == 'rsqrtps':
            out += struct.pack('<4I', *(rsqrt_intel(x, table) for x in rec))
        elif test.name == 'rsqrtss_upper':
            out += struct.pack('<4I', rsqrt_intel(rec[0], table), 0, 0, rec[1])
        else:
            out += struct.pack('<I', rsqrt_intel(rec[0], table))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--count', type=int, default=2048)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--as', dest='gas', default='as')
    ap.add_argument('--ld', default='ld')
    ap.add_argument('--only', help='only the tests whose name starts with this (for timing a form at a large --count)')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    tests = build(random.Random(args.seed), args.count)
    if args.only:
        tests = [t for t in tests if t.name.startswith(args.only)]
    table = rsqrt_table()
    manifest = []
    for t in tests:
        in_size = struct.calcsize(t.in_fmt)
        src = ASM.format(stack_top=hex(STACK + 0x1000), setup=t.setup, out=hex(OUT), count=len(t.inputs),
                         body=t.body, in_size=in_size, out_size=t.out_bytes)
        base = os.path.join(args.out, t.name)
        with open(base + '.s', 'w') as f:
            f.write(src)
            for rec in t.inputs:
                f.write('.byte ' + ','.join(str(b) for b in struct.pack(t.in_fmt, *rec)) + '\n')
        subprocess.check_call([args.gas, '--32', '-o', base + '.o', base + '.s'])
        # code and data in one writable, executable image at CODE (the tests patch their own code)
        subprocess.check_call([args.ld, '-m', 'elf_i386', '-N', '-Ttext', hex(CODE), '--oformat', 'binary',
                               '-e', '_start', '-o', base + '.bin', base + '.o'])
        os.remove(base + '.o')
        os.remove(base + '.s')
        out_size = t.out_bytes * len(t.inputs)
        with open(base + '.cfg', 'w') as f:
            f.write('mode 32\nfeat 0\n')
            f.write('region %#x %#x\n' % (STACK, 0x1000))
            f.write('region %#x %#x\n' % (OUT, (out_size + 0xFFF) & ~0xFFF))
            f.write('region %#x %#x\n' % (0x00500000, 0x1000))
            f.write('out %#x %#x %d\n' % (OUT, out_size, t.out_bytes))
            for k, v in t.env:
                f.write('env %s %s\n' % (k, v))
            if t.single:
                f.write('x87single\n')
        m = model(t, table)
        if m is not None:
            with open(base + '.model', 'wb') as f:
                f.write(m)
        manifest.append('%s %s' % (t.name, t.tags))
    with open(os.path.join(args.out, 'manifest.txt'), 'w') as f:
        f.write('\n'.join(manifest) + '\n')
    print('%d tests, %d cases each, in %s' % (len(tests), args.count, args.out))


if __name__ == '__main__':
    sys.exit(main())
