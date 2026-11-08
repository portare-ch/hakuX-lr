/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XBE patch format for RetroArch's Cheats menu:
 *
 *   xbe-patch-v1 CRC32:SIZE+OFFSET:BYTES...
 *
 * Fields use eight hex digits; each run holds 1–64 bytes. Up to 128 runs
 * are allowed, sorted and non-overlapping, starting at offset 4 to
 * preserve the XBE signature. See mods.c for application. */
#ifndef NATIVE_CORE_XBE_PATCH_H
#define NATIVE_CORE_XBE_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_XBE_PATCH_TAG "xbe-patch-v1"
#define NC_XBE_PATCH_RUNS_MAX 128
#define NC_XBE_PATCH_RUN_MAX 64
#define NC_XBE_PATCH_FRONT_MAX 256      /* the frontend's cheat indices */

typedef struct NcXbePatchRun {
    uint32_t off;
    uint8_t len;
    uint8_t bytes[NC_XBE_PATCH_RUN_MAX];
} NcXbePatchRun;

typedef struct NcXbePatch {
    uint32_t crc, size;
    unsigned nruns;
    NcXbePatchRun runs[NC_XBE_PATCH_RUNS_MAX];
} NcXbePatch;

/* Identify the XBE patch prefix, even if the rest is invalid. */
bool nc_xbe_patch_is(const char *code);
/* Parse a patch, reporting validation failures in err. */
bool nc_xbe_patch_parse(const char *code, NcXbePatch *out, char *err, size_t errlen);

/* Keep frontend patches by cheat index until reset. Invalid patches
 * remain disabled and report the error in err. */
bool nc_xbe_patch_front_set(unsigned index, bool on, const char *code, char *err, size_t errlen);
void nc_xbe_patch_front_reset(void);
/* Hash enabled patch codes independently of cheat indices (0 if none). */
uint64_t nc_xbe_patch_front_hash(void);
/* Match CRC-32 and size. Returns 1 for one match, 0 for none, or -1
 * for a conflict; fills the matched indices and patch-set hash. */
int nc_xbe_patch_front_match(uint32_t crc, uint32_t size, NcXbePatch *out, unsigned *index, unsigned *other,
                             uint64_t *hash);
/* Summarize enabled patches for logging. */
unsigned nc_xbe_patch_front_list(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
