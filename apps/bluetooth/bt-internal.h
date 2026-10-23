/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Marcin Bukat
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
#ifndef _BT_INTERNAL_H_
#define _BT_INTERNAL_H_

/* Between the core (bt_core.c: stack, GAP, devices) and the A2DP and
 * AVRCP side (bt.c: PCM sink, remote control, debug screen) */

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "bt.h"
#include "pcm_sink.h"

/* bt.c, registered by the core */
void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
/* The stream endpoint offered to sinks, once A2DP source is up */
bool bt_a2d_register_endpoint(void);

/* bt.c, for the encoders in codecs/: the PCM to send, silence while
 * nothing plays */
size_t bt_read_pcm(uint8_t *buf, size_t len);

/* bt.c: the A2DP link is gone or going, use the built-in output again
 * and drop the encoder and the stream setup */
void bt_sink_lost(void);

/* bt_volume.c: the Bluetooth output's volume */
extern const struct pcm_sink_volume bt_pcm_sink_volume;
/* The volume for the device that connected, saved for it on leaving */
void bt_volume_connected(const uint8_t *addr);
void bt_volume_disconnected(void);
void bt_volume_debug(char *buf, size_t len);

/* bt_core.c, A2DP connection changes passed on by bt_a2d_cb(). False if
 * the change is about another device than the link's, to be ignored. */
bool bt_core_a2d_conn_state(esp_a2d_connection_state_t state,
                            const uint8_t *addr);
/* A profile reported it is down, see stack_down() */
void bt_core_prof_down(void);
/* bt.c, bt_volume.c: an AVRCP profile's init is done, or failed */
void bt_core_prof_up(bool ok);
/* The connected A2DP sink, NULL if there is none */
const uint8_t *bt_core_peer(void);
/* The codec the A2DP sink is asked to use, from its codec option */
enum bt_codec bt_core_codec(void);
/* SBC bitpool ceiling for the A2DP sink, from its codec option */
int bt_core_max_bitpool(void);

#endif /* _BT_INTERNAL_H_ */
