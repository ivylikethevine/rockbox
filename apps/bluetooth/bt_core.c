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

/* The Bluetooth core: brings Bluedroid up and down, looks for devices,
 * pairs, connects and forgets them.
 *
 * Bluedroid keeps the bonds in ROCKBOX_DIR/bt_config.conf, one section
 * per device. What Rockbox keeps about a device goes in the same section
 * under Rb* keys, so it goes away with the bond. */

/*#define LOGF_ENABLE*/
#include <stdio.h>
#include <string.h>
#include "bt.h"
#include "bt-internal.h"
#include "bt_hci.h"
#include "button.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "logf.h"
#include "misc.h"
#include "pcm.h"
#include "pcm_sink.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "file.h"

/* Keys in a device's section of the bonding config */
#define BT_KEY_NAME     "RbName"    /* the name it gives itself */
#define BT_KEY_ALIAS    "RbAlias"   /* the name the user gave it */

/* Discovery runs in rounds of this many 1.28 s units */
#define BT_INQ_LEN      5

static struct bt_device paired[BT_MAX_PAIRED];
static int paired_count;

static struct bt_device found[BT_MAX_FOUND];
static int found_count;
static bool discovery_wanted;

/* The A2DP link */
static uint8_t peer[BT_ADDR_LEN];
static enum bt_conn_state peer_state = BT_CONN_DISCONNECTED;
/* Connect to this one once the current link is down */
static uint8_t pending[BT_ADDR_LEN];
static bool pending_valid;

static uint8_t pair_addr[BT_ADDR_LEN];
static char pair_name[BT_NAME_LEN];
static enum bt_pair_state pair_state = BT_PAIR_IDLE;

static bool addr_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, BT_ADDR_LEN) == 0;
}

/* Bluedroid takes non-const addresses */
static uint8_t *addr_arg(const uint8_t *addr)
{
    return (uint8_t *)addr;
}

static void bt_changed(void)
{
    button_queue_post(BUTTON_REDRAW, 0);
}

/* Names go into an INI style file, keep them on one line */
static void sanitize_name(char *name)
{
    for (unsigned char *p = (unsigned char *)name; *p; p++)
        if (*p < ' ')
            *p = ' ';
}

static bool get_prop(const uint8_t *addr, const char *key,
                     char *buf, size_t len)
{
    return esp_bt_gap_get_bond_device_property(addr_arg(addr), key,
                                               buf, len) == ESP_OK
           && buf[0] != '\0';
}

static void set_prop(const uint8_t *addr, const char *key, const char *val)
{
    esp_bt_gap_set_bond_device_property(addr_arg(addr), key, val);
}

static void set_name_prop(const uint8_t *addr, const char *key,
                          const char *name)
{
    char buf[BT_NAME_LEN];
    strmemccpy(buf, name, sizeof(buf));
    sanitize_name(buf);
    set_prop(addr, key, buf[0] ? buf : NULL);
}

static void device_name(const uint8_t *addr, char *buf, size_t len)
{
    if (!get_prop(addr, BT_KEY_ALIAS, buf, len) &&
        !get_prop(addr, BT_KEY_NAME, buf, len))
        snprintf(buf, len, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
}

static int find_dev(const struct bt_device *list, int count,
                    const uint8_t *addr)
{
    for (int i = 0; i < count; i++)
        if (addr_eq(list[i].addr, addr))
            return i;
    return -1;
}

static void drop_found(const uint8_t *addr)
{
    int i = find_dev(found, found_count, addr);
    if (i >= 0)
    {
        memmove(&found[i], &found[i + 1],
                (found_count - i - 1) * sizeof(found[0]));
        found_count--;
    }
}

/* Reading the config can block, so the list is built aside and swapped
 * in at once */
static void load_paired(void)
{
    static struct bt_device list[BT_MAX_PAIRED];
    esp_bd_addr_t addrs[BT_MAX_PAIRED];
    int count = BT_MAX_PAIRED;

    if (esp_bt_gap_get_bond_device_list(&count, addrs) != ESP_OK)
        count = 0;

    for (int i = 0; i < count; i++)
    {
        memcpy(list[i].addr, addrs[i], BT_ADDR_LEN);
        device_name(list[i].addr, list[i].name, BT_NAME_LEN);
        list[i].rssi = -128;
    }

    memcpy(paired, list, count * sizeof(list[0]));
    paired_count = count;

    for (int i = 0; i < count; i++)
        drop_found(paired[i].addr);
}

/***********************************/
/*    GAP                          */

/* Only show what can play audio */
static bool is_audio_sink(uint32_t cod)
{
    return esp_bt_gap_is_valid_cod(cod) &&
           (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV ||
            (esp_bt_gap_get_cod_srvc(cod) &
             (ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO)));
}

static void gap_disc_res(esp_bt_gap_cb_param_t *param)
{
    const uint8_t *addr = param->disc_res.bda;
    uint32_t cod = 0;
    uint8_t *eir = NULL;
    int8_t rssi = -128;

    for (int i = 0; i < param->disc_res.num_prop; i++)
    {
        esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
        switch (p->type)
        {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)p->val;
                break;
            default:
                break;
        }
    }

    if (!is_audio_sink(cod) || !eir ||
        find_dev(paired, paired_count, addr) >= 0)
        return;

    uint8_t len = 0;
    uint8_t *name = esp_bt_gap_resolve_eir_data(eir,
                        ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!name)
        name = esp_bt_gap_resolve_eir_data(eir,
                        ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    if (!name)
        return;

    int i = find_dev(found, found_count, addr);
    if (i < 0)
    {
        if (found_count >= BT_MAX_FOUND)
            return;
        i = found_count++;
    }

    struct bt_device *dev = &found[i];
    len = MIN(len, BT_NAME_LEN - 1);
    memcpy(dev->addr, addr, BT_ADDR_LEN);
    memcpy(dev->name, name, len);
    dev->name[len] = '\0';
    sanitize_name(dev->name);
    dev->rssi = rssi;
    bt_changed();
}

static void gap_auth_cmpl(esp_bt_gap_cb_param_t *param)
{
    const uint8_t *addr = param->auth_cmpl.bda;
    bool ours = pair_state == BT_PAIR_BUSY && addr_eq(addr, pair_addr);
    char name[BT_NAME_LEN];

    if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS)
    {
        if (ours)
            pair_state = BT_PAIR_FAILED;
        bt_changed();
        return;
    }

    if (!get_prop(addr, BT_KEY_NAME, name, sizeof(name)))
    {
        if (ours && pair_name[0])
            set_name_prop(addr, BT_KEY_NAME, pair_name);
        else
            set_name_prop(addr, BT_KEY_NAME,
                          (const char *)param->auth_cmpl.device_name);
    }

    if (ours)
        pair_state = BT_PAIR_DONE;

    load_paired();
    bt_changed();
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_BT_GAP_DISC_RES_EVT:
            gap_disc_res(param);
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            logf("bt: discovery %d", param->disc_st_chg.state);
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
                discovery_wanted)
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                           BT_INQ_LEN, 0);
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            /* Speakers and headphones have no display, nothing to compare */
            logf("bt: confirm %lu", (unsigned long)param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_PIN_REQ_EVT:
        {
            /* Legacy devices nearly all take 0000 */
            esp_bt_pin_code_t pin = {0};
            int len = 16;
            if (!param->pin_req.min_16_digit)
            {
                memset(pin, '0', 4);
                len = 4;
            }
            esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
            break;
        }
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            logf("bt: auth %d", param->auth_cmpl.stat);
            gap_auth_cmpl(param);
            break;
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS &&
                find_dev(paired, paired_count, param->read_rmt_name.bda) >= 0)
            {
                set_name_prop(param->read_rmt_name.bda, BT_KEY_NAME,
                              (const char *)param->read_rmt_name.rmt_name);
                load_paired();
                bt_changed();
            }
            break;
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            logf("bt: unbond %d", param->remove_bond_dev_cmpl.status);
            esp_bt_gap_remove_bond_device_properties(
                    param->remove_bond_dev_cmpl.bda);
            load_paired();
            bt_changed();
            break;
        default:
            logf("bt: gap %d", event);
            break;
    }
}

/***********************************/
/*    A2DP LINK                    */

static bool a2d_connect(const uint8_t *addr)
{
    /* Inquiry and paging share the radio */
    bt_discovery_stop();

    if (esp_a2d_source_connect(addr_arg(addr)) != ESP_OK)
        return false;

    memcpy(peer, addr, BT_ADDR_LEN);
    peer_state = BT_CONN_CONNECTING;
    bt_changed();
    return true;
}

void bt_core_a2d_conn_state(esp_a2d_connection_state_t state,
                            const uint8_t *addr)
{
    logf("bt: a2dp %d", state);
    memcpy(peer, addr, BT_ADDR_LEN);

    switch (state)
    {
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            peer_state = BT_CONN_CONNECTING;
            break;
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            peer_state = BT_CONN_CONNECTED;
            /* Keep its own name current */
            esp_bt_gap_read_remote_name(addr_arg(addr));
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            peer_state = BT_CONN_DISCONNECTING;
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            peer_state = BT_CONN_DISCONNECTED;
            /* The link went before the bond was made */
            if (pair_state == BT_PAIR_BUSY && addr_eq(addr, pair_addr))
                pair_state = BT_PAIR_FAILED;
            if (pending_valid)
            {
                pending_valid = false;
                a2d_connect(pending);
            }
            break;
        default:
            break;
    }

    bt_changed();
}

const uint8_t *bt_core_peer(void)
{
    return peer_state == BT_CONN_CONNECTED ? peer : NULL;
}

/***********************************/
/*    STACK                        */

bool bt_is_enabled(void)
{
    return esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

static void stack_down(void)
{
    esp_a2d_source_deinit();
    esp_avrc_ct_deinit();
    esp_avrc_tg_deinit();
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
        esp_bluedroid_disable();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED)
        esp_bluedroid_deinit();
    bt_hci_disable();
}

static bool avrc_up(void)
{
    if (esp_avrc_tg_init() != ESP_OK ||
        esp_avrc_tg_register_callback(bt_avrc_tg_cb) != ESP_OK ||
        esp_avrc_ct_init() != ESP_OK ||
        esp_avrc_ct_register_callback(bt_avrc_ct_cb) != ESP_OK)
        return false;

    static const uint8_t cmds[] = {
        ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STOP,
        ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD,
        ESP_AVRC_PT_CMD_FAST_FORWARD, ESP_AVRC_PT_CMD_REWIND,
    };
    esp_avrc_psth_bit_mask_t psth = {0};
    for (unsigned i = 0; i < ARRAYLEN(cmds); i++)
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET,
                                         &psth, cmds[i]);

    return esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD,
                                           &psth) == ESP_OK;
}

static bool stack_up(void)
{
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    cfg.sc_en = true;
    if (esp_bluedroid_init_with_cfg(&cfg) != ESP_OK ||
        esp_bluedroid_enable() != ESP_OK)
        return false;

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_pin_code_t pin = {0};
    if (esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                      &iocap, sizeof(iocap)) != ESP_OK ||
        esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin) != ESP_OK ||
        esp_bt_gap_register_callback(gap_cb) != ESP_OK)
        return false;

    char name[32] = "Rockbox";
    int fd = open_utf8(ROCKBOX_DIR "/playername.txt", O_RDONLY);
    if (fd >= 0)
    {
        read_line(fd, name, sizeof(name));
        close(fd);
    }
    if (esp_bt_gap_set_device_name(name) != ESP_OK)
        return false;

    if (!avrc_up())
        return false;

    if (esp_a2d_source_init() != ESP_OK ||
        esp_a2d_register_callback(bt_a2d_cb) != ESP_OK ||
        !bt_a2d_register_endpoint())
        return false;

    return esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                    ESP_BT_NON_DISCOVERABLE) == ESP_OK;
}

bool bt_enable(void)
{
    if (bt_is_enabled())
        return true;

    bt_hci_enable();
    if (!stack_up())
    {
        logf("bt: stack failed to start");
        stack_down();
        return false;
    }

    found_count = 0;
    load_paired();
    bt_changed();
    return true;
}

void bt_disable(void)
{
    if (!bt_is_enabled())
        return;

    bt_discovery_stop();
    pending_valid = false;
    stack_down();

    /* No disconnect event comes after the stack is gone */
    if (peer_state != BT_CONN_DISCONNECTED)
        pcm_switch_sink(PCM_SINK_BUILTIN);
    peer_state = BT_CONN_DISCONNECTED;
    if (pair_state == BT_PAIR_BUSY)
        pair_state = BT_PAIR_FAILED;
    found_count = 0;
    bt_changed();
}

/***********************************/
/*    DEVICES                      */

int bt_paired_count(void)
{
    return paired_count;
}

bool bt_paired_get(int idx, struct bt_device *dev)
{
    if (idx < 0 || idx >= paired_count)
        return false;
    *dev = paired[idx];
    return true;
}

void bt_discovery_start(void)
{
    if (!bt_is_enabled() || discovery_wanted)
        return;

    discovery_wanted = true;
    found_count = 0;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, BT_INQ_LEN, 0);
    bt_changed();
}

void bt_discovery_stop(void)
{
    if (!discovery_wanted)
        return;

    discovery_wanted = false;
    esp_bt_gap_cancel_discovery();
}

int bt_found_count(void)
{
    return found_count;
}

bool bt_found_get(int idx, struct bt_device *dev)
{
    if (idx < 0 || idx >= found_count)
        return false;
    *dev = found[idx];
    return true;
}

bool bt_pair(const uint8_t *addr)
{
    int i = find_dev(found, found_count, addr);

    if (!bt_is_enabled())
        return false;

    memcpy(pair_addr, addr, BT_ADDR_LEN);
    if (i >= 0)
        strmemccpy(pair_name, found[i].name, sizeof(pair_name));
    else
        pair_name[0] = '\0';
    pair_state = BT_PAIR_BUSY;

    /* Bluedroid bonds when the A2DP link asks for security */
    if (!bt_connect(addr))
    {
        pair_state = BT_PAIR_FAILED;
        return false;
    }
    return true;
}

enum bt_pair_state bt_pair_state(void)
{
    return pair_state;
}

bool bt_connect(const uint8_t *addr)
{
    if (!bt_is_enabled())
        return false;

    if (peer_state == BT_CONN_DISCONNECTED)
        return a2d_connect(addr);

    if (addr_eq(peer, addr))
        return true;

    /* Swap sinks once the current one is gone */
    memcpy(pending, addr, BT_ADDR_LEN);
    pending_valid = true;
    if (peer_state != BT_CONN_DISCONNECTING)
        esp_a2d_source_disconnect(peer);
    return true;
}

bool bt_disconnect(const uint8_t *addr)
{
    if (pending_valid && addr_eq(pending, addr))
        pending_valid = false;

    if (!bt_is_enabled() || peer_state == BT_CONN_DISCONNECTED ||
        !addr_eq(peer, addr))
        return false;

    return esp_a2d_source_disconnect(peer) == ESP_OK;
}

enum bt_conn_state bt_conn_state(const uint8_t *addr)
{
    if (pending_valid && addr_eq(pending, addr))
        return BT_CONN_CONNECTING;
    if (!addr_eq(peer, addr))
        return BT_CONN_DISCONNECTED;
    return peer_state;
}

bool bt_forget(const uint8_t *addr)
{
    if (!bt_is_enabled())
        return false;

    bt_disconnect(addr);
    return esp_bt_gap_remove_bond_device(addr_arg(addr)) == ESP_OK;
}

bool bt_rename(const uint8_t *addr, const char *name)
{
    if (!bt_is_enabled())
        return false;

    set_name_prop(addr, BT_KEY_ALIAS, name);
    load_paired();
    bt_changed();
    return true;
}
