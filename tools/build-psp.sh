#!/usr/bin/env bash
# Build the PSP front end into an installable EBOOT.PBP.
#
#   tools/build-psp.sh      -> build/psp/EBOOT.PBP
#                          (+ a starter pspssh.cfg, if there is not one)
#
# Runs inside the toolchain image from tools/build-toolchain.sh, so no local PSP
# toolchain is needed. Build that first if you have not.
#
# ## Installing
#
# Copy the whole build/psp directory to the memory card as:
#
#     ms0:/PSP/GAME/pspssh/
#
# On a PSP Go the internal storage is ef0: instead, so:
#
#     ef0:/PSP/GAME/pspssh/
#
# Then edit pspssh.cfg on the card with the server to reach, and launch it from
# the XMB under Game > Memory Stick.
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE=pspssh/toolchain:latest

export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.orbstack/run/docker.sock}"
DOCKER_CONFIG="$(mktemp -d)"; export DOCKER_CONFIG
printf '{}' > "$DOCKER_CONFIG/config.json"
trap 'rm -rf "$DOCKER_CONFIG"' EXIT

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "$IMAGE is missing — run tools/build-toolchain.sh first" >&2
    exit 1
fi

echo "==> compiling for PSP"
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src "$IMAGE" sh -lc '
    export PATH="$PSPDEV/bin:$PATH"
    cd src/psp
    make clean >/dev/null 2>&1 || true
    make
'

mkdir -p build/psp
cp src/psp/EBOOT.PBP build/psp/

# Written only when it is not already there. A rebuild that silently reset
# somebody's server, password and profile number would be a small betrayal, and
# this file gets edited far more often than the binary gets rebuilt.
#
# The password sits in plain text on a memory card, which the file says rather
# than leaving someone to discover it.
if [ -f build/psp/pspssh.cfg ]; then
    echo "==> keeping the existing build/psp/pspssh.cfg"
else
cat > build/psp/pspssh.cfg <<'CFG'
# pspssh — the server to reach.
#
# This file is plain text on a memory card. Anyone holding the PSP can read it,
# so use an account you are willing to have on a games console.

host=192.168.1.10
port=22
user=me
password=

# Which saved Wi-Fi connection to use, counting from 1 in the order they appear
# under Settings > Network Settings. The network existing on your router is not
# enough — the PSP needs its own connection saved for it.
profile=1
CFG
fi

SIZE=$(stat -f%z build/psp/EBOOT.PBP 2>/dev/null || stat -c%s build/psp/EBOOT.PBP)
echo
echo "==> built build/psp/EBOOT.PBP ($SIZE bytes)"
echo
echo "Copy build/psp to the memory card as PSP/GAME/pspssh/,"
echo "edit pspssh.cfg there, then launch it from the XMB."
