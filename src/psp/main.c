/* pspssh — the PSP front end.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything PSP-specific lives here: bringing up Wi-Fi, resolving a name,
 * opening a socket, and putting bytes on the screen. The session itself is
 * src/core, which has no idea any of this exists — it is handed two function
 * pointers and asks for bytes.
 *
 * This is the same core that tools/test-host.sh drives against a real OpenSSH
 * from a laptop. If a handshake works there and fails here, the fault is in
 * this file rather than in the protocol, which is the whole reason for the
 * split: a device with no debugger is a bad place to discover a key exchange
 * bug.
 */

#include "../core/pspssh.h"
#include "console.h"
#include "gfx.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspsdk.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

PSP_MODULE_INFO("pspssh", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);        /* all but a megabyte; wolfSSL wants room */

/* Where the connection details are read from. Typing a hostname on a device
 * with no keyboard is the problem this project has not solved yet, so for now
 * it is a file next to the binary. */
#define CONFIG_PATH "pspssh.cfg"

typedef struct {
    char host[128];
    char port[8];
    char user[64];
    char password[128];
    int profile;            /* which saved Wi-Fi connection to use, 1-based */
} settings;

/* ------------------------------------------------------------- lifecycle -- */

static int exit_requested;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    exit_requested = 1;
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

static void wait_for_button(void)
{
    SceCtrlData pad;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    console_printf("\n  press X to exit\n");
    while (!exit_requested) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CROSS) {
            break;
        }
        sceKernelDelayThread(50 * 1000);
    }
}

/* ---------------------------------------------------------------- config -- */

/* A tiny key=value reader. Deliberately forgiving about whitespace and blank
 * lines, and silent about keys it does not know: a config file edited on a
 * phone or a laptop should not fail over a stray space. */
static int load_settings(settings *out)
{
    SceUID fd;
    char buf[1024];
    int len;
    char *line;
    char *save;

    memset(out, 0, sizeof(*out));
    strcpy(out->port, "22");
    out->profile = 1;

    fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return 0;
    }
    len = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (len <= 0) {
        return 0;
    }
    buf[len] = '\0';

    /* Split by hand rather than with strtok_r, which newlib does not declare
     * under -std=c99. Reaching for a feature-test macro to expose one POSIX
     * function is a worse trade than six lines that cannot surprise us. */
    line = buf;
    while (line < buf + len) {
        char *eq;
        char *key = line;
        char *value;

        save = line;
        while (*save != '\0' && *save != '\n' && *save != '\r') {
            save++;
        }
        while (*save == '\n' || *save == '\r') {
            *save++ = '\0';
        }

        eq = strchr(line, '=');
        line = save;

        if (key[0] == '#' || eq == NULL) {
            continue;
        }
        *eq = '\0';
        value = eq + 1;
        while (*key == ' ') key++;
        while (*value == ' ') value++;

        if (strcmp(key, "host") == 0) {
            strncpy(out->host, value, sizeof(out->host) - 1);
        } else if (strcmp(key, "port") == 0) {
            strncpy(out->port, value, sizeof(out->port) - 1);
        } else if (strcmp(key, "user") == 0) {
            strncpy(out->user, value, sizeof(out->user) - 1);
        } else if (strcmp(key, "password") == 0) {
            strncpy(out->password, value, sizeof(out->password) - 1);
        } else if (strcmp(key, "profile") == 0) {
            out->profile = atoi(value);
            if (out->profile < 1) {
                out->profile = 1;
            }
        }
    }
    return out->host[0] != '\0' && out->user[0] != '\0';
}

/* ------------------------------------------------------------------- net -- */

/* Which saved connections exist, and what they are called.
 *
 * Guessing a profile number was a mistake worth undoing rather than papering
 * over. Deleting a connection in the PSP's settings does not necessarily
 * renumber the ones after it, so "the only remaining connection" is not
 * reliably number 1 — which is exactly how a profile that works perfectly in
 * the browser fails here. The device knows the answer, so it gets asked. */
static int list_profiles(int *found, int max)
{
    netData data;
    int count = 0;
    int id;

    console_printf("  saved connections:\n");
    for (id = 1; id <= 10 && count < max; id++) {
        if (sceUtilityCheckNetParam(id) != 0) {
            continue;
        }
        found[count++] = id;

        memset(&data, 0, sizeof(data));
        console_printf("   %d:", id);
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &data) == 0) {
            console_printf(" %s", data.asString);
        }
        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_SSID, &data) == 0) {
            console_printf("  (ssid %s)", data.asString);
        }
        console_printf("\n");
    }
    if (count == 0) {
        console_printf("   none. create one under Settings > Network Settings.\n");
    }
    return count;
}

/* The sequence is fixed and not guessable; it comes from the PSPSDK samples.
 * The profile is one the user configured in the PSP's own network settings —
 * this picks a profile, it does not join a network. */
static int try_profile(int profile);

static int start_network(int preferred)
{
    int available[10];
    int count;
    int i;
    int err;

    sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

    err = pspSdkInetInit();
    if (err != 0) {
        console_printf("  the network stack would not start (0x%08x)\n", err);
        return 0;
    }

    count = list_profiles(available, 10);
    if (count == 0) {
        return 0;
    }
    console_printf("\n");

    /* The configured one first, then everything else. A wrong number in the
     * config should cost a few seconds, not an evening. */
    if (try_profile(preferred)) {
        return 1;
    }
    for (i = 0; i < count && !exit_requested; i++) {
        if (available[i] != preferred && try_profile(available[i])) {
            console_printf("  (put profile=%d in pspssh.cfg to go straight there)\n",
                   available[i]);
            return 1;
        }
    }
    return 0;
}

static int try_profile(int profile)
{
    int state = 0;
    int highest = 0;
    int tries = 0;
    int err;

    err = sceNetApctlConnect(profile);
    if (err != 0) {
        console_printf("  profile %d refused to start (0x%08x)\n", profile, err);
        return 0;
    }

    /* State 4 is "associated and holding an address". Anything less and a
     * socket would fail in a way that looks like the server's fault.
     *
     * The states are shown as they change rather than only at the end. Where it
     * stalls is the diagnosis: stuck at 0 means the connection never started at
     * all, which on this device almost always means there is no saved profile
     * with that number — the network existing on the router is not enough, the
     * PSP needs its own connection under Settings > Network Settings.
     *
     * Thirty seconds, not ten. A cold radio scanning 2.4 GHz and then waiting on
     * DHCP is routinely slower than a first guess suggests, and giving up early
     * reports a working network as a broken one. */
    console_printf("  joining profile %d", profile);
    while (state != 4 && tries++ < 600 && !exit_requested) {
        int previous = state;

        if (sceNetApctlGetState(&state) < 0) {
            break;
        }
        if (state != previous) {
            console_printf(" %d", state);
        }
        if (state > highest) {
            highest = state;
        }
        sceKernelDelayThread(50 * 1000);
    }
    console_printf("\n");

    if (state != 4) {
        /* The path matters more than where it stopped, and reporting only the
         * final state hid that: a connection that reached 2 and fell back to 0
         * was described as "never started", which is a different problem with a
         * different fix. What counts is the furthest it got. */
        if (highest >= 2) {
            console_printf("  profile %d was refused: it found the network, asked to\n",
                   profile);
            console_printf("  join, and was turned away (reached %d, fell back to %d)\n",
                   highest, state);
            console_printf("  usually the security type — a psp does wep or wpa-tkip,\n");
            console_printf("  and wpa2-aes only with the wpa2psp plugin loaded\n");
        } else if (highest == 1) {
            console_printf("  profile %d scanned but never found the network\n", profile);
            console_printf("  check the ssid, and that it is on 2.4 ghz\n");
        } else {
            console_printf("  profile %d never started (state %d)\n", profile, state);
            console_printf("  is there a saved connection at that number?\n");
        }
        /* Left disconnected on the way out, or the next attempt inherits a
         * half-open association and fails for a reason that is not its own. */
        sceNetApctlDisconnect();
        sceKernelDelayThread(500 * 1000);
        return 0;
    }

    {
        union SceNetApctlInfo info;
        if (sceNetApctlGetInfo(8, &info) == 0) {
            console_printf("  wi-fi up, address %s\n", info.ip);
        }
    }
    return 1;
}

static int resolve(const char *name, struct in_addr *out)
{
    unsigned char buf[1024];
    int rid;
    int ok;

    /* An address in dotted form needs no resolver, and asking for one on a
     * network with no DNS would fail for the wrong reason. */
    if (inet_aton(name, out)) {
        return 1;
    }

    if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0) {
        return 0;
    }
    ok = sceNetResolverStartNtoA(rid, name, out, 2, 3) >= 0;
    sceNetResolverDelete(rid);
    return ok;
}

static int connect_to(const settings *s)
{
    struct sockaddr_in addr;
    struct in_addr host;
    int fd;

    /* Said before the attempt, not after. connect() blocks until the kernel's
     * TCP timeout, which on an unreachable address is tens of seconds of a
     * screen that looks frozen — and a frozen screen is indistinguishable from
     * a crash to the person holding it. */
    console_printf("  looking up %s\n", s->host);
    if (!resolve(s->host, &host)) {
        console_printf("  could not resolve %s\n", s->host);
        return -1;
    }

    console_printf("  connecting to %s:%s ...\n", inet_ntoa(host), s->port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        console_printf("  no socket (%d)\n", errno);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(s->port));
    addr.sin_addr = host;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        console_printf("  %s:%s refused the connection (%d)\n", s->host, s->port, errno);
        close(fd);
        return -1;
    }

    /* The socket is left blocking on purpose, and readiness is decided by
     * select() instead — see wait_readable below. The obvious alternative,
     * setsockopt(SO_NONBLOCK), does nothing here: the PSP SDK defines
     * SO_NONBLOCK as 0, so the call compiles, runs, sets option zero, and
     * leaves the socket exactly as it was. It looked like it worked. */
    return fd;
}

/* ------------------------------------------------------------- transport -- */

/* Whether the socket has something, without committing to waiting for it.
 *
 * select() rather than a non-blocking socket, because the PSP's SO_NONBLOCK is
 * a no-op and a blocking recv on a quiet socket never returns — which on a
 * handheld is a frozen screen with no way to tell it from a crash. A short
 * timeout gives the session its "nothing yet" answer and lets everything above
 * carry on. */
static int wait_readable(int fd, int milliseconds)
{
    fd_set readable;
    struct timeval timeout;

    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;

    return select(fd + 1, &readable, NULL, NULL, &timeout) > 0;
}

static int sock_recv(void *io, void *buf, unsigned int len)
{
    int fd = *(int *)io;
    int n;

    if (!wait_readable(fd, 50)) {
        return 0;                       /* nothing yet, not a failure */
    }
    n = recv(fd, buf, len, 0);
    if (n == 0) {
        return -1;                      /* orderly close is still the end */
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return n;
}

static int sock_send(void *io, const void *buf, unsigned int len)
{
    int fd = *(int *)io;
    int n = send(fd, buf, len, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return n;
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

/* ------------------------------------------------------------------ main -- */

/* Every way out of this application is the same way out: say something, wait
 * for the button, put the screen back, leave. Writing it once means the screen
 * cannot be left un-terminated down one path out of five — which is exactly the
 * kind of thing that goes unnoticed until a PSP needs its battery pulled. */
static int quit(void)
{
    wait_for_button();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}

int main(void)
{
    settings config;
    pspssh_config session_config;
    pspssh_session *session;
    char err[PSPSSH_ERROR_LEN];
    int fd;

    if (gfx_init() != 0) {
        /* Nothing can be shown if this fails, including why it failed. Leaving
         * rather than carrying on blind is the only honest option. */
        sceKernelExitGame();
        return 0;
    }
    console_reset();
    setup_callbacks();

    /* The banner. It names the build and who made it, and then says what the
     * client actually negotiates — which is the whole reason this exists, and
     * is worth stating where someone will read it rather than only in a
     * README they will not. */
    console_printf("pspssh v%s  -  %s\n", PSPSSH_VERSION, PSPSSH_AUTHOR);
    console_printf("ssh for the psp: curve25519, ed25519, aes256-ctr\n");
    console_printf("------------------------------------------------------------\n\n");

    if (!load_settings(&config)) {
        console_printf("  no usable %s next to the binary.\n\n", CONFIG_PATH);
        console_printf("  it should look like:\n");
        console_printf("    host=192.168.1.10\n");
        console_printf("    port=22\n");
        console_printf("    user=me\n");
        console_printf("    password=secret\n");
        console_printf("    profile=1\n");
        return quit();
    }
    console_printf("  %s@%s:%s\n\n", config.user, config.host, config.port);

    if (!start_network(config.profile)) {
        return quit();
    }

    fd = connect_to(&config);
    if (fd < 0) {
        return quit();
    }
    console_printf("  connected. key exchange may take a moment...\n");

    if (pspssh_init() != 0) {
        console_printf("  the ssh library would not start\n");
        return quit();
    }

    memset(&session_config, 0, sizeof(session_config));
    session_config.user = config.user;
    session_config.password = config.password;
    session_config.io = &fd;
    session_config.recv = sock_recv;
    session_config.send = sock_send;
    session_config.on_hostkey = show_and_accept;
    session_config.on_wait = yield_briefly;
    session_config.handshake_timeout_ms = 30000;
    /* What the debug screen actually gives us until the real terminal lands. */
    session_config.columns = 60;
    session_config.rows = 34;

    session = pspssh_open(&session_config, err, sizeof(err));
    if (session == NULL) {
        console_printf("\n  ssh failed: %s\n", err);
        close(fd);
        pspssh_cleanup();
        return quit();
    }

    console_printf("  session open\n\n");

    /* Runs one command and shows what comes back. A keyboard is the next
     * problem; proving the session carries real output is this one. */
    pspssh_write(session, "uname -a; id\n", 13);

    {
        char buf[512];
        int quiet = 0;

        while (!exit_requested && quiet < 150) {
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
    }

    pspssh_close(session);
    close(fd);
    pspssh_cleanup();

    return quit();
}
