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

/* Settings > Bluetooth Settings
 *
 *   Bluetooth                  on/off
 *   Devices                    [Pair New Device], then the paired devices
 *     [Pair New Device]        devices found nearby, "Pair with X?"
 *     <device>                 per device menu, titled with its name
 *       Connect/Disconnect
 *       Forget                 "Forget X?"
 *       Change Name            keyboard
 *       Options                as below, for this device only
 *   Options                    defaults for the devices
 *     Automatically Connect
 *     Codec
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "action.h"
#include "settings.h"
#include "menu.h"
#include "list.h"
#include "splash.h"
#include "yesno.h"
#include "keyboard.h"
#include "talk.h"
#include "system.h"
#include "string-extra.h"
#include "exported_menus.h"
#include "misc.h"
#include "bluetooth/bt.h"

/* The device the device menu works on */
static struct bt_device cur_dev;

/* Bluetooth has to be on for the devices; offer to turn it on */
static bool bt_ensure_enabled(void)
{
    if (bt_is_enabled())
        return true;

    if (!yesno_pop(str(LANG_BLUETOOTH_TURN_ON)))
        return false;

    global_settings.bluetooth = true;
    settings_save();
    bt_set_enabled(true);
    return bt_is_enabled();
}

/***********************************/
/*    DEVICE OPTIONS               */

static int bt_dev_auto_connect(void)
{
    bool val = bt_get_option(cur_dev.addr, BT_OPT_AUTO_CONNECT);
    bool old = val;

    set_bool_options(str(LANG_BLUETOOTH_AUTO_CONNECT), &val,
                     str(LANG_SET_BOOL_YES), LANG_SET_BOOL_YES,
                     str(LANG_SET_BOOL_NO), LANG_SET_BOOL_NO, NULL);
    if (val != old)
        bt_set_option(cur_dev.addr, BT_OPT_AUTO_CONNECT, val);
    return 0;
}

static int bt_dev_codec(void)
{
    static const struct opt_items names[] = {
        [BT_CODEC_SBC_HIGH] = { STR(LANG_BLUETOOTH_CODEC_SBC_HIGH) },
        [BT_CODEC_SBC_STANDARD] = { STR(LANG_BLUETOOTH_CODEC_SBC_STANDARD) },
    };
    int val = bt_get_option(cur_dev.addr, BT_OPT_CODEC);
    int old = val;

    set_option(str(LANG_BLUETOOTH_CODEC), &val, RB_INT,
               names, ARRAYLEN(names), NULL);
    if (val != old)
        bt_set_option(cur_dev.addr, BT_OPT_CODEC, val);
    return 0;
}

MENUITEM_FUNCTION(bt_dev_auto_connect_item, 0,
                  ID2P(LANG_BLUETOOTH_AUTO_CONNECT),
                  bt_dev_auto_connect, NULL, Icon_Menu_setting);
MENUITEM_FUNCTION(bt_dev_codec_item, 0, ID2P(LANG_BLUETOOTH_CODEC),
                  bt_dev_codec, NULL, Icon_Menu_setting);

MAKE_MENU(bt_dev_options_menu, ID2P(LANG_BLUETOOTH_OPTIONS), NULL,
          Icon_Submenu, &bt_dev_auto_connect_item, &bt_dev_codec_item);

/***********************************/
/*    DEVICE MENU                  */

static bool bt_cur_dev_linked(void)
{
    enum bt_conn_state state = bt_conn_state(cur_dev.addr);
    return state == BT_CONN_CONNECTED || state == BT_CONN_CONNECTING;
}

/* No feedback of its own: the item flips once the link changes, the
 * theme can show the rest */
static int bt_connect_toggle(void)
{
    if (bt_cur_dev_linked())
        bt_disconnect(cur_dev.addr);
    else
        bt_connect(cur_dev.addr);
    return 0;
}

static int bt_connect_lang(void)
{
    return bt_cur_dev_linked() ? LANG_BLUETOOTH_DISCONNECT
                               : LANG_BLUETOOTH_CONNECT;
}

static char *bt_connect_get_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return str(bt_connect_lang());
}

static int bt_connect_speak(int selected_item, void *data)
{
    (void)selected_item; (void)data;
    talk_id(bt_connect_lang(), false);
    return 0;
}

/* Returns 1 to leave the device menu once the device is gone */
static int bt_forget_device(void)
{
    char buf[BT_NAME_LEN + 32];

    snprintf(buf, sizeof(buf), str(LANG_BLUETOOTH_FORGET_DEVICE),
             cur_dev.name);
    if (!yesno_pop(buf))
        return 0;

    bt_forget(cur_dev.addr);
    return 1;
}

static int bt_rename_device(void)
{
    char buf[BT_NAME_LEN];

    strmemccpy(buf, cur_dev.name, sizeof(buf));
    if (kbd_input(buf, sizeof(buf), NULL) < 0)
        return 0;

    bt_rename(cur_dev.addr, buf);

    /* An empty name brings back the device's own */
    for (int i = 0; i < bt_paired_count(); i++)
    {
        struct bt_device dev;
        if (bt_paired_get(i, &dev) &&
            !memcmp(dev.addr, cur_dev.addr, BT_ADDR_LEN))
        {
            cur_dev = dev;
            break;
        }
    }
    return 0;
}

MENUITEM_FUNCTION_DYNTEXT(bt_connect_item, 0, bt_connect_toggle,
                          bt_connect_get_name, bt_connect_speak, NULL,
                          NULL, Icon_Menu_functioncall);
MENUITEM_FUNCTION(bt_forget_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_BLUETOOTH_FORGET), bt_forget_device, NULL,
                  Icon_Menu_functioncall);
MENUITEM_FUNCTION(bt_rename_item, 0, ID2P(LANG_BLUETOOTH_CHANGE_NAME),
                  bt_rename_device, NULL, Icon_Keyboard);

/* MAKE_MENU with the device name as title. The name is handed out
 * directly so the title follows a rename. */
static char *bt_device_menu_title(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return cur_dev.name;
}

static const struct menu_item_ex *bt_device_menu_[] = {
    &bt_connect_item, &bt_forget_item, &bt_rename_item, &bt_dev_options_menu,
};
static const struct menu_get_name_and_icon bt_device_menu__ = {
    NULL, bt_device_menu_title, NULL, NULL, Icon_Submenu_Entered,
};
static const struct menu_item_ex bt_device_menu = {
    MT_MENU|MENU_DYNAMIC_DESC|MENU_ITEM_COUNT(ARRAYLEN(bt_device_menu_)),
    { (void *)bt_device_menu_ }, { .menu_get_name_and_icon = &bt_device_menu__ },
};

/***********************************/
/*    PAIRING                      */

/* Wait for the bond; the user may give up */
static bool bt_pair_wait(const uint8_t *addr)
{
    splash(0, ID2P(LANG_BLUETOOTH_PAIRING));

    while (bt_pair_state() == BT_PAIR_BUSY)
    {
        int action = get_action(CONTEXT_STD, HZ/10);
        if (action == ACTION_STD_CANCEL ||
            default_event_handler(action) == SYS_USB_CONNECTED)
        {
            bt_disconnect(addr);
            return false;
        }
    }

    if (bt_pair_state() == BT_PAIR_DONE)
        return true;

    splash(HZ*2, ID2P(LANG_BLUETOOTH_PAIRING_FAILED));
    return false;
}

static const char *bt_found_get_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct bt_device dev;
    (void)data;

    if (!bt_found_get(selected_item, &dev))
        return str(LANG_BLUETOOTH_SEARCHING);

    strmemccpy(buffer, dev.name, buffer_len);
    return buffer;
}

static int bt_found_action(int action, struct gui_synclist *lists)
{
    struct bt_device dev;
    char buf[BT_NAME_LEN + 32];

    switch (action)
    {
        case ACTION_REDRAW:
            gui_synclist_set_nb_items(lists, MAX(1, bt_found_count()));
            return action;
        case ACTION_STD_OK:
            if (!bt_found_get(gui_synclist_get_sel_pos(lists), &dev))
                return ACTION_REDRAW;

            snprintf(buf, sizeof(buf), str(LANG_BLUETOOTH_PAIR_WITH),
                     dev.name);
            if (!yesno_pop(buf))
                return ACTION_REDRAW;

            if (bt_pair(dev.addr) && bt_pair_wait(dev.addr))
                return ACTION_STD_CANCEL; /* back to the devices */

            bt_discovery_start();
            return ACTION_REDRAW;
        default:
            return action;
    }
}

static void bt_pair_new_device(void)
{
    struct simplelist_info info;

    bt_discovery_start();

    simplelist_info_init(&info, str(LANG_BLUETOOTH_PAIR_NEW),
                         MAX(1, bt_found_count()), NULL);
    info.action_callback = bt_found_action;
    info.get_name = bt_found_get_name;
    simplelist_show_list(&info);

    bt_discovery_stop();
}

/***********************************/
/*    DEVICES                      */

/* Item 0 is [Pair New Device], the paired devices follow */
static const char *bt_devices_get_name(int selected_item, void *data,
                                       char *buffer, size_t buffer_len)
{
    struct bt_device dev;
    (void)data;

    if (selected_item == 0 || !bt_paired_get(selected_item - 1, &dev))
        return str(LANG_BLUETOOTH_PAIR_NEW);

    switch (bt_conn_state(dev.addr))
    {
        case BT_CONN_CONNECTED:
            snprintf(buffer, buffer_len, "%s (%s)", dev.name,
                     str(LANG_BLUETOOTH_CONNECTED));
            break;
        case BT_CONN_CONNECTING:
            snprintf(buffer, buffer_len, "%s (%s)", dev.name,
                     str(LANG_BLUETOOTH_CONNECTING));
            break;
        default:
            strmemccpy(buffer, dev.name, buffer_len);
            break;
    }
    return buffer;
}

static int bt_devices_action(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    switch (action)
    {
        case ACTION_REDRAW:
            gui_synclist_set_nb_items(lists, 1 + bt_paired_count());
            return action;
        case ACTION_STD_OK:
            if (sel == 0)
                bt_pair_new_device();
            else if (bt_paired_get(sel - 1, &cur_dev))
                do_menu(&bt_device_menu, NULL, NULL, false);
            gui_synclist_set_nb_items(lists, 1 + bt_paired_count());
            return ACTION_REDRAW;
        default:
            return action;
    }
}

static int bt_devices_screen(void)
{
    struct simplelist_info info;

    if (!bt_ensure_enabled())
        return 0;

    simplelist_info_init(&info, str(LANG_BLUETOOTH_DEVICES),
                         1 + bt_paired_count(), NULL);
    info.action_callback = bt_devices_action;
    info.get_name = bt_devices_get_name;
    simplelist_show_list(&info);
    return 0;
}

/***********************************/
/*    BLUETOOTH SETTINGS MENU      */

MENUITEM_SETTING(bt_enable_item, &global_settings.bluetooth, NULL);
MENUITEM_FUNCTION(bt_devices_item, 0, ID2P(LANG_BLUETOOTH_DEVICES),
                  bt_devices_screen, NULL, Icon_Submenu);

/* The defaults for devices not set otherwise */
MENUITEM_SETTING(bt_auto_connect_item,
                 &global_settings.bluetooth_auto_connect, NULL);
MENUITEM_SETTING(bt_codec_item, &global_settings.bluetooth_codec, NULL);

MAKE_MENU(bt_options_menu, ID2P(LANG_BLUETOOTH_OPTIONS), NULL, Icon_Submenu,
          &bt_auto_connect_item, &bt_codec_item);

MAKE_MENU(bluetooth_settings_menu, ID2P(LANG_BLUETOOTH_SETTINGS), NULL,
          Icon_Submenu,
          &bt_enable_item, &bt_devices_item, &bt_options_menu);
