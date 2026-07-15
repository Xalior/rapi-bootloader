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

# LLVM/libc++ comes from a GIT CHECKOUT at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball fetch. Codeberg regenerates its
# auto-archive tarballs, so their bytes (and SHA256) drift from the hash
# circle-stdlib pins -- a clean --libcxx build then fails its hash check. A git
# tag is immutable, so this reproduces from a fresh clone. The checkout lands
# in the gitignored libs/llvm-project that --libcxx-repo reads.
LLVM_REPO = https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  = circle-stdlib-22.1.3-v2

.PHONY: deps loaders images dist card clean $(LOADERS)

# One single-core world per board. They differ ONLY in RASPPI (configure -r),
# derived from the board token (rpi4 -> 4). NONE define ARM_ALLOW_MULTI_CORE:
# Circle's EnableChainBoot() (both loaders use it) refuses a multicore build.
#
# `bash ./configure` (not ./configure): the shebang would pin macOS's bash 3.2;
# PATH resolution finds a modern bash when installed. MAKEINFO=true: newlib
# insists on building its manuals otherwise, which fails without texinfo -- the
# manuals aren't the product.
deps:
	@for b in $(BOARDS); do \
	  r=$${b#rpi}; w=circle-stdlib-$$b; \
	  echo "== $$w (RASPPI=$$r) =="; \
	  git submodule update --init --recursive $$w; \
	  [ -f $$w/libs/llvm-project/runtimes/CMakeLists.txt ] || \
	    git clone --depth 1 --branch $(LLVM_TAG) $(LLVM_REPO) $$w/libs/llvm-project; \
	  ( cd $$w && bash ./configure -r $$r -p aarch64-none-elf- \
	      --libcxx-repo --kernel-max-size 256 && $(MAKE) MAKEINFO=true ); \
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
