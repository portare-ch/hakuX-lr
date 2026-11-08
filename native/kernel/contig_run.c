/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/kernel/contig_run.h"

#include <stdbool.h>
#include <stdlib.h>

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint32_t nc_contig_run_end(const NcContigRun *b, unsigned n, uint32_t pa, uint32_t limit,
                           uint64_t *scratch, unsigned cap)
{
    unsigned home = n;
    for (unsigned i = 0; i < n && home == n; i++) {
        if (pa - b[i].pa < b[i].size) {
            home = i;
        }
    }
    if (home == n) {
        return 0;
    }
    uint32_t end = b[home].pa + b[home].size, next = end + b[home].slack;
    if (limit <= end) {
        return end;
    }
    /* Collect same-caller candidates up to the requested end. */
    unsigned k = 0;
    bool more = false;
    for (unsigned i = 0; i < n; i++) {
        if (i != home && b[i].ra == b[home].ra && b[i].pa >= end && b[i].pa < limit) {
            if (k == cap) {
                more = true;
                break;
            }
            scratch[k++] = (uint64_t)b[i].pa << 32 | i;
        }
    }
    if (more) {
        /* Avoid dropping a nearby block when scratch is too small:
         * find each next block directly. */
        for (bool found = true; found && end < limit;) {
            found = false;
            for (unsigned i = 0; i < n; i++) {
                if (b[i].ra == b[home].ra && (b[i].pa == end || b[i].pa == next) && b[i].size) {
                    end = b[i].pa + b[i].size;
                    next = end + b[i].slack;
                    found = true;
                    break;
                }
            }
        }
        return end;
    }
    qsort(scratch, k, sizeof(*scratch), cmp_u64);
    for (unsigned j = 0; j < k && end < limit; j++) {
        const NcContigRun *c = &b[(uint32_t)scratch[j]];
        if (c->pa != end && c->pa != next) {
            break;                          /* a gap */
        }
        end = c->pa + c->size;
        next = end + c->slack;
    }
    return end;
}
