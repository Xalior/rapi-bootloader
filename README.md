# rapi-bootloader

A single-core boot building block for bare-metal Raspberry Pi 4 payloads
built on the [Circle](https://github.com/rsta2/circle) framework. These are
small argv-loaders that stamp a plain-text argument string into a kernel
image at a fixed offset and chain-boot it:

- **network-loader** — a development chain-loader. It takes its address from a
  `[rapi-bootloader]` section of the card's `config.txt`, or asks DHCP when
  that section states none; serves TFTP, HTTP (port 8080) and WebDAV
  (port 8081); receives a kernel image over the wire into a high-heap staging
  buffer, optionally stamps an argv defaults-string into it, and chain-boots
  it. Iteration without rewriting the card: push a new image, it runs in RAM.
  `card/config.txt.example` is a card configuration with the section filled
  in. See [`network-loader/README.md`](network-loader/README.md).
- **menu-loader** — an on-card boot picker. It reads a `bootmenu.cfg` list
  from the SD card, presents it on screen, takes a keyboard selection,
  stamps the chosen defaults-string into a staged platform kernel image, and
  chain-boots it. See [`menu-loader/README.md`](menu-loader/README.md).

Both are **single-core** by design, so the C/C++ runtime world they link is
configured without `ARM_ALLOW_MULTI_CORE`. Circle's `EnableChainBoot()`
refuses a multicore build, and the Raspberry Pi 5 replacement described below
keeps the same restriction.

## Where this comes from

The repository is new; almost nothing in it is. Anyone who has worked
bare-metal on a Raspberry Pi for long has built these parts, because the board
gives you none of them: a way to get a fresh kernel onto the thing without
pulling the SD card out for the hundredth time; a way to hand a payload its
arguments when there is no command line, no environment and no filesystem
convention to read them from; a way to choose, at boot, which of several
payloads runs. Each time, they were rebuilt inside whichever project needed
them — and stayed there, buried, when that project ended. The boot picker's
design goes back to NextPi in 2018, and the ideas under it are older still.
More than a decade of the same wheels, re-cut.

rapi-bootloader is that accumulation, dug out, cleaned up, and put somewhere
public. It gives the pieces one home and the argument-passing ABI a single
owner, so that the next project can depend on them rather than write them
again. Nothing here is specific to the payload that finally prompted the
collection: any Circle kernel carrying the 0x800 block can be pushed, stamped
and booted by these loaders — and any Circle kernel WITHOUT one can be booted
too, unstamped. The block is an optional ABI, so a loader here boots a plain
Circle kernel as readily as an argv-taking one.

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
block's own `Capacity` field. The refusal is of the WRITE only: an image
with no block still boots, unstamped. Loaders do not decide this for
themselves — `BootImageWithDefaults()` in `defaultsblock/` stamps where it
can and chain-boots either way, so every loader answers the question the same
way and a new one inherits the answer. The consuming kernel tokenises `Text[]` and
appends it to its own argv.

The struct is `PACKED` and little-endian (matching AArch64), so its `sizeof`
is `4 + 2 + 2 + Capacity`. An image must be at least `0x800 + sizeof` bytes
long before any writer may dereference the block. AArch64 kernels load at
`MEM_KERNEL_START` (`0x80000`), so the block's runtime address is `0x80800`;
the image's first instruction is a `b` trampoline over the reserved boot
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

Writing it — verify the magic, enforce the block's own `Capacity`, write
`Text` NUL-terminated, update `Length` (excluding the NUL). That is the whole
contract:

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

An empty string clears the block: the consuming kernel then appends nothing and
boots its own default behaviour, so "unpatched" is not a special case.

## The shared parts

A loader in this repository decides: where a payload kernel comes
from, and which argument string goes into it. The network-loader receives the
payload over the network; the menu-loader reads it from the SD card and asks a
person to choose. Everything below that decision is identical, and lives in
directories that every loader uses:

- `defaultsblock/` — the 0x800 argument-block writer described above.
- `chainboot/` — placing a payload in memory where it can be received and
  copied from, and then replacing the running loader with it.

A loader gets both by including `mk/commons.mk` in its makefile and adding
`$(COMMON_OBJS)` to its object list. This is one line rather than a copied
set of build rules, because a loader that wires the shared parts by hand can
miss one without the build failing.

### Chain-boot on the Raspberry Pi 5

On the Raspberry Pi 3 and Pi 4, Circle's own chain-boot is correct and is
what those builds use. On the Pi 5 it cannot work, for separate
reasons:

1. Circle switches the data cache off and then runs compiled C++ that uses
   the stack. The Pi 5's Cortex-A76 core does not tolerate that order; the
   Pi 4's Cortex-A72 does.
2. Circle places its copy routine at address `0x7FC00`. On the Pi 5 that
   address is inside Trusted Firmware, which occupies `0x1000` to `0x80000`
   and stays in memory to start the payload's other CPU cores.
3. The Pi 5 has a memory-side system cache that set/way cache maintenance
   does not reach. Anything the loader leaves in it is stale data for a
   payload that writes its early state with the memory management unit off
   and reads it back with the unit on. Maintenance by virtual address is
   honoured, and is what the replacement uses.

`chainboot/rapi_chainboot.cpp` addresses all of them by defining Circle's
chain-boot functions itself on Pi 5 builds. A library member is only linked
when it resolves a symbol nothing else defines, so defining them here keeps
Circle's version out of the build entirely. On the Pi 3 and Pi 4 the file
defines none of them and Circle's version is linked as usual. The Circle tree
itself is never modified.

The one thing a loader must do is call `RapiChainBootMainReturning()` from
`CKernel::Run()`, immediately before it returns `ShutdownReboot`. On the
Pi 5 this marks the point after which the hand-off is safe to perform; on the
other boards it does nothing, so a loader calls it without testing which
board it is built for.

## Layout

```
defaultsblock/   the shared 0x800 ABI (writer side): PatchDefaults()
chainboot/       the shared chain-boot: staging a payload, and replacing
                 the running loader with it (see below)
network-loader/  the TFTP/HTTP/WebDAV chain-loader  (see its README)
menu-loader/     the on-card boot picker            (see its README)
mk/commons.mk    one include that gives a loader both shared parts
mk/ld/           TLS-adjacent linker script + link rule (see below)
circle-stdlib-rpi{3,4,5}/  submodules: one single-core C/C++ runtime world
                 per board (RASPPI 3/4/5); a loader links the selected one
dist/            collected per-board loader images (make dist)
```

## Keyboard layout

The menu-loader reads keyboard input to take a choice from the boot menu, so the keyboard layout must be configured correctly. The SD card's `cmdline.txt` carries this setting, which Circle reads at boot. It defaults to US; to match a different keyboard, add `keymap=` to the line:

    keymap=uk

Valid values are: `us` (default), `uk`, `de`, `es`, `fr`, `it`, `dv` (Dvorak). Without the correct layout, arrow keys and number keys produce the wrong characters and the boot menu becomes hard to navigate. See [`menu-loader/README.md`](menu-loader/README.md#keyboard-layout) for details.

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
make deps                 # fetch + build every single-core world (RASPPI 3/4/5)
make -j loaders           # every loader x every board, concurrently -> dist/
# or one combo at a time:
make network-loader-rpi4  # just the Pi 4 chain-loader, in its own build tree
```

`make deps` initialises the `circle-stdlib-rpi{3,4,5}` world submodules
(each pinned to the project's tested commit), clones the immutable-tagged
LLVM/libc++ checkout each builds libc++ from, then configures every world
**single-core** (`-r <board> -p aarch64-none-elf- --libcxx-repo
--kernel-max-size 256`) and builds it.

`make -j loaders` builds each (loader, board) in its own out-of-tree directory
`<loader>/build/<board>/` — isolated objects and a distinctly-named image
(`kernel8.img` / `kernel8-rpi4.img` / `kernel_2712.img`) — so all combos build
concurrently with zero collision, then collects the images into
`dist/<loader>/`. Every supported board maps onto one of these worlds
(Zero 2 W + CM3 → rpi3, Pi 4 + CM4 + Pi 400 → rpi4, Pi 5 + CM5 → rpi5); the
board→image routing lives in the card's `config.txt`. This per-board,
concurrent-tree methodology is the one the pi-mame split reuses to dispatch
concurrent CI build targets.

## Licence

GPLv3 — see `LICENSE`. `mk/ld/circle-tls.ld` is derived from Circle
(GPLv3, © R. Stange).
