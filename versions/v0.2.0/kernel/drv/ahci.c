/* NaOS AHCI driver. All SATA ports visible, one active at a time
 * (shared DMA structures, reprogrammed on select). Single slot, polling.
 * Own code. */
#include "ahci.h"
#include "pci.h"
#include "../lib/kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../../common/io.h"
#include "../../common/cstring.h"

/* HBA registers (ABAR +). */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define HBA_BOHC  0x28
#define GHC_AE    0x80000000u
#define BOHC_BOS  0x01u
#define BOHC_OOS  0x02u

/* Port registers (ABAR + 0x100 + port*0x80). */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_CI    0x38
#define CMD_ST  0x01u
#define CMD_FRE  0x10u
#define CMD_FR   0x4000u
#define CMD_CR   0x8000u
#define TFD_BSY  0x80u
#define TFD_DRQ  0x08u
#define TFD_ERR  0x01u
#define SIG_SATA 0x00000101u

/* Command header (32B), slot 0. */
#define H_CFL   0x1Fu   /* FIS length DWORDs (mask) */
#define H_W     0x40u   /* write */
#define H_PRDTL 0xFFFF0000u

/* Host-to-device FIS. */
#define FIS_H2D 0x27u

/* ATA commands via AHCI. */
#define ATA_IDENTIFY 0xECu
#define ATA_READ_DMA_EXT 0x25u
#define ATA_WRITE_DMA_EXT 0x35u
#define ATA_FLUSH 0xEAu

static uint64_t g_abar;
static uint8_t g_port;
static int g_ok;
static uint64_t g_sectors;

/* DMA areas (identity-mapped PMM pages). */
static uint8_t *g_clb;   /* 1KB command list */
static uint8_t *g_fb;    /* 256B FIS area */
static uint8_t *g_ct;    /* command table + PRDT */
static uint8_t *g_iobuf; /* data staging (4KB) */

static uint32_t r32(uint64_t off) {
  return *(volatile uint32_t *)(uintptr_t)(g_abar + off);
}

static void w32(uint64_t off, uint32_t v) {
  *(volatile uint32_t *)(uintptr_t)(g_abar + off) = v;
}

static uint64_t port_off(void) {
  return 0x100ULL + (uint64_t)g_port * 0x80ULL;
}

static int dma(uint32_t lba, uint8_t cmd, int is_write); /* fwd */

static int wait_tfd_clear(uint32_t tries) {
  /* Idle = BSY clear. (DRQ belongs to PIO; DMA completion is via CI.) */
  while (tries--) {
    uint8_t t = (uint8_t)(r32(port_off() + P_TFD) & 0xFFu);
    if ((t & TFD_BSY) == 0) {
      if (t & TFD_ERR) return -2;
      return 0;
    }
  }
  return -1;
}

static int wait_ci_clear(uint32_t tries) {
  while (tries--) {
    if ((r32(port_off() + P_CI) & 1u) == 0) {
      if (r32(port_off() + P_TFD) & TFD_ERR) return -2;
      return 0;
    }
  }
  return -1;
}

/* Run one command: fis[20] ready-made H2D, data in/out of g_iobuf. */
static int run_cmd(const uint8_t *fis, int is_write, int prdt_entries) {
  /* PRDT: single entry covering 4KB. Layout is DBA(64) + Reserved(32)
   * + DescInfo(32: DBC 21:0 = bytes-1, I bit 31). DBC is DWORD 3! */
  uint64_t *prdt = (uint64_t *)((uint8_t *)g_ct + 128);
  prdt[0] = (uint64_t)(uintptr_t)g_iobuf;
  prdt[1] = 0;
  ((uint32_t *)prdt)[2] = 0;
  ((uint32_t *)prdt)[3] = 4096 - 1;
  /* Command header slot 0. CFL=5 DWORDs for our 20-byte H2D FIS. */
  uint32_t *hdr = (uint32_t *)(uintptr_t)g_clb;
  hdr[0] = (5u & H_CFL) | (is_write ? H_W : 0) |
           (((uint32_t)prdt_entries << 16) & H_PRDTL);
  hdr[1] = 0;
  hdr[2] = (uint32_t)(uintptr_t)g_ct;
  hdr[3] = 0;
  hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;
  naos_memcpy(g_ct, fis, 20);
  /* Clear pending port interrupts, then issue. */
  w32(port_off() + P_IS, 0xFFFFFFFFu);
  w32(port_off() + P_CI, 1u);
  return wait_ci_clear(2000000);
}

static int identify(void) {
  uint8_t fis[20];
  naos_memset(fis, 0, sizeof(fis));
  fis[0] = FIS_H2D;
  fis[1] = 0x80; /* C: command */
  fis[2] = ATA_IDENTIFY;
  fis[7] = 0xE0; /* master */
  if (wait_tfd_clear(1000000) != 0) return -1;
  if (run_cmd(fis, 0, 1) != 0) return -1;
  uint16_t *id = (uint16_t *)(uintptr_t)g_iobuf;
  /* 48-bit max LBA (words 100-103) when the disk does LBA48, else the
   * legacy 28-bit words 60-61 (which wrap past 137GB). Our DMA FIS is
   * already 48-bit-clean, so only the count needed fixing. */
  uint64_t secs;
  if (id[83] & 0x0400u)
    secs = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
           ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
  else
    secs = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
  if (secs > 0xFFFFFFFFULL) secs = 0xFFFFFFFFULL; /* 32-bit LBA layer */
  g_sectors = secs;
  char model[41];
  for (int i = 0; i < 20; i++) {
    model[2 * i] = (char)(id[27 + i] >> 8);
    model[2 * i + 1] = (char)(id[27 + i] & 0xFF);
  }
  model[40] = '\0';
  for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
  kprintf("ahci: %s sectors=%u\n", model, (unsigned)g_sectors);
  return 0;
}

/* ---- multi-port: one active at a time, shared DMA structures ---- */

static int hba_done;

/* PCI find + BAR + MMIO map + BIOS/OS handoff + AE. Once. 1 ok. */
static int hba_up(void) {
  uint8_t bus, slot, func;
  if (hba_done) return 1;
  if (!pci_find_class(1, 6, &bus, &slot, &func)) return 0;
  kprintf("ahci: PCI %x:%x.%u\n", (unsigned)bus, (unsigned)slot,
          (unsigned)func);
  /* Enable MMIO + bus mastering. */
  uint16_t cmd = pci_read16(bus, slot, func, 0x04);
  pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd | 0x06u));
  /* BAR5 (ABAR): assume 32-bit MMIO. */
  uint32_t bar = pci_read(bus, slot, func, 0x24);
  if (bar & 1u) return 0; /* I/O BAR: not AHCI memory */
  g_abar = bar & ~0xFu;
  /* ABAR may sit above our 1G identity map (or above 4G): map it. */
  if (g_abar == 0) return 0;
  if (vmm_map_mmio(g_abar & ~0x1FFFFFULL) != 0) return 0;
  /* BIOS/OS handoff. */
  uint32_t bohc = r32(HBA_BOHC);
  if (bohc & BOHC_BOS) {
    w32(HBA_BOHC, bohc | BOHC_OOS);
    uint32_t t = 1000000;
    while ((r32(HBA_BOHC) & BOHC_BOS) && t--) { }
    if (r32(HBA_BOHC) & BOHC_BOS) return 0;
  }
  w32(HBA_GHC, r32(HBA_GHC) | GHC_AE);
  /* NOTE: no HBA reset (GHC.HR) here: QEMU's ICH9 model never clears HR,
   * and firmware hands us working ports. Port stop + COMRESET below is
   * enough for takeover. */
  hba_done = 1;
  return 1;
}

/* Bitmask of implemented SATA ports (ATAPI ignored). Read-only probe. */
uint32_t ahci_ports(void) {
  uint32_t m = 0;
  uint8_t save;
  if (!hba_up()) return 0;
  save = g_port;
  {
    uint32_t pi = r32(HBA_PI);
    for (int p = 0; p < 32; p++) {
      if (!(pi & (1u << (unsigned)p))) continue;
      g_port = (uint8_t)p;
      if ((r32(port_off() + P_SSTS) & 0xFu) != 3) continue;
      if (r32(port_off() + P_SIG) != SIG_SATA) continue;
      m |= (uint32_t)(1u << (unsigned)p);
    }
  }
  g_port = save;
  return m;
}

/* Stop a port (clear ST|FRE, wait FR/CR idle). Best effort. */
static void stop_port(void) {
  uint32_t t = 1000000;
  w32(port_off() + P_CMD,
      r32(port_off() + P_CMD) & ~(CMD_ST | CMD_FRE));
  while ((r32(port_off() + P_CMD) & (CMD_FR | CMD_CR)) && t--) { }
}

/* Bring port p up: stop, program shared DMA structures, start,
 * identify. Structures are allocated once and reprogrammed per port. */
static int start_port(uint8_t p) {
  g_port = p;
  stop_port();
  if (!g_clb) {
    uint64_t clb = pmm_alloc_page();
    uint64_t fb = pmm_alloc_page();
    uint64_t ct = pmm_alloc_page();
    uint64_t io = pmm_alloc_page();
    if (!clb || !fb || !ct || !io) return 0;
    g_clb = (uint8_t *)(uintptr_t)clb;
    g_fb = (uint8_t *)(uintptr_t)fb;
    g_ct = (uint8_t *)(uintptr_t)ct;
    g_iobuf = (uint8_t *)(uintptr_t)io;
    naos_memset(g_clb, 0, 0x1000);
    naos_memset(g_fb, 0, 0x1000);
    naos_memset(g_ct, 0, 0x1000);
  }
  w32(port_off() + P_CLB, (uint32_t)(uintptr_t)g_clb);
  w32(port_off() + P_CLBU, 0);
  w32(port_off() + P_FB, (uint32_t)(uintptr_t)g_fb);
  w32(port_off() + P_FBU, 0);
  w32(port_off() + P_SERR, 0xFFFFFFFFu); /* clear errors */
#if 0 /* COMRESET vs QEMU: firmware leaves a working link; resetting it
       * makes QEMU's backend return zeros. Revisit on real hardware. */
  /* COMRESET: take the link back from firmware state. */
  w32(port_off() + P_SCTL,
      (r32(port_off() + P_SCTL) & ~0xFu) | 1u);
  {
    volatile uint32_t d = 200000;
    while (d--) { }
  }
  w32(port_off() + P_SCTL, r32(port_off() + P_SCTL) & ~0xFu);
  {
    uint32_t t = 2000000;
    while ((r32(port_off() + P_SSTS) & 0xFu) != 3 && t--) { }
    if ((r32(port_off() + P_SSTS) & 0xFu) != 3) return 0;
  }
  w32(port_off() + P_SERR, 0xFFFFFFFFu); /* reset raises DIAG bits */
#endif
  w32(port_off() + P_CMD, r32(port_off() + P_CMD) | CMD_FRE);
  {
    uint32_t t = 1000000;
    while (!(r32(port_off() + P_CMD) & CMD_FR) && t--) { }
    if (!(r32(port_off() + P_CMD) & CMD_FR)) return 0;
  }
  w32(port_off() + P_CMD, r32(port_off() + P_CMD) | CMD_ST);
  if (identify() != 0) return 0;
  g_ok = 1;
  return 1;
}

int ahci_init(void) {
  uint32_t m;
  g_ok = 0;
  m = ahci_ports();
  if (m == 0) return 0;
  for (int p = 0; p < 32; p++) {
    if (!(m & (1u << (unsigned)p))) continue;
    if (start_port((uint8_t)p)) return 1; /* first SATA disk wins */
  }
  return 0;
}

uint8_t ahci_port(void) { return g_port; }

/* Route I/O to another present SATA port (parks the old one). 0 ok. */
int ahci_use_port(uint8_t p) {
  uint32_t m;
  if (p >= 32) return -1;
  m = ahci_ports();
  if (!(m & (1u << (unsigned)p))) return -1;
  if (g_ok && p == g_port) return 0;
  if (g_ok) stop_port(); /* park the old port (g_port still old) */
  if (!start_port(p)) {
    g_ok = 0;
    return -1;
  }
  return 0;
}

uint64_t ahci_sectors(void) { return g_sectors; }

static int dma(uint32_t lba, uint8_t cmd, int is_write) {
  uint8_t fis[20];
  naos_memset(fis, 0, sizeof(fis));
  fis[0] = FIS_H2D;
  fis[1] = 0x80;
  fis[2] = cmd;
  fis[3] = 0;             /* features 7:0 */
  fis[4] = (uint8_t)(lba & 0xFF);
  fis[5] = (uint8_t)((lba >> 8) & 0xFF);
  fis[6] = (uint8_t)((lba >> 16) & 0xFF);
  fis[7] = 0xE0;          /* master, LBA */
  fis[8] = (uint8_t)((lba >> 24) & 0xFF);
  fis[9] = 0;
  fis[10] = 0;
  fis[12] = 1;            /* count 7:0 = 1 sector */
  if (wait_tfd_clear(1000000) != 0) return -1;
  return run_cmd(fis, is_write, 1);
}

int ahci_read(uint32_t lba, uint8_t count, void *buf) {
  uint8_t *out = (uint8_t *)buf;
  if (!g_ok || count == 0 || !buf) return -1;
  for (uint32_t s = 0; s < count; s++) {
    if (dma(lba + s, ATA_READ_DMA_EXT, 0) != 0) return -1;
    naos_memcpy(out + (uint64_t)s * 512u, g_iobuf, 512);
  }
  return 0;
}

int ahci_write(uint32_t lba, uint8_t count, const void *buf) {
  const uint8_t *in = (const uint8_t *)buf;
  uint8_t flush[20];
  if (!g_ok || count == 0 || !buf) return -1;
  for (uint32_t s = 0; s < count; s++) {
    naos_memcpy(g_iobuf, in + (uint64_t)s * 512u, 512);
    if (dma(lba + s, ATA_WRITE_DMA_EXT, 1) != 0) return -1;
  }
  naos_memset(flush, 0, sizeof(flush));
  flush[0] = FIS_H2D;
  flush[1] = 0x80;
  flush[2] = ATA_FLUSH;
  flush[7] = 0xE0;
  if (wait_tfd_clear(1000000) != 0) return -1;
  return run_cmd(flush, 0, 0);
}
