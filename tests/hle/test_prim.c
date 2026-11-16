/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/pb/prim.h"
#include "hle_test.h"

enum { TRIS = 5, STRIP = 6, FAN = 7, QUADS = 8, QSTRIP = 9, POLY = 10, LOOP = 3 };
enum { VT_TRIS = 3, VT_TRI_STRIP = 4, VT_TRI_FAN = 5 };

static uint16_t out[64];

static bool is(uint32_t pt, uint32_t n, uint32_t cnt, const uint16_t *want)
{
    memset(out, 0xEE, sizeof(out));
    if (hle_draw_prim_count(pt, n) != cnt) {
        return false;
    }
    hle_draw_prim_indices(pt, NULL, n, 0, out);
    return !memcmp(out, want, cnt * 2) && out[cnt] == 0xEEEE;
}

/* a triangle's vertices in the order they wind: the same triangle, the same
 * way round, whichever of them comes first */
static bool same_winding(const uint16_t *a, const uint16_t *b)
{
    for (unsigned r = 0; r < 3; r++) {
        if (a[0] == b[r] && a[1] == b[(r + 1) % 3] && a[2] == b[(r + 2) % 3]) {
            return true;
        }
    }
    return false;
}

/* without flat shading nothing changes: strips and fans as they are, a quad
 * the NV2A's two triangles */
static void test_smooth(void)
{
    HT_CHECK(!hle_draw_prim_converted(TRIS) && !hle_draw_prim_converted(STRIP) &&
             !hle_draw_prim_converted(FAN) && !hle_draw_prim_converted(QSTRIP) &&
             !hle_draw_prim_converted(POLY));
    HT_CHECK(hle_draw_prim_converted(QUADS) && hle_draw_prim_converted(LOOP));
    HT_EQ_U32(hle_draw_prim_topology(STRIP), VT_TRI_STRIP);
    HT_EQ_U32(hle_draw_prim_topology(QSTRIP), VT_TRI_STRIP);
    HT_EQ_U32(hle_draw_prim_topology(POLY), VT_TRI_FAN);
    HT_CHECK(is(QUADS, 4, 6, (const uint16_t[]){ 0, 1, 2, 0, 2, 3 }));
    HT_CHECK(is(STRIP, 5, 5, (const uint16_t[]){ 0, 1, 2, 3, 4 }));
}

/* FIRST_VTX: Vulkan's own vertex for triangles, strips and fans */
static void test_first(void)
{
    const uint32_t f = HLE_PRIM_FLAT_FIRST;
    HT_CHECK(!hle_draw_prim_converted(TRIS | f) && !hle_draw_prim_converted(STRIP | f) &&
             !hle_draw_prim_converted(FAN | f));
    HT_EQ_U32(hle_draw_prim_topology(FAN | f), VT_TRI_FAN);
    /* a quad's last vertex, a polygon's first: lists that start with it */
    HT_CHECK(is(QUADS | f, 8, 12, (const uint16_t[]){ 3, 0, 1, 3, 1, 2, 7, 4, 5, 7, 5, 6 }));
    HT_EQ_U32(hle_draw_prim_topology(QSTRIP | f), VT_TRIS);
    HT_CHECK(is(QSTRIP | f, 6, 12, (const uint16_t[]){ 3, 2, 0, 3, 0, 1, 5, 4, 2, 5, 2, 3 }));
    HT_EQ_U32(hle_draw_prim_topology(POLY | f), VT_TRIS);
    HT_CHECK(is(POLY | f, 5, 9, (const uint16_t[]){ 0, 1, 2, 0, 2, 3, 0, 3, 4 }));
    HT_EQ_U32(hle_draw_prim_count(QSTRIP | f, 3), 0);
    HT_EQ_U32(hle_draw_prim_count(POLY | f, 2), 0);
}

/* LAST_VTX: each triangle starts with its last vertex and winds as before */
static void test_last(void)
{
    const uint32_t l = HLE_PRIM_FLAT_LAST;
    HT_EQ_U32(hle_draw_prim_topology(TRIS | l), VT_TRIS);
    HT_EQ_U32(hle_draw_prim_topology(STRIP | l), VT_TRIS);
    HT_EQ_U32(hle_draw_prim_topology(FAN | l), VT_TRIS);
    HT_CHECK(is(TRIS | l, 7, 6, (const uint16_t[]){ 2, 0, 1, 5, 3, 4 }));
    HT_CHECK(is(STRIP | l, 5, 9, (const uint16_t[]){ 2, 0, 1, 3, 2, 1, 4, 2, 3 }));
    HT_CHECK(is(FAN | l, 5, 9, (const uint16_t[]){ 2, 0, 1, 3, 0, 2, 4, 0, 3 }));
    /* strip triangle i is i, i + 1, i + 2, every other one the other way round */
    hle_draw_prim_indices(STRIP | l, NULL, 6, 0, out);
    for (uint16_t i = 0; i < 4; i++) {
        const uint16_t t[3] = { i & 1 ? i + 1 : i, i & 1 ? i : i + 1, i + 2 };
        HT_CHECK(out[3 * i] == i + 2 && same_winding(out + 3 * i, t));
    }
    /* quads and polygons as under FIRST_VTX */
    HT_CHECK(is(QUADS | l, 4, 6, (const uint16_t[]){ 3, 0, 1, 3, 1, 2 }));
    HT_CHECK(is(POLY | l, 4, 6, (const uint16_t[]){ 0, 1, 2, 0, 2, 3 }));
}

/* given indices, rebased */
static void test_indexed(void)
{
    const uint16_t in[4] = { 10, 11, 12, 13 };
    hle_draw_prim_indices(QUADS | HLE_PRIM_FLAT_FIRST, in, 4, 10, out);
    HT_CHECK(!memcmp(out, (const uint16_t[]){ 3, 0, 1, 3, 1, 2 }, 12));
    hle_draw_prim_indices(LOOP, in, 4, 10, out);
    HT_CHECK(!memcmp(out, (const uint16_t[]){ 0, 1, 2, 3, 0 }, 10));
}

int main(void)
{
    test_smooth();
    test_first();
    test_last();
    test_indexed();
    HT_DONE();
}
