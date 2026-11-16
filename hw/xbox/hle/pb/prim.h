/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HAKUX_HLE_PB_PRIM_H
#define HAKUX_HLE_PB_PRIM_H

#include <stdbool.h>
#include <stdint.h>

/* NV2A / Xbox D3D primitive types (1 points .. 10 polygon): the Vulkan
 * topology they are drawn with, the index count after conversion, and the
 * converted indices (in NULL: 0..n-1; rebased by -base). */
/* pt may carry the flat shading in effect (SHADE_MODEL FLAT): the vertex a
 * primitive takes its colours from, FLAT_SHADE_OP's. Vulkan's is each
 * triangle's first, so a primitive whose vertex is another is drawn as a
 * list of triangles that start with it (hle_draw_prim_converted). */
#define HLE_PRIM_FLAT_FIRST 0x100u
#define HLE_PRIM_FLAT_LAST  0x200u
#define HLE_PRIM_TYPE(pt)   ((pt) & 0xFFu)
bool hle_draw_prim_converted(uint32_t pt);      /* its indices are not 0 .. n-1 as given */
uint32_t hle_draw_prim_topology(uint32_t pt);
uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n);
void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out);

#endif
