/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The console's system fonts and the programs that check them. */
#ifndef NATIVE_CORE_SYSFONT_H
#define NATIVE_CORE_SYSFONT_H

#include "native/core/xdvdfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Before a disc XBE boots: an Xbox Live updater (a game disc's update.xbe)
 * draws with the dashboard's fonts on C: and takes them only if they hash as
 * its file table (the XIPS section) says, which the original dashboard's
 * do. Where C: has the open font the core installed (dash.c) at a path of
 * that table, the table's entry for it, and the section's digest in the XBE
 * header, become that font's in the image the core boots. Nothing else of
 * the XBE changes, another font at the path leaves its entry as it is, and
 * the game is not a modified one for Xbox Live. hdd: the hard disk's host
 * folder. */
void nc_sysfont_accept(NcDisc *disc, const char *xbe_path, const char *hdd);

#ifdef __cplusplus
}
#endif

#endif
