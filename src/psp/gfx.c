/* pspssh — the screen, drawn by us.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See gfx.h for what this is and why it replaced pspDebugScreen.
 */

#include "gfx.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../third_party/font8x8_basic.h"

#define SCR_WIDTH  480
#define SCR_HEIGHT 272

/* The GE wants a power-of-two stride, so each row is 512 pixels of which 480
 * are visible. Every address calculation has to use this and not SCR_WIDTH —
 * a skewed diagonal picture is what happens when one of them does not. */
#define BUF_WIDTH  512

#define FRAME_PIXELS (BUF_WIDTH * SCR_HEIGHT)
#define FRAME_BYTES  (FRAME_PIXELS * 4)

/* VRAM through the uncached mirror. Writing here means the GE sees the pixels
 * without a cache writeback, and a forgotten writeback is a bug that shows up
 * as half a frame updating — occasionally, and never while being watched. */
#define VRAM_UNCACHED 0x44000000u

/* The GE command list. Static and aligned because the hardware reads it
 * directly. */
static unsigned int __attribute__((aligned(16))) display_list[16 * 1024];

static unsigned int *draw_buffer;
static int drawing_second;
static int started;

static unsigned int *buffer_for(int second)
{
    return (unsigned int *)(VRAM_UNCACHED + (second ? (unsigned int)FRAME_BYTES : 0u));
}

int gfx_init(void)
{
    if (started) {
        return 0;
    }

    sceGuInit();

    sceGuStart(GU_DIRECT, display_list);
    /* Draw into the first buffer, show the second. sceGuSwapBuffers exchanges
     * them, and drawing_second tracks which is ours. */
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)FRAME_BYTES, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    /* A depth buffer, even though nothing here submits geometry that needs one.
     *
     * It is for the system dialogs. sceUtilityOskUpdate draws through the same
     * GE and sets its own render state, and the SDK's samples all configure a
     * Z buffer before calling one. Leave sceGuDepthBuffer unset and GU's idea
     * of where the depth buffer lives is offset zero — which is this
     * application's first framebuffer. A dialog that enabled depth testing
     * would then write depth values over the picture.
     *
     * Whether the OSK actually does that is not something this project can
     * check without hardware, and 272 KB of otherwise unused VRAM is a cheap
     * way not to need the answer. */
    sceGuDepthBuffer((void *)(FRAME_BYTES * 2), BUF_WIDTH);
    sceGuDepthRange(0xc350, 0x2710);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    drawing_second = 0;
    draw_buffer = buffer_for(drawing_second);
    started = 1;

    /* Both buffers start as whatever the previous application left in VRAM, and
     * one of them is already on screen. Clearing both means the first frame
     * cannot briefly show somebody else's game. */
    gfx_clear(GFX_BLACK);
    gfx_flip();
    gfx_clear(GFX_BLACK);

    return 0;
}

void gfx_shutdown(void)
{
    if (!started) {
        return;
    }
    gfx_clear(GFX_BLACK);
    gfx_flip();
    sceGuTerm();
    started = 0;
}

void gfx_clear(unsigned int colour)
{
    int i;

    if (!started) {
        return;
    }
    /* A loop rather than sceGuClear: this module never opens a GE list of its
     * own outside init, and a clear that has to be bracketed by
     * sceGuStart/Finish/Sync would be slower than writing the pixels. */
    for (i = 0; i < FRAME_PIXELS; i++) {
        draw_buffer[i] = colour;
    }
}

void gfx_fill_cells(int col, int row, int cols, int rows, unsigned int colour)
{
    int x, y;
    int x0, y0, x1, y1;

    if (!started) {
        return;
    }

    /* Clamped rather than trusted. Every caller computes these from a list
     * index or a string length, and one off-screen row would be a write past
     * the framebuffer into whatever VRAM holds next. */
    x0 = col * GFX_CELL_W;
    y0 = row * GFX_CELL_H;
    x1 = x0 + cols * GFX_CELL_W;
    y1 = y0 + rows * GFX_CELL_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_WIDTH) x1 = SCR_WIDTH;
    if (y1 > SCR_HEIGHT) y1 = SCR_HEIGHT;

    for (y = y0; y < y1; y++) {
        unsigned int *line = draw_buffer + (unsigned int)y * BUF_WIDTH;
        for (x = x0; x < x1; x++) {
            line[x] = colour;
        }
    }
}

void gfx_glyph(int col, int row, unsigned char ch,
               unsigned int fg, unsigned int bg)
{
    const char *glyph;
    int y;

    if (!started || col < 0 || row < 0 || col >= GFX_COLS || row >= GFX_ROWS) {
        return;
    }

    /* Anything outside printable ASCII is drawn as a space. The font only
     * covers 0x00..0x7f, and the control-code end of it is a set of shapes
     * nobody meant to display — a stray 0x07 should not put a smiley in the
     * middle of a hostname. */
    if (ch < 0x20 || ch > 0x7e) {
        ch = ' ';
    }
    glyph = font8x8_basic[ch];

    for (y = 0; y < GFX_CELL_H; y++) {
        unsigned int *line = draw_buffer
                             + (unsigned int)(row * GFX_CELL_H + y) * BUF_WIDTH
                             + (unsigned int)(col * GFX_CELL_W);
        unsigned char bits = (unsigned char)glyph[y];
        int x;

        /* Bit 0 is the leftmost pixel in this font — verified against the
         * glyph for 'F', which is mirrored the other way round. */
        for (x = 0; x < GFX_CELL_W; x++) {
            line[x] = (bits & (1u << x)) ? fg : bg;
        }
    }
}

void gfx_puts(int col, int row, const char *text,
              unsigned int fg, unsigned int bg)
{
    int at = col;

    if (text == NULL) {
        return;
    }
    while (*text != '\0' && at < GFX_COLS) {
        gfx_glyph(at++, row, (unsigned char)*text++, fg, bg);
    }
}

void gfx_printf(int col, int row, unsigned int fg, unsigned int bg,
                const char *fmt, ...)
{
    /* One row's worth, plus room to notice an overrun before it is clipped. */
    char line[GFX_COLS + 2];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    gfx_puts(col, row, line, fg, bg);
}

void gfx_flip(void)
{
    if (!started) {
        return;
    }
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    drawing_second = !drawing_second;
    draw_buffer = buffer_for(drawing_second);
}
