# NaOS

A usable x86_64 UEFI operating system built **from scratch (0), in C** — no GRUB, no Limine, no gnu-efi, no prebuilt bootloader, kernel, or libc. Own `BOOTX64.EFI`, own `KERNEL.ELF`, own drivers, own stack.

> Rule #1: **zero prebuilt OS code, ever.** Host toolchain (gcc, nasm, qemu, OVMF firmware) is allowed — everything under `boot/` and `kernel/` is ours.

## Download — try it without building

* **Bootable ISO:** [NaOS.v0.1.0.iso](https://github.com/nacer0s/NaOS/releases/download/v0.1.0/NaOS.v0.1.0.iso) (68MB, UEFI, from the [v0.1.0 release](https://github.com/nacer0s/NaOS/releases)) — flash with Rufus in **DD mode**, boot as a UEFI device with **Secure Boot off**. CD/USB boot runs a **live RAM session** (files work, writes vanish on reboot); persistence needs a SATA/NVMe disk.
* **Raw disk image:** [NaOS.v0.1.0.img](https://github.com/nacer0s/NaOS/releases/download/v0.1.0/NaOS.v0.1.0.img) (64MB FAT) — attach as a VM hard disk (IDE/SATA: boots **with files**) or `dd` to USB.
* **Read the code online:** [file viewer](https://naos.nacer0s.com/browse.html) — browse every source file in your browser, no clone needed.
* **Project website:** <https://naos.nacer0s.com> (this repo's `index.html`, served by GitHub Pages).

## Versions

Each version is a **frozen, self-contained snapshot**: own source, Makefile, tools, docs, and changelog. Work happens inside one version folder at a time; finished versions are never touched again.

| Version | Status | Highlights |
|---|---|---|
| [v0.1.0](versions/v0.1.0/) | ✅ current — verified on QEMU | `NaOS>` shell (34 commands), FAT32 read/write, multitasking + `fork()`, USB keyboard, UDP/DHCP/TCP + HTTP `fetch`, AHCI/NVMe/ATA disks, ISO export |

Per-version docs live inside the folder: [v0.1.0 README](versions/v0.1.0/README.md) (build + commands), [IDEA](versions/v0.1.0/IDEA.md) (vision + phases), [CHANGELOG](versions/v0.1.0/CHANGELOG.md) (every change). Version numbers are short SemVer (`VERSION` in each Makefile); `version` prints the build date on its own line.

## Numbers (v0.1.0, verified)

| What | Value |
|---|---|
| Shell commands | 39, plain English |
| Syscalls | 10 (`int 0x80`, #0–9: exit/print/yield/getpid/sleep/fork/open/read/write/close) |
| Tasks / fds | 16 slots, 50ms quantum @100Hz · 8 fds per task, opens from 3 |
| Kernel / user bases | `0x100000` / `0x40100000` · PML4 identity 0–1G |
| BOOTX64.EFI / KERNEL.ELF | 15,656 B / 150,848 B |
| ESP / ISO / RAM disk / DATA.NAFS | 64MiB / ~67.7MiB / 40MiB / 61,952 B |
| Default network | 10.0.2.15/24 via 10.0.2.2, DNS 10.0.2.3 (QEMU user-net) |
| Fonts | 12x24 + 8x16 bitmaps, scales 1–4 · 256-line scrollback ring |

## Build & run (every session)

```bash
cd versions/v0.1.0
export PATH=/opt/cross/bin:$PATH
./tools/check.sh     # verify every tool + OVMF firmware
make all             # build/BOOTX64.EFI + build/KERNEL.ELF
make esp             # build/NaOS.v0.1.0.img — 64M FAT ESP image
make run             # QEMU + OVMF, boots to the NaOS> shell
make iso             # build/NaOS.v0.1.0.iso (needs xorriso)
make run-iso         # boot the ISO itself in QEMU to verify it
```

One-time host setup (Windows + MSYS2 UCRT64, QEMU via `winget`, rest via `pacman`, cross-compiler in `/opt/cross`) is documented in the [v0.1.0 README](versions/v0.1.0/README.md#one-time-setup-msys2-ucrt64).

## Layout (root)

```
README.md            this file — project hub (global, not per-version)
LICENCE.md           custom licence for the whole project (source-available, non-commercial)
SECURITY.md          security policy: supported versions + how to report
index.html           root redirect → home/ (meta + JS + manual link)
home/index.html      project website (dossier design, offline-safe)
home/browse/index.html in-browser source viewer (GitHub API, no install)
home/activity/index.html live repo track: commits, releases, download counts
404.html             website "lost sector" page (must stay at root for Pages)
versions/
  v0.1.0/            frozen v0.1.0 snapshot: Makefile, boot/, kernel/,
                     common/, progs/, tools/, docs, website, build/ (generated)
```

## Rules (global)

* OS code is always ours; host toolchain is allowed.
* Every change is logged in that version's `CHANGELOG.md`.
* Version stamp: `VERSION` in the version's Makefile → `build/version.h`; the shell `version` command prints it.
* Licence: [LICENCE.md](LICENCE.md) — free for personal/learning use, **no commercial use, no re-hosting as your own, credit us everywhere**.

## Author — nacer0s

NaOS is designed, written and tested by **nacer0s**. Follow the build:

* GitHub: <https://github.com/nacer0s> · Web: <https://nacer0s.com>
* X: <https://x.com/nacer0s> · Instagram: <https://instagram.com/nacer0s>
* TikTok: <https://tiktok.com/@nacer0s> · Telegram: <https://t.me/nacer0s>
* Threads: @nacer0s · Mortakaz: @nacer0s
