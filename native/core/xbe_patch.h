/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XBE patch format for RetroArch's Cheats menu:
 *
 *   xbe-patch-v1 CRC32:SIZE+OFFSET:BYTES... [screen=widescreen]
 *
 * Fields use eight hex digits; each run holds 1–64 bytes. Up to 128 runs
 * are allowed, sorted and non-overlapping, starting at offset 4 to
 * preserve the XBE signature. screen=widescreen after the runs marks a
 * patch that makes the game draw for a 16:9 screen (Screen Format's Auto
 * sets the console to widescreen while one is applied). See mods.c for
 * application. */
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
    bool widescreen;                    /* screen=widescreen */
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
/* The enabled patches for an XBE (its CRC-32 and size), combined into one:
 * their bytes together, in runs of the format's limits, widescreen if any
 * of them is. Patches combine when no two write different bytes to the same
 * offset; the same byte from two of them is written once.
 * Returns how many were combined (index: the first's cheat index), 0 for
 * none, NC_XBE_PATCH_CONFLICT when two write different bytes (index and
 * other: theirs; at: the offset), or NC_XBE_PATCH_TOO_BIG when together
 * they need more runs than a patch holds. Fills the patch-set hash. */
#define NC_XBE_PATCH_CONFLICT (-1)
#define NC_XBE_PATCH_TOO_BIG  (-2)
int nc_xbe_patch_front_match(uint32_t crc, uint32_t size, NcXbePatch *out, unsigned *index, unsigned *other,
                             uint32_t *at, uint64_t *hash);
/* Summarize enabled patches for logging. */
unsigned nc_xbe_patch_front_list(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
