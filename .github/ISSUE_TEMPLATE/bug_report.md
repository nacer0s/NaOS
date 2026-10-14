---
name: Bug report
about: Report something broken in NaOS (wrong output, hang, crash, boot failure)
title: ''
labels: ''
assignees: ''

---

**Describe the bug**
A clear and concise description of what is broken. One problem per issue.

**NaOS version**
Output of the `version` command (e.g. `NaOS v0.1.0`), or the ISO/IMG file
name and date you booted.

**Where it runs**
- [ ] QEMU (command line you used + OVMF path)
- [ ] VirtualBox (machine type — must be 64-bit — EFI on/off)
- [ ] Real hardware (board/CPU, disk, keyboard, UEFI or legacy, Secure Boot state)

**To Reproduce**
Commands typed, in order (e.g. `list`, `run /PROGS/hello.elf`):
1. ...
2. ...

**Boot messages (for boot/file/disk bugs)**
Press `PgUp` to scroll back and copy the relevant lines
(`ramdisk:`, `blk:`, `fat:`, `ahci:`/`nvme:`, ...), or attach a photo.

**Expected behavior**
What you expected to happen.

**Photos / serial log**
A screen photo or the QEMU serial log (`-serial file:boot.txt`) helps
more than anything for firmware and driver bugs.

**Additional context**
Anything else: did it work on an older ISO? Does it work in QEMU but not
on hardware (or vice versa)?

**Security bugs:** do NOT open a public issue — see SECURITY.md (report to
nacer.eddine.dev@gmail.com or contact@nacer0s.com first).
