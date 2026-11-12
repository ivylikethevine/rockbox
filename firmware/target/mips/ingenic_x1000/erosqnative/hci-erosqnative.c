/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2026 Skye Green
 * Copyright (C) 2026 Ivy Duggan
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

/* bt_hci for the hw4 players' AzureWave AW-NM372 (CYW43438) on UART0:
 * Broadcom patch RAM download, then H4 at 3 Mbaud with RTS/CTS, in the
 * order the OF's brcm_patchram_plus uses. The OF also enables the chip's
 * UART sleep mode; it was tried and found worse for the stream, so it is
 * turned off, whatever the patch defaults to, and HOST_WAKE_BT stays high. */

/*#define LOGF_ENABLE*/
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "logf.h"
#include "bt_hci.h"
#include "devicedata.h"
#include "esp_bluedroid_hci.h"
#include "osi/bt_stats.h"
#include "gpio-ingenic.h"
#include "uart-x1000.h"
#include "general.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BT_UART         PORT_UART0
#define HCD_FILE        ROCKBOX_DIR "/fw/erosqnative_hw4.hcd"
#define BDADDR_FILE     ROCKBOX_DIR "/bt_addr.txt"
#define BOOT_BAUD       115200
#define H4_BAUD         3000000

/* Largest packet the controller sends: ACL header plus the CYW43438's
 * 1021 byte ACL buffer, with room to spare */
#define HCI_RECV_BUF_SIZE   2048

#ifdef BT_DEBUG_PCAP
/* the capture writes to the SD card from the receive thread */
#define HCI_STACK_SIZE      (DEFAULT_STACK_SIZE + 0x2000)
#else
#define HCI_STACK_SIZE      DEFAULT_STACK_SIZE
#endif

/* Commands, vendor ones last */
#define HCI_RESET               0x0c03
#define HCI_READ_BD_ADDR        0x1009
#define BCM_WRITE_BD_ADDR       0xfc01
#define BCM_UPDATE_BAUD         0xfc18
#define BCM_SLEEP_MODE          0xfc27
#define BCM_DOWNLOAD_MINIDRIVER 0xfc2e

/* How long the sleep clock's edges are counted at power-on */
#define LPO_MEASURE_US          2000

static const esp_bluedroid_hci_driver_callbacks_t *hci_callbacks;
static struct semaphore hci_callbacks_sem;
static struct mutex hci_tx_mutex;
static uint8_t hci_recv_buf[HCI_RECV_BUF_SIZE];
static uint32_t hci_stack[HCI_STACK_SIZE / sizeof(uint32_t)];
static unsigned int hci_thread_id;

/***********************************/
/*    H4 TRANSPORT                 */

#define H4_CMD  0x01
#define H4_ACL  0x02
#define H4_SCO  0x03
#define H4_EVT  0x04
#define H4_ISO  0x05

/* Header bytes after the type byte, -1 for an unknown type */
static int h4_header_len(uint8_t type)
{
    switch(type) {
    case H4_CMD: return 3;
    case H4_ACL: return 4;
    case H4_SCO: return 3;
    case H4_EVT: return 2;
    case H4_ISO: return 4;
    default:     return -1;
    }
}

/* Payload length from the type byte and header */
static size_t h4_payload_len(const uint8_t *pkt)
{
    switch(pkt[0]) {
    case H4_ACL: return pkt[3] | (pkt[4] << 8);
    case H4_ISO: return (pkt[3] | (pkt[4] << 8)) & 0x3fff;
    case H4_EVT: return pkt[2];
    default:     return pkt[3];
    }
}

static volatile bool h4_closing;

static void h4_open(int baud)
{
    /* uart_init() sets 8N1 with RTS/CTS */
    h4_closing = false;
    uart_init(BT_UART, baud);
}

static void h4_close(void)
{
    /* uart_deinit() wakes up a pending uart_rx() */
    h4_closing = true;
    uart_deinit(BT_UART);
}

static bool h4_read(uint8_t *buf, size_t n, long deadline, bool block)
{
    while(n > 0) {
        if(h4_closing)
            return false;

        int timeout = TIMEOUT_BLOCK;
        if(!block) {
            timeout = deadline - current_tick;
            if(timeout <= 0)
                return false;
        }

        size_t got = uart_rx_timeout(BT_UART, buf, n, timeout);
        buf += got;
        n -= got;
    }

    return true;
}

/* One complete packet, H4 type byte included: its length, 0 on timeout or
 * h4_close(), -1 if the stream is out of sync or it doesn't fit */
static int h4_recv(uint8_t *buf, size_t len, int timeout)
{
    bool block = (timeout == TIMEOUT_BLOCK);
    long deadline = current_tick + (block ? 0 : timeout);

    if(len < 1 || !h4_read(buf, 1, deadline, block))
        return 0;

    int hdr = h4_header_len(buf[0]);
    if(hdr < 0 || len < 1 + (size_t)hdr)
        return -1;
    if(!h4_read(&buf[1], hdr, deadline, block))
        return 0;

    size_t plen = h4_payload_len(buf);
    if(len < 1 + hdr + plen)
        return -1;
    if(!h4_read(&buf[1 + hdr], plen, deadline, block))
        return 0;

    return 1 + hdr + plen;
}

/* HCI command during bring-up, waiting up to a second for its Command
 * Complete: the length of the return parameters copied to 'ret', which needs
 * room for one byte, -1 on any failure. A Command Status with an error gives
 * 1 with the status in ret[0]. */
static int h4_hci_cmd(uint16_t opcode, const uint8_t *params, size_t plen,
                      uint8_t *ret, size_t retlen)
{
    static uint8_t cmd[4 + 255];
    uint8_t *evt = hci_recv_buf; /* the receive thread isn't running yet */

    cmd[0] = H4_CMD;
    cmd[1] = opcode;
    cmd[2] = opcode >> 8;
    cmd[3] = plen;
    if(plen)
        memcpy(&cmd[4], params, plen);
    if(!uart_tx_timeout(BT_UART, cmd, 4 + plen, HZ/2))
        return -1;

    long deadline = current_tick + HZ;
    for(;;) {
        long timeout = deadline - current_tick;
        if(timeout <= 0)
            return -1;

        int n = h4_recv(evt, HCI_RECV_BUF_SIZE, timeout);
        if(n <= 0)
            return -1;
        if(evt[0] != H4_EVT)
            continue;

        /* Command Complete */
        if(evt[1] == 0x0e && n >= 6 && (evt[4] | (evt[5] << 8)) == opcode) {
            size_t len = MIN((size_t)n - 6, retlen);
            memcpy(ret, &evt[6], len);
            return len;
        }

        /* Command Status with an error */
        if(evt[1] == 0x0f && n >= 7 && (evt[5] | (evt[6] << 8)) == opcode &&
           evt[3]) {
            ret[0] = evt[3];
            return 1;
        }
    }
}

#ifdef BT_DEBUG_PCAP
static int hci_log_fd = -1;
static struct mutex hci_log_mutex;

/* LINKTYPE_BLUETOOTH_HCI_H4_WITH_PHDR, direction 0 sent, 1 received */
static void hci_log_open(void)
{
    static const uint8_t file_header[24] = {
        0xd4, 0xc3, 0xb2, 0xa1, 0x02, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00, 0xc9, 0x00, 0x00, 0x00,
    };

    mutex_init(&hci_log_mutex);
    hci_log_fd = open("/hci_log.pcap", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(hci_log_fd >= 0)
        write(hci_log_fd, file_header, sizeof(file_header));
}

static void hci_log_close(void)
{
    mutex_lock(&hci_log_mutex);
    if(hci_log_fd >= 0)
        close(hci_log_fd);
    hci_log_fd = -1;
    mutex_unlock(&hci_log_mutex);
}

static void hci_log_packet(const uint8_t *data, size_t len, bool received)
{
    struct {
        uint32_t seconds, microseconds, captured_len, original_len;
        uint32_t direction; /* big endian */
    } header = {
        .seconds = current_tick / HZ,
        .microseconds = (current_tick % HZ) * (1000000 / HZ),
        .captured_len = len + 4,
        .original_len = len + 4,
        .direction = received ? 0x01000000 : 0,
    };

    mutex_lock(&hci_log_mutex);
    if(hci_log_fd >= 0) {
        write(hci_log_fd, &header, sizeof(header));
        write(hci_log_fd, data, len);
        /* survive a panic */
        fsync(hci_log_fd);
    }
    mutex_unlock(&hci_log_mutex);
}
#else
static inline void hci_log_open(void) {}
static inline void hci_log_close(void) {}
static inline void hci_log_packet(const uint8_t *data, size_t len,
                                  bool received)
{
    (void)data; (void)len; (void)received;
}
#endif

/* The sleep clock's frequency as seen on its pin, for the debug screen: 0
 * if no edges show, which is no clock or a pin that doesn't read back while
 * it is a clock output. Interrupts are off for the count, so none is
 * missed. */
static uint32_t lpo_measure(void)
{
    const uint32_t bit = 1 << 26;
    uint32_t first = 0, last = 0, edges = 0;

    int irq = disable_irq_save();
    uint32_t start = __ost_read32();
    uint32_t level = REG_GPIO_PIN(GPIO_B) & bit;
    uint32_t now;
    while((now = __ost_read32()) - start < LPO_MEASURE_US * OST_TICKS_PER_US) {
        uint32_t l = REG_GPIO_PIN(GPIO_B) & bit;
        if(l && !level) {
            if(edges++ == 0)
                first = now;
            last = now;
        }
        level = l;
    }
    restore_irq(irq);

    if(edges < 2)
        return 0;
    return (uint64_t)(edges - 1) * OST_FREQUENCY / (last - first);
}

static void chip_power(bool on)
{
    /* BT_PWR, the OF's "BT Power" GPIO 86, is an output held low at boot
     * and the chip boots without it, so it isn't the chip's own supply:
     * the module's other rail or WL_REG_ON, on which the shared antenna
     * front end may depend. Driven as agptekrocker drives its BT_PWR. */
    extern int bt_pwr_pin; /* test setting, apps/bluetooth/bt.c */

    if(on) {
        /* RTC 32 kHz output, the chip's sleep clock, before BT_REG_ON;
         * the chip takes a while to boot before it answers */
        gpio_set_level(GPIO_HOST_WAKE_BT, 1);
        gpio_set_function(GPIO_PB(26), GPIOF_DEVICE(0));
        sleep(HZ/100 + 1);
        bt_stats.lpo_hz = lpo_measure();
        if(bt_pwr_pin) {
            gpio_set_level(GPIO_BT_PWR, 1);
            sleep(HZ/100 + 1);
        }
        gpio_set_level(GPIO_BT_REG_ON_HW4, 1);
        sleep(HZ/10);
    } else {
        gpio_set_level(GPIO_BT_REG_ON_HW4, 0);
        gpio_set_level(GPIO_BT_PWR, 0);
        gpio_set_level(GPIO_HOST_WAKE_BT, 0);
        gpio_set_function(GPIO_PB(26), GPIOF_INPUT);
    }
}

/* Init commands; true if the chip answered with a zero status */
static bool init_cmd(uint16_t opcode, const uint8_t *params, size_t plen)
{
    uint8_t status = 0xff;
    int rc = h4_hci_cmd(opcode, params, plen, &status, 1);
    if(rc < 1 || status != 0) {
        logf("bt: command %04x failed (%d, %02x)", opcode, rc, status);
        return false;
    }
    return true;
}

/* HCI_Reset at the boot rate, retried while the chip boots, then up to
 * 3 Mbaud. The chip answers the baud change at the old rate. */
static bool reset_and_speed_up(void)
{
    static const uint8_t baud[6] = {
        0x00, 0x00,                 /* encoded rate, unused */
        H4_BAUD & 0xff, (H4_BAUD >> 8) & 0xff,
        (H4_BAUD >> 16) & 0xff, (H4_BAUD >> 24) & 0xff,
    };
    bool ok = false;

    uart_set_baud(BT_UART, BOOT_BAUD);
    for(int i = 0; i < 3 && !ok; ++i)
        ok = init_cmd(HCI_RESET, NULL, 0);
    if(!ok || !init_cmd(BCM_UPDATE_BAUD, baud, sizeof(baud)))
        return false;

    uart_set_baud(BT_UART, H4_BAUD);
    sleep(HZ/100 + 1);
    return true;
}

/* Sleep mode 0, none: the patch's own default is unknown, and a chip that
 * naps holds up the stream. BT_WAKE and HOST_WAKE stay active high. A chip
 * that refuses the command runs as the patch left it. */
static void disable_sleep_mode(void)
{
    static const uint8_t params[12] = {
        0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    init_cmd(BCM_SLEEP_MODE, params, sizeof(params));
}

/* Each .hcd record is an HCI command: opcode, length, parameters. The
 * last one, Launch_RAM, reboots the chip into the patched firmware. */
static bool download_patchram(void)
{
    uint8_t rec[3 + 255];
    bool ok = false;

    int fd = open(HCD_FILE, O_RDONLY);
    if(fd < 0) {
        logf("bt: can't open " HCD_FILE);
        return false;
    }

    if(!init_cmd(BCM_DOWNLOAD_MINIDRIVER, NULL, 0))
        goto out;
    sleep(HZ/20);

    for(;;) {
        ssize_t n = read(fd, rec, 3);
        if(n == 0) {
            ok = true;
            break;
        }
        if(n != 3 || read(fd, &rec[3], rec[2]) != rec[2]) {
            logf("bt: truncated " HCD_FILE);
            break;
        }
        if(!init_cmd(rec[0] | (rec[1] << 8), &rec[3], rec[2]))
            break;
    }

out:
    close(fd);
    return ok;
}

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

/* The unit's address, from a file that can also be edited to set it by
 * hand; on first use made up from Broadcom's NAP/UAP and a random LAP
 * outside the reserved inquiry range 0x9e8b00-0x9e8b3f */
static void get_unit_addr(uint8_t addr[6], const uint8_t nap_uap[3])
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

    uint32_t lap;
    do {
        lap = prng_newseed() & 0xffffff;
    } while(lap >= 0x9e8b00 && lap <= 0x9e8b3f);

    addr[0] = nap_uap[0];
    addr[1] = nap_uap[1];
    addr[2] = nap_uap[2];
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

/* Modules without an address programmed come up with Broadcom's default
 * one, the same on every unit: give those the unit's own */
static void set_unit_addr(void)
{
    static const uint8_t bcm_default[6] = { 0x43, 0x43, 0xa1, 0x12, 0x1f, 0xac };
    uint8_t ret[1 + 6], addr[6], le[6];

    int rc = h4_hci_cmd(HCI_READ_BD_ADDR, NULL, 0, ret, sizeof(ret));
    if(rc != sizeof(ret) || ret[0] != 0)
        return;

    /* HCI sends it least significant byte first */
    for(int i = 0; i < 6; ++i)
        addr[i] = ret[6 - i];
    if(memcmp(addr, bcm_default, 6) != 0)
        return;

    get_unit_addr(addr, bcm_default);
    for(int i = 0; i < 6; ++i)
        le[i] = addr[5 - i];
    init_cmd(BCM_WRITE_BD_ADDR, le, sizeof(le));
}

static void hci_send(uint8_t *data, uint16_t len)
{
    hci_log_packet(data, len, false);

    mutex_lock(&hci_tx_mutex);
    /* A packet that can't go out within a second is dropped, Bluedroid
     * times out on it rather than hanging here */
    if(!uart_tx_timeout(BT_UART, data, len, HZ))
        bt_stats.tx_drops++;
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

    while(!h4_closing) {
        int n = h4_recv(hci_recv_buf, sizeof(hci_recv_buf), TIMEOUT_BLOCK);
        if(n == 0)
            break; /* h4_close() */
        if(n < 0) {
            /* Bluedroid can't recover from lost bytes */
            logf("bt: H4 stream out of sync (%02x)", hci_recv_buf[0]);
            queue_broadcast(SYS_BLUETOOTH_HCI_FAILED, 0);
            break;
        }

        hci_log_packet(hci_recv_buf, n, true);
        bt_stats.drv_rx_errors = uart_rx_errors[PORT_UART0];
        hci_callbacks->notify_host_recv(hci_recv_buf, n);
    }
}

/* On failure no driver is attached, so Bluedroid fails to start */
void bt_hci_enable(void)
{
    /* the pins and the module are only known on hw4 */
    if(device_data.hw_rev != 4) {
        logf("bt: not supported on hw%d", device_data.hw_rev);
        return;
    }

    hci_log_open();
    h4_open(BOOT_BAUD);
    chip_power(true);

    /* The patch goes over at 3 Mbaud like the OF does it, then the chip
     * reboots into it at the boot rate */
    if(!reset_and_speed_up() || !download_patchram())
        goto fail;
    sleep(HZ/4);
    if(!reset_and_speed_up())
        goto fail;

    disable_sleep_mode();
    set_unit_addr();

    hci_callbacks = NULL;
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
    return;

fail:
    logf("bt: controller failed to start");
    h4_close();
    chip_power(false);
    hci_log_close();
}

void bt_hci_disable(void)
{
    if(!hci_thread_id)
        return;

    h4_close();

    /* Wake the thread if Bluedroid never registered its callbacks */
    if(hci_callbacks == NULL)
        semaphore_release(&hci_callbacks_sem);
    thread_wait(hci_thread_id);
    hci_thread_id = 0;

    hci_callbacks = NULL;
    esp_bluedroid_detach_hci_driver();

    chip_power(false);
    hci_log_close();
}
