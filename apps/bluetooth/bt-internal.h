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

/* bt.c, registered by the core */
void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
/* The stream endpoint offered to sinks, once A2DP source is up */
bool bt_a2d_register_endpoint(void);

/* bt.c, for the encoders in codecs/: the PCM to send, silence while
 * nothing plays */
size_t bt_read_pcm(uint8_t *buf, size_t len);

/* bt.c: the A2DP sink is gone or going, pause and use the built-in
 * output again */
void bt_sink_lost(void);

/* bt_core.c, A2DP connection changes passed on by bt_a2d_cb() */
void bt_core_a2d_conn_state(esp_a2d_connection_state_t state,
                            const uint8_t *addr);
/* The connected A2DP sink, NULL if there is none */
const uint8_t *bt_core_peer(void);
/* SBC bitpool ceiling for the A2DP sink, from its codec option */
int bt_core_max_bitpool(void);

#endif /* _BT_INTERNAL_H_ */
