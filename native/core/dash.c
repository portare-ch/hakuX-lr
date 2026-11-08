/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Install the bundled XBE at the dashboard paths used by disc launchers and Xbox Live. */
#include "native/core/dash.h"
#include "native/core/nc.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE "hakux-dash"

bool nc_dash_is_ours(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t head[0x120], name[80];
    bool ours = false;
    if (fread(head, 1, sizeof(head), f) == sizeof(head) && !memcmp(head, "XBEH", 4)) {
        uint32_t base, cert;
        memcpy(&base, head + 0x104, 4);
        memcpy(&cert, head + 0x118, 4);
        if (cert > base && fseek(f, (long)(cert - base + 0xC), SEEK_SET) == 0 &&
            fread(name, 1, sizeof(name), f) == sizeof(name)) {
            ours = true;                    /* Match the full UTF-16LE title and terminator. */
            for (size_t i = 0; i <= sizeof(TITLE) - 1 && ours; i++) {
                ours = name[2 * i] == (uint8_t)TITLE[i] && name[2 * i + 1] == 0;
            }
        }
    }
    fclose(f);
    return ours;
}

static bool same_as(const char *path, const uint8_t *xbe, size_t size)
{
    struct stat st;
    if (stat(path, &st) != 0 || (uint64_t)st.st_size != size) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[65536];
    size_t off = 0;
    bool same = true;
    while (same && off < size) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        same = n > 0 && off + n <= size && !memcmp(buf, xbe + off, n);
        off += n;
    }
    fclose(f);
    return same && off == size;
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Publish without replacing an existing file when hard links are available.
 * Otherwise check the destination before renaming. */
static bool publish_new(const char *tmp, const char *path)
{
    struct stat st;
    if (link(tmp, path) == 0) {
        unlink(tmp);
        return true;
    }
    if (errno != EEXIST && lstat(path, &st) != 0 && rename(tmp, path) == 0) {
        return true;
    }
    unlink(tmp);
    return false;
}

/* Stage each write separately. For an update, check the old inode before
 * replacing the file. */
static bool write_xbe(const char *path, const uint8_t *xbe, size_t size, ino_t old)
{
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    const int fd = mkstemp(tmp);
    if (fd < 0) {
        nc_log("hakuX-native", "dash: can't write %s: %s", path, strerror(errno));
        return false;
    }
    bool ok = fchmod(fd, 0644) == 0 && write_all(fd, xbe, size) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        nc_log("hakuX-native", "dash: can't write %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (!old) {
        return publish_new(tmp, path);
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_ino != old || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* Match a file or directory case-insensitively, as the guest does. */
static bool find(const char *dir, const char *name, bool want_dir, char *out, size_t n)
{
    DIR *d = opendir(dir);
    bool found = false;
    for (struct dirent *e; d && !found && (e = readdir(d));) {
        struct stat st;
        snprintf(out, n, "%s/%s", dir, e->d_name);
        found = !strcasecmp(e->d_name, name) && stat(out, &st) == 0 &&
                (want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
    }
    if (d) {
        closedir(d);
    }
    return found;
}

static bool subdir(const char *dir, const char *name, char *out, size_t n)
{
    if (find(dir, name, true, out, n)) {
        return true;
    }
    snprintf(out, n, "%s/%s", dir, name);
    return mkdir(out, 0775) == 0 || errno == EEXIST;
}

/* Replace the XBE in place, retaining its filename casing. */
static void place(const char *dir, const char *name, const uint8_t *xbe, size_t size, bool *ok)
{
    char host[600];
    const bool have = find(dir, name, false, host, sizeof(host));
    if (have && same_as(host, xbe, size)) {
        return;
    }
    const bool other = have && !nc_dash_is_ours(host);
    struct stat st;
    const ino_t old = have && stat(host, &st) == 0 ? st.st_ino : 0;
    if (!have) {
        snprintf(host, sizeof(host), "%s/%s", dir, name);
    }
    if (write_xbe(host, xbe, size, old)) {
        if (other) {
            nc_log("hakuX-native", "dash: replaced another dashboard with hakuX-lr's at %s", host);
        } else {
            nc_log("hakuX-native", "dash: %s hakuX-lr's dashboard as %s", have ? "updated" : "installed", host);
        }
    }
    *ok = stat(host, &st) == 0;             /* Another boot may have published it. */
}

static bool mkdirs(const char *path)
{
    char p[600];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0775) != 0 && errno != EEXIST) {
                return false;
            }
            *s = '/';
        }
    }
    return mkdir(p, 0775) == 0 || errno == EEXIST;
}

bool nc_dash_install(const char *hdd)
{
    /* Let tests keep their own dashboard XBEs. */
    const char *keep = getenv("NC_DASH_KEEP");
    if (keep && *keep && strcmp(keep, "0") != 0) {
        return true;
    }
    size_t size;
    const uint8_t *xbe = nc_dash_xbe(&size);
    char c_drive[600], online[600];
    snprintf(c_drive, sizeof(c_drive), "%s/Partition2", hdd);
    if (!mkdirs(c_drive)) {
        nc_log("hakuX-native", "dash: no C: drive at %s: %s", c_drive, strerror(errno));
        return false;
    }
    bool ok = true;
    place(c_drive, "xboxdash.xbe", xbe, size, &ok);
    if (ok && subdir(c_drive, "xodash", online, sizeof(online))) {
        bool online_ok = true;
        place(online, "xonlinedash.xbe", xbe, size, &online_ok);
    }
    return ok;
}
