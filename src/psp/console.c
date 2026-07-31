/* pspssh — a scrolling log on the character grid.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See console.h.
 */

#include "console.h"
#include "gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char cells[GFX_ROWS][GFX_COLS];
static unsigned int colours[GFX_ROWS][GFX_COLS];

static int cursor_col;
static int cursor_row;
static unsigned int pen = GFX_WHITE;
static int ready;

static void blank_row(int row)
{
    int col;

    for (col = 0; col < GFX_COLS; col++) {
        cells[row][col] = ' ';
        colours[row][col] = GFX_WHITE;
    }
}

void console_reset(void)
{
    int row;

    for (row = 0; row < GFX_ROWS; row++) {
        blank_row(row);
    }
    cursor_col = 0;
    cursor_row = 0;
    pen = GFX_WHITE;
    ready = 1;
}

static void scroll_up(void)
{
    int row;

    for (row = 0; row + 1 < GFX_ROWS; row++) {
        memcpy(cells[row], cells[row + 1], GFX_COLS);
        memcpy(colours[row], colours[row + 1], sizeof(colours[row]));
    }
    blank_row(GFX_ROWS - 1);
}

static void newline(void)
{
    cursor_col = 0;
    if (++cursor_row >= GFX_ROWS) {
        cursor_row = GFX_ROWS - 1;
        scroll_up();
    }
}

static void put(char ch)
{
    if (ch == '\n') {
        newline();
        return;
    }
    if (ch == '\r') {
        cursor_col = 0;
        return;
    }
    if (ch == '\t') {
        /* Rounded up to the next multiple of eight, as a terminal does. Output
         * from a shell arrives with these in it and expanding them here beats
         * drawing a blank where a column was meant to be. */
        int next = (cursor_col + 8) & ~7;
        while (cursor_col < next && cursor_col < GFX_COLS) {
            cells[cursor_row][cursor_col] = ' ';
            colours[cursor_row][cursor_col] = pen;
            cursor_col++;
        }
        if (cursor_col >= GFX_COLS) {
            newline();
        }
        return;
    }

    if (cursor_col >= GFX_COLS) {
        newline();
    }
    cells[cursor_row][cursor_col] = ch;
    colours[cursor_row][cursor_col] = pen;
    cursor_col++;
}

/* Static now. It was public for a caller that would draw over the log in the
 * same frame, and no such caller was ever written. */
static void console_render(void)
{
    int row, col;

    if (!ready) {
        return;
    }
    for (row = 0; row < GFX_ROWS; row++) {
        for (col = 0; col < GFX_COLS; col++) {
            gfx_glyph(col, row, (unsigned char)cells[row][col],
                      colours[row][col], GFX_BLACK);
        }
    }
}

void console_printf(const char *fmt, ...)
{
    /* Two screenfuls. A single call that produced more than this would be a
     * caller with a different problem, and truncating is better than a stack
     * buffer sized by whatever it felt like passing. */
    char text[GFX_COLS * GFX_ROWS * 2];
    va_list args;
    const char *at;

    if (!ready) {
        console_reset();
    }

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    for (at = text; *at != '\0'; at++) {
        put(*at);
    }

    gfx_clear(GFX_BLACK);
    console_render();
    gfx_flip();
}
