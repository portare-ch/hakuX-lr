/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX-lr HLE GPU — the NV2A's primitives as the triangles, lines and points
 * Vulkan draws (prim.h).
 */
#include "hw/xbox/hle/pb/prim.h"

enum {
    PT_POINTLIST = 1, PT_LINELIST, PT_LINELOOP, PT_LINESTRIP, PT_TRIANGLELIST,
    PT_TRIANGLESTRIP, PT_TRIANGLEFAN, PT_QUADLIST, PT_QUADSTRIP, PT_POLYGON,
};
/* VkPrimitiveTopology (no Vulkan headers here) */
enum { VT_POINTS = 0, VT_LINES = 1, VT_LINE_STRIP = 2, VT_TRIS = 3, VT_TRI_STRIP = 4,
       VT_TRI_FAN = 5 };

/* The NV2A's flat shading takes a primitive's colours from one vertex, as
 * OpenGL's provoking vertex (nxdk_pgraph_tests' Shade model): FIRST_VTX the
 * first of a triangle, strip triangle i's vertex i, fan triangle i's i + 1;
 * LAST_VTX each triangle's last. A quad's (list or strip) is its last and a
 * polygon's its first under either. Vulkan's is the first of each triangle
 * it draws (strip i: i, fan i: i + 1): the others are lists of triangles
 * that start with the vertex, wound as they were. Two triangles of a quad
 * otherwise took two colours, and LAST_VTX drew FIRST_VTX's. */
static bool flat_list(uint32_t pt)
{
    if (!(pt & (HLE_PRIM_FLAT_FIRST | HLE_PRIM_FLAT_LAST))) {
        return false;
    }
    switch (HLE_PRIM_TYPE(pt)) {
    case PT_QUADSTRIP: case PT_POLYGON:
        return true;
    case PT_TRIANGLELIST: case PT_TRIANGLESTRIP: case PT_TRIANGLEFAN:
        return (pt & HLE_PRIM_FLAT_LAST) != 0;
    default:
        return false;
    }
}

/* The NV2A draws a polygon in line mode by its own edges (its edge flags):
 * a quad's four, a polygon's outline. As triangles in line mode the
 * diagonals between them were drawn too (nxdk_pgraph_tests' Line width). */
static bool outline(uint32_t pt)
{
    const uint32_t t = HLE_PRIM_TYPE(pt);
    return (pt & HLE_PRIM_OUTLINE) && (t == PT_QUADLIST || t == PT_QUADSTRIP || t == PT_POLYGON);
}

bool hle_draw_prim_converted(uint32_t pt)
{
    return outline(pt) || HLE_PRIM_TYPE(pt) == PT_QUADLIST || HLE_PRIM_TYPE(pt) == PT_LINELOOP || flat_list(pt);
}

uint32_t hle_draw_prim_topology(uint32_t pt)
{
    if (outline(pt)) {
        return VT_LINES;
    }
    if (flat_list(pt)) {
        return VT_TRIS;
    }
    pt = HLE_PRIM_TYPE(pt);
    switch (pt) {
    case PT_POINTLIST:     return VT_POINTS;
    case PT_LINELIST:      return VT_LINES;
    case PT_LINELOOP:
    case PT_LINESTRIP:     return VT_LINE_STRIP;
    case PT_TRIANGLESTRIP:
    case PT_QUADSTRIP:     return VT_TRI_STRIP;    /* same vertex order */
    case PT_TRIANGLEFAN:
    case PT_POLYGON:       return VT_TRI_FAN;
    default:               return VT_TRIS;         /* lists; quads become triangles */
    }
}

uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n)
{
    if (outline(pt)) {
        switch (HLE_PRIM_TYPE(pt)) {
        case PT_QUADLIST:  return n / 4 * 8;
        case PT_QUADSTRIP: return n >= 4 ? (n / 2 - 1) * 8 : 0;
        default:           return n >= 2 ? n * 2 : 0;
        }
    }
    if (flat_list(pt)) {
        switch (HLE_PRIM_TYPE(pt)) {
        case PT_TRIANGLELIST: return n / 3 * 3;
        case PT_QUADSTRIP:    return n >= 4 ? (n / 2 - 1) * 6 : 0;
        default:              return n >= 3 ? (n - 2) * 3 : 0;
        }
    }
    pt = HLE_PRIM_TYPE(pt);
    if (pt == PT_QUADLIST) {
        return n / 4 * 6;
    }
    if (pt == PT_LINELOOP) {
        return n ? n + 1 : 0;
    }
    return n;
}

void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out)
{
#define IN(i) ((uint16_t)((in ? in[i] : (i)) - base))
#define TRI(a, b, c) (*out++ = IN(a), *out++ = IN(b), *out++ = IN(c))
    const bool flat = pt & (HLE_PRIM_FLAT_FIRST | HLE_PRIM_FLAT_LAST);
#define LINE(a, b) (*out++ = IN(a), *out++ = IN(b))
    if (outline(pt)) {
        switch (HLE_PRIM_TYPE(pt)) {
        case PT_QUADLIST:
            for (uint32_t q = 0; q + 3 < n; q += 4) {
                LINE(q, q + 1);
                LINE(q + 1, q + 2);
                LINE(q + 2, q + 3);
                LINE(q + 3, q);
            }
            break;
        case PT_QUADSTRIP:              /* quad i: 2 i, 2 i + 1, 2 i + 3, 2 i + 2 around */
            for (uint32_t i = 0; i + 3 < n; i += 2) {
                LINE(i, i + 1);
                LINE(i + 1, i + 3);
                LINE(i + 3, i + 2);
                LINE(i + 2, i);
            }
            break;
        default:
            for (uint32_t i = 0; n >= 2 && i < n; i++) {
                LINE(i, i + 1 < n ? i + 1 : 0);
            }
            break;
        }
        return;
    }
    if (flat_list(pt)) {
        switch (HLE_PRIM_TYPE(pt)) {
        case PT_TRIANGLELIST:
            for (uint32_t i = 0; i + 2 < n; i += 3) {
                TRI(i + 2, i, i + 1);
            }
            break;
        case PT_TRIANGLESTRIP:
            for (uint32_t i = 0; i + 2 < n; i++) {
                if (i & 1) {
                    TRI(i + 2, i + 1, i);
                } else {
                    TRI(i + 2, i, i + 1);
                }
            }
            break;
        case PT_TRIANGLEFAN:
            for (uint32_t i = 0; i + 2 < n; i++) {
                TRI(i + 2, 0, i + 1);
            }
            break;
        case PT_QUADSTRIP:              /* each quad's last, the strip's 2 i + 3 */
            for (uint32_t i = 0; i + 3 < n; i += 2) {
                TRI(i + 3, i + 2, i);
                TRI(i + 3, i, i + 1);
            }
            break;
        default:                        /* a polygon's first */
            for (uint32_t i = 0; i + 2 < n; i++) {
                TRI(0, i + 1, i + 2);
            }
            break;
        }
        return;
    }
    pt = HLE_PRIM_TYPE(pt);
    if (pt == PT_QUADLIST) {
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            if (flat) {                 /* the quad's last */
                TRI(q + 3, q, q + 1);
                TRI(q + 3, q + 1, q + 2);
            } else {
                TRI(q, q + 1, q + 2);
                TRI(q, q + 2, q + 3);
            }
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        *out++ = IN(i);
    }
    if (pt == PT_LINELOOP && n) {
        *out = IN(0);
    }
#undef LINE
#undef TRI
#undef IN
}
