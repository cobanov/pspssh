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
    while (state != 4 && tries++ < 600 && !pad_exit_requested()) {
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
            console_printf("  profile %d was refused: it found the network, asked\n",
                           profile);
            console_printf("  to join, and was turned away (reached %d, fell to %d)\n",
                           highest, state);
            console_printf("  usually the security type — a psp does wep or\n");
            console_printf("  wpa-tkip, and wpa2-aes only with wpa2psp loaded\n");
        } else if (highest == 1) {
            console_printf("  profile %d scanned but never found the network\n",
                           profile);
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

int net_start(int preferred)
{
    int available[10];
    int count;
    int i;

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
        return 0;
    }
    console_printf("\n");

    /* The configured one first, then everything else. A wrong number in a saved
     * host should cost a few seconds, not an evening. */
    if (preferred > 0 && try_profile(preferred)) {
        connected = 1;
        return 1;
    }
    for (i = 0; i < count && !pad_exit_requested(); i++) {
        if (available[i] == preferred) {
            continue;
        }
        if (try_profile(available[i])) {
            connected = 1;
            if (preferred > 0) {
                console_printf("  (set this host's profile to %d to go straight"
                               " there)\n", available[i]);
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
