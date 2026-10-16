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

/* bt_hci for the CSR8811: PS keys over BCSP, then H4 with RTS/CTS for
 * Bluedroid. See csr8811-agptekrocker.c. */

#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "panic.h"
#include "general.h"
#include "bt_hci.h"
#include "esp_bluedroid_hci.h"
#include "csr8811-agptekrocker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BDADDR_FILE "/.rockbox/bt_addr.txt"

/* Largest packet the controller sends: ACL header plus the CSR8811's
 * 1021 byte ACL buffer, with room to spare */
#define HCI_RECV_BUF_SIZE   2048
#define HCI_STACK_SIZE      DEFAULT_STACK_SIZE

static const esp_bluedroid_hci_driver_callbacks_t *hci_callbacks;
static struct semaphore hci_callbacks_sem;
static struct mutex hci_tx_mutex;
static uint8_t hci_recv_buf[HCI_RECV_BUF_SIZE];
static uint32_t hci_stack[HCI_STACK_SIZE / sizeof(uint32_t)];
static unsigned int hci_thread_id;
static volatile bool hci_running;

static bool parse_bdaddr(const char *s, uint8_t addr[6])
{
    for(int i = 0; i < 6; ++i) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if(end == s || v > 0xff || (i < 5 && *end != ':'))
            return false;
        addr[i] = v;
        s = end + 1;
    }

    return true;
}

/* Every unit would otherwise share the address from the OF's .psr.
 * Generate one on first use and keep it in a file, which can also be
 * edited to set the address by hand. */
static void get_bdaddr(uint8_t addr[6])
{
    char buf[32];
    int fd = open(BDADDR_FILE, O_RDONLY);
    if(fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if(n > 0) {
            buf[n] = '\0';
            if(parse_bdaddr(buf, addr))
                return;
        }
    }

    /* Keep the OF's NAP/UAP and pick a random LAP outside the
     * reserved inquiry range 0x9e8b00-0x9e8b3f */
    uint32_t lap;
    do {
        lap = prng_newseed() & 0xffffff;
    } while(lap >= 0x9e8b00 && lap <= 0x9e8b3f);

    addr[0] = 0x00;
    addr[1] = 0x19;
    addr[2] = 0x01;
    addr[3] = lap >> 16;
    addr[4] = lap >> 8;
    addr[5] = lap;

    fd = open(BDADDR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd >= 0) {
        int n = snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x\n",
                         addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        write(fd, buf, n);
        close(fd);
    }
}

static void hci_send(uint8_t *data, uint16_t len)
{
    /* A packet that can't go out within a second is dropped, Bluedroid
     * times out on it rather than hanging here */
    mutex_lock(&hci_tx_mutex);
    h4_send(data, len, HZ);
    mutex_unlock(&hci_tx_mutex);
}

static bool hci_check_send_available(void)
{
    return true;
}

static esp_err_t hci_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback)
{
    if(hci_callbacks != NULL)
        return ESP_FAIL;

    hci_callbacks = callback;
    semaphore_release(&hci_callbacks_sem);
    return ESP_OK;
}

static void hci_thread(void)
{
    semaphore_wait(&hci_callbacks_sem, TIMEOUT_BLOCK);

    while(hci_running) {
        int n = h4_recv(hci_recv_buf, sizeof(hci_recv_buf), TIMEOUT_BLOCK);
        if(n == 0)
            break; /* h4_close() */
        if(n < 0)
            panicf("bt: H4 stream out of sync (%02x)", hci_recv_buf[0]);

        hci_callbacks->notify_host_recv(hci_recv_buf, n);
    }
}

static void bringup_failed(const char *what)
{
    bcsp_close();
    csr_power(false);
    panicf("bt: %s", what);
}

void bt_hci_enable(void)
{
    uint8_t addr[6], ret[1];
    int errkey, rc;

    csr_power(true);

    if(!bcsp_open(CSR_BCSP_BOOT_BAUD))
        bringup_failed("no BCSP link to CSR8811");

    rc = csr_psr_load(CSR_PSR_FILE, &errkey);
    if(rc == -1)
        bringup_failed("can't read " CSR_PSR_FILE);
    if(rc != 0) {
        bcsp_close();
        csr_power(false);
        if(rc == -2)
            panicf("bt: syntax error in " CSR_PSR_FILE " line %d", errkey);
        panicf("bt: CSR8811 rejected PS key %04x", errkey);
    }

    get_bdaddr(addr);
    if(!csr_set_bdaddr(addr))
        bringup_failed("can't set BD_ADDR");

    if(!csr_switch_to_h4(CSR_H4_BAUD))
        bringup_failed("switch to H4 failed");
    bcsp_close();

    h4_open(CSR_H4_BAUD);
    sleep(HZ/10);

    /* The chip may still be booting on the first try */
    for(int i = 0; i < 3; ++i) {
        rc = csr_hci_cmd_h4(0x0c03, NULL, 0, ret, sizeof(ret));
        if(rc != -1)
            break;
    }
    if(rc < 1 || ret[0] != 0) {
        h4_close();
        csr_power(false);
        panicf("bt: HCI_Reset over H4 failed (%d)", rc);
    }

    hci_callbacks = NULL;
    hci_running = true;
    semaphore_init(&hci_callbacks_sem, 1, 0);
    mutex_init(&hci_tx_mutex);

    esp_bluedroid_hci_driver_operations_t ops = {
        .send = hci_send,
        .check_send_available = hci_check_send_available,
        .register_host_callback = hci_register_host_callback,
    };
    esp_bluedroid_attach_hci_driver(&ops);

    hci_thread_id = create_thread(hci_thread, hci_stack, sizeof(hci_stack), 0,
                                  "bt_hci" IF_PRIO(, PRIORITY_BLUETOOTH));
}

void bt_hci_disable(void)
{
    hci_running = false;
    h4_close();

    /* Wake the thread if Bluedroid never registered its callbacks */
    if(hci_callbacks == NULL)
        semaphore_release(&hci_callbacks_sem);
    thread_wait(hci_thread_id);

    hci_callbacks = NULL;
    esp_bluedroid_detach_hci_driver();
    csr_power(false);
}
