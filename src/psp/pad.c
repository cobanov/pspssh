/* pspssh — the buttons.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pad.h.
 */

#include "pad.h"

#include <pspctrl.h>

static unsigned int previous;
static unsigned int held;
static int started;
static volatile int exit_asked;

void pad_init(void)
{
    if (started) {
        return;
    }
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    started = 1;
    previous = 0;
}

unsigned int pad_pressed(void)
{
    SceCtrlData pad;
    unsigned int now;
    unsigned int newly;

    if (!started) {
        pad_init();
    }

    /* Latest sample rather than the whole buffer: this is called once a frame
     * and a queue of stale samples would make the menu respond to presses from
     * a second ago. */
    sceCtrlPeekBufferPositive(&pad, 1);
    now = pad.Buttons;

    newly = now & ~previous;
    previous = now;
    held = now;
    return newly;
}

unsigned int pad_held(void)
{
    return held;
}

int pad_exit_requested(void)
{
    return exit_asked;
}

void pad_set_exit_requested(void)
{
    exit_asked = 1;
}
