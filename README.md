# NaOS

A usable x86_64 UEFI operating system built **from scratch (0), in C** — no GRUB, no Limine, no gnu-efi, no prebuilt bootloader, kernel, or libc. Own `BOOTX64.EFI`, own `KERNEL.ELF`, own drivers, own stack.

> Rule #1: **zero prebuilt OS code, ever.** Host toolchain (gcc, nasm, qemu, OVMF firmware) is allowed — everything under `boot/` and `kernel/` is ours.

## Versions

Each version is a **frozen, self-contained snapshot**: own source, Makefile, tools, docs, and changelog. Work happens inside one version folder at a time; finished versions are never touched again.

| Version | Status | Highlights |
|---|---|---|
| [v0.1.0](versions/v0.1.0/) | ✅ current — verified on QEMU | `NaOS>` shell, FAT32 read/write, multitasking + `fork()`, USB keyboard, UDP/DHCP/TCP + HTTP `fetch`, AHCI/NVMe/ATA disks |

Per-version docs live inside the folder: [v0.1.0 README](versions/v0.1.0/README.md) (build + commands), [IDEA](versions/v0.1.0/IDEA.md) (vision + phases), [CHANGELOG](versions/v0.1.0/CHANGELOG.md) (every change).

## Build & run (every session)

```bash
cd versions/v0.1.0
export PATH=/opt/cross/bin:$PATH
./tools/check.sh     # verify every tool + OVMF firmware
make all             # build/BOOTX64.EFI + build/KERNEL.ELF
make esp             # build/NaOS.img — 64M FAT ESP image
make run             # QEMU + OVMF, boots to the NaOS> shell
```

One-time host setup (Windows + MSYS2 UCRT64, QEMU via `winget`, rest via `pacman`, cross-compiler in `/opt/cross`) is documented in the [v0.1.0 README](versions/v0.1.0/README.md#one-time-setup-msys2-ucrt64).

## Layout (root)

```
README.md            this file — project hub (global, not per-version)
LICENCE.md           MIT licence for the whole project
index.html           project website (vanilla HTML/CSS/JS, offline-safe)
versions/
  v0.1.0/            frozen v0.1.0 snapshot: Makefile, boot/, kernel/,
                     common/, progs/, tools/, docs, build/ (generated)
```

## Rules (global)

* OS code is always ours; host toolchain is allowed.
* Every change is logged in that version's `CHANGELOG.md`.
* Version stamp: `VERSION` in the version's Makefile → `build/version.h`; the shell `version` command prints it.
* License: [LICENCE.md](LICENCE.md) (MIT).
