# rapi-bootloader

A single-core boot building block for bare-metal Raspberry Pi 4 payloads
built on the [Circle](https://github.com/rsta2/circle) framework. It is two
small argv-loaders that stamp a plain-text argument string into a kernel
image at a fixed offset and chain-boot it:

- **network-loader** — a development chain-loader. It brings up the network,
  serves TFTP, HTTP (port 8080) and WebDAV (port 8081), receives a kernel
  image over the wire into a high-heap staging buffer, optionally stamps an
  argv defaults-string into it, and chain-boots it. Reflash-free iteration:
  push a new image, it runs in RAM. See
  [`network-loader/README.md`](network-loader/README.md).
- **menu-loader** — an on-card boot picker. It reads a `bootmenu.cfg` list
  from the SD card, presents it on screen, takes a keyboard selection,
  stamps the chosen defaults-string into a staged platform kernel image, and
  chain-boots it. See [`menu-loader/README.md`](menu-loader/README.md).

Both are **single-core** by design: Circle's `EnableChainBoot()` refuses a
multicore build, so the boot world is configured without
`ARM_ALLOW_MULTI_CORE`.

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
and booted by these loaders.

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

## Layout

```
defaultsblock/   the shared 0x800 ABI (writer side): PatchDefaults()
network-loader/  the TFTP/HTTP/WebDAV chain-loader  (see its README)
menu-loader/     the on-card boot picker            (see its README)
mk/ld/           TLS-adjacent linker script + link rule (see below)
circle-stdlib-rpi{3,4,5}/  submodules: one single-core C/C++ runtime world
                 per board (RASPPI 3/4/5); a loader links the selected one
dist/            collected per-board loader images (make dist)
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
make deps                 # fetch + build all three single-core worlds (RASPPI 3/4/5)
make -j loaders           # every loader x every board, concurrently -> dist/
# or one combo at a time:
make network-loader-rpi4  # just the Pi 4 chain-loader, in its own build tree
```

`make deps` initialises the three `circle-stdlib-rpi{3,4,5}` world submodules
(each pinned to the project's tested commit), clones the immutable-tagged
LLVM/libc++ checkout each builds libc++ from, then configures every world
**single-core** (`-r <board> -p aarch64-none-elf- --libcxx-repo
--kernel-max-size 256`) and builds it.

`make -j loaders` builds each (loader, board) in its own out-of-tree directory
`<loader>/build/<board>/` — isolated objects and a distinctly-named image
(`kernel8.img` / `kernel8-rpi4.img` / `kernel_2712.img`) — so all combos build
concurrently with zero collision, then collects the images into
`dist/<loader>/`. Every supported board maps onto one of the three worlds
(Zero 2 W + CM3 → rpi3, Pi 4 + CM4 + Pi 400 → rpi4, Pi 5 + CM5 → rpi5); the
board→image routing lives in the card's `config.txt`. This per-board,
concurrent-tree methodology is the one the pi-mame split reuses to dispatch
concurrent CI build targets.

## Licence

GPLv3 — see `LICENSE`. `mk/ld/circle-tls.ld` is derived from Circle
(GPLv3, © R. Stange).
