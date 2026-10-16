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

/* CSR8811 bring-up. Out of reset the chip only talks BCSP, so a
 * minimal BCSP link (window of one, as in BlueZ's ubcsp) carries the
 * BCCMDs that load the PS keys, the same way the OF does with
 *   bccmd -t bcsp -d /dev/ttyS0 -b 115200 psload -r csr8811.psr
 * Unlike the OF, which stays on BCSP, the keys are then overridden to
 * switch the chip to H4 with RTS/CTS for the rest of the session.
 */

#include "system.h"
#include "kernel.h"
#include "file.h"
#include "gpio-ingenic.h"
#include "uart-x1000.h"
#include "csr8811-agptekrocker.h"
#include <stdlib.h>
#include <string.h>

#define BT_UART PORT_UART0

#define SLIP_FRAME          0xc0
#define SLIP_ESC            0xdb
#define SLIP_ESC_FRAME      0xdc
#define SLIP_ESC_ESC        0xdd

#define BCSP_CHAN_ACK       0
#define BCSP_CHAN_LE        1
#define BCSP_CHAN_HCI       5

#define BCSP_MAX_PAYLOAD    512
#define BCSP_RETX_TIMEOUT   (HZ/4)
#define BCSP_TX_TIMEOUT     (2*HZ)
#define BCSP_LE_TIMEOUT     (3*HZ)

#define BCCMD_GETRESP       0x0001
#define BCCMD_SETREQ        0x0002
#define BCCMD_MAX_VALUE     240

#define CSR_VARID_WARM_RESET    0x4002
#define CSR_VARID_PS            0x7003
#define CSR_STORES_PSRAM        0x0008

#define CSR_PSKEY_BDADDR            0x0001
#define CSR_PSKEY_UART_BAUDRATE     0x01be
#define CSR_PSKEY_UART_BITRATE      0x01ea
#define CSR_PSKEY_HOST_INTERFACE    0x01f9
#define CSR_PSKEY_DEEP_SLEEP_STATE  0x0229

#define CSR_HOST_INTERFACE_H4       3

enum {
    LE_SYNC,
    LE_SYNC_RESP,
    LE_CONF,
    LE_CONF_RESP,
    LE_NONE,
};

static const uint8_t le_msg[4][4] = {
    [LE_SYNC]      = {0xda, 0xdc, 0xed, 0xed},
    [LE_SYNC_RESP] = {0xac, 0xaf, 0xef, 0xee},
    [LE_CONF]      = {0xad, 0xef, 0xac, 0xed},
    [LE_CONF_RESP] = {0xde, 0xad, 0xd0, 0xd0},
};

enum {
    LINK_UNINIT,
    LINK_INIT,
    LINK_ACTIVE,
};

struct bcsp_pkt {
    int chan;
    bool reliable;
    size_t len;
    const uint8_t *data;
};

static struct {
    int link;
    bool peer_reset;
    uint8_t txseq;      /* seq of the reliable packet in flight */
    uint8_t txack;      /* next seq expected from the chip */
    bool tx_acked;
    uint16_t bccmd_seq;
    size_t stage_pos, stage_len;
    uint8_t stage[64];
    uint8_t frame[4 + BCSP_MAX_PAYLOAD + 2];
    uint8_t tx[2 * (4 + BCSP_MAX_PAYLOAD + 2) + 2];
} bcsp;

void csr_power(bool on)
{
    if(on) {
        gpio_set_level(GPIO_BT_PWR, 1);
        mdelay(10);
        gpio_set_level(GPIO_BT_REG_ON, 0);
        mdelay(10);
        gpio_set_level(GPIO_BT_REG_ON, 1);
        /* RTC 32 kHz output, the chip's sleep clock */
        gpio_set_function(GPIO_PB(26), GPIOF_DEVICE(0));
    } else {
        gpio_set_function(GPIO_PB(26), GPIOF_INPUT);
        gpio_set_level(GPIO_BT_REG_ON, 0);
        gpio_set_level(GPIO_BT_PWR, 0);
    }
}

static uint16_t bcsp_crc(uint16_t crc, uint8_t b)
{
    static const uint16_t tab[16] = {
        0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
        0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f,
    };

    crc = (crc >> 4) ^ tab[(crc ^ b) & 0xf];
    crc = (crc >> 4) ^ tab[(crc ^ (b >> 4)) & 0xf];
    return crc;
}

static uint16_t bitrev16(uint16_t x)
{
    uint16_t r = 0;
    for(int i = 0; i < 16; ++i) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

static size_t slip_put(uint8_t *out, size_t n, uint8_t b)
{
    if(b == SLIP_FRAME) {
        out[n++] = SLIP_ESC;
        out[n++] = SLIP_ESC_FRAME;
    } else if(b == SLIP_ESC) {
        out[n++] = SLIP_ESC;
        out[n++] = SLIP_ESC_ESC;
    } else {
        out[n++] = b;
    }
    return n;
}

static void bcsp_send_hdr(uint8_t h0, int chan, const uint8_t *data, size_t len)
{
    uint8_t hdr[4];
    hdr[0] = h0 | 0x40; /* always send a CRC */
    hdr[1] = (chan & 0xf) | ((len & 0xf) << 4);
    hdr[2] = len >> 4;
    hdr[3] = ~(hdr[0] + hdr[1] + hdr[2]);

    uint16_t crc = 0xffff;
    size_t n = 0;
    bcsp.tx[n++] = SLIP_FRAME;
    for(int i = 0; i < 4; ++i) {
        crc = bcsp_crc(crc, hdr[i]);
        n = slip_put(bcsp.tx, n, hdr[i]);
    }
    for(size_t i = 0; i < len; ++i) {
        crc = bcsp_crc(crc, data[i]);
        n = slip_put(bcsp.tx, n, data[i]);
    }
    crc = bitrev16(crc);
    n = slip_put(bcsp.tx, n, crc >> 8);
    n = slip_put(bcsp.tx, n, crc & 0xff);
    bcsp.tx[n++] = SLIP_FRAME;

    uart_tx(BT_UART, bcsp.tx, n);
}

static void bcsp_send_le(int msg)
{
    /* Link establishment packets carry no ack/seq */
    bcsp_send_hdr(0, BCSP_CHAN_LE, le_msg[msg], 4);
}

static void bcsp_send_ack(void)
{
    bcsp_send_hdr(bcsp.txack << 3, BCSP_CHAN_ACK, NULL, 0);
}

static void bcsp_send_reliable(int chan, const uint8_t *data, size_t len)
{
    bcsp_send_hdr(0x80 | (bcsp.txack << 3) | bcsp.txseq, chan, data, len);
}

static int bcsp_getc(long deadline)
{
    if(bcsp.stage_pos == bcsp.stage_len) {
        long remaining = deadline - current_tick;
        if(remaining <= 0)
            return -1;

        bcsp.stage_len = uart_rx_timeout(BT_UART, bcsp.stage,
                                         sizeof(bcsp.stage), remaining);
        bcsp.stage_pos = 0;
        if(bcsp.stage_len == 0)
            return -1;
    }

    return bcsp.stage[bcsp.stage_pos++];
}

/* Reads one SLIP frame, returns its length or -1 on timeout */
static int bcsp_read_frame(long deadline)
{
    bool in_frame = false, esc = false;
    size_t n = 0;

    for(;;) {
        int c = bcsp_getc(deadline);
        if(c < 0)
            return -1;

        if(c == SLIP_FRAME) {
            if(in_frame && n > 0)
                return n;
            in_frame = true;
            esc = false;
            n = 0;
            continue;
        }

        if(!in_frame)
            continue;

        if(esc) {
            esc = false;
            if(c == SLIP_ESC_FRAME)
                c = SLIP_FRAME;
            else if(c == SLIP_ESC_ESC)
                c = SLIP_ESC;
            else {
                in_frame = false;
                continue;
            }
        } else if(c == SLIP_ESC) {
            esc = true;
            continue;
        }

        if(n == sizeof(bcsp.frame)) {
            in_frame = false;
            continue;
        }

        bcsp.frame[n++] = c;
    }
}

static int le_which(const struct bcsp_pkt *pkt)
{
    if(pkt->len != 4)
        return LE_NONE;

    for(int i = 0; i < LE_NONE; ++i)
        if(!memcmp(pkt->data, le_msg[i], 4))
            return i;

    return LE_NONE;
}

static void bcsp_handle_le(const struct bcsp_pkt *pkt)
{
    switch(le_which(pkt)) {
    case LE_SYNC:
        if(bcsp.link == LINK_ACTIVE)
            bcsp.peer_reset = true;
        else
            bcsp_send_le(LE_SYNC_RESP);
        break;

    case LE_SYNC_RESP:
        if(bcsp.link == LINK_UNINIT)
            bcsp.link = LINK_INIT;
        break;

    case LE_CONF:
        if(bcsp.link != LINK_UNINIT)
            bcsp_send_le(LE_CONF_RESP);
        break;

    case LE_CONF_RESP:
        if(bcsp.link == LINK_INIT)
            bcsp.link = LINK_ACTIVE;
        break;

    default:
        break;
    }
}

/* Receives one valid packet. Acks and link establishment are handled
 * here; the packet is still returned so callers can track progress. */
static bool bcsp_recv(struct bcsp_pkt *pkt, long deadline)
{
    for(;;) {
        int n = bcsp_read_frame(deadline);
        if(n < 0)
            return false;
        if(n < 4)
            continue;

        uint8_t *h = bcsp.frame;
        if(((h[0] + h[1] + h[2] + h[3]) & 0xff) != 0xff)
            continue;

        size_t len = (h[1] >> 4) | (h[2] << 4);
        bool has_crc = h[0] & 0x40;
        if((size_t)n != 4 + len + (has_crc ? 2 : 0))
            continue;

        if(has_crc) {
            uint16_t crc = 0xffff;
            for(size_t i = 0; i < 4 + len; ++i)
                crc = bcsp_crc(crc, h[i]);
            crc = bitrev16(crc);
            if(h[4 + len] != (crc >> 8) || h[5 + len] != (crc & 0xff))
                continue;
        }

        pkt->reliable = h[0] & 0x80;
        pkt->chan = h[1] & 0xf;
        pkt->len = len;
        pkt->data = &h[4];

        if(pkt->chan == BCSP_CHAN_LE && !pkt->reliable) {
            bcsp_handle_le(pkt);
            return true;
        }

        uint8_t ack = (h[0] >> 3) & 7;
        if(!bcsp.tx_acked && ack == ((bcsp.txseq + 1) & 7)) {
            bcsp.tx_acked = true;
            bcsp.txseq = ack;
        }

        if(pkt->reliable) {
            bool in_seq = (h[0] & 7) == bcsp.txack;
            if(in_seq)
                bcsp.txack = (bcsp.txack + 1) & 7;
            bcsp_send_ack();
            if(!in_seq)
                continue; /* retransmission of something we already have */
        }

        return true;
    }
}

bool bcsp_open(int baud)
{
    uart_init(BT_UART, baud);
    uart_set_parity(BT_UART, UART_PARITY_EVEN);
    uart_set_flow_control(BT_UART, false);

    memset(&bcsp, 0, sizeof(bcsp));
    bcsp.tx_acked = true;

    long deadline = current_tick + BCSP_LE_TIMEOUT;
    long next_tx = current_tick;
    while(bcsp.link != LINK_ACTIVE) {
        if(TIME_AFTER(current_tick, deadline))
            return false;

        if(!TIME_BEFORE(current_tick, next_tx)) {
            bcsp_send_le(bcsp.link == LINK_UNINIT ? LE_SYNC : LE_CONF);
            next_tx = current_tick + BCSP_RETX_TIMEOUT;
        }

        struct bcsp_pkt pkt;
        bcsp_recv(&pkt, next_tx);
    }

    return true;
}

void bcsp_close(void)
{
    uart_deinit(BT_UART);
}

typedef bool (*bcsp_match_fn)(const struct bcsp_pkt *pkt, void *arg);

/* Sends a reliable packet and waits for it to be acked and, when
 * 'match' is given, for a packet it accepts. */
static bool bcsp_transact(int chan, const uint8_t *data, size_t len,
                          bcsp_match_fn match, void *arg)
{
    if(bcsp.link != LINK_ACTIVE || bcsp.peer_reset)
        return false;

    bcsp.tx_acked = false;
    bcsp_send_reliable(chan, data, len);

    long deadline = current_tick + BCSP_TX_TIMEOUT;
    long retx = current_tick + BCSP_RETX_TIMEOUT;
    for(;;) {
        struct bcsp_pkt pkt;
        long until = (bcsp.tx_acked || TIME_AFTER(retx, deadline)) ? deadline : retx;
        bool got = bcsp_recv(&pkt, until);

        if(bcsp.peer_reset)
            return false;

        if(!got) {
            if(!TIME_BEFORE(current_tick, deadline))
                return false;
            if(!bcsp.tx_acked) {
                bcsp_send_reliable(chan, data, len);
                retx = current_tick + BCSP_RETX_TIMEOUT;
            }
            continue;
        }

        if(!bcsp.tx_acked)
            continue;
        if(!match)
            return true;
        if(match(&pkt, arg))
            return true;
    }
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

struct bccmd_wait {
    uint16_t seq;
    uint8_t *value;
    size_t len;
    int status;
};

static bool bccmd_match(const struct bcsp_pkt *pkt, void *arg)
{
    struct bccmd_wait *w = arg;
    const uint8_t *d = pkt->data;

    /* Vendor event 0xff, payload descriptor 0xc2 (BCCMD, one fragment) */
    if(pkt->chan != BCSP_CHAN_HCI || pkt->len < 13 ||
       d[0] != 0xff || d[2] != 0xc2)
        return false;
    if(get16(&d[3]) != BCCMD_GETRESP || get16(&d[7]) != w->seq)
        return false;

    w->status = get16(&d[11]);
    if(w->value) {
        size_t avail = pkt->len - 13;
        memcpy(w->value, &d[13], MIN(avail, w->len));
    }
    return true;
}

static int csr_bccmd(uint16_t type, uint16_t varid, uint8_t *value,
                     size_t len, bool wait_resp)
{
    static uint8_t cmd[4 + 10 + BCCMD_MAX_VALUE + 2];

    if(len > BCCMD_MAX_VALUE)
        return -1;

    /* BCCMD length is in words and is never less than 9 */
    size_t words = len < 8 ? 9 : (len + 1) / 2 + 5;
    struct bccmd_wait w = {
        .seq = bcsp.bccmd_seq++,
        .value = value,
        .len = len,
        .status = -1,
    };

    memset(cmd, 0, sizeof(cmd));
    /* HCI vendor command 0xfc00 carrying a BCCMD */
    cmd[0] = 0x00;
    cmd[1] = 0xfc;
    cmd[2] = words * 2 + 1;
    cmd[3] = 0xc2;
    put16(&cmd[4], type);
    put16(&cmd[6], words);
    put16(&cmd[8], w.seq);
    put16(&cmd[10], varid);
    put16(&cmd[12], 0);
    if(len)
        memcpy(&cmd[14], value, len);

    if(!bcsp_transact(BCSP_CHAN_HCI, cmd, words * 2 + 4,
                      wait_resp ? bccmd_match : NULL, &w))
        return -1;

    return wait_resp ? w.status : 0;
}

static int csr_bccmd_set(uint16_t varid, const uint8_t *value, size_t len)
{
    static uint8_t buf[BCCMD_MAX_VALUE];

    if(len > sizeof(buf))
        return -1;
    memcpy(buf, value, len);
    return csr_bccmd(BCCMD_SETREQ, varid, buf, len, true);
}

static int csr_ps_write(uint16_t pskey, const uint16_t *words, size_t nwords)
{
    uint8_t buf[BCCMD_MAX_VALUE];

    if(6 + nwords * 2 > sizeof(buf))
        return -1;

    put16(&buf[0], pskey);
    put16(&buf[2], nwords);
    put16(&buf[4], CSR_STORES_PSRAM);
    for(size_t i = 0; i < nwords; ++i)
        put16(&buf[6 + i*2], words[i]);

    return csr_bccmd_set(CSR_VARID_PS, buf, 6 + nwords * 2);
}

static bool csr_warm_reset(void)
{
    /* No response comes back, only the BCSP ack */
    return csr_bccmd(BCCMD_SETREQ, CSR_VARID_WARM_RESET, NULL, 0, false) == 0;
}

bool csr_set_bdaddr(const uint8_t addr[6])
{
    /* LAP high byte, LAP low word, UAP, NAP */
    uint16_t w[4] = {
        addr[3],
        (addr[4] << 8) | addr[5],
        addr[2],
        (addr[0] << 8) | addr[1],
    };

    return csr_ps_write(CSR_PSKEY_BDADDR, w, 4) == 0;
}

int csr_psr_load(const char *path, int *errkey)
{
    static char text[8192];
    uint16_t words[(BCCMD_MAX_VALUE - 6) / 2];

    *errkey = 0;

    int fd = open(path, O_RDONLY);
    if(fd < 0)
        return -1;
    ssize_t size = read(fd, text, sizeof(text) - 1);
    close(fd);
    if(size < 0 || size == sizeof(text) - 1)
        return -1;
    text[size] = '\0';

    int lineno = 0;
    char *line = text;
    while(line && *line) {
        char *next = strchr(line, '\n');
        if(next)
            *next++ = '\0';
        lineno++;

        char *comment = strstr(line, "//");
        if(comment)
            *comment = '\0';

        char *p = line;
        while(*p == ' ' || *p == '\t')
            p++;

        if(*p == '&') {
            char *end;
            unsigned long key = strtoul(p + 1, &end, 16);
            if(end == p + 1 || key > 0xffff || !(p = strchr(end, '='))) {
                *errkey = lineno;
                return -2;
            }

            size_t n = 0;
            for(p++;; p = end) {
                unsigned long v = strtoul(p, &end, 16);
                if(end == p)
                    break;
                if(v > 0xffff || n == ARRAYLEN(words)) {
                    *errkey = lineno;
                    return -2;
                }
                words[n++] = v;
            }

            while(*end == ' ' || *end == '\t' || *end == '\r')
                end++;
            if(n == 0 || *end != '\0') {
                *errkey = lineno;
                return -2;
            }

            if(csr_ps_write(key, words, n) != 0) {
                *errkey = key;
                return -3;
            }
        }

        line = next;
    }

    return 0;
}

bool csr_switch_to_h4(int baud)
{
    uint16_t w[2];

    w[0] = CSR_HOST_INTERFACE_H4;
    if(csr_ps_write(CSR_PSKEY_HOST_INTERFACE, w, 1) != 0)
        return false;

    /* UART_BITRATE wins, UART_BAUDRATE (baud * 0.004096) is set to
     * match in case the ROM looks at it */
    w[0] = baud >> 16;
    w[1] = baud & 0xffff;
    if(csr_ps_write(CSR_PSKEY_UART_BITRATE, w, 2) != 0)
        return false;
    w[0] = ((baud / 125) * 512 + 500) / 1000;
    if(csr_ps_write(CSR_PSKEY_UART_BAUDRATE, w, 1) != 0)
        return false;

    /* H4 has no way to wake the chip without losing bytes */
    w[0] = 0;
    if(csr_ps_write(CSR_PSKEY_DEEP_SLEEP_STATE, w, 1) != 0)
        return false;

    return csr_warm_reset();
}

static volatile bool h4_closing;

void h4_open(int baud)
{
    /* uart_init() sets 8N1 with RTS/CTS */
    h4_closing = false;
    uart_init(BT_UART, baud);
}

void h4_close(void)
{
    /* uart_deinit() wakes up a pending uart_rx() */
    h4_closing = true;
    uart_deinit(BT_UART);
}

bool h4_send(const uint8_t *pkt, size_t len, int timeout)
{
    return uart_tx_timeout(BT_UART, pkt, len, timeout);
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

int h4_recv(uint8_t *buf, size_t len, int timeout)
{
    bool block = (timeout == TIMEOUT_BLOCK);
    long deadline = current_tick + (block ? 0 : timeout);
    size_t hdr, plen;

    if(len < 1 || !h4_read(buf, 1, deadline, block))
        return 0;

    switch(buf[0]) {
    case 0x01: hdr = 3; break; /* command */
    case 0x02: hdr = 4; break; /* ACL */
    case 0x03: hdr = 3; break; /* SCO */
    case 0x04: hdr = 2; break; /* event */
    case 0x05: hdr = 4; break; /* ISO */
    default:
        return -1;
    }

    if(len < 1 + hdr)
        return -1;
    if(!h4_read(&buf[1], hdr, deadline, block))
        return 0;

    switch(buf[0]) {
    case 0x02:
        plen = get16(&buf[3]);
        break;
    case 0x04:
        plen = buf[2];
        break;
    case 0x05:
        plen = get16(&buf[3]) & 0x3fff;
        break;
    default:
        plen = buf[3];
        break;
    }

    if(len < 1 + hdr + plen)
        return -1;
    if(!h4_read(&buf[1 + hdr], plen, deadline, block))
        return 0;

    return 1 + hdr + plen;
}

static int h4_cmd_send(uint16_t opcode, const uint8_t *params, size_t plen)
{
    static uint8_t cmd[4 + 255];

    if(plen > 255)
        return -2;

    cmd[0] = 0x01;
    put16(&cmd[1], opcode);
    cmd[3] = plen;
    if(plen)
        memcpy(&cmd[4], params, plen);

    return h4_send(cmd, 4 + plen, HZ/2) ? 0 : -2;
}

static int h4_cmd_wait(uint16_t opcode, uint8_t *ret, size_t retlen)
{
    static uint8_t evt[1 + 2 + 255];
    long deadline = current_tick + HZ;

    for(;;) {
        long timeout = deadline - current_tick;
        if(timeout <= 0)
            return -1;

        int n = h4_recv(evt, sizeof(evt), timeout);
        if(n == 0)
            return -1;
        if(n < 0)
            return -3;
        if(evt[0] != 0x04)
            continue;

        /* Command Complete */
        if(evt[1] == 0x0e && n >= 6 && get16(&evt[4]) == opcode) {
            size_t len = MIN((size_t)n - 6, retlen);
            memcpy(ret, &evt[6], len);
            return len;
        }

        /* Command Status with an error */
        if(evt[1] == 0x0f && n >= 7 && get16(&evt[5]) == opcode && evt[3]) {
            if(retlen)
                ret[0] = evt[3];
            return retlen ? 1 : 0;
        }
    }
}

int csr_hci_cmd_h4(uint16_t opcode, const uint8_t *params, size_t plen,
                   uint8_t *ret, size_t retlen)
{
    int rc = h4_cmd_send(opcode, params, plen);
    if(rc < 0)
        return rc;

    return h4_cmd_wait(opcode, ret, retlen);
}
