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

/* The longest string the keyboard will ever hand back, terminator aside.
 *
 * Stated here because callers were sizing their buffers by guesswork. A 512
 * byte one implies a 511 character line and gets 255, which is not wrong so
 * much as misleading — and a buffer whose size means nothing is one nobody can
 * reason about. */
#define OSK_MAX_TEXT 255

/* What came back. */
typedef enum {
    OSK_ENTERED = 1,    /* the user confirmed; `out` holds what they typed */
    OSK_CANCELLED = 0,  /* the user backed out; `out` is untouched */
    OSK_UNAVAILABLE = -1/* the keyboard would not start at all */
} osk_result;

/* Called once per frame while the keyboard is up, to paint what sits behind it.
 * The keyboard covers the lower half of the screen, so a form that keeps
 * drawing stays legible above it — and a black rectangle looks like a crash.
 *
 * **Do not clear.** The screen has already been cleared when this is called,
 * through the graphics engine, and clearing again is the same buffer written
 * twice a frame with the second copy undoing the first. Paint only. */
typedef void (*osk_backdrop_fn)(void *ctx);

/* Puts the keyboard up and does not return until it is dismissed.
 *
 * `prompt` is the caption above the panel. `initial` pre-fills it and may be
 * NULL. `out` receives the result, up to `out_len` including the terminator.
 *
 * The panel always opens unrestricted, whatever the field is for: asking the
 * system for digits alone was the one departure from the SDK's sample, and it
 * was the port field that would not open on hardware.
 *
 * On OSK_CANCELLED `out` is left exactly as it was, which is what makes
 * "cancel" mean cancel: a caller passing the current value in `initial` and the
 * same buffer as `out` keeps it. */
osk_result osk_prompt(const char *prompt, const char *initial,
                      char *out, int out_len,
                      osk_backdrop_fn backdrop, void *backdrop_ctx);

/* Why OSK_UNAVAILABLE came back, in words, for putting in front of a person.
 * Never NULL. */
const char *osk_failure(void);

#endif /* PSPSSH_OSK_H */
