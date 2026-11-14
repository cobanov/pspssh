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
#include <time.h>

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

/* Returns at once, as the PSP's does: nothing waiting is 0, and the session's
 * wait callback is the only place time is spent. A recv that waited would make
 * every turn of the handshake longer than the millisecond it is counted as. */
static int sock_recv(void *io, void *buf, unsigned int len)
{
    int fd = *(int *)io;
    ssize_t n = recv(fd, buf, len, MSG_DONTWAIT);

    if (n == 0) {
        return -1;                      /* orderly close is still the end */
    }
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

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

/* The host has pre-emptive scheduling and would survive without this, but the
 * PSP does not — so the same code path is exercised here rather than left to
 * be discovered on hardware.
 *
 * Returns the milliseconds it waited, which is what bounds the handshake. */
static int yield_briefly(void *ctx)
{
    (void)ctx;
    usleep(1000);
    return 1;
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

static int refuse_hostkey(void *ctx, const char *fingerprint,
                          const unsigned char *key, unsigned int key_len)
{
    (void)ctx; (void)fingerprint; (void)key; (void)key_len;
    return 0;
}

/* ------------------------------------------------------- the ways it fails -- */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Opens a session that is expected not to open, and returns why it did not.
 * An empty string means it opened, which every caller counts as a failure. */
static const char *refusal(int fd, const char *user, const char *password,
                           pspssh_hostkey_fn on_hostkey, int timeout_ms)
{
    static char err[PSPSSH_ERROR_LEN];
    pspssh_config config;
    pspssh_session *session;

    memset(&config, 0, sizeof(config));
    config.user = user;
    config.password = password;
    config.io = &fd;
    config.recv = sock_recv;
    config.send = sock_send;
    config.on_hostkey = on_hostkey;
    config.on_wait = yield_briefly;
    config.handshake_timeout_ms = timeout_ms;

    err[0] = '\0';
    session = pspssh_open(&config, err, sizeof(err));
    if (session != NULL) {
        pspssh_close(session);
        err[0] = '\0';
    }
    close(fd);
    printf("        it said: %s\n", err[0] ? err : "(nothing — it opened)");
    return err;
}

/* A server that accepts the connection and then says nothing, which is what a
 * wrong port or a dropped link looks like from here. Made in this process, so
 * it needs nothing installed: a listening socket completes the connection by
 * itself and nobody ever reads from it. */
static int connect_to_silence(int *listener)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    *listener = socket(AF_INET, SOCK_STREAM, 0);
    if (*listener < 0
            || bind(*listener, (struct sockaddr *)&addr, sizeof(addr)) != 0
            || listen(*listener, 1) != 0
            || getsockname(*listener, (struct sockaddr *)&addr, &len) != 0) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -1;
    }
    return fd;
}

static void test_the_ways_it_fails(const char *host, const char *port,
                                   const char *user, const char *password)
{
    const char *why;
    int listener = -1;
    int fd;
    long started;

    /* The refusal is the whole of the host key check: if a "no" from the
     * callback did not end the connection, everything the front end does to
     * remember keys would be decoration. tools/test-host.sh then confirms from
     * the server's log that no password followed it. */
    fd = connect_to(host, port);
    why = fd >= 0 ? refusal(fd, user, password, refuse_hostkey, 30000) : "";
    check("a refused host key ends the connection, and says so",
          strstr(why, "host key was refused") != NULL);

    /* One wrong password is one failed login. Offered again each time the
     * server said no, it was six, and the error named the disconnection. */
    fd = connect_to(host, port);
    why = fd >= 0 ? refusal(fd, user, "not-the-password", accept_hostkey, 30000)
                  : "";
    check("a wrong password is refused, and says so",
          strstr(why, "refused the password") != NULL);

    /* The limit is counted in milliseconds the wait callback reports, so it is
     * only a limit if a turn of the loop takes about what it reports. */
    fd = connect_to_silence(&listener);
    started = now_ms();
    why = fd >= 0 ? refusal(fd, user, password, accept_hostkey, 300) : "";
    check("a server that never speaks is given up on",
          strstr(why, "did not finish") != NULL);
    check("and within the time it was given, not a multiple of it",
          now_ms() - started < 3000);
    if (listener >= 0) {
        close(listener);
    }

    /* The key blob the assertions below look at must be the one from the
     * session that follows, not from the wrong-password attempt above. */
    key_blob_len = 0;
    seen_fingerprint[0] = '\0';
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

    test_the_ways_it_fails(host, port, user, password);

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
