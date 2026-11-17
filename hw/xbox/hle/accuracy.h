/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — the accuracy level: which of the renderer's graphics
 * trade-offs a run makes. A level is resolved once, before the device is
 * made, into the settings below; the renderer reads those, never the level.
 * Each setting that changes a shader is in its key (HlePshKey.precise,
 * sblend, fblend), so pipelines of different levels never mix.
 *
 * Render scale, audio, pacing, memory and the CPU are not part of it.
 */
#ifndef HW_XBOX_HLE_ACCURACY_H
#define HW_XBOX_HLE_ACCURACY_H

#include <stdbool.h>

typedef enum HleAccuracyLevel {
    HLE_ACCURACY_POTATO,        /* approximations allowed, for constrained devices */
    HLE_ACCURACY_MEDIUM,        /* the balance for play */
    HLE_ACCURACY_HIGH,          /* every accuracy path the device supports */
    HLE_ACCURACY_CUSTOM,        /* the individual settings as given */
} HleAccuracyLevel;

typedef struct HleGraphicsAccuracy {
    /* combiner stage outputs rounded to the NV2A's 9 bits (shader/psh.c) */
    bool precise_combiners;
    /* ADDSIGNED / REVSUBTRACTSIGNED blended by the shader with the target's
     * pixel; off: the unsigned equations (vk/draw.c signed_blend) */
    bool precise_signed_blend;
    /* a dithered blend into a 16-bit target: blended by the shader and
     * dithered after, as the NV2A orders them; off: the host's blend over a
     * dithered colour (vk/draw.c dither_blend) */
    bool precise_postblend_dither;
} HleGraphicsAccuracy;

/* The settings a level stands for. custom: CUSTOM's (the other levels ignore
 * it; NULL: what hle_accuracy_default gives). */
HleGraphicsAccuracy hle_accuracy_resolve(HleAccuracyLevel level, const HleGraphicsAccuracy *custom);

/* What a run that never sets a level uses: every path on, the combiners as
 * the platform's default has them (rounded on AMD64 Linux). */
HleGraphicsAccuracy hle_accuracy_default(void);

/* The run's settings: set before the device is made (a core's options, at
 * boot), read by the renderer. */
void hle_accuracy_set(HleAccuracyLevel level, const HleGraphicsAccuracy *a);
const HleGraphicsAccuracy *hle_accuracy(void);
HleAccuracyLevel hle_accuracy_level(void);
const char *hle_accuracy_level_name(HleAccuracyLevel level);

#endif
