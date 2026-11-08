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

/* Install or update the bundled dashboard on C: (Partition2).
 * Return whether the main dashboard is present. */
bool nc_dash_install(const char *hdd);

#endif
