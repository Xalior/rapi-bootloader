# network-loader

A development chain-loader for bare-metal Raspberry Pi 4 payloads. It brings
up the network on a static IP, serves TFTP + HTTP + WebDAV, receives a kernel
image over the wire into a high-heap staging buffer, optionally stamps an argv
[defaults-string](../README.md#the-0x800-defaults-block-abi) into it, and
chain-boots it — iteration without rewriting the SD card: push a new image and
it runs from RAM.

The chain-boot carries the device tree over to the image it boots, so a
chain-booted image has a working network on every board. (Without that, on the
Raspberry Pi 5 the Ethernet driver has no source for the adapter's hardware
address and networking does not start.)

Part of [rapi-bootloader](../README.md). Build with `make network-loader` from
the repo root (after `make deps`); the image is `network-loader/kernel8-rpi4.img`.
Single-core by design — Circle's `EnableChainBoot()` refuses a multicore
build, and the Raspberry Pi 5 chain-boot in [`chainboot/`](../chainboot/)
keeps the same restriction.

## Network

The loader takes its address from the SD card, in a `[rapi-bootloader]`
section of `config.txt`:

    [rapi-bootloader]
    ipaddress=192.168.1.50
    netmask=255.255.255.0
    gateway=192.168.1.1
    dnsserver=192.168.1.1

State an address there and the loader uses it. State none, or leave the
section out, and the loader asks DHCP for one and reports what it was given on
its screen and its serial log. There is no built-in address.

`config.txt` is the Raspberry Pi firmware's own boot configuration file. The
firmware skips any section whose name it does not recognise, so this section is
invisible to it, which is why the loader's settings can live in the file you
are already editing. **Keep it as the last section in the file**: every setting
below a section heading belongs to that section, so a firmware setting placed
after this one would be skipped along with it.

Only `ipaddress` is required for a static address. `netmask` defaults to
255.255.255.0. A `gateway` is needed only to reach the loader from a different
network segment.

`dnsserver` is the resolver a name is looked up at, and it is optional in the
same way the gateway is. **A static address is configured by hand, and so is
its resolver**: DHCP hands out a resolver along with the address, and a
configuration written on the card has only what the card states. Name one here
and anything that boots from this section can use host names; name none and it
uses addresses. Both are ordinary, and neither is a lesser version of the
other.

The loader itself looks up no names, so this setting changes nothing the
loader does. It is here because the section is one definition of what a card
says about the network, and the payload the loader chain-boots reads the same
file — a payload asking a time server for the clock by name is the case that
needs it.

If no address is configured and no DHCP server answers, the loader keeps
asking and says so; there is nothing else for it to do until it has an
address.

## Interfaces

Three ports, each a distinct job over the SD card and the chain-boot path.

### TFTP — the chain-boot push

A kernel-image write is accepted **only when the filename matches
`kernel*.img`** — a `kernel` prefix and a `.img` suffix. Any other name is
refused at open, before a byte transfers, so a mis-named local copy (say
`commodore-core.img`) fails fast instead of half-uploading. A received
`kernel*.img` is staged in the **high heap** (above 1 GB, so a MAME-sized
image can never overlap its own copy destination at the kernel load address),
stamped with any pending defaults-string, then chain-booted. An image with no
0x800 block is booted unstamped rather than refused, so any Circle kernel can
be pushed here.

Two reserved TFTP names are not images:

- `inject` — arms the next push's defaults-string: the argv text stamped into
  the image's 0x800 block before it boots (one-shot; disarmed after use).
- `sd/<path>` — writes a file onto the SD card instead of chain-booting.

Push with any TFTP client, e.g.
`tftp -m binary 192.168.1.50 -c put kernel8-rpi4.img`, using whatever
address the loader reported at start-up.

### HTTP — port 8080

A small web UI: choose a `kernel*.img` to upload, optionally type an **ABI
parameters** string (the argv text stamped into the image's 0x800 block —
the same injection the TFTP `inject` path performs; leave it blank for a plain
boot), and press *Boot now!*. Plus a `/reboot` endpoint.

### WebDAV — port 8081 (class 1)

Read/write access to the whole SD card (the DAV root) from a WebDAV client at
`http://<loader-address>:8081/`. Implemented methods:

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
`http://<loader-address>:8081/`.
