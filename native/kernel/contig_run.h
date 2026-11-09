/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Contiguous allocation run lookup, shared by the kernel and tests. */
#ifndef NATIVE_KERNEL_CONTIG_RUN_H
#define NATIVE_KERNEL_CONTIG_RUN_H

#include <stdint.h>

typedef struct NcContigRun {
    uint32_t pa, size;  /* physical start and size in bytes */
    uint32_t slack;     /* reserved alignment pages following the block */
    uint32_t ra;        /* allocation call site */
} NcContigRun;

/* Find the run containing pa and extend through adjacent same-caller
 * blocks until reaching limit. Stop at gaps or different callers.
 * Returns 0 if pa belongs to no block. scratch holds cap candidates;
 * a smaller buffer uses a slower complete search. */
uint32_t nc_contig_run_end(const NcContigRun *b, unsigned n, uint32_t pa, uint32_t limit,
                           uint64_t *scratch, unsigned cap);

#endif
