/* NaOS FAT32 whole-disk formatter. Own code. */
#ifndef NAOS_MKFS_H
#define NAOS_MKFS_H

#include <stdint.h>

/* Format the CURRENT block device as one FAT32 volume (superfloppy).
 * 0 ok, -1 I/O error, -2 too small/huge. Call fat_init() to mount. */
int fat_format(uint64_t sectors);

#endif /* NAOS_MKFS_H */
