/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NATIVE_CPU_KVM_RSQRT_H
#define NATIVE_CPU_KVM_RSQRT_H

#include <stdbool.h>
#include <stdint.h>

/* A byte per upper-21-bit input bucket, indexing the XOR palette. Built
 * from this host's estimates; never applied to another host's results. */
typedef struct NcRsqrt {
    uint8_t *index;
    uint32_t delta[256];
    unsigned ndelta;
} NcRsqrt;

typedef struct NcRsqrtInsn {
    unsigned len, dst, src;
    bool scalar, mem;
    uint32_t address;
} NcRsqrtInsn;

/* Decode one explicitly selected 32-bit guest instruction, not a scan for
 * opcodes in arbitrary bytes. regs uses x86's EAX..EDI encoding order. */
bool nc_rsqrt_decode(const uint8_t *code, unsigned size, const uint32_t regs[8],
                     uint32_t fs_base, NcRsqrtInsn *out);

bool nc_rsqrt_host_amd(void);
uint32_t nc_rsqrt_native(uint32_t bits);
uint32_t nc_rsqrt_intel(uint32_t bits);
bool nc_rsqrt_init(NcRsqrt *r);
void nc_rsqrt_destroy(NcRsqrt *r);
uint32_t nc_rsqrt_correct(const NcRsqrt *r, uint32_t bits);

#endif
