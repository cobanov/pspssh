/* pspssh — the screen, drawn by us.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A character grid on the PSP's 480x272 display, and nothing more: no sprites,
 * no blending, no textures. Everything this application shows is text, so the
 * whole drawing layer is "put this glyph in that cell".
 *
 * This replaced pspDebugScreen, which was fine for bring-up and then became the
 * obstacle. Two things need what it cannot give:
 *
 *   - The on-screen keyboard renders itself into the *GU* draw buffer, and its
 *     loop is sceGuStart/Finish/Sync -> sceUtilityOskUpdate -> sceGuSwapBuffers.
 *     pspDebugScreen writes straight at sceDisplaySetFrameBuf and knows nothing
 *     about any of that.
 *   - A terminal addresses cells and redraws lines in place. A debug console
 *     only appends.
 *
 * ## Redraw everything, every frame
 *
 * The display is double buffered, so the buffer being drawn into is the one
 * that was on screen two frames ago, not the one just presented. Anything
 * incremental would flicker between two half-finished pictures. Callers
 * therefore paint a whole frame and then flip; at 2040 cells this costs about a
 * millisecond and removes an entire class of bug.
 */

#ifndef PSPSSH_GFX_H
#define PSPSSH_GFX_H

/* An 8x8 font on a 480x272 screen. 80 columns would need a six-pixel font and
 * is a separate question; this is what the debug screen gave us, so moving to
 * it regresses nothing. */
#define GFX_CELL_W 8
#define GFX_CELL_H 8
#define GFX_COLS   60
#define GFX_ROWS   34

/* The framebuffer is GU_PSM_8888, which is ABGR in memory — so the literals
 * read backwards from the usual RGB and are worth constructing rather than
 * typing. */
#define GFX_RGB(r, g, b) \
    ((unsigned int)0xff000000u | ((unsigned int)(b) << 16) \
     | ((unsigned int)(g) << 8) | (unsigned int)(r))

#define GFX_BLACK   GFX_RGB(0x00, 0x00, 0x00)
#define GFX_WHITE   GFX_RGB(0xe8, 0xe8, 0xe8)
#define GFX_GREY    GFX_RGB(0x80, 0x80, 0x80)
#define GFX_DIM     GFX_RGB(0x50, 0x50, 0x50)
#define GFX_RED     GFX_RGB(0xe0, 0x40, 0x40)
#define GFX_GREEN   GFX_RGB(0x40, 0xd0, 0x60)
#define GFX_YELLOW  GFX_RGB(0xe0, 0xc0, 0x40)
#define GFX_BLUE    GFX_RGB(0x60, 0x90, 0xe0)
#define GFX_CYAN    GFX_RGB(0x40, 0xc0, 0xc0)
#define GFX_MAGENTA GFX_RGB(0xc0, 0x60, 0xc0)

/* The accent used for a selected row and for the title bar. */
#define GFX_ACCENT  GFX_RGB(0x1e, 0x4a, 0x7a)

/* Brings up the GU and the display. Returns 0 on success. */
int gfx_init(void);

/* Puts the GU back and blanks the screen. */
void gfx_shutdown(void);

/* Fills the whole draw buffer. */
void gfx_clear(unsigned int colour);

/* A run of cells in one colour, for highlight bars and rules. */
void gfx_fill_cells(int col, int row, int cols, int rows, unsigned int colour);

/* One glyph. Characters outside 0x20..0x7e are drawn as a blank rather than as
 * whatever happens to sit at that offset in the font. */
void gfx_glyph(int col, int row, unsigned char ch,
               unsigned int fg, unsigned int bg);

/* Text from `col`, clipped at the right edge rather than wrapped: a caller that
 * overruns wants to know it laid out badly, not to have its next line eaten. */
void gfx_puts(int col, int row, const char *text,
              unsigned int fg, unsigned int bg);

/* As gfx_puts, formatted. Clipped to one row. */
void gfx_printf(int col, int row, unsigned int fg, unsigned int bg,
                const char *fmt, ...);

/* Waits for the vertical blank and presents what was drawn. */
void gfx_flip(void);

#endif /* PSPSSH_GFX_H */
