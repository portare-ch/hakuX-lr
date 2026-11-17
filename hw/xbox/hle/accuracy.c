/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/accuracy.h"
#include "hw/xbox/hle/shader/psh.h"

#include <stddef.h>

HleGraphicsAccuracy hle_accuracy_default(void)
{
    return (HleGraphicsAccuracy){
#if defined(__x86_64__) && !defined(__ANDROID__)
        .precise_combiners = true,
#else
        .precise_combiners = false,
#endif
        .precise_signed_blend = true,
        .precise_postblend_dither = true,
    };
}

/* The one place a level becomes settings. Potato's and Medium's
 * approximations are the paths the renderer already takes on a device
 * without the capability; none is new. */
HleGraphicsAccuracy hle_accuracy_resolve(HleAccuracyLevel level, const HleGraphicsAccuracy *custom)
{
    switch (level) {
    case HLE_ACCURACY_POTATO:
        return (HleGraphicsAccuracy){ .precise_combiners = false, .precise_signed_blend = false,
                                      .precise_postblend_dither = false };
    case HLE_ACCURACY_MEDIUM:
        return (HleGraphicsAccuracy){ .precise_combiners = true, .precise_signed_blend = true,
                                      .precise_postblend_dither = false };
    case HLE_ACCURACY_HIGH:
        return (HleGraphicsAccuracy){ .precise_combiners = true, .precise_signed_blend = true,
                                      .precise_postblend_dither = true };
    case HLE_ACCURACY_CUSTOM:
    default:
        return custom ? *custom : hle_accuracy_default();
    }
}

static HleGraphicsAccuracy g_acc;
static HleAccuracyLevel g_level = HLE_ACCURACY_CUSTOM;
static bool g_set;

void hle_accuracy_set(HleAccuracyLevel level, const HleGraphicsAccuracy *a)
{
    g_acc = *a;
    g_level = level;
    g_set = true;
    hle_psh_set_precision(a->precise_combiners);
}

const HleGraphicsAccuracy *hle_accuracy(void)
{
    if (!g_set) {
        g_acc = hle_accuracy_default();
        g_set = true;
    }
    return &g_acc;
}

HleAccuracyLevel hle_accuracy_level(void)
{
    return g_level;
}

const char *hle_accuracy_level_name(HleAccuracyLevel level)
{
    switch (level) {
    case HLE_ACCURACY_POTATO: return "Potato";
    case HLE_ACCURACY_MEDIUM: return "Medium";
    case HLE_ACCURACY_HIGH:   return "High";
    default:                  return "Custom";
    }
}
