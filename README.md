# rapi-bootloader

A single-core boot building block for bare-metal Raspberry Pi 4 payloads
built on the [Circle](https://github.com/rsta2/circle) framework. It is two
small argv-loaders that stamp a plain-text argument string into a kernel
image at a fixed offset and chain-boot it:

- **network-loader** — a development chain-loader. It brings up the network,
  serves TFTP, HTTP (port 8080) and WebDAV (port 8081), receives a kernel
  image over the wire into a high-heap staging buffer, optionally stamps an
  argv defaults-string into it, and chain-boots it. Reflash-free iteration:
  push a new image, it runs in RAM.
- **menu-loader** — an on-card boot picker. It reads a `bootmenu.cfg` list
  from the SD card, presents it on screen, takes a keyboard selection,
  stamps the chosen defaults-string into a staged platform kernel image, and
  chain-boots it.

Both are **single-core** by design: Circle's `EnableChainBoot()` refuses a
multicore build, so the boot world is configured without
`ARM_ALLOW_MULTI_CORE`.

## The 0x800 defaults-block ABI

rapi-bootloader owns the small ABI both loaders write through
(`defaultsblock/`). A consuming kernel image carries a fixed-layout block at
image offset **0x800**:

| field | offset | notes |
|-------|--------|-------|
| `Magic[4]` | 0x00 | `'P','M','8','D'` — a seatbelt, verified before any write |
| `Capacity` | 0x04 | `u16`, bytes available in `Text[]` |
| `Length`   | 0x06 | `u16`, bytes used in `Text[]` (excludes the NUL) |
| `Text[]`   | 0x08 | NUL-terminated plain-text argv string (starts at 0x808) |

`PatchDefaults()` verifies the magic first and refuses the write if it is
absent (a re-ordered link script becomes a refused write, never argv text
stamped over startup code) and enforces the string length against the
block's own `Capacity` field. The consuming kernel tokenises `Text[]` and
appends it to its own argv.

## Layout

```
defaultsblock/   the shared 0x800 ABI (writer side): PatchDefaults()
network-loader/  the TFTP/HTTP/WebDAV chain-loader
menu-loader/     the on-card boot picker
mk/ld/           TLS-adjacent linker script + link rule (see below)
circle-stdlib/   submodule: the C/C++ runtime world both loaders link
```

`mk/ld/circle-tls.ld` is derived from Circle's `circle.ld` and differs only
in that `.tbss` directly follows `.tdata` — binutils 2.44+ refuses to map a
`PT_TLS` segment from non-adjacent TLS sections, which libc++/libunwind
carry. The Circle tree itself is never modified.

## Building

**Prerequisites**

- The [Arm GNU `aarch64-none-elf`
  toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  on your `PATH`.
- A modern `bash` (5+) and GNU `getopt` on your `PATH` — circle-stdlib's
  `configure` needs `mapfile` and GNU-style option parsing (macOS ships
  bash 3.2 and BSD getopt; `brew install bash gnu-getopt` provides both).

**Steps**

```sh
git clone --recursive <this-repo-url> rapi-bootloader
cd rapi-bootloader
make deps            # fetch + build the single-core circle-stdlib world
make network-loader  # -> network-loader/kernel8-rpi4.img
make menu-loader     # -> menu-loader/kernel8-rpi4.img
# or: make loaders   # both
```

`make deps` initialises the `circle-stdlib` submodule (pinned to the
project's tested commit), clones the immutable-tagged LLVM/libc++ checkout it
builds libc++ from, then configures circle-stdlib **single-core**
(`-r 4 -p aarch64-none-elf- --libcxx-repo --kernel-max-size 256`) and builds
it. Each loader then links its image against that world.

## Licence

GPLv3 — see `LICENSE`. `mk/ld/circle-tls.ld` is derived from Circle
(GPLv3, © R. Stange).
