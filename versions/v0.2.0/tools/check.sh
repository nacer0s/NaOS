#!/bin/sh
# tools/check.sh — fail fast if any NaOS build requirement is missing.
set -e
ok=1
need() { if ! command -v "$1" >/dev/null 2>&1; then echo "MISSING: $1 ($2)"; ok=0; else echo "ok: $1 $(command -v "$1")"; fi; }
need x86_64-w64-mingw32-gcc "bootloader PE+ compiler (MSYS2 ucrt toolchain)"
# Cross-compiler may live in MSYS2 /opt/cross but this shell may mount it at /c/msys64/opt/cross.
# Add both candidates to PATH non-destructively for the check.
for _c in /opt/cross/bin /c/msys64/opt/cross/bin "C:/msys64/opt/cross/bin"; do
  if [ -x "$_c/x86_64-elf-gcc.exe" ] || [ -x "$_c/x86_64-elf-gcc" ]; then
    case ":$PATH:" in *":$_c:"*) ;; *) export PATH="$_c:$PATH";; esac
  fi
done
need x86_64-elf-gcc "/opt/cross cross compiler — MSYS2: export PATH=/opt/cross/bin:\$PATH"
need x86_64-elf-ld "/opt/cross binutils"
need nasm "kernel asm stubs"
need make "build"
need mformat "mtools ESP creation"
need mcopy "mtools ESP copy"
need python "helpers"
need qemu-system-x86_64 "emulator (winget QEMU.QEMU or msys qemu)"
# Console font: DejaVu Sans Mono rasterized at build time via Pillow.
for _py in /ucrt64/bin/python "C:/msys64/ucrt64/bin/python.exe" python3; do
  if "$_py" -c "import PIL" >/dev/null 2>&1; then echo "ok: Pillow ($_py)"; PIL_OK=1; break; fi
done
if [ -z "$PIL_OK" ]; then echo "MISSING: Pillow (pacman -S mingw-w64-ucrt-x86_64-python-pillow)"; ok=0; fi
for _ttf in /ucrt64/share/fonts/TTF/DejaVuSansMono.ttf "C:/msys64/ucrt64/share/fonts/TTF/DejaVuSansMono.ttf"; do
  if [ -f "$_ttf" ]; then echo "ok: font $_ttf"; TTF_OK=1; break; fi
done
if [ -z "$TTF_OK" ]; then echo "MISSING: DejaVuSansMono.ttf (pacman -S mingw-w64-ucrt-x86_64-ttf-dejavu)"; ok=0; fi
if command -v xorriso >/dev/null 2>&1; then echo "ok: xorriso (ISO builds)"; else echo "note: no xorriso — 'make iso' needs 'pacman -S xorriso' (optional)"; fi
for f in "C:/Program Files/qemu/share/edk2-x86_64-code.fd" /ucrt64/share/qemu/edk2-x86_64-code.fd /mingw64/share/qemu/edk2-x86_64-code.fd /usr/share/qemu/edk2-x86_64-code.fd; do
  if [ -f "$f" ]; then echo "ok: OVMF $f"; OVMF_FOUND=1; break; fi
done
if [ -z "$OVMF_FOUND" ]; then echo "MISSING: OVMF edk2-x86_64-code.fd"; ok=0; fi
x86_64-elf-gcc --version | head -n1
x86_64-w64-mingw32-gcc --version | head -n1
if [ "$ok" = "1" ]; then echo "check: ALL OK"; else echo "check: FAILED"; exit 1; fi
