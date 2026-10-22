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

/* The device the device menu works on */
static int cur_device;

/* Placeholders until the menus are wired to the Bluetooth core */
static int bt_ui_device_count(void)
{
    return 0;
}

static const char *bt_ui_device_name(int idx)
{
    (void)idx;
    return "";
}

static int bt_not_ready(void)
{
    splash(HZ, "Not implemented yet");
    return 0;
}

/***********************************/
/*    OPTIONS                      */

MENUITEM_FUNCTION(bt_auto_connect_item, 0, ID2P(LANG_BLUETOOTH_AUTO_CONNECT),
                  bt_not_ready, NULL, Icon_Menu_setting);
MENUITEM_FUNCTION(bt_codec_item, 0, ID2P(LANG_BLUETOOTH_CODEC),
                  bt_not_ready, NULL, Icon_Menu_setting);

MAKE_MENU(bt_options_menu, ID2P(LANG_BLUETOOTH_OPTIONS), NULL, Icon_Submenu,
          &bt_auto_connect_item, &bt_codec_item);

/***********************************/
/*    DEVICE MENU                  */

static char *bt_connect_get_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return str(LANG_BLUETOOTH_CONNECT);
}

static int bt_connect_speak(int selected_item, void *data)
{
    (void)selected_item; (void)data;
    talk_id(LANG_BLUETOOTH_CONNECT, false);
    return 0;
}

MENUITEM_FUNCTION_DYNTEXT(bt_connect_item, 0, bt_not_ready,
                          bt_connect_get_name, bt_connect_speak, NULL,
                          NULL, Icon_Menu_functioncall);
MENUITEM_FUNCTION(bt_forget_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_BLUETOOTH_FORGET), bt_not_ready, NULL,
                  Icon_Menu_functioncall);
MENUITEM_FUNCTION(bt_rename_item, 0, ID2P(LANG_BLUETOOTH_CHANGE_NAME),
                  bt_not_ready, NULL, Icon_Keyboard);
MENUITEM_FUNCTION(bt_dev_options_item, 0, ID2P(LANG_BLUETOOTH_OPTIONS),
                  bt_not_ready, NULL, Icon_Submenu);

/* MAKE_MENU with the device name as title */
static char *bt_device_menu_title(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    strmemccpy(buffer, bt_ui_device_name(cur_device), buffer_len);
    return buffer;
}

static const struct menu_item_ex *bt_device_menu_[] = {
    &bt_connect_item, &bt_forget_item, &bt_rename_item, &bt_dev_options_item,
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

static int bt_pair_new_device(void)
{
    return bt_not_ready();
}

/***********************************/
/*    DEVICES                      */

/* Item 0 is [Pair New Device], the paired devices follow */
static const char *bt_devices_get_name(int selected_item, void *data,
                                       char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    if (selected_item == 0)
        return str(LANG_BLUETOOTH_PAIR_NEW);
    return bt_ui_device_name(selected_item - 1);
}

static int bt_devices_action(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    switch (action)
    {
        case ACTION_REDRAW:
            gui_synclist_set_nb_items(lists, 1 + bt_ui_device_count());
            return action;
        case ACTION_STD_OK:
            if (sel == 0)
                bt_pair_new_device();
            else
            {
                cur_device = sel - 1;
                do_menu(&bt_device_menu, NULL, NULL, false);
            }
            return ACTION_REDRAW;
        default:
            return action;
    }
}

static int bt_devices_screen(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, str(LANG_BLUETOOTH_DEVICES),
                         1 + bt_ui_device_count(), NULL);
    info.action_callback = bt_devices_action;
    info.get_name = bt_devices_get_name;
    simplelist_show_list(&info);
    return 0;
}

/***********************************/
/*    BLUETOOTH SETTINGS MENU      */

MENUITEM_FUNCTION(bt_enable_item, 0, ID2P(LANG_BLUETOOTH),
                  bt_not_ready, NULL, Icon_Menu_setting);
MENUITEM_FUNCTION(bt_devices_item, 0, ID2P(LANG_BLUETOOTH_DEVICES),
                  bt_devices_screen, NULL, Icon_Submenu);

MAKE_MENU(bluetooth_settings_menu, ID2P(LANG_BLUETOOTH_SETTINGS), NULL,
          Icon_Submenu,
          &bt_enable_item, &bt_devices_item, &bt_options_menu);
