/* pspssh — the buttons.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sceCtrlReadBufferPositive reports what is held down, which is the wrong
 * question for a menu: a button held for a tenth of a second is read sixty
 * times and moves the selection sixty rows. Every caller here wants "what was
 * newly pressed since I last looked", so that is what this returns.
 */

#ifndef PSPSSH_PAD_H
#define PSPSSH_PAD_H

/* Starts sampling. Safe to call more than once. */
void pad_init(void);

/* Buttons that went down since the previous call, as PSP_CTRL_* flags.
 *
 * Reads the pad exactly once, so calling it twice in a frame gives the second
 * caller nothing — there is one of these per loop iteration by design. */
unsigned int pad_pressed(void);

/* Buttons currently held, for the rare caller that wants a repeat. */
unsigned int pad_held(void);

/* Whether the user has asked the system to close the application. Set from the
 * exit callback, so it is true even while a modal dialog is up. */
int pad_exit_requested(void);
void pad_set_exit_requested(void);

#endif /* PSPSSH_PAD_H */
