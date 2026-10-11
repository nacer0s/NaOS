#!/bin/sh
# tools/mkesp.sh — build 64M FAT ESP from scratch (no prebuilt image).
# Layout: \EFI\BOOT\BOOTX64.EFI (+ \NaOS\KERNEL.ELF from P2 on).
set -e
BUILD=build
IMG=$BUILD/NaOS.img
EFI=$BUILD/BOOTX64.EFI
mkdir -p $BUILD
if [ ! -f "$EFI" ]; then echo "ERROR: $EFI missing — run 'make bootloader' first"; exit 1; fi
rm -f $IMG
# 64M zeroed image, FAT32, label NaOS
dd if=/dev/zero of=$IMG bs=1M count=64 status=none
mformat -i $IMG -F -v NaOS ::
mmd -i $IMG ::/EFI ::/EFI/BOOT || true
mcopy -i $IMG $EFI ::/EFI/BOOT/BOOTX64.EFI
if [ -f "$BUILD/KERNEL.ELF" ]; then
  mmd -i $IMG ::/NaOS || true
  mcopy -i $IMG $BUILD/KERNEL.ELF ::/NaOS/KERNEL.ELF
fi
if ls $BUILD/progs/*.elf >/dev/null 2>&1; then
  mmd -i $IMG ::/PROGS || true
  mcopy -i $IMG $BUILD/progs/*.elf ::/PROGS/
fi
# Live-boot image for BOOTX64.EFI (last-resort disk when no drive mounts).
if [ -f "$BUILD/ramdisk.img" ]; then
  mdir -i $IMG ::/NaOS >/dev/null 2>&1 || mmd -i $IMG ::/NaOS
  mcopy -i $IMG $BUILD/ramdisk.img ::/NaOS/RAMDISK.IMG
fi
# Demo text file for the `cat` command (our own content, 8.3 name).
printf 'Hello from the NaOS disk!\r\nThis file lives on the FAT32 ESP image.\r\nTry: go naos, list, read README.TXT\r\n' > $BUILD/README.TXT
mcopy -i $IMG $BUILD/README.TXT ::/README.TXT
# Demo file with a long filename (exercises the kernel LFN reader).
printf 'If you can read this by its long name, LFN works.\r\nTry: read "long-file-name-demo.txt"\r\n' > "$BUILD/long-file-name-demo.txt"
mcopy -i $IMG "$BUILD/long-file-name-demo.txt" "::/long-file-name-demo.txt"
# Default settings (prompt + console theme). Edit it in NaOS with `edit`.
printf '# NaOS settings. Lines are key=value (# starts a comment).\r\n# prompt: shell prompt word. fg/bg: console colors as RRGGBB hex.\r\nprompt=NaOS\r\nfg=FFFFFF\r\nbg=000000\r\n' > $BUILD/SETTINGS.TXT
mcopy -i $IMG $BUILD/SETTINGS.TXT ::/SETTINGS.TXT
echo "esp: $IMG ($(stat -c%s "$IMG" 2>/dev/null || stat -f%z "$IMG") bytes)"
mdir -i $IMG ::/EFI/BOOT || true
