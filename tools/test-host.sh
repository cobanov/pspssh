#!/usr/bin/env bash
# Build the session core for the host and run it against a real OpenSSH server.
#
#   tools/test-host.sh [host] [port] [user] [password]
#
# Defaults to the project's test container on 127.0.0.1:2222.
#
# This is the half of the verification that cannot be done with test vectors. A
# key exchange either convinces OpenSSH or it does not, so the only way to know
# is to ask one — and doing it here, on a laptop, means the PSP is never the
# place a protocol bug is found.
#
# The same src/core builds for the PSP. Only the two transport callbacks in
# src/host differ.
set -euo pipefail

cd "$(dirname "$0")/.."

HOST="${1:-127.0.0.1}"
PORT="${2:-2222}"
USER_NAME="${3:-bb}"
PASSWORD="${4:-bbssh}"

IMAGE=pspssh/host:latest

export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.orbstack/run/docker.sock}"
DOCKER_CONFIG="$(mktemp -d)"; export DOCKER_CONFIG
BUILD_DIR="$(mktemp -d)"
printf '{}' > "$DOCKER_CONFIG/config.json"
trap 'rm -rf "$DOCKER_CONFIG" "$BUILD_DIR"' EXIT

if ! nc -z "$HOST" "$PORT" 2>/dev/null; then
    echo "nothing listening on $HOST:$PORT" >&2
    echo "the project's test server is a container: docker start bbssh" >&2
    exit 1
fi

# Built once and cached. Same versions and the same feature flags as the PSP
# toolchain, deliberately: a host test proves nothing about the device build if
# the two libraries were configured differently.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    cat > "$BUILD_DIR/Dockerfile" <<'EOF'
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential curl ca-certificates autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN curl -fsSL -o wolfssl.tar.gz \
      https://github.com/wolfSSL/wolfssl/archive/refs/tags/v5.7.0-stable.tar.gz \
    && tar xzf wolfssl.tar.gz
WORKDIR /build/wolfssl-5.7.0-stable
RUN ./autogen.sh \
    && ./configure --prefix=/usr/local --enable-static --disable-shared \
        --enable-wolfssh --enable-harden --enable-curve25519 \
        --enable-ed25519 --enable-ed25519-stream --enable-keygen \
        --enable-aesctr --enable-chacha --enable-poly1305 --enable-sha512 \
        --disable-examples --disable-crypttests \
    && make -j"$(nproc)" && make install

WORKDIR /build
RUN curl -fsSL -o wolfssh.tar.gz \
      https://github.com/wolfSSL/wolfssh/archive/refs/tags/v1.5.0-stable.tar.gz \
    && tar xzf wolfssh.tar.gz
WORKDIR /build/wolfssh-1.5.0-stable

# NO_TERMIOS matches the PSP build. The host has a tty and could use wolfSSH's
# terminal handling, but then the two builds would not be the same library and
# this test would stop being evidence about the device.
RUN ./autogen.sh \
    && ./configure --prefix=/usr/local --with-wolfssl=/usr/local \
        --enable-static --disable-shared --disable-examples \
        CFLAGS="-O2 -DNO_TERMIOS -DWOLFSSH_USER_IO" \
        LDFLAGS="-L/usr/local/lib" LIBS="-lwolfssl" \
    && make -j"$(nproc)" && make install
EOF
    echo "==> building the host toolchain (once)"
    docker build -t "$IMAGE" -f "$BUILD_DIR/Dockerfile" "$BUILD_DIR"
fi

echo "==> compiling"
docker run --rm -v "$PWD:/src" -w /src "$IMAGE" sh -c '
    set -e
    mkdir -p build
    gcc -O2 -Wall -Wextra -Werror -std=c99 -D_DEFAULT_SOURCE \
        -I/usr/local/include \
        src/core/pspssh.c src/host/test_against_server.c \
        -o build/test_against_server \
        -L/usr/local/lib -lwolfssh -lwolfssl -lm
'

echo "==> running against $HOST:$PORT"
# host.docker.internal is how a container reaches a server on the host.
TARGET="$HOST"
if [ "$HOST" = "127.0.0.1" ] || [ "$HOST" = "localhost" ]; then
    TARGET="host.docker.internal"
fi
OUTPUT="$(docker run --rm -v "$PWD:/src" -w /src \
    --add-host=host.docker.internal:host-gateway \
    "$IMAGE" ./build/test_against_server "$TARGET" "$PORT" "$USER_NAME" "$PASSWORD")"
STATUS=$?
echo "$OUTPUT" | grep -v '^FINGERPRINT='

# The check that caught the client quietly settling on ssh-rsa. A fingerprint
# that is merely well-formed proves nothing; it has to be the server's.
echo
echo "==> comparing the fingerprint with the server's own"
OURS="$(echo "$OUTPUT" | sed -n 's/^FINGERPRINT=//p')"
THEIRS="$(ssh-keyscan -t ed25519 -p "$PORT" "$HOST" 2>/dev/null \
          | ssh-keygen -lf - 2>/dev/null | awk '{print $2}' | head -1)"

if [ -z "$OURS" ]; then
    echo "  FAIL  the client did not report a fingerprint" >&2
    exit 1
fi
if [ -z "$THEIRS" ]; then
    echo "  SKIP  ssh-keyscan could not reach the server to confirm"
elif [ "$OURS" = "$THEIRS" ]; then
    echo "  PASS  it matches what the server reports"
    echo "        $OURS"
else
    echo "  FAIL  fingerprint mismatch — the client is hashing the wrong thing," >&2
    echo "        or negotiated a different host key than expected" >&2
    echo "        client: $OURS" >&2
    echo "        server: $THEIRS" >&2
    exit 1
fi

exit $STATUS
