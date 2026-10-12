/* NaOS-FS v1 ("NaOS File System"): tiny read-only guest filesystem.
 * Canonical on-disk spec (mirrored in tools/mknafs.py — keep in sync):
 *   blocks are 512B, little-endian. Block 0 = superblock:
 *     u32 magic 0x5346414E, u32 version 1, u32 total_blocks,
 *     u32 root_block, char label[16], zeros to 508,
 *     u32 checksum = sum(bytes[0,508)) mod 2^32.
 *   dir block = 16 x 32B entries: name[20] NUL, u8 type
 *     (0 free, 1 file, 2 dir), u8 res[3], u32 start_block, u32 size.
 *   file data = contiguous blocks from start_block (last may be partial).
 * The image lives as a regular file on FAT (e.g. /DATA.NAFS); blocks are
 * read positionally through the FAT layer. v0.1.0 is read-only. Own code. */
#ifndef NAOS_NAFS_H
#define NAOS_NAFS_H

#include <stdint.h>

/* Mount a FAT-hosted image (one volume at a time). 0 ok. */
int nafs_mount(const char *fatpath);
/* List a NaFS dir ("" or "/" = root). 0 ok. */
int nafs_list(const char *dirpath);
/* Print a text file (16KB cap). 0 ok. */
int nafs_read(const char *filepath);

#endif /* NAOS_NAFS_H */
