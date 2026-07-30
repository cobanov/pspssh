#!/usr/bin/env bash
# Build wolfSSL and wolfSSH for the PSP and bake them into a toolchain image.
#
#   tools/build-toolchain.sh     -> pspssh/toolchain:latest
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

WOLFSSL_VERSION=5.7.0
WOLFSSH_VERSION=1.5.0
IMAGE=pspssh/toolchain:latest

export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.orbstack/run/docker.sock}"
DOCKER_CONFIG="$(mktemp -d)"; export DOCKER_CONFIG
BUILD_DIR="$(mktemp -d)"
printf '{}' > "$DOCKER_CONFIG/config.json"
trap 'rm -rf "$DOCKER_CONFIG" "$BUILD_DIR"' EXIT

cat > "$BUILD_DIR/Dockerfile" <<EOF
FROM pspdev/pspdev:latest

# util-linux is for colrm, which wolfSSL's configure uses to generate its
# options header and which busybox does not provide. coreutils gives it a cut
# that behaves the way the script expects.
RUN apk add --no-cache curl make autoconf automake libtool util-linux coreutils

WORKDIR /build

# ---------------------------------------------------------------- wolfSSL ---
RUN curl -fsSL -o wolfssl.tar.gz \\
      https://github.com/wolfSSL/wolfssl/archive/refs/tags/v${WOLFSSL_VERSION}-stable.tar.gz \\
    && tar xzf wolfssl.tar.gz

WORKDIR /build/wolfssl-${WOLFSSL_VERSION}-stable

# --host puts autotools into cross-compile mode. Without it, configure runs the
# MIPS test binaries it just built and concludes the compiler is broken.
#
# --enable-wolfssh is wolfSSL's own preset for the features wolfSSH needs. The
# three primitives after it are named anyway: the preset is not documented as a
# stable contract, and a silently missing curve25519 is the failure this whole
# script exists to prevent.
#
# --enable-static --disable-shared because the PSP links one binary.
# --disable-filesystem: keys and known-hosts are ours to handle, and newlib's
# filesystem on a PSP is not what wolfSSL expects.
RUN ./autogen.sh \\
    && ./configure \\
        --host=mipsel-unknown-elf \\
        --prefix=\${PSPDEV}/psp \\
        --enable-static \\
        --disable-shared \\
        --enable-wolfssh \\
        --enable-curve25519 \\
        --enable-ed25519 \\
        --enable-aesctr \\
        --enable-chacha \\
        --enable-poly1305 \\
        --enable-sha512 \\
        --disable-examples \\
        --disable-crypttests \\
        --disable-oldtls \\
        CC=psp-gcc AR=psp-ar RANLIB=psp-ranlib \\
        CFLAGS="-G0 -O2 -DWOLFSSL_USER_IO -DNO_WRITEV -DNO_WOLFSSL_DIR -DSINGLE_THREADED" \\
    && make -j"\$(nproc)" \\
    && make install

# Fail loudly here rather than at a handshake on the device.
RUN set -e; \\
    for s in HAVE_CURVE25519 HAVE_ED25519 WOLFSSL_AES_COUNTER HAVE_CHACHA HAVE_POLY1305; do \\
        grep -qE "^#define \$s\b" \${PSPDEV}/psp/include/wolfssl/options.h \\
            || { echo "wolfSSL built without \$s — SSH would fail to negotiate"; exit 1; }; \\
        echo "  wolfSSL: \$s"; \\
    done

# ---------------------------------------------------------------- wolfSSH ---
WORKDIR /build
RUN curl -fsSL -o wolfssh.tar.gz \\
      https://github.com/wolfSSL/wolfssh/archive/refs/tags/v${WOLFSSH_VERSION}-stable.tar.gz \\
    && tar xzf wolfssh.tar.gz

WORKDIR /build/wolfssh-${WOLFSSH_VERSION}-stable

# --disable-term, plus NO_TERMIOS on top of it. WOLFSSH_TERM is on by default
# and port.h then includes <termios.h>; the PSP toolchain ships a termios.h
# whose first line includes a <sys/termios.h> it does not have, so anything
# reaching for it fails to compile. The guard is
# `!defined(NO_TERMIOS) && defined(WOLFSSH_TERM)`, so both halves are addressed
# rather than relying on one. It costs nothing — that code puts a *local* POSIX
# terminal into raw mode, which is meaningless on a device with no tty.
#
# WOLFSSH_USER_IO is a design decision rather than a workaround. wolfSSH's
# default I/O reaches for <sys/socket.h>, which the PSP does not have — its BSD
# sockets arrive through pspnet_inet. The macro excludes the socket layer and
# hands it to callbacks we register, so wolfSSH becomes pure protocol on a byte
# stream and the transport belongs to us. That is what lets the same library run
# over a PSP socket and a host socket without knowing the difference.
RUN ./autogen.sh \\
    && ./configure \\
        --host=mipsel-unknown-elf \\
        --prefix=\${PSPDEV}/psp \\
        --with-wolfssl=\${PSPDEV}/psp \\
        --enable-static \\
        --disable-shared \\
        --disable-examples \\
        --disable-term \\
        CC=psp-gcc AR=psp-ar RANLIB=psp-ranlib \\
        CFLAGS="-G0 -O2 -DNO_TERMIOS -DWOLFSSH_USER_IO" \\
        LDFLAGS="-L\${PSPDEV}/psp/lib" \\
        LIBS="-lwolfssl" \\
    && make -j"\$(nproc)" \\
    && make install

# The algorithm names live in the library as strings. If curve25519-sha256 is
# not among them the negotiation cannot succeed, whatever else built cleanly.
RUN set -e; \\
    for a in curve25519-sha256 ssh-ed25519 aes256-ctr hmac-sha2-256; do \\
        strings \${PSPDEV}/psp/lib/libwolfssh.a | grep -qF "\$a" \\
            || { echo "libwolfssh.a does not offer \$a"; exit 1; }; \\
        echo "  wolfSSH: \$a"; \\
    done
EOF

echo "==> building wolfSSL ${WOLFSSL_VERSION} and wolfSSH ${WOLFSSH_VERSION} for PSP"
echo "    (slow: the pspdev image is amd64 and runs emulated on Apple Silicon)"
docker build --platform linux/amd64 -t "$IMAGE" -f "$BUILD_DIR/Dockerfile" "$BUILD_DIR"

echo
echo "==> $IMAGE is ready"
