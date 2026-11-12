/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kvm-rsqrt-lane-test [--exhaustive]: the in-guest lane routine's bytes
 * (kvm_rsqrt_guest.h), run natively, against nc_rsqrt_intel. Its bytes mean
 * the same in 64-bit mode, the table below 4 GiB (MAP_32BIT) so EDX holds
 * its address. 2^24 spread inputs, or all 2^32 with --exhaustive.
 */
#define _GNU_SOURCE
#include "native/cpu/kvm_rsqrt.h"
#include "native/cpu/kvm_rsqrt_guest.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static uint32_t lane(const void *fn, uint32_t in, uint64_t table)
{
    uint64_t a = in;
    /* past the red zone: the call pushes its return address */
    __asm__ volatile("sub $128, %%rsp\n\tcall *%[fn]\n\tadd $128, %%rsp"
                     : "+a"(a) : "d"(table), [fn] "r"(fn) : "rcx", "memory", "cc");
    return (uint32_t)a;
}

int main(int argc, char **argv)
{
    const bool all = argc > 1 && !strcmp(argv[1], "--exhaustive");
    uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    uint8_t *table = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (code == MAP_FAILED || table == MAP_FAILED) {
        printf("FAIL: memory below 4 GiB\n");
        return 1;
    }
    memcpy(code, rsqrt_guest_lane, sizeof(rsqrt_guest_lane));
    memcpy(table, nc_rsqrt_intel_mantissa(), 2048 * sizeof(uint16_t));
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC)) {
        printf("FAIL: an executable page\n");
        return 1;
    }
    uint64_t bad = 0, n = 0;
    const uint32_t step = all ? 1u : 255u;      /* odd: every low bit pattern visited */
    uint32_t i = 0;
    do {
        const uint32_t got = lane(code, i, (uint64_t)(uintptr_t)table), want = nc_rsqrt_intel(i);
        if (got != want && bad++ < 5) {
            printf("  input %08x: guest routine %08x, reference %08x\n", i, got, want);
        }
        n++;
        i += step;
    } while (all ? i != 0 : n < (1u << 24));
    printf("%s: the guest lane routine on %llu inputs, %llu differ\n", bad ? "FAIL" : "PASS",
           (unsigned long long)n, (unsigned long long)bad);
    return bad != 0;
}
