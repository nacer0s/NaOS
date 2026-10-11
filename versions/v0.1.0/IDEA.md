# NaOS — IDEA.md

> NaOS: a usable operating system written from scratch (0), in C, named NaOS.
> Rule #1: **no prebuilt OS code ever** — no GRUB, no Limine, no gnu-efi, no EDK2 libs, no copied bootloader, no libc in kernel. Everything under `boot/` and `kernel/` is ours.
> Host tools (gcc, nasm, qemu, OVMF firmware) are allowed — otherwise nothing can be compiled.

## 1. Vision

Build a small but **real, usable** OS from zero:

* Boots on QEMU x86_64 via UEFI with our own `BOOTX64.EFI`.
* Loads our own `KERNEL.ELF` (ELF64, custom linker script).
* Brings up framebuffer console (GOP), serial, GDT/IDT, timer, PS/2 keyboard, physical memory manager, tiny `kmalloc`.
* Ends in a C shell: `NaOS>` with plain-English commands (`help`, `list`, `read`, ...).
* All in C (+ tiny NASM stubs for entry/ISR). No assembly kernel, no prebuilt boot.

v0.1 done = **power-on → UEFI → C kernel → shell prompt, typed commands work**.

## 2. Locked Decisions

| Decision | Choice | Why |
|---|---|---|
| CPU | x86_64 64-bit long mode | Modern, future-proof. UEFI already enters long mode so no mode-switch asm needed. |
| Firmware | UEFI (OVMF `edk2-x86_64-code.fd`) | Modern path. Harder than BIOS (no INT 0x10), but required for GPT/ESP/GOP. |
| Bootloader | 100% custom, C + own `uefi.h`, no GRUB fallback | Purity rule. We parse the UEFI spec ourselves. |
| Bootloader output | PE32+ `BOOTX64.EFI`, subsystem `EFI_APPLICATION`, entry `efi_main` | UEFI requirement. Built with `x86_64-w64-mingw32-gcc` (PE toolchain is correct here). |
| Kernel format | ELF64, entry `kernel_entry`, linked at higher-half stub → physical for v0.1 | Standard, parseable by our loader. Built with `x86_64-elf-gcc` (ELF toolchain). |
| Console | GOP framebuffer + embedded 8x16 font + serial COM1 for logs | UEFI has no VGA text `0xB8000`. We draw pixels ourselves. |
| Input | UEFI `SimpleTextInputEx` in bootloader; PS/2 0x60/0x64 driver in kernel | QEMU provides both. USB stack is out of scope for v0.1. |
| Build host | Windows + MSYS2 UCRT64 + `/opt/cross` | Native, verified. No WSL required. |
| Language | C11, `-ffreestanding -nostdlib`, own `string.c / vsprintf` | No hosted libc in kernel/bootloader. |

Verified host versions (2026-10-09):

* QEMU `11.1.0` + `edk2-x86_64-code.fd` present in `C:/Program Files/qemu/share`
* NASM `3.02`, GNU Make `4.4.1`
* `x86_64-w64-mingw32-gcc 16.2.0` (bootloader), `x86_64-elf-gcc 16.2.0` + `ld 2.47` (kernel, built from source per OSDev wiki)
* `mformat/mcopy` (mtools), `dosfstools`, `python 3.14.8`

## 3. Repository Tree

```
NaOS/
  IDEA.md            # this file: vision, tree, flow, workflow
  README.md          # GitHub front page: quickstart, commands
  CHANGELOG.md       # log of EVERY change (Keep a Changelog)
  LICENCE.md         # MIT (your spelling)
  .gitignore
  Makefile           # all / bootloader / kernel / esp / run / clean / check
  tools/
    check.sh         # verify gcc/nasm/mtools/python/qemu/OVMF/cross compiler
    mkesp.sh         # create 64M FAT ESP, copy BOOTX64.EFI + KERNEL.ELF (our logic)
    build-cross.sh   # reference: how /opt/cross was built (binutils+gcc)
  boot/uefi/
    uefi.h           # OWN UEFI types/spec structs (no gnu-efi) — EFI_SYSTEM_TABLE,
                     #   EFI_BOOT_SERVICES, SIMPLE_TEXT_OUTPUT, GOP, FILE_PROTOCOL
    main.c           # efi_main: ConOut hello → GOP query → load KERNEL.ELF → ExitBootServices → jump
    elf.c / elf.h    # minimal ELF64 parser (EHDR/PHDR, PT_LOAD copy)
    gop.c / file.c / mem.c  # phase 2+ (helpers, kept small)
    link.ld          # PE layout notes (mingw handles PE; ld script documents, not strictly needed)
  kernel/
    entry.asm        # NASM: stack, clear direction, call kmain, halt
    link.ld          # ELF: base 0x100000 (1M), .text/.rodata/.data/.bss, 4K aligned
    kmain.c          # kernel_entry(fb_info): init console/serial/gdt/idt/kbd/shell
    lib/string.c     # strlen/memcpy/memset/strcmp + vsprintf (own)
    drv/fb.c         # GOP framebuffer putpixel/putchar/scroll
    drv/serial.c     # COM1 0x3F8 logging
    drv/ps2_kbd.c    # scancode set-1 → ascii, polling
    drv/pit.c        # timer 100Hz (phase 4)
    cpu/gdt.c idt.c isr.asm isr.c io.h
    mm/pmm.c kmalloc.c
    shell.c          # NaOS> loop
    drv/fb.c           # 32bpp console: blits build-time font bitmaps (12x24)
  tools/
    font_render.py     # rasterizes installed DejaVu Sans Mono -> build/font_bitmap.h
  build/
    font_bitmap.h      # GENERATED 2-bit alpha glyphs (not committed)
  docs/
    boot-flow.md     # (phase 2) GOP + memory-map notes
    memory-map.md    # (phase 2) physical layout
  build/             # generated: BOOTX64.EFI, KERNEL.ELF, NaOS.img (gitignored)
```

## 4. Boot & Data Flow

```
QEMU -drive if=pflash,file=edk2-x86_64-code.fd -drive file=build/NaOS.img
 │
 ▼  UEFI firmware mounts ESP (FAT), executes \EFI\BOOT\BOOTX64.EFI
BOOTX64.EFI (our C, PE+):
  1. efi_main(ImageHandle, SystemTable)
  2. SystemTable->ConOut->OutputString("Hello NaOS UEFI\r\n")   [P1 milestone]
  3. BootServices->LocateProtocol(GOP) → framebuffer base/size  [P2+]
  4. SimpleFileSystem->OpenVolume → Open("\NaOS\KERNEL.ELF") → Read → parse ELF64
  5. AllocatePages for each PT_LOAD, memcpy segments, zero BSS
  6. GetMemoryMap → ExitBootServices (one-shot!) → build handoff struct
  7. call kernel_entry(fb_base, fb_w, fb_h, fb_pitch, memmap)    [P3]
      │
      ▼
KERNEL.ELF (our C+asm, ELF64, already in long mode):
  entry.asm: cli, mov rsp,stack_top, call kmain, hlt loop
  kmain: fb_clear → fb_puts("NaOS v0.1") → serial_init → gdt_init → idt_init
         → pit_init(100Hz) → ps2_kbd_init → pmm_init(uefi memmap) → shell_run()
  shell: print "NaOS> ", read line via kbd (echo to fb+serial),
         dispatch plain-English commands, repeat. Never returns.
```

Handoff struct (ours, not multiboot):

```c
typedef struct {
  uint64_t fb_base, fb_size;
  uint32_t fb_w, fb_h, fb_pitch, fb_bpp;
  uint64_t memmap_addr; uint64_t memmap_entries, memmap_entry_size;
  uint64_t rsdp;
} naos_bootinfo_t;
```

## 5. Memory Map (v0.1)

* `0x000000–0x09FFFF` low mem (UEFI-owned until ExitBootServices, do not touch early)
* `0x100000 (1M)` kernel ELF load base, `LOAD` segments copied here by loader
* Kernel stack: 64K reserved in `.bss` (`stack_top`), 16-byte aligned per SysV ABI
* Framebuffer: identity-mapped by firmware; we reuse mapping (no own paging in v0.1; own PML4 is phase 5+)
* PMM: free pages discovered from UEFI `MemoryMap` (`EfiConventionalMemory` only)

## 6. Workflow (how we work, every time)

```bash
# one-time (MSYS2 UCRT64, already done 2026-10-09 — kept here for README/repro):
pacman -Syu
pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
pacman -S --needed nasm make python dosfstools mingw-w64-ucrt-x86_64-mtools mingw-w64-ucrt-x86_64-nasm
# cross compiler from source → /opt/cross (binutils 2.47 + gcc 16.2.0, x86_64-elf)
echo 'export PATH=/opt/cross/bin:$PATH' >> ~/.bashrc

# every session:
export PATH=/opt/cross/bin:$PATH
./tools/check.sh            # fail fast if tool missing
make all                    # build/BOOTX64.EFI + build/KERNEL.ELF (when kernel lands)
make esp                    # build/NaOS.img (64M FAT, \EFI\BOOT\BOOTX64.EFI + \NaOS\KERNEL.ELF)
make run                    # qemu-system-x86_64 -bios edk2... -drive file=build/NaOS.img -serial stdio
make clean
```

`make run` QEMU flags (v0.1): `-drive if=pflash,format=raw,unit=0,file=<ovmf>,readonly=on -drive format=raw,file=build/NaOS.img -m 256M -serial stdio -display gtk` (pflash, not `-bios`, verified on winget QEMU 11.1.0 Windows build). OVMF path auto-detected: `C:/Program Files/qemu/share/edk2-x86_64-code.fd` → `/mingw64/share/qemu/...` → `qemu` relative.

File creation rule: ESP is built with `mformat -i build/NaOS.img -F -v NaOS ::` + `mcopy`, never by hand-copying a prebuilt image. Scripts are ours.

## 7. Phases (P0 done, P1 next)

* **P0 toolchain+docs DONE (2026-10-09):** MSYS2 packages, `/opt/cross` x86_64-elf 16.2.0, IDEA/README/CHANGELOG/LICENCE, Makefile skeleton, `check.sh`, hello-EFI that prints and halts.
* **P1 hello-UEFI DONE:** `BOOTX64.EFI` prints via ConOut, `make esp/run` boots to it in QEMU/OVMF.
* **P2 ELF loader + framebuffer kernel DONE (2026-10-09):** `elf.c` + `file.c` + GOP + ExitBootServices; kernel prints `NaOS v0.1 - kernel OK` on GOP + serial (screenshot-verified). Handoff: MS-ABI RCX → SysV RDI shim in `entry.asm`.
* **P3 console+serial:** `fb.c`, `serial.c`, `string.c`, embedded font; kernel log mirrors to COM1.
* **P4 interrupts+input DONE (2026-10-09):** GDT (0x08/0x10), 256-entry IDT with 48 NASM stubs, 8259 PIC remap (0x20/0x28), PIT 100 Hz, IRQ1 PS/2 set-1 driver (shift/caps, ring buffer).
* **P5 shell v0.1 DONE (2026-10-09):** PMM free-page stack from UEFI memmap (~52K pages), bump `kmalloc`, `NaOS>` shell (plain-English commands). Black console. Tag `v0.1`.
* **P6 paging+heap DONE (2026-10-09):** own PML4 (2MB pages, identity 0..1G + fb window), CR2 page-fault handler, free-list `kmalloc`/`kfree`, `heap` + `fault` shell commands.
* **P7 files DONE (2026-10-09):** ATA PIO driver (QEMU IDE primary-master), read-only FAT32 (own BPB/cluster code), `list` + `read` shell commands, `README.TXT` demo file on the ESP.
* **P8 multitasking DONE (2026-10-09):** 7-entry GDT + TSS (RSP0), ring-3 user tasks, `int 0x80` syscalls (exit/print/yield/getpid/sleep), preemptive round-robin (50ms), `run`/`tasks`/`kill`, demo `counter`/`ticker` programs. Single address space (no isolation yet).
* **P9a clock+power DONE (2026-10-09):** CMOS RTC (`time`/`date`/`uptime`), ACPI poweroff via config-table RSDP + `_S5_` parse, 8042 `reboot`.
* **P9b sound+history DONE (2026-10-09):** PC-speaker `beep`, 16-entry Up/Down shell history, extended key codes.
* **P9c writable files DONE (2026-10-09):** ATA write + FAT32 create/truncate/mkdir/delete, `edit`/`makedir`/`delete` shell commands.
* **P9d isolation DONE (2026-10-09):** per-task address spaces (supervisor kernel + `.utext` + private stacks at fixed VA), protection faults kill only the task, `peek` demo proves it.
* **P10a disk programs DONE (2026-10-09):** `progs/` ELF binaries at fixed base (crt0 + user.ld), FAT loader with validate-then-map, `run <file>` (`hello`, `fib` examples in `\PROGS`).
* **Stability mission DONE (2026-10-10):** reap gating, syscall pointer validation, OOM-path guards; `cp`, `cls`/`dir`/`type` aliases; `loop` + `crashme` demo programs; slot-exhaustion + big-file + kill-cycle stress verified.
* **P10b real-HW storage DONE (2026-10-10):** PCI config-space scan (`pci` shell command), AHCI port-0 polling DMA driver (IDENTIFY/READ/WRITE/FLUSH) with ATA PIO fallback via `blk` layer, uncacheable ABAR map (`vmm_map_mmio`); QEMU-verified on ICH9 pc + q35 (IDENTIFY + FAT mount + file create/read/delete round-trip).
* **Daily polish DONE (2026-10-10):** LFN reads (`list` + lookup, checksum-checked), `<cmd> > <file>` output redirect (8KB window), `/SETTINGS.TXT` (prompt + fg/bg theme, `settings` command).
* **Fork+CoW DONE (2026-10-10):** `SYS_FORK` clones address spaces with copy-on-write (PTE_COW/WASW bits, refcount table, write-fault privatizer, shared-aware reaping); `forkdemo` proves bidirectional isolation.
* **USB keyboard DONE (2026-10-10):** polled XHCI + HID boot driver (control/interrupt transfers, hotplug rescan, `usb` command); QEMU-verified typing incl. shift/digits/backspace, boot-attached and hot-attached.
* **Network DONE (2026-10-11):** E1000 driver (82540EM/82574, polled rings) + ARP/ICMP stack (static IP, `ping`/`net` commands); QEMU user-net verified 4/4 ping replies on both NIC models.
* **File syscalls DONE (2026-10-11):** `open`/`read`/`write`/`close` (+ stdout on fd 1) with per-task tables, buffered whole-file flush; `fdemo` proves program file I/O.
* **TCP DONE (2026-10-11):** UDP sockets + DHCP client (boot lease w/ static fallback) + minimal TCP client (SYN/ACK/retransmit/graceful close) + `fetch` HTTP demo; QEMU user-net verified against host HTTP server.
* **NVMe DONE (2026-10-11):** polled driver (admin + 1 I/O queue, 512B NSID 1) in `blk` (AHCI→NVMe→ATA); NVMe-only boot verified (FAT on NVMe) + nondestructive selftest.
* **Full sentences DONE (2026-10-10):** `make-folder`, `make-file`, multi-source `copy`/`move` (relinking rename), `rename`, `go`; no Unix commands remain.
* **ISO export DONE (2026-10-11):** `tools/mkiso.sh` (El Torito UEFI), `make iso`/`run-iso`, real-hardware install guide; CD-boot verified (boots to shell, runs diskless — ATAPI is v0.2).
* **Full sentences DONE (2026-10-10):** `make folder`, `copy` (files + recursive folders), `move` (rename / copy+delete); unix aliases removed.
* Post-v0.1.0 (v0.2.0 ideas, not in scope): TCP listen/accept, DNS resolver, 4K-LBA NVMe, MSI-X interrupts, ATAPI CD-ROM + USB mass storage (ISO/USB boot with files), file seek/append syscalls for programs, more NICs (virtio-net), real-hardware boot reports.

## 8. Coding Rules (purity + safety)

1. No `#include <...>` hosted headers in `boot/`/`kernel/` except our own + compiler `stdint.h`-style freestanding headers. No `printf/scanf/malloc` — we write them.
2. No `gnu-efi/edk2` headers or libs. `boot/uefi/uefi.h` is hand-written from the UEFI spec (types + GUIDs + tables we use).
3. `-ffreestanding -mno-red-zone -fno-stack-protector -nostdlib`, `-Wall -Wextra -Werror` for our code.
4. Every new file logged in `CHANGELOG.md`. Every boot-affecting change tested with `make esp run`.
5. Serial log is the debugger: `serial_puts` from day one; QEMU `-serial stdio` captures it.

## 9. Risks

* UEFI spec surface is large; we mitigate by implementing only what we call (ConOut, GOP, FileSystem, AllocatePages, GetMemoryMap, ExitBootServices).
* `ExitBootServices` can only be called once and fails if the key is stale → retry loop with fresh `GetMemoryMap`.
* GOP pixel format varies (BGR vs RGB + padding) → read `PixelFormat` + `PixelInformation`, never assume.
* FAT ESP tooling on Windows is fragile (`mtools` drive-letter vs `-i` image syntax) → `tools/mkesp.sh` pins `-i` mode and is tested in CI-less `check.sh`.
* Cross-compiler rebuilds are slow (~30min) → never delete `/opt/cross`; `build-cross.sh` is reference only.
