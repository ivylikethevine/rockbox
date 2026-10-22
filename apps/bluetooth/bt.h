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
#ifndef _BT_H_
#define _BT_H_

/* What the UI asks of the Bluetooth core. Calls are made from the UI
 * thread; Bluedroid answers on its own threads, so most of these only
 * start an operation. Whenever the device lists or a connection change
 * BUTTON_REDRAW is posted, so an open list picks it up. */

#include <stdbool.h>
#include <stdint.h>

#define BT_ADDR_LEN     6
#define BT_NAME_LEN     64  /* with the terminator */
#define BT_MAX_PAIRED   16
#define BT_MAX_FOUND    32

struct bt_device {
    uint8_t addr[BT_ADDR_LEN];
    char name[BT_NAME_LEN]; /* the user's name for it, else its own */
    int8_t rssi;            /* found devices only */
};

enum bt_conn_state {
    BT_CONN_DISCONNECTED,
    BT_CONN_CONNECTING,
    BT_CONN_CONNECTED,
    BT_CONN_DISCONNECTING,
};

enum bt_pair_state {
    BT_PAIR_IDLE,
    BT_PAIR_BUSY,
    BT_PAIR_DONE,
    BT_PAIR_FAILED,
};

/* Stack, with the controller power */
bool bt_enable(void);
void bt_disable(void);
bool bt_is_enabled(void);
/* The Bluetooth setting: on the way up shows progress, and if the stack
 * fails to start says so and turns the setting back off */
void bt_set_enabled(bool on);

/* Paired devices, copied out by index */
int bt_paired_count(void);
bool bt_paired_get(int idx, struct bt_device *dev);

/* Looking for devices to pair, restarted until stopped. The found list
 * leaves out paired devices and devices without a name. */
void bt_discovery_start(void);
void bt_discovery_stop(void);
int bt_found_count(void);
bool bt_found_get(int idx, struct bt_device *dev);

/* Pair with a found device by connecting to it */
bool bt_pair(const uint8_t *addr);
enum bt_pair_state bt_pair_state(void);

/* One A2DP sink at a time: connecting to another device drops the
 * current one first */
bool bt_connect(const uint8_t *addr);
bool bt_disconnect(const uint8_t *addr);
enum bt_conn_state bt_conn_state(const uint8_t *addr);

/* Remove the bond and everything kept for the device */
bool bt_forget(const uint8_t *addr);
/* Name the device; an empty name goes back to its own */
bool bt_rename(const uint8_t *addr, const char *name);

/* Per device options, kept with the device. A device follows the
 * global setting until set for it. */
enum bt_option {
    BT_OPT_AUTO_CONNECT,    /* bool: connect when Bluetooth comes up */
    BT_OPT_CODEC,           /* enum bt_codec, used from the next connection */
};

/* The codec a device is asked to use. If it can't, it gets the best of
 * the ones below that it can: SBC, which every device has, at the end.
 * Values are stored, new ones go at the end. */
enum bt_codec {
    BT_CODEC_SBC_HIGH,      /* bitpool up to 53, ~328 kbps */
    BT_CODEC_SBC_STANDARD,  /* bitpool up to 35, ~229 kbps */
    BT_CODEC_APTX,
    BT_CODEC_APTX_HD,
    BT_CODEC_LDAC,
    BT_NUM_CODECS
};

int bt_get_option(const uint8_t *addr, enum bt_option opt);
bool bt_set_option(const uint8_t *addr, enum bt_option opt, int val);

/* Debug menu */
bool bt_heap_info(void);

#endif /* _BT_H_ */
