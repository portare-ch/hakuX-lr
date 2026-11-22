/*
 * hakuX-lr libretro core — save states: a state of the path-based engine
 * (native/state) and the cache files it refers to, as one buffer.
 *
 * The engine writes a state file and, beside it, a blobs folder with the
 * Xbox cache drives' files. The buffer holds both, so a state loads in a
 * fresh process with an empty cache folder:
 *
 *   u32 magic "HXLR", u32 version, u64 length (of all that follows),
 *   u32 files, then per file: u16 name length, the name (relative, '/'
 *   between its parts), u64 size, the bytes
 *
 * RetroArch's buffer is retro_serialize_size() long: this state's own
 * length where it was taken for that answer (core.c), and then all of it.
 */
#include "native/libretro/libretro_int.h"

#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LR_STATE_MAGIC   0x524C5848u    /* "HXLR" */
#define LR_STATE_VERSION 1u
#define LR_STATE_HEAD    16u
#define LR_STATE_FILES   65536u        /* a state's files: the engine's, and its distinct cache files */

static char g_why[256];

static const char *why(const char *fmt, const char *a, const char *b)
{
    snprintf(g_why, sizeof(g_why), fmt, a, b);
    return g_why;
}

/* Empties dir of the files the engine and this file put there (the state,
 * the blobs folder's); made if it isn't there. */
bool nc_lr_state_dir(const char *dir)
{
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/blobs", dir);
    if ((mkdir(dir, 0700) != 0 && errno != EEXIST) || (mkdir(sub, 0700) != 0 && errno != EEXIST)) {
        return false;
    }
    const char *const dirs[2] = {dir, sub};
    for (unsigned i = 0; i < 2; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) {
            return false;
        }
        for (const struct dirent *e; (e = readdir(d));) {
            /* (the store's index stays: what the cache drives' files held
             * after the last load, so the next leaves the unchanged ones) */
            if (e->d_type == DT_REG && !(i && !strcmp(e->d_name, "index"))) {
                unlinkat(dirfd(d), e->d_name, 0);
            }
        }
        closedir(d);
    }
    return true;
}

typedef struct Out {
    uint8_t *p;
    size_t len, cap;
    uint64_t need;              /* all of it, also what doesn't fit */
    unsigned files;
    bool full;
} Out;

static void put(Out *o, const void *p, size_t n)
{
    o->need += n;
    if (o->full || o->len > o->cap || n > o->cap - o->len) {
        o->full = true;
        return;
    }
    memcpy(o->p + o->len, p, n);
    o->len += n;
}

/* dir/sub's regular files, named sub/<name> (sub "": dir's own) */
static const char *put_files(Out *o, const char *dir, const char *sub)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s%s%s", dir, sub[0] ? "/" : "", sub);
    DIR *d = opendir(path);
    if (!d) {
        return sub[0] ? NULL : why("%s: %s", path, strerror(errno));    /* a state without cache files */
    }
    const char *err = NULL;
    for (const struct dirent *e; !err && (e = readdir(d));) {
        struct stat st;
        const int fd = e->d_type == DT_REG ? openat(dirfd(d), e->d_name, O_RDONLY | O_CLOEXEC) : -1;
        if (fd < 0) {
            continue;
        }
        if (sub[0] && !strcmp(e->d_name, "index")) {
            close(fd);                  /* the store's notes on this machine's cache files */
            continue;
        }
        char name[512];
        const int n = snprintf(name, sizeof(name), "%s%s%s", sub, sub[0] ? "/" : "", e->d_name);
        if (fstat(fd, &st) != 0 || n <= 0 || n >= (int)sizeof(name)) {
            err = why("%s/%s can't be read", path, e->d_name);
        } else {
            const uint16_t nl = (uint16_t)n;
            const uint64_t size = (uint64_t)st.st_size;
            put(o, &nl, 2);
            put(o, name, nl);
            put(o, &size, 8);
            o->need += size;
            if (!o->full && o->len <= o->cap && size <= o->cap - o->len) {
                size_t got = 0;
                for (ssize_t r; got < size && (r = read(fd, o->p + o->len + got, size - got)) > 0;) {
                    got += (size_t)r;
                }
                if (got != size) {
                    err = why("%s/%s can't be read", path, e->d_name);
                }
                o->len += size;
            } else {
                o->full = true;
            }
            o->files++;
        }
        close(fd);
    }
    closedir(d);
    return err;
}

size_t nc_lr_state_packed(const char *dir)
{
    Out o = {NULL, LR_STATE_HEAD + 4, 0, LR_STATE_HEAD + 4, 0, false};
    const char *err = put_files(&o, dir, "");
    err = err ? err : put_files(&o, dir, "blobs");
    return err ? 0 : (size_t)o.need;
}

const char *nc_lr_state_pack(const char *dir, void *data, size_t size, size_t *used)
{
    if (size < LR_STATE_HEAD + 4) {
        return "the frontend's buffer is too small";
    }
    Out o = {data, LR_STATE_HEAD + 4, size, LR_STATE_HEAD + 4, 0, false};
    const char *err = put_files(&o, dir, "");
    err = err ? err : put_files(&o, dir, "blobs");
    if (err) {
        return err;
    }
    if (o.files > LR_STATE_FILES) {
        /* (what unpack takes: a state it would refuse isn't one saved) */
        snprintf(g_why, sizeof(g_why), "the state has %u cache files, more than the %u a save state may have", o.files - 1,
                 LR_STATE_FILES - 1);
        return g_why;
    }
    if (o.full) {
        snprintf(g_why, sizeof(g_why), "the state takes %.0f MiB, the frontend gave it %.0f MiB. Save again.",
                 (double)o.need / 1048576.0, (double)size / 1048576.0);
        return g_why;
    }
    const uint32_t head[2] = {LR_STATE_MAGIC, LR_STATE_VERSION};
    const uint64_t len = o.len - LR_STATE_HEAD;
    memcpy(o.p, head, 8);
    memcpy(o.p + 8, &len, 8);
    memcpy(o.p + LR_STATE_HEAD, &o.files, 4);
    memset(o.p + o.len, 0, size - o.len);
    *used = o.len;
    return NULL;
}

/* Checks the whole buffer before writing any file. */
const char *nc_lr_state_unpack(const void *data, size_t size, const char *dir)
{
    const uint8_t *p = data;
    uint32_t head[2], files;
    uint64_t len;
    if (size < LR_STATE_HEAD + 4) {
        return "not a hakuX-lr save state";
    }
    memcpy(head, p, 8);
    memcpy(&len, p + 8, 8);
    memcpy(&files, p + LR_STATE_HEAD, 4);
    if (head[0] != LR_STATE_MAGIC) {
        return "not a hakuX-lr save state";
    }
    if (head[1] != LR_STATE_VERSION) {
        return "a save state of another hakuX-lr version (its container)";
    }
    if (len < 4 || len > size - LR_STATE_HEAD || files > LR_STATE_FILES) {
        return "the save state is cut short";
    }
    const size_t end = LR_STATE_HEAD + (size_t)len;
    for (unsigned pass = 0; pass < 2; pass++) {
        size_t at = LR_STATE_HEAD + 4;
        if (pass && !nc_lr_state_dir(dir)) {
            return why("%s: %s", dir, strerror(errno));
        }
        for (uint32_t i = 0; i < files; i++) {
            uint16_t nl;
            uint64_t fs;
            char name[512];
            if (end - at < 2) {
                return "the save state is cut short";
            }
            memcpy(&nl, p + at, 2);
            at += 2;
            if (!nl || nl >= sizeof(name) || end - at < (size_t)nl + 8) {
                return "the save state is cut short";
            }
            memcpy(name, p + at, nl);
            name[nl] = 0;
            at += nl;
            memcpy(&fs, p + at, 8);
            at += 8;
            if (fs > end - at) {
                return "the save state is cut short";
            }
            /* its own files only: a name, or blobs/ and a name */
            const char *base = !strncmp(name, "blobs/", 6) ? name + 6 : name;
            if (!base[0] || strchr(base, '/') || !strcmp(base, ".") || !strcmp(base, "..") ||
                strlen(name) != nl) {
                return "the save state names a file it may not";
            }
            if (pass) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", dir, name);
                const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                size_t done = 0;
                for (ssize_t w; fd >= 0 && done < fs && (w = write(fd, p + at + done, fs - done)) > 0;) {
                    done += (size_t)w;
                }
                const int werr = fd < 0 || done != fs ? errno : 0;
                if (fd >= 0 && close(fd) != 0 && !werr) {
                    return why("%s: %s", path, strerror(errno));
                }
                if (fd >= 0 && werr == EFAULT) {
                    /* RetroArch hands a file cut short over with the length its
                     * own header gives: the bytes aren't all there */
                    nc_lr_state_dir(dir);
                    return "the save state is cut short";
                }
                if (fd < 0 || done != fs) {
                    return why("%s: %s", path, strerror(werr ? werr : EIO));
                }
            }
            at += (size_t)fs;
        }
    }
    return NULL;
}
