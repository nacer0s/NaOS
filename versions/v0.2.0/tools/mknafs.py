#!/usr/bin/env python3
"""NaOS tools/mknafs.py — build a NaOS-FS v1 image from a staging dir.

NaOS-FS v1 (little-endian, 512B blocks), canonical spec (also in
kernel/fs/nafs.h — keep the two in sync):
  block 0 (superblock):
    u32 magic 0x5346414E ("NAFS"), u32 version 1, u32 total_blocks,
    u32 root_block, char label[16] (NUL-padded),
    bytes 32..507 reserved (0), u32 checksum (sum of bytes [0,508) mod 2^32)
  dir block: 16 entries x 32B:
    char name[20] (NUL-terminated, case-sensitive), u8 type (0 free,
    1 file, 2 dir), u8 reserved[3], u32 start_block, u32 size_bytes
  file data: contiguous blocks from start_block (last partial ok).
Subdirectories nest (kernel caps lookup depth at 8). Read-only in v0.1.0.

Usage: mknafs.py --out IMG --label NAME STAGEDIR
"""
import argparse
import os
import struct
import sys

MAGIC = 0x5346414E
VERSION = 1
DIR_ENTRIES = 16


def die(msg):
    print(f"mknafs: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def build_dir(path):
    """Returns (entries, blobs) for a staging dir; recurses for subdirs."""
    try:
        names = sorted(os.listdir(path))
    except OSError as e:
        die(f"list {path}: {e}")
    entries = []  # (name, type, blob-or-subentries)
    for nm in names:
        if len(nm.encode("utf-8", "ignore")) > 19:
            die(f"name too long: {nm}")
        full = os.path.join(path, nm)
        if os.path.isdir(full):
            entries.append((nm, 2, build_dir(full)))
        elif os.path.isfile(full):
            with open(full, "rb") as f:
                entries.append((nm, 1, f.read()))
        # anything else (links, fifos): skipped silently
    if len(entries) > DIR_ENTRIES:
        die(f"{path}: more than {DIR_ENTRIES} entries")
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("stagedir")
    a = ap.parse_args()
    if len(a.label.encode("utf-8", "ignore")) > 15:
        die("label too long (max 15)")

    blocks = [None]  # block 0 reserved for superblock; blobs appended

    def alloc_blob(data):
        n = (len(data) + 511) // 512
        start = len(blocks)
        for i in range(n):
            blocks.append(data[i * 512:(i + 1) * 512].ljust(512, b"\0"))
        return start, len(data)

    def alloc_dir(entries):
        kids = []
        for nm, typ, payload in entries:
            if typ == 1:
                start, size = alloc_blob(payload)
                kids.append((nm, typ, start, size))
            else:
                sub = alloc_dir(payload)
                kids.append((nm, typ, sub, 0))
        start = len(blocks)
        raw = bytearray(512)
        for i, (nm, typ, start_b, size) in enumerate(kids):
            nb = nm.encode("utf-8", "ignore")
            raw[i * 32:i * 32 + 20] = nb + b"\0" * (20 - len(nb))
            raw[i * 32 + 20] = typ
            struct.pack_into("<II", raw, i * 32 + 24, start_b, size)
        blocks.append(bytes(raw))
        return start

    root = alloc_dir(build_dir(a.stagedir))
    total = len(blocks)
    sb = bytearray(512)
    struct.pack_into("<IIII", sb, 0, MAGIC, VERSION, total, root)
    lab = a.label.encode("utf-8", "ignore")
    sb[16:16 + len(lab)] = lab
    s = sum(sb[0:508]) & 0xFFFFFFFF
    struct.pack_into("<I", sb, 508, s)
    blocks[0] = bytes(sb)

    with open(a.out, "wb") as f:
        for b in blocks:
            f.write(b)
    print(f"nafs: {a.out} ({total} blocks, root@{root}, label '{a.label}')")


if __name__ == "__main__":
    main()
