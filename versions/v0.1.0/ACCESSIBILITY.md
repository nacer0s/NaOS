# Accessibility Notes — NaOS v0.1.0

Global commitment and how to report: [../../ACCESSIBILITY.md](../../ACCESSIBILITY.md).
Rule: barriers are bugs — report them like any other defect.

## What works in this version

* **Keyboard-only, fully.** PS/2 and USB HID keyboards; there is no mouse
  anywhere, so nothing is reachable-only-by-pointer. Every command runs
  from the `NaOS>` prompt.
* **Plain-English commands.** `list`, `read`, `make-file`, `go`, `copy`,
  `run` — no Unix-isms to memorize; `help` lists everything with examples.
* **Adjustable console size.** `font small | normal | big | 3 | 4`
  (8x16 and 12x24 DejaVu Sans Mono, scaled x1–x4), saved across reboots
  in `/SETTINGS.TXT`.
* **Adjustable colors.** `fg=`/`bg=` in `/SETTINGS.TXT` as RRGGBB hex.
  Default is white on black (high contrast); if you change them, keep the
  contrast strong — nothing stops you picking unreadable pairs.
* **Reading back.** 256-line scrollback with `PgUp`/`PgDn` (PS/2 and USB),
  so output never scrolls away before you finish it.
* **Orientation aids.** Blinking cursor (1s phases) that tracks input and
  output; `clear` for a fresh screen; every destructive action confirms
  in words (`saved`, `deleted`, `renamed`, ...).
* **Audio feedback.** `beep` drives the PC speaker — usable as a sound
  check and completion signal where hardware has a speaker (many modern
  laptops have none; the command still returns normally).
* **Serial mirror.** The whole session (banner, commands, output, errors)
  is also printed to COM1, so a second machine can capture and read
  everything with a screen reader. Verified on HDD, ISO and live boots.

## Known limits (accepted for v0.1.0)

* No on-device screen reader or speech; the serial mirror is the bridge.
* No pointer/mouse support (by design, not an oversight).
* No display scaling beyond the font sizes; cells are fixed 8x16 / 12x24.
* No reduced-motion setting; the only animation is the 1s cursor blink.
* No preset high-contrast themes beyond the default; custom `fg=`/`bg=`
  pairs are your responsibility.
* `font`/`settings` save to disk on HDD installs, to RAM only on live
  USB/CD sessions (choices reset on reboot there).

If any of the above stops you, that is a report, not an insult — thank you.
See [CHANGELOG.md](CHANGELOG.md) for the accessibility fix history so far.
