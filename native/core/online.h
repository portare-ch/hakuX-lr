/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Per-session Internet access for games using cheats, mods or uncapped timing. */
#ifndef NATIVE_CORE_ONLINE_H
#define NATIVE_CORE_ONLINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the frontend's Cheats option before boot. */
void nc_online_set_cheats(bool allowed);
bool nc_online_cheats(void);
/* Clear the session's block and pending notice. */
void nc_online_boot(void);

typedef enum { NC_ONLINE_CHEATS, NC_ONLINE_UNCAPPED } NcOnlineCause;
/* Block Internet access until the next boot and queue a notice. Thread-safe. */
void nc_online_modified(NcOnlineCause cause, const char *what);
bool nc_online_blocked(void);
/* First cause of the current block. */
NcOnlineCause nc_online_cause(void);
/* Whether cheats, patches or mods changed the game during this session. */
bool nc_online_cheated(void);
/* Consume the pending notice. Returns NULL if none is pending. */
const char *nc_online_take_notice(void);

#ifdef __cplusplus
}
#endif

#endif
