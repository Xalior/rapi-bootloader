# rapi-bootloader

rapi-bootloader is a pair of single-core boot loaders for bare-metal
Raspberry Pi payloads, built on the
[Circle](https://github.com/rsta2/circle) framework. Both stamp a
plain-text argument string into a kernel image at a fixed offset, then
chain-boot it:

- **network-loader** is a development chain-loader. It takes its address
  from a `[rapi-bootloader]` section of the card's `config.txt`, or asks
  DHCP when the section states none. It serves TFTP, HTTP (port 8080) and
  WebDAV (port 8081). It receives a kernel image over the network into a
  staging buffer, stamps an optional argv defaults-string into it, and
  chain-boots it. Push a new image and it runs in RAM, so nothing rewrites
  the SD card between iterations. `card/config.txt.example` is a card
  configuration with the section already filled in. See
  [`network-loader/README.md`](network-loader/README.md).
- **menu-loader** is an on-card boot picker. It reads a `bootmenu.cfg` list
  from the SD card and shows it on screen. It takes a keyboard selection,
  stamps the chosen defaults-string into a staged kernel image, and
  chain-boots it. See [`menu-loader/README.md`](menu-loader/README.md).

Both loaders are single-core by design. Circle's `EnableChainBoot()` refuses
a multicore build, so the C/C++ runtime world each links is configured
without `ARM_ALLOW_MULTI_CORE`. The Raspberry Pi 5 replacement described
below keeps the same restriction.

## Where this comes from

The repository is new; almost nothing in it is. Anyone who has worked
bare-metal on a Raspberry Pi for long has built these parts, because the board
gives you none of them: a way to get a fresh kernel onto the thing without
pulling the SD card out for the hundredth time; a way to hand a payload its
arguments when there is no command line, no environment and no filesystem
convention to read them from; a way to choose, at boot, which of several
payloads runs. Each time, they were rebuilt inside whichever project needed
them, and stayed there, buried, when that project ended. The boot picker's
design goes back to NextPi in 2018, and the ideas under it are older still.
More than a decade of the same wheels, re-cut.

rapi-bootloader is that accumulation, dug out, cleaned up, and put somewhere
public. It gives the pieces one home and the argument-passing ABI a single
owner, so that the next project can depend on them rather than write them
again. Nothing here is specific to the payload that finally prompted the
collection. Any Circle kernel carrying the 0x800 block can be pushed,
stamped and booted by these loaders; any Circle kernel without one boots
too, just unstamped. The block is an optional ABI, so a loader here boots a
plain Circle kernel as readily as an argv-taking one.

## The 0x800 defaults-block ABI

rapi-bootloader owns the small ABI both loaders write through
(`defaultsblock/`). A consuming kernel image carries a fixed-layout block at
image offset **0x800**:

| field | offset | notes |
|-------|--------|-------|
| `Magic[4]` | 0x00 | `'P','M','8','D'`, a seatbelt verified before any write |
| `Capacity` | 0x04 | `u16`, bytes available in `Text[]` |
| `Length`   | 0x06 | `u16`, bytes used in `Text[]` (excludes the NUL) |
| `Text[]`   | 0x08 | NUL-terminated plain-text argv string (starts at 0x808) |

`PatchDefaults()` checks the magic first and refuses the write if it is
absent, so a re-ordered link script produces a refused write rather than
argv text stamped over startup code. It also enforces the string length
against the block's own `Capacity` field. The refusal applies to the write
only. An image with no block still boots, unstamped, because no loader
decides that for itself. `BootImageWithDefaults()` in `defaultsblock/`
stamps where it can and chain-boots either way, so every loader answers the
question the same way, and a new loader inherits the answer. The consuming
kernel tokenises `Text[]` and appends it to its own argv.

The struct is `PACKED` and little-endian (matching AArch64), so its `sizeof`
is `4 + 2 + 2 + Capacity`. An image must be at least `0x800 + sizeof` bytes
long before any writer may dereference the block. AArch64 kernels load at
`MEM_KERNEL_START` (`0x80000`), so the block's runtime address is `0x80800`.
The image's first instruction is a `b` trampoline over the reserved boot
furniture, which is what lets the block occupy fixed space near the head of
the image without displacing Circle's startup contract.

### Worked example

Nothing beyond ordinary binary file I/O is needed, so a patcher can be written
in any language. In a real image the block reads:

```
00000800: 504d 3844 0002 0c00 6336 3420 2d69 6563  PM8D....c64 -iec
00000810: 3820 2222 0000 0000 0000 0000 0000 0000  8 ""............
```

`PM8D` at `+0x00`; `Capacity` `00 02` (LE `0x0200` = 512); `Length` `0c 00`
(LE `0x000c` = 12, the length of `c64 -iec8 ""`); `Text` from `+0x08` holding
that string, NUL-padded. Read it with nothing but `xxd`:

```sh
xxd -l 8   -s 0x800 kernel8-rpi4.img     # magic + capacity + length
xxd -l 512 -s 0x808 kernel8-rpi4.img | head -1   # the text
```

Writing it means verifying the magic, enforcing the block's own `Capacity`,
writing `Text` NUL-terminated, and updating `Length` (excluding the NUL).
That is the whole contract:

```python
#!/usr/bin/env python3
import struct, sys

OFFSET       = 0x800
CAPACITY_OFF = OFFSET + 4
LENGTH_OFF   = OFFSET + 6
TEXT_OFF     = OFFSET + 8

def patch(path, text):
    data = bytearray(open(path, "rb").read())

    if bytes(data[OFFSET:OFFSET + 4]) != b"PM8D":
        sys.exit(f"{path}: no PM8D magic at 0x{OFFSET:x} — not an ABI image")

    capacity = struct.unpack_from("<H", data, CAPACITY_OFF)[0]
    encoded  = text.encode("ascii") + b"\x00"
    if len(encoded) > capacity:
        sys.exit(f"{path}: too long ({len(encoded)} bytes, capacity {capacity})")

    data[TEXT_OFF:TEXT_OFF + len(encoded)] = encoded
    struct.pack_into("<H", data, LENGTH_OFF, len(encoded) - 1)  # excludes the NUL

    with open(path, "r+b") as f:
        f.seek(OFFSET)
        f.write(data[OFFSET:TEXT_OFF + capacity])

if __name__ == "__main__":
    patch(sys.argv[1], sys.argv[2])
```

```sh
python3 patch.py kernel8-rpi4.img 'c64 -iec8 ""'
```

Writing an empty string clears the block. The consuming kernel then appends
nothing and boots its own default behaviour, so a patched-but-empty image
behaves exactly like an unpatched one.

## The shared parts

Each loader in this repository makes two decisions: where a payload kernel
comes from, and what argument string goes into it. The network-loader
receives the payload over the network. The menu-loader reads it from the SD
card and asks a person to choose. Below that decision, everything is
identical, and lives in directories every loader uses:

- `defaultsblock/` is the 0x800 argument-block writer described above.
- `chainboot/` places a payload in memory where it can be received and
  copied from, then replaces the running loader with it.
- `buildstamp/` is the image's own build time, in its own object so a
  relink that reuses other objects still reports the build it rides in.

A loader gets all three by including `mk/commons.mk` in its makefile and
adding `$(COMMON_OBJS)` to its object list, one line instead of a copied set
of build rules. A loader that wires the shared parts by hand can miss one
without the build failing to say so.

### Chain-boot on the Raspberry Pi 5

On the Raspberry Pi 3 and Pi 4, Circle's own chain-boot is correct and is
what those builds use. On the Pi 5 it cannot work, for separate
reasons:

1. Circle switches the data cache off and then runs compiled C++ that uses
   the stack. The Pi 5's Cortex-A76 core does not tolerate that order. The
   Pi 4's Cortex-A72 does.
2. Circle places its copy routine at address `0x7FC00`. On the Pi 5 that
   address is inside Trusted Firmware, which occupies `0x1000` to `0x80000`
   and stays in memory to start the payload's other CPU cores.
3. The Pi 5 has a memory-side system cache that set/way cache maintenance
   cannot reach. A payload writes its early state with the memory
   management unit off, then reads it back with the unit on, so anything
   the loader left behind in that cache reads back stale. Maintenance by
   virtual address does reach it, and that is what the replacement uses.

`chainboot/rapi_chainboot.cpp` addresses all of them by defining Circle's
chain-boot functions itself on Pi 5 builds. A library member is only linked
when it resolves a symbol nothing else defines, so defining them here keeps
Circle's version out of the build entirely. On the Pi 3 and Pi 4 the file
defines none of them, so the build links Circle's own version as usual. The
Circle tree itself is never modified.

The one thing a loader must do is call `RapiChainBootMainReturning()` from
`CKernel::Run()`, immediately before it returns `ShutdownReboot`. On the
Pi 5 this marks the point after which the hand-off is safe. On the other
boards it does nothing, so a loader calls it without testing which board it
is built for.

## Layout

```
defaultsblock/   the shared 0x800 ABI (writer side): PatchDefaults()
chainboot/       the shared chain-boot: staging a payload, and replacing
                 the running loader with it (see below)
buildstamp/      the shared build-time stamp for a rider's boot banner
network-loader/  the TFTP/HTTP/WebDAV chain-loader  (see its README)
menu-loader/     the on-card boot picker            (see its README)
mk/commons.mk    one include that gives a loader all three shared parts
mk/ld/           TLS-adjacent linker script + link rule (see below)
circle-stdlib-rpi{3,4,5}/  submodules: one single-core C/C++ runtime world
                 per board (RASPPI 3/4/5); a loader links the selected one
card/            the multi-board network-loader SD card source (make card)
dist/            collected per-board loader images (make dist)
```

`mk/ld/circle-tls.ld` is derived from Circle's `circle.ld`. The only
difference is that `.tbss` follows `.tdata` directly, because binutils
2.44+ refuses to map a `PT_TLS` segment from non-adjacent TLS sections,
which is the layout libc++ and libunwind need. The Circle tree itself is
never modified.

## Keyboard layout

The SD card's `cmdline.txt` carries the keyboard layout, which Circle reads
at boot. A missing or unrecognised name falls back to German. To match a
different keyboard, add `keymap=` to the line:

    keymap=UK

Valid values are `US`, `UK`, `DE`, `ES`, `FR`, `IT` and `DV` (Dvorak). Names
are case-sensitive. A name in the wrong case matches nothing, and the
system falls back to German instead.

**The loaders themselves do not use it.** The menu-loader reads the keyboard
as raw HID usage codes. The cursor keys, the digits, Enter and the paging
keys carry the same codes on every layout, so its menu works the same
whatever is set here. The setting is for the payload that gets booted, not
for the loader. Anything that reads typed characters, rather than named
keys, gets them through this layout. On the wrong one, the letters and
digits are still right and the punctuation is not.

It is set here rather than by the payload because `cmdline.txt` belongs to
the card, and one card can boot several payloads.

## Building

**Prerequisites**

- The [Arm GNU `aarch64-none-elf`
  toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  on your `PATH`.
- A modern `bash` (5+) and GNU `getopt` on your `PATH`. circle-stdlib's
  `configure` needs `mapfile` and GNU-style option parsing (macOS ships
  bash 3.2 and BSD getopt; `brew install bash gnu-getopt` provides both).

**Steps**

```sh
git clone --recursive <this-repo-url> rapi-bootloader
cd rapi-bootloader
make deps                 # fetch + build every single-core world (RASPPI 3/4/5)
make -j loaders           # every loader x every board, concurrently -> dist/
# or one combo at a time:
make network-loader-rpi4  # just the Pi 4 chain-loader, in its own build tree
```

`make deps` initialises the `circle-stdlib-rpi{3,4,5}` world submodules,
each pinned to the project's tested commit. It clones the immutable-tagged
LLVM/libc++ checkout each world builds libc++ from. Then it configures
every world **single-core** (`-r <board> -p aarch64-none-elf- --libcxx-repo
--kernel-max-size 255`) and builds it.

`make -j loaders` builds each (loader, board) pair in its own out-of-tree
directory, `<loader>/build/<board>/`, with isolated objects and a
distinctly-named image (`kernel8.img` / `kernel8-rpi4.img` /
`kernel_2712.img`). That isolation is what lets every combination build
concurrently with zero collision. It then collects the images into
`dist/<loader>/`. Every supported board maps onto one of these worlds:
Zero 2 W + CM3 → rpi3, Pi 4 + CM4 + Pi 400 → rpi4, Pi 5 + CM5 → rpi5. The
board-to-image routing lives in the card's `config.txt`. The pi-mame split
reuses this per-board, concurrent-tree layout to dispatch its own
concurrent CI build targets.

## Licence

GPLv3, see `LICENSE`. `mk/ld/circle-tls.ld` is derived from Circle
(GPLv3, © R. Stange).
