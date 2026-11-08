/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hle_pb_blit_clip (hw/xbox/hle/pb/blitclip.c): an NV09F blit's rows cut to
 * the narrower pitch and the destination's GPU tile. */
#include "hw/xbox/hle/pb/blitclip.h"
#include "hle_test.h"

static uint32_t g_tiles[16];

/* tile i: base..limit (the last byte), valid */
static void tile(unsigned i, uint32_t base, uint32_t limit)
{
    g_tiles[2 * i] = base | 1;
    g_tiles[2 * i + 1] = limit;
}

static void test_inside(void)
{
    uint32_t rw, rows, tail;
    HT_CHECK(hle_pb_blit_clip(640, 480, 4, 2560, 2560, 0x03cd4000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rw, 640);
    HT_EQ_U32(rows, 480);
    HT_EQ_U32(tail, 0);
}

/* WWE WrestleMania 21: 1023 x 511 from pitch 4096 to its 640 x 480 front
 * buffer (pitch 2560), its tile ending where the push buffer begins */
static void test_wrestlemania(void)
{
    tile(0, 0x03cd4000, 0x03dfffff);
    uint32_t rw, rows, tail;
    HT_CHECK(hle_pb_blit_clip(1023, 511, 4, 4096, 2560, 0x03cd4000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rw, 640);                      /* the narrower pitch */
    HT_EQ_U32(rows, 480);                    /* the tile's end */
    HT_EQ_U32(tail, 0);
    /* the last row's end within the tile: nothing cut */
    HT_CHECK(hle_pb_blit_clip(640, 480, 4, 2560, 2560, 0x03cd4000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rows, 480);
}

/* the limit inside a row: whole rows, then part of one */
static void test_tail(void)
{
    tile(1, 0x01000000, 0x01000000 + 2 * 256 + 100 - 1);
    uint32_t rw, rows, tail;
    HT_CHECK(hle_pb_blit_clip(64, 8, 4, 256, 256, 0x01000000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rows, 2);
    HT_EQ_U32(tail, 100);
    /* in the gap past a row's 64 pixels (pitch 512): that row whole */
    tile(1, 0x01000000, 0x01000000 + 2 * 512 + 300 - 1);
    HT_CHECK(hle_pb_blit_clip(64, 8, 4, 512, 512, 0x01000000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rows, 3);
    HT_EQ_U32(tail, 0);
}

/* an invalid tile, or one the blit doesn't cross, cuts nothing */
static void test_other_tiles(void)
{
    for (unsigned i = 0; i < 16; i++) {
        g_tiles[i] = 0;
    }
    g_tiles[4] = 0x02000000;                 /* tile 2 not valid */
    g_tiles[5] = 0x02000fff;
    tile(3, 0x05000000, 0x05ffffff);         /* elsewhere */
    uint32_t rw, rows, tail;
    HT_CHECK(hle_pb_blit_clip(256, 64, 4, 1024, 1024, 0x02000000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rows, 64);
}

/* one row with no pitch copies; nothing to copy is false */
static void test_edges(void)
{
    uint32_t rw, rows, tail;
    HT_CHECK(hle_pb_blit_clip(4, 1, 4, 0, 0, 0x02000000, g_tiles, &rw, &rows, &tail));
    HT_EQ_U32(rw, 4);
    HT_EQ_U32(rows, 1);
    HT_CHECK(!hle_pb_blit_clip(4, 2, 4, 0, 16, 0x02000000, g_tiles, &rw, &rows, &tail));
    HT_CHECK(!hle_pb_blit_clip(0, 4, 4, 16, 16, 0x02000000, g_tiles, &rw, &rows, &tail));
    HT_CHECK(!hle_pb_blit_clip(4, 4, 0, 16, 16, 0x02000000, g_tiles, &rw, &rows, &tail));
}

int main(void)
{
    HT_RUN(test_inside);
    HT_RUN(test_wrestlemania);
    HT_RUN(test_tail);
    HT_RUN(test_other_tiles);
    HT_RUN(test_edges);
    HT_DONE();
}
