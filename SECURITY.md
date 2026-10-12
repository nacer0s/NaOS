# Security Policy — NaOS

## Supported versions

| Version | Status |
|---|---|
| [v0.1.0](versions/v0.1.0/) | ✅ supported — receives fixes |
| older / pre-release snapshots | ❌ unsupported |

Each release supersedes the last. Check `version` in the shell or the [releases page](https://github.com/nacer0s/NaOS/releases).

## Reporting a vulnerability

1. **Email:** nacer.eddine.dev@gmail.com with subject `NaOS security`, **or**
   GitHub → *Security* → *Report a vulnerability* (private, preferred).
2. Include: NaOS version (`version` output), where it runs (QEMU / VirtualBox /
   real hardware), steps to reproduce or a PoC, and your guess at impact.
3. I acknowledge within **3 days**, fix in the newest development version,
   credit you in that version's `CHANGELOG.md` (unless you decline), and
   backport only to supported versions.
4. Please **do not open public issues** for unpatched holes — give me the
   3 days first.

## Honest scope (read before reporting)

NaOS v0.1.0 is a hobby OS, not a hardened production system:

- **No memory exploit mitigations:** no ASLR, no stack protector
  (`-fno-stack-protector`), no W^X distinction beyond ring separation.
- **No cryptography:** no TLS/HTTPS, no signatures yet — network input and
  update files are unauthenticated by design (for now).
- **No users or auth:** anyone at the keyboard (or on the LAN, for ICMP/UDP)
  is trusted; FAT volumes are trusted.
- **Most likely bug class:** malformed-packet/device-input crashes in the new
  parsers (ARP/IP/UDP/TCP/DHCP, USB descriptors, PCI BARs, FAT structures).
  These are in scope and welcome — a QEMU repro is perfect.
- **Out of scope:** bugs in EDK2/OVMF, QEMU, or VirtualBox themselves
  (report those upstream); social engineering; attacks needing physical
  access beyond normal use.

Per-version attack-surface notes live next to the code:
[versions/v0.1.0/SECURITY.md](versions/v0.1.0/SECURITY.md).
