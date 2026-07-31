# Builds the PSP front end into an EBOOT.PBP.
#
# Driven by tools/build-psp.sh, which runs it inside the toolchain image so no
# local PSP toolchain is needed.

TARGET = pspssh
OBJS = main.o entropy.o ../core/pspssh.o

INCDIR =
CFLAGS = -G0 -O2 -Wall -Wextra -std=c99 -DNO_TERMIOS -DWOLFSSH_USER_IO
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)

# wolfssh before wolfssl: the linker resolves left to right and wolfssh is what
# depends on wolfssl, not the other way round.
#
# The pspnet libraries are separate stubs and all four are needed — inet for
# sockets, apctl to join a profile, resolver for DNS, and pspnet underneath
# them.
#
# No PSP stub libraries here, and that is the fix rather than an omission.
#
# Naming -lpspnet_inet, -lpspnet_resolver or -lpsputility made every build warn
# "could not fixup imports, stubs out of order" and say the binary may or may
# not work — which is not a thing to ship. Isolating it against the SDK's own
# samples showed the wolf libraries were innocent and any *extra* PSP stub
# triggered it, in either order.
#
# They turned out to be unnecessary. build.mak appends -lpspnet -lpspnet_apctl
# and the standard set, and those resolve everything: the ELF still imports
# sceNetInetInit, sceNetApctlConnect, sceNetResolverStartNtoA and
# sceUtilityLoadNetModule. The SDK's own resolver sample links none of them
# either, which was the clue.
LIBS = -lwolfssh -lwolfssl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = pspssh

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
