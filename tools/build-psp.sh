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

docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src "$IMAGE" sh -lc '
    export PATH="$PSPDEV/bin:$PATH"
    cd src/psp && make clean
' >/dev/null 2>&1 || true

# PARAM.SFO is written here rather than inside the toolchain image, which has no
# python3 — and mksfoex, which it does have, emits an unsorted key table. See
# tools/make-sfo.py. It goes between the clean and the build because make's rule
# for it has no prerequisites: present, and make leaves it alone.
#
# APP_VER has to be XX.YY — two digits, a point, two digits — and giving it
# "1.3.4" is why the information panel showed a dash where the version goes.
# Three components do not fit in four digits, so the minor and the patch share
# the second pair: 1.3.7 becomes 01.37. It is a compression rather than a
# translation, which is why the exact version also goes in the title, where it
# is proven to display and needs no shape at all.
echo "==> writing PARAM.SFO"
VERSION="$(sed -n 's/^#define PSPSSH_VERSION "\(.*\)"$/\1/p' src/core/pspssh.h)"
[ -n "$VERSION" ] || { echo "no PSPSSH_VERSION in src/core/pspssh.h" >&2; exit 1; }
# 1.3.7 -> 01.37. Falls back to the major and minor alone if the minor ever
# reaches ten, at which point the pair has run out of room and a wrong number is
# worse than a rounded one.
SFO_VER="$(printf '%s' "$VERSION" | awk -F. '{
    if ($2 < 10 && $3 < 10) printf "%02d.%d%d", $1, $2, $3;
    else printf "%02d.%02d", $1, $2;
}')"

tools/make-sfo.py \
    APP_VER="$SFO_VER" DISC_VERSION="$SFO_VER" \
    CATEGORY=MG DISC_ID=UCJS10041 PSP_SYSTEM_VER=1.00 \
    BOOTABLE=:1 REGION=:32768 PARENTAL_LEVEL=:1 MEMSIZE=:1 \
    "pspssh $VERSION" src/psp/PARAM.SFO

echo "==> compiling for PSP"
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src "$IMAGE" sh -lc '
    export PATH="$PSPDEV/bin:$PATH"
    cd src/psp
    make
'

mkdir -p build/psp
cp src/psp/EBOOT.PBP build/psp/

# No starter pspssh.cfg any more, and its absence is deliberate.
#
# Servers are added on the device now, so a placeholder file would be imported
# on first launch as a host called 192.168.1.10 belonging to a user called "me"
# — a fake entry to delete before the real one can be added. The application
# still reads a pspssh.cfg written by hand, once, for anyone upgrading.
#
# Anything the user saves lives in pspssh.hosts beside the binary, which this
# never touches: a rebuild that reset somebody's servers would be a small
# betrayal, and the binary gets rebuilt far more often than the list changes.

SIZE=$(stat -f%z build/psp/EBOOT.PBP 2>/dev/null || stat -c%s build/psp/EBOOT.PBP)
echo
echo "==> built build/psp/EBOOT.PBP ($SIZE bytes)"
echo
echo "Copy build/psp to the memory card as PSP/GAME/pspssh/ and launch it"
echo "from the XMB. Add servers from the list on the first screen."
