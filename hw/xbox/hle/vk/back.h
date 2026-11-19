/*
 * hakuX HLE GPU — back end (spec §5): render thread, timeline, surfaces.
 * No Vulkan types; callable from any HLE module.
 */
#ifndef HW_XBOX_HLE_VK_BACK_H
#define HW_XBOX_HLE_VK_BACK_H

#include <stdbool.h>
#include <stdint.h>

/* Vulkan device (floor), guest-RAM import, arena, ring, timeline, threads.
 * 0 = ok; negative = the title must use LLE. */
int hle_back_start(void);
bool hle_back_running(void);
/* The core stops (M2; the push-buffer decoder has stopped): the renderer's
 * threads joined, the device idle, every object on it destroyed and the
 * renderer's state as before hle_back_start. The device stays
 * (hle_vk_shutdown). */
void hle_back_shutdown(void);
void hle_back_kick(void);
/* The guest blocks on the timeline: pending fence / kick boundaries are
 * submitted at once (vCPU thread). */
void hle_back_urgent(void);

/* Timeline value = GPU time (§5.12). Any thread. */
uint64_t hle_tl_completed(void);
uint64_t hle_tl_submitted(void);
void hle_tl_wait(uint64_t value);
/* The device lost (hle_vk_lose): every value counts as reached, so no
 * thread waits on the GPU any more. */
void hle_tl_lose(void);
/* How long the GPU has been on our work without running out of it (0: it's
 * out now), and a moment of idle GPU asked for: the app's window, on a GPU
 * queue below ours, holds a frame the compositor shows the game's behind
 * (NativeCoreActivity's OverlayPacer). Any thread. */
int64_t hle_tl_busy_ns(void);
void hle_tl_gap(void);
/* len bytes the GPU copies into the arena at arena_off go to guest memory at
 * pa once it reaches value, before any thread sees the value (a readback
 * without zero-copy guest RAM). False if too many wait (nothing queued). */
bool hle_tl_land(uint64_t value, uint32_t arena_off, uint32_t pa, uint32_t len);
/* The back end recorded the copy into arena_off (rows of row bytes, pitch
 * apart): those land (else nothing does). */
void hle_tl_land_copied(uint32_t arena_off, uint32_t pitch, uint32_t row, uint32_t rows);
/* Apply queued readbacks and copies in queue order before publishing value. The swizzle
 * operation writes packed w x h texels of bpp bytes from off in the landing buffer, or the
 * arena, to guest pa. It requires the copy to be recorded with hle_tl_land_copied. The copy
 * operation moves rows within guest memory, for example when a blit reads a target just
 * written back. The note operation only signals completion.
 *
 * done(token, landed) runs on the thread applying the operation with the timeline lock held.
 * Callbacks must not queue another operation. An operation queued at a value already applied
 * runs immediately. A false result means the queue is full. Check hle_tl_land_room(n) before
 * queuing. hle_tl_land_oldest() is the value to wait for to make room, or zero when empty. */
typedef void (*HleLandDone)(uint32_t token, bool landed);
bool hle_tl_land_room(unsigned n);
uint64_t hle_tl_land_oldest(void);
bool hle_tl_land_swz(uint64_t value, uint32_t off, uint32_t pa, uint32_t w, uint32_t h, uint32_t bpp,
                     HleLandDone done, uint32_t token);
bool hle_tl_land_copy(uint64_t value, uint32_t spa, uint32_t spitch, uint32_t dpa, uint32_t dpitch,
                      uint32_t row, uint32_t rows, HleLandDone done, uint32_t token);
bool hle_tl_land_note(uint64_t value, HleLandDone done, uint32_t token);

typedef struct HleBackStats {
    uint64_t cmds, clears, passes, submits, swaps, readbacks, draws;
    uint64_t ring_hw;
    uint64_t readback_skips;        /* READBACKs not performed (format/zero-copy) */
    uint64_t copies, copy_skips;    /* CopyRects performed / not (vk/back.c cmd_copy) */
    uint64_t uploads, upload_skips; /* guest texels into surfaces / not (cmd_upload) */
    uint64_t zreads, zread_skips;   /* depth buffers read as colour / not (vk/zread.c) */
    uint64_t nopass;                /* draws dropped: neither target nor depth buffer usable */
    uint32_t last_color, last_rt;   /* last colour clear of the presented surface (M2) */
} HleBackStats;
void hle_back_draw_diag(void);      /* logs the draw-feature counters */

/* Frames the render thread finished in this process (its swaps; after a
 * savestate's load, the frames since it). Any thread. */
uint64_t hle_back_swap_count(void);

/* Headless frame capture (native/run/xboxrun.c). At each swap the render thread calls
 * want(n), starting at n = 1. If it returns true, got(n) receives the frame after GPU
 * completion as top-down RGBA at display size, including the gamma ramp unless the consumer
 * applies it. A frame without a display image passes NULL. Register the hook before the game
 * starts. Both callbacks run synchronously on the render thread. */
typedef struct HleFrameHook {
    bool (*want)(uint64_t n, void *opaque);
    void (*got)(uint64_t n, const uint8_t *rgba, uint32_t w, uint32_t h, void *opaque);
    void *opaque;
} HleFrameHook;
void hle_back_set_frame_hook(const HleFrameHook *hook);
/* Render thread (a frame hook): the pipeline cache and the records of the
 * pipelines made written now, for a run that ends in an exit (the process
 * never reaches the GPU's teardown, nor the next periodic write). */
void hle_back_save_caches(void);


#endif
