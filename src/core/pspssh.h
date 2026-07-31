/* pspssh — an SSH session, with no idea what it is running on.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Nothing in this header mentions a socket, a PSP, or wolfSSH. That is
 * deliberate and it is the load-bearing decision of the whole project: a
 * session is given two function pointers that move bytes, and it never learns
 * where they go.
 *
 * The payoff is that the same code runs on a laptop against a real OpenSSH
 * server and on a PSP against the same server, and only the two functions
 * differ. Encodings can be proved with test vectors; a protocol cannot — a key
 * exchange either convinces OpenSSH or it does not. So the protocol has to be
 * exercised against the real thing, and it must not need a games console to be.
 */

#ifndef PSPSSH_H
#define PSPSSH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The build's own name for itself.
 *
 * An application that cannot tell you which build it is makes "did I copy the
 * new one across?" unanswerable without a computer — which is exactly the
 * question that comes up while iterating on a device you flash by hand. */
#define PSPSSH_VERSION "1.2.2"

/* Whose it is. Shown on the first screen, because a thing somebody made by
 * hand should say so. */
#define PSPSSH_AUTHOR  "Mert Cobanov"

/* Enough for "SHA256:" and a 43-character base64 digest. */
#define PSPSSH_FINGERPRINT_LEN 64

/* Anything longer than this from a server is not a message, it is an attack. */
#define PSPSSH_ERROR_LEN 256

typedef struct pspssh_session pspssh_session;

/* Move bytes. Return the count moved, 0 for "would block", negative for a
 * fatal error. The contract matches what a BSD socket does, because that is
 * what both implementations are wrapping. */
typedef int (*pspssh_recv_fn)(void *io, void *buf, unsigned int len);
typedef int (*pspssh_send_fn)(void *io, const void *buf, unsigned int len);

/* Asked once per connection, before anything secret is sent.
 *
 * `fingerprint` is the SHA-256 base64 form, formatted exactly as
 * `ssh-keygen -lf` prints it — a fingerprint in a shape nobody can compare
 * against anything is decoration rather than security.
 *
 * Return non-zero to continue, zero to refuse. Refusing must abort the
 * connection; there is deliberately no "warn and carry on". */
typedef int (*pspssh_hostkey_fn)(void *ctx,
                                 const char *fingerprint,
                                 const unsigned char *key, unsigned int key_len);

/* Called while the session is waiting for the far side to say something, and
 * required rather than optional.
 *
 * The handshake retries until bytes arrive, and on a cooperative scheduler a
 * retry loop that never yields does not merely waste a processor — it can
 * starve the very thread that would have delivered those bytes, so the
 * connection waits forever for something it is itself preventing. On a PSP that
 * is a frozen screen. On a desktop it is only a spinning core, which is exactly
 * how the fault stays hidden during testing. Making it mandatory means nobody
 * can write a third front end that rediscovers this.
 *
 * **Return the number of milliseconds actually waited.** That return value is
 * the only clock the session has: it has no access to one of its own, and it
 * bounds the handshake by adding these up. Under-report and the handshake gives
 * up early; over-report and a hung connection is called live for longer than it
 * should be. A callback that sleeps for a millisecond returns 1. */
typedef int (*pspssh_wait_fn)(void *ctx);

typedef struct {
    const char *user;
    const char *password;   /* NULL if not using one */

    void *io;               /* passed back to recv/send */
    pspssh_recv_fn recv;
    pspssh_send_fn send;

    pspssh_hostkey_fn on_hostkey;
    void *hostkey_ctx;

    /* on_wait is required — see the note on pspssh_wait_fn for why, and for
     * what it has to return. handshake_timeout_ms of 0 means the built-in
     * default. */
    pspssh_wait_fn on_wait;
    void *wait_ctx;
    int handshake_timeout_ms;

    /* The pty the far side is told about. 0 leaves it to the server. */
    int columns;
    int rows;
} pspssh_config;

/* Runs the handshake, authenticates, and opens a shell with a pty.
 *
 * On failure returns NULL and writes a reason into `err`, which is for a
 * person to read: on a device with no console it is the only diagnosis
 * available. */
pspssh_session *pspssh_open(const pspssh_config *config,
                            char *err, size_t err_len);

/* Both return bytes moved, 0 for "nothing right now", negative on failure. */
int pspssh_read(pspssh_session *session, void *buf, unsigned int len);
int pspssh_write(pspssh_session *session, const void *buf, unsigned int len);

/* Tells the far side the terminal changed shape. Harmless if it did not. */
int pspssh_resize(pspssh_session *session, int columns, int rows);

/* Sends a disconnect if it still can, then frees everything. Safe on NULL. */
void pspssh_close(pspssh_session *session);

/* The last failure, for a caller that wants it after the fact. Never NULL. */
const char *pspssh_error(const pspssh_session *session);

/* Process-wide setup and teardown. Call once each. */
int pspssh_init(void);
void pspssh_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPSSH_H */
