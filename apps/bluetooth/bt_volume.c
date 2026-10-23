/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Skye Green
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

/* Volume of the Bluetooth output, kept apart from the headphone volume.
 *
 * The volume is an AVRCP absolute volume, 0 (mute) to 127 (100%). If the
 * sink supports absolute volume it sets it itself, at full resolution,
 * and tells us when its own buttons change it. Otherwise the PCM is
 * scaled before encoding (bt_sw_volume.c). Balance is always scaled.
 *
 * Each device's volume is kept with it in bt_config.conf and set again
 * when it connects; a new device starts quiet. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "ata_idle_notify.h"
#include "pcm_sink.h"
#include "settings.h"
#include "bt.h"
#include "bt-internal.h"
#include "bt_sw_volume.h"

#define VOL_MAX         127     /* AVRCP absolute volume, 100% */
#define VOL_STEP        4       /* per volume button press, 32 steps */
/* Volume as seen through SOUND_VOLUME: absolute volume - 127, so the top
 * is 0 as for a volume in dB; one step lower than -127, so the steps from
 * the bottom reach the top */
#define VOL_UI_MIN      (-128)

/* Without absolute volume, 127..1 is 0..-60 dB, 0 mutes */
#define SWVOL_RANGE_CB  600
#define SWVOL_MUTE_CB   (-730)

/* AVRCP transaction labels */
#define TL_RN_CAPS      1
#define TL_SET_VOL      2
#define TL_VOL_NOTIFY   3

static uint8_t volume = BT_VOLUME_DEFAULT;
static int balance;             /* percent, as SOUND_BALANCE */
static bool abs_volume;         /* the sink sets it, AVRCP absolute volume */

/* The device the volume is for, and whether it's been saved for it */
static uint8_t dev_addr[BT_ADDR_LEN];
static bool dev_valid;
static bool dev_dirty;

/***********************************/
/*    VOLUME                       */

/* The scaling for the volume and balance as things are now */
static void update_scaling(bool ramp)
{
    int l, r;

    if (abs_volume)
        l = 0;
    else if (volume == 0)
        l = SWVOL_MUTE_CB;
    else
        l = -(VOL_MAX - volume) * SWVOL_RANGE_CB / (VOL_MAX - 1);
    r = l;

    /* as sound.c does with the hardware volume */
    if (balance > 0)
        l -= (l - SWVOL_MUTE_CB) * balance / 100;
    else if (balance < 0)
        r += (r - SWVOL_MUTE_CB) * balance / 100;

    if (ramp)
        bt_sw_volume_set_volume(l, r);
    else
        bt_sw_volume_jump_volume(l, r);
}

static void save_volume(void)
{
    if (!dev_dirty || !dev_valid || !bt_is_enabled())
        return;
    dev_dirty = false;
    bt_set_option(dev_addr, BT_OPT_VOLUME, volume);
}

/* The volume changed, by us or by the sink: save it once storage is
 * idle, and at the latest on disconnecting or shutting down */
static void volume_changed(uint8_t vol)
{
    if (vol == volume)
        return;
    volume = vol;

    if (dev_valid && !dev_dirty)
    {
        dev_dirty = true;
        register_storage_idle_func(save_volume);
    }
    update_scaling(true);
}

static void send_volume(void)
{
    if (abs_volume)
        esp_avrc_ct_send_set_absolute_volume_cmd(TL_SET_VOL, volume);
}

void bt_volume_connected(const uint8_t *addr)
{
    memcpy(dev_addr, addr, BT_ADDR_LEN);
    dev_valid = true;
    dev_dirty = false;

    int vol = bt_get_option(addr, BT_OPT_VOLUME);
    volume = (vol < 0 || vol > VOL_MAX) ? BT_VOLUME_DEFAULT : vol;
    logf("bt volume %d for the device", volume);
    balance = global_settings.balance;

    /* nothing plays yet: start at the device's level, not from the
     * last one's */
    update_scaling(false);
    send_volume();
}

void bt_volume_disconnected(void)
{
    if (!dev_valid)
        return;
    unregister_storage_idle_func(save_volume, true);
    dev_valid = false;
}

/***********************************/
/*    AVRCP                        */

void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_CT_PROF_STATE_EVT:
        if (param->avrc_ct_init_stat.state >= ESP_AVRC_DEINIT_SUCCESS)
            bt_core_prof_down();
        else
            bt_core_prof_up(param->avrc_ct_init_stat.state !=
                            ESP_AVRC_INIT_FAIL);
        break;

    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        logf("avrc-ct connected %d", param->conn_stat.connected);
        abs_volume = false;
        update_scaling(true);
        /* whether the sink does absolute volume */
        if (param->conn_stat.connected)
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_RN_CAPS);
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                &param->get_rn_caps_rsp.evt_set, ESP_AVRC_RN_VOLUME_CHANGE))
        {
            logf("avrc-ct absolute volume");
            abs_volume = true;
            update_scaling(true);
            send_volume();
            esp_avrc_ct_send_register_notification_cmd(TL_VOL_NOTIFY,
                ESP_AVRC_RN_VOLUME_CHANGE, 0);
        }
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        /* what the sink actually set */
        volume_changed(param->set_volume_rsp.volume & VOL_MAX);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id != ESP_AVRC_RN_VOLUME_CHANGE)
            break;
        /* The interim response is the value as we registered, after
         * ours was sent: only a change is news. It ends the
         * registration. */
        if (!param->change_ntf.is_interim)
        {
            logf("avrc-ct sink volume %d", param->change_ntf.event_parameter.volume);
            volume_changed(param->change_ntf.event_parameter.volume & VOL_MAX);
            global_status.last_volume_change = current_tick;
            esp_avrc_ct_send_register_notification_cmd(TL_VOL_NOTIFY,
                ESP_AVRC_RN_VOLUME_CHANGE, 0);
        }
        break;

    default:
        logf("avrc-ct %d", event);
        break;
    }
}

/***********************************/
/*    SINK VOLUME                  */

static int get_volume(void)
{
    return volume - VOL_MAX;
}

static void set_volume(int vol)
{
    vol += VOL_MAX;
    volume_changed(vol < 0 ? 0 : vol > VOL_MAX ? VOL_MAX : vol);
    send_volume();
}

static void set_balance(int bal)
{
    balance = bal;
    update_scaling(true);
}

/* Volume buttons: a step at a time, shown as when the headphone volume
 * changes */
static void adjust_volume(int steps)
{
    set_volume(get_volume() + steps * VOL_STEP);
    global_status.last_volume_change = current_tick;
}

const struct pcm_sink_volume bt_pcm_sink_volume = {
    .volume_min = VOL_UI_MIN,
    .volume_max = 0,
    .volume_default = BT_VOLUME_DEFAULT - VOL_MAX,
    .volume_step = VOL_STEP,
    .volume_numdecimals = 0,
    .volume_unit = "",
    .get_volume = get_volume,
    .set_volume = set_volume,
    .set_balance = set_balance,
    .adjust_volume = adjust_volume,
};

void bt_volume_debug(char *buf, size_t len)
{
    snprintf(buf, len, "vol %d %s", volume, abs_volume ? "avrcp" : "sw");
}
