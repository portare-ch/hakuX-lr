/*
 * hakuX-lr HLE GPU — first-write barrier (spec §4.10; P2 Decision 5): the
 * guest's first store to a page the HLE cleaned waits for the GPU work that
 * reads it.
 */
#ifndef HW_XBOX_HLE_MEM_BARRIER_H
#define HW_XBOX_HLE_MEM_BARRIER_H

#include <stdbool.h>
#include <stdint.h>

void hle_barrier_init(uint64_t ram_size);
/* Free the tables and reset the hooks and counters after the vCPU, decoder and back end stop.
 * Stopping push-buffer mode restores access to stale ranges and report pages. */
void hle_barrier_free(void);
/* GPU work up to `value` reads [pa, pa+len) in place: the first CPU write
 * waits, and the CPU cache lines of its pages written since they were last
 * cleaned go to memory first (the GPU doesn't snoop the CPU's caches over
 * the imported RAM). */
void hle_barrier_arm(uint32_t pa, uint32_t len, uint64_t value);
/* GPU work up to `value` writes [pa, pa+len) in place (a readback into the
 * imported RAM): as hle_barrier_arm, and the range's CPU cache lines are
 * cleaned and invalidated, so neither a dirty line written back later nor a
 * stale clean one hides what the GPU wrote. */
void hle_barrier_arm_landing(uint32_t pa, uint32_t len, uint64_t value);
/* Make CPU writes wait for GPU timeline value on pages fully covered by [pa, pa + len).
 * Overlapped reads into those pages also wait. Keep their dirty bits and leave shared edge
 * pages accessible. If gpu is true, clean and invalidate CPU cache lines before the GPU
 * writes in place. Use hle_mem_cache_inval after the GPU write. */
void hle_barrier_arm_keep(uint32_t pa, uint32_t len, uint64_t value, bool gpu);
/* ... a copy queued to read [pa, pa+len) when it lands: every page of it
 * waits (no guard covers a copy's source) */
void hle_barrier_arm_src(uint32_t pa, uint32_t len, uint64_t value);
void hle_mem_cache_inval(uint32_t pa, uint32_t len);
/* Protect a stale scanout range until the guest first reads or writes one of its pages. The
 * guest thread calls fill(pa, len) once and waits for the readback it schedules. One range
 * can be active at a time. A zero length clears it. */
void hle_barrier_set_stale(uint32_t pa, uint32_t len, void (*fill)(uint32_t pa, uint32_t len));
/* Guard guest memory whose render-target image is newer. A read or write waits for an
 * existing readback (hle_barrier_guard_landing) or calls the fill callback on the guest
 * thread to schedule one. The callback receives the access type and returns a timeline value,
 * or zero if no readback is needed. After waiting, restore access with first-write tracking.
 * A range re-armed during the wait is also accessible until its next arm.
 *
 * The host can service accesses outside the guarded bytes of a shared page while keeping the
 * page closed (hle_mem_stale_neighbour in native/hle/dirty.c).
 *
 * hle_barrier_set_stale(.., NULL) clears all guards. hle_barrier_unguard drops one before its
 * image is destroyed. hle_barrier_unguard_range drops all overlapping guards when the host or
 * a device writes the range, or when it is freed. hle_barrier_guard_drops counts guards
 * dropped or reopened. */
enum { HLE_GUARD_NONE, HLE_GUARD_ARMED, HLE_GUARD_LANDING, HLE_GUARD_OPEN };
void hle_barrier_set_guard_fill(uint64_t (*fill)(uint32_t pa, uint32_t len, bool read));
void hle_barrier_guard(uint32_t pa, uint32_t len);
void hle_barrier_guard_landing(uint32_t pa, uint32_t len, uint64_t v);
int hle_barrier_guard_state(uint32_t pa, uint32_t len, uint64_t *v);
void hle_barrier_unguard(uint32_t pa, uint32_t len);
void hle_barrier_unguard_range(uint32_t pa, uint32_t len);
uint32_t hle_barrier_guard_drops(void);
/* Classify a guest access to [lo, hi) within one protected page. Return 0 if it touches a
 * guarded target or scanout or the page is unprotected, 1 if the host can handle it outside
 * the guards, and 2 if repeated accesses made the page hot. In case 2 access is restored, the
 * guest retries the access, and new guards skip the page when it is an edge page.
 * hle_barrier_guard_stats counts handled accesses and hot pages. */
int hle_mem_stale_neighbour(uint32_t lo, uint32_t hi);
void hle_barrier_guard_stats(uint64_t *neighbours, uint64_t *hot);
/* Guest memory freed (native/core/vm.c): the guards over it dropped, its
 * pages hot no more */
void hle_barrier_freed(uint32_t pa, uint32_t len);
/* The host, at a guest access to a page without access (hle_barrier_set_stale,
 * a guard; write: a store, or not known): the image in, the access back. */
void hle_mem_stale_access(uint32_t pa);
void hle_mem_stale_access_rw(uint32_t pa, bool write);
/* Guest RAM the host wrote through the RAM view (device models, HLE copies:
 * host.h hle_host_mem_set_dirty): its pages' cache lines are cleaned before
 * the GPU next reads them in place. */
void hle_mem_host_wrote(uint32_t pa, uint32_t len);
/* Was [pa, pa+len) written by the CPU since the last call (HLE dirty bits,
 * then cleared)? Texture snapshots use it to see writes made without Lock
 * (spec §5.5 "dirty bitmap over the object's range"). True when unknown. */
bool hle_mem_written(uint32_t pa, uint32_t len);
/* Guest RAM written by the emulated GPU (a copy the decode performs): the
 * next hle_mem_written() over it sees the write. */
void hle_mem_mark_written(uint32_t pa, uint32_t len);
/* ... by a target's image landing there: the same, the target's guard kept
 * (host.h hle_host_mem_landed). */
void hle_mem_mark_landed(uint32_t pa, uint32_t len);
/* Writes reported to [pa, pa + len)'s pages (first-write faults, host
 * writes), summed, and those of its first and last pages: a range written
 * since an earlier look has a larger sum (0: not counted, HAKUX_HLE_NO_CLEAN). */
uint64_t hle_mem_wseq_sum(uint32_t pa, uint32_t len, uint32_t *first, uint32_t *last);
/* ... the same counted by blocks of 64 pages over the range (a coarse look:
 * unchanged, no page of it was written) */
uint64_t hle_mem_wseq_blocks(uint32_t pa, uint32_t len);
/* A count of the writes reported anywhere: unchanged, no sum above moved. */
uint32_t hle_mem_write_epoch(void);
/* GPU progress source (back end, Task 13). */
void hle_barrier_set_waiter(uint64_t (*completed)(void), void (*wait)(uint64_t));
/* Before a first write waits for `value`: the producer that may still hold
 * it open closes it (push-buffer mode's decode thread), else the wait could
 * outlast it. NULL: none. */
void hle_barrier_set_flush(void (*flush)(uint64_t value));
/* Whether a first write to a page of [pa, pa + len) had to wait for the GPU
 * (the CPU rewrites memory while the GPU still reads it). */
bool hle_barrier_contended(uint32_t pa, uint32_t len);
/* After copying [pa, pa + len) for a draw, make the next guest write wait for the decoder. A
 * later queued draw may still need the old data. The catch_up callback runs on the guest
 * thread and returns after decoding the work submitted before the store. */
void hle_barrier_arm_copied(uint32_t pa, uint32_t len);
/* Protect texels until queued draws have read them. */
void hle_barrier_arm_texels(uint32_t pa, uint32_t len);
/* Re-arm written texel spans once, at the guest thread's next PUT. */
void hle_barrier_rearm_texels(void);
void hle_barrier_set_catch_up(void (*catch_up)(void));
/* Track visibility reports from GET_REPORT in push-buffer mode. The scout runs on the guest
 * thread at each PUT write. It assigns each report to a frame by counting preceding swaps and
 * protects the report's page. The guest waits up to 30 ms for pending reports from earlier
 * frames before accessing the page. The decoder marks reports it reaches, and the back end
 * marks results it writes.
 *
 * The scout callback scans to the guest's PUT and returns the current frame. A NULL scout
 * disables the gate. The held callback reports whether the decoder depends on the guest, such
 * as for an interrupt trap, disabled FIFO access or a flip stall. The guest skips waiting for
 * reports the held decoder has yet to reach. */
void hle_barrier_report_pushed(uint32_t pa, uint32_t frame);
void hle_barrier_report_decoded(uint32_t pa);
void hle_barrier_report_written(uint32_t pa);
void hle_barrier_set_report_scout(uint32_t (*scout)(void), bool (*held)(void));
/* The gate's counts: accesses that waited, their time, waits given up after
 * 30 ms, and at once (the decoder held by the guest short of the report). */
void hle_barrier_report_stats(uint64_t *waits, uint64_t *ns, uint64_t *timeouts, uint64_t *held);
/* The host, at the guest's first store to a page tracking made clean
 * (host.h hle_host_mem_test_and_clear). */
void hle_mem_first_write(uint64_t ram_addr, unsigned size);
/* For an asynchronous write to [pa, pa + len), return the GPU timeline value the writer must
 * wait for, or zero if ready. Flush the producer first. The writer calls hle_mem_wait_fence
 * before writing and hle_host_mem_set_dirty afterward. */
uint64_t hle_mem_write_fence(uint32_t pa, uint32_t len);
void hle_mem_wait_fence(uint64_t value);


typedef struct HleBarrierStats {
    uint64_t traps, waits, wait_ns;
    uint64_t armed;                 /* traps on pages armed for GPU reads (else: dirty tracking) */
    uint64_t unarmed;               /* traps with nothing armed: always a bug */
    uint64_t cleaned;               /* pages whose cache lines went to memory for the GPU */
} HleBarrierStats;
HleBarrierStats hle_barrier_stats(void);
/* debug: the guest RAM pages (PA >> 12) whose first write waited, the last
 * few (newest first); returns how many */
unsigned hle_barrier_wait_pages(uint32_t *pages, unsigned max);

#endif
