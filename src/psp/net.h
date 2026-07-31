/* pspssh — the radio and the socket.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Joining a saved Wi-Fi connection, resolving a name, and opening a TCP socket.
 * Split out of main.c when the host list arrived, because "bring the network
 * up" stopped being something that happens once on the way to the only server
 * and became something a menu asks for.
 */

#ifndef PSPSSH_NET_H
#define PSPSSH_NET_H

/* Brings up the stack and joins a connection.
 *
 * `preferred` is the profile number to try first, or 0 for none — the rest are
 * tried in turn either way, because a number that was right yesterday is not
 * necessarily right after a connection is deleted in the PSP's settings.
 *
 * Progress goes to the console as it happens, since this can take half a minute
 * and a still screen is indistinguishable from a crash.
 *
 * Returns 1 once there is an address, 0 otherwise. Does nothing and succeeds if
 * the network is already up. */
int net_start(int preferred);

/* Resolves and connects. Returns a socket, or -1 with the reason on the
 * console. */
int net_connect(const char *host, int port);

/* The two callbacks the session core is given. `io` is a pointer to the socket.
 *
 * The socket is left blocking and readiness is decided with select(). The
 * obvious alternative, setsockopt(SO_NONBLOCK), does nothing on this platform:
 * the SDK defines SO_NONBLOCK as 0, so the call compiles, runs, sets option
 * zero and leaves the socket exactly as it was. It looked like it worked, and a
 * blocking recv on a quiet socket never returns — a frozen screen with no way
 * to tell it from a crash. */
int net_recv(void *io, void *buf, unsigned int len);
int net_send(void *io, const void *buf, unsigned int len);

#endif /* PSPSSH_NET_H */
