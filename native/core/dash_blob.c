/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Embed the dashboard from the XBE selected at build time. */
#include "native/core/dash.h"

__asm__(".pushsection .rodata\n"
        ".balign 16\n"
        ".hidden nc_dash_xbe_start\n"
        ".hidden nc_dash_xbe_end\n"
        "nc_dash_xbe_start:\n"
        ".incbin \"" NC_DASH_XBE "\"\n"
        "nc_dash_xbe_end:\n"
        ".popsection\n");

extern const uint8_t nc_dash_xbe_start[], nc_dash_xbe_end[];

const uint8_t *nc_dash_xbe(size_t *size)
{
    *size = (size_t)(nc_dash_xbe_end - nc_dash_xbe_start);
    return nc_dash_xbe_start;
}

/* The open system font (native/dash/fonts), gzip-compressed, and its licence. */
__asm__(".pushsection .rodata\n"
        ".balign 16\n"
        ".hidden nc_dash_font_start\n"
        ".hidden nc_dash_font_end\n"
        ".hidden nc_dash_font_license_start\n"
        ".hidden nc_dash_font_license_end\n"
        "nc_dash_font_start:\n"
        ".incbin \"" NC_DASH_FONT "\"\n"
        "nc_dash_font_end:\n"
        "nc_dash_font_license_start:\n"
        ".incbin \"" NC_DASH_FONT_LICENSE "\"\n"
        "nc_dash_font_license_end:\n"
        ".popsection\n");

extern const uint8_t nc_dash_font_start[], nc_dash_font_end[];
extern const uint8_t nc_dash_font_license_start[], nc_dash_font_license_end[];

const uint8_t *nc_dash_font_gz(size_t *size)
{
    *size = (size_t)(nc_dash_font_end - nc_dash_font_start);
    return nc_dash_font_start;
}

const uint8_t *nc_dash_font_license(size_t *size)
{
    *size = (size_t)(nc_dash_font_license_end - nc_dash_font_license_start);
    return nc_dash_font_license_start;
}
