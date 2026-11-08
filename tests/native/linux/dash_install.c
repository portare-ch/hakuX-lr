/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test dashboard installation, upgrades, and concurrent boots. */
#include "native/core/dash.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failed;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        printf("%s: ", (cond) ? "ok" : "FAIL");                                                              \
        printf(__VA_ARGS__);                                                                                 \
        printf("\n");                                                                                        \
        g_failed |= !(cond);                                                                                 \
    } while (0)

static char g_root[256], g_hdd[300];

static void path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/hdd/Partition2/%s", g_root, rel);
}

static long size_of(const char *rel)
{
    char p[512];
    struct stat st;
    path(p, sizeof(p), rel);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static ino_t inode_of(const char *rel)
{
    char p[512];
    struct stat st;
    path(p, sizeof(p), rel);
    return stat(p, &st) == 0 ? st.st_ino : 0;
}

static void put(const char *rel, const uint8_t *data, size_t n)
{
    char p[512];
    path(p, sizeof(p), rel);
    FILE *f = fopen(p, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

static bool embedded_at(const char *p)
{
    size_t n;
    const uint8_t *xbe = nc_dash_xbe(&n);
    FILE *f = fopen(p, "rb");
    if (!f) {
        return false;
    }
    uint8_t *buf = malloc(n + 1);
    const bool same = fread(buf, 1, n + 1, f) == n && !memcmp(buf, xbe, n);
    free(buf);
    fclose(f);
    return same;
}

static bool embedded(const char *rel)
{
    char p[512];
    path(p, sizeof(p), rel);
    return embedded_at(p);
}

/* Count directory entries, excluding . and ... */
static int entries(const char *rel)
{
    char p[512];
    path(p, sizeof(p), rel);
    DIR *d = opendir(p);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* Reset the fixture to an empty C: drive. */
static void fresh(void)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/hdd' && mkdir -p '%s/hdd/Partition2'", g_root, g_root);
    if (system(cmd) != 0) {
        g_failed = 1;
    }
    snprintf(g_hdd, sizeof(g_hdd), "%s/hdd", g_root);
}

int main(void)
{
    snprintf(g_root, sizeof(g_root), "/tmp/hakux-dash-XXXXXX");
    if (!mkdtemp(g_root)) {
        return 1;
    }
    size_t n;
    const uint8_t *xbe = nc_dash_xbe(&n);
    CHECK(n > 0x1000 && !memcmp(xbe, "XBEH", 4), "the core embeds an XBE (%zu bytes)", n);

    fresh();
    CHECK(nc_dash_install(g_hdd) && embedded("xboxdash.xbe") && embedded("xodash/xonlinedash.xbe"),
          "an empty C: gets the dashboard as xboxdash.xbe and xodash\\xonlinedash.xbe");
    char p[512];
    path(p, sizeof(p), "xboxdash.xbe");
    CHECK(nc_dash_is_ours(p), "it reads as hakuX-lr's (its certificate's title)");

    const ino_t dash = inode_of("xboxdash.xbe"), online = inode_of("xodash/xonlinedash.xbe");
    nc_dash_install(g_hdd);
    CHECK(inode_of("xboxdash.xbe") == dash && inode_of("xodash/xonlinedash.xbe") == online,
          "the same dashboard again: neither file written");

    uint8_t *old = malloc(n);
    memcpy(old, xbe, n);
    old[n - 1] ^= 0xFF;                     /* Simulate an older build with the same title. */
    put("xboxdash.xbe", old, n);
    put("xodash/xonlinedash.xbe", old, n - 16);
    nc_dash_install(g_hdd);
    CHECK(embedded("xboxdash.xbe") && embedded("xodash/xonlinedash.xbe"), "an older copy of ours is replaced");

    /* Replace another dashboard's XBE and keep its other files. */
    uint8_t *theirs = malloc(n);
    memcpy(theirs, xbe, n);
    uint32_t base, cert;
    memcpy(&base, theirs + 0x104, 4);
    memcpy(&cert, theirs + 0x118, 4);
    theirs[cert - base + 0xC] = 'H';        /* Use a different certificate title. */
    fresh();
    put("XBOXDASH.XBE", theirs, n);
    path(p, sizeof(p), "fonts");
    mkdir(p, 0775);
    put("fonts/Xbox.xtf", (const uint8_t *)"font", 4);
    nc_dash_install(g_hdd);
    CHECK(embedded("XBOXDASH.XBE") && size_of("xboxdash.xbe") < 0 && embedded("xodash/xonlinedash.xbe") &&
              size_of("fonts/Xbox.xtf") == 4,
          "someone else's XBOXDASH.XBE: replaced under its name, its other files kept");

    /* Reuse an existing XODash folder and replace another XBE in it. */
    fresh();
    path(p, sizeof(p), "XODash");
    mkdir(p, 0775);
    put("XODash/xonlinedash.xbe", theirs, n);
    nc_dash_install(g_hdd);
    CHECK(embedded("xboxdash.xbe") && size_of("xodash") < 0 && embedded("XODash/xonlinedash.xbe"),
          "an XODash folder is used as it is, someone else's xonlinedash.xbe in it replaced");

    /* Skip dashboard installation with NC_DASH_KEEP=1. */
    fresh();
    put("xboxdash.xbe", theirs, n);
    setenv("NC_DASH_KEEP", "1", 1);
    nc_dash_install(g_hdd);
    unsetenv("NC_DASH_KEEP");
    CHECK(size_of("xboxdash.xbe") == (long)n && !embedded("xboxdash.xbe") && size_of("xodash") < 0,
          "NC_DASH_KEEP=1: someone else's xboxdash.xbe kept, nothing added");

    /* An xboxrun HDD override can point to a missing directory. */
    char cmd[600], q[600], r[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/hdd'", g_root);
    if (system(cmd) != 0) {
        g_failed = 1;
    }
    snprintf(g_hdd, sizeof(g_hdd), "%s/hdd/new/disk", g_root);
    snprintf(q, sizeof(q), "%s/Partition2/xboxdash.xbe", g_hdd);
    snprintf(r, sizeof(r), "%s/Partition2/xodash/xonlinedash.xbe", g_hdd);
    CHECK(nc_dash_install(g_hdd) && embedded_at(q) && embedded_at(r),
          "a hard disk folder that isn't there yet: made, then the dashboard put on it");

    /* Concurrent boots leave complete XBEs and remove temporary files. */
    bool whole = true;
    for (int round = 0; round < 10 && whole; round++) {
        fresh();
        pid_t kids[8];
        for (int i = 0; i < 8; i++) {
            if ((kids[i] = fork()) == 0) {
                _exit(nc_dash_install(g_hdd) ? 0 : 1);
            }
        }
        for (int i = 0; i < 8; i++) {
            int status = 1;
            waitpid(kids[i], &status, 0);
            whole &= WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        whole &= embedded("xboxdash.xbe") && embedded("xodash/xonlinedash.xbe") && entries("") == 2 &&
                 entries("xodash") == 1;
    }
    CHECK(whole, "eight boots installing at once: one whole copy, nothing else on C:");

    free(old);
    free(theirs);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    if (system(cmd) != 0) {
        g_failed = 1;
    }
    printf(g_failed ? "FAIL: dash-install-test\n" : "PASS: dash-install-test\n");
    return g_failed;
}
