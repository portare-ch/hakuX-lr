/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/core/online.h"
#include "native/core/nc.h"
#include "native/net/net.h"

#include <stdbool.h>
#include <stddef.h>

static bool g_mods;
static const char *g_what;              /* First reason for the block, or NULL. */
static const char *g_notice;            /* Pending frontend notice. */
static NcOnlineCause g_cause;
static bool g_cheated;                  /* Saved independently of the first reason. */

void nc_online_set_cheats(bool allowed)
{
    __atomic_store_n(&g_mods, allowed, __ATOMIC_RELEASE);
}

bool nc_online_cheats(void)
{
    return __atomic_load_n(&g_mods, __ATOMIC_ACQUIRE);
}

void nc_online_boot(void)
{
    __atomic_store_n(&g_what, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_notice, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cheated, false, __ATOMIC_RELEASE);
    nc_net_internet(true);
}

void nc_online_modified(NcOnlineCause cause, const char *what)
{
    if (cause == NC_ONLINE_CHEATS) {
        __atomic_store_n(&g_cheated, true, __ATOMIC_RELEASE);
    }
    const char *none = NULL;
    if (!__atomic_compare_exchange_n(&g_what, &none, what, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&g_cause, cause, __ATOMIC_RELEASE);
    nc_net_internet(false);
    nc_log("hakuX-native", "online: %s changed the game: Xbox Live (the Internet) is off until it boots "
           "again; system link stays", what);
    __atomic_store_n(&g_notice, what, __ATOMIC_RELEASE);
}

NcOnlineCause nc_online_cause(void)
{
    return __atomic_load_n(&g_cause, __ATOMIC_ACQUIRE);
}

bool nc_online_cheated(void)
{
    return __atomic_load_n(&g_cheated, __ATOMIC_ACQUIRE);
}

bool nc_online_blocked(void)
{
    return __atomic_load_n(&g_what, __ATOMIC_ACQUIRE) != NULL;
}

const char *nc_online_take_notice(void)
{
    return __atomic_exchange_n(&g_notice, NULL, __ATOMIC_ACQ_REL);
}
