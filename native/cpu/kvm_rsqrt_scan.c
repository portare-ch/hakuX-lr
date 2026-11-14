/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RSQRT sites by recursive descent.
 *
 * Code is decoded only where control flow reaches it: from the entry point
 * and the TLS callbacks, through direct jumps and calls and through switch
 * tables (jmp [reg*4 + table], bounded by the cmp before it). Indirect calls
 * aren't followed, and an XDK title's startup only reaches main through
 * one, so the rest is found from pointer seeds: the immediates and absolute
 * operands of decoded code, and the aligned dwords of the data sections.
 *
 * A seed is only a number that is a code address, so three things keep a
 * wrong one from producing a site:
 *  - It has to land where code can start (plausible_start): after padding,
 *    a ret, an unconditional jmp or a jump table, and never after a prefix
 *    byte (the 0F 52 of an F3 0F 52 is an RSQRTPS that isn't there).
 *  - Each seed starts a family (its region, and what that calls), ranked by
 *    how it was found: the entry 0, an immediate one more than the family
 *    whose code holds it, a data dword DATA_RANK. A decode that runs into a
 *    less trusted family's bytes evicts that family with the families
 *    seeded from its code, and what it had turned away is tried again.
 *  - A site counts only if a linear sweep of the section decodes the same
 *    instruction at the same address.
 */
#include "native/cpu/kvm_rsqrt_scan.h"
#include "native/cpu/x86_decode.h"

#include <stdlib.h>
#include <string.h>

enum { START = 1, INSIDE = 2, TABLE = 3 };
/* a code byte's state: the mark, the walk in progress' own, and whether
 * the address was tried as a root */
#define MARK      0x03u
#define WALK      0x0Cu
#define WALK_SH   2
#define TRIED     0x80u
#define DATA_RANK 1000u
#define TABLE_MAX 1024u

enum { FROM_ENTRY, FROM_IMM, FROM_DATA };

typedef struct Sec {
    NcRsqrtScanSection s;
    uint8_t *state;  /* code: a byte's each */
    int32_t *owner;  /* code: the region that marked it, or -1 */
} Sec;

typedef struct Run {
    uint32_t va, len;
} Run;

typedef struct Site {
    uint32_t va;
    bool swept;
} Site;

typedef struct Item { /* a root waiting, or turned away by a family */
    uint32_t root, rank;
    int32_t fam;     /* trusted: the caller's; a pointer: the family whose code holds it, or -1 */
    uint32_t seq;    /* a pointer's: later ones first within a rank */
    int32_t next;    /* in a family's blocked list */
    uint8_t origin;
    bool trusted;
} Item;

typedef struct Family {
    uint32_t rank, root;
    int32_t region0, region1; /* its regions, a list through Region.next */
    int32_t child0, child1;   /* the families seeded from its code, through Family.sibling */
    int32_t sibling;
    int32_t blocked0, blocked1;
    uint8_t origin;
    bool dead;
} Family;

typedef struct Region {
    uint32_t root;
    int32_t fam, next;
    size_t site0, nsites, range0, nranges;
    bool dead;
} Region;

#define VEC(T) \
    struct { \
        T *p; \
        size_t n, cap; \
    }

typedef struct Scan {
    Sec *secs;
    unsigned nsecs;
    Sec *last;       /* the section of the last address looked up */
    bool overlap;    /* sections overlap: the first one in the header's order counts */
    bool oom;
    uint32_t seq;
    VEC(uint32_t) work, calls, ptrs;
    VEC(int32_t) stack;
    VEC(Run) journal, ranges;
    VEC(Site) sites;
    VEC(Item) trusted, heap, blocked;
    VEC(Family) fams;
    VEC(Region) regions;
    NcRsqrtScan *out;
} Scan;

#define PUSH(v, x) \
    do { \
        if ((v).n == (v).cap) { \
            const size_t cap_ = (v).cap ? (v).cap * 2 : 64; \
            void *p_ = realloc((v).p, cap_ * sizeof(*(v).p)); \
            if (!p_) { \
                sc->oom = true; \
                break; \
            } \
            (v).p = p_; \
            (v).cap = cap_; \
        } \
        (v).p[(v).n++] = (x); \
    } while (0)

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static Sec *sec_of(Scan *sc, uint32_t va)
{
    if (sc->last && va - sc->last->s.va < sc->last->s.size) {
        return sc->last;
    }
    for (unsigned i = 0; i < sc->nsecs; i++) {
        if (va - sc->secs[i].s.va < sc->secs[i].s.size) {
            if (!sc->overlap) {
                sc->last = &sc->secs[i];
            }
            return &sc->secs[i];
        }
    }
    return NULL;
}

static Sec *code_at(Scan *sc, uint32_t va)
{
    Sec *s = sec_of(sc, va);
    return s && s->s.code ? s : NULL;
}

static bool read32(Scan *sc, uint32_t va, uint32_t *v)
{
    const Sec *s = sec_of(sc, va);
    if (!s || s->s.data_size - (va - s->s.va) < 4) {
        return false;
    }
    *v = rd32(s->s.data + (va - s->s.va));
    return true;
}

static bool decode_at(Scan *sc, uint32_t va, NcX86Insn *in)
{
    const Sec *s = sec_of(sc, va);
    if (!s) {
        return false;
    }
    const uint32_t o = va - s->s.va, left = s->s.data_size - o;
    return nc_x86_decode(s->s.data + o, left > 16 ? 16 : left, va, in) && in->len <= s->s.size - o;
}

enum { WALK_OK, WALK_CONFLICT, WALK_REJECT };

/* Decodes by control flow from root, marking the walk's own bits (journal:
 * what it marked) over the marks already there. WALK_CONFLICT: it ran into
 * marked bytes at *at. Anything else that isn't code is WALK_REJECT, as is
 * a strict walk (a pointer seed's) through an instruction compiled code
 * doesn't have. */
static int walk(Scan *sc, uint32_t root, bool strict, uint32_t *at)
{
    sc->work.n = sc->journal.n = sc->calls.n = sc->ptrs.n = 0;
    PUSH(sc->work, root);
    while (sc->work.n && !sc->oom) {
        uint32_t va = sc->work.p[--sc->work.n];
        struct {
            uint8_t reg;
            uint32_t imm;
        } cmp[8]; /* the last instructions of this run: their cmp r32, imm */
        unsigned ncmp = 0;
        for (;;) {
            Sec *s = code_at(sc, va);
            if (!s) {
                break;
            }
            const uint32_t o = va - s->s.va;
            const uint8_t b = s->state[o];
            if ((b & WALK) == START << WALK_SH || (!(b & WALK) && (b & MARK) == START)) {
                break;
            }
            if (b & (WALK | MARK)) {
                *at = va;
                return b & WALK ? WALK_REJECT : WALK_CONFLICT;
            }
            NcX86Insn in;
            if (!decode_at(sc, va, &in)) {
                return WALK_REJECT;
            }
            for (unsigned k = 1; k < in.len; k++) {
                if (s->state[o + k] & (WALK | MARK)) {
                    *at = va + k;
                    return s->state[o + k] & WALK ? WALK_REJECT : WALK_CONFLICT;
                }
            }
            if (strict && in.odd) {
                return WALK_REJECT;
            }
            s->state[o] |= START << WALK_SH;
            for (unsigned k = 1; k < in.len; k++) {
                s->state[o + k] |= INSIDE << WALK_SH;
            }
            PUSH(sc->journal, ((Run){va, in.len}));
            if (in.rsqrt) {
                PUSH(sc->sites, ((Site){va, false}));
            }
            /* what may be the address of code: the next rank's seeds */
            uint32_t v;
            if (in.has_abs && read32(sc, in.abs, &v) && code_at(sc, v)) {
                PUSH(sc->ptrs, v);
            }
            for (unsigned i = 0; i < in.nimm; i++) {
                if (code_at(sc, in.imm[i])) {
                    PUSH(sc->ptrs, in.imm[i]);
                }
            }
            if (in.flow == NC_X86_END || in.flow == NC_X86_JMP_OTHER) {
                break;
            }
            if (in.flow == NC_X86_JMP || in.flow == NC_X86_JCC) {
                PUSH(sc->work, in.imm[0]);
                if (in.flow == NC_X86_JMP) {
                    break;
                }
            } else if (in.flow == NC_X86_JMP_TABLE) {
                uint64_t bound = TABLE_MAX;
                for (unsigned i = 0; i < 8 && i < ncmp; i++) {
                    if (cmp[(ncmp - 1 - i) % 8].reg == in.index + 1) {
                        bound = (uint64_t)cmp[(ncmp - 1 - i) % 8].imm + 1;
                        break;
                    }
                }
                for (uint32_t n = 0; n < TABLE_MAX && n < bound; n++) {
                    const uint32_t entry = in.abs + 4 * n;
                    if (!read32(sc, entry, &v) || !code_at(sc, v)) {
                        break;
                    }
                    PUSH(sc->work, v);
                    for (unsigned k = 0; k < 4; k++) { /* a table among the code is not code */
                        Sec *t = code_at(sc, entry + k);
                        if (t && !(t->state[entry + k - t->s.va] & (WALK | MARK))) {
                            t->state[entry + k - t->s.va] |= TABLE << WALK_SH;
                            PUSH(sc->journal, ((Run){entry + k, 1}));
                        }
                    }
                }
                break;
            } else if (in.flow == NC_X86_CALL) {
                PUSH(sc->calls, in.imm[0]);
            }
            cmp[ncmp % 8].reg = in.cmp_reg;
            cmp[ncmp % 8].imm = in.cmp_imm;
            ncmp++;
            va += in.len;
        }
    }
    return WALK_OK;
}

static void walk_undo(Scan *sc, size_t site0)
{
    for (size_t i = 0; i < sc->journal.n; i++) {
        const Run r = sc->journal.p[i];
        Sec *s = code_at(sc, r.va);
        for (uint32_t k = 0; k < r.len; k++) {
            s->state[r.va - s->s.va + k] &= (uint8_t)~WALK;
        }
    }
    sc->sites.n = site0;
}

static int run_cmp(const void *a, const void *b)
{
    const uint32_t x = ((const Run *)a)->va, y = ((const Run *)b)->va;
    return x < y ? -1 : x > y;
}

/* The walk's marks become region rid's; its ranges are what an eviction
 * unmarks. */
static void walk_commit(Scan *sc, int32_t rid)
{
    Region *r = &sc->regions.p[rid];
    qsort(sc->journal.p, sc->journal.n, sizeof(Run), run_cmp);
    r->range0 = sc->ranges.n;
    for (size_t i = 0; i < sc->journal.n; i++) {
        const Run j = sc->journal.p[i];
        Sec *s = code_at(sc, j.va);
        const uint32_t o = j.va - s->s.va;
        for (uint32_t k = 0; k < j.len; k++) {
            const uint8_t b = s->state[o + k];
            s->state[o + k] = (uint8_t)((b & TRIED) | ((b & WALK) >> WALK_SH));
            s->owner[o + k] = rid;
        }
        Run *lastr = sc->ranges.n > r->range0 ? &sc->ranges.p[sc->ranges.n - 1] : NULL;
        if (lastr && lastr->va + lastr->len == j.va && o) {
            lastr->len += j.len;
        } else {
            PUSH(sc->ranges, j);
        }
    }
    r = &sc->regions.p[rid];
    r->nranges = sc->ranges.n - r->range0;
}

/* Where a pointer may point for code to start there: a code section's
 * start, or right after int3/nop padding, a ret, an unconditional jmp or a
 * jump table. */
static bool plausible_start(Scan *sc, const Sec *s, uint32_t va)
{
    static const struct {
        uint8_t len, b[7];
    } nops[] = { /* the multi-byte nops compilers pad with */
        {3, {0x8D, 0x40, 0x00}}, {3, {0x8D, 0x49, 0x00}}, {3, {0x8D, 0x76, 0x00}},
        {4, {0x8D, 0x64, 0x24, 0x00}}, {4, {0x8D, 0x74, 0x26, 0x00}},
        {6, {0x8D, 0x9B, 0, 0, 0, 0}}, {7, {0x8D, 0xA4, 0x24, 0, 0, 0, 0}}, {6, {0x8D, 0xB6, 0, 0, 0, 0}},
        {6, {0x8D, 0xBF, 0, 0, 0, 0}}, {6, {0x8D, 0x80, 0, 0, 0, 0}},
    };
    const uint32_t o = va - s->s.va;
    const uint8_t *d = s->s.data + o; /* d[-k]: k bytes before */
    if (d[0] == 0x00) {
        return false; /* add [m], r8: zeros, or a misaligned decode */
    }
    if (o == 0) {
        return true;
    }
    switch (d[-1]) {
    case 0xF2: case 0xF3: case 0x66: case 0x67: case 0xF0: case 0x2E: case 0x36: case 0x3E: case 0x26:
    case 0x64: case 0x65:
        return false;
    case 0xCC: case 0x90: case 0xC3:
        return true;
    }
    if ((o >= 2 && (d[-2] == 0xEB || (d[-2] == 0xFF && d[-1] >= 0xE0 && d[-1] <= 0xE7))) || /* jmp rel8, reg */
        (o >= 3 && d[-3] == 0xC2) ||                                                        /* ret imm16 */
        (o >= 5 && d[-5] == 0xE9) ||                                                        /* jmp rel32 */
        (o >= 6 && d[-6] == 0xFF && d[-5] == 0x25) ||                                       /* jmp [abs] */
        (o >= 7 && d[-7] == 0xFF && d[-6] == 0x24 && (d[-5] & 0xC7) == 0x85)) {             /* jmp [table] */
        return true;
    }
    for (unsigned i = 0; i < sizeof(nops) / sizeof(nops[0]); i++) {
        if (o >= nops[i].len && !memcmp(d - nops[i].len, nops[i].b, nops[i].len)) {
            return true;
        }
    }
    /* after a jump table: its last two entries point back into the function before it */
    if (va % 4 == 0 && o >= 8) {
        for (unsigned i = 1; i <= 2; i++) {
            const uint32_t e = rd32(d - 4 * i);
            if (e >= va || va - e >= 0x10000u || !code_at(sc, e)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static int32_t family_new(Scan *sc, uint32_t rank, uint8_t origin, uint32_t root, int32_t parent)
{
    const int32_t fid = (int32_t)sc->fams.n;
    PUSH(sc->fams, ((Family){.rank = rank, .root = root, .origin = origin, .region0 = -1, .region1 = -1,
                             .child0 = -1, .child1 = -1, .sibling = -1, .blocked0 = -1, .blocked1 = -1}));
    if (sc->oom) {
        return -1;
    }
    if (parent >= 0) {
        Family *p = &sc->fams.p[parent];
        if (p->child1 >= 0) {
            sc->fams.p[p->child1].sibling = fid;
        } else {
            p->child0 = fid;
        }
        p->child1 = fid;
    }
    return fid;
}

/* the heap of pointer seeds: the lowest rank first, the latest within it */
static bool seed_before(const Item *a, const Item *b)
{
    return a->rank != b->rank ? a->rank < b->rank : a->seq > b->seq;
}

static void seed_push(Scan *sc, uint32_t va, uint32_t rank, uint8_t origin, int32_t src)
{
    PUSH(sc->heap, ((Item){.root = va, .rank = rank, .fam = src, .seq = ++sc->seq, .next = -1, .origin = origin}));
    if (sc->oom) {
        return;
    }
    Item *h = sc->heap.p;
    size_t i = sc->heap.n - 1;
    const Item x = h[i];
    for (; i && seed_before(&x, &h[(i - 1) / 2]); i = (i - 1) / 2) {
        h[i] = h[(i - 1) / 2];
    }
    h[i] = x;
}

static Item seed_pop(Scan *sc)
{
    Item *h = sc->heap.p;
    const Item top = h[0], x = h[--sc->heap.n];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= sc->heap.n) {
            break;
        }
        c += c + 1 < sc->heap.n && seed_before(&h[c + 1], &h[c]);
        if (!seed_before(&h[c], &x)) {
            break;
        }
        h[i] = h[c];
        i = c;
    }
    h[i] = x;
    return top;
}

static void untry(Scan *sc, uint32_t va)
{
    Sec *s = code_at(sc, va);
    if (s) {
        s->state[va - s->s.va] &= (uint8_t)~TRIED;
    }
}

/* Family fid and the families seeded from its code are given up: their
 * bytes are unmarked, and what they turned away is queued again. */
static void evict(Scan *sc, int32_t fid)
{
    sc->stack.n = 0;
    PUSH(sc->stack, fid);
    while (sc->stack.n && !sc->oom) {
        const int32_t id = sc->stack.p[--sc->stack.n];
        Family *f = &sc->fams.p[id];
        if (f->dead) {
            continue;
        }
        f->dead = true;
        sc->out->evicted++;
        for (int32_t rid = f->region0; rid >= 0; rid = sc->regions.p[rid].next) {
            Region *r = &sc->regions.p[rid];
            r->dead = true;
            for (size_t i = 0; i < r->nranges; i++) {
                const Run g = sc->ranges.p[r->range0 + i];
                Sec *s = code_at(sc, g.va);
                for (uint32_t k = 0; k < g.len; k++) {
                    const uint32_t o = g.va - s->s.va + k;
                    if (s->owner[o] == rid) { /* not a cut table's bytes, decoded since */
                        s->state[o] &= (uint8_t)~MARK;
                        s->owner[o] = -1;
                    }
                }
            }
            untry(sc, r->root);
        }
        for (int32_t c = f->child0; c >= 0; c = sc->fams.p[c].sibling) {
            PUSH(sc->stack, c);
        }
        int32_t b = f->blocked0;
        f->blocked0 = f->blocked1 = -1;
        for (; b >= 0 && !sc->oom; b = sc->blocked.p[b].next) {
            const Item it = sc->blocked.p[b];
            untry(sc, it.root);
            sc->out->requeued++;
            if (it.trusted) {
                Item t = it;
                t.next = -1;
                PUSH(sc->trusted, t);
            } else {
                seed_push(sc, it.root, it.rank, it.origin, it.fam);
            }
        }
    }
}

/* A switch table whose bound ran into code: unmarked from the entry
 * holding offset o on. */
static void table_cut(Scan *sc, Sec *s, uint32_t o, int32_t rid)
{
    uint32_t a = o;
    while (a && (s->state[a - 1] & MARK) == TABLE && s->owner[a - 1] == rid) {
        a--;
    }
    for (uint32_t e = o - (o - a) % 4; e < s->s.size && (s->state[e] & MARK) == TABLE && s->owner[e] == rid; e++) {
        s->state[e] &= (uint8_t)~MARK;
        s->owner[e] = -1;
    }
    sc->out->tables_cut++;
}

/* An item turned away by family fid's code waits for it: queued again
 * should the family be given up (evict). */
static void block(Scan *sc, Item item, int32_t fid)
{
    item.next = -1;
    const int32_t bi = (int32_t)sc->blocked.n;
    PUSH(sc->blocked, item);
    if (!sc->oom) {
        Family *f = &sc->fams.p[fid];
        if (f->blocked1 >= 0) {
            sc->blocked.p[f->blocked1].next = bi;
        } else {
            f->blocked0 = bi;
        }
        f->blocked1 = bi;
    }
}

static void descend(Scan *sc)
{
    while ((sc->trusted.n || sc->heap.n) && !sc->oom) {
        Item item;
        if (sc->trusted.n) {
            item = sc->trusted.p[--sc->trusted.n];
            if (sc->fams.p[item.fam].dead) {
                continue;
            }
            item.rank = sc->fams.p[item.fam].rank;
        } else {
            item = seed_pop(sc);
            if (item.fam >= 0 && sc->fams.p[item.fam].dead) {
                continue;
            }
        }
        const bool strict = !item.trusted;
        const uint32_t root = item.root, rank = item.rank;
        Sec *sec = code_at(sc, root);
        if (!sec) {
            continue; /* not code */
        }
        if (sec->state[root - sec->s.va] & MARK) {
            /* Already code (or a table), but another family's, which may
             * yet be given up and its bytes with it: this reference to them
             * is then the one that has them walked again. The entry's
             * family never is. */
            const int32_t orid = sec->owner[root - sec->s.va];
            const int32_t ofid = orid >= 0 ? sc->regions.p[orid].fam : -1;
            if (ofid >= 0 && ofid != item.fam && sc->fams.p[ofid].origin != FROM_ENTRY) {
                block(sc, item, ofid);
            }
            continue;
        }
        if (sec->state[root - sec->s.va] & TRIED) {
            continue; /* done */
        }
        sec->state[root - sec->s.va] |= TRIED;
        if (strict) {
            sc->out->seeds++;
            if (!plausible_start(sc, sec, root)) {
                sc->out->seeds_not_code_start++;
                continue;
            }
        }
        const size_t site0 = sc->sites.n;
        bool ok = false;
        /* Each further attempt follows a family given up or a table cut,
         * and none is made here: the attempts end. */
        while (!sc->oom) {
            uint32_t at = 0;
            const int rc = walk(sc, root, strict, &at);
            if (rc == WALK_OK && !sc->oom) {
                ok = true;
                break;
            }
            walk_undo(sc, site0);
            if (rc != WALK_CONFLICT) {
                break; /* not code, on its own merits */
            }
            Sec *cs = code_at(sc, at);
            const uint32_t co = at - cs->s.va;
            const int32_t orid = cs->owner[co], ofid = sc->regions.p[orid].fam;
            const Family *of = &sc->fams.p[ofid];
            if ((cs->state[co] & MARK) == TABLE && rank <= of->rank) {
                table_cut(sc, cs, co, orid);
                continue;
            }
            if (rank < of->rank) {
                evict(sc, ofid);
                continue;
            }
            /* a direct call's code over a pointer-seeded family's first
             * instruction: that seed landed inside a real instruction */
            if (!strict && of->origin != FROM_ENTRY && ofid != item.fam) {
                NcX86Insn first;
                const bool have = decode_at(sc, of->root, &first);
                if (at == of->root || (have && at > of->root && at - of->root < first.len)) {
                    sc->out->evicted_straddled++;
                    evict(sc, ofid);
                    continue;
                }
            }
            block(sc, item, ofid);
            break;
        }
        if (!ok) {
            continue;
        }
        int32_t fid = item.fam;
        if (strict) {
            fid = family_new(sc, rank, item.origin, root, item.fam);
            sc->out->seeds_accepted++;
        }
        const int32_t rid = (int32_t)sc->regions.n;
        PUSH(sc->regions, ((Region){.root = root, .fam = fid, .next = -1, .site0 = site0,
                                    .nsites = sc->sites.n - site0}));
        if (sc->oom) {
            break;
        }
        Family *f = &sc->fams.p[fid];
        if (f->region1 >= 0) {
            sc->regions.p[f->region1].next = rid;
        } else {
            f->region0 = rid;
        }
        f->region1 = rid;
        walk_commit(sc, rid);
        /* a function called directly belongs to its caller's family */
        for (size_t i = 0; i < sc->calls.n; i++) {
            PUSH(sc->trusted, ((Item){.root = sc->calls.p[i], .fam = fid, .next = -1, .trusted = true}));
        }
        for (size_t i = 0; i < sc->ptrs.n; i++) {
            seed_push(sc, sc->ptrs.p[i], rank + 1, FROM_IMM, fid);
        }
    }
}

static int site_cmp(const void *a, const void *b)
{
    const uint32_t x = ((const Site *)a)->va, y = ((const Site *)b)->va;
    return x < y ? -1 : x > y;
}

/* Every code section decoded front to back, a byte skipped where nothing
 * decodes: the second opinion on each site. */
static void sweep(Scan *sc, Site *sites, size_t n)
{
    for (unsigned i = 0; i < sc->nsecs; i++) {
        const NcRsqrtScanSection *s = &sc->secs[i].s;
        if (!s->code) {
            continue;
        }
        for (uint32_t o = 0; o < s->size;) {
            NcX86Insn in;
            const uint32_t left = s->data_size - o;
            if (!nc_x86_decode(s->data + o, left > 16 ? 16 : left, s->va + o, &in)) {
                o++;
                continue;
            }
            if (in.rsqrt) {
                const Site key = {s->va + o, false};
                Site *hit = bsearch(&key, sites, n, sizeof(Site), site_cmp);
                if (hit) {
                    hit->swept = true;
                } else {
                    sc->out->unreached++;
                }
            }
            o += in.len;
        }
    }
}

bool nc_rsqrt_scan(const NcRsqrtScanSection *secs, unsigned nsecs, const uint32_t *roots, unsigned nroots,
                   NcRsqrtScan *out)
{
    Scan scan = {.nsecs = nsecs, .out = out}, *sc = &scan;
    memset(out, 0, sizeof(*out));
    sc->secs = calloc(nsecs ? nsecs : 1, sizeof(Sec));
    sc->oom = !sc->secs;
    for (unsigned i = 0; i < nsecs && !sc->oom; i++) {
        Sec *s = &sc->secs[i];
        s->s = secs[i];
        for (unsigned j = 0; j < i; j++) {
            sc->overlap |= s->s.va < secs[j].va + (uint64_t)secs[j].size && secs[j].va < s->s.va + (uint64_t)s->s.size;
        }
        if (s->s.code && s->s.size) {
            s->state = calloc(s->s.size, 1);
            s->owner = malloc((size_t)s->s.size * sizeof(int32_t));
            sc->oom = !s->state || !s->owner;
            if (!sc->oom) {
                memset(s->owner, 0xFF, (size_t)s->s.size * sizeof(int32_t));
            }
        }
    }
    const int32_t entry = sc->oom ? -1 : family_new(sc, 0, FROM_ENTRY, nroots ? roots[0] : 0, -1);
    for (unsigned i = 0; i < nroots; i++) {
        PUSH(sc->trusted, ((Item){.root = roots[i], .fam = entry, .next = -1, .trusted = true}));
    }
    for (unsigned i = 0; i < nsecs && !sc->oom; i++) {
        const NcRsqrtScanSection *s = &secs[i];
        if (!s->seeds) {
            continue;
        }
        for (uint32_t o = 0; o + 4 <= s->data_size && !sc->oom; o += 4) {
            const uint32_t v = rd32(s->data + o);
            if (code_at(sc, v)) {
                seed_push(sc, v, DATA_RANK, FROM_DATA, -1);
            }
        }
    }
    descend(sc);

    /* the sites of the regions kept, as the linear sweep agrees */
    size_t n = 0;
    for (size_t i = 0; i < sc->regions.n; i++) {
        const Region *r = &sc->regions.p[i];
        if (!r->dead) {
            out->regions++;
        }
        if (!r->dead && r->nsites) {
            memmove(sc->sites.p + n, sc->sites.p + r->site0, r->nsites * sizeof(Site));
            n += r->nsites;
        }
    }
    if (!sc->oom && n) {
        qsort(sc->sites.p, n, sizeof(Site), site_cmp);
    }
    if (!sc->oom) {
        sweep(sc, sc->sites.p, n);
        out->sites = malloc((n ? n : 1) * sizeof(uint32_t));
        sc->oom = !out->sites;
    }
    for (size_t i = 0; i < n && !sc->oom; i++) {
        if (sc->sites.p[i].swept) {
            out->sites[out->nsites++] = sc->sites.p[i].va;
        } else {
            out->disputed++;
        }
    }
    for (unsigned i = 0; i < nsecs && sc->secs; i++) {
        free(sc->secs[i].state);
        free(sc->secs[i].owner);
    }
    free(sc->secs);
    free(sc->work.p);
    free(sc->calls.p);
    free(sc->ptrs.p);
    free(sc->stack.p);
    free(sc->journal.p);
    free(sc->ranges.p);
    free(sc->sites.p);
    free(sc->trusted.p);
    free(sc->heap.p);
    free(sc->blocked.p);
    free(sc->fams.p);
    free(sc->regions.p);
    if (sc->oom) {
        nc_rsqrt_scan_free(out);
        return false;
    }
    return true;
}

void nc_rsqrt_scan_free(NcRsqrtScan *scan)
{
    free(scan->sites);
    memset(scan, 0, sizeof(*scan));
}

/* XBE header fields (offsets in the file, addresses from the base) */
#define XBE_BASE     0x104
#define XBE_NSECS    0x11C
#define XBE_SECS     0x120
#define XBE_ENTRY    0x128
#define XBE_TLS      0x12C
#define XBE_SEC_SIZE 0x38
#define XBE_SEC_EXEC 0x04u
#define XBE_MAX_SECS 1024u
#define XBE_MAX_TLS  16u

static bool image_rd32(const NcRsqrtScanSection *secs, unsigned n, uint32_t va, uint32_t *v)
{
    for (unsigned i = 0; i < n; i++) {
        const uint32_t o = va - secs[i].va;
        if (o < secs[i].size) {
            if (secs[i].data_size - o < 4) {
                return false;
            }
            *v = rd32(secs[i].data + o);
            return true;
        }
    }
    return false;
}

static bool image_code(const NcRsqrtScanSection *secs, unsigned n, uint32_t va)
{
    for (unsigned i = 0; i < n; i++) {
        if (va - secs[i].va < secs[i].size) {
            return secs[i].code;
        }
    }
    return false;
}

bool nc_rsqrt_scan_xbe(const uint8_t *xbe, uint32_t size, NcRsqrtScan *out)
{
    /* sections that are executable by their flags, and data by their names */
    static const char *const data_names[] = {".rdata", ".data", ".bss", "DOLBY", ".XTLID", "$$"};
    /* the entry point is stored XORed with the retail or the debug key */
    static const uint32_t entry_keys[] = {0xA8FC57ABu, 0x94859D4Bu, 0};
    memset(out, 0, sizeof(*out));
    if (size < 0x180 || memcmp(xbe, "XBEH", 4)) {
        return false;
    }
    const uint32_t base = rd32(xbe + XBE_BASE), nsecs = rd32(xbe + XBE_NSECS), hdrs = rd32(xbe + XBE_SECS) - base;
    if (nsecs > XBE_MAX_SECS || hdrs > size || (size - hdrs) / XBE_SEC_SIZE < nsecs) {
        return false;
    }
    NcRsqrtScanSection *secs = calloc(nsecs ? nsecs : 1, sizeof(*secs));
    uint8_t **padded = calloc(nsecs ? nsecs : 1, sizeof(*padded));
    bool ok = secs && padded;
    for (unsigned i = 0; i < nsecs && ok; i++) {
        const uint8_t *h = xbe + hdrs + i * XBE_SEC_SIZE;
        NcRsqrtScanSection *d = &secs[i];
        const uint32_t raw = rd32(h + 0x0C), raw_size = rd32(h + 0x10), name = rd32(h + 0x14) - base;
        const uint32_t have = raw >= size ? 0 : raw_size < size - raw ? raw_size : size - raw;
        d->va = rd32(h + 0x04);
        d->size = rd32(h + 0x08);
        d->data = xbe + (have ? raw : 0);
        d->data_size = have;
        if (have < d->size) { /* the rest is zeros */
            padded[i] = calloc(d->size, 1);
            ok = padded[i] != NULL;
            if (ok) {
                memcpy(padded[i], d->data, have);
            }
            d->data = padded[i];
            d->data_size = d->size;
        }
        d->code = (rd32(h) & XBE_SEC_EXEC) != 0;
        for (unsigned j = 0; j < sizeof(data_names) / sizeof(data_names[0]); j++) {
            const size_t len = strlen(data_names[j]);
            d->code &= !(name < size && size - name >= len && !memcmp(xbe + name, data_names[j], len));
        }
        d->seeds = !d->code && !(name < size && size - name >= 2 && !memcmp(xbe + name, "$$", 2));
    }
    if (ok) {
        uint32_t roots[1 + XBE_MAX_TLS] = {0};
        unsigned nroots = 1;
        for (unsigned i = 0; i < sizeof(entry_keys) / sizeof(entry_keys[0]); i++) {
            roots[0] = rd32(xbe + XBE_ENTRY) ^ entry_keys[i];
            if (image_code(secs, nsecs, roots[0])) {
                break;
            }
        }
        /* the TLS directory's callbacks: a zero-terminated array of functions */
        uint32_t list = 0, fn = 0;
        if (rd32(xbe + XBE_TLS) && image_rd32(secs, nsecs, rd32(xbe + XBE_TLS) + 12, &list) && list) {
            while (nroots <= XBE_MAX_TLS && image_rd32(secs, nsecs, list + 4 * (nroots - 1), &fn) && fn) {
                roots[nroots++] = fn;
            }
        }
        ok = nc_rsqrt_scan(secs, nsecs, roots, nroots, out);
    }
    for (unsigned i = 0; i < nsecs && padded; i++) {
        free(padded[i]);
    }
    free(padded);
    free(secs);
    return ok;
}
