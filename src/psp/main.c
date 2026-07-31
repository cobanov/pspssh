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
#include "net.h"
#include "osk.h"
#include "pad.h"
#include "ui.h"

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

static int show_and_accept(void *ctx, const char *fingerprint,
                           const unsigned char *key, unsigned int key_len)
{
    (void)ctx; (void)key; (void)key_len;

    /* Shown rather than checked, for now. Storing it and refusing a change is
     * the next step; printing a fingerprint nobody compares would be theatre,
     * so this says plainly what it is doing. */
    console_printf("  host key %s\n", fingerprint);
    console_printf("  (not yet checked against a stored one)\n");
    return 1;
}

/* --------------------------------------------------------------- session -- */

/* Overwriting through a volatile pointer, so the compiler cannot decide a
 * buffer nobody reads again does not need clearing. */
static void wipe(void *data, unsigned int len)
{
    volatile unsigned char *at = (volatile unsigned char *)data;

    while (len-- > 0) {
        *at++ = 0;
    }
}

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
    config.on_hostkey = show_and_accept;
    config.on_wait = yield_briefly;
    config.handshake_timeout_ms = 30000;
    /* What the console gives us until the real terminal lands. */
    config.columns = GFX_COLS;
    config.rows = GFX_ROWS;

    session = pspssh_open(&config, err, sizeof(err));
    if (session == NULL) {
        close(fd);
        wipe(&host, sizeof(host));
        ui_message("ssh failed", err, GFX_RED);
        return;
    }

    /* Handed over; nothing below this needs it. */
    wipe(host.password, sizeof(host.password));

    console_printf("  session open\n\n");

    /* Runs one command and shows what comes back. Typing is the next problem;
     * proving the session carries real output is this one. */
    pspssh_write(session, "uname -a; id\n", 13);

    {
        char buf[512];
        int quiet = 0;

        while (!pad_exit_requested() && quiet < 150) {
            int n = pspssh_read(session, buf, sizeof(buf) - 1);

            if (n < 0) {
                console_printf("\n  session ended: %s\n", pspssh_error(session));
                break;
            }
            if (n == 0) {
                quiet++;
                sceKernelDelayThread(20 * 1000);
                continue;
            }
            quiet = 0;
            buf[n] = '\0';
            console_printf("%s", buf);
        }
        wipe(buf, sizeof(buf));
    }

    pspssh_close(session);
    close(fd);
    wipe(&host, sizeof(host));

    ui_message("the session ended", "what it said is behind this", GFX_GREY);
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
