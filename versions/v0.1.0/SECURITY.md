# Security Notes — NaOS v0.1.0

Global policy (how to report, response times): [../../SECURITY.md](../../SECURITY.md).
Rule: report privately, get credited publicly.

## What's new (and least audited) in v0.1.0

- **Network parsers** (`kernel/net/`): ARP/IP/ICMP/UDP/TCP/DHCP read
  untrusted bytes from the wire. Lengths are bounds-checked and TCP sequence
  math is wrap-aware, but this is the youngest code — fuzz it mentally first.
- **USB descriptors** (`kernel/drv/xhci.c`, `usb_kbd.c`): HID reports are
  length-checked; exotic devices are ignored, not parsed.
- **Storage input** (`ahci.c`, `nvme.c`, `ata.c`, `fat32.c`): IDENTIFY data,
  PCI BARs and FAT structures come from the (possibly hostile) disk or image.
  A malicious image can crash the boot — treat images like executables.
- **File parsers**: `/SETTINGS.TXT` (bounded 1KB read, strict `key=value`
  parse), ELF loader (validate-then-map), font bitmaps (build-time only).
- **No TLS**: `fetch`/DHCP/DNS-less networking is plaintext; do not fetch
  secrets, and do not trust fetched bytes for anything but display.

## Hardening already present

- Ring-3 isolation with per-task page tables; faults kill the task, not the kernel.
- Syscall pointer validation (`user_range_ok`, ≤2048B per call); forked
  children start fd-less; CoW write-fault privatizer.
- `ExitBootServices` exactly once; bootloader dies loudly (halt) on any error
  instead of continuing half-built.
- `-Wall -Wextra -Werror` on all kernel/boot code; every fix logged below.

## Known gaps (accepted for v0.1.0, tracked for later)

- No ASLR / stack protector / W^X enforcement.
- No crypto: no signed updates, no HTTPS, hash-then-trust only.
- FAT driver assumes a cooperative volume (no fsck, no journal).
- Flood-while-scrolled rejoins live (availability over history) by design.

If you break any of the above, that is a report, not an insult — thank you.
See [CHANGELOG.md](CHANGELOG.md) for the fix history so far.
