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
#include <stddef.h>
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
    BT_PAIR_CONFIRM,    /* the user compares bt_pair_code() with the device's */
    BT_PAIR_PASSKEY,    /* the user types bt_pair_code() on the device */
    BT_PAIR_DONE,
    BT_PAIR_FAILED,
};

/* Stack, with the controller power */
bool bt_enable(void);
void bt_disable(void);
bool bt_is_enabled(void);
/* After the controller was lost (SYS_BLUETOOTH_HCI_FAILED): down and up
 * again, as when turning the setting off and on */
void bt_restart(void);
/* Writes the event log if it has news and the stream is stopped */
void bt_log_autosave(void);
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

/* Pair with a found device by connecting to it. 'pin' answers a legacy
 * PIN request, NULL for 0000. */
bool bt_pair(const uint8_t *addr, const char *pin);
enum bt_pair_state bt_pair_state(void);
/* The number for BT_PAIR_CONFIRM and BT_PAIR_PASSKEY */
uint32_t bt_pair_code(void);
/* The answer to BT_PAIR_CONFIRM */
void bt_pair_confirm(bool same);
/* The device asked for a legacy PIN during the last pairing */
bool bt_pair_pin_asked(void);

/* One A2DP sink at a time: connecting to another device drops the
 * current one first */
bool bt_connect(const uint8_t *addr);
bool bt_disconnect(const uint8_t *addr);
enum bt_conn_state bt_conn_state(const uint8_t *addr);

/* Receiving: the player as a Bluetooth speaker for a phone. While on, the
 * stack is in the A2DP sink role instead of the source one, the player is
 * visible to devices looking for one, and a device pairing with it is put
 * to the user as BT_PAIR_CONFIRM. Playback doesn't start meanwhile. */
bool bt_receive_start(void);
void bt_receive_stop(void);
bool bt_receiving(void);

struct bt_receive_status {
    bool streaming;
    char title[96];
    char artist[96];
};
void bt_receive_status(struct bt_receive_status *st);

enum bt_receive_cmd {
    BT_RECEIVE_PLAY_PAUSE,
    BT_RECEIVE_NEXT,
    BT_RECEIVE_PREV,
};
/* Sent to the phone over AVRCP */
void bt_receive_control(enum bt_receive_cmd cmd);

/* The name the player is visible as */
const char *bt_local_name(void);

/* The device audio goes to or comes from, false if none */
bool bt_is_connected(void);
bool bt_connected_device(char *buf, size_t len);
/* The device being paired with, for BT_PAIR_CONFIRM */
void bt_pair_peer_name(char *buf, size_t len);

/* Remove the bond and everything kept for the device */
bool bt_forget(const uint8_t *addr);
/* Name the device; an empty name goes back to its own */
bool bt_rename(const uint8_t *addr, const char *name);

/* Per device options, kept with the device. A device follows the
 * global setting until set for it. */
enum bt_option {
    BT_OPT_AUTO_CONNECT,    /* bool: connect when Bluetooth comes up */
    BT_OPT_CODEC,           /* enum bt_codec, used from the next connection */
    BT_OPT_VOLUME,          /* AVRCP absolute volume 0..127, kept by bt_volume.c */
};

/* Volume of a device until it has its own: about 40%, not to start loud */
#define BT_VOLUME_DEFAULT   51

/* The codec a device is asked to use. If it can't, it gets the best of
 * the ones below that it can: SBC, which every device has, at the end.
 * Values are stored, new ones go at the end. */
enum bt_codec {
    BT_CODEC_SBC_HIGH,      /* bitpool up to 53, ~328 kbps */
    BT_CODEC_SBC_STANDARD,  /* bitpool up to 35, ~229 kbps */
    BT_CODEC_APTX,
    BT_CODEC_APTX_HD,
    BT_CODEC_LDAC,
    BT_CODEC_BEST,          /* the best the device supports, the default */
    BT_NUM_CODECS
};

int bt_get_option(const uint8_t *addr, enum bt_option opt);
bool bt_set_option(const uint8_t *addr, enum bt_option opt, int val);

/* Debug menu */
bool bt_heap_info(void);

#endif /* _BT_H_ */
