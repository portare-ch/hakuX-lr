/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NATIVE_CPU_KVM_RSQRT_SCAN_H
#define NATIVE_CPU_KVM_RSQRT_SCAN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The RSQRTSS/RSQRTPS instructions of a title's image, for the AMD-only
 * correction (kvm_rsqrt.h): found by walking the code as control flow
 * reaches it, never by matching bytes, since an INT3 over bytes that only
 * look like the instruction (an operand, a constant) corrupts the title.
 */
typedef struct NcRsqrtScanSection {
    uint32_t va, size;    /* in the image */
    const uint8_t *data;  /* its bytes: data_size of them, size at least */
    uint32_t data_size;
    bool code;            /* executable: walked */
    bool seeds;           /* data whose dwords may be the addresses of code */
} NcRsqrtScanSection;

typedef struct NcRsqrtScan {
    uint32_t *sites;      /* instruction addresses, ascending (malloc'd) */
    unsigned nsites;
    unsigned disputed;    /* reached, but the linear sweep decodes otherwise: left out */
    unsigned unreached;   /* in the linear sweep only: unreferenced code, or not code */
    /* how the walk went */
    unsigned regions;     /* runs of code kept, each from one root */
    unsigned seeds;       /* code addresses tried as roots */
    unsigned seeds_not_code_start;
    unsigned seeds_accepted;
    unsigned evicted;     /* families of regions given up for more trusted code */
    unsigned evicted_straddled;
    unsigned tables_cut;
    unsigned requeued;
} NcRsqrtScan;

/* roots: the entry point and the TLS callbacks. false: no memory. */
bool nc_rsqrt_scan(const NcRsqrtScanSection *secs, unsigned nsecs, const uint32_t *roots, unsigned nroots,
                   NcRsqrtScan *out);
/* An XBE file's. false: not an XBE, or no memory. */
bool nc_rsqrt_scan_xbe(const uint8_t *xbe, uint32_t size, NcRsqrtScan *out);
void nc_rsqrt_scan_free(NcRsqrtScan *scan);

#endif
