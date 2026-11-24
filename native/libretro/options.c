/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Libretro core options (LIBRETRO_SPEC.md section 10).
 * RetroArch handles display filters and aspect ratio. Crop, rumble,
 * frame pacing and frame repeats update live. Other options apply at boot.
 */
#include "native/libretro/libretro_int.h"
#include "native/libretro/widescreen_titles.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/core/online.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"
#include "native/dev/dev.h"
#include "native/dev/usb.h"
#include "hw/xbox/hle/accuracy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEXT " Restart the game to apply."

/* Audio Processing's Auto: Full DSP where the build has the DSP JIT, except
 * at the Potato accuracy level. The DSP's frame thread is the core's largest
 * cost after the guest CPU (a third to half of a core on a Snapdragon 8 Gen
 * 2 in games), more than a low-end device has left. */
#if NC_HAVE_DSP_JIT
#define AUDIO_PROCESSING_AUTO_DSP 1
#else
#define AUDIO_PROCESSING_AUTO_DSP 0
#endif

static bool dmi_is(const char *dir, const char *file, const char *want)
{
    char path[256], line[64] = "";
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    const bool got = fgets(line, sizeof(line), fp) != NULL;
    fclose(fp);
    line[strcspn(line, "\r\n")] = 0;
    return got && !strcmp(line, want);
}

/* Valve uses Jupiter for the LCD Deck and Galileo for the OLED Deck. */
static bool steam_deck(const char *dir)
{
    return dmi_is(dir, "sys_vendor", "Valve") &&
           (dmi_is(dir, "product_name", "Jupiter") || dmi_is(dir, "product_name", "Galileo"));
}

#if defined(__x86_64__) && !defined(__ANDROID__)
#define COMBINER_PRECISION_DEFAULT "enabled"
#else
#define COMBINER_PRECISION_DEFAULT "disabled"
#endif

enum { OPT_SCALE, OPT_ACCURACY, OPT_CROP, OPT_FORMAT, OPT_480P, OPT_720P, OPT_SCALE_720P, OPT_LANGUAGE, OPT_REGION, OPT_RUMBLE, OPT_NETWORK,
       OPT_COMMUNICATOR, OPT_MIC_GATE, OPT_RESAMPLER, OPT_PROCESSING, OPT_PRECISION, OPT_PACE,
       OPT_REPEAT, OPT_MODS, OPT_RAM, OPT_RAM_PATCH, OPT_COUNT };

/* The language values follow XC_LANGUAGE after auto: english is 1. */
static struct retro_core_option_v2_definition g_defs[OPT_COUNT + 1] = {
    [OPT_SCALE] = {
        "hakux_render_scale", "Render Scale", NULL,
        "1x uses the Xbox's native resolution (usually 640x480). The default, 2x, doubles it. "
        "Higher values sharpen the picture but need more GPU power." NEXT,
        NULL, NULL,
        {{"1", "1x (Xbox)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {NULL, NULL}},
        "2"},
    /* The values in HleAccuracyLevel's order. Custom is the default while
     * existing set-ups move over: it keeps Combiner Precision as it is set. */
    [OPT_ACCURACY] = {
        "hakux_accuracy_level", "Accuracy Level", NULL,
        "Selects graphics accuracy trade-offs. Potato permits approximations for performance and may "
        "cause visual errors or compatibility problems. Medium is recommended for general play. High "
        "enables all implemented graphics accuracy features supported by the device. Custom uses the "
        "individual settings (Combiner Precision). Render scale is separate; Audio Processing's Auto "
        "uses Fast at Potato." NEXT,
        NULL, NULL,
        {{"potato", "Potato - Performance"}, {"medium", "Medium - Balanced"}, {"high", "High - Accuracy"},
         {"custom", "Custom - Individual settings"}, {NULL, NULL}},
        "custom"},
    [OPT_CROP] = {
        "hakux_crop_overscan", "Crop Overscan", NULL,
        "Removes 3.75% from each edge to hide the TV overscan border. Off shows the full frame.",
        NULL, NULL,
        {{"enabled", "On"}, {"disabled", "Off"}, {NULL, NULL}},
        "enabled"},
    [OPT_FORMAT] = {
        "hakux_screen_format", "Screen Format", NULL,
        "Sets the console's screen format. Widescreen uses 16:9; Letterbox fits 16:9 inside 4:3. "
        "Only games that support these formats use them. Auto uses Widescreen for games known to "
        "support it unless the display is narrower than 16:10, and while a widescreen patch (Cheats) "
        "is applied to the game; Normal otherwise." NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"normal", "Normal (4:3)"}, {"widescreen", "Widescreen (16:9)"},
         {"letterbox", "Letterbox"}, {NULL, NULL}},
        "auto"},
    [OPT_480P] = {
        "hakux_video_480p", "480p", NULL,
        "Allow games to select 480p." NEXT,
        NULL, NULL,
        {{"enabled", "On"}, {"disabled", "Off"}, {NULL, NULL}},
        "enabled"},
    [OPT_720P] = {
        "hakux_video_720p", "720p", NULL,
        "Allow games to select 720p (1280x720, 16:9)." NEXT,
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_SCALE_720P] = {
        "hakux_render_scale_720p", "Render Scale (720p)", NULL,
        "Render scale for games running in 720p. 3x is 3840x2160." NEXT,
        NULL, NULL,
        {{"1", "1x (1280x720)"}, {"2", "2x"}, {"3", "3x"}, {NULL, NULL}},
        "1"},
    [OPT_LANGUAGE] = {
        "hakux_language", "Language", NULL,
        "Sets the language in games that support it. Auto uses RetroArch's language, "
        "falling back to English if the Xbox does not support it." NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"english", "English"}, {"japanese", "Japanese"}, {"german", "German"},
         {"french", "French"}, {"spanish", "Spanish"}, {"italian", "Italian"}, {"korean", "Korean"},
         {"chinese", "Chinese (Traditional)"}, {"portuguese", "Portuguese"}, {NULL, NULL}},
        "auto"},
    [OPT_REGION] = {
        "hakux_video_region", "Video Region", NULL,
        "Sets the console's TV standard. Auto selects PAL for European games and NTSC-M otherwise. "
        "PAL games use 60 Hz when supported; 50 Hz games keep their speed but may look less smooth."
        NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"ntsc-m", "NTSC-M (Americas)"}, {"ntsc-j", "NTSC-J (Japan)"},
         {"pal", "PAL (Europe)"}, {NULL, NULL}},
        "auto"},
    [OPT_RUMBLE] = {
        "hakux_rumble", "Rumble", NULL,
        "Enables vibration when supported by RetroArch and the controller.",
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_NETWORK] = {
        "hakux_network", "Network", NULL,
        "Plugs in the console's network cable, as on a home network: Xbox Live through Insignia, the Xbox Live "
        "revival, and system link with the consoles on this network, at once. For Xbox Live, start the core "
        "without a game, open Xbox Live in the dashboard, register the console, then create an account there. "
        "System link: turn the network on on each device; "
        "discovery uses UDP port 3075. For consoles on another subnet or a VPN, add their addresses to "
        "lan-peers.txt in the save directory. If your provider blocks Insignia's DNS server, put the address of "
        "an insigniaDNS server in insignia-dns.txt in the save directory." NEXT,
        NULL, NULL,
        {{"disabled", "Off (no cable)"}, {"enabled", "On"}, {NULL, NULL}},
        "enabled"},
    [OPT_COMMUNICATOR] = {
        "hakux_communicator", "Xbox Live Communicator", NULL,
        "Plugs a Communicator headset into player 1's controller, for voice chat over Xbox Live (Insignia) or "
        "system link in games that have it. It hears RetroArch's microphone (Settings > Audio > Microphone), "
        "silence without one, and its headphone plays with the game's sound. The microphone is used only while "
        "a game's voice chat runs. Experimental." NEXT,
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_MIC_GATE] = {
        "hakux_mic_sensitivity", "Communicator Microphone Sensitivity", NULL,
        "With the Communicator on: how quiet a sound still counts as talking. Quieter sounds, and sounds that "
        "aren't a voice (buttons, sticks, a fan), are muted, so the game doesn't show you as talking for them. "
        "Always on sends everything the microphone hears." NEXT,
        NULL, NULL,
        {{"medium", "Medium"}, {"low", "Low (only clear speech)"}, {"high", "High (quiet voices too)"},
         {"off", "Always on (no noise gate)"}, {NULL, NULL}},
        "medium"},
    [OPT_RESAMPLER] = {
        "hakux_audio_resampler", "Audio Resampler", NULL,
        "Converts game audio to 48 kHz. Hardware interpolates as the Xbox does. Sinc preserves "
        "high frequencies and reduces aliasing. Linear uses the least CPU but softens high "
        "frequencies." NEXT,
        NULL, NULL,
        {{"hardware", "Hardware (as the Xbox)"}, {"sinc", "Sinc"}, {"linear", "Linear"}, {NULL, NULL}},
        "hardware"},
    [OPT_PROCESSING] = {
        "hakux_audio_processing", "Audio Processing", NULL,
        "Full DSP runs the game's Xbox reverb and other audio effects with stereo output. "
        "It uses more CPU. Fast plays the dry mix; try it if Full DSP crackles or runs slowly. "
        "Auto uses Fast at the Potato accuracy level, on a Steam Deck and where Full DSP is not "
        "available, Full DSP otherwise." NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"fast", "Fast (dry mix)"}, {"dsp", "Full DSP (experimental)"}, {NULL, NULL}},
        "auto"},
    [OPT_PRECISION] = {
        "hakux_combiner_precision", "Combiner Precision", NULL,
        "Applies only when Accuracy Level is Custom. Accurate rounds pixel-shader combiner outputs "
        "like the Xbox. Fast uses floating-point outputs and may improve GPU performance." NEXT,
        NULL, NULL,
        {{"disabled", "Fast"}, {"enabled", "Accurate"}, {NULL, NULL}},
        COMBINER_PRECISION_DEFAULT},
    /* The value is the decoder's depth: the swaps back it waits for the GPU
     * to finish (pb/pbdraw.c g_pace). */
    [OPT_PACE] = {
        "hakux_frame_pace", "Frame Pacing", NULL,
        "Holds the game to the GPU's rendering pace when the GPU can't keep up, instead of letting "
        "the game run ahead of the picture and add input lag. On keeps up to two frames waiting for "
        "the GPU; Lowest Latency keeps one, which may lower the frame rate. Off lets the game run ahead.",
        NULL, NULL,
        {{"disabled", "Off"}, {"2", "On"}, {"1", "Lowest Latency"}, {NULL, NULL}},
        "disabled"},
    [OPT_REPEAT] = {
        "hakux_repeat_frames", "Repeat Frames", NULL,
        "Send the previous frame again when the game has no new one. Enable for RetroArch "
        "recording, streaming or black-frame insertion. FPS counters include repeats.",
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_MODS] = {
        "hakux_cheats", "Cheats", NULL,
        "Applies RetroArch's cheats and XBE patches (such as 60 fps patches). Off ignores them all. On, a "
        "game they change can't use Xbox Live (Insignia) until it restarts: they give an unfair advantage "
        "or desync real consoles. System link still works." NEXT,
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_RAM] = {
        "hakux_console_ram", "Console Memory", NULL,
        "Sets console RAM. 64 MB matches a retail Xbox. 128 MB and 256 MB provide extra memory for homebrew "
        "and compatible games. Most games keep their 64 MB limit unless Patch Games for Console Memory is "
        "enabled." NEXT,
        NULL, NULL,
        {{"64", "64 MB (retail)"}, {"128", "128 MB"}, {"256", "256 MB"}, {NULL, NULL}},
        "64"},
    [OPT_RAM_PATCH] = {
        "hakux_ram_patch", "Patch Games for Console Memory", NULL,
        "Removes the game's 64 MB limit when using 128 MB or 256 MB of console RAM. Some games may not work "
        "with extra memory. Game files are not changed." NEXT,
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    {NULL},
};

_Static_assert(HLE_ACCURACY_POTATO == 0 && HLE_ACCURACY_MEDIUM == 1 && HLE_ACCURACY_HIGH == 2 &&
               HLE_ACCURACY_CUSTOM == 3, "hakux_accuracy_level's values are in HleAccuracyLevel's order");

static int g_applied[OPT_COUNT];   /* the value each option booted with (its index) */

/* Frame pacing's depth for the option's value index (0: off), and as the
 * log says it. */
static int pace_depth(int i)
{
    return atoi(g_defs[OPT_PACE].values[i].value);
}

static const char *pace_text(int i)
{
    static char text[16];
    if (!pace_depth(i)) {
        return "off";
    }
    snprintf(text, sizeof(text), "%d back", pace_depth(i));
    return text;
}

/* A frontend without the v2 options (or v1, which adds nothing used here):
 * the legacy variables, "Description; default|other|...". */
static void declare_legacy(retro_environment_t env)
{
    static char text[OPT_COUNT][512];
    static struct retro_variable vars[OPT_COUNT + 1];
    for (int i = 0; i < OPT_COUNT; i++) {
        const struct retro_core_option_v2_definition *d = &g_defs[i];
        size_t n = (size_t)snprintf(text[i], sizeof(text[i]), "%s; %s", d->desc, d->default_value);
        for (const struct retro_core_option_value *v = d->values; v->value && n < sizeof(text[i]); v++) {
            if (strcmp(v->value, d->default_value)) {
                n += (size_t)snprintf(text[i] + n, sizeof(text[i]) - n, "|%s", v->value);
            }
        }
        vars[i] = (struct retro_variable){d->key, text[i]};
    }
    env(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

void nc_lr_options_declare(retro_environment_t env)
{
    static struct retro_core_option_v2_category none[] = {{NULL, NULL, NULL}};
    static struct retro_core_options_v2 opts = {none, g_defs};
    unsigned version = 0;
    if (!env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2 ||
        !env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts)) {
        declare_legacy(env);
    }
}

/* The option's value now, as the index of one of its values (the default's
 * for one the frontend doesn't have, or doesn't know). */
static int value_of(int opt)
{
    const struct retro_core_option_v2_definition *d = &g_defs[opt];
    struct retro_variable var = {d->key, NULL};
    const char *want = nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : d->default_value;
    int def = 0;
    for (int i = 0; d->values[i].value; i++) {
        if (!strcmp(d->values[i].value, want)) {
            return i;
        }
        def = strcmp(d->values[i].value, d->default_value) ? def : i;
    }
    return def;
}

/* Auto: RetroArch's language as XC_LANGUAGE, English where the Xbox has none. */
static int auto_language(void)
{
    unsigned lang = RETRO_LANGUAGE_ENGLISH;
    nc_lr_env(RETRO_ENVIRONMENT_GET_LANGUAGE, &lang);
    switch (lang) {
    case RETRO_LANGUAGE_JAPANESE:
        return 2;
    case RETRO_LANGUAGE_GERMAN:
        return 3;
    case RETRO_LANGUAGE_FRENCH:
        return 4;
    case RETRO_LANGUAGE_SPANISH:
        return 5;
    case RETRO_LANGUAGE_ITALIAN:
        return 6;
    case RETRO_LANGUAGE_KOREAN:
        return 7;
    case RETRO_LANGUAGE_CHINESE_TRADITIONAL:
    case RETRO_LANGUAGE_CHINESE_SIMPLIFIED:
        return 8;
    case RETRO_LANGUAGE_PORTUGUESE_BRAZIL:
    case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL:
        return 9;
    default:
        return 1;
    }
}

/* Combiner Precision is the level's to decide unless the level is Custom:
 * hidden then, where the frontend can hide an option (its help says so
 * where it can't). By the level selected now, which a restart applies. */
static void display_update(void)
{
    struct retro_core_option_display d = {g_defs[OPT_PRECISION].key,
                                          value_of(OPT_ACCURACY) == HLE_ACCURACY_CUSTOM};
    nc_lr_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
}

/* Screen Format's Auto: each XBE boot tells whether a widescreen patch was
 * applied to it and which title it is (mods.c), before the game reads the
 * console's settings. Widescreen with such a patch, as the user set it up;
 * and for a title known to draw a 16:9 picture (widescreen_titles.h) unless
 * the display is known not to be wide. A game doesn't say so itself: in 480p the display
 * mode it asks for is the same either way (on a standard-definition cable
 * the mode's bit 28 tells), hence the list. */
static bool g_format_auto;              /* atomic */
static uint32_t g_video_base;           /* HDTV and PAL60 flags shared by all screen formats. */
static int g_display_wide = -2;         /* atomic: the frontend's display is 16:10 or wider; -1 not known
                                           (-2: not asked yet) */

static bool widescreen_title(uint32_t title_id)
{
    size_t lo = 0, hi = sizeof(k_widescreen_titles) / sizeof(k_widescreen_titles[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (k_widescreen_titles[mid] == title_id) {
            return true;
        }
        if (k_widescreen_titles[mid] < title_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* The display's shape, where the frontend tells: a 16:9 picture on a 4:3
 * display would only be the smaller for it. Not every frontend does
 * (RetroArch up to 1.22.2 doesn't): a display not known is taken as wide, as
 * nearly every one is, and Normal remains to be set by hand on one that
 * isn't. */
static void display_query(void)
{
    struct retro_display_metadata m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.size = sizeof(m);
    int wide = -1;
    if (nc_lr_env(RETRO_ENVIRONMENT_GET_DISPLAY_METADATA, &m) && m.geo.native_width && m.geo.native_height) {
        wide = (uint64_t)m.geo.native_width * 10 >= (uint64_t)m.geo.native_height * 16;
    }
    if (wide != __atomic_exchange_n(&g_display_wide, wide, __ATOMIC_ACQ_REL)) {
        if (wide < 0) {
            nc_log("hakuX-lr", "display: its size isn't known (taken as wide by Screen Format's Auto)");
        } else {
            nc_log("hakuX-lr", "display: %u x %u, %s", m.geo.native_width, m.geo.native_height,
                   wide ? "wide" : "not wide");
        }
    }
}

static void screen_auto(bool patch, uint32_t title_id)
{
    if (!__atomic_load_n(&g_format_auto, __ATOMIC_ACQUIRE)) {
        return;
    }
    const bool listed = widescreen_title(title_id);
    const int wide = __atomic_load_n(&g_display_wide, __ATOMIC_ACQUIRE);
    const bool widescreen = patch || (listed && wide != 0);
    nc_kernel_set_video(g_video_base | (widescreen ? NC_VIDEO_WIDESCREEN : 0), NC_AV_PACK_HDTV);
    nc_lr_video_set_widescreen(widescreen);
    nc_log("hakuX-lr", "screen format auto: %s (title %08X: %s)", widescreen ? "widescreen" : "normal", title_id,
           patch              ? "a widescreen patch is applied"
           : !listed          ? "not a known widescreen title"
           : wide > 0         ? "a widescreen title, the display is wide"
           : wide == 0        ? "a widescreen title, but the display is not wide"
                              : "a widescreen title, the display's size isn't known");
}

void nc_lr_options_apply(void)
{
    for (int i = 0; i < OPT_COUNT; i++) {
        g_applied[i] = value_of(i);
    }
    const int scale = atoi(g_defs[OPT_SCALE].values[g_applied[OPT_SCALE]].value);
    nc_hle_set_render_scale(scale);
    const int scale_720p = atoi(g_defs[OPT_SCALE_720P].values[g_applied[OPT_SCALE_720P]].value);
    nc_hle_set_render_scale_720p(scale_720p);
    const bool crop = strcmp(g_defs[OPT_CROP].values[g_applied[OPT_CROP]].value, "disabled") != 0;
    nc_lr_video_set_crop(crop);
    const bool rumble = strcmp(g_defs[OPT_RUMBLE].values[g_applied[OPT_RUMBLE]].value, "enabled") == 0;
    nc_lr_input_set_rumble(rumble);
    nc_lr_video_set_repeat(!strcmp(g_defs[OPT_REPEAT].values[g_applied[OPT_REPEAT]].value, "enabled"));
    nc_online_set_cheats(!strcmp(g_defs[OPT_MODS].values[g_applied[OPT_MODS]].value, "enabled"));
    nc_core_set_ram((unsigned)atoi(g_defs[OPT_RAM].values[g_applied[OPT_RAM]].value));
    nc_xbe_set_unlimit(!strcmp(g_defs[OPT_RAM_PATCH].values[g_applied[OPT_RAM_PATCH]].value, "enabled"));

    /* ex.c clears PAL60 for NTSC consoles. */
    const char *format = g_defs[OPT_FORMAT].values[g_applied[OPT_FORMAT]].value;
    const bool p480 = !strcmp(g_defs[OPT_480P].values[g_applied[OPT_480P]].value, "enabled");
    const bool p720 = !strcmp(g_defs[OPT_720P].values[g_applied[OPT_720P]].value, "enabled");
    g_video_base = NC_VIDEO_PAL60 | (p480 ? NC_VIDEO_480P : 0) | (p720 ? NC_VIDEO_720P : 0);
    const uint32_t flags = g_video_base | (!strcmp(format, "widescreen") ? NC_VIDEO_WIDESCREEN
                                           : !strcmp(format, "letterbox") ? NC_VIDEO_LETTERBOX
                                                                          : 0);
    nc_kernel_set_video(flags, NC_AV_PACK_HDTV);
    nc_lr_video_set_widescreen(flags & NC_VIDEO_WIDESCREEN);
    /* Auto: normal until the booting XBE says widescreen (screen_auto) */
    display_query();
    __atomic_store_n(&g_format_auto, !strcmp(format, "auto"), __ATOMIC_RELEASE);
    nc_mods_set_screen_hook(screen_auto);

    const int language = g_applied[OPT_LANGUAGE] ? g_applied[OPT_LANGUAGE] : auto_language();
    nc_kernel_set_language(language);
    /* Always emulate console drive timing, as upstream's release app does
     * (only debug builds can toggle it). Disc and hard disk reads take at
     * least as long as on the console; instant loads froze Morrowind when
     * leaving a building. NC_DEVTIME tunes the timing. */
    nc_io_set_device_time(true);

    const char *region = g_defs[OPT_REGION].values[g_applied[OPT_REGION]].value;
    nc_kernel_set_av_region(!strcmp(region, "ntsc-m")   ? NC_AV_REGION_NTSC_M
                            : !strcmp(region, "ntsc-j") ? NC_AV_REGION_NTSC_J
                            : !strcmp(region, "pal")    ? NC_AV_REGION_PAL_I
                                                        : 0);

    const char *network = g_defs[OPT_NETWORK].values[g_applied[OPT_NETWORK]].value;
    nc_lr_net_apply(network);
    /* the noise gate's threshold in dBFS, as the app's sensitivities */
    const bool communicator = !strcmp(g_defs[OPT_COMMUNICATOR].values[g_applied[OPT_COMMUNICATOR]].value, "enabled");
    const char *sensitivity = g_defs[OPT_MIC_GATE].values[g_applied[OPT_MIC_GATE]].value;
    const char *gate = !strcmp(sensitivity, "low")    ? "-30"
                       : !strcmp(sensitivity, "high") ? "-46"
                       : !strcmp(sensitivity, "off")  ? "off"
                                                      : "-38";
    nc_dev_ohci_set_voice(communicator ? 1 : 0);
    nc_xblc_set_gate(gate);
    const char *resampler = g_defs[OPT_RESAMPLER].values[g_applied[OPT_RESAMPLER]].value;
    nc_dev_apu_set_resampler(resampler);
    nc_hle_set_frame_pace(pace_depth(g_applied[OPT_PACE]));
    /* The accuracy level, resolved once for the run (accuracy.h). Custom:
     * the individual settings, which are Combiner Precision and, for the
     * rest, every path on, as before the level existed. */
    const HleAccuracyLevel level = (HleAccuracyLevel)g_applied[OPT_ACCURACY];
    const char *processing = g_defs[OPT_PROCESSING].values[g_applied[OPT_PROCESSING]].value;
    const bool deck = steam_deck("/sys/class/dmi/id");
    const bool dsp = !strcmp(processing, "dsp") ||
                     (!strcmp(processing, "auto") && AUDIO_PROCESSING_AUTO_DSP && level != HLE_ACCURACY_POTATO && !deck);
    nc_dev_apu_set_dsp(dsp);
    HleGraphicsAccuracy custom = hle_accuracy_default();
    custom.precise_combiners = !strcmp(g_defs[OPT_PRECISION].values[g_applied[OPT_PRECISION]].value, "enabled");
    const HleGraphicsAccuracy acc = hle_accuracy_resolve(level, &custom);
    hle_accuracy_set(level, &acc);
    const bool precise = acc.precise_combiners;
    display_update();
    nc_log("hakuX-lr", "options: render scale %dx, overscan %s, screen %s, HDTV modes%s%s%s, language %d (%s), video region %s, "
           "rumble %s, network %s, resampler %s, audio processing %s, frame pacing %s, communicator %s%s%s, "
           "accuracy %s (combiners %s, signed blending %s, post-blend dithering %s), render scale in 720p %dx",
           scale, crop ? "cropped" : "shown", format, p480 ? " 480p" : "",
           p720 ? " 720p" : "", p480 || p720 ? "" : " off", language,
           g_defs[OPT_LANGUAGE].values[g_applied[OPT_LANGUAGE]].value, region, rumble ? "on" : "off",
           strcmp(network, "disabled") ? "on" : "off", resampler,
           dsp ? "dsp" : deck && !strcmp(processing, "auto") ? "fast (Auto on a Steam Deck)" : "fast",
           pace_text(g_applied[OPT_PACE]), communicator ? "on (noise gate " : "off", communicator ? gate : "",
           communicator ? ")" : "", hle_accuracy_level_name(level), precise ? "accurate" : "fast",
           acc.precise_signed_blend ? "accurate" : "approximate",
           acc.precise_postblend_dither ? "accurate" : "approximate", scale_720p);
}

void nc_lr_options_run(void)
{
    bool changed = false;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) || !changed) {
        return;
    }
    display_update();
    const int crop = value_of(OPT_CROP);
    if (crop != g_applied[OPT_CROP]) {
        g_applied[OPT_CROP] = crop;
        nc_lr_video_set_crop(strcmp(g_defs[OPT_CROP].values[crop].value, "disabled") != 0);
    }
    const int rumble = value_of(OPT_RUMBLE);
    if (rumble != g_applied[OPT_RUMBLE]) {
        g_applied[OPT_RUMBLE] = rumble;
        nc_lr_input_set_rumble(strcmp(g_defs[OPT_RUMBLE].values[rumble].value, "enabled") == 0);
    }
    /* the decoder takes it at its next swap; a stall it is in ends as it would */
    const int pace = value_of(OPT_PACE);
    if (pace != g_applied[OPT_PACE]) {
        g_applied[OPT_PACE] = pace;
        nc_hle_set_frame_pace(pace_depth(pace));
        nc_log("hakuX-lr", "options: frame pacing %s", pace_text(pace));
    }
    const int repeat = value_of(OPT_REPEAT);
    if (repeat != g_applied[OPT_REPEAT]) {
        g_applied[OPT_REPEAT] = repeat;
        nc_lr_video_set_repeat(!strcmp(g_defs[OPT_REPEAT].values[repeat].value, "enabled"));
    }
    char names[256] = "";
    size_t n = 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (i != OPT_CROP && i != OPT_RUMBLE && i != OPT_PACE && i != OPT_REPEAT && value_of(i) != g_applied[i] &&
            n < sizeof(names)) {
            n += (size_t)snprintf(names + n, sizeof(names) - n, "%s%s", n ? ", " : "", g_defs[i].desc);
        }
    }
    if (!n) {
        return;
    }
    static char text[320];
    snprintf(text, sizeof(text), "%s: restart the game to apply", names);
    const struct retro_message msg = {text, 240};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
    nc_log("hakuX-lr", "options: %s", text);
}
