#!/bin/sh
# tools/mkramdisk.sh — 40M FAT32 RAM-disk for live boot.
# Loaded into memory by BOOTX64.EFI (\NaOS\RAMDISK.IMG); the kernel mounts
# it as a last-resort disk, so CD/USB boot has working files (RAM session).
# Contents mirror the ESP docs+progs (TXT blocks kept in sync with mkesp.sh).
set -e
BUILD=build
RD=$BUILD/ramdisk.img
if ! ls $BUILD/progs/*.elf >/dev/null 2>&1; then
  echo "ERROR: progs missing — run 'make progs' first"; exit 1
fi
mkdir -p $BUILD
rm -f $RD
# 40M: smallest round size that still formats FAT32 (needs >=65525 clusters).
dd if=/dev/zero of=$RD bs=1M count=40 status=none
mformat -i $RD -F -v NaOSRD ::
mmd -i $RD ::/PROGS || true
mcopy -i $RD $BUILD/progs/*.elf ::/PROGS/
# NaOS-FS demo image (keep in sync with mkesp.sh): without it the `nafs`
# command has nothing to mount on a live boot.
if [ -f "$BUILD/data.nafs" ]; then
  mcopy -i $RD $BUILD/data.nafs ::/DATA.NAFS
fi
# Same demo docs as the ESP (keep in sync with mkesp.sh).
printf 'Hello from the NaOS disk!\r\nThis file lives on the FAT32 ESP image.\r\nTry: go naos, list, read README.TXT\r\n' > $BUILD/README.TXT
mcopy -i $RD $BUILD/README.TXT ::/README.TXT
printf 'If you can read this by its long name, LFN works.\r\nTry: read "long-file-name-demo.txt"\r\n' > "$BUILD/long-file-name-demo.txt"
mcopy -i $RD "$BUILD/long-file-name-demo.txt" "::/long-file-name-demo.txt"
printf '# NaOS settings. Lines are key=value (# starts a comment).\r\n# prompt: shell prompt word. fg/bg: console colors as RRGGBB hex.\r\nprompt=NaOS\r\nfg=FFFFFF\r\nbg=000000\r\n' > $BUILD/SETTINGS.TXT
mcopy -i $RD $BUILD/SETTINGS.TXT ::/SETTINGS.TXT
echo "ramdisk: $RD ($(stat -c%s "$RD" 2>/dev/null || stat -f%z "$RD") bytes)"
