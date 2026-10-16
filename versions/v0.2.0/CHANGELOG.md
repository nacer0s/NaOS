# Changelog — NaOS

All notable changes to NaOS are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/); versioning follows SemVer once `v0.1.0` tags. Until then everything lands under `Unreleased`.

## [0.2.0] - Unreleased

Goal: v0.1.0 shell + files + network, plus a TCP server — `serve` answers HTTP from NaOS itself.

### Added - 2026-10-16 (own TLS 1.2 client: `get-page-tls`)

* `kernel/net/tls_{aes,prf,bn,p256,rsa,x509}.c` + `tls.c` (`get-page-tls <ip|host> <port> </path>`): from-scratch TLS 1.2 client, single suite `ECDHE_RSA_AES_128_GCM_SHA256 (0xC02F)` on secp256r1 with RSA-2048 PKCS#1 v1.5/SHA-256 signatures and an own X.509 chain check against the built-in TEST CA (`tls_ca.h`, localhost only — replace before real use). Nonces/keys come from a SHA-256 counter DRBG (not certified, documented in-code).
* Verified live against OpenSSL: `handshake done (ECDHE_RSA_AES_128_GCM_SHA256)`, server confirms `TLSv1.2 ('ECDHE-RSA-AES128-GCM-SHA256', ...)`, response body byte-exact. Independently proven with a static-ECDHE stub server + offline recomputation: PMS, master, keys, Finished decrypt and verify_data all match.
* Interop bugs found and fixed: ClientKeyExchange used handshake type 4 instead of 16 (server RST); key_block split as salt,salt,key,key instead of RFC 5246 key,key,IV,IV; `p_hash` scratch buffer 96B truncated the 109B master-secret seed (stack over-read — master/keys were garbage); peer explicit nonces must be accepted as-sent (OpenSSL randomizes them); post-handshake close_notify must be GCM-encrypted; bare-close HTTP/1.0 bodies count as success once headers complete.

### Added - 2026-10-16 (`SYS_SEEK`, `show-drives`, `install-system`)

* `SYS_SEEK 10` (`kernel/task.c`, `ulib.h ULIB_SYS_SEEK` + `SEEK_SET/CUR/END`, `progs/seektest.c`): read `[0,size]`, write clamped to length, no sparse files. Live `seektest` 13 checks ALL PASS.
* `show-drives` + `install-system <id> erase` (`kernel/fs/install.c`, `kernel/fs/mkfs.c fat_format()` superfloppy formatter): unified block layer (`kernel/drv/blk.c` — AHCI multi-port, ATA LBA48, NVMe 4K via paged `blk512()`), whole-disk file copy in 8KB chunks, refuses the boot volume. Proof: live ISO installed to a blank 256MB AHCI disk (18 entries), native boot ALL PASS.
* Verified on the final image: 21-assertion QEMU serial+HMP battery ALL PASS (LFN + NaFS + version regressions) plus end-to-end `get-page-tls` above.

### Added - 2026-10-15 (NaOS-FS writes)

* `nafs write <fat-src> </nafs-dst>` + `nafs delete </file>` (`kernel/fs/nafs.c`, shell + usage): whole-image edit in the 128KB `g_img` work buffer, source streamed via `fat_read_at`, allocation by first-fit `alloc_run` over `mark_used`, write-back through `fat_write` + quiet remount (revalidates checksum, refreshes the host cluster). Limits: image cap 128KB, read cap 16KB.
* Allocator fix: `mark_used` now marks directory blocks, root included — before, the root block itself (root@120 in the demo) was handed out and a write clobbered the directory it lived in.
* `tools/mknafs.py --pad 64`: the demo image ships 64 free blocks (185 total, 94,720 B, label `NaFSdemo`) so writes have headroom; a fully-packed image would (correctly) refuse with `no contiguous space`.
* Verified: QEMU `mount/list/read/write/read/delete` round-trip, then host-parsed `DATA.NAFS` (magic `NAFS`, v1, 185 blocks, checksum OK, `note.txt` present-then-absent, originals intact).

### Fixed - 2026-10-15 (FAT writes match long filenames)

* Reads were LFN-aware but every write path compared the lossy 8.3 form only, so touching `DATA.NAFS` or `long-file-name-demo.txt` by its real name created a duplicate entry (two live `DATA~1`-style dirents, original orphaned). Now `resolve_parent` also returns the uppercased FULL leaf (`char[256]`), `dir_find_name` reports the match slot, and overwrite/append/delete/mkdir-exists/copy-dst/rename lookups all match the long name first (8.3 fallback). File-descriptor flushes carry the full leaf too (`ffd_t.up[256]` via `fat_parent_leaf`).
* Overwrites preserve the on-disk 8.3 name (new `update_entry`: cluster+size only) instead of rewriting it — the old code re-mangled the name (`DATA.NAFS` → `DATA.NAF` on screen) and orphaned the LFN checksum. Delete/rename retire the LFN alias slots with the entry (new `wipe_alias`), so no checksum ghosts survive; renaming an LFN-carrying entry re-creates it 8.3-only (same fidelity as `make-file`, names still resolve through symmetric mangling).
* 8.3 fallback fixed at the root: the leaf was truncated to 15 chars before mangling (`LONG-FIL` with blank extension); both the 8.3 form and the compare key now derive from the full leaf.
* Verified: 21-assertion QEMU serial+HMP battery ALL PASS (LFN read/overwrite/rename/delete, NaFS round-trip, version/list regressions) + host sector forensics: one live `DATA~1` + valid LFN, three `0xE5` marks, zero duplicates, zero ghosts.

### Fixed - 2026-10-15 (`version` build date went stale)

* `build/version.h` had an order-only prerequisite, so it was generated once and never refreshed — today's binary still reported `build 2026-10-14`. New `FORCE` prerequisite re-runs the recipe every build; `cmp`-swap keeps the mtime (and all kernel objects) stable when nothing changed. `version` now prints the real build day.

### Added - 2026-10-14 (TCP listen + serve)

* `tcp_listen(port)` / `tcp_accept(listen, timeout)` (`kernel/net/tcp.c/.h`): passive open with a 3-deep SYN backlog. Tick context only queues SYNs (no ARP, no blocking — IRQ-safe); `accept()` (command context) ARPs the peer, completes the handshake (SYN-ACK + RTO resends) and returns an ESTABLISHED child. New states `LISTEN`/`SYN_Q`/`SYN_RCVD`; TCB pool 2 → 6; MSS option parsing shared with the client path; unknown ports keep the silent drop (no RST storms); `tcp_close` on a listener drops unaccepted handshakes.
* `serve <port> </file>` (`net_serve`, shell + help): serves one FAT file over HTTP/1.0 per call, then back to the prompt. Validates the request line (non-GET → 400, wrong path → 404, `..` refused), streams in 1460B chunks, `Content-Type` by extension. 60s accept timeout.
* Verified on QEMU (CD boot, host client via user-net forward): 200 + exact bytes, 400, 404, listener/TCB recycling across invocations, client `fetch` unregressed. Docs: command count 39 → 40, SECURITY server surface, site command row.

Goal: power-on → UEFI → C kernel → `NaOS>` shell (plain-English commands).

### Added - 2026-10-09

* `IDEA.md`: vision, zero-prebuilt rule, x86_64+UEFI rationale, repo tree, UEFI→ELF boot flow, handoff struct, memory map, `make` workflow, P0–P5 phases, risks.
* `README.md`: requirements table (QEMU 11.1.0, NASM 3.02, Make 4.4.1, mingw-gcc 16.2.0, x86_64-elf-gcc 16.2.0/ld 2.47, mtools, Python 3.14.8), MSYS2 setup, `check/all/esp/run/clean` quickstart.
* `LICENCE.md`: MIT.
* `Makefile` skeleton: `all/bootloader/esp/run/clean/check` with OVMF auto-detect (winget path + MSYS2 paths).
* `tools/check.sh`: fail-fast verification of gcc/nasm/make/mformat/python/qemu/OVMF/`/opt/cross`.
* `tools/mkesp.sh`: creates 64M FAT `NaOS.img` with `mformat -i` + `mcopy` (`\EFI\BOOT\BOOTX64.EFI`), no prebuilt image.
* `tools/build-cross.sh`: reference record of the `/opt/cross` x86_64-elf build (binutils 2.47 + gcc 16.2.0).
* `boot/uefi/uefi.h` (own UEFI types, no gnu-efi) + `boot/uefi/main.c` (`efi_main` prints `Hello NaOS UEFI`, halts).
* Toolchain P0 verified: `pacman` base-devel + ucrt toolchain + nasm/mtools/dosfstools installed; `/opt/cross` cross-compiler built and `x86_64-elf-gcc 16.2.0` on PATH via `~/.bashrc`.
* P1 verified 2026-10-09: `make clean && make all && ./tools/check.sh && ./tools/mkesp.sh` → `BOOTX64.EFI` PE32+ EFI application (6217 bytes), `NaOS.img` 64M FAT (`\EFI\BOOT\BOOTX64.EFI`); QEMU 11.1.0 + OVMF via pflash reaches `BdsDxe: starting Boot0001` (EFI app launched). `make run` uses pflash (not `-bios`) on Windows QEMU build.

### Added - 2026-10-09 (P2: ELF loader + framebuffer kernel)

* `common/`: `bootinfo.h` (own UEFI→kernel handoff ABI), `io.h` (port I/O), `serial.h/.c` (COM1 38400 8N1, shared bootloader+kernel), `cstring.h/.c` (memcpy/memset/memmove/memcmp/strlen/strcmp, freestanding).
* `boot/uefi/`: extended `uefi.h` (BootServices positional table, LoadedImage, SimpleFS+File, GOP, memmap, GUIDs); `conout.h/.c`, `file.h/.c` (ESP `\NaOS\KERNEL.ELF` reader), `elf.h/.c` (ELF64 PT_LOAD loader @p_paddr), `bootutil.h/.c` (GOP query, ExitBootServices retry loop); `main.c` full flow with serial+ConOut logging and halt-on-error.
* `kernel/`: `entry.asm` (long-mode entry, own 64K stack, MS-ABI RCX→SysV RDI shim), `link.ld` (ELF64 @1M), `kmain.c` (handoff snapshot, serial+fb banner), `drv/fb.h/.c` (32bpp GOP console, RGBX/BGRX, scroll), `lib/kprintf.h/.c` (fb+serial `%c%s%u%x%X%p`), `font8x8.h` (hand-drawn 8x8 font).
* `Makefile`: kernel rules (`x86_64-elf-gcc -nostdlib -T link.ld -lgcc`), `CROSS_BINDIR` auto-detect (`/opt/cross` vs `C:/msys64/opt/cross` for native Windows make), VPATH + header deps, `esp: all`.

### Fixed - 2026-10-09 (P2 debugging, all verified by QEMU serial + screenshot)

* `EFI_TABLE_HEADER` is 24 bytes, not a pointer — `Hdr` as `VOID*` shifted every SystemTable/BootServices field (ConOut calls hit ConIn, LocateProtocol hung). Real 24-byte struct added.
* UEFI error statuses carry the high bit (`EFI_BUFFER_TOO_SMALL` = `0x8000000000000005`, not 5) — GetMemoryMap probe misread as failure.
* MS→SysV ABI cross: mingw bootloader passes bootinfo in RCX, kernel SysV expects RDI — `mov rdi, rcx` shim in `entry.asm`.
* `make run` uses pflash (`-bios` fails on winget QEMU Windows build).

### Verified - 2026-10-09 (P2)

* `KERNEL.ELF`: ELF 64-bit x86-64, entry `0x101160` (`_start`), LOAD R+E @1M + RW BSS (64K stack).
* QEMU serial: `GOP 1280x800 base=0x80000000` → `ELF bytes=15984` → `Entry=0x101160` → `jumping to kernel` → `[NaOS] kernel starting` → `NaOS v0.1 - kernel OK` + `memmap entries=103`.
* QEMU screendump: blue `0x1018A0` screen, white 8x8 banner text, correct RGB order.

### Added - 2026-10-09 (P4: interrupts + keyboard, P5: memory + shell = v0.1)

* `kernel/cpu/`: `gdt.h/.c` (long-mode code/data), `load.asm` (`gdt_flush` far-return, `idt_load`), `idt.h/.c` (256 gates, 0x8E), `isr.asm` (48 stubs, err-code aware, common frame), `isr.h/.c` (exception print+halt, IRQ dispatch), `pic.h/.c` (8259 remap 0x20/0x28, masks, EOI).
* `kernel/drv/pit.h/.c` (100 Hz ticks), `kernel/drv/ps2_kbd.h/.c` (IRQ1 ring buffer, set-1 map, shift/caps, blocking `kbd_getc`).
* `kernel/mm/pmm.h/.c` (conventional-memory page stack, kernel image excluded), `kernel/mm/kmalloc.h/.c` (page-backed bump allocator).
* `kernel/shell.h/.c` (`NaOS>` line editor, backspace/tab, `help/ver/clear/echo/mem/halt`).
* `kernel/drv/fb.h/.c`: `fb_backspace()`; console theme changed to **black background** (`0x000000`) per request.
* `kernel/link.ld`: `kernel_end` symbol for PMM exclusion. `Makefile`: per-asm object rules (fixed `isr.c`/`isr.asm` → `isr.o` collision), wider VPATH.

### Added - 2026-10-09 (P8: user mode + preemptive scheduler)

* `kernel/cpu/gdt.h/.c`: 7-entry GDT (user data/code DPL3 at 0x18/0x20, TSS slot); `gdt_install_tss()`.
* `kernel/cpu/tss.h/.c`: 104-byte TSS, RSP0 per task, I/O bitmap denies all user port I/O, `ltr`.
* `kernel/cpu/idt.c`: per-gate DPL; `isr128` syscall gate at 0x80/DPL3. `kernel/cpu/isr.asm`: `g_switch_to` frame-swap resume. `isr_frame_t` carries ss:rsp for ring-3 frames.
* `kernel/cpu/isr.c`: syscall dispatch, user-fault kills task (with CR2) instead of halting, tick drives scheduler.
* `kernel/task.h/.c`: round-robin (50ms quantum), READY/SLEEP/DEAD, sleep/wake, syscalls exit/print/yield/getpid/sleep, spawn with crafted iretq frames, kill, reap, stack-reuse cache, `tasks` list. `kernel/mm/pmm.c`: `pmm_alloc_contig()` for stacks.
* `kernel/user/ulib.h` (ring-3 syscall wrappers), `prog_counter.c`, `prog_ticker.c`, `progs.h`.
* `kernel/shell.c`: `run <counter|ticker>`, `tasks`, `kill <id>`. `kprintf`/`kmalloc` disable preemption around shared state (depth-counted lock).
* Single address space: all pages U/S=1 (VMM change) — rings block privileged instructions, not memory (no isolation yet, documented).

### Fixed - 2026-10-09 (P8 debugging)

* First task faulted instantly (`err=15` on its stack push): VMM mapped everything supervisor-only — U/S bit set on all PDEs.
* Second spawn failed (`-2`): reaped stack pages scrambled PMM LIFO order that contig-alloc depends on — freed stacks now cached for reuse instead of returned.
* Shell `tasks` output looked empty under load: lines were interleaved with task output in serial; raw bytes confirmed all tasks listed with correct states.

### Verified - 2026-10-09 (P8)

* `run counter` + `run ticker`: 5:1 interleave (`counter 1 184..187`, `tick 32`, …), shell stays responsive mid-load.
* `tasks`: `0* kernel run`, `1 counter sleep`, `2 ticker sleep` (states accurate).
* `kill 1`: `stopped program 1`, counter output stops, slot reaped (`tasks` shows kernel only).

### Added - 2026-10-09 (P9a: clock + power)

* `kernel/drv/rtc.h/.c`: CMOS RTC reader (UIP wait, BCD/binary + 12/24h modes, rollover re-read, validation).
* `kernel/drv/acpi.h/.c`: RSDP from UEFI config table (via `bootinfo.rsdp`; BIOS-ROM scan fallback), XSDT/RSDT→FACP→DSDT walk with checksums, minimal `_S5_` AML parser, ACPI-enable doorbell, PM1 poweroff, 8042 reboot pulse. `boot/uefi/`: `EFI_CONFIGURATION_TABLE` + ACPI GUIDs, RSDP handoff. `common/io.h`: `naos_inw`/`naos_outw`.
* Shell: `time`, `date`, `uptime`, `reboot`; `shutdown` attempts real poweroff (halts if ACPI missing).

### Fixed - 2026-10-09 (P9a debugging)

* `-Warray-bounds` fires on absolute BIOS-area accesses — narrow `#pragma GCC diagnostic ignored` helper for the EBDA read.
* No RSDP in BIOS ROM under OVMF: RSDP now comes from the UEFI configuration table (preferred) with ROM scan as fallback.
* QEMU's DSDT defines `_S5_` as four zeros — not a parse bug: QEMU's PIIX4 treats SLP_TYP 0 as soft-off. Verified by testing, not just reading.

### Verified - 2026-10-09 (P9a)

* `time` → `23:33:14`, `date` → `2026-10-09` (matches host), `uptime` → `up 0:0:13`.
* `shutdown`: QEMU process exits on its own (rc=0, no failure line) — guest powered off.
* `reboot`: serial shows second `BdsDxe: starting` + fresh `[NaOS] kernel starting` — 8042 reset works.

### Added - 2026-10-10 (ISO export + real-hardware docs)

* `tools/mkiso.sh`: wraps the ESP FAT image in an El Torito UEFI ISO (pure UEFI, no CSM). `make iso` / `make run-iso` (`-cdrom` boot check). `tools/check.sh` notes missing xorriso without failing.
* `README.md`: ISO + real-hardware section (Rufus DD mode, Secure Boot off, honest IDE/PS2 limits, backup warning); layout + requirements updated.

### Verified - 2026-10-10 (ISO boots)

* `make iso` → 67MB `NaOS.iso` (El Torito FAT image, xorriso warning about ESP copy is benign).
* `make run-iso` (`-cdrom`) boots OVMF → bootloader → kernel → `NaOS>` shell. Same image writable to USB via Rufus DD mode.

### Added - 2026-10-10 (full-sentence file commands)

* Shell: `makedir` → `make-folder`, new `make-file` (empty file), `cp` → `copy` (one or many sources into a folder), new `move` (same multi-shape), new `rename <old> <new name>` (stays put, folders rejected from changing folders). Unix aliases (`cls`, `dir`, `type`) removed.
* `kernel/fs/fat32.h/.c`: `fat_rename()` rewritten as true re-linking (same folder: rewrite name; anywhere: new entry + `..` repoint + old slot cleared — instant, no data moves, guarded against moving into itself); `fat_copy()` recursive trees with self-copy guard.
* Help/README/IDEA updated.

### Verified - 2026-10-10 (full-sentence file commands)

* `make-folder work`, `make-file work/note.txt` (0 bytes), `copy readme.txt work/copy.txt`, `rename work/copy.txt renamed`, multi-copy with per-file results, full-folder `move work bak2` (73KB tree relinked, dotdot correct), `makedir` now unknown.

### Added - 2026-10-10 (full-sentence commands, v1 — superseded above today)

* Shell spoke full words: `makedir` → `make folder`, `cp` → `copy`, new `move` (`move <from> <to>`). Unix/DOS aliases (`cls`, `dir`, `type`) removed — plain English only, no regular-OS commands.
* `kernel/fs/fat32.h/.c`: `fat_rename()` (same-folder rename for files and full folders; files across folders go copy+delete; folders across folders refused with a message), `fat_copy()` (files + recursive folder trees with self-copy guard and depth cap).
* Help text updated.

### Verified - 2026-10-10 (full-sentence commands)

* `make folder test`, `copy readme.txt test/copy.txt` (105 bytes), `move test/copy.txt test/moved.txt` (rename, old name gone).
* `copy naos bak` recursively copies 73312 bytes (KERNEL.ELF intact in copy); `move bak bak2` renames the full folder; `move bak2 naos/x` refused cleanly; old `makedir` is unknown.

### Added - 2026-10-10 (go command: folders you can walk into)

* `kernel/shell.c`: `go <folder>` (plain English for `cd`) — relative/absolute paths, `.`/`..`/`/` handling with normalization, bare `go` goes home; prompt shows the folder (`NaOS/naos>`); `list/read/edit/makedir/delete/cp/run` all resolve through it. `kernel/fs/fat32.h/.c`: `fat_is_dir()`.
* `tools/mkesp.sh`: demo hint mentions `go naos` now.

### Fixed - 2026-10-10 (go debugging)

* Error message printed an empty name (arg pointer was consumed during normalization) — original arg saved for the message.

### Added - 2026-10-10 (stability mission: fixes, commands, apps)

* `kernel/shell.c`: `cp <from> <to>` (128KB window), `cls`/`dir`/`type` aliases, `kill` id digit cap.
* `progs/loop.c` (yield demo), `progs/crashme.c` (divide-by-zero kill-path demo).

### Fixed - 2026-10-10 (stability review)

* `reap_dead()` ran even with preemption locked and could interleave with a locked `pmm_alloc_contig()` mid-memmove — now gated on `!g_nopreempt`.
* `SYS_PRINT` read user pointers unchecked (a bad pointer halted the whole OS): new `user_range_ok()` validates present+user mappings, returns -1 instead.
* `vmm_free_prog(0)` on the OOM path read address 0 as page tables (it is mapped!) and could free garbage into the PMM — guarded.
* Task-fault log no longer prints stale CR2/addr for non-page-fault exceptions.
* Two shell edits that accidentally deleted neighboring lines caught by re-reading before building (process note: always re-read after multi-edit).

### Verified - 2026-10-10 (stability mission)

* 15/15 task slots fill, 16th fails cleanly (`-1`); `tasks` accurate under full load; kill/respawn cycles reuse slots.
* `cp` of 68KB multi-cluster file + read-back (`ELF` magic); `delete` of non-empty dir correctly refused (`-7`); overwrite/truncate round-trips.
* `loop` counts with yields; `crashme` dies with `vec=0 (Divide by zero)`, OS unaffected.

### Added - 2026-10-10 (P10b: PCI + AHCI storage)

* `kernel/drv/pci.h/.c`: config-space access via `0xCF8`/`0xCFC` (bus 0), class scan, `pci_list()`; `common/io.h` gains `inl/outl`.
* `kernel/drv/ahci.h/.c`: AHCI port-0 polling driver — BIOS/OS handoff, port stop/start, IDENTIFY, READ/WRITE DMA EXT (single sector per command), FLUSH; single slot, single 4KB PRDT entry, 4 PMM pages for CLB/FB/CT/data.
* `kernel/drv/blk.h/.c`: block layer — AHCI first, ATA PIO fallback; `fat32.c` and boot order rewired (`blk_init` after PMM+VMM, `fat_init` after); `kmain.c` no longer calls ATA directly.
* `kernel/mm/vmm.h/.c`: `vmm_map_mmio()` maps one 2MB MMIO window uncacheable (PWT+PCD) into the kernel (boot) space only — task spaces stay clean.
* `kernel/shell.c`: `pci` command (lists bus 0 devices + active disk driver).

### Fixed - 2026-10-10 (P10b debugging, all found by QEMU runs)

* PML4[3] is the 1.5TB region, not 3G-4G: the MMIO window hangs off PDPT_LO[phys>>30] (reuse PD_LO/PD_FB for index 0/2, lazy PDs otherwise). First version faulted at the ABAR address.
* PRDT byte count lives in DWORD 3 (DBC = bytes-1), DWORD 2 is reserved — first version wrote it to DWORD 2, so every transfer moved 1 byte (IDENTIFY showed only word 0 = `0x40`, reads moved nothing).
* FLUSH carries no data: PRDTL must be 0 for it (1 for data commands).
* No HBA reset (GHC.HR): QEMU's ICH9 model never clears HR — best-effort resets stay port-level (stop + COMRESET path kept behind `#if 0` for real hardware; firmware leaves a working link).

### Verified - 2026-10-10 (P10b)

* q35 (native AHCI) + pc + `-device ahci`: `ahci: QEMU HARDDISK sectors=131072`, `blk: using ahci`, FAT mounts, shell up.
* `pci` lists 7 devices incl. `00:1f.2 1/6` (q35) / `00:4.0 1/6` (pc); `list` reads via AHCI; `make-file`/`read`/`delete` round-trip through WRITE DMA + FLUSH.
* pc with IDE only: `blk: using ata`, FAT mounts — fallback intact. `make all` zero warnings, `check.sh ALL OK.

### Added - 2026-10-10 (daily polish: LFN, redirect, settings)

* `kernel/fs/fat32.c`: long-filename reads — LFN chunk collection with order + checksum validation, `list` shows long names, path lookup matches them (case-insensitive); write paths still use short names. 255-char components.
* `kernel/lib/kprintf.h/.c`: output capture (`begin/end/full`); `kernel/shell.c` splits `<cmd> > <file>` (8192-byte window, truncation noted, `edit` refused as interactive), new `settings` command + help lines.
* `kernel/settings.h/.c`: `/SETTINGS.TXT` (`prompt=`, `fg=`/`bg=` RRGGBB hex, `#` comments, unknown keys ignored); `kernel/drv/fb.h/.c` gains `fb_set_colors()`; prompt and `clear` follow the theme.
* `tools/mkesp.sh`: ships `long-file-name-demo.txt` (LFN exercise) + default `SETTINGS.TXT`.

### Verified - 2026-10-10 (daily polish)

* `list` shows `long-file-name-demo.txt` (not `LONG-F~1`), `read` opens it by long name (88 bytes correct).
* `list > /saved.txt` saves 155 bytes, `read` shows identical listing; `print ... > /p.txt` saves exactly 16 bytes.
* Custom `SETTINGS.TXT` (`prompt=hello`, green-on-navy, bogus key) → `hello>` prompt, `settings` reports values; defaults when absent.

### Added - 2026-10-10 (fork with copy-on-write)

* `kernel/mm/cow.h/.c`: refcount table for shared user pages (share/release/shared; sole-owner release frees).
* `kernel/task.h/.c`: `SYS_FORK` (5); `task_fork()` clones the caller's space (fresh tables + kstack, all user pages shared read-only, child resumes from a copied syscall frame with 0); `task_cow_fixup()` privatizes on write fault (WASW bit preserves original writability, stale-COW-after-sibling-death copies lazily).
* `kernel/mm/vmm.h/.c`: `PTE_COW`/`PTE_WASW` bits, `vmm_fork_tables()`, `vmm_free_prog()` releases through CoW; `reap_dead()` skips bundles for CoW-shared tasks (full release instead).
* `kernel/cpu/isr.c`: user write fault on a present page tries the CoW fixup before the kill path.
* `kernel/user/ulib.h`: `u_fork()`; `progs/forkdemo.c` (parent/child print + divergent global write); `progs/user.ld` splits .data/.bss (kills the RWX-LOAD linker warning).
* `Makefile`: `kernel/mm/cow.c`, `progs/forkdemo.c`.

### Verified - 2026-10-10 (fork)

* `run /PROGS/forkdemo.elf`: child pid=2 sees 7, parent pid=1 child=2 sees 42 — both directions isolated, zero faults (fixup silent).
* Both exit cleanly (shared-page reap path + lazy stale-COW writes exercised, no faults after).
* `hello` + `fib(25)=75025` still pass (user.ld relink safe); `make all` zero warnings.

### Added - 2026-10-10 (USB HID keyboard via XHCI)

* `kernel/drv/pci.h/.c`: `pci_find_class_pi()` (class/subclass/prog-if match; XHCI = 0x0C/0x03/0x30).
* `kernel/drv/xhci.h/.c`: polled XHCI driver — 64-bit BAR + `vmm_map_mmio` window, halt/reset/start, command/event rings, No-Op self-test, Enable/Disable Slot, Address Device, control transfers (setup/data/status with EDK2-matching TRB shapes), Configure Endpoint, interrupt-IN queueing, event drain with status-TRB matching.
* `kernel/drv/usb_kbd.h/.c`: HID boot-keyboard enumeration (descriptors, boot protocol, idle, configure, EP1 pipe), 8-byte report → set-1 scancode synthesis (full HID table + modifiers) into the shared kbd ring, hotplug rescan (poll-driven, throttled, backoff) + `usb` shell command.
* `kernel/mm/vmm.c`: `vmm_map_mmio()` generalized past 4G (per-PML4-entry PDPTs + PD table, canonical-address check) for 64-bit PCI BARs (QEMU puts XHCI at 0xC000000000).
* `kernel/task.c`: `usb_kbd_poll()` on every tick; `kernel/kmain.c`: `usb_kbd_init()` after disk; `Makefile` + shell `usb` command + help.

### Fixed - 2026-10-10 (XHCI debugging, QEMU-traced)

* MMIO helpers double-added the BAR (absolute bases through offset-taking accessors faulted at 2×BAR).
* PRDT-style lesson repeated: DCS is DW2 bit 0 of the EP TR pointer (was DW3); Setup DW2 is length-only with direction/TRT in DW3 (mirrored from EDK2's fetches); TD Size 0 for single-TRB stages.
* Root-hub port number is slot-DW1 bits 23:16 (was 31:24 → "port not found").
* Control/event waits need ~0.5s budgets (cached-RAM spins expire in ms; QEMU answers async).
* Event consumer gated on rings-ready, not init-done (init-time waits saw an empty ring).
* Configure Endpoint omits EP0 from ADD (re-adding the live EP0 makes QEMU TRB-error; omitting is spec-legal).
* Slot leak on failed enumeration: Disable Slot on every post-enable failure (a stuck slot blocks its port).

### Verified - 2026-10-10 (USB keyboard)

* Boot-attached and hot-attached (`device_add`) `usb-kbd`: `usb: HID keyboard on port 5 (slot 1)`, full `print AbC123`, backspace-correcting line, `version` — zero faults.
* No-XHCI boot unchanged (silent skip); `check.sh ALL OK.

### Added - 2026-10-11 (network: E1000 + ARP/ICMP)

* `kernel/drv/e1000.h/.c`: 82540EM/82574 driver — PCI class match, 64-bit-capable BAR + MMIO map, reset, link (SLU + full duplex + MDIC autoneg kick), RX/TX legacy rings (128/64 descs, polled DD), MAC from RAL/RAH, manageability off.
* `kernel/net/net.h/.c`: static 10.0.2.15/24 via 10.0.2.2, ARP cache + request/reply, ICMP echo in/out, blocking `ping` with RTT, `net` status, dotted-quad parser.
* Shell `ping <ip>` + `net` commands + help; `kmain` init after USB; RX drain on every tick.
* RX consumes at a software head (`g_rx_next`), not RDH (which is hardware's next-to-fill).

### Fixed - 2026-10-11 (network debugging, QEMU-traced)

* TX never started until the programming sequence mirrored Linux (lazy TCTL.EN at first send, full-duplex CTRL, exact RCTL/TXDCTL values, MDIC autoneg kick) — verified against Linux's own register trace on the same QEMU.
* RX never delivered until consumption moved off RDH (see above); also fixed missing `volatile` on ring polling.
* `vmm_map_mmio()` generalized past 4G along the way (64-bit PCI BARs, e.g. XHCI at 0xC000000000).

### Verified - 2026-10-11 (network)

* `ping 10.0.2.2`: 4/4 replies, 0ms, on both `-device e1000` and `-device e1000e` (user-net); pcap shows our ARP out + replies in.
* `net` shows MAC/IP/link/tx/rx; no-NIC boot unchanged; `check.sh ALL OK.

### Added - 2026-10-11 (file syscalls for programs)

* `kernel/task.h/.c`: `SYS_OPEN` (6, path + mode 0=r/1=w/2=a), `SYS_READ` (7), `SYS_WRITE` (8), `SYS_CLOSE` (9); per-task fd table (3-7, fds 1-2 are console stdout); reads stream via `fat_read_at`, writes stage in a PMM page (4KB cap) and flush via `fat_write_parts` at close; forked children start fd-less; reaping frees staged buffers.
* `kernel/fs/fat32.h/.c`: `fat_write_parts()` (split out of `fat_write`), `fat_fopen_r()`, `fat_parent_leaf()`, `fat_read_at()` (positional cluster walk).
* `kernel/user/ulib.h`: `u_sys3()` + `u_open`/`u_read`/`u_write`/`u_close`; `progs/fdemo.c` (write + read-back + stdout demo); `Makefile` builds it.
* Limits (documented): 2048 bytes per call, 4KB per open-for-write file, no seek yet, killed tasks lose unflushed writes.

### Verified - 2026-10-11 (file syscalls)

* `run /PROGS/fdemo.elf`: saves, re-reads in chunks, prints via fd 1, zero faults; shell `read /HELLO.TXT` shows identical 45 bytes.

### Added - 2026-10-11 (UDP/DHCP/TCP)

* `kernel/net/pkt.h` (header-only: big-endian put/get, ones-complement sums, RFLAGS save/restore IRQ guard), `udp.h/.c` (4 sockets × 2-packet queue, payload-only recvfrom, real checksums), `dhcp.h/.c` (discover/offer/request/ack, options 53/54/1/3/6, static fallback), `tcp.h/.c` (2 TCBs, active open, MSS parse, in-order RX + dup-ACK, 1s RTO ×4, graceful close, `fetch` HTTP/1.0 demo).
* `kernel/net/net.c`: static config → mutable (DHCP-applied) addrs + DNS, `net_send_ip`/`net_send_ip_mac` shared sender, TCP/UDP RX dispatch, IRQ-guarded ARP/ICMP sends, broadcast accept. `kmain` runs `net_dhcp()` after `sti`; shell gains `dhcp` + `fetch <ip> <port> </path>`.
* Fixed along the way: `udp_recvfrom` used to hand callers the UDP header (broke DHCP parsing); `tcp_send` never advanced `snd_nxt` (every data ACK rejected — caught via QEMU pcap).
* Limits (documented): TCP client only (no listen), stop-and-wait sends (≤1460B/call), 4KB RX window, no DNS yet, fetch prints text bodies (≤64KB).

### Verified - 2026-10-11 (UDP/DHCP/TCP)

* Boot: `dhcp: offer 10.0.2.15, requesting...` → `dhcp: ack 10.0.2.15`; `net` shows mask/gw/dns.
* `fetch 10.0.2.2 8099 /hello.txt` against host `python3 -m http.server`: exact 49-byte body, graceful FIN close; pcap shows textbook handshake + checksums OK on every segment.
* `check.sh ALL OK`, zero warnings.

### Added - 2026-10-11 (NVMe driver)

* `kernel/drv/nvme.h/.c`: PCI 0x010802 (64-bit BAR0, incl. above-4G), controller cycle (disable→queues→enable), 64-entry admin + 32-entry I/O queue pair, polled phase-tag completion, IDENTIFY controller/namespace-1, single-block READ/WRITE with PMM bounce page, 512B namespaces only (clean error otherwise).
* `kernel/drv/blk.c`: probe order AHCI → NVMe → ATA (`blk: using nvme`); shell `nvme` command = controller info + save/pattern/verify/restore selftest on last LBA (safe on the boot disk too).
* Fixed along the way: CQ poll used a plain load that `-O2` hoisted out of the loop (completion sat in RAM unseen) — volatile loads now.
* Limits (documented): port/queue count 1, no MSI-X (polled), no 4K-LBA yet.

### Verified - 2026-10-11 (NVMe)

* AHCI boot + 16MB scratch NVMe: `model 'QEMU NVMe Ctrl'`, `namespace 1: 32768 blocks`, `selftest OK`.
* NVMe-only boot (system image on `-device nvme`): `blk: using nvme`, FAT mounts (`tot=131072`), `list`/`read /README.TXT` work, selftest OK with FS intact.

### Added - 2026-10-11 (version system)

* `VERSION := 0.1.0` in the Makefile generates `build/version.h` (`NAOS_VERSION`/`NAOS_BUILD`, rewritten only when changed so same-day builds don't rechurn); boot banner + `version` use it. `version` prints two short lines: `NaOS v0.1.0` + build date.

### Changed - 2026-10-11 (frozen snapshot)

* Whole tree moved to `versions/v0.1.0/`; repo root keeps only the global `README.md`, `LICENCE.md` (MIT), and `.gitignore`. v0.1.0 rebuilt from clean + reboot-verified in the new home; stale `v0.2` mentions fixed.

### Added - 2026-10-09 (P10a: programs from disk)

* `progs/user.ld` (base 0x40100000, entry `_ustart`), `progs/crt0.asm`, `progs/hello.c`, `progs/fib.c` (share `kernel/user/ulib.h`); `Makefile` builds `build/progs/*.elf`, `mkesp.sh` installs to `\PROGS\`. Verified: no undefined symbols, no libgcc/extern calls in user objects.
* `kernel/mm/vmm.h/.c`: `vmm_map_user()` (4KB user pages in shared PT window), `vmm_free_prog()` (range free+clear on reap).
* `kernel/task.c`: `task_spawn_elf()` — bounded file read (128KB static scratch, not kmalloc), validate-then-map two phases, OOM rollback with bundle pushback; spawn refactored around shared `spawn_commit()`.
* `kernel/shell.c`: `run` falls back to disk paths (`run progs/hello.elf`).

### Fixed - 2026-10-09 (P10a debugging)

* Disk tasks faulted at entry (`protection err=5 addr=0x0`, zeroed regs executing `add %al,(%rax)`): the per-page copy compared a VA against a file offset, so zero bytes were ever copied. Copy window rewritten purely in VA terms; proven by dumping file bytes vs mapped bytes.
* Missing-PT-level lesson from P9d applied from the start (per-task PML4/PDPT/PD/PT).

### Verified - 2026-10-09 (P10a)

* `run progs/hello.elf` → `hello from disk! pid=1`, exits, slot reaped. `run progs/fib.elf` → `fib(25)=75025`.
* `run nope.elf` → clean `run: no program and no file nope.elf (-3)`.
* Second spawn reuses first bundle (identical tables/stack addrs in log).

### Added - 2026-10-09 (P9d: per-task address spaces)

* `kernel/link.ld`: `.utext` section (2M-aligned) holding only `prog_*.o` text+rodata — the sole user-executable kernel region (`utext_start/end`, `kernel_end` moved after it so PMM excludes it).
* `kernel/mm/vmm.h/.c`: `vmm_create_space()` — per-task PML4/PDPT/PD/PT; shared stripped PD_LO copy (supervisor except `.utext`), supervisor-only framebuffer, per-task 4KB user stack pages at fixed `USTACK_VA` (0x40000000). TLB flush when shared flags change.
* `kernel/task.h/.c`: per-task `cr3` (loaded on every switch; task 0 keeps boot tables), reuse bundles extended with table pages.
* `kernel/user/ulib.h`: 32-bit print helper (64-bit division would call supervisor-resident libgcc at ring 3 — always-inline + no 64-bit div/mod in user code, verified via objdump).
* `kernel/user/prog_peek.c` (+ `run peek`): reads kernel text to demo the protection fault.
* `kernel/cpu/isr.c`: fault reason decoder (not-present/protection/reserved) + CR3 in task-fault log.

### Fixed - 2026-10-09 (P9d debugging)

* First isolated task faulted on its stack push (`not-present` at top of stack): 4KB mappings were placed directly in a PD, but a PD entry with PS=0 is a *page-table pointer*, not a 4KB mapping — the CPU walked into a zeroed page. Added the missing PT level (per-task PML4/PDPT/PD/PT).
* U/S strategy: kernel supervisor-only except `.utext`; user stacks user-accessible; syscall args work because handlers run at CPL0.

### Verified - 2026-10-09 (P9d)

* `run peek` → `TASK FAULT vec=14 protection err=5 addr=0x100000 ... -- killed`, OS keeps running (isolation proven with distinct CR3s per task).
* `run counter` + `run ticker` interleave; `kill 1` stops counter, ticker continues alone, slot reaped.

### Added - 2026-10-09 (P9c: writable files)

* `kernel/drv/ata.c`: `ata_write()` (WRITE SECTORS + cache flush); `common/io.h`: `naos_outsw()`.
* `kernel/fs/fat32.c`: full write path — `fat_set()` (both FAT copies), `fat_alloc()` with hint + zeroing, `free_chain()`, `resolve_parent()` (refuses `.`/`..`), `dir_alloc_slot()` (extends dirs), `write_entry()`, `fat_write()` (create-or-truncate with OOM rollback), `fat_mkdir()` (with `.`/`..`), `fat_delete()` (files + empty dirs only). `dir_find` also returns slot position; volume labels skipped.
* `kernel/shell.c`: `edit` (append editor, `.save`/`.quit`, 8KB cap), `makedir`, `delete`; line input factored into `read_line()` (shared with history).
* Help text updated.

### Verified - 2026-10-09 (P9c)

* `makedir test` → `edit test/hi.txt` → `.save` → `read` round-trips bytes; overwrite frees old chain; `delete` file then empty dir; root clean after. Multi-cluster `KERNEL.ELF` untouched.

### Added - 2026-10-09 (P9b: sound + history)

* `kernel/drv/speaker.h/.c`: PC speaker via PIT channel 2 + port 0x61 (blocking, hlt-waits on timer). Shell `beep` plays a C-E-G arpeggio.
* `kernel/drv/ps2_kbd.h/.c`: `kbd_getc` now returns `int` — ASCII or `KBD_UP/DOWN/LEFT/RIGHT` from 0xE0 sequences (previously dropped).
* `kernel/shell.c`: 16-entry history (Up recalls older, Down newer, partial line preserved, exact duplicates skipped).

### Verified - 2026-10-09 (P9b)

* `version` → Up → Enter re-ran it (later tick count proves recall, not retype). `beep` returns to prompt (audio path runs; host had no sound device to hear it).

### Added - 2026-10-09 (installed console font)

* Console font is now DejaVu Sans Mono (installed package, open license), rasterized at build time — no more hand-drawn glyphs. `tools/font_render.py` (ours) renders ASCII 12x24 with Pillow into 2-bit alpha `build/font_bitmap.h`; `kernel/drv/fb.c` just blits it. Hand-drawn `font8x8.h`/`font_stroke.h` retired. `Makefile` generates the header (`PY`/`FONT_TTF` auto-detect); `tools/check.sh` verifies Pillow + TTF.
* Explicit, user-approved exception to the zero-prebuilt rule: font DATA comes from `mingw-w64-ucrt-x86_64-ttf-dejavu`, all code remains ours.
* Screenshot-verified: clean professional text at 106x33.

### Added - 2026-10-09 (vector console font, superseded same day)

* `kernel/font_stroke.h`: hand-designed stroke font — every glyph as line segments on an 8x16 grid (no copied font data). `kernel/drv/fb.c` rasterizes all 128 glyphs once at boot (integer fixed-point point-segment distance, round caps) into a blit cache; console cells are now 12x24 (106x33) with 2-bit grayscale anti-aliasing + per-pixel blending. Deleted blocky `font8x8.h`. (Iterated: 16x32/3px was too fat, 12x24 binary was dotted, AA at 12x24 was smooth but airy — cells tightened to 10x24 with non-uniform 1.25x/1.5x render scale.)
* Fixed after screenshot review: `s` read as `e` and `c` as `o` (stroke caps closed their counters at 3px) — openings widened; verified `sectors/entries/free/pages/list` legible.
* Note: kernel uses no SSE (`-mno-sse`), so the rasterizer is integer-only — a first float version failed to build (`SSE register return with SSE disabled`).

* `kernel/shell.c`: Unix-jargon commands replaced with normal words — `read` (was `cat`), `list` (was `ls`/`dir`), `version` (was `ver`), `memory` (was `mem`), `print` (was `echo`), `test` (was `heap`), `crash` (was `fault`), `shutdown` (was `halt`); `help`/`clear` unchanged. Whole-word matcher `after()` so `listen` is not `list`. Multi-line `help` with one-line descriptions.
* `tools/mkesp.sh`: `README.TXT` demo text updated to the new words.

### Fixed - 2026-10-09 (font)

* `kernel/font8x8.h`: `N` glyph was mirrored (diagonal drifted left going down — off-by-one in hand-computed bit positions). Redrawn as `{0x66,0x76,0x76,0x6E,0x6E,0x66,0x66}` with the diagonal stepping right downward; verified via QEMU screenshot. Other diagonal glyphs (M/W/V/X/K) audited, OK.

### Added - 2026-10-09 (P6: paging + heap)

* `kernel/mm/vmm.h/.c`: own PML4 (2MB pages) — identity 0..1G + framebuffer window, CR3 switch while in long mode. 4 table pages.
* `kernel/cpu/isr.c`: vector 14 prints fault address (CR2) + error + RIP.
* `kernel/mm/kmalloc.h/.c`: rewritten as first-fit free-list allocator (split/coalesce, 16-aligned) + `kfree()` + `kmalloc_used_bytes()`.
* `kernel/shell.c`: `heap` (alloc/free/reuse workout), `fault` (deliberate unmapped read demo); `mem` now shows heap usage. Boot heap self-test in `kmain.c`.

### Verified - 2026-10-09 (P6)

* Serial: `[vmm] paging on own tables` → `cr3=0xfe42000`, `heap selftest OK`, shell up.
* `heap`: `a/b/c` carved from one page, `d` reuses freed `b` slot, neighbors intact, `used=0` after free.
* `fault`: `PAGE FAULT err=0 addr=0x50000000 rip=0x101f76 -- HALT` (CR2 exact).

### Added - 2026-10-09 (P7: ATA + FAT32 files)

* `common/io.h`: `naos_insw()` (REP INSW) for PIO data.
* `kernel/drv/ata.h/.c`: primary-master PATA, LBA28 polling reads, IDENTIFY (model + capacity), bounded waits (no hangs on missing device).
* `kernel/fs/fat32.h/.c`: read-only FAT32 (own BPB parsing, cluster chains, subdirs, 8.3 + case-insensitive paths, volume-label/LFN/deleted handling); `fat_ls()`, `fat_read()`.
* `kernel/shell.c`: `ls [dir]`, `cat <file>` (4K cap, truncation note). `kmain.c`: ATA + FAT init logging.
* `tools/mkesp.sh`: adds `README.TXT` demo file to the ESP.

### Fixed - 2026-10-09 (P7 debugging)

* `dir_find` matched the `mformat -v` volume-label entry (`NAOS`, attr 0x08) shadowing the real `NAOS/` directory — volume labels now skipped (found by dumping root-sector bytes).

### Verified - 2026-10-09 (P7)

* Serial: `ata: QEMU HARDDISK sectors=131072`, `fat32: spc=1 fat@32 data@2050 root=2 tot=131072` (matches host-side BPB dump).
* `ls` → `EFI/ NAOS/ README.TXT 101 bytes` (+ OVMF's own `NVVARS` file — real-world data handled). `ls naos` → `./ ../ KERNEL.ELF 35608 bytes`.
* `cat readme.txt` (lowercase) → full 101-byte text. `cat naos/kernel.elf` → 35608 bytes truncated to 4K, `ELF` magic visible (70-cluster chain walk).

### Verified - 2026-10-09 (v0.1)

* Clean rebuild, zero warnings. Serial: `free=52440 pages` → `NaOS shell. Type help.` → `NaOS>`.
* Monitor `sendkey` session: `help` lists commands, `ver` shows `ticks=898` (timer IRQ live), `echo hi` echoes, `mem` reports `52442 pages (209768 KB)`, unknown commands rejected, `halt` stops.
* Editing: `Shift+H` uppercases, backspace erases on fb+serial (`Hab\b \bc` → `Hac`).
* Screenshot: black screen, white banner + live shell transcript, correct RGB order.
* P2 history preserved above; console is black since P4/P5 (was blue in P2).

### Verified - 2026-10-11 (ISO boot)

* `make iso` → `build/NaOS.iso` (67MB, El Torito UEFI, xorriso 1.5.8); `run-iso` (`-cdrom`) boots: bootloader loads `\NaOS\KERNEL.ELF` from CD, `NaOS v0.1.0 - kernel OK`, shell live (`version` correct).
* CD boot runs diskless by design (ATAPI CD-ROM unsupported): `blk: using ata` → `ata: no IDENTIFY response` → `fat: cannot read boot sector`, `list` → `fat: not ready`. No crash/hang; all non-disk commands work. ATAPI + USB mass storage are v0.2.0.

### Fixed - 2026-10-11 (ISO boots on strict firmware)

* VirtualBox EFI saw the CD but offered no boot entry: the ISO carried `BOOTX64.EFI` only inside the El Torito boot image. `mkiso.sh` now also ships `\EFI\BOOT\BOOTX64.EFI` as a real ISO file (the UEFI fallback path); our loader finds `\NaOS\KERNEL.ELF` on the same filesystem. Verified present on disc (14,601 bytes) + QEMU regression boot OK.

### Added - 2026-10-11 (live RAM disk: ISO boots with files)

* `tools/mkramdisk.sh` (40MB FAT32, PROGS + docs; `VERSION`-style FAT32 floor: 80,628 clusters verified via BPB) + `mkesp.sh` ships it as `\NaOS\RAMDISK.IMG`; `make esp` builds it via new `ramdisk` target.
* Bootloader (`boot/uefi/main.c`): pool-reads the image, copies into ≤4 page runs below 1G (`AllocateMaxAddress`), describes them in new `bootinfo.rd_base/rd_pages`; absent/broken file = silent disk boot (never fatal).
* Kernel (`kernel/drv/ramblk.h/.c`): memcpy backend over the runs (<1G guard, bounds-checked, multi-run walk). `blk` order is now AHCI → NVMe → probed ATA (new `ata_probe`: LBA0 must carry `0x55AA`) → RAM disk → legacy ATA. `pci` reports `ramdisk`.
* Fixed along the way: duplicate `mmd ::/NaOS` hung `make esp` on mtools' interactive autorename prompt — now existence-checked.
* Semantics (documented): real disks always win (HDD/NVMe persistence unchanged); RAM disk is last resort, writes live in RAM for the session. Give VMs ≥256MB.

### Verified - 2026-10-11 (live ISO)

* QEMU HDD boot: `RAMDISK live image ready` staged, `blk: using ahci`, FAT mounts — zero regression.
* QEMU CD boot: `blk: using ramdisk (live)`, `fat32: tot=81920`, `list` shows `PROGS/` + docs; `run /PROGS/hello.elf` → `hello from disk!`; `make-file /LIVE.TXT` persists for the session.
* VirtualBox (headless, EFI): same `ramdisk (live)` mount + FAT + shell; test VM deleted after.

### Added - 2026-10-11 (scrollback + font size)

* Console text ring (`kernel/drv/fb.c`): every printed char recorded with its colors (128 lines); live output still draws incrementally, scroll keys/font switches/clears repaint from the ring.
* `PgUp`/`PgDn` (PS/2 set-1 `0x49`/`0x51`, same codes via USB HID): one screen back/forward, view freezes while reading, any shell key rejoins live; flood-while-scrolled rejoins live instead of showing garbage.
* `font [normal|big]` command: 12x24 vs pixel-doubled 24x48, repaints instantly, persists via new `font=` key + `settings_save()` rewriting `/SETTINGS.TXT`; `settings` shows it; `help` documents both.

### Verified - 2026-10-11 (scrollback + font size)

* QEMU screendumps: post-`help` PgUp changes pixels, PgDn restores the live frame **byte-identical**, `font big` relaids out, `settings` shows `font=big (/SETTINGS.TXT)`.
* Reboot: `font=big` reloaded from file automatically; `font normal` restores defaults. Zero faults, zero warnings.

### Added - 2026-10-11 (font sizes + fast console)

* Sizes: `small` (authentic 8x16 DejaVu render, new `--tag` mode in `tools/font_render.py`, byte-identical default output proven), `normal`, `big`, custom `3`/`4` (Nx pixel scale). `font [small|normal|big|1|2|3|4]`, persisted as words in `/SETTINGS.TXT`.
* Lag fix: per-glyph 4-entry blend tables (12 divisions per glyph instead of ~900) + 32-bit screen fills + 32-bit pixel stores — repaint cost down several-fold on every size.
* Verified: `small`/`3` render + save + reload from file; all three layouts pixel-distinct; zero faults, zero warnings. (`big`/`normal` restore share the identical verified paths; interactive re-confirm pending.)

### Fixed - 2026-10-11 (scrollback viewport)

* The frozen viewport ignored the scroll distance (PgUp barely moved) and the back-cap measured from live totals (uint32 underflow past the oldest frozen line). Viewport now ends `view` lines back, capped against the frozen total; ring enlarged 128 → 256 lines so history stays until `clear`.
* Verified with screendumps over 3× `help`: two PgUps show two distinct older screens, two PgDns restore live byte-identical.

### Fixed - 2026-10-11 (QEMU machine pin)

* `make run` / `make run-iso` never passed `-machine q35`: stock QEMU boots the `pc` machine, whose OVMF drops to the EFI Internal Shell instead of auto-booting `\EFI\BOOT\BOOTX64.EFI` (reproduced headless; FS0 mounts fine, manual `FS0:` → `BOOTX64.EFI` path works). Both targets now pin `-machine q35`; README command block updated.

### Added - 2026-10-11 (updater phase 1 + about)

* `about` command: version/build, developer, links, licence.
* DNS client (`kernel/net/dns.h/.c`): A-record lookup over UDP with retries, compression-safe parsing, one CNAME chain followed; `dns` shell command (IP literals pass through).
* SHA-256 (`kernel/lib/sha256.h/.c`, streaming) with built-in `abc` selftest that gates every update operation.
* `fat_append()` (create-or-append, single consistent entry update) for streaming downloads.
* `update check <host> <port> </manifest>` (version compare, file list) + `update get <host> <port> </remote> </local> [sha]` (2KB chunks straight to FAT, incremental hash). Host may be IP or DNS name. Nothing auto-applies in phase 1 — the human reviews.
* Fixed along the way: SHA final-add used `f,g` for `h[4],h[5]` instead of `e,f` (caught by the selftest + host hashlib cross-check); `udp_recvfrom` lesson applied (payload-only).

### Verified - 2026-10-11 (updater)

* Against host HTTP server (user-net): manifest compare (`installed 0.1.0, available 0.2.0`), 62-byte download with **sha OK** vs python hashlib, file reads back identical.
* `dns github.com` → real address over the wire (140.82.121.4). Zero faults.

### Changed - 2026-10-12 (release assets + website)

* Release files renamed to `NaOS v0.1.0.iso` / `NaOS v0.1.0.img`; every download button and doc link updated (`%20`-encoded URLs). Build outputs keep canonical names.
* Website: doc links (README/CHANGELOG/IDEA/LICENCE) now open an in-page popup served from GitHub raw (copy + open-on-GitHub inside, plain-link fallback without JS).
* Fixed `browse.html` line gutter (missing `white-space:pre` ran all numbers onto one row).

### Added - 2026-10-12 (activity page)

* `activity.html`: live repo track — status counters (stars/forks/issues/last push), latest release with per-asset download counts, 20-commit feed with ages, manual refresh; partial-failure tolerant, rate-limit honest.

### Changed - 2026-10-12 (microscope pass: landing sections + numbers)

* Landing page rebuilt around 4 new spec sections (boot sequence, memory map, syscall table, honest limits) + renumbered 02–09; dossier system kept (Archivo Black + Plex Mono, hairlines, amber).
* Numbers pinned everywhere from source: artifact bytes, bases, ports, caps, counts (root README table, v0.1.0 README table, versions meta, v0.1.0 run notes, browse head).
* Fixed stale IDEA memory-map lines (own PML4 is done since P6).

### Changed - 2026-10-12 (website IA: folder-per-page URLs)

* Site restructured to `home/`, `browse/`, `activity/` folders (auto `index.html` each); root `index.html` is a redirect (meta + JS + manual link). `404.html` stays at root (Pages requirement).
* All five footers unified: tagline + © year nacer0s, socials, section links.

### Changed - 2026-10-12 (home/ IA + dotted artifact names)

* `browse/` + `activity/` moved under `home/` (`/home/browse/`, `/home/activity/`); all cross-links re-resolved and machine-checked.
* Build outputs renamed from `VERSION`: `NaOS.v0.1.0.img` / `.iso` (fixes an immediate-vs-deferred make trap: `VERSION` now defined above first use). `mkesp.sh`/`mkiso.sh` take paths as args.
* Release files renamed to match (`NaOS.v0.1.0.*`); every button/label/doc updated.

* NaOS File System v1 spec (in `kernel/fs/nafs.h` + `tools/mknafs.py`, kept in sync): 512B LE blocks, superblock (magic/version/root/checksum), 16×32B dir entries (case-sensitive names), contiguous files, nesting depth 8.
* `tools/mknafs.py` builds images from a staging dir; `make nafsdata` + `mkesp.sh` ship `/DATA.NAFS` on the ESP.
* Read-only kernel driver + `nafs mount/list/read` commands (image lives as a FAT file; all offsets bounds-checked, corrupt images error cleanly). Writes are v0.2.0.

### Verified - 2026-10-11 (NaFS)

* `nafs mount /DATA.NAFS` → label/blocks; `list` shows dirs + files; `read` prints text identical to the FAT originals. `dns github.com` → real address over the wire. Zero faults.

### Added - 2026-10-11 (firmware-shell auto-boot)

* `tools/mkesp.sh` writes `\startup.nsh` (ESP root; mirrored to the ISO tree): if firmware ever drops to its shell, it auto-runs after the countdown and boots the first volume carrying `\EFI\BOOT\BOOTX64.EFI` — no typing needed. Zero impact on the normal path (shell never starts there).

### Verified - 2026-10-11 (auto-boot)

* Default-`pc` QEMU, hands-free: serial shows the shell executing our script (`echo NaOS auto-boot...` → `for/if` → launch) followed by `NaOS v0.1.0 - kernel OK`. Manual `FS0:`/`cd`/`BOOTX64.EFI` steps obsolete.

### Added - 2026-10-11 (blinking cursor)

* Vertical-bar input cursor (`kernel/drv/fb.c` + shell idle loop): 2px at normal, 4px at big, ~4Hz, live-tail only, pixels-only (never in the ring or serial log). Any output hides it first; scroll/font/clear paths reset it.

### Verified - 2026-10-11 (cursor)

* QEMU screendumps ~0.6s apart at an idle prompt differ with zero input (bar toggling; prompt otherwise stable). No other spontaneous redraws exist, so the delta is the cursor.
* Heisenbug on record (host-side, image exonerated): one evening, 4 consecutive q35 boots of a known-good image all dropped to shell; fresh `make esp` + 3 straight boots passed, and the identical save history (`font big`/`normal`) rebooted clean. `mdir` + same-history A/B rule out image corruption — busy-host BDS disk-discovery race. README documents the 3-command shell fallback.

### Added - 2026-10-11 (website, viewer, licence)

* Vanilla HTML/CSS/JS site: root `index.html` (hero + live-typing boot terminal, features, commands, build), `versions/index.html` (release catalog), `versions/v0.1.0/index.html` (command reference, run guide, limits), `404.html` ("lost sector"), `browse.html` (GitHub-API source viewer — read every file online, no install). Clean directory URLs, scroll-reveal, back-to-top, copy buttons, social footers.
* `LICENCE.md` replaced (MIT → custom source-available, non-commercial): free personal/learning use, no commercial use, no re-hosting as your own version, attribution to nacer0s required everywhere in public.
* Root + v0.1.0 READMEs: download links (`NaOS.iso` release asset), website/viewer links, hardware limits, author + socials.

### Fixed - 2026-10-12 (framebuffer mapping beyond 4G)

* Real hardware (fb at 0x4000000000) hung after 3 lines: `vmm_init` mapped the fb to VA `2G+((phys>>21)&511)*2M`, identical to identity only inside [2G,3G) — every QEMU run. First post-CR3-switch print faulted silently (those 3 lines print pre-switch).
* Fix: fb pages identity-mapped at any address via the generic MMIO path (also uncacheable now, was write-back); fixed 2G window, its task entry, and the `pti==2` special case all removed.
* Task spaces mirror every kernel MMIO mapping at create/fork time (supervisor-only, shared pages) so syscalls reach fb + devices — this also fixes program file I/O on NVMe-only disks (above-4G BAR under task tables faulted before).
* Verified: q35 HDD (hello/fdemo/forkdemo incl. CoW), CD live boot + mount, NVMe-only boot + selftest + `fdemo`, zero faults, zero warnings.

### Fixed - 2026-10-13 (real hardware: refuse unknown NICs)

* Bare metal (Intel LAN at 0:1f.6, I219-class) hung inside `e1000_init`: 8254x-era register pokes at the wrong chip turn bounded MMIO polls into forever on dead registers. Driver now reads the PCI device ID first and drives only 0x100E/0x10D3 — anything else prints `unsupported … net down` and boot continues (net is optional; boot first). Rule going forward: never touch unknown hardware.
* Same photo proved the vmm fix on silicon (XHCI 64-bit BAR 0x601d100000 mapped+read, ramdisk live, FAT mounted) and `acpi: no FACP` is graceful (poweroff unavailable there, nothing hangs).
* Verified: e1000e (82574) still inits + net up on QEMU, zero warnings.

### Fixed - 2026-10-13 (real hardware round 3: live files + USB)

* Foreign disk (e.g. Windows SSD/GPT) made `fat_init` fail with a cryptic sector-size message and left the shell diskless. Now: clearer `not a FAT volume` message, `g_alloc_hint` reset (stale hints across volumes would corrupt), and `fat_init` failure falls back to the staged live image (`blk_use_ramdisk`) — real machines boot into a full live session with files.
* USB on real iron: added the BIOS->OS ownership handoff (USBLEGCTLSTS, fully guarded — absent on QEMU, skipped silently) so port resets stop fighting firmware; tick-time rescans are quiet now (boot + manual `usb` stay verbose).
* Verified: normal HDD boot unchanged (no fallback), corrupt-BPB boot falls back to ramdisk + FAT + shell, QEMU USB keyboard still enumerates with no takeover line, e1000e unaffected, zero warnings.

### Fixed - 2026-10-13 (real hardware round 5: live image staging)

* USB-stick boot reached the shell but `list` said `fat: not ready`: the bootloader staged the 40MB live image with one 40MB pool allocation + one 40MB firmware Read, which stingy real firmware pools refuse — so no live fallback existed (USB stick is invisible to ATA/AHCI, internal SSD is foreign). Fix: new `file_open_stream()` (`boot/uefi/file.c/.h`, open + size without the giant alloc; `file_read_all` reuses it, kernel load unchanged) and `load_ramdisk()` now allocates the page runs first, then streams the file in 1MB pool chunks (also fixes a latent memset-past-allocation when the size isn't a multiple of 4 runs). Each failure step prints its own `RAMDISK <reason>: disk boot` line.
* Kernel prints `ramdisk: live image staged/absent` at boot, and when FAT fails with no live image it says `fat: no usable disk (live image absent)` instead of going quiet until `list`.
* Cursor report checked: the idle blink works as designed (proven in QEMU — 48px bar, ~500ms period, only on the live tail; it rests while scrolled back with PgUp/PgDn until any key). If it ever looks static on iron, run `uptime` twice: frozen time means IRQ0 never fires on that box (report it), counting time means the bar is blinking at the prompt end.
* Verified: HDD boot (ahci + 9 entries), CD-only boot (`blk: using ramdisk (live)` + 4 entries), CD + blank-disk boot (`fat: bad boot signature` → `falling back to live image` → 4 entries), zero warnings (`-Werror` both toolchains).

### Fixed - 2026-10-13 (cursor blink on wall clock)

* The idle cursor blinked off the PIT tick counter (4Hz phases). On some real hardware the blink froze after boot (timer-rate quirks: too fast reads as solid, too slow as static), while QEMU always looked right. Fix: new `rtc_second()` (`kernel/drv/rtc.c/.h`, UIP-guarded single CMOS read, -1 when no clock) drives the blink with 1-second wall-clock phases; PIT toggle kept as fallback when no RTC. Typing/`time` verified unaffected.

### Fixed - 2026-10-14 (cursor blinks through output, never goes stale)

* Every output char hid the bar, so during any program output it looked dead (each char reset it to off). Now `fb_putc`/`fb_backspace` only sync the flag (the glyph repaints the cell and covers the bar); only a cursor MOVE without drawing (newline/CR/wrap) erases stale pixels. `read_line` no longer hides on entry/keypress. Invariant: flag set ⟺ bar pixels present (repaint/clear wipe + clear it). Verified: `help` text intact with no stale bars, backspace round-trip exact, RTC phases still flip 48px.

### Fixed - 2026-10-14 (blink survives dead timers + input floods)

* Real iron still shows a static cursor: it follows text but never toggles idle. Two remaining suspects, both closed: (1) broken RTC *and* odd PIT would pin the phase — new `pit_tsc()` fallback (`kernel/drv/pit.c/.h`, raw `rdtsc`, always advances while the CPU runs; bit 32 ≈ 1–4s phases) replaces the PIT fallback, so all three timebases would have to fail at once; (2) a garbage-input trickle could starve the old empty-only branch — the phase now evaluates every loop pass, not just when idle. `rtc_second()` also caches a missing clock instead of burning 100K port reads per pass. Verified both paths flip 48px in QEMU (TSC path via forced-off RTC test, then reverted).

### Fixed - 2026-10-14 (500ms blink ladder + backspace keeps the bar single)

* Two cursor complaints from real iron: (1) delete left a second static bar — `fb_backspace` cleared the flag without erasing the old cell before moving left; now it hides first, so the flag⟺pixels invariant holds on every path (verified: typed/erased text exact, single bar at the live cell). (2) blink felt slow/frozen — the ladder is now PIT-led at exact 500ms phases (auto-checked each RTC second, RTC parity when PIT stalls, TSC bit 31 with no clock), evaluated every loop pass so floods can't starve it.
* Measurement note for future screenshots: ~1s-spaced captures period-lock with a 500ms blink and read as static; serial tick logs (exactly 100/s) plus off-resonance captures are the ground truth. Both used here.

### Fixed - 2026-10-14 (ACPI tables above 1G + real shutdown)

* `shutdown` printed `no ACPI, halting` even where ACPI exists: `find_table` skipped every table at/above 1G (`addr >= 0x40000000`), but modern firmware (and QEMU with 2G+ RAM) parks tables high — reproduced with `-m 8G` (`no FACP`). Fix: `map_range()` maps table pages on demand via the VMM (bounds + canonical + size guards), applied to the RSDP hint, RSDT/XSDT walk, and DSDT; `acpi_init` moved after `vmm_init` in `kmain` (mappings need PMM + own tables). Second bug in the same walk: X_DSDT lives at FACP+132, not +124 (that offset is X_FIRMWARE_CTRL — a FACS pointer misread as "no DSDT").
* Verified: `-m 8G` goes `no FACP` → `poweroff ready (PM1 604/0 SLP 0/0)`; `shutdown` powers QEMU off (process exits, no screenshot left); `-m 256M` path unchanged. Boxes genuinely without FACP keep the honest halt message.

### Added - 2026-10-14 (Ctrl+C cancels the line)

* PS/2 (`0x1D`) and USB-HID (`E01D`) Ctrl tracking in `ps2_kbd.c`; Ctrl+letter yields the control code, other Ctrl combos are ignored so they can never type. `read_line` treats `0x03` as cancel: erases the echo, prints `^C`, fresh prompt (works in `edit` too, same input loop). `help` documents it. Verified: typed `ab`, Ctrl+C left a clean `^C` line, `version` ran fine after.

### Fixed - 2026-10-14 (website fully responsive)

* Tables (boot chain, memory map, syscalls, limits, requirements) now scroll inside their `.tbl` wrappers instead of blowing out narrow screens (the reported issue); the one bare table got wrapped too. File-tree rows ellipsis long names; activity stats stack to one column under 520px with correct dividers. Everything else already collapsed properly (verified by audit: grids, modals, nav burger, clamp headings, wrapping buttons). No pixel screenshots available on this machine — fixes are additive (media queries + overflow containers) and desktop-neutral.

### Fixed - 2026-10-14 (cursor rewritten stateless from scratch)

* Root causes found, not patched around: (1) `read_line` parked inside blocking `kbd_getc` on any input glitch, freezing the old loop-tied blink eval — the blink now also runs from the PIT tick (IRQ, independent of input state) and from `getc`'s own wait; (2) the old show/hide flag could strand pixels, doubling the bar — there is no flag anymore.
* New design (`kernel/drv/fb.c`, `fb.h`, `shell.c`, `task.c`, `ps2_kbd.c`): `caret_refresh()` repaints the caret cell from the text ring (input char or blank) plus the bar per blink phase — every paint derives from ring state, so stale/double is impossible by construction. Moves repaint the old cell from ring state first (committed line for newline/wrap, input line for CR, blank neighbor for backspace). Blink ladder unchanged (PIT-checked 500ms, RTC, TSC). Repaint/clear end with a refresh; history view stays bar-free.
* Verified: 48px alternation off-resonance, single-bar column proof (one run max, never two) at the live cell after type/erase, `help`/`font`/`list` output exact, zero warnings.

### Fixed - 2026-10-13 (live session: every command verified on ISO)

* `nafs mount` had nothing to mount on a live boot: the ramdisk never shipped `/DATA.NAFS` (only the ESP had it). `tools/mkramdisk.sh` now copies `build/data.nafs` like `mkesp.sh` does.
* Full command matrix verified on CD-only live boot (ramdisk): list/read/make-file/edit(.save round-trip)/make-folder/copy/move/rename/delete/go (incl. `..`, `/`, non-empty refusal), run (hello.elf/counter/ticker ELF+builtin), tasks/kill (slot reaped), nafs mount/list/read (NaFS stays case-sensitive by design: `/README.TXT`), settings/font (saved to RAM), `version > v.txt` redirect round-trip, fdemo file syscalls (`/HELLO.TXT`), dhcp/net/ping 4-4/dns (real WAN answer)/fetch (host server, 34B)/update check (0.1.0 vs 0.2.0 manifest)/update get ±sha (`sha OK`, files persist on RAM FAT)/beep/memory/test/print/about/pci (`disk driver: ramdisk`)/usb+nvme (graceful absent)/reboot (back to live shell)/shutdown (`no ACPI, halting`). `crash` intentionally untested live (verified pre-freeze; it reboots by design).

### Fixed - 2026-10-13 (build order: ramdisk needs data.nafs first)

* Clean `make clean && make iso` silently shipped a ramdisk WITHOUT `/DATA.NAFS`: `esp: all ramdisk nafsdata` builds the ramdisk before `data.nafs` exists, so the new copy block skipped. Fix: `ramdisk: nafsdata` (which already pulls `progs`). Incremental builds were unaffected (stale `data.nafs` present), which is why verification passed. Proven with a from-scratch rebuild + live `nafs mount/list`.

### Fixed - 2026-10-13 (ramdisk staging reports its reason)

* Real-hardware ISO boot showed `ramdisk: absent` with no reason (USB stick invisible to ATA, SSD foreign, no fallback). The bootloader now reports WHY through the handoff: new `rd_status/rd_done/rd_size` in `common/bootinfo.h` (0=file absent 1=ready 2=page-alloc 3=buffer 4=read-fail+counts); the kernel prints `ramdisk: absent (<reason>)`, with byte counts on read failure so a firmware size cliff shows its location. The chunk loop also rides through short reads now (progress counts; only hard errors/EOF stop). Verified: normal boot prints staged, file-less boot prints `absent (file not found)` end to end.

### Fixed - 2026-10-14 (ramdisk open failure pinpointed)

* The new reason line on real iron says `absent (file not found)`: the open itself fails while `KERNEL.ELF` opens fine moments earlier. `file_open_stream()` now reports its stage, the handoff carries `rd_status` 5/6/7 + the failing EFI status (`rd_estatus`), and the kernel prints e.g. `ramdisk: absent (open failed, efi 0x...)`. The status value names the firmware's refusal (not-found vs denied vs ...), which picks the fix (layout vs attributes vs ...). Verified normal path unchanged (staged, zero warnings).

### Fixed - 2026-10-14 (one shared volume root for the whole boot)

* Real iron reports `open failed, efi 0x8000000000000000`: not a real UEFI code (value 0 + error bit = garbage), from the *second* file open on the volume — the classic shape of a firmware whose second `OpenVolume` goes stale after the first root was closed. `file_open_stream()` now opens the volume once into a static root shared by kernel + ramdisk loads and never closes it (ExitBootServices tears down anyway). Verified QEMU boots + stages + shells as before, zero warnings.

### Fixed - 2026-10-14 (ramdisk loads first + volume retry)

* Real iron now returns a clean `EFI_NOT_FOUND` for the ramdisk open while the kernel opens fine — same volume, same directory. Remaining suspects are order/state quirks, so the bootloader now stages the optional image FIRST (kernel load is unchanged and still fatal), and a failed ramdisk open retries once on a fresh volume root without closing the old one (new `file_reset_root()`; close path suspected). Status 8 + `(after retry)` in the staged message (and a `RAMDISK retry...` line) tell which path worked. Verified QEMU stages first-try with the new order, zero warnings.

### Fixed - 2026-10-14 (live image = whole-volume block copy)

* File opens for the 40MB image are firmware quicksand (NOT_FOUND/garbage while small files open fine), so the bootloader no longer opens it at all: `load_ramdisk` copies the whole boot volume via Block I/O into the page runs — no filenames, no per-file opens, no size-at-open issues. New minimal `EFI_BLOCK_IO_PROTOCOL` types in `boot/uefi/uefi.h`; known-size reads (`RD_VOL_BYTES` = ESP bytes, must match `mkesp.sh`) with FAT boot-signature validation, so wrong views (ISO9660 fallback, whole-stick exposure) are discarded like an absent image. `\NaOS\RAMDISK.IMG` retired: `tools/mkramdisk.sh` deleted, `esp` drops the `ramdisk` prereq, ESP/ISO slim down ~40MB. Kernel side untouched (mounts the copy as before).
* Bonus catch while testing: the CD BlockIo reports unit-confused sizes (~270MB for a 67MB ISO), which the old reported-size check would refuse — fixed-size reads + content validation ignore the numbers and verify the bytes instead.
* Verified: HDD boot stages 64MB + ahci shell; CD boot stages + `blk: using ramdisk (live)` + `list` shows all 8 ESP entries; zero warnings.

### Fixed - 2026-10-13 (real hardware round 4: framebuffer speed)

* Bare metal scrolled one line per eternity: every scroll did a full-screen pixel `memmove` that READS the framebuffer back over PCIe (uncacheable mapping, no read combining) plus per-pixel writes — QEMU never notices (RAM-speed VRAM), real GPUs crawl. Fix: `vmm_init` programs PAT index 2 as write-combining and maps the fb with `vmm_map_wc` (drivers stay uncacheable via `vmm_map_mmio`; RAM stays write-back), and `fb_putc` no longer moves pixels at all — newlines advance the cursor, overflowing only into the canonical `repaint()` (already used by clear/font/PgUp/PgDn), which writes sequentially and never reads VRAM.
* Typing/Enter/clear cost one glyph or one sequential fill; a full scroll costs one repaint (~30 glyph rows + bg fill, all WC writes). QEMU output pixel-identical (`help` + `clear` screendumps).
* Verified: zero warnings (`-Werror`), `check.sh ALL OK`, QEMU `help` renders fully through multiple overflow repaints, `clear` clean, prompt live. Needs a real-hardware reflash photo to confirm the lag is gone.
