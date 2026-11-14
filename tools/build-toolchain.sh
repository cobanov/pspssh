#!/usr/bin/env bash
# Build wolfSSL and wolfSSH for the PSP and bake them into a toolchain image.
#
#   tools/build-toolchain.sh     -> pspssh/toolchain:<versions>, see versions.sh
#
# Everything happens inside the container. Nothing is installed on the host and
# no local toolchain is needed beyond Docker.
#
# ## Why wolfSSL is rebuilt rather than used from psp-pacman
#
# The packaged wolfSSL is configured for TLS, which is what everyone else on
# this platform wants it for — curl over HTTPS. Reading its options.h shows
# HAVE_CHACHA and HAVE_POLY1305 and AES-GCM, but *not* HAVE_CURVE25519,
# HAVE_ED25519 or WOLFSSL_AES_COUNTER.
#
# Those three are exactly what a modern SSH session needs: curve25519-sha256 to
# agree a key, ssh-ed25519 to verify the host, aes256-ctr to encrypt. Built
# against the packaged library, wolfSSH compiles and links happily and then
# cannot negotiate with any current OpenSSH — the failure would appear as a
# handshake that dies for no visible reason, on hardware with no debugger.
#
# So it is rebuilt with --enable-wolfssh, which is wolfSSL's own preset for
# exactly this, plus the primitives named explicitly rather than hoped for.
set -euo pipefail

cd "$(dirname "$0")/.."

. tools/versions.sh
IMAGE=$TOOLCHAIN_IMAGE

export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.orbstack/run/docker.sock}"
DOCKER_CONFIG="$(mktemp -d)"; export DOCKER_CONFIG
BUILD_DIR="$(mktemp -d)"
printf '{}' > "$DOCKER_CONFIG/config.json"
trap 'rm -rf "$DOCKER_CONFIG" "$BUILD_DIR"' EXIT

cat > "$BUILD_DIR/Dockerfile" <<EOF
FROM ${PSPDEV_IMAGE}

# util-linux is for colrm, which wolfSSL's configure uses to generate its
# options header and which busybox does not provide. coreutils gives it a cut
# that behaves the way the script expects.
RUN apk add --no-cache curl make autoconf automake libtool util-linux coreutils

WORKDIR /build

# ---------------------------------------------------------------- wolfSSL ---
RUN curl -fsSL -o wolfssl.tar.gz \\
      https://github.com/wolfSSL/wolfssl/archive/refs/tags/v${WOLFSSL_VERSION}-stable.tar.gz \\
    && echo "${WOLFSSL_SHA256}  wolfssl.tar.gz" | sha256sum -c - \\
    && tar xzf wolfssl.tar.gz

# wolfSSL compiles with -Werror, and random.c calls the seed function without a
# prototype in sight — an implicit declaration is then fatal. A forced include
# gives it one without patching the source.
RUN printf '%s\\n' \\
        'int pspssh_generate_seed(unsigned char *output, unsigned int sz);' \\
        > /build/pspssh_seed.h

WORKDIR /build/wolfssl-${WOLFSSL_VERSION}-stable

# --host puts autotools into cross-compile mode. Without it, configure runs the
# MIPS test binaries it just built and concludes the compiler is broken.
#
# The feature flags are in tools/versions.sh, shared with the host build.
#
# CUSTOM_RAND_GENERATE_SEED is not optional here, it is the difference between
# working and not. A PSP has no /dev/urandom and no hardware RNG, so wolfSSL
# found nothing to seed from and wc_InitRng returned RNG_FAILURE_E (-199) —
# which surfaced as every session failing to be created, with no clue why.
#
# The named function lives in src/psp/entropy.c and is resolved at the final
# link. It harvests timing jitter into a SHA-256 pool and refuses to produce
# anything if the clock turns out not to jitter, because predictable bytes here
# are a guessable session key.
#
# This is the one place the PSP and host builds deliberately differ. An entropy
# source cannot be platform-neutral, and the host has a real one.
#
# Static because the PSP links one binary.
#
# WOLFSSL_NO_GETPID: wolfSSL remembers the process id so a forked child reseeds.
# configure finds a getpid in the SDK and turns that on, and then random.c does
# not compile, because nothing it includes declares one. A PSP cannot fork, so
# the check is switched off rather than given a prototype.
#
# WOLFSSL_NO_SOCK: wolfSSL's headers otherwise reach for <sys/socket.h>, which
# on this SDK lives under psp/sdk/include and is not on the path of a plain
# cross-compile. Nothing here wants wolfSSL's sockets anyway — the transport is
# two callbacks of ours — so saying so is truer than adding the path.
RUN ./autogen.sh \\
    && ./configure \\
        --host=mipsel-unknown-elf \\
        --prefix=\${PSPDEV}/psp \\
        ${WOLFSSL_FEATURES} \\
        CC=psp-gcc AR=psp-ar RANLIB=psp-ranlib \\
        CFLAGS="-G0 -O2 -DWOLFSSL_USER_IO -DNO_WRITEV -DNO_WOLFSSL_DIR -DSINGLE_THREADED \\
                -DWOLFSSL_NO_GETPID -DWOLFSSL_NO_SOCK \\
                -DCUSTOM_RAND_GENERATE_SEED=pspssh_generate_seed \\
                -include /build/pspssh_seed.h" \\
    && make -j"\$(nproc)" \\
    && make install

# Fail loudly here rather than at a handshake on the device.
#
# WOLFSSL_NO_GETPID is here for a different reason from the rest: it changes
# the layout of WC_RNG, so it has to reach options.h or the application and the
# library would disagree about the size of a structure one puts on the stack.
#
# Only macros that configure actually writes into options.h belong in this list.
# HAVE_ED25519_KEY_IMPORT and _EXPORT do not: settings.h derives them from
# HAVE_ED25519, so grepping options.h for them fails on a perfectly good build.
# Those are covered by the compile check below, which is the authoritative one
# because it evaluates every header together the way the real build does.
RUN set -e; \\
    for s in HAVE_CURVE25519 HAVE_ED25519 WOLFSSL_ED25519_STREAMING_VERIFY \\
             WOLFSSL_AES_COUNTER HAVE_CHACHA HAVE_POLY1305 WOLFSSL_NO_GETPID; do \\
        grep -qE "^#define \$s\b" \${PSPDEV}/psp/include/wolfssl/options.h \\
            || { echo "wolfSSL built without \$s — SSH would fail to negotiate"; exit 1; }; \\
        echo "  wolfSSL: \$s"; \\
    done

# ---------------------------------------------------------------- wolfSSH ---
WORKDIR /build
RUN curl -fsSL -o wolfssh.tar.gz \\
      https://github.com/wolfSSL/wolfssh/archive/refs/tags/v${WOLFSSH_VERSION}-stable.tar.gz \\
    && echo "${WOLFSSH_SHA256}  wolfssh.tar.gz" | sha256sum -c - \\
    && tar xzf wolfssh.tar.gz

WORKDIR /build/wolfssh-${WOLFSSH_VERSION}-stable

# WOLFSSH_TERM stays ON, and NO_TERMIOS is what makes that possible.
#
# The PSP toolchain ships a termios.h whose first line includes a
# <sys/termios.h> that does not exist, so anything reaching for it fails to
# compile. The obvious response is --disable-term, and it is wrong: reading the
# guards shows WOLFSSH_SESSION_TERMINAL lives inside `#ifdef WOLFSSH_TERM` in
# ssh.c, so disabling it removes the ability to request a pty at all — which for
# a terminal client is the entire point.
#
# The right combination comes from the guard itself:
#
#     #if !defined(NO_TERMIOS) && defined(WOLFSSH_TERM)
#         #include <termios.h>
#
# Keeping WOLFSSH_TERM and defining NO_TERMIOS satisfies both halves: pty-req,
# the SESSION_TERMINAL channel type and wolfSSH_ChangeTerminalSize are all
# compiled, while the <termios.h> include and every use of WOLFSSH_TERMIOS —
# which are guarded on !NO_TERMIOS too — are not. What is dropped is local tty
# handling, which is meaningless on a device that has no tty. Terminal size
# falls back to a built-in 80x24 and we override it explicitly.
#
# WOLFSSH_USER_IO is a design decision rather than a workaround. wolfSSH's
# default I/O reaches for <sys/socket.h>, which the PSP does not have — its BSD
# sockets arrive through pspnet_inet. The macro excludes the socket layer and
# hands it to callbacks we register, so wolfSSH becomes pure protocol on a byte
# stream and the transport belongs to us. That is what lets the same library run
# over a PSP socket and a host socket without knowing the difference.
#
# ac_cv_lib_wolfssl_wolfCrypt_Init answers a question configure cannot ask here.
# It checks for wolfSSL by linking a program against it, and that link needs
# pspssh_generate_seed, which lives in the application and does not exist yet.
# The library was installed by the step above; whether it has what SSH needs is
# what the checks after this one establish.
RUN ./autogen.sh \\
    && ./configure \\
        --host=mipsel-unknown-elf \\
        --prefix=\${PSPDEV}/psp \\
        --with-wolfssl=\${PSPDEV}/psp \\
        --enable-static \\
        --disable-shared \\
        --disable-examples \\
        CC=psp-gcc AR=psp-ar RANLIB=psp-ranlib \\
        CFLAGS="-G0 -O2 ${WOLFSSH_CFLAGS}" \\
        LDFLAGS="-L\${PSPDEV}/psp/lib" \\
        LIBS="-lwolfssl" \\
        ac_cv_lib_wolfssl_wolfCrypt_Init=yes \\
    && make -j"\$(nproc)" \\
    && make install

# Ask the compiler, not the strings.
#
# Grepping the archive for "ssh-ed25519" passed while ed25519 was in fact
# disabled: wolfSSH turns it off unless wolfSSL provides HAVE_ED25519 *and*
# WOLFSSL_ED25519_STREAMING_VERIFY *and* key import *and* key export, and the
# name still appears in the binary for other reasons. The session then
# negotiated ssh-rsa quite happily, which is the exact failure this project
# exists to prevent — and it took comparing a fingerprint against the server's
# own to notice.
#
# So the check is now the one that cannot be fooled: compile against the headers
# and fail if wolfSSH says the algorithm is off.
RUN printf '%s\\n' \\
        '#include <wolfssh/ssh.h>' \\
        '#include <wolfssh/internal.h>' \\
        '#ifdef WOLFSSH_NO_ED25519' \\
        '#error "ed25519 host keys are disabled — the client would fall back to RSA"' \\
        '#endif' \\
        '#ifdef WOLFSSH_NO_CURVE25519_SHA256' \\
        '#error "curve25519 key exchange is disabled"' \\
        '#endif' \\
        '#ifdef WOLFSSH_NO_AES_CTR' \\
        '#error "aes-ctr is disabled — nothing modern would be left to encrypt with"' \\
        '#endif' \\
        'int main(void) { return 0; }' \\
        > /tmp/algo_check.c \\
    && psp-gcc -G0 ${WOLFSSH_CFLAGS} \\
        -I\${PSPDEV}/psp/include \\
        -I/build/wolfssh-${WOLFSSH_VERSION}-stable \\
        -c /tmp/algo_check.c -o /tmp/algo_check.o \\
    && echo "  wolfSSH: ed25519, curve25519 and aes-ctr are all compiled in"

RUN set -e; \\
    for a in curve25519-sha256 ssh-ed25519 aes256-ctr hmac-sha2-256; do \\
        strings \${PSPDEV}/psp/lib/libwolfssh.a | grep -qF "\$a" \\
            || { echo "libwolfssh.a does not offer \$a"; exit 1; }; \\
        echo "  wolfSSH: \$a"; \\
    done; \\
    export PATH="\${PSPDEV}/bin:\$PATH"; \\
    cd /tmp && psp-ar x \${PSPDEV}/psp/lib/libwolfssh.a; \\
    for f in wolfSSH_connect wolfSSH_SetChannelType wolfSSH_ChangeTerminalSize \\
             wolfSSH_stream_read wolfSSH_SetIORecv wolfSSH_CTX_SetPublicKeyCheck; do \\
        psp-nm *.o 2>/dev/null | grep -q " T \$f\$" \\
            || { echo "libwolfssh.a has no \$f — the terminal API was compiled out"; exit 1; }; \\
        echo "  wolfSSH: \$f"; \\
    done; \\
    rm -f /tmp/*.o
EOF

echo "==> building wolfSSL ${WOLFSSL_VERSION} and wolfSSH ${WOLFSSH_VERSION} for PSP"
echo "    (slow: the pspdev image is amd64 and runs emulated on Apple Silicon)"
docker build --platform linux/amd64 -t "$IMAGE" -f "$BUILD_DIR/Dockerfile" "$BUILD_DIR"

echo
echo "==> $IMAGE is ready"
