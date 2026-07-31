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

#include <pspkernel.h>
#include <pspdebug.h>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

PSP_MODULE_INFO("pspssh", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);        /* all but a megabyte; wolfSSL wants room */

#define printf pspDebugScreenPrintf

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
    printf("\n  press X to exit\n");
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

    printf("  saved connections:\n");
    for (id = 1; id <= 10 && count < max; id++) {
        if (sceUtilityCheckNetParam(id) != 0) {
            continue;
        }
        found[count++] = id;

        memset(&data, 0, sizeof(data));
        printf("   %d:", id);
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &data) == 0) {
            printf(" %s", data.asString);
        }
        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_SSID, &data) == 0) {
            printf("  (ssid %s)", data.asString);
        }
        printf("\n");
    }
    if (count == 0) {
        printf("   none. create one under Settings > Network Settings.\n");
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
        printf("  the network stack would not start (0x%08x)\n", err);
        return 0;
    }

    count = list_profiles(available, 10);
    if (count == 0) {
        return 0;
    }
    printf("\n");

    /* The configured one first, then everything else. A wrong number in the
     * config should cost a few seconds, not an evening. */
    if (try_profile(preferred)) {
        return 1;
    }
    for (i = 0; i < count && !exit_requested; i++) {
        if (available[i] != preferred && try_profile(available[i])) {
            printf("  (put profile=%d in pspssh.cfg to go straight there)\n",
                   available[i]);
            return 1;
        }
    }
    return 0;
}

static int try_profile(int profile)
{
    int state = 0;
    int tries = 0;
    int err;

    err = sceNetApctlConnect(profile);
    if (err != 0) {
        printf("  profile %d refused to start (0x%08x)\n", profile, err);
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
    printf("  joining profile %d", profile);
    while (state != 4 && tries++ < 600 && !exit_requested) {
        int previous = state;

        if (sceNetApctlGetState(&state) < 0) {
            break;
        }
        if (state != previous) {
            printf(" %d", state);
        }
        sceKernelDelayThread(50 * 1000);
    }
    printf("\n");

    if (state != 4) {
        printf("  profile %d stalled at state %d\n", profile, state);
        /* Left disconnected on the way out, or the next attempt inherits a
         * half-open association and fails for a reason that is not its own. */
        sceNetApctlDisconnect();
        sceKernelDelayThread(500 * 1000);
        return 0;
    }

    {
        union SceNetApctlInfo info;
        if (sceNetApctlGetInfo(8, &info) == 0) {
            printf("  wi-fi up, address %s\n", info.ip);
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
    printf("  looking up %s\n", s->host);
    if (!resolve(s->host, &host)) {
        printf("  could not resolve %s\n", s->host);
        return -1;
    }

    printf("  connecting to %s:%s ...\n", inet_ntoa(host), s->port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("  no socket (%d)\n", errno);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(s->port));
    addr.sin_addr = host;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("  %s:%s refused the connection (%d)\n", s->host, s->port, errno);
        close(fd);
        return -1;
    }

    /* Non-blocking, because the session's callbacks report "nothing yet"
     * rather than treating a quiet socket as a dead one. */
    {
        int flag = 1;
        setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &flag, sizeof(flag));
    }
    return fd;
}

/* ------------------------------------------------------------- transport -- */

static int sock_recv(void *io, void *buf, unsigned int len)
{
    int fd = *(int *)io;
    int n = recv(fd, buf, len, 0);

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

/* -------------------------------------------------------------- host key -- */

static int show_and_accept(void *ctx, const char *fingerprint,
                           const unsigned char *key, unsigned int key_len)
{
    (void)ctx; (void)key; (void)key_len;

    /* Shown rather than checked, for now. Storing it and refusing a change is
     * the next step; printing a fingerprint nobody compares would be theatre,
     * so this says plainly what it is doing. */
    printf("  host key %s\n", fingerprint);
    printf("  (not yet checked against a stored one)\n");
    return 1;
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    settings config;
    pspssh_config session_config;
    pspssh_session *session;
    char err[PSPSSH_ERROR_LEN];
    int fd;

    pspDebugScreenInit();
    setup_callbacks();

    printf("pspssh %s\n\n", PSPSSH_VERSION);

    if (!load_settings(&config)) {
        printf("  no usable %s next to the binary.\n\n", CONFIG_PATH);
        printf("  it should look like:\n");
        printf("    host=192.168.1.10\n");
        printf("    port=22\n");
        printf("    user=me\n");
        printf("    password=secret\n");
        printf("    profile=1\n");
        wait_for_button();
        sceKernelExitGame();
        return 0;
    }
    printf("  %s@%s:%s\n\n", config.user, config.host, config.port);

    if (!start_network(config.profile)) {
        wait_for_button();
        sceKernelExitGame();
        return 0;
    }

    fd = connect_to(&config);
    if (fd < 0) {
        wait_for_button();
        sceKernelExitGame();
        return 0;
    }
    printf("  connected. key exchange may take a moment...\n");

    if (pspssh_init() != 0) {
        printf("  the ssh library would not start\n");
        wait_for_button();
        sceKernelExitGame();
        return 0;
    }

    memset(&session_config, 0, sizeof(session_config));
    session_config.user = config.user;
    session_config.password = config.password;
    session_config.io = &fd;
    session_config.recv = sock_recv;
    session_config.send = sock_send;
    session_config.on_hostkey = show_and_accept;
    /* What the debug screen actually gives us until the real terminal lands. */
    session_config.columns = 60;
    session_config.rows = 34;

    session = pspssh_open(&session_config, err, sizeof(err));
    if (session == NULL) {
        printf("\n  ssh failed: %s\n", err);
        close(fd);
        pspssh_cleanup();
        wait_for_button();
        sceKernelExitGame();
        return 0;
    }

    printf("  session open\n\n");

    /* Runs one command and shows what comes back. A keyboard is the next
     * problem; proving the session carries real output is this one. */
    pspssh_write(session, "uname -a; id\n", 13);

    {
        char buf[512];
        int quiet = 0;

        while (!exit_requested && quiet < 150) {
            int n = pspssh_read(session, buf, sizeof(buf) - 1);

            if (n < 0) {
                printf("\n  session ended: %s\n", pspssh_error(session));
                break;
            }
            if (n == 0) {
                quiet++;
                sceKernelDelayThread(20 * 1000);
                continue;
            }
            quiet = 0;
            buf[n] = '\0';
            printf("%s", buf);
        }
    }

    pspssh_close(session);
    close(fd);
    pspssh_cleanup();

    wait_for_button();
    sceKernelExitGame();
    return 0;
}
