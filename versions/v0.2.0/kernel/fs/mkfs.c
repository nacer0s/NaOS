/* NaOS FAT32 whole-disk formatter (superfloppy, like our ESP images:
 * no partition table — UEFI fallback boot finds \EFI\BOOT\BOOTX64.EFI).
 * Layout mirrors tools/mkesp.sh geometry (32 reserved, 2 FATs, root=2).
 * Own code, no libc. */
#include "mkfs.h"
#include "../drv/blk.h"
#include "../../common/cstring.h"

static uint8_t zsec[512]; /* zero page (bss, already zero) */

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Write `n` zero sectors starting at lba. */
static int zero_range(uint32_t lba, uint32_t n) {
  uint8_t chunk;
  while (n > 0) {
    chunk = (n > 128) ? 128 : (uint8_t)n;
    if (blk_write(lba, chunk, zsec) != 0) return -1;
    lba += chunk;
    n -= chunk;
  }
  return 0;
}

/* Format the CURRENT block device as one FAT32 volume.
 * 0 ok, -1 I/O error, -2 too small/huge. Leaves the volume unmounted
 * (call fat_init() to mount). */
int fat_format(uint64_t sectors) {
  static uint8_t sec[512];
  uint32_t spc = 1, fatsz, rsvd = 32, data, clusters;
  uint64_t est;
  uint32_t volid;
  if (sectors < 70000ULL || sectors > 0xFFFFFFFFULL) return -2;
  naos_memset(zsec, 0, sizeof(zsec));
  /* Sectors/cluster: keep cluster count in [65525, 4M]. */
  for (;;) {
    est = sectors / spc;
    fatsz = (uint32_t)((est * 4 + 511) / 512 + 1);
    data = (uint32_t)(sectors - rsvd - 2 * (uint64_t)fatsz);
    clusters = data / spc;
    if (clusters <= 4194304u || spc >= 64u) break;
    spc *= 2;
  }
  /* Smallest FAT covering its own cluster count (grows into place). */
  fatsz = 1;
  for (;;) {
    uint32_t need;
    data = (uint32_t)(sectors - rsvd - 2u * (uint64_t)fatsz);
    clusters = data / spc;
    need = (uint32_t)(((uint64_t)clusters * 4u + 511u) / 512u + 1u);
    if (need <= fatsz) break;
    fatsz = need;
  }
  data = (uint32_t)(sectors - rsvd - 2u * (uint64_t)fatsz);
  clusters = data / spc;
  if (clusters < 65525u) return -2; /* not a FAT32-sized disk */
  /* Volume boot record. */
  naos_memset(sec, 0, sizeof(sec));
  sec[0] = 0xEB;
  sec[1] = 0x58;
  sec[2] = 0x90;
  naos_memcpy(sec + 3, "NaOS    ", 8);
  put16(sec + 11, 512);
  sec[13] = (uint8_t)spc;
  put16(sec + 14, (uint16_t)rsvd);
  sec[16] = 2;
  sec[21] = 0xF8;
  put16(sec + 24, 63); /* sectors/track (mirrors mformat) */
  put16(sec + 26, 16); /* heads (mirrors mformat) */
  put32(sec + 28, 0); /* hidden sectors: whole-disk volume */
  put32(sec + 32, (uint32_t)sectors);
  put32(sec + 36, fatsz);
  put32(sec + 44, 2); /* root dir = cluster 2 */
  put16(sec + 48, 1); /* FSInfo at sector 1 */
  put16(sec + 50, 6); /* backup VBR at sector 6 */
  sec[64] = 0x80;
  sec[66] = 0x29;
  volid = (uint32_t)(sectors ^ (sectors >> 32) ^ 0x4E614F53u);
  put32(sec + 67, volid);
  naos_memcpy(sec + 71, "NO NAME    ", 11);
  naos_memcpy(sec + 82, "FAT32   ", 8);
  sec[510] = 0x55;
  sec[511] = 0xAA;
  if (blk_write(0, 1, sec) != 0) return -1;
  if (blk_write(6, 1, sec) != 0) return -1; /* backup VBR */
  /* FSInfo + backup. */
  naos_memset(sec, 0, sizeof(sec));
  put32(sec + 0, 0x41615252u);
  put32(sec + 484, 0x61417272u);
  put32(sec + 488, clusters - 1u); /* free (root allocated) */
  put32(sec + 492, 3u);            /* next free */
  sec[510] = 0x55;
  sec[511] = 0xAA;
  if (blk_write(1, 1, sec) != 0) return -1;
  if (blk_write(7, 1, sec) != 0) return -1;
  /* FATs: [0]=media, [1]=EOC, [2]=root EOC, rest free. */
  naos_memset(sec, 0, sizeof(sec));
  put32(sec + 0, 0x0FFFFFF8u);
  put32(sec + 4, 0x0FFFFFFFu);
  put32(sec + 8, 0x0FFFFFFFu);
  if (blk_write(rsvd, 1, sec) != 0) return -1;
  if (blk_write(rsvd + fatsz, 1, sec) != 0) return -1;
  if (zero_range(rsvd + 1, fatsz - 1u) != 0) return -1;
  if (zero_range(rsvd + fatsz + 1u, fatsz - 1u) != 0) return -1;
  /* Root directory cluster (first data cluster). */
  if (zero_range(rsvd + 2u * fatsz, spc) != 0) return -1;
  return 0;
}
