/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the libretro core's parts (LIBRETRO_SPEC.md):
 * core.c the ABI and the game's lifecycle, audio.c the APU's output (and
 * the Communicator's headphone), input.c the RetroPad as the Duke, mic.c the
 * Communicator's microphone, video.c the frames to RetroArch, options.c the
 * core options, net.c the network option.
 */
#ifndef NATIVE_LIBRETRO_LIBRETRO_INT_H
#define NATIVE_LIBRETRO_LIBRETRO_INT_H

#include "native/core/nc.h"
#include "native/libretro/libretro.h"

/* The emulated video rate, exactly the Xbox's: 60000/1001 Hz (nv2a.c's
 * vblank). The core reports it, and each retro_run is one period. */
#define NC_LR_FPS_NUM 60000ull
#define NC_LR_FPS_DEN 1001ull
#define NC_LR_FRAME_NS (1000000000ull * NC_LR_FPS_DEN / NC_LR_FPS_NUM)   /* whole ns */

/* core.c — the frontend's environment callback; a failure that stops the
 * content (logged; retro_run asks RetroArch to shut down); the guest booted,
 * once (false: it could not be); the core stopped (M2), or with
 * HAKUX_LR_STOP=0 in PortareOS's build RetroArch's process ended, or started
 * again for a title's relaunch. */
bool nc_lr_env(unsigned cmd, void *data);
/* True for Start Core: boot C:\xboxdash.xbe with no disc. */
bool nc_lr_dashboard(void);
void nc_lr_fail(const char *why);
bool nc_lr_start(void);
void nc_lr_end(const char *why);

/* audio.c — one period's samples to RetroArch (retro_run), with player 1's
 * Communicator's headphone mixed in. */
void nc_lr_audio_run(retro_audio_sample_batch_t batch);

/* mic.c — RetroArch's microphone into player 1's Communicator while the
 * title moves its audio (retro_run); closed, the game stopping (stop_core). */
void nc_lr_mic_run(void);
void nc_lr_mic_stop(void);

/* input.c — each port's RetroPad to its player, and with rumble on (the
 * core option, nc_lr_input_set_rumble) the title's rumble to its motors
 * (retro_run); the motors stopped, the game stopping (stop_core). */
void nc_lr_input_run(retro_input_poll_t poll, retro_input_state_t state);
void nc_lr_input_set_rumble(bool on);
void nc_lr_input_rumble_stop(void);
/* The controller info and input descriptors (retro_load_game). */
void nc_lr_input_describe(retro_environment_t env);
/* A controller on every port (retro_init); the port's device from RetroArch,
 * RETRO_DEVICE_NONE pulling its player's controller out
 * (retro_set_controller_port_device). */
void nc_lr_input_reset(void);
void nc_lr_input_set_device(unsigned port, unsigned device);

/* Send at most one video callback per tick. Return true on submission.
 * Geometry accounts for overscan and the console's widescreen setting.
 * Pickup timing lets NV2A schedule vblank away from the presenter. */
bool nc_lr_video_run(retro_video_refresh_t video);
/* Repeat Frames: a video_cb every retro_run, the last frame again if need be. */
void nc_lr_video_set_repeat(bool on);
void nc_lr_video_pickup(void);
void nc_lr_video_geometry(struct retro_game_geometry *g);
void nc_lr_overscan(uint32_t w, uint32_t h, uint32_t *cx, uint32_t *cy);
void nc_lr_video_size(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh);
void nc_lr_video_set_widescreen(bool on);
/* The game unloaded: the frame state and its read-back buffer gone. */
void nc_lr_video_reset(void);
void nc_lr_video_set_crop(bool on);

/* options.c — the core options: declared to the frontend
 * (retro_set_environment), applied to the console before it boots
 * (retro_load_game), and a change during the game noted (retro_run): every
 * option but the overscan crop, rumble and frame pacing takes effect when
 * the game starts next. */
void nc_lr_options_declare(retro_environment_t env);
void nc_lr_options_apply(void);
void nc_lr_options_run(void);

/* Select off, LAN, Insignia NAT or both before boot. Warn about missing
 * Insignia accounts and duplicate console identities during play. */
void nc_lr_net_apply(const char *mode);
void nc_lr_net_run(void);

/* Vulkan negotiation boots the guest in context_reset. Without Vulkan
 * support, the video path falls back to CPU readback. */
bool nc_lr_vk_negotiate(void);
/* Return true if Vulkan handles video. Set *shown on submission. */
bool nc_lr_vk_run(retro_video_refresh_t video, bool repeat, bool *shown);
/* The game unloaded (after context_destroy): the negotiation forgotten. */
void nc_lr_vk_reset(void);

#endif
