# network-loader

A development chain-loader for bare-metal Raspberry Pi 4 payloads. It brings
up the network on a static IP, serves TFTP + HTTP + WebDAV, receives a kernel
image over the wire into a high-heap staging buffer, optionally stamps an argv
[defaults-string](../README.md#the-0x800-defaults-block-abi) into it, and
chain-boots it — iteration without rewriting the SD card: push a new image and
it runs from RAM.

**On the Raspberry Pi 5, a chain-booted image cannot use the built-in
Ethernet.** The Pi 5 Ethernet driver reads the adapter's hardware address from
the device tree that the firmware passes at power-on, and a chain-booted image
is entered without it, so network start-up fails. Payloads that do not use the
network (an emulator, for example) chain-boot normally. The practical
consequence is that this loader cannot be used to test a *new build of itself*
in RAM on a Pi 5: a new loader has to be written to the SD card and the board
powered on.

Part of [rapi-bootloader](../README.md). Build with `make network-loader` from
the repo root (after `make deps`); the image is `network-loader/kernel8-rpi4.img`.
Single-core by design — Circle's `EnableChainBoot()` refuses a multicore
build, and the Raspberry Pi 5 chain-boot in [`chainboot/`](../chainboot/)
keeps the same restriction.

## Network

At startup the loader asks for an address by DHCP. If a server answers within
4 seconds, the loader uses the address, netmask, gateway and DNS server it is
given. If no server answers in that time, the loader uses the fixed address
**192.168.42.99** instead, so it also works on a network that hands out no
addresses at all. Edit the address octets at the top of `kernel.cpp` to change
the fixed address for your own network.

The loader only ever *requests* an address. It never hands addresses out to
other machines.

The 4-second limit is chosen because a DHCP server that is present answers in
a few milliseconds; the wait costs those 4 seconds only on a network where no
server exists.

The DHCP half of this has not yet been tested against a live DHCP server. The
fall back to the fixed address is the path in daily use.

## Interfaces

Three ports, each a distinct job over the SD card and the chain-boot path.

### TFTP — the chain-boot push

A kernel-image write is accepted **only when the filename matches
`kernel*.img`** — a `kernel` prefix and a `.img` suffix. Any other name is
refused at open, before a byte transfers, so a mis-named local copy (say
`commodore-core.img`) fails fast instead of half-uploading. A received
`kernel*.img` is staged in the **high heap** (above 1 GB, so a MAME-sized
image can never overlap its own copy destination at the kernel load address),
stamped with any pending defaults-string, then chain-booted.

Two reserved TFTP names are not images:

- `inject` — arms the next push's defaults-string: the argv text stamped into
  the image's 0x800 block before it boots (one-shot; disarmed after use).
- `sd/<path>` — writes a file onto the SD card instead of chain-booting.

Push with any TFTP client, e.g.
`tftp -m binary 192.168.42.99 -c put kernel8-rpi4.img`.

### HTTP — port 8080

A small web UI: choose a `kernel*.img` to upload, optionally type an **ABI
parameters** string (the argv text stamped into the image's 0x800 block —
the same injection the TFTP `inject` path performs; leave it blank for a plain
boot), and press *Boot now!*. Plus a `/reboot` endpoint.

### WebDAV — port 8081 (class 1)

Read/write access to the whole SD card (the DAV root) from a WebDAV client at
`http://192.168.42.99:8081/`. Implemented methods:

| Method | Purpose |
|--------|---------|
| `OPTIONS`      | class-1 capability advertisement (`DAV: 1`) |
| `PROPFIND`     | directory listing (Depth 0/1; `infinity` folds to 1) |
| `GET` / `HEAD` | read a file |
| `PUT`          | write a file (intermediate directories auto-created) |
| `DELETE`       | remove a file or empty directory (the root is refused) |
| `MKCOL`        | make a directory |

This is **WebDAV class 1**: it does **not** implement `LOCK`/`UNLOCK` (the
class-2 locking methods). `OPTIONS` advertises `DAV: 1` so a client's
capability probe succeeds and it will engage. Command-line and scripted
clients (`curl`, `cadaver`, davfs2) work fully. GUI file managers that require
class-2 locking to mount **read-write** — notably macOS Finder — may mount
read-only or decline the write. Mount from Finder with ⌘K →
`http://192.168.42.99:8081/`.
