# Contributing to NaOS

Thanks for wanting to help. NaOS is a hobby x86_64 UEFI OS written from
scratch in C — small codebase, high standards, plain-English shell included.

This file is global: the process below applies to every version. Version
code lives under `versions/vX.Y.Z/`; each version documents its own changes
in its `CHANGELOG.md` / `IDEA.md`.

## What to contribute

* **Code:** kernel, bootloader, user programs, tools (`versions/v0.1.0/`
  shows the layout: `boot/`, `kernel/`, `common/`, `progs/`, `tools/`).
* **Docs & website:** guides, `home/` pages, per-version `index.html`.
* **Bug reports:** with a repro. A QEMU command line + serial log or
  screenshot is perfect; real-hardware reports should include the boot
  messages (`PgUp` scrolls back) and the exact failing line.

## One-time setup

Windows + MSYS2 UCRT64, cross-compiler in `/opt/cross`, QEMU + OVMF —
follow [versions/v0.1.0/README.md](versions/v0.1.0/README.md#one-time-setup-msys2-ucrt64),
then confirm the gate passes:

```bash
bash tools/check.sh   # expect: ALL OK
```

## Ground rules

* **Zero prebuilt OS code** in `boot/` and `kernel/`. Host toolchain
  (compilers, mtools, Python, fonts) is allowed; OS code is always ours.
* **Zero warnings:** `-Wall -Wextra -Werror` on all kernel/boot code.
  If it warns, it doesn't merge.
* **Verify by execution, not by reading.** Boot it in QEMU, run the
  affected commands, check serial output and screenshots. Say what you ran.
* **Log every change** in that version's `CHANGELOG.md` (and `IDEA.md`
  when it changes the plan) — same commit, short entries, dated.
* **Focused commits:** one concern per commit, short imperative subject
  (`Fix fb mapping beyond 4G`, not `stuff`), stage only intended files.
  No secrets, no binaries, no `build/` output in commits.
* **Security first:** malformed input (packets, descriptors, images, FAT
  structures) must fail safe, never trust first. See [SECURITY.md](SECURITY.md).

## Licence (read this before sending anything)

Contributions merge under [LICENCE.md](LICENCE.md) §4: source-available,
non-commercial. By sending a pull request or patch you confirm it is your
own work and grant the project the rights to use it under that licence.

## How to submit

1. Fork, branch from `main`, keep the branch to one concern.
2. Open a pull request against `main` describing **what** changed and
   **how you verified it** (commands run, QEMU results, hardware if any).
3. Expect review on correctness first, style second. Small PRs merge fastest.

## Conduct

Be respectful and technical. No harassment, no spam, no drive-by
commercial pitches — this is a non-commercial learning project and the
tone stays that way.

## Response time

One maintainer, hobby hours: best effort, no SLA. Security reports jump
the queue — see [SECURITY.md](SECURITY.md) (acknowledged within 3 days).
