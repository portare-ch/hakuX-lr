/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No KVM device required. --exhaustive compares every binary32 bit pattern
 * of the Intel reference with this host's real RSQRT instruction: equal on
 * a host that isn't AMD. */
#include "native/cpu/kvm_rsqrt.h"

#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

static void check(bool ok, const char *name)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

static void decoder(void)
{
    const uint32_t regs[8] = {0x100, 0x20, 0x30, 0xFFE0, 0x400, 0x500, 0x40, 0x50};
    NcRsqrtInsn in;
    const uint8_t scalar[] = {0xF3, 0x0F, 0x52, 0xD9}; /* xmm3, xmm1 */
    check(nc_rsqrt_decode(scalar, sizeof(scalar), regs, 0, &in) &&
          in.scalar && !in.mem && in.len == 4 && in.dst == 3 && in.src == 1,
          "RSQRTSS register operands");
    const uint8_t packed[] = {0x0F, 0x52, 0xE7}; /* xmm4, xmm7 */
    check(nc_rsqrt_decode(packed, sizeof(packed), regs, 0, &in) &&
          !in.scalar && !in.mem && in.len == 3 && in.dst == 4 && in.src == 7,
          "RSQRTPS register operands");
    const uint8_t sib[] = {0x64, 0xF3, 0x0F, 0x52, 0x5C, 0x88, 0xF0};
    check(nc_rsqrt_decode(sib, sizeof(sib), regs, 0x7000, &in) && in.mem &&
          in.len == sizeof(sib) && in.address == 0x7170,
          "FS:[eax + ecx*4 - 16]");
    const uint8_t absolute[] = {0x0F, 0x52, 0x05, 0x40, 0x30, 0x20, 0x10};
    check(nc_rsqrt_decode(absolute, sizeof(absolute), regs, 0, &in) &&
          in.mem && in.address == 0x10203040u, "absolute memory operand");
    const uint8_t addr16[] = {0x67, 0xF3, 0x0F, 0x52, 0x00};
    check(nc_rsqrt_decode(addr16, sizeof(addr16), regs, 0, &in) &&
          in.address == 0x20, "16-bit BX+SI address wraps before segment base");
    const uint8_t abs16[] = {0x64, 0x67, 0x0F, 0x52, 0x06, 0xCD, 0xAB};
    check(nc_rsqrt_decode(abs16, sizeof(abs16), regs, 0x10000, &in) &&
          in.address == 0x1ABCD, "16-bit absolute address with FS");
    bool truncated = true;
    for (unsigned i = 0; i < sizeof(sib); i++) {
        truncated &= !nc_rsqrt_decode(sib, i, regs, 0, &in);
    }
    check(truncated, "truncated prefixes, opcode, ModRM, SIB and displacement rejected");
    const uint8_t wrong[][4] = {{0xF2, 0x0F, 0x52, 0xC0}, {0x66, 0x0F, 0x52, 0xC0},
                                {0xF0, 0x0F, 0x52, 0xC0}, {0xF3, 0x0F, 0x51, 0xC0}};
    bool rejected = true;
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
        rejected &= !nc_rsqrt_decode(wrong[i], sizeof(wrong[i]), regs, 0, &in);
    }
    check(rejected, "other opcodes and unsupported mandatory prefixes rejected");
    const uint8_t rcpss[] = {0xF3, 0x0F, 0x53, 0xD9}, rcpps[] = {0x0F, 0x53, 0x05, 0x40, 0x30, 0x20, 0x10};
    check(nc_rsqrt_decode(rcpss, sizeof(rcpss), regs, 0, &in) && in.rcp && in.scalar && !in.mem &&
          in.len == 4 && in.dst == 3 && in.src == 1 &&
          nc_rsqrt_decode(rcpps, sizeof(rcpps), regs, 0, &in) && in.rcp && !in.scalar && in.mem &&
          in.address == 0x10203040u && nc_rsqrt_decode(scalar, sizeof(scalar), regs, 0, &in) && !in.rcp,
          "RCPSS and RCPPS operands, told from RSQRT");
    const uint8_t too_long[] = {0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64,
                                0x64, 0x64, 0x64, 0x64, 0x64, 0xF3, 0x0F, 0x52, 0xC0};
    check(!nc_rsqrt_decode(too_long, sizeof(too_long), regs, 0, &in), "15-byte instruction limit");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    decoder();
    static const struct { uint32_t input, expected; } cases[] = {
        {0x3F800000, 0x3F7FF000}, {0x40800000, 0x3EFFF000},
        {0x3E800000, 0x3FFFF000}, {0x41800000, 0x3E7FF000},
        {0x43800000, 0x3D7FF000}, /* Intel Skylake's published rsqrt(256) */
        {0, 0x7F800000}, {0x80000000, 0xFF800000},
        {1, 0x7F800000}, {0x807FFFFF, 0xFF800000},
        {0x7F800000, 0}, {0xFF800000, 0xFFC00000},
        {0xBF800000, 0xFFC00000}, {0x7F800001, 0x7FC00001},
        {0xFF800123, 0xFFC00123}, {0x7FC12345, 0x7FC12345},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint32_t got = nc_rsqrt_intel(cases[i].input);
        if (got != cases[i].expected) {
            printf("FAIL: input %08x expected %08x got %08x\n", cases[i].input, cases[i].expected, got);
            failures++;
        }
    }
    check(!failures, "Xbox's four failing inputs, published Intel result and special values");
    static const struct { uint32_t input, expected; } rcp[] = {
        {0x3F800000, 0x3F7FF000}, {0x40000000, 0x3EFFF000},
        {0x3F000000, 0x3FFFF000}, {0xC0800000, 0xBE7FF000},
        {0x00800000, 0x7E7FF000}, {0x7E800000, 0}, {0xFF000000, 0x80000000},
        {0, 0x7F800000}, {0x80000000, 0xFF800000},
        {1, 0x7F800000}, {0x807FFFFF, 0xFF800000},
        {0x7F800000, 0}, {0xFF800000, 0x80000000},
        {0x7F800001, 0x7FC00001}, {0xFF800123, 0xFFC00123}, {0x7FC12345, 0x7FC12345},
    };
    unsigned rcp_failures = 0;
    for (unsigned i = 0; i < sizeof(rcp) / sizeof(rcp[0]); i++) {
        const uint32_t got = nc_rcp_intel(rcp[i].input);
        if (got != rcp[i].expected) {
            printf("FAIL: RCP input %08x expected %08x got %08x\n", rcp[i].input, rcp[i].expected, got);
            rcp_failures++;
        }
    }
    check(!rcp_failures, "RCP: powers of two, the exponent range's ends and special values");
    /* The reference against a CPU that is one: every input on a host that
     * isn't AMD. An AMD host's own estimates differ, which is what the
     * correction is for: how many, for the record. */
    const bool exhaustive = argc == 2 && !strcmp(argv[1], "--exhaustive");
    const uint64_t limit = exhaustive ? 1ull << 32 : 1ull << 21;
    uint64_t differ = 0, rcp_differ = 0;
    uint32_t random = 1, first = 0;
    for (uint64_t i = 0; i < limit; i++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const uint32_t bits = exhaustive ? (uint32_t)i : random;
        if (nc_rsqrt_native(bits) != nc_rsqrt_intel(bits) && !differ++) {
            first = bits;
        }
        rcp_differ += nc_rcp_native(bits) != nc_rcp_intel(bits);
        if (exhaustive && (i & ((1u << 28) - 1)) == (1u << 28) - 1) {
            printf("Checked %llu / 4294967296 bit patterns\n", (unsigned long long)i + 1);
        }
    }
    if (nc_rsqrt_host_amd()) {
        printf("Host: AMD, its RSQRT differs from the Intel reference on %llu of %llu inputs\n",
               (unsigned long long)differ, (unsigned long long)limit);
    } else {
        if (differ) {
            printf("FAIL: input %08x: this CPU %08x, the reference %08x\n", first, nc_rsqrt_native(first),
                   nc_rsqrt_intel(first));
        }
        check(!differ, exhaustive ? "all 2^32 input bit patterns: the reference is this CPU's RSQRT" :
                                    "2^21 pseudorandom inputs: the reference is this CPU's RSQRT");
    }
    /* RCP's table is an Intel reference no Xbox has confirmed yet: counted, not checked */
    printf("Host: its RCP differs from the Intel reference on %llu of %llu inputs\n",
           (unsigned long long)rcp_differ, (unsigned long long)limit);
    return failures ? 1 : 0;
}
