/* pspssh — the PSP front end.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What is left here is the wiring: start the screen, load the saved hosts, and
 * hand whichever one the user picked to a session. The pieces it wires together
 * each own one thing —
 *
 *     gfx      the character grid on the 480x272 panel
 *     console  a scrolling log on that grid
 *     pad      buttons, as presses rather than as held state
 *     osk      the system keyboard
 *     hosts    the saved servers, on the memory card
 *     ui       the list and the editor
 *     net      the radio, the resolver and the socket
 *     ../core  the SSH session, which knows about none of the above
 *
 * That last separation is the load-bearing one. The same core builds for a
 * laptop and is driven against a real OpenSSH by tools/test-host.sh, so a
 * protocol fault is never found on a games console with no debugger.
 */

#include "../core/pspssh.h"

#include "console.h"
#include "gfx.h"
#include "hosts.h"
#include "knownhosts.h"
#include "net.h"
#include "osk.h"
#include "pad.h"
#include "term.h"
#include "ui.h"

#include <pspctrl.h>
#include <pspkernel.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

PSP_MODULE_INFO("pspssh", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);        /* all but a megabyte; wolfSSL wants room */

/* ------------------------------------------------------------- lifecycle -- */

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    pad_set_exit_requested();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int id;

    (void)args; (void)argp;
    id = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callback_thread,
                                     0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

/* Yielding while the handshake waits, and telling it how long for.
 *
 * A millisecond is enough to let the network threads run, and short enough
 * that the handshake is not slowed by the waiting. Without this the main
 * thread spins and never gives them a chance, so the bytes it is waiting for
 * never arrive — the screen sat on "key exchange may take a moment" forever.
 *
 * The return value is the session's only clock: it adds these up to decide
 * when a handshake has taken too long. */
static int yield_briefly(void *ctx)
{
    (void)ctx;
    sceKernelDelayThread(1000);         /* microseconds */
    return 1;                           /* milliseconds */
}

/* -------------------------------------------------------------- host key -- */

/* Called during the handshake, before anything secret is sent. Returning zero
 * aborts the connection, and there is deliberately no third answer. */
static int check_hostkey(void *ctx, const char *fingerprint,
                         const unsigned char *key, unsigned int key_len)
{
    const host_entry *host = (const host_entry *)ctx;
    const char *known = knownhosts_lookup(host->address, host->port);
    char line[HOST_ADDRESS_LEN + 40];

    (void)key; (void)key_len;

    if (known != NULL) {
        if (strcmp(known, fingerprint) == 0) {
            console_printf("  host key %s (known)\n", fingerprint);
            return 1;
        }

        /* The one moment the whole scheme earns its keep. No "continue
         * anyway": on a handheld that button gets pressed reflexively, and
         * this is the only point at which the attack is visible at all. */
        console_printf("\n  THE HOST KEY CHANGED\n");
        console_printf("  expected %s\n", known);
        console_printf("  offered  %s\n", fingerprint);
        ui_message("this is not the server you connected to before",
                   "if you rebuilt it, forget its key from the list — SELECT",
                   GFX_RED);
        return 0;
    }

    /* First sight. Trusted if the user says so, and remembered either way it
     * is answered — a "no" that did not stick would ask again next time and
     * train the answer out of them. */
    console_printf("  host key %s\n", fingerprint);
    snprintf(line, sizeof(line), "%s:%d has not been seen before",
             host->address, host->port);
    if (!ui_confirm(line, fingerprint)) {
        return 0;
    }
    if (knownhosts_remember(host->address, host->port, fingerprint) != 0) {
        /* Worth continuing: the user looked at the fingerprint and accepted
         * it, so this connection is as verified as it was going to be. Only
         * the next one loses out, and saying so is better than refusing a
         * connection the user just approved. */
        console_printf("  (it could not be remembered: %s)\n",
                       knownhosts_error());
    }
    return 1;
}

/* -------------------------------------------------------------- terminal -- */

/* Overwriting through a volatile pointer, so the compiler cannot decide a
 * buffer nobody reads again does not need clearing. */
static void wipe(void *data, unsigned int len)
{
    volatile unsigned char *at = (volatile unsigned char *)data;

    while (len-- > 0) {
        *at++ = 0;
    }
}

/* The title row and the key legend take one line each, so the shell gets the
 * twenty between them. */
#define TERM_ROWS (GFX_ROWS - 2)
#define TERM_TOP  1

/* SGR's sixteen, in the order SGR numbers them. Not the saturated primaries a
 * palette generator would give: this is read on a small backlit panel, often at
 * arm's length, and pure blue text on black is close to unreadable there. */
static const unsigned int palette[16] = {
    GFX_RGB(0x00, 0x00, 0x00), GFX_RGB(0xcc, 0x44, 0x44),
    GFX_RGB(0x44, 0xbb, 0x55), GFX_RGB(0xc0, 0xa0, 0x30),
    GFX_RGB(0x50, 0x80, 0xd0), GFX_RGB(0xb0, 0x60, 0xb0),
    GFX_RGB(0x40, 0xa8, 0xa8), GFX_RGB(0xc8, 0xc8, 0xc8),
    GFX_RGB(0x60, 0x60, 0x60), GFX_RGB(0xff, 0x70, 0x70),
    GFX_RGB(0x70, 0xf0, 0x80), GFX_RGB(0xf0, 0xd0, 0x60),
    GFX_RGB(0x80, 0xb0, 0xff), GFX_RGB(0xe0, 0x90, 0xe0),
    GFX_RGB(0x70, 0xe0, 0xe0), GFX_RGB(0xff, 0xff, 0xff)
};

static void draw_terminal(const host_entry *host, int blink)
{
    int row;
    int col;

    gfx_clear(GFX_BLACK);

    gfx_fill_cells(0, 0, GFX_COLS, 1, GFX_ACCENT);
    gfx_printf(1, 0, GFX_WHITE, GFX_ACCENT, "%s@%s", host->user, host->name);
    gfx_printf(GFX_COLS - 26, 0, GFX_WHITE, GFX_ACCENT,
               "v%s  %s", PSPSSH_VERSION, PSPSSH_AUTHOR);

    for (row = 0; row < TERM_ROWS; row++) {
        for (col = 0; col < GFX_COLS; col++) {
            const term_cell *cell = term_at(col, row);
            unsigned char ch = cell != NULL ? cell->ch : ' ';
            unsigned int fg = palette[cell != NULL ? (cell->fg & 15) : 7];
            unsigned int bg = palette[cell != NULL ? (cell->bg & 15) : 0];

            /* The cursor is a block that blinks, because a still one on a
             * screen full of text is genuinely hard to find, and knowing where
             * the shell thinks you are is most of what a cursor is for. */
            if (blink && row == term_cursor_row()
                    && col == term_cursor_col()) {
                unsigned int swap = fg;

                fg = bg;
                bg = swap;
            }
            gfx_glyph(col, row + TERM_TOP, ch, fg, bg);
        }
    }

    gfx_puts(1, GFX_ROWS - 1,
             "X type  [] enter  /\\ ctrl-c  O leave  dpad arrows",
             GFX_GREY, GFX_BLACK);
}

/* Drawn behind the on-screen keyboard while it is up.
 *
 * The host editor always had one of these; the terminal passed NULL, so its
 * backdrop was black by construction — which is why a keyboard that failed to
 * appear presented as an unexplained black screen there and would have been
 * obvious anywhere else. Now the session stays visible behind the panel, which
 * is both nicer to type against and self-diagnosing. */
static void terminal_backdrop(void *ctx)
{
    draw_terminal((const host_entry *)ctx, 0);
}

/* Sends the whole of a short string, pumping until it is gone.
 *
 * pspssh_write returns 0 when the channel's send window is full, which right
 * after a channel opens is the ordinary case rather than an exceptional one.
 * A caller that wrote once and moved on would silently drop the first thing
 * anybody typed. */
static int send_all(pspssh_session *session, const char *text, int len)
{
    int sent = 0;
    int spins = 0;

    while (sent < len && spins < 2000) {
        int n = pspssh_write(session, text + sent, (unsigned int)(len - sent));

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            spins++;
            sceKernelDelayThread(1000);
            continue;
        }
        spins = 0;
        sent += n;
    }
    return sent == len ? 0 : -1;
}

static void run_terminal(pspssh_session *session, const host_entry *host)
{
    char typed[512];
    int frames = 0;
    int ended = 0;
    const char *ending = NULL;

    term_reset(GFX_COLS, TERM_ROWS);

    while (!ended && !pad_exit_requested()) {
        unsigned int pressed;
        int drained = 0;

        /* Read until the far side is quiet or a screenful has arrived, rather
         * than once per frame. At one 512-byte read per vertical blank a
         * directory listing would take seconds to appear, which reads as a slow
         * connection and is really a slow client. */
        for (;;) {
            unsigned char chunk[1024];
            int n = pspssh_read(session, chunk, sizeof(chunk));

            if (n < 0) {
                ending = pspssh_error(session);
                ended = 1;
                break;
            }
            if (n == 0) {
                break;
            }
            term_feed(chunk, n);
            drained += n;
            if (drained > 8192) {
                break;
            }
        }

        draw_terminal(host, (frames / 20) % 2 == 0);
        gfx_flip();
        frames++;

        if (ended) {
            break;
        }

        pressed = pad_pressed();
        if (pressed == 0) {
            continue;
        }

        if ((pressed & PSP_CTRL_CROSS) != 0) {
            /* The keyboard is modal, so this is line-at-a-time input. It suits
             * a shell and does not suit an editor, which the README says rather
             * than leaving people to find out. */
            typed[0] = '\0';
            if (osk_prompt("type a line for the shell", "",
                           typed, sizeof(typed), OSK_TEXT,
                           terminal_backdrop, (void *)host) == OSK_ENTERED) {
                int len = (int)strlen(typed);

                typed[len] = '\n';
                if (send_all(session, typed, len + 1) != 0) {
                    ending = "the line could not be sent";
                    ended = 1;
                }
            }
            wipe(typed, sizeof(typed));
            continue;
        }

        {
            /* What the buttons cannot spell, they send directly. Ctrl-C in
             * particular: a wedged command with no way to interrupt it would
             * mean pulling the battery. */
            const char *keys = NULL;
            int len = 1;

            if ((pressed & PSP_CTRL_SQUARE) != 0)        keys = "\r";
            else if ((pressed & PSP_CTRL_TRIANGLE) != 0) keys = "\003";
            else if ((pressed & PSP_CTRL_SELECT) != 0)   keys = "\004";
            else if ((pressed & PSP_CTRL_START) != 0)    keys = "\t";
            else if ((pressed & PSP_CTRL_LTRIGGER) != 0) keys = "\033";
            else if ((pressed & PSP_CTRL_RTRIGGER) != 0) keys = "\014";
            else if ((pressed & PSP_CTRL_UP) != 0)    { keys = "\033[A"; len = 3; }
            else if ((pressed & PSP_CTRL_DOWN) != 0)  { keys = "\033[B"; len = 3; }
            else if ((pressed & PSP_CTRL_RIGHT) != 0) { keys = "\033[C"; len = 3; }
            else if ((pressed & PSP_CTRL_LEFT) != 0)  { keys = "\033[D"; len = 3; }

            if (keys != NULL && send_all(session, keys, len) != 0) {
                ending = "the keystroke could not be sent";
                ended = 1;
            }
        }

        if ((pressed & PSP_CTRL_CIRCLE) != 0) {
            if (ui_confirm("leave this session?",
                           "the shell on the other side will be closed")) {
                return;
            }
        }
    }

    ui_message("the session ended",
               ending != NULL ? ending : "the far side closed it", GFX_GREY);
}

/* --------------------------------------------------------------- session -- */

static void run_session(const host_entry *chosen)
{
    /* A copy, because the password may be typed rather than stored, and because
     * the list underneath can be edited between one connection and the next. */
    host_entry host = *chosen;
    pspssh_config config;
    pspssh_session *session;
    char err[PSPSSH_ERROR_LEN];
    int fd;

    if (host.password[0] == '\0'
            && !ui_ask_password(&host, host.password, sizeof(host.password))) {
        wipe(&host, sizeof(host));
        return;
    }

    console_reset();
    console_printf("pspssh v%s  -  %s\n", PSPSSH_VERSION, PSPSSH_AUTHOR);
    console_printf("curve25519-sha256, ssh-ed25519, aes256-ctr\n");
    console_printf("------------------------------------------------------------\n\n");
    console_printf("  %s@%s:%d\n\n", host.user, host.address, host.port);

    if (!net_start(host.profile)) {
        wipe(&host, sizeof(host));
        ui_message("the network would not come up",
                   "the log behind this says how far it got", GFX_RED);
        return;
    }

    fd = net_connect(host.address, host.port);
    if (fd < 0) {
        wipe(&host, sizeof(host));
        ui_message("the server could not be reached",
                   "the log behind this says why", GFX_RED);
        return;
    }
    console_printf("  connected. key exchange may take a moment...\n");

    memset(&config, 0, sizeof(config));
    config.user = host.user;
    config.password = host.password;
    config.io = &fd;
    config.recv = net_recv;
    config.send = net_send;
    config.on_hostkey = check_hostkey;
    config.hostkey_ctx = &host;
    config.on_wait = yield_briefly;
    config.handshake_timeout_ms = 30000;
    /* The shape the far side is told about, which is the screen less the title
     * row and the key legend. Getting this right at the handshake rather than
     * correcting it afterwards means a full-screen program never draws its
     * first frame two lines too tall. */
    config.columns = GFX_COLS;
    config.rows = TERM_ROWS;

    session = pspssh_open(&config, err, sizeof(err));
    if (session == NULL) {
        close(fd);
        wipe(&host, sizeof(host));
        ui_message("ssh failed", err, GFX_RED);
        return;
    }

    /* Handed over; nothing below this needs it. */
    wipe(host.password, sizeof(host.password));

    run_terminal(session, &host);

    pspssh_close(session);
    close(fd);
    wipe(&host, sizeof(host));
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    if (gfx_init() != 0) {
        /* Nothing can be shown if this fails, including why it failed. Leaving
         * rather than carrying on blind is the only honest option. */
        sceKernelExitGame();
        return 0;
    }
    console_reset();
    pad_init();
    setup_callbacks();

    if (pspssh_init() != 0) {
        ui_message("the ssh library would not start", NULL, GFX_RED);
        gfx_shutdown();
        sceKernelExitGame();
        return 0;
    }

    if (knownhosts_load() != 0) {
        /* Nothing is loaded when this fails, so every server would look new and
         * every one would be trusted on sight. That is the check turning itself
         * off, and it has to be said out loud. */
        ui_message("the remembered host keys could not be read",
                   knownhosts_error(), GFX_RED);
    }

    if (hosts_load() != 0) {
        /* Nothing is loaded in this case rather than a fragment of somebody's
         * list, so saying so is the whole of the recovery. */
        ui_message("the saved hosts could not be read", hosts_error(), GFX_RED);
    }

    for (;;) {
        int chosen = ui_host_list();

        if (chosen < 0) {
            break;
        }
        run_session(hosts_at(chosen));
    }

    /* A password read off the memory card does not outlive the session that
     * used it. Cheap, and the habit matters more than this one case: homebrew
     * shares an address space with whatever plugins are loaded beside it. */
    hosts_forget_passwords();

    pspssh_cleanup();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
