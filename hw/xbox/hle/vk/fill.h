/* Copyright (C) 2026-present PortareOS */
/*
 * hakuX HLE GPU — fill modes and smoothing (NV2A_ACCURACY row 12).
 *
 * What one recording of a draw rasterizes, from the render states pb/kelvin.c
 * takes from SET_FRONT / BACK_POLYGON_MODE (0x038C, 0x0390), the smoothing
 * enables (0x0320, 0x0324) and SET_ANTI_ALIASING_CONTROL (0x1D7C). No Vulkan
 * here: the modes are VkPolygonMode's values, the culled faces
 * VkCullModeFlagBits' (tests/hle/test_fill.c).
 */
#ifndef HW_XBOX_HLE_VK_FILL_H
#define HW_XBOX_HLE_VK_FILL_H

#include <stdbool.h>
#include <stdint.h>

enum { HLE_FILL_FILL = 0, HLE_FILL_LINE = 1, HLE_FILL_POINT = 2 };     /* VkPolygonMode */
enum { HLE_CULL_NONE = 0, HLE_CULL_FRONT = 1, HLE_CULL_BACK = 2 };     /* VkCullModeFlagBits */
/* the faces a recording takes: both, or the front (back) ones, the others culled */
enum { HLE_FACES_BOTH, HLE_FACES_FRONT, HLE_FACES_BACK };
/* the topology's class: point, line or triangle lists and strips */
enum { HLE_TOPO_POINTS, HLE_TOPO_LINES, HLE_TOPO_TRIS };

/* A polygon-mode register's value: 0x1B00 points, 0x1B01 lines, anything else
 * filled (the power-on default; D3DFILLMODE uses GL's values) */
static inline unsigned hle_fill_mode(uint32_t v)
{
    return v == 0x1B00 ? HLE_FILL_POINT : v == 0x1B01 ? HLE_FILL_LINE : HLE_FILL_FILL;
}

/* A pipeline has one polygon mode: a triangle draw whose front and back faces
 * are filled differently is recorded twice when neither kind is culled, once
 * for each (the other culled), the front faces first (not in the guest's
 * primitive order). */
static inline bool hle_fill_split(unsigned topo, uint32_t front, uint32_t back, unsigned cull)
{
    return topo == HLE_TOPO_TRIS && cull == HLE_CULL_NONE &&
           hle_fill_mode(front) != hle_fill_mode(back);
}

typedef struct HleFill {
    uint8_t mode;               /* HLE_FILL_*: the polygon mode (fill for points, lines) */
    bool lines;                 /* rasterized as lines: line width and smoothing */
    bool points;                /* rasterized as points: point size and sprites */
    bool lsmooth, psmooth;      /* line, polygon smoothing in effect */
    uint8_t offset;             /* the polygon offset enable that applies: 0 SOLID,
                                   1 WIREFRAME, 2 POINT (the mode's) */
} HleFill;

/* One recording (faces: HLE_FACES_*): a triangle draw's faces take the front
 * mode, or the back one when only back faces are drawn (the front ones culled,
 * or the back pass of a split). Line and point primitives keep theirs. Without
 * nonsolid (the device's fillModeNonSolid) everything fills. Smoothing acts
 * with multisampling off only (aactl bit 0 clear; D3D's 0xFFFF0001 sets it):
 * the Xbox draws alike whatever the flags while it is on. edgeaa: bit 0 line,
 * bit 1 polygon smoothing; polygons smooth when filled. */
static inline HleFill hle_fill(unsigned topo, uint32_t front, uint32_t back, unsigned cull,
                               unsigned faces, uint32_t edgeaa, uint32_t aactl, bool nonsolid)
{
    HleFill f = { HLE_FILL_FILL, topo == HLE_TOPO_LINES, topo == HLE_TOPO_POINTS,
                  false, false, 0 };
    if (topo == HLE_TOPO_TRIS && nonsolid) {
        bool b = faces == HLE_FACES_BACK || (faces == HLE_FACES_BOTH && cull == HLE_CULL_FRONT);
        f.mode = (uint8_t)hle_fill_mode(b ? back : front);
        f.lines = f.mode == HLE_FILL_LINE;
        f.points = f.mode == HLE_FILL_POINT;
        f.offset = f.mode;
    }
    uint32_t smooth = aactl & 1 ? 0 : edgeaa & 3;
    f.lsmooth = f.lines && (smooth & 1);
    f.psmooth = topo == HLE_TOPO_TRIS && f.mode == HLE_FILL_FILL && (smooth & 2);
    return f;
}

#endif
