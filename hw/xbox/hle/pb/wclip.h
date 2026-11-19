/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — push-buffer mode's window clip (SET_WINDOW_CLIP_*) as
 * disjoint rectangles: the back end records a draw once in each, scissored
 * to it (vk/draw.c). Pure: the decoder passes the registers.
 */
#ifndef HW_XBOX_HLE_PB_WCLIP_H
#define HW_XBOX_HLE_PB_WCLIP_H

#include "hw/xbox/hle/front/cmd.h"

/* Intersect the surface clip area with the NV2A window clip and write up to HLE_CMD_MAX_CLIP
 * disjoint rectangles to out. The surface area uses exclusive right and bottom edges in
 * render-target pixels. The eight horizontal and vertical register pairs use inclusive min
 * (bits 11:0) and max (27:16) in logical pixels. Scale each axis by its antialiasing factor
 * (ax or ay).
 *
 * Inclusive mode keeps the union of the clip rectangles. Exclusive mode keeps the area
 * outside all of them. The hardware uses rectangles through the most recently written index,
 * even if registers at higher indices still contain old values. A pair of zero words covers
 * pixel (0, 0). When count is unknown (zero), use only pairs with at least one nonzero word.
 *
 * Return the number of output rectangles, 0 if everything is clipped or -1 if the whole
 * surface area can be drawn without clipping. */
int hle_pb_window_clip(const uint32_t *horiz, const uint32_t *vert, unsigned count, bool exclusive,
                       uint32_t ax, uint32_t ay, const HleRect *area, HleRect *out);

#endif
