/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for contiguous runs, ownership boundaries, and scratch limits. */
#include "native/kernel/contig_run.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static int g_fail;
static uint64_t g_scratch[4096];

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

static uint32_t run(const NcContigRun *b, unsigned n, uint32_t pa, uint32_t limit)
{
    return nc_contig_run_end(b, n, pa, limit, g_scratch, 4096);
}

int main(void)
{
    /* Adjacent blocks from the same caller, in arbitrary order. */
    const NcContigRun same[] = {
        {0x01200000, 0x100000, 0, 0xcab47}, {0x01000000, 0x100000, 0, 0xcab47},
        {0x01100000, 0x100000, 0, 0xcab47}, {0x03000000, 0x1000, 0, 0x1234},
    };
    check(run(same, 4, 0x01000000, 0x0112c000) == 0x01200000, "the same caller's next block carries a target on");
    check(run(same, 4, 0x01000000, 0x01258000) == 0x01300000, "through several blocks, to the target's end");
    check(run(same, 4, 0x01000000, 0x01080000) == 0x01100000, "a target inside its block: the block's end");
    check(run(same, 4, 0x01000000, 0) == 0x01100000, "no limit: the block's end");
    check(run(same, 4, 0x02000000, 0x02100000) == 0, "no block holds the address: 0");

    /* Adjacent allocations from different callers must remain separate. */
    const NcContigRun other[] = {{0x03c00000, 0x96000, 0, 0x100}, {0x03c96000, 0x96000, 0, 0x200}};
    check(run(other, 2, 0x03c00000, 0x03d2c000) == 0x03c96000, "another caller's adjacent block ends the run");

    /* Distinguish unallocated gaps from reserved alignment slack. */
    const NcContigRun gap[] = {{0x01000000, 0x100000, 0, 7}, {0x01101000, 0x100000, 0, 7}};
    check(run(gap, 2, 0x01000000, 0x01180000) == 0x01100000, "a page between the blocks ends the run");
    const NcContigRun slack[] = {{0x01000000, 0xff000, 0x1000, 7}, {0x01100000, 0x100000, 0, 7}};
    check(run(slack, 2, 0x01000000, 0x01180000) == 0x01200000, "a block after the last one's slack carries it on");

    /* Time 300 adjacent blocks in a reverse-ordered table of 5,000. */
    static NcContigRun many[5000];
    for (unsigned i = 0; i < 5000; i++) {
        many[i] = (NcContigRun){0x01000000 + (4999 - i) * 0x1000u, 0x1000, 0, 9};
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = true;
    for (unsigned r = 0; r < 1000; r++) {
        ok &= run(many, 5000, 0x01000000, 0x0112c000) == 0x0112c000;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("5,000 blocks, a 300-block run: %.1f us a call\n", ms);
    check(ok, "a run over 300 of 5,000 4 KiB blocks");
    check(ms < 2000, "... in under 2 ms a call");
    /* An undersized scratch buffer must not drop blocks or skip gaps. */
    static NcContigRun lots[6000];
    for (unsigned i = 0; i < 6000; i++) {
        lots[i] = (NcContigRun){0x01000000 + (5999 - i) * 0x1000u, 0x1000, 0, 9};
    }
    check(nc_contig_run_end(lots, 6000, 0x01000000, 0x01000000 + 6000 * 0x1000u, g_scratch, 4096) ==
          0x01000000 + 6000 * 0x1000u, "6,000 candidates through 4,096 entries: every block");
    check(nc_contig_run_end(lots, 20, 0x01000000 + 5980 * 0x1000u, 0x03000000, g_scratch, 8) ==
          0x01000000 + 6000 * 0x1000u, "20 through 8 entries: every block");
    lots[3000].pa = 0x01800000;             /* leave a gap */
    check(nc_contig_run_end(lots, 6000, 0x01000000, 0x01000000 + 6000 * 0x1000u, g_scratch, 4096) ==
          0x01000000 + 2999 * 0x1000u, "... and a gap still ends the run");
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
