
/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Skye Green
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdint.h>
#include <string.h>

#include "fixedpoint.h"
#include "bt_sw_volume.h"

#define BT_SW_VOLUME_SW_VOLUME_FRACBITS 16
#define BT_SW_VOLUME_UNITY (1 << BT_SW_VOLUME_SW_VOLUME_FRACBITS)
#define BT_SW_VOLUME_FRAME_SIZE (2 * sizeof (int16_t))
#define BT_SW_VOLUME_TRANS_FRAMES 1024

/* Unity until set, the PCM is copied as it is */
static uint32_t bt_sw_volume_new_factor_l = BT_SW_VOLUME_UNITY, bt_sw_volume_new_factor_r = BT_SW_VOLUME_UNITY;
static uint32_t bt_sw_volume_factor_l = BT_SW_VOLUME_UNITY, bt_sw_volume_factor_r = BT_SW_VOLUME_UNITY;
static uint32_t bt_sw_volume_scale_progress = 0;
static typeof (memcpy) *bt_sw_volume_scaling_fn = memcpy;

static void bt_sw_volume_sync_pcm_factors(void);

/* Return the scale factor corresponding to the centibel level */
static uint32_t bt_sw_volume_centibels_to_factor(int volume)
{
    if (volume <= -730)
        return 0;
    
    if (volume > 0)
        return BT_SW_VOLUME_UNITY;

    /* Centibels -> fixedpoint */
    return (uint32_t)fp_factor(fp_div(volume, 10, BT_SW_VOLUME_SW_VOLUME_FRACBITS),
                               BT_SW_VOLUME_SW_VOLUME_FRACBITS);
}

static inline int32_t bt_sw_volume_scale_sample(uint32_t f, int32_t s)
{
    return ((int32_t)f * s) >> BT_SW_VOLUME_SW_VOLUME_FRACBITS;
}

static void * bt_sw_volume_scale_buffer_cut(void *dst, const void *src, size_t src_size)
{
    int16_t *d = dst;
    const int16_t *s = src;
    uint32_t factor_l = bt_sw_volume_factor_l, factor_r = bt_sw_volume_factor_r;

    while (src_size >= BT_SW_VOLUME_FRAME_SIZE)
    {
        *d++ = bt_sw_volume_scale_sample(factor_l, *s++);
        *d++ = bt_sw_volume_scale_sample(factor_r, *s++);
        src_size -= BT_SW_VOLUME_FRAME_SIZE;
    }

    /* The buffers don't always end on a frame, copy the rest as is */
    memcpy(d, s, src_size);

    return dst;
}

/* The factor partway from the old value to the new one, along an inverted
   cosinus from PI..0 in order to minimize amplitude-modulated harmonics
   generation (zipper effects). */
static uint32_t bt_sw_volume_trans_factor(uint32_t factor, uint32_t new_factor)
{
    int32_t sweep = (1 << 14) - fp14_cos(180*bt_sw_volume_scale_progress / BT_SW_VOLUME_TRANS_FRAMES); /* 0.0..2.0 */
    return fp_mul(sweep, (int32_t)new_factor - (int32_t)factor, 15) + factor;
}

/* Transition the volume change smoothly over BT_SW_VOLUME_TRANS_FRAMES,
   however many buffers that takes */
static void * bt_sw_volume_scale_buffer_trans(void *dst, const void *src, size_t src_size)
{
    int16_t *d = dst;
    const int16_t *s = src;

    while (src_size >= BT_SW_VOLUME_FRAME_SIZE &&
           bt_sw_volume_scale_progress < BT_SW_VOLUME_TRANS_FRAMES)
    {
        uint32_t f_l = bt_sw_volume_trans_factor(bt_sw_volume_factor_l, bt_sw_volume_new_factor_l);
        uint32_t f_r = bt_sw_volume_trans_factor(bt_sw_volume_factor_r, bt_sw_volume_new_factor_r);
        *d++ = bt_sw_volume_scale_sample(f_l, *s++);
        *d++ = bt_sw_volume_scale_sample(f_r, *s++);
        src_size -= BT_SW_VOLUME_FRAME_SIZE;
        bt_sw_volume_scale_progress += 1;
    }

    if (bt_sw_volume_scale_progress >= BT_SW_VOLUME_TRANS_FRAMES) {
        bt_sw_volume_sync_pcm_factors();
        bt_sw_volume_scaling_fn(d, s, src_size);
    } else {
        memcpy(d, s, src_size);
    }

    return dst;
}

/* Assign the new scaling function for normal steady-state operation */
static void bt_sw_volume_sync_pcm_factors(void)
{
    uint32_t new_factor_l = bt_sw_volume_new_factor_l;
    uint32_t new_factor_r = bt_sw_volume_new_factor_r;

    bt_sw_volume_factor_l = new_factor_l;
    bt_sw_volume_factor_r = new_factor_r;

    if (new_factor_l == BT_SW_VOLUME_UNITY && new_factor_r == BT_SW_VOLUME_UNITY) {
        bt_sw_volume_scaling_fn = memcpy;
    } else {
        bt_sw_volume_scaling_fn = bt_sw_volume_scale_buffer_cut;
    }
}

void bt_sw_volume_set_volume(int vol_l, int vol_r)
{
    uint32_t new_factor_l = bt_sw_volume_centibels_to_factor(vol_l);
    uint32_t new_factor_r = bt_sw_volume_centibels_to_factor(vol_r);

    if (new_factor_l == bt_sw_volume_new_factor_l && new_factor_r == bt_sw_volume_new_factor_r)
        return;

    /* A change in the middle of a transition goes on from where that got to */
    if (bt_sw_volume_scaling_fn == bt_sw_volume_scale_buffer_trans) {
        bt_sw_volume_factor_l = bt_sw_volume_trans_factor(bt_sw_volume_factor_l, bt_sw_volume_new_factor_l);
        bt_sw_volume_factor_r = bt_sw_volume_trans_factor(bt_sw_volume_factor_r, bt_sw_volume_new_factor_r);
    }

    bt_sw_volume_new_factor_l = new_factor_l;
    bt_sw_volume_new_factor_r = new_factor_r;
    bt_sw_volume_scale_progress = 0;
    bt_sw_volume_scaling_fn = bt_sw_volume_scale_buffer_trans;
}

void bt_sw_volume_jump_volume(int vol_l, int vol_r)
{
    bt_sw_volume_new_factor_l = bt_sw_volume_centibels_to_factor(vol_l);
    bt_sw_volume_new_factor_r = bt_sw_volume_centibels_to_factor(vol_r);
    bt_sw_volume_sync_pcm_factors();
}

void bt_sw_volume_copy_buffer(void *dst, const void *src, size_t src_size)
{
    bt_sw_volume_scaling_fn(dst, src, src_size);
}
