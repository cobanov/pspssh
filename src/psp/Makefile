# Builds the PSP front end into an EBOOT.PBP.
#
# Driven by tools/build-psp.sh, which runs it inside the toolchain image so no
# local PSP toolchain is needed.

TARGET = pspssh
OBJS = main.o gfx.o console.o pad.o osk.o hosts.o knownhosts.o cardfile.o term.o ui.o net.o entropy.o \
       ../core/pspssh.o

INCDIR =
CFLAGS = -G0 -O2 -Wall -Wextra -std=c99 -DNO_TERMIOS -DWOLFSSH_USER_IO
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)

# wolfssh before wolfssl: the linker resolves left to right and wolfssh is what
# depends on wolfssl, not the other way round.
#
# No *extra stub* libraries here, and that is the fix rather than an omission.
#
# Naming -lpspnet_inet, -lpspnet_resolver or -lpsputility made every build warn
# "could not fixup imports, stubs out of order" and say the binary may or may
# not work — which is not a thing to ship. Isolating it against the SDK's own
# samples showed the wolf libraries were innocent and any *extra* PSP stub
# triggered it, in either order.
#
# The reason is in psp-gcc's own specs, which end their library list with:
#
#     -lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver -lpspsdk ...
#
# Those are therefore on every link line already. Naming one again puts a second
# copy of its stub section ahead of the first, and the order of those sections
# is precisely what psp-fixup-imports objects to. It is why the ELF imports
# sceNetInetInit, sceNetApctlConnect, sceNetResolverStartNtoA and
# sceUtilityLoadNetModule while naming none of them — and why the SDK's own
# resolver sample links none of them either, which was the clue.
#
# -lpspgu is a different kind of thing and is safe to add: not a stub library at
# all, but ordinary code that assembles display lists and hands them to the GE.
# The GE's stubs (-lpspge) are already in build.mak's list, so this introduces
# no new imports to get out of order.
LIBS = -lwolfssh -lwolfssl -lm -lpspgu

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = pspssh

# What the XMB shows under Information, taken from the header rather than
# written down here.
#
# The version field was blank because nothing ever set APP_VER — DISC_VERSION
# exists and is what a UMD uses, but a memory-stick title is read from APP_VER.
# Reading both out of pspssh.h means the version on that screen, the version in
# the banner and the version in the git tag cannot disagree; tools/release.sh
# already refuses to tag a mismatch between the last two.
PSPSSH_VERSION := $(shell sed -n 's/^\#define PSPSSH_VERSION "\(.*\)"$$/\1/p' ../core/pspssh.h)

# PARAM.SFO is not built here. tools/build-psp.sh writes it before invoking
# make, using tools/make-sfo.py — because mksfoex does not sort the key table
# and the format says it is ordered, and because the toolchain image has no
# python3 to run the replacement inside. make's rule for PARAM.SFO has no
# prerequisites, so a file already present is left alone.

# What the XMB shows for this application, packed into the EBOOT itself rather
# than copied to the card beside it.
#
# Named only when present, so the build works with none of them and picks each
# one up the moment it is added — the alternative is a missing artwork file
# failing a build that has nothing to do with artwork.
#
#   ICON0.PNG   144x80    the icon in the game list
#   ICON1.PMF   144x80    an animation shown instead of the icon
#   PIC0.PNG    480x272   drawn over the background
#   PIC1.PNG    480x272   the background, when this is the highlighted item
#   SND0.AT3    ATRAC3    music, looped while it is highlighted
#
# The names are the ones pack-pbp documents. They are not what the format cares
# about — the slots are positional and the file could be called anything — but
# a directory of files named after the slot they fill is a directory nobody has
# to consult a Makefile to understand.
ASSETS = ../../assets

ifneq ($(wildcard $(ASSETS)/ICON0.PNG),)
PSP_EBOOT_ICON = $(ASSETS)/ICON0.PNG
endif
ifneq ($(wildcard $(ASSETS)/ICON1.PMF),)
PSP_EBOOT_ICON1 = $(ASSETS)/ICON1.PMF
endif
ifneq ($(wildcard $(ASSETS)/PIC0.PNG),)
PSP_EBOOT_PIC0 = $(ASSETS)/PIC0.PNG
endif
ifneq ($(wildcard $(ASSETS)/PIC1.PNG),)
PSP_EBOOT_PIC1 = $(ASSETS)/PIC1.PNG
endif
ifneq ($(wildcard $(ASSETS)/SND0.AT3),)
PSP_EBOOT_SND0 = $(ASSETS)/SND0.AT3
endif

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
