/* pspssh — the boot log, tested off the device.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * console.c is what shows every message the application produces before a
 * session opens: which Wi-Fi profiles exist, how far the radio got, why a
 * socket failed. It wraps, it scrolls and it expands tabs, and none of that had
 * ever been checked.
 *
 * It draws through gfx, which needs VRAM. So gfx is stubbed here — the stub
 * records what was drawn, which makes these tests assertions about what
 * actually reaches the screen rather than about what console.c believes it
 * asked for.
 */

#include "../psp/console.h"
#include "../psp/gfx.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------- the stubbed screen -- */

static unsigned char screen[GFX_ROWS][GFX_COLS];
static int flips;

void gfx_clear(unsigned int colour)
{
    (void)colour;
    memset(screen, ' ', sizeof(screen));
}

void gfx_glyph(int col, int row, unsigned char ch,
               unsigned int fg, unsigned int bg)
{
    (void)fg; (void)bg;
    if (col >= 0 && row >= 0 && col < GFX_COLS && row < GFX_ROWS) {
        screen[row][col] = ch;
    }
}

void gfx_flip(void)
{
    flips++;
}

/* The rest of gfx is never reached from console.c, and defining it would be
 * pretending this stub is a screen. */

/* ------------------------------------------------------------------ harness -- */

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

/* A drawn row as a string, trailing blanks trimmed. */
static const char *row_text(int row)
{
    static char out[GFX_COLS + 1];
    int col;
    int last = -1;

    for (col = 0; col < GFX_COLS; col++) {
        out[col] = (char)screen[row][col];
        if (out[col] != ' ') {
            last = col;
        }
    }
    out[last + 1] = '\0';
    return out;
}

int main(void)
{
    printf("==> the boot log, without a psp\n");

    /* --- the basics ------------------------------------------------------ */
    console_reset();
    console_printf("hello");
    check("text is drawn where it was written",
          strcmp(row_text(0), "hello") == 0);
    check("and presented", flips > 0);

    console_printf(" world");
    check("the next call continues the same line",
          strcmp(row_text(0), "hello world") == 0);

    console_printf("\nsecond");
    check("a newline starts the next row",
          strcmp(row_text(1), "second") == 0);
    check("without disturbing the one above",
          strcmp(row_text(0), "hello world") == 0);

    /* --- formatting ------------------------------------------------------ */
    console_reset();
    console_printf("%s %d 0x%08x", "profile", 4, 0x8002013c);
    check("it formats like printf",
          strcmp(row_text(0), "profile 4 0x8002013c") == 0);

    /* A percent sign arriving inside data rather than as a format. Every caller
     * that prints a server's reply is one bad string away from this. */
    console_reset();
    console_printf("%s", "100% sure, and a %s in the data");
    check("data containing percent signs is not re-interpreted",
          strcmp(row_text(0), "100% sure, and a %s in the data") == 0);

    /* --- carriage return -------------------------------------------------- */
    console_reset();
    console_printf("abcdef\rXY");
    check("a carriage return returns to the left and overwrites",
          strcmp(row_text(0), "XYcdef") == 0);

    /* --- tabs -------------------------------------------------------------- */
    console_reset();
    console_printf("a\tb");
    check("a tab moves to the next multiple of eight",
          strcmp(row_text(0), "a       b") == 0);

    console_reset();
    console_printf("12345678\tx");
    check("a tab on the boundary moves a whole stop",
          strcmp(row_text(0), "12345678        x") == 0);

    /* --- wrapping ---------------------------------------------------------- */
    {
        char wide[GFX_COLS + 8];
        int i;

        for (i = 0; i < GFX_COLS + 4; i++) {
            wide[i] = 'x';
        }
        wide[GFX_COLS + 4] = '\0';

        console_reset();
        console_printf("%s", wide);
        check("a line longer than the screen wraps rather than being lost",
              (int)strlen(row_text(0)) == GFX_COLS
                  && (int)strlen(row_text(1)) == 4);
    }

    /* --- scrolling ---------------------------------------------------------- */
    {
        int i;

        console_reset();
        for (i = 0; i < GFX_ROWS + 3; i++) {
            console_printf("line %d\n", i);
        }
        /* GFX_ROWS + 3 lines were written and each ended with a newline, so
         * four scrolls have happened and the cursor rests on a blank bottom
         * row. The oldest four lines are gone. */
        char newest[16];

        snprintf(newest, sizeof(newest), "line %d", GFX_ROWS + 2);
        check("the log scrolls when it reaches the bottom",
              strcmp(row_text(0), "line 4") == 0);
        check("and the newest line is the last one drawn",
              strcmp(row_text(GFX_ROWS - 2), newest) == 0);
        check("with the row below it empty, where the cursor is",
              row_text(GFX_ROWS - 1)[0] == '\0');
    }

    /* --- reset ------------------------------------------------------------- */
    console_reset();
    console_printf("gone");
    console_reset();
    console_printf("");
    check("reset empties the screen", row_text(0)[0] == '\0');

    /* --- more than fits ------------------------------------------------------ */
    {
        /* A single call bigger than the screen. It is truncated rather than
         * overrunning anything, and the point of the test is that it returns
         * at all. */
        char huge[GFX_COLS * GFX_ROWS * 4];

        memset(huge, 'z', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';

        console_reset();
        console_printf("%s", huge);
        check("a call larger than the buffer is truncated, not fatal",
              (int)strlen(row_text(0)) == GFX_COLS);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
