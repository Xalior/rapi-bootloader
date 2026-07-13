#
# rapi-bootloader — top-level build orchestration.
#
#   make deps            fetch + build the single-core circle-stdlib world
#                        (the C/C++ runtime both loaders link against)
#   make network-loader  the TFTP/HTTP/WebDAV chain-loader image
#   make menu-loader     the on-card boot-picker image
#   make loaders         both loader images
#   make clean           remove loader build outputs (leaves circle-stdlib)
#
# Requires the Arm GNU aarch64-none-elf toolchain on PATH, plus a modern
# bash (5+) and GNU getopt for circle-stdlib's configure. See README.md.

.PHONY: deps network-loader menu-loader loaders clean

# LLVM/libc++ comes from a GIT CHECKOUT at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball fetch. Codeberg regenerates its
# auto-archive tarballs, so their bytes (and SHA256) drift from the hash
# circle-stdlib pins — a clean --libcxx build then fails its hash check. A git
# tag is immutable, so this reproduces from a fresh clone. The checkout lands
# in the gitignored libs/llvm-project that --libcxx-repo reads.
LLVM_REPO = https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  = circle-stdlib-22.1.3-v2

# Single-core boot world: NO -o ARM_ALLOW_MULTI_CORE. Circle's
# EnableChainBoot() (both loaders use it) refuses a multicore build.
#
# `bash ./configure` (not ./configure): the shebang would pin macOS's
# bash 3.2; PATH resolution finds a modern bash when one is installed.
# MAKEINFO=true: newlib insists on building its manuals otherwise, which
# fails on any system without texinfo — the manuals aren't the product.
deps:
	git submodule update --init --recursive circle-stdlib
	@[ -f circle-stdlib/libs/llvm-project/runtimes/CMakeLists.txt ] || \
		git clone --depth 1 --branch $(LLVM_TAG) $(LLVM_REPO) circle-stdlib/libs/llvm-project
	cd circle-stdlib && bash ./configure -r 4 -p aarch64-none-elf- \
		--libcxx-repo --kernel-max-size 256 && $(MAKE) MAKEINFO=true

network-loader:
	$(MAKE) -C network-loader

menu-loader:
	$(MAKE) -C menu-loader

loaders: network-loader menu-loader

clean:
	$(MAKE) -C network-loader clean
	$(MAKE) -C menu-loader clean
