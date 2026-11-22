/*
 * Save the title's Xbox cache partitions (X/Y/Z, Partition3–5) along with the Partition0
 * cache ownership table. The console reuses the least recently used cache partition, so a
 * different title can replace it between save and load. Include the utility drive and any
 * cache drive with an open file. Restore them before reopening files in FILE.
 *
 * Store each file's content once per game under blobs/, named by its XXH3-128 hash. New blobs
 * use level-1 gzip at <hash>.gz. Older raw blobs remain readable. The state lists the files
 * and hashes.
 *
 * Treat a cached file as unchanged only when its size, mtime and ctime match the snapshot. A
 * title can preserve mtime while rewriting the file at the same size. ctime catches those
 * writes and time changes.
 */
#include "native/core/xdvdfs.h"
#include "native/kernel/kernel.h"
#include "native/state/save.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

typedef struct NcCacheState {
    uint32_t nparts;
} NcCacheState;

typedef struct NcCachePart {
    uint32_t part, raw_len, ndirs, nfiles;
} NcCachePart;

typedef struct NcCacheDir {
    char path[256];
} NcCacheDir;

typedef struct NcCacheFile {
    char path[256];
    uint64_t size, mtime_ns;
    uint8_t hash[16];
    uint32_t disc_sector, disc_size;    /* its content is the disc's from there (0: stored whole) */
} NcCacheFile;

#define NC_DEF_SECTION CACHE
#define NC_DEF_FILE "native/state/cache.def"
#include "native/state/def.h"
#include "native/state/sections.h"

#define E_CACHE "CACHE"             /* partition 1's folder of cache files */
#define RAW_MAX 0x80000u    /* io.c RAW_KEEP: the partition head a raw file holds */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------- the blob store */

/* blobs/index: what each cache file hashed to, at its size and time (a save
 * reads only the files that changed since) */
typedef struct IndexEntry {
    char key[264];          /* "<part>/<path>" */
    uint64_t size, mtime_ns, ctime_ns;
    uint8_t hash[16];
} IndexEntry;

#define INDEX_MAGIC 0x32495848u     /* "HXI2"; then sizeof(IndexEntry); an index otherwise is rebuilt */

static IndexEntry *g_idx;
static unsigned g_nidx, g_capidx;

static void blob_dir(char *out, size_t n)
{
    snprintf(out, n, "%s", nc_state_path());
    char *slash = strrchr(out, '/');
    snprintf(slash ? slash + 1 : out, n - (size_t)(slash ? slash + 1 - out : 0), "blobs");
}

static void hex(const uint8_t h[16], char out[33])
{
    for (unsigned i = 0; i < 16; i++) {
        snprintf(out + 2 * i, 3, "%02x", h[i]);
    }
}

static void blob_path(const char *bdir, const uint8_t h[16], bool gz, char *out, size_t n)
{
    char x[33];
    hex(h, x);
    snprintf(out, n, "%s/%s%s", bdir, x, gz ? ".gz" : "");
}

/* A gzip file's last word: its content's size (mod 2^32). */
static bool gz_size_is(const char *p, uint64_t size)
{
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat sb;
    uint8_t t[4];
    const bool ok = fstat(fd, &sb) == 0 && sb.st_size >= 18 && pread(fd, t, 4, sb.st_size - 4) == 4 &&
                    (uint32_t)(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24) == (uint32_t)size;
    close(fd);
    return ok;
}

enum { BLOB_NONE, BLOB_RAW, BLOB_GZ, BLOB_DISC };

/* A file copied from the disc, as the store keeps it (<hash>.dz, gzipped):
 * this head, then for each DZ_CHUNK of the file a byte: 0, the disc's bytes
 * there (the disc file's first sector plus the offset), or 1 and the chunk's
 * own bytes (what the title hasn't copied yet, or changed). */
#define DZ_MAGIC 0x315A4448u        /* "HDZ1" */
#define DZ_CHUNK 65536u
typedef struct DzHead {
    uint32_t magic, sector, disc_size, pad;
    uint64_t size;
} DzHead;

static void dz_path(const char *bdir, const uint8_t h[16], char *out, size_t n)
{
    char x[33];
    hex(h, x);
    snprintf(out, n, "%s/%s.dz", bdir, x);
}

/* The store's disc copy of content h (size bytes), if it has one: its head. */
static bool dz_find(const char *bdir, const uint8_t h[16], uint64_t size, char *out, size_t n, DzHead *head)
{
    dz_path(bdir, h, out, n);
    gzFile gz = gzopen(out, "rb");
    const bool ok = gz && gzread(gz, head, sizeof(*head)) == (int)sizeof(*head) && head->magic == DZ_MAGIC &&
                    head->size == size && head->sector;
    if (gz) {
        gzclose(gz);
    }
    return ok;
}

/* The store's copy of content h (size bytes): out its path. A disc copy
 * counts only where the state names its disc file (sector: 0, none). */
static int blob_find(const char *bdir, const uint8_t h[16], uint64_t size, char *out, size_t n)
{
    struct stat sb;
    blob_path(bdir, h, true, out, n);
    if (gz_size_is(out, size)) {
        return BLOB_GZ;
    }
    blob_path(bdir, h, false, out, n);
    if (stat(out, &sb) == 0 && (uint64_t)sb.st_size == size) {
        return BLOB_RAW;
    }
    return BLOB_NONE;
}

static void index_load(const char *bdir)
{
    g_nidx = 0;
    char p[700];
    snprintf(p, sizeof(p), "%s/index", bdir);
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        return;
    }
    uint32_t head[2] = {0, 0};
    if (fread(head, sizeof(head), 1, fp) != 1 || head[0] != INDEX_MAGIC || head[1] != sizeof(IndexEntry)) {
        fclose(fp);                     /* another layout: every file is hashed again */
        return;
    }
    IndexEntry e;
    while (fread(&e, sizeof(e), 1, fp) == 1) {
        if (g_nidx == g_capidx) {
            g_capidx = g_capidx ? g_capidx * 2 : 64;
            IndexEntry *q = realloc(g_idx, g_capidx * sizeof(*q));
            if (!q) {
                break;
            }
            g_idx = q;
        }
        e.key[sizeof(e.key) - 1] = 0;
        g_idx[g_nidx++] = e;
    }
    fclose(fp);
}

static void index_save(const char *bdir)
{
    char p[700], tmp[710];
    snprintf(p, sizeof(p), "%s/index", bdir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return;
    }
    const uint32_t head[2] = {INDEX_MAGIC, sizeof(IndexEntry)};
    const bool ok = fwrite(head, sizeof(head), 1, fp) == 1 && fwrite(g_idx, sizeof(IndexEntry), g_nidx, fp) == g_nidx;
    if (fclose(fp) == 0 && ok) {
        rename(tmp, p);
    } else {
        unlink(tmp);
    }
}

static IndexEntry *index_find(const char *key)
{
    for (unsigned i = 0; i < g_nidx; i++) {
        if (!strcmp(g_idx[i].key, key)) {
            return &g_idx[i];
        }
    }
    return NULL;
}

static void index_set(const char *key, uint64_t size, uint64_t mtime, uint64_t ctime, const uint8_t hash[16])
{
    IndexEntry *e = index_find(key);
    if (!e) {
        if (g_nidx == g_capidx) {
            g_capidx = g_capidx ? g_capidx * 2 : 64;
            IndexEntry *q = realloc(g_idx, g_capidx * sizeof(*q));
            if (!q) {
                return;
            }
            g_idx = q;
        }
        e = &g_idx[g_nidx++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->size = size;
    e->mtime_ns = mtime;
    e->ctime_ns = ctime;
    memcpy(e->hash, hash, 16);
}

static bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *q = p;
    while (n) {
        const ssize_t w = write(fd, q, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        q += w;
        n -= (size_t)w;
    }
    return true;
}

#define CHUNK (1u << 20)

/* A file gzipped on PZ_THREADS threads, PZ_PIECE bytes each, as pigz does:
 * each piece raw deflate primed with the 32 KiB before it and ended on a
 * byte boundary (a sync flush, the last on the final block), the pieces one
 * deflate stream in one gzip member (CRCs combined): any gunzip reads it.
 * On one thread Halo 2's 720 MiB of new cache took 9.2 s of a 9.6 s save. */
#define PZ_THREADS 4
#define PZ_PIECE (4u << 20)
#define PZ_DICT 32768u

typedef struct PzPiece {
    const uint8_t *in, *dict;
    size_t len, dict_len;
    bool last;
    uint8_t *out;
    size_t out_len, out_cap;
    uLong crc;
    bool ok;
    pthread_t th;
    bool started;
} PzPiece;

static void *pz_piece(void *arg)
{
    PzPiece *p = arg;
    z_stream s;
    memset(&s, 0, sizeof(s));
    p->ok = false;
    if (deflateInit2(&s, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    const size_t need = deflateBound(&s, p->len) + 16;
    if (need > p->out_cap) {
        free(p->out);
        p->out = malloc(need);
        p->out_cap = p->out ? need : 0;
    }
    if (p->out && (!p->dict_len || deflateSetDictionary(&s, p->dict, (uInt)p->dict_len) == Z_OK)) {
        s.next_in = (Bytef *)p->in;
        s.avail_in = (uInt)p->len;
        s.next_out = p->out;
        s.avail_out = (uInt)p->out_cap;
        const int r = deflate(&s, p->last ? Z_FINISH : Z_SYNC_FLUSH);
        p->ok = (p->last ? r == Z_STREAM_END : r == Z_OK) && s.avail_in == 0;
        p->out_len = p->out_cap - s.avail_out;
    }
    deflateEnd(&s);
    p->crc = crc32(0, p->in, (uInt)p->len);
    return NULL;
}

/* src (size bytes) to the gzip file out, hashed meanwhile (XXH3-128 into st) */
static bool pz_file(int in, uint64_t size, int out, XXH3_state_t *st)
{
    static const uint8_t head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 3};
    uint8_t *buf = malloc(PZ_DICT + (size_t)PZ_THREADS * PZ_PIECE);
    PzPiece pc[PZ_THREADS];
    memset(pc, 0, sizeof(pc));
    bool ok = buf && write_all(out, head, sizeof(head));
    uLong crc = crc32(0, NULL, 0);
    uint64_t done = 0;
    size_t dict = 0;                            /* the last round's tail, at buf */
    while (ok) {
        /* a round: up to PZ_THREADS pieces after the dictionary bytes */
        uint8_t *data = buf + PZ_DICT;
        const uint64_t left = size - done, round = (uint64_t)PZ_THREADS * PZ_PIECE;
        size_t have = 0, want = (size_t)(left < round ? left : round);
        while (ok && have < want) {
            const ssize_t r = read(in, data + have, want - have);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            ok = r > 0;
            have += r > 0 ? (size_t)r : 0;
        }
        if (!ok) {
            break;
        }
        XXH3_128bits_update(st, data, have);
        const bool final = done + have == size;
        unsigned n = 0;
        for (size_t at = 0; at < have || (!n && final); at += PZ_PIECE, n++) {
            PzPiece *p = &pc[n];
            p->in = data + at;
            p->len = have - at < PZ_PIECE ? have - at : PZ_PIECE;
            p->dict_len = at ? PZ_DICT : dict;
            p->dict = p->in - p->dict_len;
            p->last = final && at + p->len == have;
            p->started = n && pthread_create(&p->th, NULL, pz_piece, p) == 0;
        }
        for (unsigned i = 0; i < n; i++) {
            if (!i || !pc[i].started) {
                pz_piece(&pc[i]);
            }
        }
        for (unsigned i = 0; i < n; i++) {
            if (pc[i].started) {
                pthread_join(pc[i].th, NULL);
            }
            ok = ok && pc[i].ok && write_all(out, pc[i].out, pc[i].out_len);
            crc = crc32_combine(crc, pc[i].crc, (z_off_t)pc[i].len);
        }
        done += have;
        if (final) {
            break;
        }
        dict = have < PZ_DICT ? have : PZ_DICT;
        memmove(buf + PZ_DICT - dict, data + have - dict, dict);
    }
    if (ok) {
        const uint8_t tail[8] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
                                 (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24)};
        ok = write_all(out, tail, sizeof(tail));
    }
    for (unsigned i = 0; i < PZ_THREADS; i++) {
        free(pc[i].out);
    }
    free(buf);
    return ok;
}

/* src hashed, and its bytes stored as a gzip blob unless that content is
 * there; *stored, *packed: bytes new to the store, as read and as written. */
static bool store_file(const char *src, const char *bdir, uint8_t hash[16], uint64_t *stored, uint64_t *packed)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", bdir);
    const int in = open(src, O_RDONLY | O_CLOEXEC);
    const int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    struct stat isb;
    XXH3_state_t *st = XXH3_createState();
    bool ok = in >= 0 && out >= 0 && fstat(in, &isb) == 0 && st && XXH3_128bits_reset(st) == XXH_OK &&
              pz_file(in, (uint64_t)isb.st_size, out, st);
    const uint64_t n = ok ? (uint64_t)isb.st_size : 0;
    if (ok) {
        XXH128_canonical_t c;
        XXH128_canonicalFromHash(&c, XXH3_128bits_digest(st));
        memcpy(hash, c.digest, 16);
    }
    if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        ok = close(out) == 0 && ok;
    }
    if (st) {
        XXH3_freeState(st);
    }
    char final[700];
    struct stat sb;
    if (ok && blob_find(bdir, hash, n, final, sizeof(final)) != BLOB_NONE) {
        unlink(tmp);                        /* that content is stored already */
    } else if (ok) {
        blob_path(bdir, hash, true, final, sizeof(final));
        *packed += stat(tmp, &sb) == 0 ? (uint64_t)sb.st_size : 0;
        ok = rename(tmp, final) == 0;
        *stored += n;
    } else {
        unlink(tmp);
    }
    return ok;
}

/* The store's copy of the state's file f: a disc copy where the state names
 * its disc file, and the store's is of that one. */
static bool dz_find(const char *bdir, const uint8_t h[16], uint64_t size, char *out, size_t n, DzHead *head);

static int file_blob(const char *bdir, const NcCacheFile *f, char *out, size_t n)
{
    if (!f->disc_sector) {
        return blob_find(bdir, f->hash, f->size, out, n);
    }
    DzHead dh;
    return dz_find(bdir, f->hash, f->size, out, n, &dh) && dh.sector == f->disc_sector &&
           dh.disc_size == f->disc_size ? BLOB_DISC : BLOB_NONE;
}

/* ---------------------------------------------------------- disc copies */

typedef struct NameFind {
    const NcDisc *d;
    const char *name;
    size_t len;
    uint64_t size;
    NcDiscFile out;
    bool found;
    unsigned depth, dirs;
} NameFind;

static bool name_dir(NameFind *nf, uint32_t sector, uint32_t size);

static bool name_entry(void *ctx, const uint8_t *e, uint32_t off)
{
    NameFind *nf = ctx;
    uint32_t sector, size;
    memcpy(&sector, e + 4, 4);
    memcpy(&size, e + 8, 4);
    if (e[12] & 0x10) {
        if (nf->depth < 8 && nf->dirs < 4096) {
            nf->depth++;
            name_dir(nf, sector, size);
            nf->depth--;
        }
    } else if (e[13] == nf->len && !strncasecmp((const char *)e + 14, nf->name, nf->len) && size >= DZ_CHUNK) {
        /* the nearest in size: a title's copy is the file whole, or its start */
        const uint64_t d = size > nf->size ? size - nf->size : nf->size - size;
        const uint64_t best = !nf->found ? UINT64_MAX
                              : nf->out.size > nf->size ? nf->out.size - nf->size : nf->size - nf->out.size;
        if (d < best) {
            nf->out = (NcDiscFile){sector, size, e[12]};
            nf->found = true;
        }
    }
    return true;
}

static bool name_dir(NameFind *nf, uint32_t sector, uint32_t size)
{
    if (!size || size > (4u << 20)) {
        return true;
    }
    uint8_t *dir = malloc(size);
    const NcDiscFile df = {sector, size, 0x10};
    nf->dirs++;
    if (dir && nc_disc_read_orig(nf->d, &df, 0, dir, size)) {
        nc_xdvdfs_walk(dir, size, name_entry, nf);
    }
    free(dir);
    return true;
}

/* The disc's file named as the cache file at path is (any folder), nearest
 * to its size. */
static bool disc_named(const NcDisc *d, const char *path, uint64_t size, NcDiscFile *out)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    NameFind nf = {d, base, strlen(base), size, {0}, false, 0, 0};
    if (!d || !nf.len || nf.len > 255) {
        return false;
    }
    name_dir(&nf, d->root_sector, d->root_size);
    *out = nf.out;
    return nf.found;
}

/* src against the disc file df, chunk by chunk, into the store as a disc
 * copy: hashed, the chunks that are the disc's left out. False (nothing
 * stored) where none is, or it can't be read or written. */
static bool store_disc(const char *src, const char *bdir, const NcDisc *d, const NcDiscFile *df, uint8_t hash[16],
                       uint64_t *same)
{
    char tmp[700], final[700];
    snprintf(tmp, sizeof(tmp), "%s/.tmp-dz", bdir);
    const int in = open(src, O_RDONLY | O_CLOEXEC);
    struct stat isb;
    gzFile gz = in >= 0 && fstat(in, &isb) == 0 ? gzopen(tmp, "wb1") : NULL;
    uint8_t *a = malloc(DZ_CHUNK), *b = malloc(DZ_CHUNK);
    XXH3_state_t *st = XXH3_createState();
    bool ok = gz && a && b && st && XXH3_128bits_reset(st) == XXH_OK;
    const uint64_t size = ok ? (uint64_t)isb.st_size : 0;
    const DzHead head = {DZ_MAGIC, df->sector, df->size, 0, size};
    ok = ok && gzwrite(gz, &head, sizeof(head)) == (int)sizeof(head);
    uint64_t matched = 0;
    for (uint64_t off = 0; ok && off < size; off += DZ_CHUNK) {
        const size_t n = (size_t)(size - off < DZ_CHUNK ? size - off : DZ_CHUNK);
        size_t have = 0;
        while (have < n) {
            const ssize_t r = read(in, a + have, n - have);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                break;
            }
            have += (size_t)r;
        }
        ok = have == n;
        if (!ok) {
            break;
        }
        XXH3_128bits_update(st, a, n);
        const bool disc = off + n <= df->size && nc_disc_read_orig(d, df, off, b, n) && !memcmp(a, b, n);
        const uint8_t flag = disc ? 0 : 1;
        ok = gzwrite(gz, &flag, 1) == 1 && (disc || gzwrite(gz, a, (unsigned)n) == (int)n);
        matched += disc ? n : 0;
    }
    if (gz) {
        ok = gzclose(gz) == Z_OK && ok;
    }
    if (in >= 0) {
        close(in);
    }
    if (ok && matched * 2 >= size) {         /* mostly the disc's: else stored whole */
        XXH128_canonical_t c;
        XXH128_canonicalFromHash(&c, XXH3_128bits_digest(st));
        memcpy(hash, c.digest, 16);
        dz_path(bdir, hash, final, sizeof(final));
        ok = rename(tmp, final) == 0;
    } else {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
    }
    if (st) {
        XXH3_freeState(st);
    }
    free(a);
    free(b);
    *same = ok ? matched : 0;
    return ok;
}

/* A disc copy (the store's blob at src, of the disc file its head names)
 * read back: each chunk to fn. False if it, or the disc, can't be read. */
static bool dz_read(const char *src, const NcDisc *d, uint64_t size, bool (*fn)(void *ctx, const void *p, size_t n),
                    void *ctx)
{
    gzFile gz = gzopen(src, "rb");
    DzHead head;
    uint8_t *a = malloc(DZ_CHUNK);
    bool ok = gz && a && d && gzbuffer(gz, 256u << 10) == 0 && gzread(gz, &head, sizeof(head)) == (int)sizeof(head) &&
              head.magic == DZ_MAGIC && head.size == size;
    const NcDiscFile df = {ok ? head.sector : 0, ok ? head.disc_size : 0, 0};
    for (uint64_t off = 0; ok && off < size; off += DZ_CHUNK) {
        const size_t n = (size_t)(size - off < DZ_CHUNK ? size - off : DZ_CHUNK);
        uint8_t flag = 2;
        ok = gzread(gz, &flag, 1) == 1 && flag <= 1 &&
             (flag ? gzread(gz, a, (unsigned)n) == (int)n : off + n <= df.size && nc_disc_read_orig(d, &df, off, a, n)) &&
             fn(ctx, a, n);
    }
    if (gz) {
        gzclose(gz);
    }
    free(a);
    return ok;
}

static bool dz_to_fd(void *ctx, const void *p, size_t n)
{
    return write_all(*(int *)ctx, p, n);
}

/* Blob src (BLOB_RAW, BLOB_GZ: its CRC and length checked; BLOB_DISC: with
 * the disc's bytes) to dst, which gets size bytes. */
static bool copy_blob(const char *src, int kind, uint64_t size, const char *dst)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.state-tmp", dst);
    if (kind == BLOB_DISC) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
        bool done = fd >= 0 && dz_read(src, nc_state_disc(), size, dz_to_fd, &fd);
        if (fd >= 0) {
            done = close(fd) == 0 && done;
        }
        done = done && rename(tmp, dst) == 0;
        if (!done) {
            unlink(tmp);
        }
        return done;
    }
    const int in = open(src, O_RDONLY | O_CLOEXEC);
    gzFile gz = in >= 0 && kind == BLOB_GZ ? gzdopen(in, "rb") : NULL;   /* closes in */
    const int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664) : -1;
    uint8_t *buf = malloc(CHUNK);
    bool ok = in >= 0 && (kind != BLOB_GZ || (gz && gzbuffer(gz, 256u << 10) == 0)) && out >= 0 && buf;
    uint64_t n = 0;
    while (ok) {
        const ssize_t r = gz ? gzread(gz, buf, CHUNK) : read(in, buf, CHUNK);
        if (r < 0 && !gz && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            ok = r == 0;
            break;
        }
        ok = write_all(out, buf, (size_t)r);
        n += (uint64_t)r;
    }
    if (gz) {
        ok = gzclose(gz) == Z_OK && ok;
    } else if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        ok = close(out) == 0 && ok;
    }
    free(buf);
    ok = ok && n == size && rename(tmp, dst) == 0;
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

/* ------------------------------------------------------------ the drives */

typedef struct Walk {
    NcCacheDir *dirs;
    unsigned ndirs, capd;
    NcCacheFile *files;
    uint64_t *ctime;        /* per file: its change time (not stored in the state) */
    unsigned nfiles, capf;
    bool fail;              /* out of memory: the lists are incomplete */
} Walk;

static uint64_t ts_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

/* root's folders and files below rel, in pre-order (a folder before what's in it) */
static void walk(const char *root, const char *rel, Walk *w)
{
    char path[700];
    snprintf(path, sizeof(path), "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    struct dirent *de;
    while (!w->fail && (de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char sub[256];
        if (snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name) >= (int)sizeof(sub)) {
            continue;
        }
        char full[960];
        snprintf(full, sizeof(full), "%s/%s", root, sub);
        struct stat sb;
        if (lstat(full, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            if (w->ndirs == w->capd) {
                const unsigned cap = w->capd ? w->capd * 2 : 16;
                NcCacheDir *dirs = realloc(w->dirs, cap * sizeof(*dirs));
                if (!dirs) {
                    w->fail = true;
                    break;
                }
                w->dirs = dirs;
                w->capd = cap;
            }
            snprintf(w->dirs[w->ndirs++].path, 256, "%s", sub);
            walk(root, sub, w);
        } else if (S_ISREG(sb.st_mode)) {
            if (w->nfiles == w->capf) {
                const unsigned cap = w->capf ? w->capf * 2 : 32;
                NcCacheFile *files = realloc(w->files, cap * sizeof(*files));
                if (files) {
                    w->files = files;
                }
                uint64_t *ctime = files ? realloc(w->ctime, cap * sizeof(*ctime)) : NULL;
                if (!ctime) {
                    w->fail = true;
                    break;
                }
                w->ctime = ctime;
                w->capf = cap;
            }
            w->ctime[w->nfiles] = ts_ns(&sb.st_ctim);
            NcCacheFile *f = &w->files[w->nfiles++];
            memset(f, 0, sizeof(*f));
            snprintf(f->path, sizeof(f->path), "%s", sub);
            f->size = (uint64_t)sb.st_size;
            f->mtime_ns = ts_ns(&sb.st_mtim);
        }
    }
    closedir(d);
}

static void walk_free(Walk *w)
{
    free(w->dirs);
    free(w->files);
    free(w->ctime);
    memset(w, 0, sizeof(*w));
}

static uint32_t raw_read(int part, uint8_t *buf)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/raw-partition%d.bin", nc_io_hdd_root(), part);
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    const ssize_t n = pread(fd, buf, RAW_MAX, 0);
    close(fd);
    return n > 0 ? (uint32_t)n : 0;
}

static void swap_recover(uint32_t part);

bool nc_cache_state_save(NcBuf *b)
{
    const uint64_t t0 = mono_ns();
    /* partition 0: the cache table; partition 1: its CACHE folder, where the
     * Xbox Live library keeps its own cache files (E:\CACHE\LocalCache*.bin,
     * open and written all session in Burnout 3) */
    struct stat e_cache;
    char e_cache_dir[600];
    swap_recover(1);
    snprintf(e_cache_dir, sizeof(e_cache_dir), "%s/Partition1/" E_CACHE, nc_io_hdd_root());
    const unsigned mask = nc_io_cache_partitions() | 1u |
                          (stat(e_cache_dir, &e_cache) == 0 && S_ISDIR(e_cache.st_mode) ? 2u : 0u);
    char bdir[600];
    blob_dir(bdir, sizeof(bdir));
    mkdir(bdir, 0700);
    index_load(bdir);
    NcCacheState st = {(uint32_t)__builtin_popcount(mask)};
    nc_rec_put(b, &nc_rec_NcCacheState, &st);
    uint8_t *raw = malloc(RAW_MAX);
    bool ok = raw != NULL;
    unsigned files = 0;
    uint64_t bytes = 0, stored = 0, packed = 0, ondisc = 0;
    for (int p = 0; ok && p <= 5; p++) {
        if (!(mask & (1u << p))) {
            continue;
        }
        Walk w = {0};
        char proot[600];
        snprintf(proot, sizeof(proot), "%s/Partition%d", nc_io_hdd_root(), p);
        if (p >= 3 || p == 1) {
            swap_recover((uint32_t)p);          /* (a load cut short: before its files are listed) */
        }
        if (p >= 3) {
            walk(proot, "", &w);
        } else if (p == 1) {
            walk(proot, E_CACHE, &w);       /* that folder only: the rest is the player's saves */
        }
        if (w.fail) {
            nc_log("hakuX-lr-native-state", "cache: out of memory listing %s", proot);
            ok = false;                         /* not a state missing files */
        }
        for (unsigned i = 0; ok && i < w.nfiles; i++) {
            NcCacheFile *f = &w.files[i];
            char key[264], full[960], blob[700];
            snprintf(key, sizeof(key), "%d/%s", p, f->path);
            snprintf(full, sizeof(full), "%s/%s", proot, f->path);
            const IndexEntry *e = index_find(key);
            f->disc_sector = f->disc_size = 0;
            if (e && e->size == f->size && e->mtime_ns == f->mtime_ns && e->ctime_ns == w.ctime[i]) {
                DzHead dh;                          /* unchanged since it was stored */
                if (dz_find(bdir, e->hash, f->size, blob, sizeof(blob), &dh)) {
                    memcpy(f->hash, e->hash, 16);
                    f->disc_sector = dh.sector;
                    f->disc_size = dh.disc_size;
                    ondisc += f->size;
                    continue;
                }
                if (blob_find(bdir, e->hash, f->size, blob, sizeof(blob)) != BLOB_NONE) {
                    memcpy(f->hash, e->hash, 16);
                    continue;
                }
            }
            /* a file the title copied from its disc: the reference, and what differs */
            const NcDisc *disc = nc_state_disc();
            NcDiscFile df;
            uint64_t same = 0;
            if (disc && f->size >= DZ_CHUNK && disc_named(disc, f->path, f->size, &df) &&
                store_disc(full, bdir, disc, &df, f->hash, &same)) {
                f->disc_sector = df.sector;
                f->disc_size = df.size;
                ondisc += f->size;
                index_set(key, f->size, f->mtime_ns, w.ctime[i], f->hash);
                continue;
            }
            ok = store_file(full, bdir, f->hash, &stored, &packed);
            if (ok) {
                index_set(key, f->size, f->mtime_ns, w.ctime[i], f->hash);
            } else {
                nc_log("hakuX-lr-native-state", "cache: %s can't be stored: %s", full, strerror(errno));
            }
        }
        const NcCachePart cp = {(uint32_t)p, p == 1 ? 0 : raw_read(p, raw), w.ndirs, w.nfiles};
        nc_rec_put(b, &nc_rec_NcCachePart, &cp);
        nc_buf_put(b, raw, cp.raw_len);
        nc_recs_put(b, &nc_rec_NcCacheDir, w.dirs, sizeof(NcCacheDir), w.ndirs);
        nc_recs_put(b, &nc_rec_NcCacheFile, w.files, sizeof(NcCacheFile), w.nfiles);
        for (unsigned i = 0; i < w.nfiles; i++) {
            bytes += w.files[i].size;
        }
        files += w.nfiles;
        walk_free(&w);
    }
    free(raw);
    index_save(bdir);
    nc_log("hakuX-lr-native-state",
           "cache: drives %s%s%s, %u files, %.1f MiB (%.1f MiB of files copied from the disc, kept as references; "
           "%.1f MiB new to the store, %.1f MiB gzipped) in %.0f ms",
           mask & 8 ? "X" : "", mask & 16 ? "Y" : "", mask & 32 ? "Z" : "", files, bytes / 1048576.0,
           ondisc / 1048576.0, stored / 1048576.0, packed / 1048576.0, (mono_ns() - t0) / 1e6);
    return ok;
}

static void stage_path(const char *bdir, const uint8_t h[16], char *out, size_t n);
static bool file_as_saved(const char *key, const char *full, const NcCacheFile *f);

/* A load puts the cache drives back as a whole or not at all. Each tree it
 * replaces (a cache partition's folder; on partition 1 its CACHE folder) is
 * built beside the live one, <tree>.state-new: the files the drive already
 * holds as the state has them linked in, the others moved in from where the
 * check staged them (nc_cache_state_check) or written from the store. Only
 * when every tree is built are they swapped in, two renames each
 * (<tree>.state-old in between), and the old ones removed. A failure before
 * the swap leaves the drives as they were; a swap cut short (the process
 * killed) is put right at the next save or load (swap_recover). */
#define TREE_NEW ".state-new"
#define TREE_OLD ".state-old"

static void tree_root(uint32_t part, char *out, size_t n)
{
    if (part == 1) {
        snprintf(out, n, "%s/Partition1/" E_CACHE, nc_io_hdd_root());
    } else {
        snprintf(out, n, "%s/Partition%u", nc_io_hdd_root(), part);
    }
}

/* One of a load's own trees (TREE_NEW, TREE_OLD) and all in it. */
static void tree_remove(const char *path, unsigned depth)
{
    const size_t n = strlen(path);
    if (!depth && (n < sizeof(TREE_NEW) || (strcmp(path + n - (sizeof(TREE_NEW) - 1), TREE_NEW) &&
                                            strcmp(path + n - (sizeof(TREE_OLD) - 1), TREE_OLD)))) {
        return;                             /* nothing else is ever removed whole */
    }
    DIR *d = opendir(path);
    if (d) {
        for (struct dirent *de; (de = readdir(d));) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }
            char sub[1100];
            struct stat sb;
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            if (lstat(sub, &sb) == 0 && S_ISDIR(sb.st_mode) && depth < 32) {
                tree_remove(sub, depth + 1);
            } else {
                unlink(sub);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

/* What a swap cut short left: the old tree back if the new one never took
 * its place, and the leftovers gone. */
static void swap_recover(uint32_t part)
{
    char root[640], side[700];
    struct stat sb;
    tree_root(part, root, sizeof(root));
    snprintf(side, sizeof(side), "%s" TREE_OLD, root);
    if (stat(side, &sb) == 0) {
        if (stat(root, &sb) != 0 && rename(side, root) == 0) {
            nc_log("hakuX-lr-native-state", "cache: %s as before a load that was cut short", root);
        }
        tree_remove(side, 0);
    }
    snprintf(side, sizeof(side), "%s" TREE_NEW, root);
    if (stat(side, &sb) == 0) {
        tree_remove(side, 0);
    }
}

/* A file or folder of the state on its partition, inside the tree: the path
 * under the tree's root (partition 1's are under CACHE/). */
static const char *tree_rel(uint32_t part, const char *path)
{
    return part == 1 ? path + sizeof(E_CACHE) : path;
}

/* The tree of partition p as the state has it, built at <tree>.state-new. */
static bool tree_build(uint32_t p, const NcCacheDir *dirs, unsigned ndirs, const NcCacheFile *files, unsigned nfiles,
                       const char *bdir, uint64_t *copied, uint64_t *moved, uint64_t *kept)
{
    char root[640], neu[700], proot[600];
    tree_root(p, root, sizeof(root));
    snprintf(neu, sizeof(neu), "%s" TREE_NEW, root);
    snprintf(proot, sizeof(proot), "%s/Partition%u", nc_io_hdd_root(), p);
    swap_recover(p);
    mkdir(proot, 0775);
    if (mkdir(neu, 0775) != 0) {
        nc_state_fail("the game's cache can't be written back (%s: %s)", neu, strerror(errno));
        return false;
    }
    for (unsigned i = 0; i < ndirs; i++) {      /* (a folder before what is in it) */
        char full[1000];
        snprintf(full, sizeof(full), "%s/%s", neu, tree_rel(p, dirs[i].path));
        mkdir(full, 0775);
    }
    bool ok = true;
    for (unsigned i = 0; ok && i < nfiles; i++) {
        const NcCacheFile *f = &files[i];
        char key[264], live[960], dst[1000], blob[700], stage[700];
        snprintf(key, sizeof(key), "%u/%s", p, f->path);
        snprintf(live, sizeof(live), "%s/%s", proot, f->path);
        snprintf(dst, sizeof(dst), "%s/%s", neu, tree_rel(p, f->path));
        /* as saved on the drive: the same file under its name in the new tree */
        if (file_as_saved(key, live, f) && link(live, dst) == 0) {
            *kept += f->size;
            continue;
        }
        /* checked and written out before the load (nc_cache_state_check):
         * moved in; else from the store */
        struct stat ssb;
        stage_path(bdir, f->hash, stage, sizeof(stage));
        if (stat(stage, &ssb) == 0 && (uint64_t)ssb.st_size == f->size && rename(stage, dst) == 0) {
            *moved += f->size;
        } else {
            const int kind = file_blob(bdir, f, blob, sizeof(blob));
            if (kind == BLOB_NONE) {
                nc_log("hakuX-lr-native-state", "cache: %s's copy (%s[.gz]) is missing", live, blob);
                nc_state_fail("the state's copy of the game's cache is missing (%s)", f->path);
                ok = false;
                break;
            }
            ok = copy_blob(blob, kind, f->size, dst);
            if (!ok) {
                nc_state_fail("the game's cache file %s can't be written back (%s)", f->path, strerror(errno));
                break;
            }
        }
        const struct timespec ts[2] = {{0, UTIME_OMIT},
                                       {(time_t)(f->mtime_ns / 1000000000ull), (long)(f->mtime_ns % 1000000000ull)}};
        utimensat(AT_FDCWD, dst, ts, 0);
        *copied += f->size;
    }
    return ok;
}

/* A state's file or folder is inside what the load may write: no step out
 * of its partition, and on partition 1 only the CACHE folder's. */
static bool path_ok(const char *path, uint32_t part)
{
    if (!path[0] || path[0] == '/') {
        return false;
    }
    for (const char *p = path; *p;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n || (n == 2 && p[0] == '.' && p[1] == '.') || (n == 1 && p[0] == '.')) {
            return false;
        }
        p += n + (e ? 1 : 0);
    }
    return part != 1 || !strncasecmp(path, E_CACHE "/", sizeof(E_CACHE));
}

/* A partition of the state, read (the load's two passes). */
typedef struct LoadPart {
    NcCachePart cp;
    const void *raw;
    NcCacheDir *dirs;
    NcCacheFile *fl;
    bool tree;              /* its files are restored (a cache drive, partition 1's CACHE) */
    bool built, swapped, had;
} LoadPart;

bool nc_cache_state_load(NcRd *r, unsigned v)
{
    const uint64_t t0 = mono_ns();
    char bdir[600];
    blob_dir(bdir, sizeof(bdir));
    index_load(bdir);
    NcCacheState st = {0};
    nc_rec_get(r, &nc_rec_NcCacheState, &st, v);
    bool ok = !r->fail && st.nparts <= 6;
    uint64_t copied = 0, moved = 0, kept = 0;
    unsigned files = 0, nparts = 0;
    LoadPart lp[6];
    memset(lp, 0, sizeof(lp));
    /* the state's cache, read and checked whole */
    for (uint32_t i = 0; ok && i < st.nparts; i++) {
        LoadPart *q = &lp[nparts++];
        nc_rec_get(r, &nc_rec_NcCachePart, &q->cp, v);
        q->raw = nc_rd_ptr(r, q->cp.raw_len);
        q->dirs = calloc(q->cp.ndirs ? q->cp.ndirs : 1, sizeof(*q->dirs));
        q->fl = calloc(q->cp.nfiles ? q->cp.nfiles : 1, sizeof(*q->fl));
        ok = !r->fail && q->cp.part <= 5 && q->cp.raw_len <= RAW_MAX && q->dirs && q->fl;
        if (!ok) {
            break;
        }
        nc_recs_get(r, &nc_rec_NcCacheDir, q->dirs, sizeof(*q->dirs), q->cp.ndirs, v);
        nc_recs_get(r, &nc_rec_NcCacheFile, q->fl, sizeof(*q->fl), q->cp.nfiles, v);
        ok = !r->fail;
        for (unsigned k = 0; ok && k < q->cp.ndirs; k++) {
            q->dirs[k].path[sizeof(q->dirs[k].path) - 1] = 0;
            ok = path_ok(q->dirs[k].path, q->cp.part);
        }
        for (unsigned k = 0; ok && k < q->cp.nfiles; k++) {
            q->fl[k].path[sizeof(q->fl[k].path) - 1] = 0;
            ok = path_ok(q->fl[k].path, q->cp.part);
        }
        if (!ok && !r->fail) {
            nc_state_fail("the state names a cache file outside the cache drives");
        }
        q->tree = q->cp.part >= 3 || q->cp.part == 1;
        for (unsigned k = 0; ok && k + 1 < nparts; k++) {
            ok = lp[k].cp.part != q->cp.part;       /* (a partition once) */
        }
    }
    /* built beside the live drives: they are as they were until all is */
    for (unsigned i = 0; ok && i < nparts; i++) {
        LoadPart *q = &lp[i];
        if (q->cp.part != 1 && q->cp.raw_len) {
            /* the partition's head (the FATX superblock, the cache table) */
            char p[600];
            snprintf(p, sizeof(p), "%s/raw-partition%u.bin" TREE_NEW, nc_io_hdd_root(), q->cp.part);
            const int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
            ok = fd >= 0 && write_all(fd, q->raw, q->cp.raw_len);
            if (fd >= 0) {
                ok = close(fd) == 0 && ok;
            }
            if (!ok) {
                nc_state_fail("the game's cache can't be written back (%s: %s)", p, strerror(errno));
            }
        }
        if (ok && q->tree) {
            q->built = true;
            ok = tree_build(q->cp.part, q->dirs, q->cp.ndirs, q->fl, q->cp.nfiles, bdir, &copied, &moved, &kept);
            files += q->cp.nfiles;
        }
    }
    /* swapped in, every tree; one that can't be takes the others back */
    for (unsigned i = 0; ok && i < nparts; i++) {
        LoadPart *q = &lp[i];
        char root[640], neu[700], old[700];
        if (!q->tree) {
            continue;
        }
        tree_root(q->cp.part, root, sizeof(root));
        snprintf(neu, sizeof(neu), "%s" TREE_NEW, root);
        snprintf(old, sizeof(old), "%s" TREE_OLD, root);
        q->had = rename(root, old) == 0;
        if (rename(neu, root) == 0) {
            q->swapped = true;
        } else {
            nc_state_fail("the game's cache can't be put in place (%s: %s)", root, strerror(errno));
            if (q->had) {
                rename(old, root);
            }
            ok = false;
        }
    }
    for (unsigned i = 0; i < nparts; i++) {
        LoadPart *q = &lp[i];
        char root[640], side[700], p[600], pn[700];
        tree_root(q->cp.part, root, sizeof(root));
        snprintf(p, sizeof(p), "%s/raw-partition%u.bin", nc_io_hdd_root(), q->cp.part);
        snprintf(pn, sizeof(pn), "%s" TREE_NEW, p);
        if (!ok) {
            /* as before the load: the new trees gone, the swapped ones back */
            if (q->swapped) {
                snprintf(side, sizeof(side), "%s" TREE_NEW, root);
                rename(root, side);
                snprintf(side, sizeof(side), "%s" TREE_OLD, root);
                if (q->had) {
                    rename(side, root);
                }
            }
            if (q->built) {
                snprintf(side, sizeof(side), "%s" TREE_NEW, root);
                tree_remove(side, 0);
            }
            unlink(pn);
        } else {
            if (q->cp.part != 1) {
                if (q->cp.raw_len) {
                    rename(pn, p);
                } else {
                    unlink(p);
                }
            }
            if (q->tree) {
                snprintf(side, sizeof(side), "%s" TREE_OLD, root);
                tree_remove(side, 0);
                /* what the drive's files hold now: the next load leaves the unchanged ones */
                for (unsigned k = 0; k < q->cp.nfiles; k++) {
                    const NcCacheFile *f = &q->fl[k];
                    char key[264], full[960];
                    struct stat now;
                    snprintf(key, sizeof(key), "%u/%s", q->cp.part, f->path);
                    snprintf(full, sizeof(full), "%s/Partition%u/%s", nc_io_hdd_root(), q->cp.part, f->path);
                    if (stat(full, &now) == 0) {
                        index_set(key, f->size, ts_ns(&now.st_mtim), ts_ns(&now.st_ctim), f->hash);
                    }
                }
            }
        }
        free(q->dirs);
        free(q->fl);
    }
    index_save(bdir);
    nc_log("hakuX-lr-native-state", "cache: %u files %s, %.1f MiB written (%.1f MiB of it checked and staged before "
           "the load, moved in), %.1f MiB already on the drives, in %.0f ms", files,
           ok ? "on the drives as saved, swapped in whole" : "not restored: the drives are as before",
           copied / 1048576.0, moved / 1048576.0, kept / 1048576.0, (mono_ns() - t0) / 1e6);
    return ok;
}

/* ------------------------------------------------------------- the store */

typedef struct Refs {
    uint8_t (*h)[16];
    unsigned n, cap;
} Refs;

static bool collect(NcRd *r, unsigned v, void *ctx)
{
    Refs *refs = ctx;
    NcCacheState st = {0};
    nc_rec_get(r, &nc_rec_NcCacheState, &st, v);
    for (uint32_t i = 0; i < st.nparts && !r->fail; i++) {
        NcCachePart cp = {0};
        nc_rec_get(r, &nc_rec_NcCachePart, &cp, v);
        nc_rd_ptr(r, cp.raw_len);
        static NcCacheDir dir;
        for (uint32_t k = 0; k < cp.ndirs && !r->fail; k++) {
            nc_rec_get(r, &nc_rec_NcCacheDir, &dir, v);
        }
        static NcCacheFile f;
        for (uint32_t k = 0; k < cp.nfiles && !r->fail; k++) {
            nc_rec_get(r, &nc_rec_NcCacheFile, &f, v);
            if (refs->n == refs->cap) {
                const unsigned cap = refs->cap ? refs->cap * 2 : 64;
                uint8_t (*h)[16] = realloc(refs->h, cap * 16);
                if (!h) {
                    r->fail = true;             /* no list: no blob is let go */
                    break;
                }
                refs->h = h;
                refs->cap = cap;
            }
            memcpy(refs->h[refs->n++], f.hash, 16);
        }
    }
    return !r->fail;
}

/* ------------------------------------------------------------- the check */

/* The blob at src holds the content hash names (size bytes). */
/* A blob's content to a file and a hash at once. */
typedef struct Tee {
    int fd;                 /* -1: hashed only */
    XXH3_state_t *st;
} Tee;

static bool tee_put(void *ctx, const void *p, size_t n)
{
    Tee *t = ctx;
    return XXH3_128bits_update(t->st, p, n) == XXH_OK && (t->fd < 0 || write_all(t->fd, p, n));
}

/* The blob at src holds the content hash names (size bytes): read once,
 * and written to stage meanwhile (NULL: only read), the file the load then
 * moves into place. A blob that isn't intact leaves no file. */
static bool blob_intact(const char *src, int kind, uint64_t size, const uint8_t hash[16], const char *stage)
{
    Tee t = {stage ? open(stage, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664) : -1, XXH3_createState()};
    bool ok = (!stage || t.fd >= 0) && t.st && XXH3_128bits_reset(t.st) == XXH_OK;
    uint64_t n = 0;
    if (ok && kind == BLOB_DISC) {
        ok = dz_read(src, nc_state_disc(), size, tee_put, &t);
        n = size;
    } else if (ok) {
        const int in = open(src, O_RDONLY | O_CLOEXEC);
        gzFile gz = in >= 0 && kind == BLOB_GZ ? gzdopen(in, "rb") : NULL;   /* closes in */
        uint8_t *buf = malloc(CHUNK);
        ok = in >= 0 && (kind != BLOB_GZ || (gz && gzbuffer(gz, 256u << 10) == 0)) && buf;
        while (ok) {
            const ssize_t r = gz ? gzread(gz, buf, CHUNK) : read(in, buf, CHUNK);
            if (r < 0 && !gz && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                ok = r == 0;
                break;
            }
            ok = tee_put(&t, buf, (size_t)r);
            n += (uint64_t)r;
        }
        if (gz) {
            ok = gzclose(gz) == Z_OK && ok;
        } else if (in >= 0) {
            close(in);
        }
        free(buf);
    }
    if (ok) {
        XXH128_canonical_t c;
        XXH128_canonicalFromHash(&c, XXH3_128bits_digest(t.st));
        ok = n == size && !memcmp(c.digest, hash, 16);
    }
    if (t.fd >= 0) {
        ok = close(t.fd) == 0 && ok;
    }
    if (!ok && stage) {
        unlink(stage);
    }
    XXH3_freeState(t.st);
    return ok;
}

/* The file the check leaves for the load (stage_path): content h, whole. */
static void stage_path(const char *bdir, const uint8_t h[16], char *out, size_t n)
{
    char x[33];
    hex(h, x);
    snprintf(out, n, "%s/%s.staged", bdir, x);
}

typedef struct Check {
    const char *bdir;
    const char *err;
    char why[400];
    unsigned files, there;
    uint64_t bytes;
} Check;

/* The cache file at full is as the state has it (f): the store's index says
 * it held that content, and nothing touched it since (size, times). */
static bool file_as_saved(const char *key, const char *full, const NcCacheFile *f)
{
    const IndexEntry *e = index_find(key);
    struct stat sb;
    return e && !memcmp(e->hash, f->hash, 16) && stat(full, &sb) == 0 && S_ISREG(sb.st_mode) &&
           e->size == (uint64_t)sb.st_size && e->mtime_ns == ts_ns(&sb.st_mtim) && e->ctime_ns == ts_ns(&sb.st_ctim);
}

static bool check(NcRd *r, unsigned v, void *ctx)
{
    Check *c = ctx;
    NcCacheState st = {0};
    nc_rec_get(r, &nc_rec_NcCacheState, &st, v);
    if (r->fail || st.nparts > 6) {
        return false;
    }
    for (uint32_t i = 0; i < st.nparts && !r->fail && !c->err; i++) {
        NcCachePart cp = {0};
        nc_rec_get(r, &nc_rec_NcCachePart, &cp, v);
        if (r->fail || cp.part > 5 || cp.raw_len > RAW_MAX || !nc_rd_ptr(r, cp.raw_len)) {
            return false;
        }
        static NcCacheDir dir;
        for (uint32_t k = 0; k < cp.ndirs && !r->fail; k++) {
            nc_rec_get(r, &nc_rec_NcCacheDir, &dir, v);
        }
        static NcCacheFile f;
        for (uint32_t k = 0; k < cp.nfiles && !r->fail && !c->err; k++) {
            nc_rec_get(r, &nc_rec_NcCacheFile, &f, v);
            f.path[sizeof(f.path) - 1] = 0;
            if (r->fail || (cp.part < 3 && cp.part != 1)) {
                continue;                   /* (only the cache drives' files are restored) */
            }
            if (!path_ok(f.path, cp.part)) {
                snprintf(c->why, sizeof(c->why), "the state names a cache file outside the cache drives");
                c->err = c->why;
                break;
            }
            char src[700], stage[700], key[264], full[960];
            snprintf(key, sizeof(key), "%u/%s", cp.part, (const char *)f.path);
            snprintf(full, sizeof(full), "%s/Partition%u/%s", nc_io_hdd_root(), cp.part, (const char *)f.path);
            c->files++;
            c->bytes += f.size;
            if (file_as_saved(key, full, &f)) {
                c->there++;                 /* on the drive already: the load leaves it */
                continue;
            }
            stage_path(c->bdir, f.hash, stage, sizeof(stage));
            const int kind = file_blob(c->bdir, &f, src, sizeof(src));
            if (kind == BLOB_NONE || !blob_intact(src, kind, f.size, f.hash, stage)) {
                snprintf(c->why, sizeof(c->why), "the state's copy of the cache file %s is %s", (const char *)f.path,
                         kind == BLOB_NONE ? "missing" : "damaged");
                c->err = c->why;
            }
        }
    }
    return !r->fail;
}

const char *nc_cache_state_check(const char *state_path)
{
    static Check c;
    char bdir[600];
    const uint64_t t0 = mono_ns();
    snprintf(bdir, sizeof(bdir), "%s", state_path);
    char *slash = strrchr(bdir, '/');
    snprintf(slash ? slash + 1 : bdir, sizeof(bdir) - (size_t)(slash ? slash + 1 - bdir : 0), "blobs");
    memset(&c, 0, sizeof(c));
    c.bdir = bdir;
    index_load(bdir);
    const char *err = nc_state_read_section(state_path, NC_TAG('C', 'A', 'C', 'H'), check, &c);
    err = c.err ? c.err : err;
    nc_log("hakuX-lr-native-state", "cache check: %u files, %.1f MiB (%u on the drives already) in %.0f ms: %s",
           c.files, c.bytes / 1048576.0, c.there, (mono_ns() - t0) / 1e6,
           err ? err : "intact, staged for the load");
    c.bdir = NULL;
    return err;
}

/* ------------------------------------------------------------- the store */

void nc_cache_gc(const char *state_path)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", state_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return;
    }
    *slash = 0;
    Refs refs = {0};
    DIR *d = opendir(dir);
    struct dirent *de;
    bool ok = d != NULL;
    while (ok && (de = readdir(d))) {
        const size_t len = strlen(de->d_name);
        if (len > 4 && !strcmp(de->d_name + len - 4, ".hxs")) {
            char p[900];
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            /* a state that can't be read keeps every blob (nothing removed) */
            ok = nc_state_read_section(p, NC_TAG('C', 'A', 'C', 'H'), collect, &refs) == NULL;
        }
    }
    if (d) {
        closedir(d);
    }
    char bdir[700];
    snprintf(bdir, sizeof(bdir), "%s/blobs", dir);
    DIR *b = ok ? opendir(bdir) : NULL;
    unsigned removed = 0;
    uint64_t freed = 0;
    while (b && (de = readdir(b))) {
        const size_t len = strlen(de->d_name);
        if (len != 32 && (len != 35 || strcmp(de->d_name + 32, ".gz"))) {
            continue;                       /* <hash>, <hash>.gz only */
        }
        bool used = false;
        for (unsigned i = 0; i < refs.n && !used; i++) {
            char x[33];
            hex(refs.h[i], x);
            used = !memcmp(x, de->d_name, 32);
        }
        if (!used) {
            char p[800];
            snprintf(p, sizeof(p), "%s/%s", bdir, de->d_name);
            struct stat sb;
            freed += stat(p, &sb) == 0 ? (uint64_t)sb.st_size : 0;
            removed += unlink(p) == 0;
        }
    }
    if (b) {
        closedir(b);
    }
    free(refs.h);
    if (removed) {
        nc_log("hakuX-lr-native-state", "cache: %u copies no state uses removed (%.1f MiB)", removed, freed / 1048576.0);
    }
}
