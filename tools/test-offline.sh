#!/usr/bin/env bash
# Run the tests that need no PSP.
#
#   tools/test-offline.sh
#
# Two modules in src/psp have logic decidable without hardware, and both have
# failure modes that are miserable to diagnose on a device with no console:
#
#   hosts.c       parses, validates and writes a file. The bug it guards
#                 against is silently losing every server somebody saved.
#   knownhosts.c  decides whether the far side is who it was last time. The bug
#                 it guards against is the check quietly not checking.
#   term.c        parses escape sequences. The bug it guards against is "the
#                 screen looks a bit odd sometimes", which is not a report
#                 anyone can act on.
#   console.c     wraps, scrolls and expands tabs. It shows every message the
#                 application produces before a session opens, so a bug here
#                 corrupts the diagnosis of every other bug.
#
# The first two are compiled against a POSIX shim for the six sceIo calls;
# term.c is plain C and needs nothing. The rest of src/psp genuinely does need a screen, a
# radio and a system keyboard, and this does not pretend otherwise.
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> compiling"
$CC -std=c99 -Wall -Wextra -Werror -O1 -DPSPSSH_HOST_TEST \
    -o "$WORK/test_hosts" \
    src/host/test_hosts.c src/psp/hosts.c src/psp/cardfile.c
$CC -std=c99 -Wall -Wextra -Werror -O1 -DPSPSSH_HOST_TEST \
    -o "$WORK/test_knownhosts" \
    src/host/test_knownhosts.c src/psp/knownhosts.c src/psp/cardfile.c
$CC -std=c99 -Wall -Wextra -Werror -O1 \
    -o "$WORK/test_terminal" \
    src/host/test_terminal.c src/psp/term.c
$CC -std=c99 -Wall -Wextra -Werror -O1 \
    -o "$WORK/test_console" \
    src/host/test_console.c src/psp/console.c

# In its own directory: the host tests create, rename and delete pspssh.hosts,
# and doing that in the source tree would be a good way to lose a real one.
echo
( cd "$WORK" && ./test_hosts )
echo
( cd "$WORK" && ./test_knownhosts )
echo
( cd "$WORK" && ./test_terminal )
echo
( cd "$WORK" && ./test_console )
