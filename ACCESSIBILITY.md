# Accessibility — NaOS

Everyone should be able to boot NaOS and use it. This file states the
commitment and where things stand; per-version feature notes live next to
the code: [versions/v0.1.0/ACCESSIBILITY.md](versions/v0.1.0/ACCESSIBILITY.md).

## Commitment

Accessibility bugs are treated like functional bugs: report one and it
gets fixed in the newest development version, same as any other defect.
No separate track, no lower priority.

## Scope

* **The OS console** (the `NaOS>` shell): keyboard-only by design, readable
  defaults, adjustable sizes and colors, audio feedback where it matters.
* **This website** (`home/`): static content, no accounts, no forms, works
  with keyboard alone. It does use animation (typing hero, scroll-reveal);
  if any of it blocks you, tell me and I will fix or gate it.
* **The docs** (READMEs, guides): plain Markdown, no information locked
  inside images alone.

## What we do NOT claim (honest version)

* No WCAG conformance level — the project has never been audited against
  one. The statement above is intent plus the concrete features listed per
  version, not a certificate.
* No on-device screen reader or speech output. The console mirrors
  everything to the serial port (COM1), so a second machine can capture
  and read the full session — that bridge is tested on every boot path.
* No mouse or pointer support anywhere (keyboard-only is the design, and
  every command is reachable from the keyboard).

## Reporting a barrier

1. **Email:** nacer.eddine.dev@gmail.com with subject `NaOS accessibility`,
   **or** open a public GitHub issue (access barriers are not secrets —
   public is fine and helps others).
2. Include: NaOS version (`version` output), what you tried, what stopped
   you, and what you use (keyboard layout, screen reader on a serial
   console, high-contrast needs, ...). Photos of the screen are welcome.
3. I confirm, fix in the newest development version, note it in that
   version's `CHANGELOG.md`, and credit you unless you decline.
