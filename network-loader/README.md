# network-loader

network-loader is a development chain-loader for bare-metal Raspberry Pi
payloads. It brings up the network on a static or DHCP-assigned address and
serves TFTP, HTTP and WebDAV. It receives a kernel image over the network
into a staging buffer, optionally stamps an argv
[defaults-string](../README.md#the-0x800-defaults-block-abi) into it, and
chain-boots it. Push a new image and it runs from RAM, so nothing rewrites
the SD card between iterations.

The chain-boot carries the device tree over to the image it boots, so a
chain-booted image has a working network on every board. This matters most
on the Raspberry Pi 5, where the Ethernet driver has no other source for the
adapter's hardware address. Without the device tree, networking does not
start.

Part of [rapi-bootloader](../README.md). Build with `make network-loader`
from the repo root, after `make deps`. It builds one image per board into
`network-loader/build/<board>/`, using Circle's own names: `kernel8.img`,
`kernel8-rpi4.img` and `kernel_2712.img` for RASPPI 3, 4 and 5. The loader
is single-core by design. Circle's `EnableChainBoot()` refuses a multicore
build, and the Raspberry Pi 5 chain-boot in
[`chainboot/`](../chainboot/) keeps the same restriction.

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
firmware skips any section whose name it does not recognise, so this
section is invisible to it, which is why the loader's settings can live in
the file you are already editing. **Keep it as the last section in the
file.** Every setting below a section heading belongs to that section, so a
firmware setting placed after this one would be skipped along with it.

Only `ipaddress` is required for a static address. `netmask` defaults to
255.255.255.0. A `gateway` is needed only to reach the loader from a
different network segment.

`dnsserver` names the resolver that looks up a host name, and it is
optional in the same way the gateway is. **A static address is configured
by hand, and so is its resolver.** DHCP hands out a resolver along with the
address. A configuration written on the card has only what the card states.
Name a resolver here and anything that boots from this section can use host
names; name none and it uses addresses. Both are ordinary, and neither is a
lesser version of the other.

The loader itself looks up no names, so this setting changes nothing the
loader does. It is here because the section is one definition of what a
card says about the network, and the payload the loader chain-boots reads
the same file. A payload asking a time server for the clock by name is the
case that needs it.

If no address is configured and no DHCP server answers, the loader keeps
asking and says so. There is nothing else for it to do until it has an
address.

## Interfaces

Three ports, each a distinct job over the SD card and the chain-boot path.

### TFTP (the chain-boot push)

The loader accepts a kernel-image write only when the filename matches
`kernel*.img`, a `kernel` prefix and a `.img` suffix. It refuses any other
name at open, before a byte transfers, so a mis-named local copy (say
`commodore-core.img`) fails fast instead of half-uploading. A received
`kernel*.img` is staged in a buffer clear of the copy region, so it can
never overlap its own copy destination at the kernel load address. The
loader uses the high heap, above 1 GB, when the board has one with room,
and falls back to the low heap otherwise. It then stamps the image with any
pending defaults-string and chain-boots it. The loader boots an image
carrying no 0x800 block unstamped rather than refusing it, so any Circle
kernel can be pushed here.

Two reserved TFTP names are not images:

- `inject` arms the next push's defaults-string, the argv text stamped into
  the image's 0x800 block before it boots. It is one-shot; the loader
  disarms it after use.
- `sd/<path>` writes a file onto the SD card instead of chain-booting.

Push with any TFTP client, e.g.
`tftp -m binary 192.168.1.50 -c put kernel8-rpi4.img`, using whatever
address the loader reported at start-up.

### HTTP (port 8080)

The HTTP interface is a small web UI. Choose a `kernel*.img` to upload,
optionally type an ABI parameters string, the same argv text the TFTP
`inject` path stamps into the image's 0x800 block, and press *Boot now!*.
Leave the field blank for a plain boot. A `/reboot` endpoint is also
available.

### WebDAV (port 8081, class 1)

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

This is WebDAV class 1, so the server does not implement `LOCK`/`UNLOCK`,
the class-2 locking methods. `OPTIONS` advertises `DAV: 1`, so a client's
capability probe succeeds and it engages. Command-line and scripted
clients, such as `curl`, `cadaver` and davfs2, work fully. GUI file
managers that require class-2 locking to mount read-write, notably macOS
Finder, may mount read-only or decline the write instead. Mount from
Finder with ⌘K → `http://<loader-address>:8081/`.
