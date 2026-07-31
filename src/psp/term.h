/* pspssh — a terminal, as far as one fits on a games console.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A screen buffer and the escape-sequence parser that writes into it. What
 * arrives from a shell is not text — it is text with instructions threaded
 * through it, and a client that prints those instructions shows `ESC[0;32m`
 * where it should show a green prompt.
 *
 * ## Deliberately not the drawing layer
 *
 * Nothing here knows about the PSP. It keeps a grid of characters and colours;
 * something else paints it. That is what lets tools/test-terminal.sh drive the
 * whole parser on a laptop and assert on the result — cursor moves, erases and
 * scroll regions are exactly the kind of thing that is easy to get subtly wrong
 * and impossible to debug on a device with no console.
 *
 * ## What it implements
 *
 * The subset a shell actually emits: cursor movement and addressing, erase in
 * line and display, insert and delete lines, delete characters, colours
 * including bright and reverse, and a scroll region. Sequences outside that are
 * consumed rather than printed, because showing them is worse than ignoring
 * them.
 *
 * It is not a VT100 and does not claim to be. No character sets, no double
 * width, no alternate screen. `vim` will look wrong; `sh`, `ls`, `top` and a
 * coloured prompt will not.
 */

#ifndef PSPSSH_TERM_H
#define PSPSSH_TERM_H

#define TERM_MAX_COLS 80
#define TERM_MAX_ROWS 40

/* Colours are indices, 0-7 normal and 8-15 bright, as SGR numbers them. The
 * drawing layer decides what they look like; this only has to be consistent. */
typedef struct {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
} term_cell;

/* Clears everything and sets the size. Clamped to the maxima above. */
void term_reset(int cols, int rows);

/* Feeds bytes from the far side. Any length, split anywhere: an escape
 * sequence cut in half by a packet boundary is the normal case, not an edge
 * one, so the parser keeps its state between calls. */
void term_feed(const unsigned char *data, int len);

int term_cols(void);
int term_rows(void);
int term_cursor_col(void);
int term_cursor_row(void);

/* Never NULL for coordinates inside the grid; NULL outside it. */
const term_cell *term_at(int col, int row);

/* Whether the far side rang the bell since this was last asked. */
int term_take_bell(void);

#endif /* PSPSSH_TERM_H */
