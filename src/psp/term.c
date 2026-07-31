/* pspssh — a terminal, as far as one fits on a games console.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See term.h. Plain C with no platform in it, so the parser can be driven and
 * asserted on away from the device.
 */

#include "term.h"

#include <string.h>

#define DEFAULT_FG 7
#define DEFAULT_BG 0

/* Eight is more than any sequence a shell emits; SGR is the only one that comes
 * close and `ESC[1;31;42m` is three. A sequence with more is truncated rather
 * than rejected, since the alternative is printing it. */
#define MAX_PARAMS 8

enum {
    GROUND,
    SAW_ESC,
    IN_CSI,
    IN_OSC,          /* window title and friends: consumed, never shown */
    SAW_OSC_ESC,     /* an ESC inside OSC, which may begin the ST terminator */
    SKIP_ONE         /* charset selection and other two-byte sequences */
};

static term_cell grid[TERM_MAX_ROWS][TERM_MAX_COLS];
static int cols = TERM_MAX_COLS;
static int rows = TERM_MAX_ROWS;

static int cursor_col;
static int cursor_row;
static int saved_col;
static int saved_row;

/* The scroll region, as row indices. Everything outside it stays put when the
 * text inside it scrolls, which is how a shell keeps a status line still. */
static int scroll_top;
static int scroll_bottom;

static int pen_fg = DEFAULT_FG;
static int pen_bg = DEFAULT_BG;
static int bold;
static int reverse;

static int state = GROUND;
static int params[MAX_PARAMS];
static int param_count;
static int param_seen;      /* distinguishes "ESC[m" from "ESC[0m" */
static int private_marker;  /* the '?' in ESC[?25l */

static int bell_rang;

/* ------------------------------------------------------------------ grid -- */

static void blank_cell(term_cell *cell)
{
    cell->ch = ' ';
    /* Cleared cells take the current background, not the default one. A shell
     * that paints a coloured bar and then erases to the end of the line expects
     * the rest of the bar to be that colour. */
    cell->fg = (unsigned char)DEFAULT_FG;
    cell->bg = (unsigned char)(reverse ? pen_fg : pen_bg);
}

static void blank_row(int row)
{
    int col;

    for (col = 0; col < cols; col++) {
        blank_cell(&grid[row][col]);
    }
}

void term_reset(int c, int r)
{
    int row;

    cols = c > 0 && c <= TERM_MAX_COLS ? c : TERM_MAX_COLS;
    rows = r > 0 && r <= TERM_MAX_ROWS ? r : TERM_MAX_ROWS;

    pen_fg = DEFAULT_FG;
    pen_bg = DEFAULT_BG;
    bold = 0;
    reverse = 0;

    for (row = 0; row < TERM_MAX_ROWS; row++) {
        blank_row(row);
    }
    cursor_col = 0;
    cursor_row = 0;
    saved_col = 0;
    saved_row = 0;
    scroll_top = 0;
    scroll_bottom = rows - 1;
    state = GROUND;
    param_count = 0;
    param_seen = 0;
    private_marker = 0;
    bell_rang = 0;
}

int term_cols(void) { return cols; }
int term_rows(void) { return rows; }
int term_cursor_col(void) { return cursor_col; }
int term_cursor_row(void) { return cursor_row; }

const term_cell *term_at(int col, int row)
{
    if (col < 0 || row < 0 || col >= cols || row >= rows) {
        return NULL;
    }
    return &grid[row][col];
}

int term_take_bell(void)
{
    int rang = bell_rang;

    bell_rang = 0;
    return rang;
}

/* --------------------------------------------------------------- scroll -- */

static void scroll_region_up(int lines)
{
    int row;
    int i;

    for (i = 0; i < lines; i++) {
        for (row = scroll_top; row < scroll_bottom; row++) {
            memcpy(grid[row], grid[row + 1], sizeof(grid[row][0]) * (size_t)cols);
        }
        blank_row(scroll_bottom);
    }
}

static void scroll_region_down(int lines)
{
    int row;
    int i;

    for (i = 0; i < lines; i++) {
        for (row = scroll_bottom; row > scroll_top; row--) {
            memcpy(grid[row], grid[row - 1], sizeof(grid[row][0]) * (size_t)cols);
        }
        blank_row(scroll_top);
    }
}

static void line_feed(void)
{
    if (cursor_row == scroll_bottom) {
        scroll_region_up(1);
    } else if (cursor_row < rows - 1) {
        cursor_row++;
    }
}

/* ---------------------------------------------------------------- write -- */

static void put_char(unsigned char ch)
{
    term_cell *cell;

    if (cursor_col >= cols) {
        /* Wrap at the moment the next character arrives, not when the last one
         * filled the line. A shell that writes exactly `cols` characters and
         * then a newline should not leave a blank line behind. */
        cursor_col = 0;
        line_feed();
    }

    cell = &grid[cursor_row][cursor_col];
    cell->ch = ch;
    /* Bold is brightness here. There is one font and it has one weight, so the
     * choice is between showing bold as brighter or not showing it at all —
     * and a bold prompt that looks identical to a plain one loses information
     * that was deliberately sent. */
    cell->fg = (unsigned char)(reverse ? pen_bg : (bold ? (pen_fg | 8) : pen_fg));
    cell->bg = (unsigned char)(reverse ? (bold ? (pen_fg | 8) : pen_fg) : pen_bg);
    cursor_col++;
}

/* ----------------------------------------------------------------- CSI -- */

static int param_at(int index, int fallback)
{
    if (index >= param_count || (index == 0 && !param_seen)) {
        return fallback;
    }
    return params[index] > 0 ? params[index] : fallback;
}

/* Zero is meaningful for erase and SGR, where the default is 0 rather than 1. */
static int param_or_zero(int index)
{
    if (index >= param_count) {
        return 0;
    }
    return params[index];
}

static void clamp_cursor(void)
{
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_col >= cols) cursor_col = cols - 1;
    if (cursor_row >= rows) cursor_row = rows - 1;
}

static void erase_in_display(int mode)
{
    int row;
    int col;

    if (mode == 2 || mode == 3) {
        for (row = 0; row < rows; row++) {
            blank_row(row);
        }
        return;
    }
    if (mode == 1) {                    /* start of screen to cursor */
        for (row = 0; row < cursor_row; row++) {
            blank_row(row);
        }
        for (col = 0; col <= cursor_col && col < cols; col++) {
            blank_cell(&grid[cursor_row][col]);
        }
        return;
    }
    for (col = cursor_col; col < cols; col++) {      /* cursor to end */
        blank_cell(&grid[cursor_row][col]);
    }
    for (row = cursor_row + 1; row < rows; row++) {
        blank_row(row);
    }
}

static void erase_in_line(int mode)
{
    int col;

    if (mode == 1) {
        for (col = 0; col <= cursor_col && col < cols; col++) {
            blank_cell(&grid[cursor_row][col]);
        }
        return;
    }
    if (mode == 2) {
        blank_row(cursor_row);
        return;
    }
    for (col = cursor_col; col < cols; col++) {
        blank_cell(&grid[cursor_row][col]);
    }
}

static void delete_chars(int count)
{
    int col;

    for (col = cursor_col; col < cols; col++) {
        if (col + count < cols) {
            grid[cursor_row][col] = grid[cursor_row][col + count];
        } else {
            blank_cell(&grid[cursor_row][col]);
        }
    }
}

static void insert_chars(int count)
{
    int col;

    for (col = cols - 1; col >= cursor_col; col--) {
        if (col - count >= cursor_col) {
            grid[cursor_row][col] = grid[cursor_row][col - count];
        } else {
            blank_cell(&grid[cursor_row][col]);
        }
    }
}

static void apply_sgr(void)
{
    int i;

    if (param_count == 0) {
        params[0] = 0;
        param_count = 1;
    }

    for (i = 0; i < param_count; i++) {
        int p = params[i];

        if (p == 0) {
            pen_fg = DEFAULT_FG;
            pen_bg = DEFAULT_BG;
            bold = 0;
            reverse = 0;
        } else if (p == 1) {
            bold = 1;
        } else if (p == 2 || p == 22) {
            bold = 0;
        } else if (p == 7) {
            reverse = 1;
        } else if (p == 27) {
            reverse = 0;
        } else if (p >= 30 && p <= 37) {
            pen_fg = p - 30;
        } else if (p == 39) {
            pen_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            pen_bg = p - 40;
        } else if (p == 49) {
            pen_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            pen_fg = (p - 90) + 8;
        } else if (p >= 100 && p <= 107) {
            pen_bg = (p - 100) + 8;
        } else if ((p == 38 || p == 48) && i + 2 < param_count
                   && params[i + 1] == 5) {
            /* 256-colour, folded onto the sixteen this has. The low sixteen map
             * straight across; anything above is approximated to grey rather
             * than dropped, because losing the distinction between "coloured"
             * and "not" is what makes `ls` output unreadable. */
            int colour = params[i + 2];

            if (colour > 15) {
                colour = colour >= 244 ? 15 : 7;
            }
            if (p == 38) {
                pen_fg = colour;
            } else {
                pen_bg = colour;
            }
            i += 2;
        }
        /* Everything else — underline, blink, 24-bit colour — is consumed. A
         * console that cannot show it should not print its escape code. */
    }
}

static void dispatch_csi(unsigned char final)
{
    switch (final) {
    case 'A': cursor_row -= param_at(0, 1); clamp_cursor(); break;
    case 'B': cursor_row += param_at(0, 1); clamp_cursor(); break;
    case 'C': cursor_col += param_at(0, 1); clamp_cursor(); break;
    case 'D': cursor_col -= param_at(0, 1); clamp_cursor(); break;
    case 'E': cursor_row += param_at(0, 1); cursor_col = 0; clamp_cursor(); break;
    case 'F': cursor_row -= param_at(0, 1); cursor_col = 0; clamp_cursor(); break;
    case 'G': cursor_col = param_at(0, 1) - 1; clamp_cursor(); break;
    case 'd': cursor_row = param_at(0, 1) - 1; clamp_cursor(); break;

    case 'H':
    case 'f':
        cursor_row = param_at(0, 1) - 1;
        cursor_col = param_at(1, 1) - 1;
        clamp_cursor();
        break;

    case 'J': erase_in_display(param_or_zero(0)); break;
    case 'K': erase_in_line(param_or_zero(0)); break;

    case 'L': {
        /* Insert lines: the region below the cursor shifts down. Only inside
         * the scroll region, which is what makes it different from scrolling
         * the screen. */
        int keep_top = scroll_top;

        if (cursor_row >= scroll_top && cursor_row <= scroll_bottom) {
            scroll_top = cursor_row;
            scroll_region_down(param_at(0, 1));
            scroll_top = keep_top;
        }
        break;
    }
    case 'M': {
        int keep_top = scroll_top;

        if (cursor_row >= scroll_top && cursor_row <= scroll_bottom) {
            scroll_top = cursor_row;
            scroll_region_up(param_at(0, 1));
            scroll_top = keep_top;
        }
        break;
    }

    case 'P': delete_chars(param_at(0, 1)); break;
    case '@': insert_chars(param_at(0, 1)); break;
    case 'S': scroll_region_up(param_at(0, 1)); break;
    case 'T': scroll_region_down(param_at(0, 1)); break;

    case 'X': {                         /* erase characters in place */
        int count = param_at(0, 1);
        int col;

        for (col = cursor_col; col < cols && col < cursor_col + count; col++) {
            blank_cell(&grid[cursor_row][col]);
        }
        break;
    }

    case 'm': apply_sgr(); break;

    case 'r':
        /* The scroll region. Out-of-order or out-of-range values reset it,
         * which is what a real terminal does and what shells rely on when they
         * "clear" the region by sending ESC[r. */
        scroll_top = param_at(0, 1) - 1;
        scroll_bottom = param_count > 1 ? param_at(1, rows) - 1 : rows - 1;
        if (scroll_top < 0 || scroll_bottom >= rows || scroll_top >= scroll_bottom) {
            scroll_top = 0;
            scroll_bottom = rows - 1;
        }
        cursor_col = 0;
        cursor_row = scroll_top;
        break;

    case 's': saved_col = cursor_col; saved_row = cursor_row; break;
    case 'u': cursor_col = saved_col; cursor_row = saved_row; clamp_cursor(); break;

    default:
        /* Cursor visibility, bracketed paste, mouse reporting, device status.
         * Nothing here can act on them and printing them would be worse than
         * ignoring them. */
        break;
    }
    (void)private_marker;
}

/* ---------------------------------------------------------------- feed -- */

void term_feed(const unsigned char *data, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        unsigned char ch = data[i];

        switch (state) {
        case GROUND:
            if (ch == 0x1b) {
                state = SAW_ESC;
            } else if (ch == '\n') {
                line_feed();
            } else if (ch == '\r') {
                cursor_col = 0;
            } else if (ch == '\b') {
                if (cursor_col > 0) {
                    cursor_col--;
                }
            } else if (ch == '\t') {
                int next = (cursor_col + 8) & ~7;

                if (next >= cols) {
                    next = cols - 1;
                }
                while (cursor_col < next) {
                    put_char(' ');
                }
            } else if (ch == 0x07) {
                bell_rang = 1;
            } else if (ch >= 0x20 && ch != 0x7f) {
                /* Bytes above 0x7e are drawn as a blank by the font, so UTF-8
                 * arrives as spaces rather than as mojibake. Naming that here
                 * is better than a font that silently eats half a character. */
                put_char(ch);
            }
            break;

        case SAW_ESC:
            if (ch == '[') {
                state = IN_CSI;
                param_count = 0;
                param_seen = 0;
                private_marker = 0;
                memset(params, 0, sizeof(params));
            } else if (ch == ']') {
                state = IN_OSC;
            } else if (ch == '(' || ch == ')' || ch == '#' || ch == '%') {
                state = SKIP_ONE;
            } else if (ch == 'M') {
                /* Reverse index: up one line, scrolling the region if at the
                 * top. `less` and `man` lean on this. */
                if (cursor_row == scroll_top) {
                    scroll_region_down(1);
                } else if (cursor_row > 0) {
                    cursor_row--;
                }
                state = GROUND;
            } else if (ch == 'D') {
                line_feed();
                state = GROUND;
            } else if (ch == 'E') {
                cursor_col = 0;
                line_feed();
                state = GROUND;
            } else if (ch == 'c') {
                term_reset(cols, rows);
            } else {
                state = GROUND;
            }
            break;

        case IN_CSI:
            if (ch >= '0' && ch <= '9') {
                if (param_count == 0) {
                    param_count = 1;
                }
                if (param_count <= MAX_PARAMS) {
                    int at = param_count - 1;

                    params[at] = params[at] * 10 + (ch - '0');
                    /* Bounded so a server sending ESC[99999999999m cannot
                     * overflow its way to a different command. */
                    if (params[at] > 9999) {
                        params[at] = 9999;
                    }
                }
                param_seen = 1;
            } else if (ch == ';') {
                if (param_count < MAX_PARAMS) {
                    param_count++;
                }
                param_seen = 1;
            } else if (ch == '?' || ch == '>' || ch == '!') {
                private_marker = ch;
            } else if (ch >= 0x40 && ch <= 0x7e) {
                dispatch_csi(ch);
                state = GROUND;
            } else {
                /* Intermediate bytes such as ' ' in ESC[ q. Consumed. */
            }
            break;

        case IN_OSC:
            /* Runs until BEL or the two-byte ST. Window titles are the common
             * case and there is nothing to do with one, but it has to be eaten
             * rather than printed — a title is often the whole command line. */
            if (ch == 0x07) {
                state = GROUND;
            } else if (ch == 0x1b) {
                state = SAW_OSC_ESC;
            }
            break;

        case SAW_OSC_ESC:
            /* ESC \ ends it; anything else was an ESC inside the string. */
            state = (ch == '\\') ? GROUND : IN_OSC;
            break;

        case SKIP_ONE:
        default:
            state = GROUND;
            break;
        }
    }
}
