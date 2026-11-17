/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/accuracy.h"
#include "hw/xbox/hle/shader/psh.h"
#include "hle_test.h"

#include <string.h>

/* each level's settings, whatever Custom's are */
static void test_presets(void)
{
    const HleGraphicsAccuracy off = { false, false, false }, on = { true, true, true };
    for (int i = 0; i < 2; i++) {
        const HleGraphicsAccuracy *c = i ? &on : &off;
        HleGraphicsAccuracy a = hle_accuracy_resolve(HLE_ACCURACY_POTATO, c);
        HT_CHECK(!a.precise_combiners && !a.precise_signed_blend && !a.precise_postblend_dither);
        a = hle_accuracy_resolve(HLE_ACCURACY_MEDIUM, c);
        HT_CHECK(a.precise_combiners && a.precise_signed_blend && !a.precise_postblend_dither);
        a = hle_accuracy_resolve(HLE_ACCURACY_HIGH, c);
        HT_CHECK(a.precise_combiners && a.precise_signed_blend && a.precise_postblend_dither);
    }
}

/* Custom: the settings given, or the defaults (everything on but, off AMD64
 * Linux, the combiners) */
static void test_custom(void)
{
    const HleGraphicsAccuracy c = { true, false, true };
    HleGraphicsAccuracy a = hle_accuracy_resolve(HLE_ACCURACY_CUSTOM, &c);
    HT_CHECK(a.precise_combiners && !a.precise_signed_blend && a.precise_postblend_dither);
    a = hle_accuracy_resolve(HLE_ACCURACY_CUSTOM, NULL);
    const HleGraphicsAccuracy d = hle_accuracy_default();
    HT_CHECK(a.precise_combiners == d.precise_combiners && a.precise_signed_blend && a.precise_postblend_dither);
    HT_CHECK(d.precise_signed_blend && d.precise_postblend_dither);
}

/* a run that sets nothing has the defaults; a level set reaches the pixel
 * shaders' key (the combiners' rounding) */
static void test_set(void)
{
    const HleGraphicsAccuracy d = hle_accuracy_default();
    HT_CHECK(hle_accuracy()->precise_combiners == d.precise_combiners);
    HT_CHECK(hle_accuracy_level() == HLE_ACCURACY_CUSTOM);
    uint32_t rs[HLE_PS_REGS] = { 0 };
    HlePshKey k;
    for (int lv = HLE_ACCURACY_POTATO; lv <= HLE_ACCURACY_HIGH; lv++) {
        const HleGraphicsAccuracy a = hle_accuracy_resolve((HleAccuracyLevel)lv, NULL);
        hle_accuracy_set((HleAccuracyLevel)lv, &a);
        HT_CHECK(hle_accuracy_level() == (HleAccuracyLevel)lv);
        HT_CHECK(hle_accuracy()->precise_postblend_dither == (lv == HLE_ACCURACY_HIGH));
        hle_psh_key(rs, &k);
        HT_CHECK(k.precise == (lv != HLE_ACCURACY_POTATO));
    }
    HT_CHECK(!strcmp(hle_accuracy_level_name(HLE_ACCURACY_POTATO), "Potato"));
    HT_CHECK(!strcmp(hle_accuracy_level_name(HLE_ACCURACY_CUSTOM), "Custom"));
}

int main(void)
{
    test_set();
    test_presets();
    test_custom();
    HT_DONE();
}
