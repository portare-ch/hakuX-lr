/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stereo voice-processor output (MON_VP), used by default without the DSP.
 *
 * Route 2D speaker sends by DirectSound's bin conventions. Front left and right go to their
 * speakers at full gain. Centre goes to both at -3 dB. Surround sends go to the matching side
 * at -3 dB. Bins 6–9 feed the corresponding front and surround positions. LFE, I3DL2, effect
 * and multipass sends are excluded from this speaker mix.
 *
 * Choose the route on the first frame with an audible send. A speaker send selects the
 * speaker mix. If all audible sends go elsewhere, use the monitor mix so the voice remains
 * audible. Keep the route until VOICE_ON, even when send gains change. This lets a dry send
 * fade out beside an effect send. Multipass groups use the monitor mix.
 *
 * The gains follow DirectSound's DSP image measured in Halo (2026-10-30). EP's stereo matrix
 * at Y:0000–0009 is in AC-3 order L, C, R, Ls, Rs. The measured speaker gains were 0 and
 * -3.01 dB at 100 Hz, 1 kHz and 8 kHz. At 1 kHz the GP mapped bins 6–9 to 0, 1, 4, 5 at -0.03
 * dB, with an opposite-channel term at -24.4 dB. This mix omits that crosstalk-cancelling
 * term.
 *
 * For 3D voices, send slots 0–1 carry the left ear and slots 2–3 the right (xemu #3105).
 * SET_HRTF_SUBMIXES names the destinations. In DOA3 and The Thing, 0x09070806 selects 6, 8,
 * 7, 9, putting each ear in its own front and surround bins. Apply the headroom shift from
 * the VP mix-bin path.
 */
#ifndef NATIVE_DEV_APU_VP_STEREO_ROUTE_H
#define NATIVE_DEV_APU_VP_STEREO_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "native/dev/apu/apu_regs.h"

#define VP_BIN_FRONT_LEFT   0   /* DSMIXBIN_FRONT_LEFT */
#define VP_BIN_FRONT_RIGHT  1   /* DSMIXBIN_FRONT_RIGHT */
#define VP_BIN_FRONT_CENTER 2   /* DSMIXBIN_FRONT_CENTER */
#define VP_BIN_LFE          3   /* DSMIXBIN_LOW_FREQUENCY */
#define VP_BIN_BACK_LEFT    4   /* DSMIXBIN_BACK_LEFT */
#define VP_BIN_BACK_RIGHT   5   /* DSMIXBIN_BACK_RIGHT */
#define VP_BIN_3D_FRONT_LEFT  6 /* DirectSound's crosstalk bins (XTLK), the 3D voices' */
#define VP_BIN_3D_FRONT_RIGHT 7
#define VP_BIN_3D_BACK_LEFT   8
#define VP_BIN_3D_BACK_RIGHT  9
#define VP_SENDS            8
#define VP_DOWNMIX_SIDE     0.70710678f     /* -3 dB */

/* Sources: two voice channels, two HRTF ears and the I3DL2 room path. */
#define VP_SOURCES 5
#define VP_SOURCE_EARS 2
#define VP_SOURCE_ROOM 4

/* Route 3D sends 0–3 to the HRTF ears and I3DL2 sends 5 and 7 to the room.
 * Other sends select a voice channel by slot modulo channel count. */
static inline unsigned vp_send_channel(int slot, bool three_d, bool room, unsigned channels)
{
    if (three_d && slot < 4) {
        return VP_SOURCE_EARS + ((unsigned)slot >> 1);
    }
    if (room && (slot == 5 || slot == 7)) {
        return VP_SOURCE_ROOM;
    }
    return (unsigned)slot % channels;
}

/* A 2D voice's route in the VP's own mix, a sound's (vp.c) */
enum { VP_ROUTE_UNDECIDED = 0, VP_ROUTE_SPEAKERS = 1, VP_ROUTE_MONITOR = 2 };

/* A bin's share of the left and right speakers in the stereo downmix;
 * false for a bin that isn't heard on them (the LFE, and every bin past the
 * 3D ones) */
static inline bool vp_downmix_weights(int bin, float w[2])
{
    switch (bin) {
    case VP_BIN_FRONT_LEFT:
    case VP_BIN_3D_FRONT_LEFT:
        w[0] = 1.0f, w[1] = 0.0f;
        return true;
    case VP_BIN_FRONT_RIGHT:
    case VP_BIN_3D_FRONT_RIGHT:
        w[0] = 0.0f, w[1] = 1.0f;
        return true;
    case VP_BIN_FRONT_CENTER:
        w[0] = w[1] = VP_DOWNMIX_SIDE;
        return true;
    case VP_BIN_BACK_LEFT:
    case VP_BIN_3D_BACK_LEFT:
        w[0] = VP_DOWNMIX_SIDE, w[1] = 0.0f;
        return true;
    case VP_BIN_BACK_RIGHT:
    case VP_BIN_3D_BACK_RIGHT:
        w[0] = 0.0f, w[1] = VP_DOWNMIX_SIDE;
        return true;
    default:
        return false;
    }
}

/* Choose the speaker mix if any audible send targets a speaker bin. Use the monitor mix for
 * other audible sends, or leave the route undecided when all sends are muted. att[] contains
 * attenuate(volume) for each send, with zero for mute. */
static inline int vp_stereo_classify(const int bin[VP_SENDS], const float att[VP_SENDS])
{
    int route = VP_ROUTE_UNDECIDED;
    float w[2];
    for (int b = 0; b < VP_SENDS; b++) {
        if (att[b] != 0.0f) {
            if (vp_downmix_weights(bin[b], w)) {
                return VP_ROUTE_SPEAKERS;
            }
            route = VP_ROUTE_MONITOR;
        }
    }
    return route;
}

/* The gains of a speaker-routed voice's sends to the two speakers: g[o][s]
 * from source s (vp_send_channel) to output o (0 left, 1 right), each
 * speaker send's ea * attenuation / 2^shift[send] (its headroom) added in by
 * its downmix weights; every gain 0 when its speaker sends are muted.
 * channels: the voice's; three_d: a 3D voice, its first four sends its
 * ears'; room: its I3DL2 room path on sends 5 and 7. */
static inline void vp_stereo_gains(const int bin[VP_SENDS], const float att[VP_SENDS],
                                   const uint8_t shift[VP_SENDS], unsigned channels, bool three_d,
                                   bool room, float ea, float g[2][VP_SOURCES])
{
    float w[2];
    for (int s = 0; s < VP_SOURCES; s++) {
        g[0][s] = g[1][s] = 0.0f;
    }
    for (int b = 0; b < VP_SENDS; b++) {
        if (att[b] == 0.0f || !vp_downmix_weights(bin[b], w)) {
            continue;
        }
        const float a = ea * att[b] / (float)(1u << shift[b]);
        const unsigned src = vp_send_channel(b, three_d, room, channels);
        g[0][src] += a * w[0];
        g[1][src] += a * w[1];
    }
}

/* The voice's frame into the speakers by its gains (vp_stereo_gains), added
 * to out: a mono voice's one channel, a stereo voice's two, a 3D voice's
 * ears and room path (ears, room: NULL without). The gains move from
 * the last frame's (from) to this frame's (to) across the frame, sample i
 * i/32 of the way, as the routes' gains do in the mix bins (without
 * their rounding to 1/16384). */
static inline void vp_stereo_mix_ramp(float out[NUM_SAMPLES_PER_FRAME][2],
                                      const float in[NUM_SAMPLES_PER_FRAME][2],
                                      const float (*ears)[2], const float *room,
                                      const float from[2][VP_SOURCES],
                                      const float to[2][VP_SOURCES], unsigned channels)
{
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        const float t = i / (float)NUM_SAMPLES_PER_FRAME;
        float g[2][VP_SOURCES];
        for (int o = 0; o < 2; o++) {
            for (int s = 0; s < VP_SOURCES; s++) {
                g[o][s] = from[o][s] + (to[o][s] - from[o][s]) * t;
            }
        }
        float l = g[0][0] * in[i][0], r = g[1][0] * in[i][0];
        if (channels == 2) {
            l += g[0][1] * in[i][1];
            r += g[1][1] * in[i][1];
        }
        if (ears) {
            l += g[0][2] * ears[i][0] + g[0][3] * ears[i][1];
            r += g[1][2] * ears[i][0] + g[1][3] * ears[i][1];
        }
        if (room) {
            l += g[0][VP_SOURCE_ROOM] * room[i];
            r += g[1][VP_SOURCE_ROOM] * room[i];
        }
        out[i][0] += l;
        out[i][1] += r;
    }
}

/* Mix with constant gains. */
static inline void vp_stereo_mix(float out[NUM_SAMPLES_PER_FRAME][2],
                                 const float in[NUM_SAMPLES_PER_FRAME][2],
                                 const float (*ears)[2], const float *room,
                                 const float g[2][VP_SOURCES], unsigned channels)
{
    vp_stereo_mix_ramp(out, in, ears, room, g, g, channels);
}

#endif
