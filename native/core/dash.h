/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NATIVE_CORE_DASH_H
#define NATIVE_CORE_DASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bundled dashboard XBE. */
const uint8_t *nc_dash_xbe(size_t *size);

/* The bundled open system font, gzip-compressed, and its licence text. */
const uint8_t *nc_dash_font_gz(size_t *size);
const uint8_t *nc_dash_font_license(size_t *size);

/* The open system font as C:\Xbox.xtf (book: C:\XBox Book.xtf) has it; NULL
 * if it can't be unpacked. */
const uint8_t *nc_dash_font(bool book, size_t *size);
/* Whether the host file is that font, byte for byte. */
bool nc_dash_font_is_ours(const char *path, bool book);

/* Check whether the certificate title is hakux-dash. */
bool nc_dash_is_ours(const char *path);

/* Install the bundled dashboard on C:, and the open system fonts where C: has
 * none, and report whether the main XBE is present.
 * Tests can set NC_DASH_KEEP=1 to skip installation. */
bool nc_dash_install(const char *hdd);

#endif
