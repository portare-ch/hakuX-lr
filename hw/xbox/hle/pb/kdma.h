/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — Kelvin's context DMA objects. Surface, texture, palette
 * and vertex offsets count from the base of the object their context names.
 * D3D's objects start at 0; nxdk's pbkit points its colour and zeta objects
 * at its buffers and draws at offset 0.
 */
#ifndef HW_XBOX_HLE_PB_KDMA_H
#define HW_XBOX_HLE_PB_KDMA_H

#include <stdint.h>

enum {
    HLE_KDMA_A,          /* textures, palettes */
    HLE_KDMA_B,
    HLE_KDMA_COLOR,
    HLE_KDMA_ZETA,
    HLE_KDMA_VERTEX_A,
    HLE_KDMA_VERTEX_B,
    HLE_KDMA_N
};

/* SET_TEXTURE_FORMAT 1:0: 1 context A, 2 context B */
static inline unsigned hle_kdma_texture(uint32_t fmt)
{
    return fmt & 2 ? HLE_KDMA_B : HLE_KDMA_A;
}

/* SET_TEXTURE_PALETTE: offset 31:6, length 3:2, bit 0 context A or B */
static inline unsigned hle_kdma_palette(uint32_t pal)
{
    return pal & 1 ? HLE_KDMA_B : HLE_KDMA_A;
}

/* A P8 texture's palette: where it starts, base added to the offset (any
 * address, 0 too: the word is 0 for 256 entries at offset 0 of context A),
 * and its entries, 256 >> length. */
static inline void hle_kdma_palette_ref(uint32_t pal, uint32_t base, uint32_t *pa, uint32_t *entries)
{
    *pa = ((pal & ~0x3Fu) + base) & 0x0FFFFFC0u;
    *entries = 256u >> ((pal >> 2) & 3);
}

/* SET_VERTEX_DATA_ARRAY_OFFSET: offset 30:0, bit 31 vertex context A or B */
static inline unsigned hle_kdma_array(uint32_t off)
{
    return off >> 31 ? HLE_KDMA_VERTEX_B : HLE_KDMA_VERTEX_A;
}

static inline uint32_t hle_kdma_array_at(uint32_t off, uint32_t base)
{
    return (base + (off & 0x7FFFFFFFu)) & 0x7FFFFFFFu;
}

#endif
