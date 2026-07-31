/* pspssh — the radio and the socket.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See net.h.
 */

#include "net.h"
#include "console.h"
#include "gfx.h"
#include "pad.h"

#include <pspkernel.h>
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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int stack_started;
static int connected;
/* The saved connections, by number and by name.
 *
 * Guessing a profile number was a mistake worth undoing rather than papering
 * over. Deleting a connection in the PSP's settings does not necessarily
 * renumber the ones after it, so "the only remaining connection" is not
 * reliably number 1 — which is exactly how a profile that works perfectly in
 * the browser fails here. The device knows the answer, so it gets asked. */
typedef struct {
    int id;
    char name[64];
} saved_connection;

static int list_profiles(saved_connection *found, int max)
{
    netData data;
    int count = 0;
    int id;

    for (id = 1; id <= 10 && count < max; id++) {
        if (sceUtilityCheckNetParam(id) != 0) {
            continue;
        }

        found[count].id = id;
        found[count].name[0] = '\0';

        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &data) == 0) {
            snprintf(found[count].name, sizeof(found[count].name), "%s",
                     data.asString);
        }
        if (found[count].name[0] == '\0') {
            /* A connection saved without a name still has to be nameable, or
             * the log says "trying" and nothing else. */
            snprintf(found[count].name, sizeof(found[count].name),
                     "connection %d", id);
        }
        count++;
    }
    return count;
}

/* What the radio is doing, in words.
 *
 * These used to be printed as the numbers the SDK returns — "joining profile 1
 * 2 3 6 4" — which is a debugging aid that became the interface. The numbers
 * are meaningless to anyone who has not read pspnet_apctl.h, and a row of them
 * reads as the application flailing rather than as five things happening in
 * order. */
static const char *stage_name(int state)
{
    switch (state) {
    case 0: return "disconnected";
    case 1: return "scanning";
    case 2: return "joining";
    case 3: return "asking for an address";
    case 4: return "connected";
    case 5: return "authenticating";
    case 6: return "exchanging keys";
    default: return "working";
    }
}

static int try_profile(const saved_connection *profile)
{
    int state = 0;
    int highest = 0;
    int tries = 0;
    int err;

    console_printf("  %s\n", profile->name);

    err = sceNetApctlConnect(profile->id);
    if (err != 0) {
        console_printf("    would not start (0x%08x)\n", err);
        return 0;
    }

    /* State 4 is "associated and holding an address". Anything less and a
     * socket would fail in a way that looks like the server's fault.
     *
     * The stages are named as they change rather than counted at the end,
     * because where it stops is the diagnosis and a person should not have to
     * hold a table of state numbers to read it.
     *
     * Thirty seconds, not ten. A cold radio scanning 2.4 GHz and then waiting on
     * DHCP is routinely slower than a first guess suggests, and giving up early
     * reports a working network as a broken one. */
    console_printf("   ");
    while (state != 4 && tries++ < 600 && !pad_exit_requested()) {
        int previous = state;

        if (sceNetApctlGetState(&state) < 0) {
            break;
        }
        if (state != previous && state != 0) {
            console_printf(" %s", stage_name(state));
        }
        if (state > highest) {
            highest = state;
        }
        sceKernelDelayThread(50 * 1000);
    }

    if (state == 4) {
        union SceNetApctlInfo info;

        if (sceNetApctlGetInfo(8, &info) == 0) {
            console_printf(", address %s\n", info.ip);
        } else {
            console_printf(", connected\n");
        }
        return 1;
    }

    /* The path matters more than where it stopped, and reporting only the
     * final state hid that: a connection that reached "joining" and fell back
     * to nothing was described as "never started", which is a different
     * problem with a different fix. What counts is the furthest it got. */
    if (highest >= 2) {
        console_printf(", turned away\n");
        console_printf("    it found the network and was refused. usually the\n");
        console_printf("    security type: a psp does wep and wpa-tkip, and\n");
        console_printf("    wpa2-aes only with the wpa2psp plugin loaded.\n");
    } else if (highest == 1) {
        console_printf(", not found\n");
        console_printf("    it scanned and never saw that network. check the\n");
        console_printf("    ssid, and that it is on 2.4 ghz.\n");
    } else {
        console_printf(" nothing happened\n");
        console_printf("    the radio never started. is the wlan switch on?\n");
    }

    /* Left disconnected on the way out, or the next attempt inherits a
     * half-open association and fails for a reason that is not its own. */
    sceNetApctlDisconnect();
    sceKernelDelayThread(500 * 1000);
    return 0;
}

int net_start(int preferred)
{
    saved_connection available[10];
    int count;
    int i;
    int chosen = -1;

    if (connected) {
        return 1;
    }

    if (!stack_started) {
        int err;

        sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
        sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

        err = pspSdkInetInit();
        if (err != 0) {
            console_printf("  the network stack would not start (0x%08x)\n", err);
            return 0;
        }
        stack_started = 1;
    }

    count = list_profiles(available, 10);
    if (count == 0) {
        console_printf("  no saved wi-fi connections.\n");
        console_printf("  create one under Settings > Network Settings first —\n");
        console_printf("  this picks a saved connection, it does not join a\n");
        console_printf("  network by itself.\n");
        return 0;
    }

    /* The one the host asked for, if it is still there. A saved number can
     * outlive the connection it named, because deleting one in the PSP's
     * settings does not renumber the rest. */
    for (i = 0; i < count; i++) {
        if (available[i].id == preferred) {
            chosen = i;
        }
    }

    if (preferred > 0 && chosen < 0) {
        console_printf("  this host asks for wi-fi profile %d, which is not\n",
                       preferred);
        console_printf("  saved on this console any more. trying the %d that\n",
                       count);
        console_printf("  %s.\n\n", count == 1 ? "is" : "are");
    } else if (preferred == 0) {
        console_printf("  no wi-fi profile set for this host, so each saved\n");
        console_printf("  connection is tried in turn.\n\n");
    }

    if (chosen >= 0 && try_profile(&available[chosen])) {
        connected = 1;
        return 1;
    }

    for (i = 0; i < count && !pad_exit_requested(); i++) {
        if (i == chosen) {
            continue;               /* already tried, and it did not work */
        }
        if (try_profile(&available[i])) {
            connected = 1;
            if (preferred != available[i].id) {
                console_printf("\n  set this host's wi-fi profile to %d to come\n",
                               available[i].id);
                console_printf("  straight here next time.\n");
            }
            return 1;
        }
    }
    return 0;
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
    /* Two seconds a try, three tries. A name that does not resolve should be
     * reported in a few seconds; the socket below is what costs a minute. */
    ok = sceNetResolverStartNtoA(rid, name, out, 2, 3) >= 0;
    sceNetResolverDelete(rid);
    return ok;
}

int net_connect(const char *host, int port)
{
    struct sockaddr_in addr;
    struct in_addr resolved;
    int fd;

    /* Said before the attempt, not after. connect() blocks until the kernel's
     * TCP timeout, which on an unreachable address is tens of seconds of a
     * screen that looks frozen — and a frozen screen is indistinguishable from
     * a crash to the person holding it. */
    console_printf("  looking up %s\n", host);
    if (!resolve(host, &resolved)) {
        console_printf("  could not resolve %s\n", host);
        return -1;
    }

    console_printf("  connecting to %s:%d ...\n", inet_ntoa(resolved), port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        console_printf("  no socket (%d)\n", errno);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr = resolved;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        console_printf("  %s:%d refused the connection (%d)\n", host, port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* Whether the socket has something, without committing to waiting for it. See
 * the note in net.h on why this is select() and not a non-blocking socket. */
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

int net_recv(void *io, void *buf, unsigned int len)
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

int net_send(void *io, const void *buf, unsigned int len)
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
