/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — the part of an NV09F (image blit) copy the NV2A makes, as
 * xemu's pgraph image blit has it. Pure: the decoder passes the registers.
 */
#ifndef HW_XBOX_HLE_PB_BLITCLIP_H
#define HW_XBOX_HLE_PB_BLITCLIP_H

#include <stdbool.h>
#include <stdint.h>

/* A w x h blit of bpp-byte pixels, source and destination pitches sp and dp
 * (0 for a one-row blit), to physical address dst: rows no longer than the
 * narrower pitch (*row_px pixels), and the destination cut at the limit of
 * the GPU tile it starts in (tiles: NV_PFB_TILE's 8 {base and flags, limit}
 * pairs, valid in bit 0; the limit the tile's last byte): *rows whole rows
 * and *tail bytes of one more. False: nothing to copy. */
bool hle_pb_blit_clip(uint32_t w, uint32_t h, uint32_t bpp, uint32_t sp, uint32_t dp, uint32_t dst,
                      const uint32_t tiles[16], uint32_t *row_px, uint32_t *rows, uint32_t *tail);

#endif
