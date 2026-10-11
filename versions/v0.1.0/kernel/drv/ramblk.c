/* NaOS RAM disk: memcpy backend over bootloader page runs. */
#include "ramblk.h"
#include "../../common/cstring.h"

#define RD_RUNS 4

static int g_ok;
static uint64_t g_base[RD_RUNS];
static uint64_t g_len[RD_RUNS];
static uint64_t g_total;

void ramblk_init(const uint64_t *bases, const uint32_t *pages) {
  g_ok = 0;
  g_total = 0;
  if (!bases || !pages) return;
  for (int i = 0; i < RD_RUNS; i++) {
    g_base[i] = 0;
    g_len[i] = 0;
    if (bases[i] == 0 || pages[i] == 0) return; /* prefix-packed runs */
    if (bases[i] >= 0x40000000u) return;       /* must sit in identity map */
    g_base[i] = bases[i];
    g_len[i] = (uint64_t)pages[i] * 4096u;
    g_total += g_len[i];
  }
  if (g_total >= 512u) g_ok = 1;
}

int ramblk_ok(void) { return g_ok; }

uint64_t ramblk_sectors(void) { return g_total / 512u; }

static int rw(uint32_t lba, uint8_t count, void *buf, int is_write) {
  uint64_t off = (uint64_t)lba * 512u;
  uint64_t len = (uint64_t)count * 512u;
  uint8_t *p = (uint8_t *)buf;
  if (!g_ok || count == 0 || !buf) return -1;
  if (off + len > g_total) return -1;
  while (len > 0) {
    uint64_t base = 0;
    int found = 0;
    for (int i = 0; i < RD_RUNS && g_len[i]; i++) {
      if (off >= base && off < base + g_len[i]) {
        uint64_t room = base + g_len[i] - off;
        uint64_t n = len < room ? len : room;
        uint64_t phys = g_base[i] + (off - base);
        if (is_write) naos_memcpy((void *)(uintptr_t)phys, p, n);
        else naos_memcpy(p, (void *)(uintptr_t)phys, n);
        p += n;
        off += n;
        len -= n;
        found = 1;
        break;
      }
      base += g_len[i];
    }
    if (!found) return -1;
  }
  return 0;
}

int ramblk_read(uint32_t lba, uint8_t count, void *buf) {
  return rw(lba, count, buf, 0);
}

int ramblk_write(uint32_t lba, uint8_t count, const void *buf) {
  return rw(lba, count, (void *)buf, 1);
}
