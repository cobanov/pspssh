/* pspssh — the session, implemented over wolfSSH.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wolfSSH is confined to this file. Everything above it sees pspssh.h, which
 * mentions neither wolfSSH nor sockets — so the library stays replaceable and
 * the front ends stay portable.
 *
 * Built with WOLFSSH_USER_IO, which excludes wolfSSH's own socket layer and
 * routes I/O through callbacks instead. That is not a workaround for the PSP
 * lacking <sys/socket.h>, though it is also that: it is what makes a session
 * indifferent to its transport.
 */

#include "pspssh.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/random.h>

struct pspssh_session {
    WOLFSSH_CTX *ctx;
    WOLFSSH *ssh;

    void *io;
    pspssh_recv_fn recv;
    pspssh_send_fn send;

    pspssh_hostkey_fn on_hostkey;
    void *hostkey_ctx;

    const char *password;

    /* Set when the host key was refused, so a generic wolfSSH failure can be
     * reported as the specific thing that actually happened. */
    int hostkey_refused;

    char error[PSPSSH_ERROR_LEN];
};

static void set_error(pspssh_session *s, const char *message)
{
    if (s == NULL) {
        return;
    }
    strncpy(s->error, message, sizeof(s->error) - 1);
    s->error[sizeof(s->error) - 1] = '\0';
}

/* ------------------------------------------------------------------ I/O --
 *
 * wolfSSH hands us its own buffer and expects socket semantics back. The
 * WS_CBIO_ERR_* values are how it is told "not an error, just nothing yet",
 * which is what keeps a non-blocking transport from looking like a dead one.
 */

static int io_recv(WOLFSSH *ssh, void *buf, word32 len, void *ctx)
{
    pspssh_session *s = (pspssh_session *)ctx;
    int n;

    (void)ssh;
    n = s->recv(s->io, buf, (unsigned int)len);
    if (n == 0) {
        return WS_CBIO_ERR_WANT_READ;
    }
    if (n < 0) {
        return WS_CBIO_ERR_GENERAL;
    }
    return n;
}

static int io_send(WOLFSSH *ssh, void *buf, word32 len, void *ctx)
{
    pspssh_session *s = (pspssh_session *)ctx;
    int n;

    (void)ssh;
    n = s->send(s->io, buf, (unsigned int)len);
    if (n == 0) {
        return WS_CBIO_ERR_WANT_WRITE;
    }
    if (n < 0) {
        return WS_CBIO_ERR_GENERAL;
    }
    return n;
}

/* -------------------------------------------------------------- host key --
 *
 * The blob wolfSSH passes is the server's public key exactly as it arrived.
 * We hash it and format the digest the way `ssh-keygen -lf` does, because a
 * fingerprint the user cannot compare against the server is not a check, it is
 * a ritual.
 */

/* Base64, unpadded, written here rather than taken from wolfSSL.
 *
 * wolfSSL has one, behind WOLFSSL_BASE64_ENCODE, which is off unless a
 * configure flag turns it on. Depending on it would mean one more flag that has
 * to stay identical between the host build and the PSP build, and a mismatch
 * there is exactly the class of difference that makes a host test stop being
 * evidence about the device. Twenty lines with no security subtlety is the
 * cheaper side of that trade. */
static void base64_unpadded(const unsigned char *in, size_t in_len,
                            char *out, size_t out_len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t at = 0;

    while (i < in_len && at + 4 < out_len) {
        unsigned long triple = (unsigned long)in[i] << 16;
        int have = 1;

        if (i + 1 < in_len) { triple |= (unsigned long)in[i + 1] << 8; have++; }
        if (i + 2 < in_len) { triple |= (unsigned long)in[i + 2];       have++; }

        out[at++] = alphabet[(triple >> 18) & 0x3f];
        out[at++] = alphabet[(triple >> 12) & 0x3f];
        if (have > 1) {
            out[at++] = alphabet[(triple >> 6) & 0x3f];
        }
        if (have > 2) {
            out[at++] = alphabet[triple & 0x3f];
        }
        i += 3;
    }
    out[at] = '\0';
}

static int fingerprint_of(const unsigned char *key, word32 key_len,
                          char *out, size_t out_len)
{
    unsigned char digest[WC_SHA256_DIGEST_SIZE];
    char encoded[48];
    wc_Sha256 sha;
    int ret;

    if (out_len < PSPSSH_FINGERPRINT_LEN) {
        return -1;
    }

    ret = wc_InitSha256(&sha);
    if (ret != 0) {
        return -1;
    }
    ret = wc_Sha256Update(&sha, key, key_len);
    if (ret == 0) {
        ret = wc_Sha256Final(&sha, digest);
    }
    wc_Sha256Free(&sha);
    if (ret != 0) {
        return -1;
    }

    /* Unpadded, because that is how `ssh-keygen -lf` prints it and the whole
     * value of a fingerprint is that the two can be compared by eye. */
    base64_unpadded(digest, sizeof(digest), encoded, sizeof(encoded));
    snprintf(out, out_len, "SHA256:%s", encoded);
    return 0;
}

static int hostkey_check(const byte *key, word32 key_len, void *ctx)
{
    pspssh_session *s = (pspssh_session *)ctx;
    char fingerprint[PSPSSH_FINGERPRINT_LEN];

    if (s->on_hostkey == NULL) {
        /* No policy supplied is not the same as "accept anything". A caller
         * that forgot to decide must not silently get trust on first sight. */
        s->hostkey_refused = 1;
        set_error(s, "no host key policy was supplied, so the key was refused");
        return WS_PUBKEY_REJECTED_E;
    }

    if (fingerprint_of(key, key_len, fingerprint, sizeof(fingerprint)) != 0) {
        s->hostkey_refused = 1;
        set_error(s, "the server's host key could not be fingerprinted");
        return WS_PUBKEY_REJECTED_E;
    }

    if (!s->on_hostkey(s->hostkey_ctx, fingerprint, key, key_len)) {
        s->hostkey_refused = 1;
        set_error(s, "the host key was refused");
        return WS_PUBKEY_REJECTED_E;
    }

    return WS_SUCCESS;
}

/* ------------------------------------------------------------------ auth -- */

static int user_auth(byte type, WS_UserAuthData *data, void *ctx)
{
    pspssh_session *s = (pspssh_session *)ctx;

    if (type == WOLFSSH_USERAUTH_PASSWORD) {
        if (s->password == NULL) {
            return WOLFSSH_USERAUTH_FAILURE;
        }
        data->sf.password.password = (byte *)s->password;
        data->sf.password.passwordSz = (word32)strlen(s->password);
        return WOLFSSH_USERAUTH_SUCCESS;
    }

    /* Public key comes later; saying so plainly beats a silent failure. */
    return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

/* ------------------------------------------------------------- lifecycle -- */

int pspssh_init(void)
{
    return wolfSSH_Init() == WS_SUCCESS ? 0 : -1;
}

void pspssh_cleanup(void)
{
    wolfSSH_Cleanup();
}

pspssh_session *pspssh_open(const pspssh_config *config, char *err, size_t err_len)
{
    pspssh_session *s;
    int ret;

    if (config == NULL || config->recv == NULL || config->send == NULL
            || config->user == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "a user and both transport callbacks are required");
        }
        return NULL;
    }

    s = (pspssh_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "out of memory");
        }
        return NULL;
    }

    s->io = config->io;
    s->recv = config->recv;
    s->send = config->send;
    s->on_hostkey = config->on_hostkey;
    s->hostkey_ctx = config->hostkey_ctx;
    s->password = config->password;

    s->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (s->ctx == NULL) {
        set_error(s, "wolfSSH context could not be created");
        goto failed;
    }

    wolfSSH_SetIORecv(s->ctx, io_recv);
    wolfSSH_SetIOSend(s->ctx, io_send);
    wolfSSH_SetUserAuth(s->ctx, user_auth);
    wolfSSH_CTX_SetPublicKeyCheck(s->ctx, hostkey_check);

    /* One algorithm per slot, and this is the whole reason the project exists.
     *
     * wolfSSH's defaults are broad and prefer whatever the server offers first.
     * Against a server that still enables the old algorithms — which the test
     * container deliberately does — that quietly settles on ssh-rsa, and the
     * client is then no better than the 2008 one it replaces. It was doing
     * exactly that until the fingerprint was compared against the server's own
     * and did not match.
     *
     * Pinning is stronger than checking afterwards: with a single name in each
     * list, a weak session cannot be negotiated at all. If a server has nothing
     * modern to offer the connection fails, which is the correct outcome and a
     * legible one.
     *
     * aes256-ctr rather than ChaCha20 because wolfSSH has no
     * chacha20-poly1305@openssh.com, and a current OpenSSH still offers AES-CTR
     * by default — so nothing on the server has to be weakened for this.
     *
     * Two host key lists have to be set, and setting only one is a trap worth
     * naming: AlgoListKeyAccepted is what the client *advertises* in KEXINIT,
     * while AlgoListKey is what it will *accept* when the server picks. Pin
     * only the second and the server still chooses from the broad advertised
     * list, lands on ssh-rsa, and the connection fails with "cannot match key
     * algo with peer" — which reads like the server's fault and is not. */
    if (wolfSSH_CTX_SetAlgoListKex(s->ctx, "curve25519-sha256") != WS_SUCCESS
            || wolfSSH_CTX_SetAlgoListKeyAccepted(s->ctx, "ssh-ed25519") != WS_SUCCESS
            || wolfSSH_CTX_SetAlgoListKey(s->ctx, "ssh-ed25519") != WS_SUCCESS
            || wolfSSH_CTX_SetAlgoListCipher(s->ctx, "aes256-ctr") != WS_SUCCESS
            || wolfSSH_CTX_SetAlgoListMac(s->ctx, "hmac-sha2-256") != WS_SUCCESS) {
        set_error(s, "the modern algorithm list was not accepted by wolfSSH");
        goto failed;
    }

    s->ssh = wolfSSH_new(s->ctx);
    if (s->ssh == NULL) {
        /* wolfSSH_new returns NULL and nothing else, so the usual suspect is
         * checked here rather than guessed at from a device with no debugger.
         *
         * Creating a session initialises a random number generator, and
         * wolfSSL's seed comes from somewhere platform-specific — /dev/urandom
         * on a desktop, and on a PSP from whatever CUSTOM_RAND_GENERATE_SEED
         * points at. If that is missing or failing, every session fails at
         * birth with no clue as to why. Asking the RNG directly turns "could
         * not be created" into an error code that names the problem. */
        WC_RNG probe;
        int rng = wc_InitRng(&probe);

        if (rng != 0) {
            snprintf(s->error, sizeof(s->error),
                     "no usable random source: wc_InitRng gave %d", rng);
        } else {
            wc_FreeRng(&probe);
            set_error(s, "wolfSSH session could not be created"
                         " (the random source is fine, so this is memory)");
        }
        goto failed;
    }

    wolfSSH_SetIOReadCtx(s->ssh, s);
    wolfSSH_SetIOWriteCtx(s->ssh, s);
    wolfSSH_SetUserAuthCtx(s->ssh, s);
    wolfSSH_SetPublicKeyCheckCtx(s->ssh, s);

    if (wolfSSH_SetUsername(s->ssh, config->user) != WS_SUCCESS) {
        set_error(s, "the user name was not accepted");
        goto failed;
    }

    /* A terminal session rather than a single command: this is a shell client,
     * and the pty is what makes the far side behave like one. */
    if (wolfSSH_SetChannelType(s->ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0)
            != WS_SUCCESS) {
        set_error(s, "a pty session could not be requested");
        goto failed;
    }

    do {
        ret = wolfSSH_connect(s->ssh);
    } while (ret != WS_SUCCESS
             && (wolfSSH_get_error(s->ssh) == WS_WANT_READ
                 || wolfSSH_get_error(s->ssh) == WS_WANT_WRITE));

    if (ret != WS_SUCCESS) {
        if (!s->hostkey_refused) {
            /* wolfSSH's own words: it knows which stage failed and we do not. */
            snprintf(s->error, sizeof(s->error), "%s",
                     wolfSSH_ErrorToName(wolfSSH_get_error(s->ssh)));
        }
        goto failed;
    }

    if (config->columns > 0 && config->rows > 0) {
        /* Not fatal: a server that will not resize is still a usable server. */
        pspssh_resize(s, config->columns, config->rows);
    }

    return s;

failed:
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", s->error[0] ? s->error : "connection failed");
    }
    pspssh_close(s);
    return NULL;
}

int pspssh_read(pspssh_session *session, void *buf, unsigned int len)
{
    int n;

    if (session == NULL) {
        return -1;
    }
    n = wolfSSH_stream_read(session->ssh, (byte *)buf, (word32)len);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -1;      /* clean EOF is still the end of the session */
    }

    switch (wolfSSH_get_error(session->ssh)) {
    case WS_WANT_READ:
    case WS_WANT_WRITE:
        return 0;
    case WS_EOF:
        set_error(session, "the server closed the session");
        return -1;
    default:
        snprintf(session->error, sizeof(session->error), "%s",
                 wolfSSH_ErrorToName(wolfSSH_get_error(session->ssh)));
        return -1;
    }
}

int pspssh_write(pspssh_session *session, const void *buf, unsigned int len)
{
    int n;

    if (session == NULL) {
        return -1;
    }
    n = wolfSSH_stream_send(session->ssh, (byte *)buf, (word32)len);
    if (n >= 0) {
        return n;
    }

    switch (wolfSSH_get_error(session->ssh)) {
    case WS_WANT_READ:
    case WS_WANT_WRITE:
        return 0;

    case WS_WINDOW_FULL:
        /* The channel's send window is exhausted, and it only reopens when the
         * peer's WINDOW_ADJUST is processed — so retrying the write alone would
         * spin forever. Pumping is the protocol's requirement, not the caller's
         * problem, so it happens here: one turn of the crank, then "try again".
         *
         * Right after a channel opens the window is often zero until the first
         * adjustment arrives, which is why this is the ordinary path for a
         * client's first keystroke rather than an exceptional one. */
        {
            word32 pending = 0;
            wolfSSH_worker(session->ssh, &pending);
        }
        return 0;

    default:
        snprintf(session->error, sizeof(session->error), "%s",
                 wolfSSH_ErrorToName(wolfSSH_get_error(session->ssh)));
        return -1;
    }
}

int pspssh_resize(pspssh_session *session, int columns, int rows)
{
    if (session == NULL) {
        return -1;
    }
    return wolfSSH_ChangeTerminalSize(session->ssh,
                                      (word32)columns, (word32)rows, 0, 0)
           == WS_SUCCESS ? 0 : -1;
}

void pspssh_close(pspssh_session *session)
{
    if (session == NULL) {
        return;
    }
    if (session->ssh != NULL) {
        /* Best effort. We are leaving either way, and a failure here would
         * replace a real error with a cosmetic one. */
        wolfSSH_shutdown(session->ssh);
        wolfSSH_free(session->ssh);
    }
    if (session->ctx != NULL) {
        wolfSSH_CTX_free(session->ctx);
    }
    free(session);
}

const char *pspssh_error(const pspssh_session *session)
{
    if (session == NULL || session->error[0] == '\0') {
        return "no error";
    }
    return session->error;
}
