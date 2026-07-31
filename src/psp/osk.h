/* pspssh — the on-screen keyboard.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PSP has no keyboard and it does have sceUtilityOsk — the same panel the
 * system software puts up for Wi-Fi passwords. Wrapping it is what let the
 * server details move off the memory card and into the application.
 *
 * It is a modal dialog: it takes the screen, runs until the user is done, and
 * hands back a string. That is why the shell it feeds is line-at-a-time rather
 * than character-at-a-time, and why `vim` is not a thing this client can drive.
 */

#ifndef PSPSSH_OSK_H
#define PSPSSH_OSK_H

/* What the keyboard should offer. The panel can always be switched with SELECT;
 * this only decides where it starts, which saves a press on the common case. */
typedef enum {
    OSK_TEXT,           /* letters, to begin with */
    OSK_DIGITS,         /* a port or a profile number */
    OSK_HOSTNAME        /* the URL panel: letters, digits, dots and dashes */
} osk_kind;

/* What came back. */
typedef enum {
    OSK_ENTERED = 1,    /* the user confirmed; `out` holds what they typed */
    OSK_CANCELLED = 0,  /* the user backed out; `out` is untouched */
    OSK_UNAVAILABLE = -1/* the keyboard would not start at all */
} osk_result;

/* Called once per frame while the keyboard is up, to paint what sits behind it.
 * The keyboard covers the lower half of the screen, so a form that keeps
 * drawing stays legible above it — and a black rectangle looks like a crash. */
typedef void (*osk_backdrop_fn)(void *ctx);

/* Puts the keyboard up and does not return until it is dismissed.
 *
 * `prompt` is the caption above the panel. `initial` pre-fills it and may be
 * NULL. `out` receives the result, up to `out_len` including the terminator.
 *
 * On OSK_CANCELLED `out` is left exactly as it was, which is what makes
 * "cancel" mean cancel: a caller passing the current value in `initial` and the
 * same buffer as `out` keeps it. */
osk_result osk_prompt(const char *prompt, const char *initial,
                      char *out, int out_len, osk_kind kind,
                      osk_backdrop_fn backdrop, void *backdrop_ctx);

#endif /* PSPSSH_OSK_H */
