#
# rapi-bootloader — top-level build orchestration.
#
#   make deps        fetch + build every single-core circle-stdlib world
#                    (one per board: RASPPI 3/4/5)
#   make loaders     every loader, every board, concurrently -> dist/
#   make images      alias for loaders
#   make <loader>            all boards of one loader (network-loader / menu-loader)
#   make <loader>-<board>    one loader for one board, isolated in
#                    <loader>/build/<board>/  (e.g. network-loader-rpi4)
#   make dist        collect the built images into dist/<loader>/
#   make card        assemble the multi-board network-loader SD tree into
#                    dist/<CARD>/ (config.txt + RPi firmware + all 3 kernels)
#   make clean       remove all per-board build trees and dist/
#
# CONCURRENCY: each (loader, board) compiles in its OWN tree
# (<loader>/build/<board>/) with its own objects and its own distinctly-
# named image, so `make -j loaders` builds them all at once with zero
# shared-object collision. This is the template the pi-mame per-board split
# reuses to hold concurrent trees and dispatch concurrent CI build targets.
#
# BOARD MATRIX: the set of distinct single-core RASPPI worlds. Every
# supported board maps onto one -- Zero 2 W + CM3 -> rpi3, Pi 4 + CM4 +
# Pi 400 -> rpi4, Pi 5 + CM5 -> rpi5. A board needs a NEW world only if it
# needs a new RASPPI (the distinct kernel-image name follows from that);
# board->image routing for the variants lives in the card's config.txt.
#
# Requires the Arm GNU aarch64-none-elf toolchain on PATH, plus a modern
# bash (5+) and GNU getopt for circle-stdlib's configure. See README.md.

BOARDS  ?= rpi3 rpi4 rpi5
LOADERS ?= network-loader menu-loader
DIST    ?= dist

# GNU getopt for circle-stdlib's configure (macOS BSD getopt drops long opts ->
# wrong toolchain prefix). ccache is build/ccache.sh's job (mandatory source).
GETOPT_BIN := $(firstword $(wildcard /opt/homebrew/opt/gnu-getopt/bin /usr/local/opt/gnu-getopt/bin))
ifneq ($(GETOPT_BIN),)
export PATH := $(GETOPT_BIN):$(PATH)
endif

# A modern bash (5+) for `bash ./configure` (macOS ships 3.2, which lacks
# mapfile; the invocation is PATH-resolved for exactly this reason). Same
# conditional shape as gnu-getopt: prepend brew's bin only where a brew bash
# exists; no-op on Linux/CI.
BASH5_BIN := $(firstword $(wildcard /opt/homebrew/bin/bash /usr/local/bin/bash))
ifneq ($(BASH5_BIN),)
export PATH := $(patsubst %/,%,$(dir $(BASH5_BIN))):$(PATH)
endif

# LLVM/libc++ comes from a GIT CHECKOUT at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball fetch. Codeberg regenerates its
# auto-archive tarballs, so their bytes (and SHA256) drift from the hash
# circle-stdlib pins -- a clean --libcxx build then fails its hash check. A git
# tag is immutable, so this reproduces from a fresh clone. The checkout lands
# in the gitignored libs/llvm-project that --libcxx-repo reads.
LLVM_REPO = https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  = circle-stdlib-22.1.3-v2

# ONE checkout of that tag, shared by every world here. All three build the
# same immutable tag, so fetching it once per world was three downloads of
# identical bytes from a small volunteer-run forge, and three copies on disk.
#
# Only the runtimes build is ever read, so the checkout is sparse: the three
# runtime libraries, the cmake modules runtimes/CMakeLists.txt reaches for
# (../cmake, ../llvm/cmake, ../third-party, and ../llvm for llvm/utils/llvm-lit),
# and libc -- libc++'s from_chars includes libc/shared, which in turn reaches
# libc/src/__support. Around 525 MB against a 2.8 GB worktree; clang, lldb,
# mlir, flang and the rest never arrive.
#
# Every entry is here because cmake or a compile named the thing it could not
# find. Trim one and the build tells you which, some minutes in.
#
# CIRCLE_LLVM names the checkout. The default sits beside this repository, so
# a plain clone and a CI runner are each self-contained with nothing set;
# point several projects at one path to share the fetch across all of them.
CIRCLE_LLVM ?= $(abspath $(CURDIR)/../circle-llvm)
LLVM_SPARSE  = libcxx libcxxabi libunwind runtimes cmake llvm/cmake \
               llvm/utils third-party libc

.PHONY: deps llvm-cache loaders images dist card clean $(LOADERS)

# Fetched once, then left alone. The tag is immutable, so a checkout that
# already carries the runtimes tree is finished by definition.
llvm-cache:
	@[ -f $(CIRCLE_LLVM)/runtimes/CMakeLists.txt ] || { \
	  echo "== llvm-project $(LLVM_TAG) -> $(CIRCLE_LLVM) (once) =="; \
	  rm -rf $(CIRCLE_LLVM) && \
	  git clone --quiet --depth 1 --branch $(LLVM_TAG) --no-checkout --sparse \
	    $(LLVM_REPO) $(CIRCLE_LLVM) && \
	  git -C $(CIRCLE_LLVM) sparse-checkout set --cone $(LLVM_SPARSE) && \
	  git -C $(CIRCLE_LLVM) checkout --quiet; }

# One single-core world per board. They differ ONLY in RASPPI (configure -r),
# derived from the board token (rpi4 -> 4). NONE define ARM_ALLOW_MULTI_CORE:
# Circle's EnableChainBoot() (both loaders use it) refuses a multicore build.
#
# The rpi5 world alone carries -o DEPTH=32: the Pi 5 has ONE firmware
# surface for every kernel and the card makes it 32bpp (card/config.txt
# framebuffer_depth=32, the knob that lets the display controller do the
# format work in hardware) — so that board's console must render 32bpp
# too. CScreenDevice's depth is compiled into libcircle, hence a world
# option. Pi 3/4 firmware honors per-kernel framebuffer requests; their
# worlds keep Circle's defaults and are not rebuilt for this.
# `bash ./configure` (not ./configure): the shebang would pin macOS's bash 3.2;
# PATH resolution finds a modern bash when installed. MAKEINFO=true: newlib
# insists on building its manuals otherwise, which fails without texinfo -- the
# manuals aren't the product.
deps: llvm-cache
	@set -e; for b in $(BOARDS); do \
	  r=$${b#rpi}; w=circle-stdlib-$$b; \
	  echo "== $$w (RASPPI=$$r) =="; \
	  git submodule update --init --recursive $$w; \
	  [ -f $$w/libs/llvm-project/runtimes/CMakeLists.txt ] || \
	    ln -sfn $(CIRCLE_LLVM) $$w/libs/llvm-project; \
	  if [ $$b = rpi5 ]; then o="-o DEPTH=32"; else o=""; fi; \
	  ( cd $$w && bash ./configure -r $$r -p aarch64-none-elf- \
	      --libcxx-repo --kernel-max-size 255 $$o && $(MAKE) MAKEINFO=true ); \
	done

# Per-(loader, board) build. Runs the loader's own Makefile from an isolated
# build/<board>/ tree: SRCDIR points back at the loader sources, ROOT back at
# this directory, CIRCLE_STDLIB selects the board's world. Objects and the
# image stay in that tree, so every combo is parallel-safe. `+$(MAKE)` shares
# the -j jobserver, so a single `make -j` spreads work across all combos.
define LOADER_BOARD_rule
.PHONY: $(1)-$(2)
$(1)-$(2):
	@mkdir -p $(1)/build/$(2)
	+$$(MAKE) -C $(1)/build/$(2) -f ../../Makefile \
		ROOT=../../.. SRCDIR=../.. CIRCLE_STDLIB=circle-stdlib-$(2)
endef
$(foreach L,$(LOADERS),$(foreach B,$(BOARDS),$(eval $(call LOADER_BOARD_rule,$(L),$(B)))))

# All boards of one loader (build only; run `make dist` or `make loaders` to
# stage). e.g. `make network-loader` -> network-loader for rpi3, rpi4, rpi5.
$(foreach L,$(LOADERS),$(eval $(L): $(foreach B,$(BOARDS),$(L)-$(B))))

COMBOS = $(foreach L,$(LOADERS),$(foreach B,$(BOARDS),$(L)-$(B)))

# Build every combo (parallel under -j), then stage the images.
loaders images: $(COMBOS)
	@$(MAKE) --no-print-directory dist

# Collect the per-board images into one canonical tree the card build (and the
# pi-mame split's CI dispatch) reads from: dist/<loader>/<image>. The three
# boards' images are distinctly named, so all coexist here side by side.
dist:
	@for L in $(LOADERS); do \
	  mkdir -p $(DIST)/$$L; \
	  for B in $(BOARDS); do \
	    [ -d $$L/build/$$B ] && cp $$L/build/$$B/kernel*.img $(DIST)/$$L/ 2>/dev/null || true; \
	  done; \
	done
	@echo "staged into $(DIST)/:"; \
	 ls -1 $(DIST)/*/kernel*.img 2>/dev/null | sed 's/^/  /' || echo "  (nothing built yet)"

# Assemble the multi-board network-loader SD tree into dist/<CARD>/ from
# source: the tracked card/config.txt, standard Raspberry Pi Foundation
# firmware + DTBs from Circle's boot/ dir (board-agnostic; the rpi4 world's
# copy serves all), and all three network-loader kernel images. Reproducible
# with `make card` -- flash the resulting tree onto a FAT32 SD.
CARD    ?= multiboard-network-loader-sdcard
BOOTSRC := circle-stdlib-rpi4/libs/circle/boot

card: $(foreach B,$(BOARDS),network-loader-$(B))
	@rm -rf $(DIST)/$(CARD)
	@mkdir -p $(DIST)/$(CARD)/overlays
	@cp card/config.txt $(DIST)/$(CARD)/
	@cp $(BOOTSRC)/bootcode.bin $(BOOTSRC)/start*.elf $(BOOTSRC)/fixup*.dat \
	    $(BOOTSRC)/armstub8-rpi4.bin $(BOOTSRC)/*.dtb $(DIST)/$(CARD)/
	@cp $(BOOTSRC)/*.dtbo $(DIST)/$(CARD)/overlays/ 2>/dev/null || true
	@for B in $(BOARDS); do cp network-loader/build/$$B/kernel*.img $(DIST)/$(CARD)/; done
	@echo "assembled $(DIST)/$(CARD)/:"; ls -1 $(DIST)/$(CARD)

# Remove ONLY what this build produces: each loader's per-board build tree and
# its staged image dir under dist/. Never `rm -rf $(DIST)` wholesale -- dist/
# also holds hand-assembled card trees and other non-rebuilt artifacts that
# are not ours to delete.
clean:
	@rm -rf $(foreach L,$(LOADERS),$(L)/build $(DIST)/$(L))
	@echo "removed per-board build trees and their $(DIST)/ staging dirs"
