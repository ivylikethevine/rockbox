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

#include <string.h>
#include "osi/bt_stats.h"

struct bt_stats bt_stats;

const uint16_t bt_stats_gap_ms[BT_STATS_GAP_BUCKETS - 1] = { 10, 20, 50, 100 };

/* When the oldest uncompleted ACL packet was sent, or the last credit
 * came back while more were still out */
static bt_stats_time_t nocp_wait_start;

static uint16_t get16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

void bt_stats_reset(void)
{
    /* Keep what describes the current state rather than counts it */
    struct bt_stats keep = bt_stats;

    memset(&bt_stats, 0, sizeof(bt_stats));
    bt_stats.acl_size = keep.acl_size;
    bt_stats.acl_bufs = keep.acl_bufs;
    bt_stats.acl_out = keep.acl_out;
    bt_stats.mode = keep.mode;
    bt_stats.mode_intv = keep.mode_intv;
    bt_stats.role = keep.role;
    bt_stats.slots = keep.slots;
    bt_stats.pkt_type = keep.pkt_type;
    bt_stats.rssi = keep.rssi;
    memcpy(bt_stats.afh_map, keep.afh_map, sizeof(bt_stats.afh_map));
    bt_stats.afh_chans = keep.afh_chans;
    bt_stats.afh_reads = keep.afh_reads;
    bt_stats.lpo_hz = keep.lpo_hz;
    bt_stats.a2d_bitpool = keep.a2d_bitpool;
    bt_stats.a2d_bp_min = keep.a2d_bitpool;
    bt_stats.a2d_bitrate = keep.a2d_bitrate;
    bt_stats.a2d_mtu = keep.a2d_mtu;
}

void bt_stats_hci_tx(const uint8_t *data, uint16_t len, bt_stats_time_t t)
{
    uint32_t us = BT_STATS_US(t);

    bt_stats.tx_pkts++;
    bt_stats_max(&bt_stats.tx_us_max, us);
    if(us > 4000)
        bt_stats.tx_slow++;

    if(len < 1 || data[0] != 0x02)
        return;

    /* Each H4 ACL packet takes one controller buffer */
    bt_stats.tx_acl++;
    if(bt_stats.acl_out++ == 0)
        nocp_wait_start = bt_stats_now();
    bt_stats_max(&bt_stats.acl_out_max, bt_stats.acl_out);
    if(bt_stats.acl_bufs && bt_stats.acl_out >= bt_stats.acl_bufs)
        bt_stats.acl_full++;
}

static void nocp(const uint8_t *p, uint8_t plen)
{
    uint32_t done = 0;
    int n = p[0];

    if(plen < 1 + 4 * n)
        return;
    for(int i = 0; i < n; ++i)
        done += get16(&p[1 + 4*i + 2]);

    bt_stats.nocp_evts++;
    bt_stats.nocp_pkts += done;

    bt_stats_time_t now = bt_stats_now();
    if(bt_stats.acl_out > 0 && done > 0) {
        uint32_t ms, us = BT_STATS_US(now - nocp_wait_start);
        bt_stats_max(&bt_stats.nocp_gap_us_max, us);

        ms = us / 1000;
        int b = 0;
        while(b < BT_STATS_GAP_BUCKETS - 1 && ms >= bt_stats_gap_ms[b])
            b++;
        bt_stats.nocp_gap[b]++;
    }

    bt_stats.acl_out = (done > bt_stats.acl_out) ? 0 : bt_stats.acl_out - done;
    nocp_wait_start = now;
}

void bt_stats_hci_rx(const uint8_t *data, uint16_t len)
{
    bt_stats.rx_pkts++;
    if(len >= 1 && data[0] == 0x02)
        bt_stats.rx_acl++;

    if(len < 3 || data[0] != 0x04)
        return;

    const uint8_t *p = &data[3];
    uint8_t plen = data[2];
    if(len < 3 + plen)
        return;

    switch(data[1]) {
    case 0x05: /* Disconnection Complete: its buffers are freed */
        bt_stats.acl_out = 0;
        break;
    case 0x0e: /* Command Complete */
        if(plen < 4 || p[3] != 0)
            break;
        switch(get16(&p[1])) {
        case 0x1005: /* Read_Buffer_Size */
            if(plen >= 11) {
                bt_stats.acl_size = get16(&p[4]);
                bt_stats.acl_bufs = get16(&p[7]);
            }
            break;
        case 0x1405: /* Read_RSSI */
            if(plen >= 7)
                bt_stats.rssi = (int8_t)p[6];
            break;
        case 0x1406: /* Read_AFH_Channel_Map, with AFH on */
            if(plen >= 17 && p[6] == 1) {
                int chans = 0;
                for(int i = 0; i < 79; ++i)
                    chans += (p[7 + i / 8] >> (i % 8)) & 1;
                memcpy(bt_stats.afh_map, &p[7], sizeof(bt_stats.afh_map));
                bt_stats.afh_chans = chans;
                bt_stats.afh_reads++;
            }
            break;
        }
        break;
    case 0x10: /* Hardware Error */
        bt_stats.hw_err++;
        break;
    case 0x11: /* Flush Occurred */
        bt_stats.flush_occ++;
        break;
    case 0x12: /* Role Change */
        if(plen >= 8 && p[0] == 0) {
            bt_stats.role_chg++;
            bt_stats.role = p[7];
        }
        break;
    case 0x13: /* Number Of Completed Packets */
        if(plen >= 1)
            nocp(p, plen);
        break;
    case 0x14: /* Mode Change */
        if(plen >= 6 && p[0] == 0) {
            bt_stats.mode_chg++;
            bt_stats.mode = p[3];
            bt_stats.mode_intv = get16(&p[4]);
        }
        break;
    case 0x1b: /* Max Slots Change */
        if(plen >= 3) {
            bt_stats.slots_chg++;
            bt_stats.slots = p[2];
        }
        break;
    case 0x1d: /* Connection Packet Type Changed */
        if(plen >= 5 && p[0] == 0) {
            bt_stats.pkt_type_chg++;
            bt_stats.pkt_type = get16(&p[3]);
        }
        break;
    case 0x1e: /* QoS Violation */
        bt_stats.qos_viol++;
        break;
    }
}

void bt_stats_alloc(bt_stats_time_t t, bool is_free)
{
    uint32_t us = BT_STATS_US(t);

    if(is_free) {
        bt_stats_max(&bt_stats.free_us_max, us);
    } else {
        bt_stats.alloc_calls++;
        bt_stats_max(&bt_stats.alloc_us_max, us);
    }
    if(us > 200)
        bt_stats.alloc_slow++;
}
