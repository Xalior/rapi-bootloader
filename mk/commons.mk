#
# rapi-bootloader commons — the two parts every loader rides on.
#
# A loader in this project is a RIDER: it decides where a payload kernel
# comes from and which defaults-string goes into it. Everything below that
# decision is shared, and there are exactly two shared parts:
#
#   defaultsblock/  the 0x800 defaults-block ABI — the writer that patches
#                   a defaults-string into a staged image, the magic,
#                   capacity and length rules that make it safe, and the
#                   one decision every rider would otherwise make for
#                   itself: an image with no block boots unpatched.
#   chainboot/      the chain-boot itself — staging a payload somewhere it
#                   can be received and copied from (StageAlloc), and the
#                   hand-off that replaces the running loader with it.
#
# A rider includes this file and gets both, correctly built for its board.
# It costs one line, and it is the point: a rider that wires the commons by
# hand can silently miss one. That is not hypothetical — the Pi 5 chain-boot
# hand-off lived in one rider's own directory and never reached the other,
# so every Pi 5 card in a release stopped dead at hand-off while the bench
# loader was fine.
#
# Usage, from a rider's Makefile after including its Config.mk and before
# Circle's Rules.mk (which consumes both variables):
#
#   include $(ROOT)/mk/commons.mk
#   OBJS = main.o kernel.o ... $(COMMON_OBJS)
#
# ROOT must point at the rapi-bootloader top level (riders already set it).
#
# This file deliberately defines NO rules. Circle's Rules.mk supplies the
# %.o: %.cpp pattern and the vpath below tells it where the commons live,
# which is the same mechanism a rider already uses for its own out-of-tree
# sources. Rules here would also be the first make reads — ahead of
# Rules.mk's own — and make takes the first rule as the default goal, so a
# build would quietly make one common object, skip the image, and exit 0.
#

DEFAULTSDIR  = $(ROOT)/defaultsblock
CHAINBOOTDIR = $(ROOT)/chainboot

# Where the commons' sources are found, for Circle's pattern rule.
vpath %.cpp $(DEFAULTSDIR) $(CHAINBOOTDIR)

# Riders include "defaultsblock.h", "stagealloc.h" and "rapi_chainboot.h"
# by plain name; EXTRAINCLUDE resolves them for the commons' own objects
# and for the rider's.
EXTRAINCLUDE += -I$(DEFAULTSDIR) -I$(CHAINBOOTDIR)

# Every rider links all of these. rapi_chainboot.o defines Circle's three
# chain-boot symbols on the Pi 5 only, which keeps libcircle's chainboot.o
# out of that link (an archive member is pulled only for an otherwise
# undefined symbol); on the Pi 3 and Pi 4 it defines none of them and
# Circle's own implementation is what runs.
COMMON_OBJS = defaultsblock.o bootimage.o stagealloc.o rapi_chainboot.o
