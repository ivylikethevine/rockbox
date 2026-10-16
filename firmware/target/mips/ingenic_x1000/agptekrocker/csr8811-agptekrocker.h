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

#ifndef __CSR8811_AGPTEKROCKER_H__
#define __CSR8811_AGPTEKROCKER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CSR8811 on UART0. Out of reset the chip talks BCSP at 115200 8E1;
 * PS keys loaded over BCSP switch it to H4 after a warm reset. */

#define CSR_PSR_FILE        "/.rockbox/fw/csr8811.psr"
#define CSR_BCSP_BOOT_BAUD  115200
#define CSR_H4_BAUD         3000000

void csr_power(bool on);

/* Configures UART0 for BCSP and brings up the BCSP link. */
bool bcsp_open(int baud);
void bcsp_close(void);

/* Sends every key in a .psr file to the PS RAM store. Returns 0 on
 * success, or -1 for an unreadable file, -2 for a syntax error and
 * -3 for a key the chip rejected. 'errkey' gets the offending line
 * number or key. */
int csr_psr_load(const char *path, int *errkey);

/* 'addr' is in display order, addr[0] being the NAP's high byte */
bool csr_set_bdaddr(const uint8_t addr[6]);

/* Overrides the transport keys for H4 at 'baud' with deep sleep off,
 * then warm resets the chip. Call after csr_psr_load(); the BCSP link
 * is gone afterwards. */
bool csr_switch_to_h4(int baud);

/* H4 transport: 8N1 with RTS/CTS. h4_close() also makes a blocked
 * h4_recv() return 0. */
void h4_open(int baud);
void h4_close(void);
/* Returns false if CTS kept us from sending within 'timeout' */
bool h4_send(const uint8_t *pkt, size_t len, int timeout);
/* Receives one complete packet, H4 type byte included. Returns its
 * length, 0 on timeout or -1 if the stream is out of sync or the
 * packet doesn't fit. */
int h4_recv(uint8_t *buf, size_t len, int timeout);

/* HCI command over H4. Returns the length of the Command Complete
 * return parameters copied to 'ret', -1 on a receive timeout, -2 if
 * sending timed out and -3 on a framing error. */
int csr_hci_cmd_h4(uint16_t opcode, const uint8_t *params, size_t plen,
                   uint8_t *ret, size_t retlen);

#endif /* __CSR8811_AGPTEKROCKER_H__ */
