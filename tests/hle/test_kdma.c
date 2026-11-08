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

/* the palette's offset moves by the base; context and length stay */
static void test_palette(void)
{
    const uint32_t pal = 0x00123440 | 4 | 1;          /* 128 entries, context B */
    HT_EQ_U32(hle_kdma_palette(pal), HLE_KDMA_B);
    HT_EQ_U32(hle_kdma_palette(pal & ~1u), HLE_KDMA_A);
    HT_EQ_U32(hle_kdma_palette_at(pal, 0), pal);
    HT_EQ_U32(hle_kdma_palette_at(pal, 0x03000000), 0x03123440 | 4 | 1);
    /* an adjust below 64 bytes doesn't reach the fields */
    HT_EQ_U32(hle_kdma_palette_at(pal, 0x20) & 0x3F, 4 | 1);
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
