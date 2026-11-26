/*
 * Translate NV2A Kelvin methods (class 0x97) into back-end commands on the decoder thread.
 * This includes render target binding, clears and presentation at FLIP_INCREMENT_WRITE. Draws
 * use indexed or non-indexed vertex arrays, inline arrays or immediate vertices, with either
 * a vertex program or the fixed-function stage (shader/ffhw.h).
 */
#ifndef HW_XBOX_HLE_PB_PBDRAW_H
#define HW_XBOX_HLE_PB_PBDRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The back end, at push-buffer mode's start; false if it can't run. */
bool hle_pbd_start(void);
void hle_pbd_stop(void);
/* The core stops (M2; hle_pb_reset's, the decoder and the renderer down):
 * the buffers and the state-program cache freed, every object, texture,
 * cache keyed by guest memory, count and log limit as before the first
 * start, the kept-memory and front-buffer requests dropped. */
void hle_pbd_reset(void);
/* A Kelvin method (after the decoder's own handling). */
void hle_pbd_method(uint32_t m, uint32_t v);
/* n data words of one packet (method m, then +4 each unless noninc). */
void hle_pbd_run(uint32_t m, bool noninc, const uint32_t *w, uint32_t n);
/* Decode thread: the guest pointed the display at pa (PCRTC_START). A
 * title that flips that way itself, with no FLIP_INCREMENT_WRITE since the
 * last time (Riddick), gets the render target there presented; flip: D3D's
 * flip (hle_pbd_flip_due when the start was written), presented by it. */
void hle_pbd_display(uint32_t pa, bool flip);
/* The guest writes PCRTC_START (its thread): a swap waits for its flip and
 * the guest has been flipping (within a second), so the start is D3D's
 * flip, whose read counter increment comes next and presents the frame. */
bool hle_pbd_flip_due(void);
/* Decode thread: PVIDEO's registers changed: the overlay they describe
 * (buffer 0: YUY2 / UYVY at BASE + OFFSET, SIZE_IN onto POINT_OUT /
 * SIZE_OUT) goes over each presented frame, or none when stopped. */
void hle_pbd_overlay(bool show);
/* debug: the skipped draws by reason, " reason=n ..." */
void hle_pbd_skip_reasons(char *buf, size_t len);
/* debug: overlay updates sent to the back end */
uint64_t hle_pbd_overlays(void);
/* PCRTC_START writes that named the object shown, unchanged (not presented) */
uint64_t hle_pbd_display_repeats(void);
/* program draws that reused the last draw's constant block */
uint64_t hle_pbd_const_reuses(void);
/* debug: vertex ranges read in place and copied; submissions closed for a
 * barrier's waiter; DRAW_ARRAYS drawn without indices (one run in order) */
void hle_pbd_vb_counts(uint64_t *in_place, uint64_t *copied, uint64_t *bnd_kicks, uint64_t *unindexed);
/* Copies of ranges copied again in a frame, since the last call. */
uint32_t hle_pbd_take_recopies(void);
/* debug: small targets copied back to guest memory (writeback_small) */
uint64_t hle_pbd_writebacks(void);
/* Decode thread, between methods: a guest store waits for `value` (the
 * first-write barrier); the open submission, if it is that one, is closed
 * and sent at once. */
void hle_pbd_boundary(uint64_t value);
/* Decode thread: the display's gamma ramp (R / G / B planes), at once. */
void hle_pbd_gamma(const uint8_t rgb[3][256]);
/* The host: D3D's vblank handler flipped (the flip read counter moved), the
 * display starting at start (PCRTC_START): the oldest frame swapped and not
 * yet shown goes on screen. At the target being drawn (hle_pbd_trap), the
 * swap that comes is one flipped (hle_pbd_scanout). Any thread. */
void hle_pb_flipped(uint32_t start);
/* Decode thread, at a software trap (its handler may flip): the bound
 * target, if drawn since the last swap, is a frame whose flip can come
 * before its FLIP_INCREMENT_WRITE. */
void hle_pbd_trap(void);
/* Counters for the decoder's log line (swaps: FLIP_INCREMENT_WRITE's and
 * PCRTC_START's presents; rt_uploads: render targets the CPU wrote). */
void hle_pbd_counts(uint64_t *clears, uint64_t *swaps, unsigned *objects,
                    uint64_t *rt_uploads);
/* draws: all of them; ff: of those, fixed function */
/* Blit w x h pixels from (ix, iy) to (ox, oy) using sp and dp as pitches and bpp as bytes per
 * pixel. Run on the decoder thread. Return true when the GPU copied the render-target images
 * at their render scale without touching guest RAM. Otherwise the caller copies guest memory
 * using hle_pbd_copy_begin and hle_pbd_copy_done, then updates the images with hle_pbd_blit. */
bool hle_pbd_blit_images(uint32_t sbase, uint32_t sp, uint32_t dbase, uint32_t dp, uint32_t bpp,
                         uint32_t ix, uint32_t iy, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h);
/* Copy guest memory for a 2D blit or M2MF transfer on the decoder thread. The sp and dp row
 * pitches may be negative. Pass the blit destination surface as dbase, or zero for M2MF.
 *
 * hle_pbd_copy_begin returns zero for an immediate copy by the caller, or the queued timeline
 * value. The queued copy follows overlapping copies and readbacks from source targets with
 * newer images. hle_pbd_copy_done protects a queued destination until the copy completes and
 * marks guest memory newer than any images over it. */
uint64_t hle_pbd_copy_begin(uint32_t slo, uint32_t sp, uint32_t dlo, uint32_t dp, uint32_t row,
                            uint32_t rows, uint32_t dbase);
void hle_pbd_copy_done(uint32_t dlo, uint32_t dp, uint32_t row, uint32_t rows, uint64_t dep);
/* Decode thread: the memory of rows (row bytes each from lo, pitch apart)
 * as the GPU has it, before the caller reads it: the images of the targets
 * over it landed and waited for (a blit that blends reads its destination). */
void hle_pbd_mem_current(uint32_t lo, uint32_t pitch, uint32_t row, uint32_t rows);
/* Counters: targets' images landed, copies queued, decoder waits for them */
void hle_pbd_coh_counts(uint64_t *lands, uint64_t *copies, uint64_t *waits);
/* After a blit's copy in memory (dep: queued, hle_pbd_copy_begin): the
 * linear render target at dbase takes the rows into its image. */
void hle_pbd_blit(uint32_t dbase, uint32_t dp, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h, uint64_t dep);
/* Decode thread: a fence (a semaphore release, a report, the push buffer
 * consumed): the images of targets the guest reads (learned) landed for it. */
void hle_pbd_fence(void);
/* Decode thread: GET_REPORT for the visibility test that SET_ZPASS_PIXEL_
 * COUNT_ENABLE opened: the back end writes its report at pa (time stamp,
 * pixels, status 0) once the GPU ran its draws. False when nothing can
 * count it yet (no device): the caller writes the report itself. */
bool hle_pbd_vis_report(uint32_t pa);
/* Decode thread: the push buffer is consumed up to PUT (or stalled): reports
 * still unsubmitted go to the GPU now, kept memory goes back (hle_pbd_keep). */
void hle_pbd_idle(void);
/* Decode thread, each pass: the frame on screen into its memory when the
 * guest's first write there asked (front_stale). */
void hle_pbd_front_serve(void);
/* Decode thread, after a swap: true if the decoder stalls for the frame
 * pace until the GPU finishes an earlier swap (pbdraw.c g_pace); the GPU
 * kicks it then (hle_pb_kick), and hle_pbd_pace_reached says it has. */
bool hle_pbd_pace(void);
bool hle_pbd_pace_reached(void);
/* The diagnostics' (any thread): the depth at the last swap (0 off), the
 * swaps counted and their summed lead over the GPU (both from the start),
 * the most since the last call, and the stalls not made (note queue full). */
void hle_pbd_pace_stats(int *depth, uint64_t *ahead_sum, uint64_t *ahead_n, unsigned *ahead_max,
                        uint64_t *no_room);
/* Any thread: nothing presented for a while (the title isn't flipping), so
 * the display's memory is worth a look (hle_pbd_scanout). */
bool hle_pbd_scan_due(void);
/* On the decoder thread, process a swap left pending by a flip, then present the current
 * scanout again if its memory or image changed without another flip. This shows loading
 * prompts and other updates drawn directly into the front buffer. */
void hle_pbd_scanout(void);
/* On the decoder thread, present the frame buffer set by AvSetDisplayMode when the title
 * draws with the CPU and has no GPU device. Present it again after CPU writes. Titles with a
 * device use their own flips. */
void hle_pbd_cpu_display(uint32_t pa);
/* Any thread: guest memory [pa, pa + len) is kept for the next title (a quick
 * reboot): the decode thread writes the images over it back into it when it
 * is next idle. hle_pbd_keep_wait: until those copies are in memory (false:
 * not served within timeout_ns). */
void hle_pbd_keep(uint32_t pa, uint32_t len);
bool hle_pbd_keep_wait(uint64_t timeout_ns);
/* debug: what the decode thread waits for in a draw ("arena", "ring"), or NULL */
const char *hle_pbd_phase(void);
void hle_pbd_draw_counts(uint64_t *draws, uint64_t *ff, uint64_t *skipped, uint64_t *tex_defs);
/* The textures held (their images' bytes, pb/pbdraw.c g_pd.tex_bytes) */
void hle_pbd_tex_held(unsigned *n, uint64_t *bytes);
/* Visibility tests ended, and the submit boundaries they needed of their own */
void hle_pbd_vis_counts(uint64_t *tests, uint64_t *kicks);
/* debug (HAKUX_HLE_PB_NOVIS=1): no occlusion queries; visibility tests
 * report every pixel passed */
bool hle_pbd_novis(void);

#endif
