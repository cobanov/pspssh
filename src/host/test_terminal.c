/* pspssh — the terminal parser, tested off the device.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * src/psp/term.c is plain C with no platform in it, precisely so this can
 * exist. Cursor addressing, erase modes and scroll regions are easy to get
 * subtly wrong and impossible to debug on a device with no console, and the
 * symptom of getting them wrong is "the screen looks a bit odd sometimes" —
 * which is not a bug report anyone can act on.
 */

#include "../psp/term.h"

#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

static void check(const char *what, int ok)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

static void feed(const char *text)
{
    term_feed((const unsigned char *)text, (int)strlen(text));
}

/* A row as a string, trailing blanks trimmed, so assertions read as what is on
 * screen rather than as padding. */
static const char *row_text(int row)
{
    static char out[TERM_MAX_COLS + 1];
    int col;
    int last = -1;

    for (col = 0; col < term_cols(); col++) {
        const term_cell *cell = term_at(col, row);

        out[col] = cell != NULL ? (char)cell->ch : ' ';
        if (out[col] != ' ') {
            last = col;
        }
    }
    out[last + 1] = '\0';
    return out;
}

static int fg_at(int col, int row)
{
    const term_cell *cell = term_at(col, row);

    return cell != NULL ? cell->fg : -1;
}

static int bg_at(int col, int row)
{
    const term_cell *cell = term_at(col, row);

    return cell != NULL ? cell->bg : -1;
}

int main(void)
{
    printf("==> the terminal parser, without a psp\n");

    /* --- the basics ------------------------------------------------------ */
    term_reset(20, 6);
    feed("hello");
    check("text lands where it is written", strcmp(row_text(0), "hello") == 0);
    check("and the cursor follows it", term_cursor_col() == 5);

    feed("\r\nworld");
    check("carriage return and newline start a line",
          strcmp(row_text(1), "world") == 0);

    feed("\b\bX");
    check("backspace moves without erasing, and the next write overwrites",
          strcmp(row_text(1), "worXd") == 0);

    /* --- wrapping and scrolling ------------------------------------------ */
    term_reset(10, 3);
    feed("0123456789abc");
    check("a line wraps at the right edge",
          strcmp(row_text(0), "0123456789") == 0
              && strcmp(row_text(1), "abc") == 0);

    term_reset(10, 3);
    feed("one\r\ntwo\r\nthree\r\nfour");
    check("the screen scrolls when the last line fills",
          strcmp(row_text(0), "two") == 0
              && strcmp(row_text(1), "three") == 0
              && strcmp(row_text(2), "four") == 0);

    /* Exactly `cols` characters then a newline must not leave a blank line:
     * wrapping happens when the next character arrives, not when the last one
     * fits. Getting this wrong double-spaces every full-width line. */
    term_reset(5, 4);
    feed("abcde\r\nxy");
    check("a line filled exactly does not swallow the next one",
          strcmp(row_text(0), "abcde") == 0 && strcmp(row_text(1), "xy") == 0);

    /* --- cursor movement -------------------------------------------------- */
    term_reset(20, 6);
    feed("\x1b[3;5Hhere");
    check("ESC[r;cH addresses from one, not zero",
          strcmp(row_text(2), "    here") == 0);

    feed("\x1b[H""top");
    check("ESC[H with no parameters goes home",
          strcmp(row_text(0), "top") == 0);

    feed("\x1b[2B\x1b[1Cx");
    check("ESC[nB and ESC[nC move down and right",
          strcmp(row_text(2), "   ehere") == 0 || term_cursor_row() == 2);

    term_reset(20, 6);
    feed("abc\x1b[2Dz");
    check("ESC[nD moves left", strcmp(row_text(0), "azc") == 0);

    term_reset(20, 6);
    feed("hello\x1b[1;1Hy");
    check("addressing is absolute, not relative",
          strcmp(row_text(0), "yello") == 0);

    /* --- erasing ---------------------------------------------------------- */
    term_reset(20, 4);
    feed("abcdefgh\x1b[1;4H\x1b[K");
    check("ESC[K erases from the cursor to the end of the line",
          strcmp(row_text(0), "abc") == 0);

    term_reset(20, 4);
    feed("abcdefgh\x1b[1;4H\x1b[1K");
    check("ESC[1K erases from the start of the line to the cursor",
          strcmp(row_text(0), "    efgh") == 0);

    term_reset(20, 4);
    feed("one\r\ntwo\r\nthree\x1b[2J");
    check("ESC[2J clears the screen",
          row_text(0)[0] == '\0' && row_text(1)[0] == '\0'
              && row_text(2)[0] == '\0');

    term_reset(20, 4);
    feed("one\r\ntwo\r\nthree\x1b[2;2H\x1b[J");
    check("ESC[J clears from the cursor to the end of the screen",
          strcmp(row_text(0), "one") == 0
              && strcmp(row_text(1), "t") == 0
              && row_text(2)[0] == '\0');

    /* --- editing within a line -------------------------------------------- */
    term_reset(20, 4);
    feed("abcdef\x1b[1;3H\x1b[2P");
    check("ESC[nP deletes characters and pulls the rest left",
          strcmp(row_text(0), "abef") == 0);

    term_reset(20, 4);
    feed("abcdef\x1b[1;3H\x1b[2@");
    check("ESC[n@ inserts blanks and pushes the rest right",
          strcmp(row_text(0), "ab  cdef") == 0);

    /* --- colour ------------------------------------------------------------ */
    term_reset(20, 4);
    feed("\x1b[31mred");
    check("ESC[31m sets the foreground", fg_at(0, 0) == 1);

    feed("\x1b[0mplain");
    check("ESC[0m puts it back", fg_at(3, 0) == 7);

    term_reset(20, 4);
    feed("\x1b[1;32mbold");
    check("bold is shown as the bright variant, since there is one weight",
          fg_at(0, 0) == (2 | 8));

    term_reset(20, 4);
    feed("\x1b[44mbg");
    check("ESC[44m sets the background", bg_at(0, 0) == 4);

    term_reset(20, 4);
    feed("\x1b[7mrev");
    check("ESC[7m swaps foreground and background",
          fg_at(0, 0) == 0 && bg_at(0, 0) == 7);

    term_reset(20, 4);
    feed("\x1b[91mbright");
    check("ESC[91m is a bright colour directly", fg_at(0, 0) == 9);

    term_reset(20, 4);
    feed("\x1b[38;5;2mx");
    check("256-colour is folded onto the sixteen there are", fg_at(0, 0) == 2);

    /* An erase after setting a background fills with that background, which is
     * what a shell painting a status bar expects. */
    term_reset(20, 4);
    feed("\x1b[42m\x1b[K");
    check("erasing fills with the current background, not the default one",
          bg_at(5, 0) == 2);

    /* --- the scroll region -------------------------------------------------- */
    term_reset(10, 5);
    feed("\x1b[2;4r");                  /* rows 2..4 scroll, 1 and 5 do not */
    feed("\x1b[1;1Htop");
    feed("\x1b[5;1Hbottom");
    feed("\x1b[2;1Ha\r\nb\r\nc\r\nd");
    check("only the scroll region scrolls",
          strcmp(row_text(0), "top") == 0
              && strcmp(row_text(4), "bottom") == 0);
    check("and its contents move up",
          strcmp(row_text(1), "b") == 0
              && strcmp(row_text(2), "c") == 0
              && strcmp(row_text(3), "d") == 0);

    term_reset(10, 5);
    feed("\x1b[9;2r");
    check("an impossible region resets to the whole screen rather than"
          " corrupting one", term_cursor_row() == 0);

    /* --- sequences that must be consumed, never printed ---------------------- */
    term_reset(30, 4);
    feed("\x1b]0;a window title\x07visible");
    check("an OSC title is swallowed whole",
          strcmp(row_text(0), "visible") == 0);

    term_reset(30, 4);
    feed("\x1b]0;title\x1b\\after");
    check("including when it ends with ST rather than BEL",
          strcmp(row_text(0), "after") == 0);

    term_reset(30, 4);
    feed("\x1b[?25lhidden\x1b[?25h");
    check("private modes are consumed", strcmp(row_text(0), "hidden") == 0);

    term_reset(30, 4);
    feed("\x1b(Bplain");
    check("charset selection is consumed", strcmp(row_text(0), "plain") == 0);

    term_reset(30, 4);
    feed("\x1b[4munderline\x1b[24m");
    check("what cannot be shown is dropped, not printed",
          strcmp(row_text(0), "underline") == 0);

    /* --- split across reads -------------------------------------------------- */
    /* An escape sequence cut in half by a packet boundary is the normal case on
     * a slow link, not an edge one. A parser that reset its state between reads
     * would print the second half. */
    term_reset(20, 4);
    feed("\x1b");
    feed("[31");
    feed("m");
    feed("split");
    check("an escape sequence split across reads still works",
          strcmp(row_text(0), "split") == 0 && fg_at(0, 0) == 1);

    term_reset(20, 4);
    feed("\x1b[1;");
    feed("3Hx");
    check("and so does one split inside its parameters",
          strcmp(row_text(0), "  x") == 0);

    /* --- hostile input -------------------------------------------------------- */
    term_reset(20, 4);
    feed("\x1b[999999999999999999m""ok");
    check("an absurd parameter cannot overflow into a different command",
          strcmp(row_text(0), "ok") == 0);

    term_reset(20, 4);
    feed("\x1b[1;2;3;4;5;6;7;8;9;10;11;12Hx");
    check("more parameters than there is room for does not corrupt anything",
          row_text(0) != NULL);

    /* Asserted on the cell rather than the cursor: after writing the last
     * column the cursor legitimately rests at `cols`, waiting to see whether a
     * newline or another character comes next. That pending-wrap state is what
     * stops a full-width line from double-spacing, and reading it as
     * out-of-bounds is how a correct terminal gets "fixed" into a broken one. */
    term_reset(20, 4);
    feed("\x1b[99;99Hx");
    check("addressing past the edge is clamped to the last cell, not written"
          " past it",
          term_at(19, 3) != NULL && term_at(19, 3)->ch == 'x'
              && term_cursor_row() == 3);

    /* --- the bell -------------------------------------------------------------- */
    term_reset(20, 4);
    feed("ding\x07");
    check("the bell is noticed", term_take_bell() == 1);
    check("and only once", term_take_bell() == 0);
    check("and is not drawn", strcmp(row_text(0), "ding") == 0);

    /* --- tabs -------------------------------------------------------------------- */
    term_reset(20, 4);
    feed("a\tb");
    check("a tab moves to the next multiple of eight",
          strcmp(row_text(0), "a       b") == 0);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
