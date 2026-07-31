#!/usr/bin/env bash
# Run the host-list tests on this machine.
#
#   tools/test-hosts.sh
#
# src/psp/hosts.c is the one PSP module whose logic needs no PSP: it parses,
# validates and writes a file. Compiling it here against a POSIX shim for the
# six sceIo calls means the storage layer is proved before it is trusted with
# somebody's servers.
#
# The rest of src/psp genuinely does need hardware — a screen, a radio, a system
# keyboard — and is not pretended otherwise. This covers the part that can be
# covered, which is the part where a silent bug loses data.
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> compiling"
$CC -std=c99 -Wall -Wextra -Werror -O1 -DPSPSSH_HOST_TEST \
    -o "$WORK/test_hosts" \
    src/host/test_hosts.c src/psp/hosts.c

echo "==> running in a scratch directory"
# In its own directory: the tests create, rename and delete pspssh.hosts, and
# doing that in the source tree would be a good way to lose a real one.
( cd "$WORK" && ./test_hosts )
