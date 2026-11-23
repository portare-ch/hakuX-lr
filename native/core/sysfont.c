/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/core/sysfont.h"
#include "native/core/dash.h"
#include "native/core/nc.h"
#include "native/kernel/xc_crypto.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define HEADERS_MAX (64u << 10)
#define SECTION_MAX (1u << 20)
#define RECORD 132u                     /* a path of 128 bytes, the block count */
#define BLOCK 65536u
#define RUNS_MAX 9                      /* the fonts' entries and the section's digest */

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The digest an XBE keeps of a section and of a file's block: SHA-1 of the
 * length, then the bytes. */
static void digest(const uint8_t *data, uint32_t len, uint8_t out[20])
{
    const uint8_t n[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    NcSha1 s;
    nc_sha1_init(&s);
    nc_sha1_update(&s, n, 4);
    nc_sha1_update(&s, data, len);
    nc_sha1_final(&s, out);
}

/* dir's entry of that name, whatever its case, as the guest finds it. */
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

/* A file table path of a system font: y:\NAME or y:\fonts\NAME (y: is the
 * updater's name for C:). *book: which of the two fonts; host: its file on
 * the hard disk, if it is there. */
static bool font_path(const char *path, const char *hdd, bool *book, char *host, size_t n)
{
    char c_drive[600], fonts[620];
    if (strncasecmp(path, "y:\\", 3)) {
        return false;
    }
    const char *name = path + 3;
    const bool in_fonts = !strncasecmp(name, "fonts\\", 6);
    name += in_fonts ? 6 : 0;
    if (!strcasecmp(name, "XBox Book.xtf")) {
        *book = true;
    } else if (!strcasecmp(name, "Xbox.xtf")) {
        *book = false;
    } else {
        return false;
    }
    snprintf(c_drive, sizeof(c_drive), "%s/Partition2", hdd);
    if (in_fonts && !find(c_drive, "fonts", true, fonts, sizeof(fonts))) {
        return false;
    }
    return find(in_fonts ? fonts : c_drive, name, false, host, n);
}

void nc_sysfont_accept(NcDisc *disc, const char *xbe_path, const char *hdd)
{
    static const char cd[] = "\\Device\\CdRom0\\";
    const char *rel = !strncasecmp(xbe_path, cd, sizeof(cd) - 1)                    ? xbe_path + sizeof(cd) - 1
                      : xbe_path[0] == '\\' && strncasecmp(xbe_path, "\\Device\\", 8) ? xbe_path + 1
                                                                                     : NULL;
    NcDiscFile f;
    uint8_t head[0x180];
    if (!disc || !hdd || !rel || !nc_disc_find(disc, rel, &f) || (f.attr & 0x10) || f.size < sizeof(head) ||
        !nc_disc_read(disc, &f, 0, head, sizeof(head)) || memcmp(head, "XBEH", 4)) {
        return;
    }
    const uint32_t base = rd32(head + 0x104), hsize = rd32(head + 0x108);
    const uint32_t count = rd32(head + 0x11C), table = rd32(head + 0x120) - base;
    if (hsize < sizeof(head) || hsize > HEADERS_MAX || hsize > f.size || count > 512 || table > hsize ||
        (uint64_t)table + (uint64_t)count * 56 > hsize) {
        return;
    }
    uint8_t *headers = malloc(hsize), *data = NULL;
    if (!headers || !nc_disc_read(disc, &f, 0, headers, hsize)) {
        free(headers);
        return;
    }
    uint32_t sh = 0, raw = 0, size = 0;
    for (uint32_t i = 0; i < count && !sh; i++) {
        const uint8_t *s = headers + table + i * 56;
        const uint32_t name = rd32(s + 20) - base;
        if (name < hsize && hsize - name >= 5 && !memcmp(headers + name, "XIPS", 5)) {
            sh = table + i * 56;
            raw = rd32(s + 12);
            size = rd32(s + 16);
        }
    }
    uint8_t was[20];
    if (!sh || size < RECORD || size > SECTION_MAX || (uint64_t)raw + size > f.size || !(data = malloc(size)) ||
        !nc_disc_read(disc, &f, raw, data, size)) {
        goto out;
    }
    digest(data, size, was);
    if (memcmp(was, headers + sh + 36, 20)) {
        nc_log("hakuX-lr-native", "sysfont: %s: its file table doesn't hash as its header says: left as it is", rel);
        goto out;
    }
    NcDiscRun runs[RUNS_MAX];
    static uint8_t bytes[RUNS_MAX][20];
    unsigned n = 0;
    for (uint32_t at = 0; at + RECORD <= size && n < RUNS_MAX - 1;) {
        const uint32_t blocks = rd32(data + at + 128);
        if (!blocks || blocks > (size - at - RECORD) / 20) {
            break;
        }
        char path[129], host[640];
        memcpy(path, data + at, 128);
        path[128] = 0;
        bool book;
        size_t fsize;
        const uint8_t *font;
        if (blocks == 1 && font_path(path, hdd, &book, host, sizeof(host)) && nc_dash_font_is_ours(host, book) &&
            (font = nc_dash_font(book, &fsize))) {
            digest(font, fsize < BLOCK ? (uint32_t)fsize : BLOCK, bytes[n]);
            if (memcmp(bytes[n], data + at + RECORD, 20)) {
                memcpy(data + at + RECORD, bytes[n], 20);
                runs[n] = (NcDiscRun){ (uint64_t)raw + at + RECORD, 20, bytes[n] };
                n++;
                nc_log("hakuX-lr-native", "sysfont: %s takes the open font at %s", rel, path);
            }
        }
        at += RECORD + blocks * 20;
    }
    if (n) {
        digest(data, size, bytes[n]);
        runs[n] = (NcDiscRun){ sh + 36, 20, bytes[n] };
        n++;
        if (!nc_disc_overlay_patch_all(disc, rel, runs, n)) {
            nc_log("hakuX-lr-native", "sysfont: %s: its file table can't be changed: the fonts won't be taken", rel);
        }
    }
out:
    free(data);
    free(headers);
}
