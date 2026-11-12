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
 * This software is distributed on an "AS IS" BASIS, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Counters for chasing A2DP stutter, shown in the Bluetooth debug screen.
 * They are hooked where every target passes through (HCI send/receive,
 * the A2DP media tick, the allocator), so two controllers can be compared
 * on the same build. */

#ifndef __OSI_BT_STATS_H__
#define __OSI_BT_STATS_H__

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"

#if CONFIG_CPU == X1000 && !defined(SIMULATOR)
typedef uint32_t bt_stats_time_t;
static inline bt_stats_time_t bt_stats_now(void) { return __ost_read32(); }
#define BT_STATS_US(d)  ((d) / OST_TICKS_PER_US)
#else
typedef long bt_stats_time_t;
static inline bt_stats_time_t bt_stats_now(void) { return current_tick; }
#define BT_STATS_US(d)  ((uint32_t)(d) * (1000000 / HZ))
#endif

/* Bounds of the NOCP wait histogram, in ms; the last bucket is open */
#define BT_STATS_GAP_BUCKETS 5
extern const uint16_t bt_stats_gap_ms[BT_STATS_GAP_BUCKETS - 1];

struct bt_stats {
    /* HCI host -> controller, timed around the driver's send() */
    uint32_t tx_pkts;
    uint32_t tx_acl;
    uint32_t tx_us_max;
    uint32_t tx_slow;           /* send() over 4 ms: CTS held off */
    uint32_t tx_drops;          /* driver gave up on a packet */

    /* HCI controller -> host */
    uint32_t rx_pkts;
    uint32_t rx_acl;

    /* ACL credits: from Read_Buffer_Size and Number_Of_Completed_Packets */
    uint16_t acl_size;
    uint16_t acl_bufs;
    uint32_t acl_out;           /* sent but not completed */
    uint32_t acl_out_max;
    uint32_t acl_full;          /* sends that used the last credit */
    uint32_t nocp_evts;
    uint32_t nocp_pkts;
    uint32_t nocp_gap_us_max;   /* longest wait for a credit back */
    uint32_t nocp_gap[BT_STATS_GAP_BUCKETS];

    /* Link events */
    uint32_t mode_chg;
    uint8_t  mode;              /* 0 active, 1 hold, 2 sniff */
    uint16_t mode_intv;
    uint32_t role_chg;
    uint8_t  role;              /* 0 master, 1 slave */
    uint32_t slots_chg;
    uint8_t  slots;
    uint32_t pkt_type_chg;
    uint16_t pkt_type;
    uint32_t flush_occ;         /* packets the controller flushed */
    uint32_t qos_viol;
    uint32_t hw_err;
    int8_t   rssi;              /* Read_RSSI: dB off the golden range */

    /* Read_AFH_Channel_Map: bit n set while the link hops on channel n */
    uint8_t  afh_map[10];
    uint8_t  afh_chans;         /* channels in use, 0 until the first answer */
    uint32_t afh_reads;         /* answers with AFH on, a new map each */

    /* A2DP source */
    uint32_t a2d_ticks;
    uint32_t a2d_tick_gap_us_max;
    uint32_t a2d_tick_late;     /* media tick over 1.5 periods late */
    uint32_t a2d_work_us_max;   /* encode + queue per tick */
    uint32_t a2d_q_max;
    uint32_t a2d_q_drops;       /* encoded packets dropped, link too slow */
    uint32_t a2d_av_drops;      /* dropped by BTA AV, L2CAP backed up */
    uint32_t a2d_av_list_max;   /* most packets held back by BTA AV */
    uint32_t a2d_underflow;     /* PCM not there when the tick wanted it */
    uint16_t a2d_bitpool;       /* in use now, see the dynamic bitpool */
    uint16_t a2d_bp_min;        /* lowest bitpool used */
    uint32_t a2d_bp_down;
    uint32_t a2d_bp_up;
    uint16_t a2d_bitrate;
    uint16_t a2d_mtu;

    /* PCM feed into Bluedroid */
    uint32_t pcm_short;         /* fewer bytes than asked for */
    uint32_t pcm_silence;       /* sink locked or stopped */

    /* Bluedroid heap */
    uint32_t alloc_calls;
    uint32_t alloc_us_max;
    uint32_t free_us_max;
    uint32_t alloc_slow;        /* malloc or free over 200 us */

    /* Transport errors the driver saw, e.g. UART overruns. Copied in by
     * the driver, so it's a running total that reset doesn't clear. */
    uint32_t drv_rx_errors;

    /* The controller's sleep clock as the driver measured it at power-on,
     * 0 if it couldn't */
    uint32_t lpo_hz;
};

extern struct bt_stats bt_stats;

static inline void bt_stats_max(uint32_t *max, uint32_t v)
{
    if(v > *max)
        *max = v;
}

void bt_stats_reset(void);
void bt_stats_hci_tx(const uint8_t *data, uint16_t len, bt_stats_time_t t);
void bt_stats_hci_rx(const uint8_t *data, uint16_t len);
void bt_stats_alloc(bt_stats_time_t t, bool is_free);

#endif /* __OSI_BT_STATS_H__ */
