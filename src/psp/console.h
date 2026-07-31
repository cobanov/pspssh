/* pspssh — a scrolling log on the character grid.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the application says while it is starting up: which Wi-Fi profiles
 * exist, what the radio is doing, whether the socket opened. It appends and
 * scrolls, which is all a progress log needs and all pspDebugScreen ever did.
 *
 * It is not the terminal. A terminal addresses cells, erases regions and moves
 * a cursor backwards; this only ever moves forward. Keeping the two apart means
 * the boot log does not pay for VT parsing and the terminal does not inherit a
 * teletype's assumptions.
 *
 * A grid is kept rather than only painted, because the display is double
 * buffered: the buffer being drawn into is two frames old, so every frame is
 * repainted in full from this grid. See gfx.h.
 */

#ifndef PSPSSH_CONSOLE_H
#define PSPSSH_CONSOLE_H

/* Empties the grid and puts the cursor back at the top. */
void console_reset(void);

/* The colour subsequent text is written in. Persists until changed. */
void console_colour(unsigned int fg);

/* Appends at the cursor, wrapping at the right edge and scrolling at the
 * bottom. Understands '\n'; everything else is a glyph.
 *
 * Presents the result before returning, so a caller printing progress does not
 * have to know the screen is double buffered. That costs a vertical blank per
 * call, which is the right trade for a log that prints tens of lines and the
 * wrong one for a terminal — which is why the terminal does not use this. */
void console_printf(const char *fmt, ...);

/* Repaints the grid into the current draw buffer without presenting it, for a
 * caller drawing something else on top in the same frame. */
void console_render(void);

#endif /* PSPSSH_CONSOLE_H */
