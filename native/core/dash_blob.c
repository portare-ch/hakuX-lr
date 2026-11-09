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
