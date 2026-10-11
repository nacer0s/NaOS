#!/bin/sh
# tools/mkiso.sh — wrap the FAT ESP image in a UEFI El Torito ISO.
# Needs: xorriso (pacman -S xorriso). No MBR/CSM: UEFI-only, by design.
set -e
BUILD=build
IMG=$BUILD/NaOS.img
ISO=$BUILD/NaOS.iso
ROOT=$BUILD/iso
if [ ! -f "$IMG" ]; then echo "ERROR: $IMG missing — run 'make esp' first"; exit 1; fi
if ! command -v xorriso >/dev/null 2>&1; then
  for _x in /usr/bin/xorriso /c/msys64/usr/bin/xorriso.exe \
            "C:/msys64/usr/bin/xorriso.exe"; do
    if [ -x "$_x" ]; then XORRISO="$_x"; break; fi
  done
fi
XORRISO=${XORRISO:-xorriso}
if ! command -v "$XORRISO" >/dev/null 2>&1 && [ ! -x "$XORRISO" ]; then
  echo "ERROR: xorriso missing — run 'pacman -S xorriso' in MSYS2"
  exit 1
fi
rm -rf "$ROOT" "$ISO"
mkdir -p "$ROOT/boot"
cp "$IMG" "$ROOT/boot/esp.img"
# Also ship the kernel + programs as plain ISO files (handy, not booted).
mkdir -p "$ROOT/NaOS"
cp "$BUILD/KERNEL.ELF" "$ROOT/NaOS/" 2>/dev/null || true
cp "$BUILD/BOOTX64.EFI" "$ROOT/NaOS/" 2>/dev/null || true
cp "$BUILD/progs/"*.elf "$ROOT/NaOS/" 2>/dev/null || true
"$XORRISO" -as mkisofs -o "$ISO" -V NaOS \
  -e boot/esp.img -no-emul-boot \
  -r -J "$ROOT"
echo "iso: $ISO ($(stat -c%s "$ISO" 2>/dev/null || stat -f%z "$ISO") bytes)"
echo "boot it: qemu with -cdrom, or write to USB (Rufus, DD mode) and UEFI-boot it."
