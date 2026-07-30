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

typedef struct {
    const char *user;
    const char *password;   /* NULL if not using one */

    void *io;               /* passed back to recv/send */
    pspssh_recv_fn recv;
    pspssh_send_fn send;

    pspssh_hostkey_fn on_hostkey;
    void *hostkey_ctx;

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
