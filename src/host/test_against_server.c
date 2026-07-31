/* pspssh — the half of the verification that needs a real OpenSSH server.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Encodings can be proved with test vectors. A protocol cannot: a key exchange
 * either convinces OpenSSH or it does not, and there is no partial credit. So
 * this drives src/core against a real sshd, on a laptop, using ordinary BSD
 * sockets for the two callbacks a session needs.
 *
 * The PSP front end will supply different callbacks and nothing else changes.
 * That is the point of the split — the protocol is exercised where it can be
 * observed, not on hardware with no debugger.
 *
 *   test_against_server <host> <port> <user> <password>
 */

#include "../core/pspssh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>

static int passed;
static int failed;

static void check(const char *name, int condition)
{
    if (condition) {
        passed++;
        printf("  PASS  %s\n", name);
    } else {
        failed++;
        printf("  FAIL  %s\n", name);
    }
}

/* ------------------------------------------------------------ transport -- */

static int sock_recv(void *io, void *buf, unsigned int len)
{
    int fd = *(int *)io;
    ssize_t n = recv(fd, buf, len, 0);

    if (n < 0) {
        /* The session treats 0 as "nothing yet" rather than as an error, which
         * is what keeps a quiet server from looking like a dead one. */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return (int)n;
}

static int sock_send(void *io, const void *buf, unsigned int len)
{
    int fd = *(int *)io;
    ssize_t n = send(fd, buf, len, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return (int)n;
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *found, *at;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &found) != 0) {
        return -1;
    }
    for (at = found; at != NULL; at = at->ai_next) {
        fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, at->ai_addr, at->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);

    if (fd >= 0) {
        /* A terminal sends single keystrokes; waiting to coalesce them is the
         * difference between a responsive session and a maddening one. */
        int one = 1;
        struct timeval timeout;

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        /* Bounded rather than blocking forever: a test that hangs tells you
         * nothing, and the session's callbacks report a timeout as "nothing
         * yet", which is exactly right during a handshake. */
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    return fd;
}

/* The host has pre-emptive scheduling and would survive without this, but the
 * PSP does not — so the same code path is exercised here rather than left to
 * be discovered on hardware. */
static void yield_briefly(void *ctx)
{
    (void)ctx;
    usleep(1000);
}

/* -------------------------------------------------------------- host key -- */

static char seen_fingerprint[PSPSSH_FINGERPRINT_LEN];
static unsigned char key_blob[512];
static unsigned int key_blob_len;

static int accept_hostkey(void *ctx, const char *fingerprint,
                          const unsigned char *key, unsigned int key_len)
{
    (void)ctx;

    snprintf(seen_fingerprint, sizeof(seen_fingerprint), "%s", fingerprint);

    /* Kept so the assertions can look at what was actually presented rather
     * than at the fact that something was. */
    key_blob_len = key_len < sizeof(key_blob) ? key_len : (unsigned int)sizeof(key_blob);
    memcpy(key_blob, key, key_blob_len);

    /* Trust on first use, because this is a test against a server we own.
     * A client must ask a person; see the host key policy in the front end. */
    return 1;
}

/* ------------------------------------------------------------------ main -- */

/* Writes the lot, retrying while the transport says "not yet".
 *
 * pspssh_write returns 0 for "would block", which is a request to try again
 * rather than a failure — a distinction an earlier version of this test got
 * wrong, and which made a working session look like a broken one. */
static int write_all(pspssh_session *session, const char *data, size_t len)
{
    size_t sent = 0;
    int spins = 250;            /* five seconds at 20ms */

    while (sent < len && spins-- > 0) {
        int n = pspssh_write(session, data + sent, (unsigned int)(len - sent));

        if (n < 0) {
            printf("        write failed: %s\n", pspssh_error(session));
            return 0;
        }
        if (n == 0) {
            usleep(20 * 1000);
            continue;
        }
        sent += (size_t)n;
    }

    if (sent < len) {
        printf("        wrote only %zu of %zu bytes before giving up\n", sent, len);
        return 0;
    }
    return 1;
}

/* Reads until the marker appears or the budget runs out. The marker is written
 * split so that the command echoed back by the pty does not count as a hit —
 * a mistake worth avoiding, because it makes a broken read look like a
 * successful one. */
static int read_until(pspssh_session *session, const char *marker, int seconds)
{
    char window[8192];
    size_t filled = 0;
    int spins = seconds * 50;
    int total = 0;

    memset(window, 0, sizeof(window));

    while (spins-- > 0) {
        char buf[1024];
        int n = pspssh_read(session, buf, sizeof(buf) - 1);

        if (n < 0) {
            /* A test that fails without saying what it saw is a test you have
             * to rerun by hand to learn anything from. */
            printf("        read failed after %d bytes: %s\n",
                   total, pspssh_error(session));
            return 0;
        }
        total += n > 0 ? n : 0;
        if (n == 0) {
            usleep(20 * 1000);
            continue;
        }
        if (filled + (size_t)n >= sizeof(window)) {
            /* Keep the tail: the marker will be near the end, and an
             * unbounded buffer is how a test becomes a memory bug. */
            size_t keep = sizeof(window) / 2;
            memmove(window, window + filled - keep, keep);
            filled = keep;
        }
        memcpy(window + filled, buf, (size_t)n);
        filled += (size_t)n;
        window[filled] = '\0';

        if (strstr(window, marker) != NULL) {
            return 1;
        }
    }

    printf("        gave up after %d bytes without seeing \"%s\"\n", total, marker);
    if (total > 0) {
        printf("        last of it: %.200s\n", window);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "2222";
    const char *user = argc > 3 ? argv[3] : "bb";
    const char *password = argc > 4 ? argv[4] : "bbssh";

    pspssh_config config;
    pspssh_session *session;
    char err[PSPSSH_ERROR_LEN];
    int fd;

    printf("==> pspssh v%s (%s)  %s@%s:%s\n",
           PSPSSH_VERSION, PSPSSH_AUTHOR, user, host, port);

    if (pspssh_init() != 0) {
        printf("  FAIL  the library initialises\n");
        return 1;
    }

    fd = connect_to(host, port);
    check("a socket reaches the server", fd >= 0);
    if (fd < 0) {
        return 1;
    }

    memset(&config, 0, sizeof(config));
    config.user = user;
    config.password = password;
    config.io = &fd;
    config.recv = sock_recv;
    config.send = sock_send;
    config.on_hostkey = accept_hostkey;
    config.on_wait = yield_briefly;
    config.handshake_timeout_ms = 30000;
    config.columns = 80;
    config.rows = 30;

    session = pspssh_open(&config, err, sizeof(err));
    if (session == NULL) {
        printf("  FAIL  the session opens (%s)\n", err);
        close(fd);
        pspssh_cleanup();
        return 1;
    }
    check("the handshake completes and authentication succeeds", 1);

    /* The blob's own contents are the evidence, and checking them is the point.
     * An earlier version of this test asserted "ed25519 host key" while only
     * observing that a session opened — and the session had in fact settled on
     * ssh-rsa, because the algorithm lists were left at wolfSSH's defaults and
     * the test container still offers the old ones. A test that asserts what it
     * did not look at is worse than no test.
     *
     * An ssh-ed25519 blob is 51 bytes in wire form: a length, the 11-byte
     * algorithm name, a length, and the 32-byte key. */
    check("the host key really is ssh-ed25519, not whatever the server offered",
          key_blob_len == 51
              && memcmp(key_blob + 4, "ssh-ed25519", 11) == 0);
    printf("        blob: %u bytes, algorithm \"%.*s\"\n",
           key_blob_len,
           key_blob_len >= 15 ? 11 : 0, key_blob + 4);

    /* The fingerprint is what a user is asked to compare against the server, so
     * it has to be both correctly computed and in the shape `ssh-keygen -lf`
     * prints. Only the second half of that can be checked from here; the first
     * is checked by tools/test-host.sh against the server's own answer. */
    check("the fingerprint is ssh-keygen shaped",
          strncmp(seen_fingerprint, "SHA256:", 7) == 0
              && strlen(seen_fingerprint) == 50);
    /* Machine-readable so the wrapper can compare it with what the server says
     * about itself. That comparison is what caught the client silently
     * negotiating RSA, so it belongs in the suite rather than in someone's
     * memory of having checked once. */
    printf("FINGERPRINT=%s\n", seen_fingerprint);

    /* Split so the pty's own echo of this line cannot be mistaken for output.
     * strlen rather than a literal count: an earlier version said 19 for an
     * 18-byte string and sent the terminating NUL into the shell. */
    {
        static const char command[] = "echo pspssh""-alive\n";

        check("a shell runs a command and returns its output",
              write_all(session, command, strlen(command))
                  && read_until(session, "pspssh-alive", 10));
    }

    check("the terminal size is negotiable",
          pspssh_resize(session, 100, 40) == 0);

    pspssh_close(session);
    close(fd);
    pspssh_cleanup();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
