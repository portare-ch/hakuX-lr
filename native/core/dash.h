/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NATIVE_CORE_DASH_H
#define NATIVE_CORE_DASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bundled dashboard XBE. */
const uint8_t *nc_dash_xbe(size_t *size);

/* Check whether the certificate title is hakux-dash. */
bool nc_dash_is_ours(const char *path);

/* Install the bundled dashboard on C: and report whether the main XBE is present.
 * Tests can set NC_DASH_KEEP=1 to skip installation. */
bool nc_dash_install(const char *hdd);

#endif
