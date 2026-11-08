/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kelvin's context DMA objects (hw/xbox/hle/pb/kdma.h): the context each
 * offset names and the address it makes. */
#include "hw/xbox/hle/pb/kdma.h"
#include "hle_test.h"

/* SET_TEXTURE_FORMAT's context field: 1 A, 2 B */
static void test_texture(void)
{
    HT_EQ_U32(hle_kdma_texture(0x00010608 | 1), HLE_KDMA_A);
    HT_EQ_U32(hle_kdma_texture(0x00010608 | 2), HLE_KDMA_B);
}

/* the palette's context, start (base added; 0 is a palette too) and
 * entries (256 >> length) */
static void test_palette(void)
{
    uint32_t pa, n;
    HT_EQ_U32(hle_kdma_palette(0x00123440 | 1), HLE_KDMA_B);
    HT_EQ_U32(hle_kdma_palette(0x00123440), HLE_KDMA_A);
    hle_kdma_palette_ref(0x00123440 | 4 | 1, 0, &pa, &n);
    HT_EQ_U32(pa, 0x00123440);
    HT_EQ_U32(n, 128);
    /* the word 0: 256 entries at the start of context A, wherever it is */
    hle_kdma_palette_ref(0, 0x03000000, &pa, &n);
    HT_EQ_U32(pa, 0x03000000);
    HT_EQ_U32(n, 256);
    hle_kdma_palette_ref(0, 0, &pa, &n);
    HT_EQ_U32(pa, 0);
    HT_EQ_U32(n, 256);
    hle_kdma_palette_ref(0x40 | 0xC, 0x02000000, &pa, &n);
    HT_EQ_U32(pa, 0x02000040);
    HT_EQ_U32(n, 32);
    /* an adjust below 64 bytes doesn't move it off its alignment */
    hle_kdma_palette_ref(0x00123440, 0x20, &pa, &n);
    HT_EQ_U32(pa & 0x3F, 0);
}

/* bit 31 picks vertex context B; the rest is the offset */
static void test_array(void)
{
    HT_EQ_U32(hle_kdma_array(0x00001000), HLE_KDMA_VERTEX_A);
    HT_EQ_U32(hle_kdma_array(0x80001000), HLE_KDMA_VERTEX_B);
    HT_EQ_U32(hle_kdma_array_at(0x80001000, 0), 0x00001000);
    HT_EQ_U32(hle_kdma_array_at(0x00001000, 0x02000000), 0x02001000);
}

int main(void)
{
    HT_RUN(test_texture);
    HT_RUN(test_palette);
    HT_RUN(test_array);
    HT_DONE();
}
