/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2021 Aidan MacDonald
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

#include "timer.h"
#include "system.h"
#include "irq-x1000.h"
#include "x1000/tcu.h"

#define TIMER_CHN 5

/* The one-shot's channel: like every channel but 5 it interrupts on TCU2,
 * where the others stay masked */
#define ONESHOT_CHN 6
#define ONESHOT_FREQ (X1000_EXCLK_FREQ / 64)

static void (*oneshot_fn)(void);

static void oneshot_off(void)
{
    jz_clr(TCU_ENABLE, 1 << ONESHOT_CHN);
    jz_set(TCU_MASK, 1 << ONESHOT_CHN);
    jz_clr(TCU_FLAG, 1 << ONESHOT_CHN);
    jz_set(TCU_STOP, 1 << ONESHOT_CHN);
}

void x1000_oneshot_start(uint32_t us, void (*fn)(void))
{
    uint32_t count = (uint64_t)us * ONESHOT_FREQ / 1000000;
    if(count < 1)
        count = 1;
    if(count > 0xffff)
        count = 0xffff;

    int irq = disable_irq_save();
    oneshot_fn = fn;
    jz_clr(TCU_STOP, 1 << ONESHOT_CHN);
    jz_clr(TCU_ENABLE, 1 << ONESHOT_CHN);
    jz_overwritef(TCU_CTRL(ONESHOT_CHN), SOURCE_V(EXT), PRESCALE_V(BY_64));
    REG_TCU_COUNT(ONESHOT_CHN) = 0;
    jz_write(TCU_CMP_FULL(ONESHOT_CHN), count);
    jz_write(TCU_CMP_HALF(ONESHOT_CHN), 0);
    jz_clr(TCU_FLAG, 1 << ONESHOT_CHN);
    jz_clr(TCU_MASK, 1 << ONESHOT_CHN);
    system_enable_irq(IRQ_TCU2);
    jz_set(TCU_ENABLE, 1 << ONESHOT_CHN);
    restore_irq(irq);
}

void x1000_oneshot_stop(void)
{
    int irq = disable_irq_save();
    oneshot_off();
    oneshot_fn = NULL;
    restore_irq(irq);
}

void TCU2(void)
{
    if(!(REG_TCU_FLAG & (1 << ONESHOT_CHN)))
        return;

    void (*fn)(void) = oneshot_fn;
    oneshot_off();
    oneshot_fn = NULL;
    if(fn)
        fn();
}

bool timer_set(long cycles, bool start)
{
    if(cycles <= 0)
        return false;

    /* Calculate timer interval */
    unsigned long counter = cycles;
    unsigned prescale = 0;
    while(counter > 0xffff && prescale < 5) {
        counter /= 4;
        prescale += 1;
    }

    /* Duration too long */
    if(counter > 0xffff)
        return false;

    /* Unregister old function */
    if(start && pfn_unregister) {
        pfn_unregister();
        pfn_unregister = 0;
    }

    /* Configure the timer */
    jz_clr(TCU_STOP, 1 << TIMER_CHN);
    jz_clr(TCU_ENABLE, 1 << TIMER_CHN);
    jz_overwritef(TCU_CTRL(TIMER_CHN), SOURCE_V(EXT), PRESCALE(prescale));
    jz_write(TCU_CMP_FULL(TIMER_CHN), counter);
    jz_write(TCU_CMP_HALF(TIMER_CHN), 0);
    jz_clr(TCU_FLAG, 1 << TIMER_CHN);
    jz_clr(TCU_MASK, 1 << TIMER_CHN);

    if(start)
        return timer_start();
    else
        return true;
}

bool timer_start(void)
{
    jz_set(TCU_ENABLE, 1 << TIMER_CHN);
    return true;
}

void timer_stop(void)
{
    jz_clr(TCU_ENABLE, 1 << TIMER_CHN);
    jz_set(TCU_MASK, 1 << TIMER_CHN);
    jz_clr(TCU_FLAG, 1 << TIMER_CHN);
    jz_set(TCU_STOP, 1 << TIMER_CHN);
}

void TCU1(void)
{
    jz_clr(TCU_FLAG, 1 << TIMER_CHN);

    if(pfn_timer)
        pfn_timer();
}
