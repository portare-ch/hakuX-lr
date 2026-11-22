/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Libretro front end for RetroArch (LIBRETRO_SPEC.md).
 *
 * The CPU, renderer and APU run on their own threads. Each retro_run drives the NV2A vblank
 * clock and returns the latest frame and one period of audio without waiting for the guest.
 * After three periods without retro_run, park the emulation and pause guest time. Resume on
 * the next call.
 *
 * Starting without content boots the dashboard from C: with the tray empty. The dashboard
 * handles Insignia account creation.
 *
 * Unload, reset, title relaunch and Vulkan context loss stop and reset the core in process.
 * PortareOS builds can exit RetroArch if stop hangs (HAKUX_LR_END_PROCESS). Setting
 * HAKUX_LR_STOP=0 exits on close and re-execs RetroArch for a title relaunch.
 */
#include "native/libretro/libretro_int.h"
#include "native/core/cheats.h"
#include "native/core/core.h"
#include "native/core/dash.h"
#include "native/core/irq.h"
#include "native/core/mods.h"
#include "native/net/net.h"
#include "native/core/online.h"
#include "native/core/xbe_patch.h"
#include "native/dev/dev.h"
#include "native/state/save.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HAKUX_VERSION
#define HAKUX_VERSION "dev"
#endif
#ifndef HAKUX_LR_END_PROCESS
#define HAKUX_LR_END_PROCESS 0
#endif

#define GAP_NS (3 * NC_LR_FRAME_NS)   /* no retro_run this long: paused */
#define DASH_MARK "hakux:dashboard"   /* HAKUX_RELAUNCH for the dashboard (the app's Dashboard.URI) */

static retro_environment_t g_env;
static retro_video_refresh_t g_video;
static retro_audio_sample_batch_t g_audio_batch;
static retro_input_poll_t g_input_poll;
static retro_input_state_t g_input_state;

static NcCore g_core;
static int g_disc_fd = -1;
static int g_core_state;           /* 0 none, 1 booting, 2 running, -1 stopped */
static bool g_started;
static const char *g_fail;         /* nc_lr_fail's: the content stops */
static bool g_shutdown_sent;
static char g_game[PATH_MAX];      /* the content's real path */
static bool g_dash;                /* no content: the dashboard boots, no disc */
static bool g_armed;               /* prepared to boot (prepare_boot); a stop disarms */
static char g_hdd[PATH_MAX], g_cache[PATH_MAX];    /* the save directory's (data_dirs) */
static pthread_t g_core_thread, g_gap_thread;
static int g_gap_quit;             /* stop_core */
static bool g_handlers;            /* on_fault installed (g_old_segv, g_old_bus) */

/* Save states and the console's network don't go together (a state can't
 * hold a host's sockets or a peer's session): the content opened with the
 * Network option on (g_net_opened), or any boot of it since had it on
 * (g_net_seen), until the content closes. */
static bool g_net_opened, g_net_seen, g_net_booted;
static uint64_t g_runs;             /* retro_run calls (state_burst) */
/* save states: the one taken to answer retro_serialize_size (state_taken) */
static struct {
    bool have;                      /* its files are in the state-save folder */
    uint64_t runs, at_ns;           /* g_runs and the time it was taken */
    size_t size;
} g_taken;

static void prepare_boot(bool relaunch);
static bool can_stop(void);
static bool boot_again(const char *why, bool relaunch, const char *state);
static void cheats_forget(void);

static struct {
    pthread_mutex_t mu;
    uint64_t last_run_ns;          /* 0: no retro_run yet */
    bool paused;                   /* by a gap, not by the guest */
} g_gap = {.mu = PTHREAD_MUTEX_INITIALIZER};

static struct sigaction g_old_segv, g_old_bus;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Guest RAM tracking, MMIO and the JIT's call-ret stack are faults the core
 * resolves; any other is passed on to the handler before ours (RetroArch's,
 * another core's), or to the default action as the fault repeats. */
static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (__atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) != 0 && nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    const struct sigaction *old = sig == SIGSEGV ? &g_old_segv : &g_old_bus;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, info, ctx);
        return;
    }
    if (!(old->sa_flags & SA_SIGINFO) && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigaction(sig, &dfl, NULL);
}

bool nc_lr_env(unsigned cmd, void *data)
{
    return g_env && g_env(cmd, data);
}

bool nc_lr_dashboard(void)
{
    return g_dash;
}

static void message(const char *text, unsigned frames)
{
    const struct retro_message msg = {text, frames};
    g_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
}

void nc_lr_fail(const char *why)
{
    nc_log("hakuX-lr", "failed: %s", why);
    __atomic_store_n(&g_fail, why, __ATOMIC_RELEASE);
}

static void *core_thread(void *arg)
{
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-lr", "core: boot failed");
        __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
        nc_hle_cpu_leave();
        nc_cpu_thread_done();
        return NULL;
    }
    __atomic_store_n(&g_core_state, 2, __ATOMIC_RELEASE);
    nc_core_run(&g_core);
    nc_hle_cpu_leave();
    nc_log("hakuX-lr", "core: stopped%s", g_core.kernel.quit ? " (the title left)" : "");
    __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
    nc_cpu_thread_done();
    return NULL;
}

/* RetroArch stopped calling: pause the emulation until it calls again. */
static void *gap_thread(void *arg)
{
    while (!__atomic_load_n(&g_gap_quit, __ATOMIC_ACQUIRE)) {
        const struct timespec ts = {0, (long)NC_LR_FRAME_NS};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_gap.mu);
        if (g_gap.last_run_ns && !g_gap.paused && now_ns() - g_gap.last_run_ns > GAP_NS &&
            __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) == 2) {
            g_gap.paused = true;
            nc_pause_set(true);
        }
        pthread_mutex_unlock(&g_gap.mu);
    }
    return NULL;
}

static void gap_tick(uint64_t now)
{
    pthread_mutex_lock(&g_gap.mu);
    g_gap.last_run_ns = now;
    if (g_gap.paused) {
        g_gap.paused = false;
        nc_pause_set(false);
    }
    pthread_mutex_unlock(&g_gap.mu);
}

static bool make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

/* The save directory (PortareOS: /storage/saves/xbox): the console's hard
 * disk (hdd/, its partitions) and the renderer's caches (cache/). */
static bool data_dirs(char *hdd, char *cache, size_t n)
{
    const char *root = NULL;
    if (!g_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &root) || !root || !*root) {
        nc_log("hakuX-lr", "no save directory from the frontend");
        return false;
    }
    snprintf(hdd, n, "%s/hdd", root);
    snprintf(cache, n, "%s/cache", root);
    bool ok = make_dir(root) && make_dir(hdd) && make_dir(cache);
    for (int i = 0; ok && i <= 5; i++) {
        char p[PATH_MAX + 32];
        snprintf(p, sizeof(p), "%s/Partition%d", hdd, i);
        ok = make_dir(p);
    }
    if (!ok) {
        nc_log("hakuX-lr", "data directories under %s: %s", root, strerror(errno));
    }
    return ok;
}

/* PortareOS's build (HAKUX_LR_END_PROCESS) may end RetroArch's process: when
 * a stop hangs, and with HAKUX_LR_STOP=0 when the game closes.
 * HAKUX_LR_END_PROCESS=0 or 1 in the environment chooses for a run (the
 * tests). */
static bool end_process(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_END_PROCESS");
        on = e ? atoi(e) != 0 : HAKUX_LR_END_PROCESS;
    }
    return on;
}

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    g_env = cb;
    bool no_game = true;            /* Start Core: the dashboard */
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    nc_lr_options_declare(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
    g_video = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    g_audio_batch = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
    g_input_poll = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
    g_input_state = cb;
}

RETRO_API void retro_init(void)
{
    /* RetroArch's dlclose leaves the library mapped. FEX's allocator
     * (rpmalloc) keeps its spans for the process, its state in the library:
     * unloaded, each load would leave them behind (about 1 GiB of address
     * space); kept, the next load reuses them and finds the statics as the
     * stop reset them. */
    Dl_info self;
    if (dladdr((void *)retro_init, &self) && self.dli_fname) {
        dlopen(self.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }
    nc_lr_input_reset();    /* the library stays: its ports, not the last session's */
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "hakuX-lr";
    info->library_version = HAKUX_VERSION;
    info->valid_extensions = "iso|chd";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    nc_lr_video_geometry(&info->geometry);
    info->timing.fps = (double)NC_LR_FPS_NUM / (double)NC_LR_FPS_DEN;   /* the Xbox's, exactly */
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    nc_lr_input_set_device(port, device);
}

RETRO_API void retro_reset(void)
{
    if (!g_started || !can_stop()) {
        nc_log("hakuX-lr", "reset: not yet, while the renderer runs (M2's next part)");
        return;
    }
    boot_again("reset", false, NULL);
}

/* Install the bundled dashboard if needed before starting without content. */
static bool dashboard_installed(void)
{
    char host[512];
    nc_io_set_hdd_root(g_hdd);
    nc_dash_install(g_hdd);
    if (nc_io_host_file(NC_DASHBOARD_PATH, host, sizeof(host))) {
        return true;
    }
    static char text[PATH_MAX + 128];
    snprintf(text, sizeof(text), "The dashboard couldn't be put on the hard disk's C: drive (%s/Partition2)", g_hdd);
    message(text, 600);
    nc_log("hakuX-lr", "%s", text);
    return false;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    const bool dash = !game || !game->path || !*game->path;     /* Start Core */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!g_env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        nc_log("hakuX-lr", "the frontend has no XRGB8888");
        return false;
    }
    {   /* a state is for saving and loading by hand: seconds of a game
         * stopped, and hundreds of MiB, not a frame's work (rewind, run-ahead,
         * netplay) */
        uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE | RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT |
                          RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
        g_env(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    }
    if (!data_dirs(g_hdd, g_cache, sizeof(g_hdd))) {
        return false;
    }
    if (dash) {
        if (!dashboard_installed()) {
            return false;
        }
        g_game[0] = 0;
    } else {
        g_disc_fd = open(game->path, O_RDONLY | O_CLOEXEC);
        if (g_disc_fd < 0) {
            nc_log("hakuX-lr", "%s: %s", game->path, strerror(errno));
            return false;
        }
        if (!realpath(game->path, g_game)) {
            snprintf(g_game, sizeof(g_game), "%s", game->path);
        }
    }
    g_dash = dash;
    /* RetroArch started again for this game's relaunch (relaunch, where the
     * core couldn't stop): boot the XBE it named. */
    bool again = false;
    if (end_process()) {
        const char *e = getenv("HAKUX_RELAUNCH");
        again = e && !strcmp(e, dash ? DASH_MARK : g_game);
        unsetenv("HAKUX_RELAUNCH");
    }
    nc_lr_input_describe(g_env);
    prepare_boot(again);

    /* With Vulkan the guest boots in context_reset, on RetroArch's device. */
    const bool vulkan = nc_lr_vk_negotiate();
    if (__atomic_load_n(&g_fail, __ATOMIC_ACQUIRE)) {
        return false;
    }
    nc_log("hakuX-lr", "loaded %s; data in %s", dash ? "the dashboard (no content)" : game->path, g_hdd);
    return vulkan || nc_lr_start();
}

bool nc_lr_start(void)
{
    if (g_started) {
        return true;
    }
    if (!g_armed && (g_game[0] || g_dash)) {
        /* stopped with the game loaded: RetroArch rebuilt its video driver
         * (context_destroy, then context_reset on its new device) */
        message(g_dash ? "RetroArch rebuilt its video driver: the dashboard starts again"
                       : "RetroArch rebuilt its video driver: the game starts again", 300);
        return boot_again("video driver rebuilt", false, NULL);
    }
    g_started = true;
    __atomic_store_n(&g_core_state, 1, __ATOMIC_RELEASE);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    const int err = pthread_create(&g_core_thread, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);
    if (err) {
        nc_log("hakuX-lr", "core thread: %s", strerror(err));
        __atomic_store_n(&g_core_state, -1, __ATOMIC_RELEASE);
        g_started = false;
        return false;
    }
    __atomic_store_n(&g_gap_quit, 0, __ATOMIC_RELEASE);
    if (pthread_create(&g_gap_thread, NULL, gap_thread, NULL) != 0) {
        g_gap_thread = 0;
    }
    return true;
}

/* The core's process-wide fault handler, while a game is loaded. */
static void handlers_install(void)
{
    if (g_handlers) {
        return;
    }
    struct sigaction sa = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    g_handlers = true;
}

/* RetroArch's own back, if it is still ours (nobody installed one since). */
static void handlers_restore(void)
{
    if (!g_handlers) {
        return;
    }
    struct sigaction now;
    if (sigaction(SIGSEGV, NULL, &now) == 0 && now.sa_sigaction == on_fault) {
        sigaction(SIGSEGV, &g_old_segv, NULL);
    }
    if (sigaction(SIGBUS, NULL, &now) == 0 && now.sa_sigaction == on_fault) {
        sigaction(SIGBUS, &g_old_bus, NULL);
    }
    g_handlers = false;
}

/* What each boot is given before the guest runs: the data folders, the
 * renderer's mode, the options (read again: a reset or a relaunch takes a
 * changed one), the vblank's period, a relaunch. */
static void prepare_boot(bool relaunch)
{
    nc_core_set_relaunch(relaunch);
    nc_core_set_dashboard(g_dash);      /* no disc: C:\xboxdash.xbe */
    nc_io_set_hdd_root(g_hdd);
    nc_hle_set_mode("replace");
    nc_hle_set_base_path(g_cache);
    nc_lr_options_apply();                          /* the render scale, the console's settings */
    if (nc_net_mode() != NC_NET_OFF) {
        g_net_seen = true;
        g_net_opened = g_net_opened || !g_net_booted;
    }
    g_net_booted = true;
    nc_dev_nv2a_display_period(NC_LR_FRAME_NS);    /* retro_run's: the vblank locks one to one */
    nc_hle_set_pipeline_options(true, true);   /* the app's defaults */
    handlers_install();
    g_armed = true;
}

/* The core stops in RetroArch's process (M2). In PortareOS's build
 * HAKUX_LR_STOP=0 asks for the way before it, should a stop misbehave
 * somewhere: the process ends instead (nc_lr_end). */
static bool can_stop(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_STOP");
        on = !(end_process() && e && !strcmp(e, "0"));
    }
    return on;
}

/* PortareOS's build: a stop that takes longer than this ends RetroArch's
 * process, as a closed game did before M2, rather than leave the handheld
 * frozen; elsewhere unloading waits (LIBRETRO_SPEC section 6).
 * HAKUX_LR_STOP_TIMEOUT_MS sets it (the tests). */
#define STOP_TIMEOUT_MS 10000u

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;              /* CLOCK_MONOTONIC's */
    bool done;
    const char *why, *stage;        /* the stop's, and what it waits for */
    unsigned ms;
    uint64_t deadline_ns;
    pthread_t th;
} g_wd = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void *wd_thread(void *arg)
{
    const struct timespec until = {(time_t)(g_wd.deadline_ns / 1000000000ull),
                                   (long)(g_wd.deadline_ns % 1000000000ull)};
    pthread_mutex_lock(&g_wd.mu);
    while (!g_wd.done) {
        if (pthread_cond_timedwait(&g_wd.cv, &g_wd.mu, &until) == ETIMEDOUT && !g_wd.done) {
            nc_log("hakuX-lr", "%s: the core did not stop in %u ms (%s): ending the process", g_wd.why,
                   g_wd.ms, g_wd.stage);
            fflush(NULL);
            _exit(0);
        }
    }
    pthread_mutex_unlock(&g_wd.mu);
    return arg;
}

/* The watchdog over a stop from t0 (true: started, wd_end ends it). */
static bool wd_start(const char *why, uint64_t t0)
{
    if (!end_process()) {
        return false;
    }
    const char *e = getenv("HAKUX_LR_STOP_TIMEOUT_MS");
    g_wd.ms = e && atoi(e) > 0 ? (unsigned)atoi(e) : STOP_TIMEOUT_MS;
    g_wd.deadline_ns = t0 + g_wd.ms * 1000000ull;
    g_wd.why = why;
    g_wd.stage = "starting";
    g_wd.done = false;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wd.cv, &ca);
    pthread_condattr_destroy(&ca);
    const int err = pthread_create(&g_wd.th, NULL, wd_thread, NULL);
    if (err) {
        nc_log("hakuX-lr", "%s: no watchdog over the stop: %s", why, strerror(err));
        pthread_cond_destroy(&g_wd.cv);
        return false;
    }
    return true;
}

static void wd_stage(const char *stage)
{
    pthread_mutex_lock(&g_wd.mu);
    g_wd.stage = stage;
    pthread_mutex_unlock(&g_wd.mu);
}

static void wd_end(void)
{
    pthread_mutex_lock(&g_wd.mu);
    g_wd.done = true;
    pthread_cond_signal(&g_wd.cv);
    pthread_mutex_unlock(&g_wd.mu);
    pthread_join(g_wd.th, NULL);
    pthread_cond_destroy(&g_wd.cv);
}

/* M2: the guest stopped (its CPU, at its next block start, idle or paused),
 * its devices and I/O, then everything the boot made freed and the core's
 * state reset (nc_core_destroy), the fault handler given back: the next
 * nc_lr_start boots afresh. RetroArch's thread. */
static void stop_core(const char *why)
{
    if (!g_started) {
        return;
    }
    nc_lr_input_rumble_stop();      /* a title shaking its controller stops with it */
    nc_lr_mic_stop();               /* and its Communicator's microphone */
    const uint64_t t0 = now_ns();
    const bool wd = wd_start(why, t0);
    wd_stage("the guest CPU");
    nc_core_request_stop(&g_core);
    pthread_join(g_core_thread, NULL);
    wd_stage("the frame gap's thread");
    __atomic_store_n(&g_gap_quit, 1, __ATOMIC_RELEASE);
    if (g_gap_thread) {
        pthread_join(g_gap_thread, NULL);
        g_gap_thread = 0;
    }
    wd_stage("the devices and the renderer");
    nc_core_halt(&g_core);
    wd_stage("freeing the console");
    nc_core_destroy(&g_core);
#ifdef __GLIBC__
    /* What the boot's threads freed back to the kernel: glibc gave them arenas
     * of their own, which keep it, and the next boot's threads get others. */
    malloc_trim(0);
#endif
    if (wd) {
        wd_end();
    }
    handlers_restore();
    g_disc_fd = -1;                 /* the core's: closed with the disc */
    g_armed = false;
    __atomic_store_n(&g_core_state, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fail, NULL, __ATOMIC_RELEASE);
    g_started = false;
    g_shutdown_sent = false;
    pthread_mutex_lock(&g_gap.mu);
    g_gap.last_run_ns = 0;
    g_gap.paused = false;
    pthread_mutex_unlock(&g_gap.mu);
    nc_log("hakuX-lr", "%s: the core stopped in %.0f ms", why, (double)(now_ns() - t0) / 1e6);
}

/* Stop and reboot the game or dashboard in process for a reset, a relaunch
 * into the XBE named by the launch data page, or a save state's load (state:
 * the engine's file, whose XBE boots and which then replaces it). */
static bool boot_again(const char *why, bool relaunch, const char *state)
{
    stop_core(why);
    nc_core_set_loadstate(state);       /* for this boot only: the next stop forgets it */
    g_disc_fd = g_dash ? -1 : open(g_game, O_RDONLY | O_CLOEXEC);
    if (g_disc_fd < 0 && !g_dash) {
        nc_log("hakuX-lr", "%s: %s: %s", why, g_game, strerror(errno));
        nc_lr_fail("the game's file can't be opened again");
        return false;
    }
    prepare_boot(relaunch);
    return nc_lr_start();
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    return false;
}

/* Every descriptor but stdin, stdout and stderr closes at the exec, as at an
 * exit: the display, the input devices, the audio server. */
static void close_on_exec(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        return;
    }
    for (const struct dirent *e; (e = readdir(d));) {
        const int fd = atoi(e->d_name);
        const int flags = fd > 2 && fd != dirfd(d) ? fcntl(fd, F_GETFD) : -1;
        if (flags >= 0) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    closedir(d);
}

/* Keep dashboard restart options, including -s for the console disk's
 * directory. Drop all others (content, other cores, --menu). Return the
 * number of argv entries consumed by the option at i, or 0 to drop it. */
static unsigned kept_option(char *const *argv, unsigned i, unsigned argc)
{
    static const char *const with_value[] = {"-c", "--config", "-s", "--save", "--appendconfig", "--log-file"};
    static const char *const alone[] = {"-v", "--verbose", "-f", "--fullscreen"};
    const char *a = argv[i];
    for (size_t k = 0; k < sizeof(alone) / sizeof(*alone); k++) {
        if (!strcmp(a, alone[k])) {
            return 1;
        }
    }
    for (size_t k = 0; k < sizeof(with_value) / sizeof(*with_value); k++) {
        const size_t len = strlen(with_value[k]);
        if (strncmp(a, with_value[k], len)) {
            continue;
        }
        if (!a[len]) {
            return i + 1 < argc ? 2 : 0;
        }
        if (a[len] == '=' || len == 2) {        /* --config=x, -cx */
            return 1;
        }
    }
    return 0;
}

/* Re-exec RetroArch for XLaunchNewImage when in-process shutdown is disabled. HAKUX_RELAUNCH
 * identifies the game or DASH_MARK. The next boot takes the XBE and launch data from
 * relaunch.bin. For dashboard sessions, rebuild argv from kept_option and add -L for this
 * core so a menu launch starts content. execve preserves the PID watched by runemu.sh. Return
 * if restarting fails. */
static void relaunch(void)
{
    static char cmd[64 * 1024];
    static char *argv[1024];
    static char *menu_argv[1024 + 3];
    static char opt_core[] = "-L";
    static char mark[PATH_MAX + 32];
    static char want[PATH_MAX];
    extern char **environ;
    Dl_info self;
    if (!g_dash) {
        snprintf(want, sizeof(want), "%s", g_game);
    } else if (!dladdr((void *)retro_init, &self) || !self.dli_fname || !realpath(self.dli_fname, want)) {
        want[0] = 0;
    }
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    size_t n = 0;
    if (fd >= 0) {
        ssize_t r;
        while (n < sizeof(cmd) - 1 && (r = read(fd, cmd + n, sizeof(cmd) - 1 - n)) > 0) {
            n += (size_t)r;
        }
        close(fd);
    }
    unsigned argc = 0;
    bool on_game = false;
    for (size_t i = 0; i < n && argc < sizeof(argv) / sizeof(*argv) - 1; i += strlen(cmd + i) + 1) {
        char real[PATH_MAX];
        argv[argc++] = cmd + i;
        on_game = on_game || (*want && realpath(cmd + i, real) && !strcmp(real, want));
    }
    argv[argc] = NULL;
    char *const *start = argv;
    if (g_dash && *want && argc > 0) {
        unsigned m = 0;
        menu_argv[m++] = argv[0];
        for (unsigned i = 1; i < argc;) {
            const unsigned k = kept_option(argv, i, argc);
            for (unsigned j = 0; j < k; j++) {
                menu_argv[m++] = argv[i + j];
            }
            i += k ? k : 1;
        }
        menu_argv[m++] = opt_core;
        menu_argv[m++] = want;
        menu_argv[m] = NULL;
        start = menu_argv;
        nc_log("hakuX-lr", "relaunch: RetroArch starts again with its config and -L %s", want);
    } else if (!on_game) {
        nc_log("hakuX-lr", "relaunch: RetroArch was not started on the %s; it can't start again for it",
               g_dash ? "core" : "game");
        return;
    }
    size_t ne = 0;
    while (environ && environ[ne]) {
        ne++;
    }
    char **envp = calloc(ne + 2, sizeof(*envp));
    if (!envp) {
        return;
    }
    size_t m = 0;
    for (size_t i = 0; i < ne; i++) {
        if (strncmp(environ[i], "HAKUX_RELAUNCH=", 15)) {
            envp[m++] = environ[i];
        }
    }
    snprintf(mark, sizeof(mark), "HAKUX_RELAUNCH=%s", g_dash ? DASH_MARK : g_game);
    envp[m] = mark;
    nc_log("hakuX-lr", "relaunch: starting RetroArch again for %s", g_dash ? "the dashboard" : g_game);
    fflush(NULL);
    close_on_exec();
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    execve("/proc/self/exe", start, envp);
    nc_log("hakuX-lr", "relaunch: RetroArch can't start again: %s", strerror(errno));
    free(envp);
}

void nc_lr_end(const char *why)
{
    const int state = __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE);
    if (state == 0) {
        return;
    }
    if (can_stop()) {
        stop_core(why);
        return;
    }
    if (state < 0 && g_core.kernel.relaunch) {
        relaunch();
    }
    nc_log("hakuX-lr", "%s: the core does not stop (HAKUX_LR_STOP=0); ending the process", why);
    fflush(NULL);
    _exit(0);
}

/* Last pacing slot, or the start time of a late call. Zero after unload. */
static uint64_t g_last_run_at;

RETRO_API void retro_unload_game(void)
{
    nc_lr_end("unload");
    if (g_disc_fd >= 0) {           /* loaded, never booted (no context_reset came) */
        close(g_disc_fd);
        g_disc_fd = -1;
    }
    handlers_restore();
    nc_lr_video_reset();
    nc_lr_vk_reset();
    g_last_run_at = 0;
    cheats_forget();                /* the next game's come from RetroArch again */
    g_game[0] = 0;
    g_taken.have = false;
    g_taken.size = 0;
    g_dash = false;
    g_armed = false;
    g_net_opened = g_net_seen = g_net_booted = false;   /* the next content is asked again */
}

/* HAKUX_LR_STATS=1 logs call and frame rates plus run and video timings.
 * The timings exclude the pacing wait. */
static void run_stat(uint64_t t0, uint64_t tv, uint64_t t1, bool shown)
{
    static int on = -1;
    static uint64_t start, n, run_sum, run_max, vid_sum, vid_max, frames;
    if (on < 0) {
        const char *e = getenv("HAKUX_LR_STATS");
        on = e && atoi(e) != 0;
    }
    if (!on) {
        return;
    }
    start = start ? start : t0;
    n++;
    frames += shown;
    run_sum += t1 - t0;
    run_max = t1 - t0 > run_max ? t1 - t0 : run_max;
    vid_sum += t1 - tv;
    vid_max = t1 - tv > vid_max ? t1 - tv : vid_max;
    if (t1 - start >= 10000000000ull) {
        nc_log("hakuX-lr", "stats: %.2f retro_run/s, %.2f frames/s to RetroArch, %.2f ms each (max %.2f), "
               "video %.2f ms (max %.2f)", (double)n * 1e9 / (double)(t1 - start),
               (double)frames * 1e9 / (double)(t1 - start), (double)run_sum / (double)n / 1e6,
               (double)run_max / 1e6, (double)vid_sum / (double)n / 1e6, (double)vid_max / 1e6);
        start = t1;
        n = run_sum = run_max = vid_sum = vid_max = frames = 0;
    }
}

static void xbe_patches_changed(void);
static void online_notices(void);

/* Skipping a present can remove the frontend's vsync wait. Pace early
 * calls at the Xbox rate and restart the schedule after a late call.
 * Fast-forward, frame stepping and rewind bypass the wait. */
static uint64_t paced(uint64_t now)
{
    const uint64_t due = g_last_run_at + NC_LR_FRAME_NS;
    struct retro_throttle_state t = {RETRO_THROTTLE_NONE, 0.0f};
    if (!g_last_run_at || now >= due ||
        (g_env(RETRO_ENVIRONMENT_GET_THROTTLE_STATE, &t) &&
         (t.mode == RETRO_THROTTLE_FAST_FORWARD || t.mode == RETRO_THROTTLE_FRAME_STEPPING ||
          t.mode == RETRO_THROTTLE_REWINDING))) {
        g_last_run_at = now;
        return now;
    }
    const struct timespec ts = {(time_t)(due / 1000000000ull), (long)(due % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
    g_last_run_at = due;            /* Keep the deadline across sleep jitter. */
    return now_ns();
}

RETRO_API void retro_run(void)
{
    const uint64_t now = paced(now_ns());
    g_runs++;
    nc_lr_input_run(g_input_poll, g_input_state);
    nc_lr_mic_run();
    nc_dev_nv2a_display_vsync(now);         /* RetroArch's frame tick is the display's vsync */
    gap_tick(now);
    nc_lr_audio_run(g_audio_batch);
    const uint64_t tv = now_ns();
    const bool shown = nc_lr_video_run(g_video);
    run_stat(now, tv, now_ns(), shown);
    nc_lr_options_run();
    nc_lr_net_run();
    xbe_patches_changed();
    online_notices();

    /* A title's relaunch (XLaunchNewImage): the same game boots again, into
     * the XBE its launch data page names, in the process. */
    if (__atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) < 0 && g_core.kernel.relaunch && can_stop() &&
        !__atomic_load_n(&g_fail, __ATOMIC_ACQUIRE)) {
        nc_log("hakuX-lr", "relaunch: the %s boots again in RetroArch's process", g_dash ? "dashboard" : "game");
        boot_again("relaunch", true, NULL);
        return;
    }
    const char *why = __atomic_load_n(&g_fail, __ATOMIC_ACQUIRE);
    why = why ? why : __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) < 0 ? "the core stopped" : nc_hle_fatal();
    if (why && !g_shutdown_sent) {
        g_shutdown_sent = true;
        nc_log("hakuX-lr", "shutting down: %s", why);
        g_env(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

RETRO_API unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

/* Save states (LIBRETRO_SPEC.md, section 11): the path-based engine's state
 * (native/state) and its cache files, packed into RetroArch's buffer
 * (state.c). A load boots the state's XBE again in process, as a reset
 * does, and applies the state to it.
 *
 * RetroArch asks retro_serialize_size() right before each save, allocates
 * that many bytes for every state it holds (the one saved or loaded, the
 * undo copies of both), and writes that many to a file it keeps
 * uncompressed. One bound for every game is hundreds of MiB each time: a
 * state is 15 MiB (a game's first menu) to 250 MiB and more (Halo's cache
 * drive, its maps unpacked there), and no estimate short of storing the
 * cache files tells which. So the state is taken when RetroArch asks for
 * the size, the answer is its length, and the retro_serialize that follows
 * hands that state over: in the same frame for a save; for a load, whose
 * command asks first and saves its undo copy once the file is read, some
 * frames later, which is why a state taken is the next save's for
 * STATE_TAKEN_NS and not for its frame only (a load ends that, as handing
 * it over does). Asked when no state can be
 * taken (or by a feature that asks every frame, state_burst), the answer is
 * a nominal size, so that retro_serialize is called and says why not. */
#define STATE_NOMINAL  ((size_t)(NC_RAM_RETAIL + (32u << 20)))
#define STATE_TAKEN_NS 2000000000ull    /* a state taken for its size is that save's for this long */

static void state_said(const char *what, const char *err)
{
    static char text[384];
    snprintf(text, sizeof(text), "%s: %s", what, err);
    nc_log("hakuX-lr", "%s", text);
    message(text, 300);
}

/* Before a save or a load touches anything: the console has, or had since
 * the content opened, a network (Insignia, system link, a test mode). */
static bool state_net_blocked(const char *what)
{
    if (!g_net_seen && nc_net_mode() == NC_NET_OFF) {
        return false;
    }
    nc_log("hakuX-lr", "%s: refused, the console's network is or was on for this content", what);
    message("Save states require Network off. Close and reopen the game.", 300);
    return true;
}

/* RetroArch's rewind (and run-ahead, netplay) asks for a state every frame
 * or few, whatever the core's info file or its serialization quirks say:
 * each one stops the game for a tenth of a second and more. A third request
 * within half a second of frames of the one before is such a feature's, not
 * a player's, and they are refused at once until they have stopped for a
 * while. */
#define STATE_BURST_GAP 30u         /* retro_runs between two requests of a burst */
#define STATE_BURST_MAX 2u          /* requests of a burst served */
static uint64_t g_state_asked;      /* g_runs at the last retro_serialize */
static unsigned g_state_burst;

static bool state_burst(void)
{
    /* (frames ran in between: saves taken while paused are a player's) */
    if (g_state_asked && g_runs == g_state_asked && g_state_burst >= STATE_BURST_MAX) {
        return true;                    /* this frame's request again (its size, then its state) */
    }
    const bool close = g_state_asked && g_runs != g_state_asked && g_runs - g_state_asked < STATE_BURST_GAP;
    g_state_asked = g_runs ? g_runs : 1;
    g_state_burst = close ? g_state_burst + 1 : 0;
    if (g_state_burst < STATE_BURST_MAX) {
        return false;
    }
    if (g_state_burst == STATE_BURST_MAX) {
        nc_log("hakuX-lr", "save state: asked for every few frames (rewind, run-ahead or netplay): refused until "
               "that stops");
        message("hakuX-lr's save states are for saving by hand: turn off Rewind and Run-Ahead.", 600);
    }
    return true;
}

static void state_paths(char *dir, size_t nd, char *path, size_t np)
{
    snprintf(dir, nd, "%s/state-save", g_cache);
    snprintf(path, np, "%s/" NC_LR_STATE_FILE, dir);
}

/* The size RetroArch was last told still stands: of a state taken a moment
 * ago and not handed over yet, or handed over in this frame (asked again
 * after it, the answer is the same). */
static bool state_taken(void)
{
    return g_taken.size && (g_taken.have || g_taken.runs == g_runs) && now_ns() - g_taken.at_ns < STATE_TAKEN_NS;
}

RETRO_API size_t retro_serialize_size(void)
{
    char dir[PATH_MAX], path[PATH_MAX + 16];
    /* a game (not the dashboard, nor before a game is loaded) opened with
     * the network off; one that got the network at a later boot keeps a
     * size (it may not come back) and is refused when asked */
    if (!g_game[0] || g_net_opened) {
        return 0;
    }
    if (state_taken()) {
        return g_taken.size;            /* RetroArch asks three times for one save */
    }
    g_taken.have = false;
    g_taken.size = 0;
    if (g_net_seen || nc_net_mode() != NC_NET_OFF || __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) != 2 ||
        g_core.vm.ram_size != NC_RAM_RETAIL || state_burst()) {
        return STATE_NOMINAL;           /* retro_serialize says why not */
    }
    state_paths(dir, sizeof(dir), path, sizeof(path));
    const uint64_t t0 = now_ns();
    const char *err = nc_lr_state_dir(dir) ? nc_state_save(path) : "its folder can't be made";
    const size_t size = err ? 0 : nc_lr_state_packed(dir);
    if (!size) {
        nc_lr_state_dir(dir);
        nc_log("hakuX-lr", "save state: not taken for its size: %s", err ? err : "its files can't be read");
        return STATE_NOMINAL;           /* retro_serialize tries again, and says */
    }
    g_taken.have = true;
    g_taken.runs = g_runs;
    g_taken.at_ns = now_ns();
    g_taken.size = size;
    nc_log("hakuX-lr", "save state: taken for its size, %.1f MiB, in %.0f ms", (double)size / 1048576.0,
           (double)(g_taken.at_ns - t0) / 1e6);
    return size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    char dir[PATH_MAX], path[PATH_MAX + 16];
    const bool taken = g_taken.have && state_taken();
    g_taken.have = false;
    if (!taken && state_burst()) {
        return false;
    }
    if (state_net_blocked("Save state")) {
        return false;
    }
    if (__atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) != 2) {
        state_said("Save state", "the game isn't running");
        return false;
    }
    /* 128 MiB of RAM about doubles a state's memory part, for the few
     * titles that use it */
    if (g_core.vm.ram_size != NC_RAM_RETAIL) {
        state_said("Save state", "save states need the console's standard 64 MiB (the Console Memory option)");
        return false;
    }
    state_paths(dir, sizeof(dir), path, sizeof(path));
    const uint64_t t0 = now_ns();
    size_t used = 0;
    /* (not taken for this size: a frontend that asked earlier, or not at
     * all; the state has to fit what it gave) */
    const char *err = taken ? NULL : !nc_lr_state_dir(dir) ? "its folder can't be made" : nc_state_save(path);
    err = err ? err : nc_lr_state_pack(dir, data, size, &used);
    nc_lr_state_dir(dir);               /* the buffer has it all */
    if (err) {
        state_said("Save state", err);
        return false;
    }
    nc_log("hakuX-lr", "save state: %.1f MiB of %.1f, %s %.0f ms", (double)used / 1048576.0,
           (double)size / 1048576.0, taken ? "taken with its size; handed over in" : "in",
           (double)(now_ns() - t0) / 1e6);
    return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    char dir[PATH_MAX], path[PATH_MAX + 16];
    g_taken.have = false;               /* a state taken before this load isn't a later save's */
    g_taken.size = 0;
    if (state_net_blocked("Load state")) {
        return false;
    }
    if (!g_started || !can_stop()) {
        state_said("Load state", "the game isn't running");
        return false;
    }
    snprintf(dir, sizeof(dir), "%s/state-load", g_cache);
    snprintf(path, sizeof(path), "%s/" NC_LR_STATE_FILE, dir);
    const uint64_t t0 = now_ns();
    /* everything checked before the running game stops */
    const char *err = nc_lr_state_unpack(data, size, dir);
    err = err ? err : __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) == 2 ? nc_state_compat(path) : NULL;
    err = err ? err : nc_cache_state_check(path);
    if (err) {
        nc_lr_state_dir(dir);
        state_said("Load state", err);
        return false;
    }
    bool ok = boot_again("load state", false, path);
    /* the boot and the state's load are the core thread's: done when it runs */
    int st = 1;
    for (unsigned i = 0; ok && i < 3000 && (st = __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE)) == 1; i++) {
        usleep(10 * 1000);
    }
    ok = ok && st == 2;
    nc_lr_state_dir(dir);               /* applied, or of no use */
    if (!ok) {
        /* past the checks, with the game it replaced gone: the game starts
         * again from its disc rather than end */
        static char text[400];
        snprintf(text, sizeof(text), "%s The game starts again.",
                 nc_hle_fatal() ? nc_hle_fatal() : "The state did not load.");
        nc_log("hakuX-lr", "load state: %s", text);
        message(text, 420);
        boot_again("load state failed", false, NULL);
        return false;
    }
    nc_log("hakuX-lr", "load state: loaded in %.0f ms", (double)(now_ns() - t0) / 1e6);
    return true;
}

/* RetroArch's cheats (its Cheats menu, a .cht's codes): the engine's
 * frontend set (nc_cheat_front_set), apart from the mods' cheats, from the
 * next vblank. RetroArch sends them all again after each change (a reset,
 * then each cheat on), also before the guest boots; a reset or a relaunch
 * keeps them, unloading the game clears them. */
#define CHEAT_BAD_MAX 32
static uint64_t g_cheat_bad[CHEAT_BAD_MAX];    /* the codes refused, logged once each */
static unsigned g_ncheat_bad;

static bool cheat_told(const char *code)
{
    uint64_t h = 1469598103934665603ull;        /* FNV-1a */
    for (const char *p = code; *p; p++) {
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    }
    for (unsigned i = 0; i < g_ncheat_bad && i < CHEAT_BAD_MAX; i++) {
        if (g_cheat_bad[i] == h) {
            return true;
        }
    }
    g_cheat_bad[g_ncheat_bad++ % CHEAT_BAD_MAX] = h;
    return false;
}

static uint64_t g_xbe_told = UINT64_MAX;      /* the patch set told about (none: 0 is the empty set's) */
/* Defer the ignored-cheat notice until RetroArch has supplied the core options. */
static bool g_front_on, g_off_told;

static void cheats_forget(void)
{
    nc_cheat_front_reset();
    nc_xbe_patch_front_reset();
    g_ncheat_bad = 0;
    g_xbe_told = UINT64_MAX;
    g_front_on = g_off_told = false;
}

RETRO_API void retro_cheat_reset(void)
{
    nc_cheat_front_reset();
    nc_xbe_patch_front_reset();
    g_front_on = false;
}

/* Route XBE patches separately from memory cheats. */
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    char err[96];
    code = code ? code : "";
    g_front_on |= enabled && *code;
    const bool xbe = nc_xbe_patch_is(code);
    if (xbe) {
        nc_cheat_front_set(index, false, NULL, NULL, 0);
        if (!nc_xbe_patch_front_set(index, enabled, code, err, sizeof(err)) && !cheat_told(code)) {
            nc_log("hakuX-lr", "cheat %u: XBE patch \"%.60s%s\": %s; it stays off", index, code,
                   strlen(code) > 60 ? "..." : "", err);
        }
        return;
    }
    nc_xbe_patch_front_set(index, false, NULL, NULL, 0);
    if (!nc_cheat_front_set(index, enabled, code, err, sizeof(err)) && !cheat_told(code)) {
        nc_log("hakuX-lr", "cheat %u \"%s\": %s; it stays off (patches/mods/README.md has the codes)", index, code,
               err);
    }
}


static void online_notices(void)
{
    if (g_front_on && !g_off_told && !nc_online_cheats()) {
        g_off_told = true;
        message("Cheats are off in the core options: RetroArch's cheats and XBE patches are ignored", 360);
        nc_log("hakuX-lr", "Cheats are off: RetroArch's cheats and XBE patches are ignored");
    }
    const char *what = nc_online_take_notice();
    if (what && nc_net_mode() != NC_NET_OFF) {
        static char text[220];
        snprintf(text, sizeof(text), "Xbox Live is off until the game restarts: %s changed it. %s", what,
                 nc_online_cause() == NC_ONLINE_UNCAPPED
                     ? "Unset HAKUX_HLE_UNCAP to play on Insignia"
                     : "Turn its cheats and patches off (or the Cheats option) and restart the game to play on Insignia");
        message(text, 600);
    }
}

/* Show one restart reminder per change from the booted patch set. */
static void xbe_patches_changed(void)
{
    const uint64_t now = nc_xbe_patch_front_hash();
    if (now == nc_mods_xbe_patches_booted()) {
        g_xbe_told = UINT64_MAX;
        return;
    }
    if (now == g_xbe_told || __atomic_load_n(&g_core_state, __ATOMIC_ACQUIRE) != 2) {
        return;
    }
    g_xbe_told = now;
    message("XBE patches changed: restart the game (Quick Menu, Restart) to apply them", 360);
    nc_log("hakuX-lr", "XBE patches changed since the boot: they apply at the next restart");
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    return 0;
}
