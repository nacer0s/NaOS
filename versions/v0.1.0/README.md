# NaOS v0.1.0

A usable x86_64 UEFI operating system built **from scratch (0), in C** — no GRUB, no Limine, no gnu-efi, no prebuilt bootloader or kernel. See [IDEA.md](IDEA.md) for the full vision, tree, flow and workflow.

Status: **v0.1.0** (multitasking + isolated address spaces, files, USB, network, NVMe). `BOOTX64.EFI` loads `KERNEL.ELF`, and the kernel runs
a `NaOS>` shell on a black GOP console with PS/2 and USB-HID keyboard input
(`help version clear print memory test crash list go read make-folder make-file copy move rename edit delete run tasks kill time date uptime beep pci settings usb nvme net dhcp fetch ping reboot shutdown`), own paging (PML4),
a `kmalloc`/`kfree` heap, an AHCI/NVMe/ATA+FAT32 filesystem (read+write, long filenames), and preemptive multitasking
(ring-3 tasks with private address spaces, syscalls, `fork()` with copy-on-write, round-robin scheduler,
disk-loaded ELF programs with file I/O (`open`/`read`/`write`/`close`, stdout on fd 1)). Command output redirects with `<cmd> > <file>`, and `/SETTINGS.TXT`
styles the prompt and console colors. Networking (E1000 ARP + ICMP ping, UDP sockets, DHCP client,
TCP client with HTTP `fetch`, default 10.0.2.15/24)
works under QEMU user-net: add `-netdev user,id=n0 -device e1000,netdev=n0` to `make run`,
then `net`, `dhcp`, `ping 10.0.2.2`, and `fetch 10.0.2.2 8099 /path` (needs an HTTP server on the host).

## Requirements

| Tool | Version verified | Purpose |
|---|---|---|
| QEMU + OVMF (`edk2-x86_64-code.fd`) | 11.1.0 | Emulator + UEFI firmware |
| NASM | 3.02 | `kernel/entry.asm`, ISR stubs |
| GNU Make | 4.4.1 | Build |
| `x86_64-w64-mingw32-gcc` | 16.2.0 | Bootloader `BOOTX64.EFI` (PE+, correct tool for EFI) |
| `x86_64-elf-gcc` + `ld` | 16.2.0 / 2.47 (`/opt/cross`) | Kernel `KERNEL.ELF` (ELF64) |
| mtools (`mformat/mcopy`) + dosfstools | 4.0.49 / 4.2 | Build FAT ESP image |
| Python + Pillow + DejaVu Sans Mono | 3.14.8 / 12.3.0 / 2.37 | Rasterize console font at build time |
| xorriso | 1.5.8.2-1 | Build bootable ISO (`make iso`) |
| Python | 3.14.8 | Helper scripts |

All on Windows + **MSYS2 UCRT64**. QEMU installed via `winget`, rest via `pacman` + cross-compiler built from source per https://wiki.osdev.org/GCC_Cross-Compiler.

## One-time setup (MSYS2 UCRT64)

```bash
pacman -Syu
pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
# at "Enter a selection (default=all):" just press Enter
pacman -S --needed nasm make python dosfstools mingw-w64-ucrt-x86_64-mtools mingw-w64-ucrt-x86_64-nasm
pacman -S --needed mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-ttf-dejavu
pacman -S --needed xorriso   # only for `make iso` (real-hardware image)

# cross compiler (already built 2026-10-09 → /opt/cross, ~30min; reference: tools/build-cross.sh)
# binutils-2.47 + gcc-16.2.0, --target=x86_64-elf --prefix=/opt/cross --without-headers
echo 'export PATH=/opt/cross/bin:$PATH' >> ~/.bashrc
export PATH=/opt/cross/bin:$PATH
x86_64-elf-gcc --version
```

## Quickstart (every session)

```bash
export PATH=/opt/cross/bin:$PATH
./tools/check.sh     # verify every tool + OVMF firmware
make all             # build/BOOTX64.EFI + build/KERNEL.ELF
make esp             # build/NaOS.img — 64M FAT with \EFI\BOOT\BOOTX64.EFI + \NaOS\KERNEL.ELF
make run             # QEMU + OVMF, boots to kernel banner
make clean           # full rebuild (also needed automatically via header deps)
```

QEMU command used by `make run`:

```
qemu-system-x86_64 -drive if=pflash,format=raw,unit=0,file="<...>/edk2-x86_64-code.fd",readonly=on -drive format=raw,file=build/NaOS.img -m 256M -serial stdio
```

## ISO + real hardware

```bash
make iso      # build/NaOS.iso — El Torito UEFI boot image (needs xorriso)
make run-iso  # boot the ISO itself in QEMU (-cdrom) to verify it
```

Note: a CD-booted system runs **diskless** — ATAPI CD-ROM and USB mass storage are v0.2.0 work, so `list` reports `fat: not ready`. The shell itself boots fine and every non-disk command works. Full files need a SATA/NVMe/IDE disk (AHCI → NVMe → ATA).

Flash to USB (Windows: [Rufus](https://rufus.ie) in **DD mode**; Linux: `dd if=build/NaOS.iso of=/dev/sdX bs=4M status=progress`), then boot the stick as a **UEFI** device.

You must know before trying:

* **Secure Boot OFF.** Our `BOOTX64.EFI` is unsigned (no Microsoft keys); enable setup mode or disable Secure Boot in firmware.
* **UEFI only, 64-bit.** No legacy BIOS/CSM support, by design.
* **Honest hardware limits (v0.1.0):** the disk driver speaks AHCI first (port 0, polling DMA), then NVMe (one I/O queue pair, polled, 512B namespaces), with a legacy IDE PIO fallback; keyboards work over PS/2 and USB HID (XHCI, polled). Any SATA/NVMe disk plus a PS/2 or USB keyboard for input covers most machines.
* Writing files on real hardware writes the USB stick's FAT partition — back it up first.

## Layout

```
boot/uefi/    own uefi.h + efi_main + ELF/GOP/file/exit-services loader (no gnu-efi)
common/       bootinfo handoff, serial, cstring, io — shared, freestanding
kernel/       entry.asm (MS→SysV RCX shim) + link.ld + kmain + fb/kprintf/font
progs/        user programs (ELF binaries for ring 3) + crt0 + user link script
tools/        check.sh, mkesp.sh, mkiso.sh, font_render.py, build-cross.sh
build/        generated, gitignored (binaries, ESP image, ISO, font header)
index.html    v0.1.0 website (vanilla HTML/CSS/JS, offline-safe)
```

## Rules

* **Zero prebuilt OS code.** Host toolchain is allowed; OS code is always ours.
* Log **every** change in [CHANGELOG.md](CHANGELOG.md).
* Version stamp: `VERSION` in the Makefile → `build/version.h`; `version` prints it.
* License: [LICENCE.md](../../LICENCE.md) (MIT, project-wide).
