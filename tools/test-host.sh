#!/usr/bin/env bash
# Build the session core for the host and run it against a real OpenSSH server.
#
#   tools/test-host.sh                              # a server this starts itself
#   tools/test-host.sh <host> <port> <user> <password>     # one of yours
#
# This is the half of the verification that cannot be done with test vectors. A
# key exchange either convinces OpenSSH or it does not, so the only way to know
# is to ask one — and doing it here, on a laptop, means the PSP is never the
# place a protocol bug is found.
#
# The same src/core builds for the PSP. Only the two transport callbacks in
# src/host differ.
#
# With no arguments the server is an sshd inside the test image, with a user
# made for the purpose. Nothing has to be running beforehand, and its log can be
# read afterwards — which is how "a wrong password is tried once" is checked
# from the side that counts.
#
# Against a server of your own, one of the tests logs in with a wrong password
# on purpose. That is one failed login in its log.
set -euo pipefail

cd "$(dirname "$0")/.."

. tools/versions.sh
IMAGE=$HOST_IMAGE

export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.orbstack/run/docker.sock}"
DOCKER_CONFIG="$(mktemp -d)"; export DOCKER_CONFIG
BUILD_DIR="$(mktemp -d)"
printf '{}' > "$DOCKER_CONFIG/config.json"
trap 'rm -rf "$DOCKER_CONFIG" "$BUILD_DIR"' EXIT

# Built once per set of versions. The same sources and the same feature flags as
# the PSP toolchain, read from the same file: a host test proves nothing about
# the device build if the two libraries were configured differently.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    cat > "$BUILD_DIR/Dockerfile" <<EOF
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \\
        build-essential curl ca-certificates autoconf automake libtool \\
        openssh-server \\
    && rm -rf /var/lib/apt/lists/*

# The server the tests talk to when they are not given one.
RUN useradd -m -s /bin/bash bb && echo 'bb:bbssh' | chpasswd && mkdir -p /run/sshd

WORKDIR /build
RUN curl -fsSL -o wolfssl.tar.gz \\
      https://github.com/wolfSSL/wolfssl/archive/refs/tags/v${WOLFSSL_VERSION}-stable.tar.gz \\
    && echo "${WOLFSSL_SHA256}  wolfssl.tar.gz" | sha256sum -c - \\
    && tar xzf wolfssl.tar.gz
WORKDIR /build/wolfssl-${WOLFSSL_VERSION}-stable
RUN ./autogen.sh \\
    && ./configure --prefix=/usr/local ${WOLFSSL_FEATURES} \\
    && make -j"\$(nproc)" && make install

WORKDIR /build
RUN curl -fsSL -o wolfssh.tar.gz \\
      https://github.com/wolfSSL/wolfssh/archive/refs/tags/v${WOLFSSH_VERSION}-stable.tar.gz \\
    && echo "${WOLFSSH_SHA256}  wolfssh.tar.gz" | sha256sum -c - \\
    && tar xzf wolfssh.tar.gz
WORKDIR /build/wolfssh-${WOLFSSH_VERSION}-stable
RUN ./autogen.sh \\
    && ./configure --prefix=/usr/local --with-wolfssl=/usr/local \\
        --enable-static --disable-shared --disable-examples \\
        CFLAGS="-O2 ${WOLFSSH_CFLAGS}" \\
        LDFLAGS="-L/usr/local/lib" LIBS="-lwolfssl" \\
    && make -j"\$(nproc)" && make install
EOF
    echo "==> building the host toolchain (once per set of versions)"
    docker build -t "$IMAGE" -f "$BUILD_DIR/Dockerfile" "$BUILD_DIR"
fi

COMPILE='gcc -O2 -Wall -Wextra -Werror -std=c99 -D_DEFAULT_SOURCE \
    -I/usr/local/include src/core/pspssh.c src/host/test_against_server.c \
    -o /tmp/test_against_server -L/usr/local/lib -lwolfssh -lwolfssl -lm'

# Collected rather than piped, and the status taken by hand: under `set -e` a
# failing command substitution ends the script on the spot, before the output
# that says what failed has been printed.
STATUS=0
if [ $# -eq 0 ]; then
    echo "==> running against an sshd of its own"
    # The old algorithms are switched back on deliberately. A server offering
    # only ed25519 would pass a client that takes whatever comes first; this
    # one offers ssh-rsa too, so landing on ed25519 is the client's doing.
    OUTPUT="$(docker run --rm -v "$PWD:/src:ro" -w /src "$IMAGE" sh -c "
        set -e
        $COMPILE
        /usr/sbin/sshd -p 2222 -E /tmp/sshd.log \
            -o HostKeyAlgorithms=+ssh-rsa -o PasswordAuthentication=yes
        until ssh-keyscan -p 2222 127.0.0.1 >/dev/null 2>&1; do sleep 0.2; done
        status=0
        /tmp/test_against_server 127.0.0.1 2222 bb bbssh || status=\$?
        echo \"SERVER_FINGERPRINT=\$(ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub | cut -d' ' -f2)\"
        echo \"FAILED_PASSWORDS=\$(grep -c 'Failed password' /tmp/sshd.log || true)\"
        echo \"ACCEPTED_PASSWORDS=\$(grep -c 'Accepted password' /tmp/sshd.log || true)\"
        exit \$status
    ")" || STATUS=$?
else
    [ $# -eq 4 ] || { echo "usage: tools/test-host.sh [<host> <port> <user> <password>]" >&2; exit 2; }
    HOST="$1"; PORT="$2"
    echo "==> running against $HOST:$PORT"
    # host.docker.internal is how a container reaches a server on the host.
    TARGET="$HOST"
    if [ "$HOST" = "127.0.0.1" ] || [ "$HOST" = "localhost" ]; then
        TARGET="host.docker.internal"
    fi
    OUTPUT="$(docker run --rm -v "$PWD:/src:ro" -w /src \
        --add-host=host.docker.internal:host-gateway "$IMAGE" sh -c "
        set -e
        $COMPILE
        /tmp/test_against_server \"\$@\"
    " sh "$TARGET" "$PORT" "$3" "$4")" || STATUS=$?
    THEIRS="$(ssh-keyscan -t ed25519 -p "$PORT" "$HOST" 2>/dev/null \
              | ssh-keygen -lf - 2>/dev/null | awk '{print $2}' | head -1)"
    OUTPUT="$OUTPUT
SERVER_FINGERPRINT=$THEIRS"
fi

echo "$OUTPUT" | grep -vE '^(FINGERPRINT|SERVER_FINGERPRINT|FAILED_PASSWORDS|ACCEPTED_PASSWORDS)='

value() { echo "$OUTPUT" | sed -n "s/^$1=//p" | head -1; }

# The check that caught the client quietly settling on ssh-rsa. A fingerprint
# that is merely well-formed proves nothing; it has to be the server's.
echo
echo "==> comparing with what the server says"
OURS="$(value FINGERPRINT)"
THEIRS="$(value SERVER_FINGERPRINT)"

if [ -z "$OURS" ]; then
    echo "  FAIL  the client did not report a fingerprint" >&2
    STATUS=1
elif [ -z "$THEIRS" ]; then
    echo "  SKIP  the server's own fingerprint could not be read to confirm"
elif [ "$OURS" = "$THEIRS" ]; then
    echo "  PASS  the fingerprint matches the server's own"
    echo "        $OURS"
else
    echo "  FAIL  fingerprint mismatch — the client is hashing the wrong thing," >&2
    echo "        or negotiated a different host key than expected" >&2
    echo "        client: $OURS" >&2
    echo "        server: $THEIRS" >&2
    STATUS=1
fi

# Counted in the server's log, because that is where it matters. The session
# that works is the one accepted login; the wrong password is the one failure,
# not six; and the refused host key shows up as neither, having sent nothing.
FAILED="$(value FAILED_PASSWORDS)"
ACCEPTED="$(value ACCEPTED_PASSWORDS)"
if [ -n "$FAILED" ]; then
    if [ "$FAILED" = 1 ] && [ "$ACCEPTED" = 1 ]; then
        echo "  PASS  the server saw one failed login and one accepted, no more"
    else
        echo "  FAIL  the server saw $FAILED failed and $ACCEPTED accepted logins," >&2
        echo "        where one of each was sent on purpose" >&2
        STATUS=1
    fi
fi

exit $STATUS
