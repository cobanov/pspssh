# The libraries this is built on, stated once.
#
# Sourced by build-toolchain.sh (the PSP build), test-host.sh (the laptop build)
# and build-psp.sh. A host test is only evidence about the device while both
# were made from the same versions with the same features, and two copies of
# these lines are two things to forget to change together.
#
# The image names carry the versions, so a bump here builds new images instead
# of quietly reusing ones made from the old sources.
#
# To move to a newer release: change the version, download the archive, and put
# its `shasum -a 256` here. Then run tools/test-host.sh before anything else.

WOLFSSL_VERSION=5.9.4
WOLFSSL_SHA256=7256bfc89b183a75183806c7debfa203443873b0b4a562e1b80d68e01b45ac57

WOLFSSH_VERSION=1.6.0
WOLFSSH_SHA256=e345b580643af6715a4293ed7bab4e99ea562bb379adb8898eeae55c38b12f56

# pspdev/pspdev:latest as it was on 2026-11-01, by digest: a tag can move.
PSPDEV_IMAGE=pspdev/pspdev@sha256:ee385df3aa783189d5a033af5eb1d4a9cdd93718fb8bf652ec7bb48e54038a8b

# --enable-wolfssh is wolfSSL's own preset for what wolfSSH needs. The
# primitives after it are named anyway: the preset is not documented as a stable
# contract, and a silently missing curve25519 is a handshake that dies for no
# visible reason.
WOLFSSL_FEATURES="--enable-static --disable-shared --enable-wolfssh \
--enable-harden --enable-curve25519 --enable-ed25519 --enable-ed25519-stream \
--enable-keygen --enable-aesctr --enable-chacha --enable-poly1305 \
--enable-sha512 --disable-examples --disable-crypttests --disable-oldtls"

# NO_TERMIOS because the PSP has no tty, WOLFSSH_USER_IO because the transport
# is ours. The host has both a tty and sockets and is built without them anyway.
WOLFSSH_CFLAGS="-DNO_TERMIOS -DWOLFSSH_USER_IO"

WOLF_TAG="wolfssl-$WOLFSSL_VERSION-wolfssh-$WOLFSSH_VERSION"
TOOLCHAIN_IMAGE="pspssh/toolchain:$WOLF_TAG"
HOST_IMAGE="pspssh/host:$WOLF_TAG"
