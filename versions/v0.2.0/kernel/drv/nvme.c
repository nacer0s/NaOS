/* NaOS NVMe driver. Polled completion (phase tag), one command in
 * flight per queue, 4K-aligned PMM pages for queues/data (identity
 * mapped, so phys == VA). 512B and 4K namespaces via blk512(). */
#include "nvme.h"
#include "pci.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kmalloc.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

/* Controller registers (BAR0 +). */
#define NVME_CAP 0x00u
#define NVME_CC 0x14u
#define NVME_CSTS 0x1Cu
#define NVME_AQA 0x24u
#define NVME_ASQ 0x28u
#define NVME_ACQ 0x30u
#define NVME_DB 0x1000u

#define CC_EN 0x1u
#define CSTS_RDY 0x1u
#define CSTS_CFS 0x2u

/* Admin opcodes. */
#define OP_DEL_IOSQ 0x00u
#define OP_CREATE_IOSQ 0x01u
#define OP_DEL_IOCQ 0x04u
#define OP_CREATE_IOCQ 0x05u
#define OP_IDENTIFY 0x06u
/* NVM opcodes. */
#define OP_WRITE 0x01u
#define OP_READ 0x02u

#define ADM_DEPTH 64
#define IO_DEPTH 32

static int g_ok;
static uint64_t g_base; /* mapped BAR0 */
static uint64_t g_db;   /* doorbell base */
static uint32_t g_stride;
static uint64_t g_nsze; /* namespace capacity, in 512B sectors */
static uint64_t g_ns_blocks; /* namespace capacity, in lbads-sized blocks */
static uint8_t g_lbads; /* namespace block size as 2^n (9=512B, 12=4K) */

static uint8_t *g_asq;
static uint8_t *g_acq;
static uint8_t *g_isq;
static uint8_t *g_icq;
static uint8_t *g_id;   /* 4K identify buffer */
static uint8_t *g_io;   /* 4K data bounce buffer */

static uint16_t g_at, g_ah, g_it, g_ih;
static uint8_t g_aph, g_iph;

static uint32_t r32(uint64_t o) {
  return *(volatile uint32_t *)(uintptr_t)(g_base + o);
}

static void w32(uint64_t o, uint32_t v) {
  *(volatile uint32_t *)(uintptr_t)(g_base + o) = v;
}

static uint64_t r64(uint64_t o) {
  return *(volatile uint64_t *)(uintptr_t)(g_base + o);
}

static void w64(uint64_t o, uint64_t v) {
  *(volatile uint64_t *)(uintptr_t)(g_base + o) = v;
}

static void wdb(uint32_t idx, uint32_t v) {
  *(volatile uint32_t *)(uintptr_t)(g_db + (uint64_t)idx * g_stride) = v;
}

/* Submit on admin queue, poll CQ0. 0 ok. (Count-loop waits: runs
 * pre-sti too, where the PIT tick counter is still frozen.) */
static int adm(uint8_t op, uint32_t nsid, uint32_t c10, uint32_t c11,
               uint32_t c12, uint64_t prp) {
  uint8_t *sq = g_asq + (uint64_t)g_at * 64;
  uint32_t t = 10000000;
  uint16_t cid = (uint16_t)(g_at + 1);
  naos_memset(sq, 0, 64);
  sq[0] = op;
  sq[2] = (uint8_t)cid;
  sq[3] = (uint8_t)(cid >> 8);
  sq[4] = (uint8_t)nsid;
  sq[5] = (uint8_t)(nsid >> 8);
  sq[6] = (uint8_t)(nsid >> 16);
  sq[7] = (uint8_t)(nsid >> 24);
  for (int i = 0; i < 8; i++) sq[24 + i] = (uint8_t)(prp >> (i * 8));
  sq[40] = (uint8_t)c10;
  sq[41] = (uint8_t)(c10 >> 8);
  sq[42] = (uint8_t)(c10 >> 16);
  sq[43] = (uint8_t)(c10 >> 24);
  sq[44] = (uint8_t)c11;
  sq[45] = (uint8_t)(c11 >> 8);
  sq[46] = (uint8_t)(c11 >> 16);
  sq[47] = (uint8_t)(c11 >> 24);
  sq[48] = (uint8_t)c12;
  sq[49] = (uint8_t)(c12 >> 8);
  sq[50] = (uint8_t)(c12 >> 16);
  sq[51] = (uint8_t)(c12 >> 24);
  g_at = (uint16_t)((g_at + 1) % ADM_DEPTH);
  wdb(0, g_at);
  while (t--) {
    /* Volatile: the device writes this RAM behind our back; a plain
     * load would be hoisted out of the loop by -O2. */
    uint8_t *cq = g_acq + (uint64_t)g_ah * 16;
    uint32_t dw3 = *(volatile uint32_t *)(uintptr_t)(cq + 12);
    if (((dw3 >> 16) & 1u) == g_aph) {
      uint16_t ccid = (uint16_t)dw3;
      int ok = ((dw3 >> 17) == 0) && (ccid == cid);
      g_ah = (uint16_t)((g_ah + 1) % ADM_DEPTH);
      if (g_ah == 0) g_aph ^= 1u;
      wdb(1, g_ah);
      return ok ? 0 : -1;
    }
  }
  return -1;
}

/* Submit on I/O queue 1, poll CQ1. 0 ok. */
static int io(uint8_t op, uint32_t lba, uint64_t prp) {
  uint8_t *sq = g_isq + (uint64_t)g_it * 64;
  uint32_t t = 10000000;
  uint16_t cid = (uint16_t)(g_it + 1);
  naos_memset(sq, 0, 64);
  sq[0] = op;
  sq[2] = (uint8_t)cid;
  sq[3] = (uint8_t)(cid >> 8);
  sq[4] = 1; /* NSID 1 */
  for (int i = 0; i < 8; i++) sq[24 + i] = (uint8_t)(prp >> (i * 8));
  sq[40] = (uint8_t)lba;
  sq[41] = (uint8_t)(lba >> 8);
  sq[42] = (uint8_t)(lba >> 16);
  sq[43] = (uint8_t)(lba >> 24);
  /* cdw11 = 0 (lba < 4G), cdw12 = 0 (one block). */
  g_it = (uint16_t)((g_it + 1) % IO_DEPTH);
  wdb(2, g_it);
  while (t--) {
    /* Volatile (see adm): device-written RAM, must reload each pass. */
    uint8_t *cq = g_icq + (uint64_t)g_ih * 16;
    uint32_t dw3 = *(volatile uint32_t *)(uintptr_t)(cq + 12);
    if (((dw3 >> 16) & 1u) == g_iph) {
      uint16_t ccid = (uint16_t)dw3;
      int ok = ((dw3 >> 17) == 0) && (ccid == cid);
      g_ih = (uint16_t)((g_ih + 1) % IO_DEPTH);
      if (g_ih == 0) g_iph ^= 1u;
      wdb(3, g_ih);
      return ok ? 0 : -1;
    }
  }
  return -1;
}

int nvme_init(void) {
  uint8_t bus, slot, func;
  uint64_t bar, cap;
  uint32_t dstrd;
  if (g_ok) return 1;
  if (!pci_find_class_pi(1, 8, 2, &bus, &slot, &func)) return 0;
  kprintf("nvme: PCI %x:%x.%u\n", (unsigned)bus, (unsigned)slot,
          (unsigned)func);
  {
    uint16_t cmd = pci_read16(bus, slot, func, 0x04);
    pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd | 0x06u));
  }
  {
    uint32_t lo = pci_read(bus, slot, func, 0x10);
    uint32_t hi = pci_read(bus, slot, func, 0x14);
    if (lo & 1u) return 0;
    if ((lo & 6u) == 4u) bar = ((uint64_t)hi << 32) | (lo & ~0xFu);
    else bar = lo & ~0xFu;
  }
  if (bar == 0) return 0;
  if (vmm_map_mmio(bar & ~0x1FFFFFULL) != 0) return 0;
  g_base = bar;
  cap = r64(NVME_CAP);
  if ((cap & 0xFFFFu) < (ADM_DEPTH - 1)) return 0; /* MQES too small */
  dstrd = (uint32_t)((cap >> 32) & 0xFu);
  g_stride = 4u << dstrd;
  g_db = g_base + NVME_DB;
  /* Disable (no-op if already down), wait RDY=0. */
  w32(NVME_CC, r32(NVME_CC) & ~CC_EN);
  {
    uint32_t t = 50000000;
    while ((r32(NVME_CSTS) & CSTS_RDY) && t--) { }
    if (r32(NVME_CSTS) & CSTS_RDY) return 0;
  }
  /* Queues: PMM pages (identity mapped). */
  {
    uint64_t a = pmm_alloc_page(), c = pmm_alloc_page();
    uint64_t s = pmm_alloc_page(), q = pmm_alloc_page();
    uint64_t d = pmm_alloc_page(), b = pmm_alloc_page();
    if (!a || !c || !s || !q || !d || !b) return 0;
    g_asq = (uint8_t *)(uintptr_t)a;
    g_acq = (uint8_t *)(uintptr_t)c;
    g_isq = (uint8_t *)(uintptr_t)s;
    g_icq = (uint8_t *)(uintptr_t)q;
    g_id = (uint8_t *)(uintptr_t)d;
    g_io = (uint8_t *)(uintptr_t)b;
  }
  naos_memset(g_asq, 0, 0x1000);
  naos_memset(g_acq, 0, 0x1000);
  naos_memset(g_isq, 0, 0x1000);
  naos_memset(g_icq, 0, 0x1000);
  g_at = g_ah = g_it = g_ih = 0;
  g_aph = g_iph = 1;
  w32(NVME_AQA, (ADM_DEPTH - 1) | ((ADM_DEPTH - 1) << 16));
  w64(NVME_ASQ, (uint64_t)(uintptr_t)g_asq);
  w64(NVME_ACQ, (uint64_t)(uintptr_t)g_acq);
  /* Enable: 4K MPS, 64B SQE, 16B CQE. */
  w32(NVME_CC, CC_EN | (6u << 16) | (4u << 20));
  {
    uint32_t t = 50000000;
    while (!(r32(NVME_CSTS) & CSTS_RDY) && t--) { }
    if (!(r32(NVME_CSTS) & CSTS_RDY)) return 0;
    if (r32(NVME_CSTS) & CSTS_CFS) return 0;
  }
  /* Identify controller (sanity), then create I/O queues. */
  if (adm(OP_IDENTIFY, 0, 1, 0, 0, (uint64_t)(uintptr_t)g_id) != 0)
    return 0;
  if (adm(OP_CREATE_IOCQ, 0, 1 | ((IO_DEPTH - 1) << 16), 1,
          0, (uint64_t)(uintptr_t)g_icq) != 0)
    return 0;
  if (adm(OP_CREATE_IOSQ, 0, 1 | ((IO_DEPTH - 1) << 16), 1 | (1u << 16),
          0, (uint64_t)(uintptr_t)g_isq) != 0)
    return 0;
  /* Identify namespace 1: size + block size. */
  if (adm(OP_IDENTIFY, 1, 0, 0, 0, (uint64_t)(uintptr_t)g_id) != 0) return 0;
  {
    uint64_t nsze = 0;
    uint8_t flbas, lbads;
    for (int i = 0; i < 8; i++) nsze |= (uint64_t)g_id[i] << (i * 8);
    flbas = g_id[26] & 0xFu;
    /* LBAF entry bytes: MS(0:15), LBADS(16:23) -> byte 2. */
    lbads = g_id[128 + flbas * 4 + 2];
    if (nsze == 0) return 0;
    if (lbads != 9 && lbads != 12) {
      kprintf("nvme: LBA size 2^%u unsupported (need 512B or 4K)\n",
              (unsigned)lbads);
      return 0;
    }
    g_lbads = lbads;
    g_ns_blocks = nsze;
    /* Block layer speaks 512B sectors: count capacity in those units
     * (capped at 2^32-1, the layer's LBA width). */
    {
      uint64_t n512 = nsze << (lbads - 9);
      if (n512 > 0xFFFFFFFFULL) n512 = 0xFFFFFFFFULL;
      g_nsze = n512;
    }
    kprintf("nvme: %u-byte blocks\n", (unsigned)(1u << lbads));
  }
  g_ok = 1;
  return 1;
}

uint64_t nvme_sectors(void) { return g_nsze; }

/* 512B-sector I/O over lbads-sized namespace blocks. Consecutive
 * sectors sharing a block go in one command; partial-block writes
 * pre-read the page (g_io is a full 4K page). */
static int blk512(int is_write, uint32_t lba, uint8_t count, uint8_t *buf) {
  uint32_t shift = (uint32_t)g_lbads - 9u; /* 0 for 512B, 3 for 4K */
  uint32_t per = 1u << shift;              /* 512B sectors per block */
  uint32_t s = 0;
  if ((uint64_t)lba + count > g_nsze) return -1;
  while (s < count) {
    uint32_t page = (lba + s) >> shift;
    uint32_t off = (lba + s) & (per - 1u);
    uint32_t n = per - off;
    int full;
    if (n > (uint32_t)(count - s)) n = (uint32_t)(count - s);
    full = (off == 0 && n == per);
    if (!is_write || !full) {
      /* Reads always fetch the page; partial writes pre-read it. */
      if (io(OP_READ, page, (uint64_t)(uintptr_t)g_io) != 0) return -1;
    }
    if (is_write) {
      naos_memcpy(g_io + off * 512u, buf + (uint64_t)s * 512u, n * 512u);
      if (io(OP_WRITE, page, (uint64_t)(uintptr_t)g_io) != 0) return -1;
    } else {
      naos_memcpy(buf + (uint64_t)s * 512u, g_io + off * 512u, n * 512u);
    }
    s += n;
  }
  return 0;
}

int nvme_read(uint32_t lba, uint8_t count, void *buf) {
  if (!g_ok || count == 0 || !buf) return -1;
  return blk512(0, lba, count, (uint8_t *)buf);
}

int nvme_write(uint32_t lba, uint8_t count, const void *buf) {
  if (!g_ok || count == 0 || !buf) return -1;
  return blk512(1, lba, count, (uint8_t *)buf);
}

void nvme_info(void) {
  char model[41], serial[21];
  if (!g_ok && !nvme_init()) {
    kprintf("nvme: not found\n");
    return;
  }
  /* g_id holds namespace identify from init; re-read controller page. */
  if (adm(OP_IDENTIFY, 0, 1, 0, 0, (uint64_t)(uintptr_t)g_id) != 0) {
    kprintf("nvme: identify failed\n");
    return;
  }
  for (int i = 0; i < 40; i++) model[i] = (char)g_id[24 + i];
  model[40] = '\0';
  for (int i = 0; i < 20; i++) serial[i] = (char)g_id[4 + i];
  serial[20] = '\0';
  kprintf("nvme: model '%s' serial '%s'\n", model, serial);
  kprintf("nvme: namespace 1: %u blocks x %uB (%u MB)\n",
          (unsigned)g_ns_blocks, (unsigned)(1u << g_lbads),
          (unsigned)(g_nsze / 2048u));
}

/* Write pattern to last LBA, verify, restore original. 0 ok. */
int nvme_selftest(void) {
  uint8_t *orig, *back;
  uint32_t last;
  if (!g_ok && !nvme_init()) return -1;
  last = (uint32_t)(g_nsze - 1);
  orig = (uint8_t *)kmalloc(512);
  back = (uint8_t *)kmalloc(512);
  if (!orig || !back) {
    if (orig) kfree(orig);
    if (back) kfree(back);
    return -1;
  }
  if (nvme_read(last, 1, orig) != 0) goto fail;
  for (int i = 0; i < 512; i++) back[i] = (uint8_t)(i * 7 + 3);
  if (nvme_write(last, 1, back) != 0) goto fail;
  naos_memset(back, 0, 512);
  if (nvme_read(last, 1, back) != 0) goto fail;
  for (int i = 0; i < 512; i++) {
    if (back[i] != (uint8_t)(i * 7 + 3)) {
      kprintf("nvme: selftest mismatch @%d\n", i);
      goto fail;
    }
  }
  if (nvme_write(last, 1, orig) != 0) goto fail;
  kfree(orig);
  kfree(back);
  /* 4K namespaces: also cover the cross-block path (one 8-sector
   * write spanning two 4K pages, with partial-page pre-reads). */
  if (g_lbads == 12 && g_nsze >= 16) {
    uint8_t *o2 = (uint8_t *)kmalloc(4096);
    uint8_t *b2 = (uint8_t *)kmalloc(4096);
    int i;
    if (!o2 || !b2) {
      if (o2) kfree(o2);
      if (b2) kfree(b2);
      return -1;
    }
    if (nvme_read(4, 8, o2) != 0) goto fail2;
    for (i = 0; i < 4096; i++) b2[i] = (uint8_t)(i * 5 + 1);
    if (nvme_write(4, 8, b2) != 0) goto fail2;
    naos_memset(b2, 0, 4096);
    if (nvme_read(4, 8, b2) != 0) goto fail2;
    for (i = 0; i < 4096; i++) {
      if (b2[i] != (uint8_t)(i * 5 + 1)) {
        kprintf("nvme: selftest 4K mismatch @%d\n", i);
        goto fail2;
      }
    }
    if (nvme_write(4, 8, o2) != 0) goto fail2;
    kfree(o2);
    kfree(b2);
    return 0;
  fail2:
    nvme_write(4, 8, o2); /* best-effort restore */
    kfree(o2);
    kfree(b2);
    return -1;
  }
  return 0;
fail:
  nvme_write(last, 1, orig); /* best-effort restore */
  kfree(orig);
  kfree(back);
  return -1;
}
