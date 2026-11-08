/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/pb/blitclip.h"

bool hle_pb_blit_clip(uint32_t w, uint32_t h, uint32_t bpp, uint32_t sp, uint32_t dp, uint32_t dst,
                      const uint32_t tiles[16], uint32_t *row_px, uint32_t *rows, uint32_t *tail)
{
    *row_px = *rows = *tail = 0;
    if (!bpp || !w || !h || (h > 1 && (!sp || !dp))) {
        return false;
    }
    uint32_t rw = w;
    if (sp && dp) {
        const uint32_t narrow = (sp < dp ? sp : dp) / bpp;
        rw = rw < narrow ? rw : narrow;
    }
    if (!rw) {
        return false;
    }
    uint32_t n = h, t = 0;
    const uint64_t end = dst + (uint64_t)(h - 1) * dp + (uint64_t)rw * bpp;
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t base = tiles[2 * i] & ~0x3FFFu, limit = tiles[2 * i + 1];
        if ((tiles[2 * i] & 1) && base <= dst && dst < limit && end > limit) {
            const uint32_t keep = limit + 1 - dst;
            n = dp ? keep / dp : 0;
            t = keep - n * dp;
            if (t >= rw * bpp) {
                t = 0;                  /* the cut falls past a whole row */
                n++;
            }
            break;
        }
    }
    *row_px = rw;
    *rows = n;
    *tail = t;
    return n || t;
}
