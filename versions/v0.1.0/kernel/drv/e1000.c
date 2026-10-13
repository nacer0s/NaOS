/* NaOS E1000 (82540EM) driver. Legacy descriptors, polling, no
 * interrupts, no EEPROM (MAC comes from RAL/RAH). Own code. */
#include "e1000.h"
#include "pci.h"
#include "../lib/kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../../common/cstring.h"

/* Registers (BAR0 +). */
#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_IMC    0x00D8
#define R_MDIC   0x0020
#define R_MANC   0x0580
#define R_TXDCTL 0x3828

/* MDIC PHY access: DATA[15:0] REG[20:16] PHY[25:21] OP[27:26]
 * (1=write, 2=read) READY[28] ERROR[30]. PHY 1. */
#define MDIC_OP_W 1u
#define MDIC_OP_R 2u
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define BMSR_LINK 0x04u
#define BMSR_ANCOMP 0x20u
#define BMCR_ANEN 0x1000u
#define BMCR_RESTART 0x0200u

static uint64_t g_base;
static uint32_t r32(uint64_t addr);
static void w32(uint64_t addr, uint32_t v);

static int mdic_read(uint8_t reg, uint16_t *out) {
  uint32_t t = 2000000;
  w32(g_base + R_MDIC,
      (1u << 21) | ((uint32_t)reg << 16) | (MDIC_OP_R << 26));
  while (t--) {
    uint32_t m = r32(g_base + R_MDIC);
    if (m & MDIC_READY) {
      if (m & MDIC_ERROR) return -1;
      if (out) *out = (uint16_t)m;
      return 0;
    }
  }
  return -1;
}

static int mdic_write(uint8_t reg, uint16_t v) {
  uint32_t t = 2000000;
  w32(g_base + R_MDIC,
      (1u << 21) | ((uint32_t)reg << 16) | (MDIC_OP_W << 26) | v);
  while (t--) {
    uint32_t m = r32(g_base + R_MDIC);
    if (m & MDIC_READY) return (m & MDIC_ERROR) ? -1 : 0;
  }
  return -1;
}

/* Kick PHY autonegotiation (BMCR restart) and return. The QEMU link
 * timer completes it in the background; later TX (ping) then works.
 * (Blocking here would stall boot minutes under TCG with no cable.) */
static void phy_autoneg_kick(void) {
  uint16_t bmcr = 0;
  if (mdic_read(0, &bmcr) != 0) return;
  mdic_write(0, (uint16_t)(bmcr | BMCR_ANEN | BMCR_RESTART));
}
#define R_RCTL   0x0100
#define R_TCTL   0x0400
#define R_TIPG   0x0410
#define R_PBA    0x1000
#define R_MTA    0x5200
#define R_RAL0   0x5400
#define R_RAH0   0x5404
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818

#define CTRL_RST 0x04000000u
#define CTRL_SLU 0x00000040u
#define CTRL_FD 0x00000001u
#define STATUS_LU 0x00000002u

/* RCTL: EN + BAM (broadcast) + SECRC (strip FCS). No UPE. */
#define RCTL_VAL 0x04008002u
/* TCTL (init, EN set lazily at first send like Linux): PSP + CT + COLD. */
#define TCTL_INIT 0x0103F0F8u
#define TCTL_ENB 0x00000002u
/* TIPG: 10/10/10. */
#define TIPG_VAL 0x00A0280Au

#define RX_N 128
#define TX_N 64
#define BUF_SZ 2048

typedef struct {
  uint64_t addr;
  uint16_t len;
  uint16_t cksum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} __attribute__((packed)) rxd_t;

typedef struct {
  uint64_t addr;
  uint16_t len;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} __attribute__((packed)) txd_t;

#define TX_CMD_EOP 0x01u
#define TX_CMD_RS 0x08u
#define DESC_DD 0x01u

static int g_ok;
static uint8_t g_mac[6];
static uint64_t g_tx_count, g_rx_count;

/* Rings are volatile: the device writes status behind our back. */
static volatile rxd_t *g_rx;
static volatile txd_t *g_tx;
static uint8_t *g_rxbuf[RX_N];
static uint8_t *g_txbuf[TX_N];
static uint32_t g_tx_next;
static uint32_t g_rx_next; /* oldest descriptor software hasn't eaten */

static uint32_t r32(uint64_t addr) {
  return *(volatile uint32_t *)(uintptr_t)addr;
}

static void w32(uint64_t addr, uint32_t v) {
  *(volatile uint32_t *)(uintptr_t)addr = v;
}

int e1000_link(void) {
  if (!g_ok) return 0;
  return (r32(g_base + R_STATUS) & STATUS_LU) ? 1 : 0;
}

void e1000_mac(uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
}

uint64_t e1000_tx_count(void) { return g_tx_count; }
uint64_t e1000_rx_count(void) { return g_rx_count; }

int e1000_send(const void *data, uint32_t len) {
  uint32_t i, t;
  if (!g_ok || !data || len == 0 || len > BUF_SZ) return -1;
  i = g_tx_next;
  /* Wait for the descriptor to come back (previous send done). */
  t = 2000000;
  while (!(g_tx[i].status & DESC_DD) && t--) { }
  if (!(g_tx[i].status & DESC_DD)) return -1;
  naos_memcpy(g_txbuf[i], data, len);
  g_tx[i].addr = (uint64_t)(uintptr_t)g_txbuf[i];
  g_tx[i].len = (uint16_t)len;
  /* No IFCS (mirrors JOS/MIT-lab drivers): the frame goes as-is. */
  g_tx[i].cmd = TX_CMD_EOP | TX_CMD_RS;
  g_tx[i].status = 0;
  /* Lazy TX enable (like Linux): EN goes on with the first send. */
  w32(g_base + R_TCTL, r32(g_base + R_TCTL) | TCTL_ENB);
  w32(g_base + R_TDT, (i + 1) % TX_N);
  g_tx_next = (i + 1) % TX_N;
  /* Completion: DD via RS ( polled here so send() means sent). */
  t = 4000000;
  while (!(g_tx[i].status & DESC_DD) && t--) { }
  if (!(g_tx[i].status & DESC_DD)) {
    kprintf("e1000: tx stuck tdh=%x tdt=%x tctl=%x status=%x\n",
            (unsigned)r32(g_base + R_TDH), (unsigned)r32(g_base + R_TDT),
            (unsigned)r32(g_base + R_TCTL),
            (unsigned)r32(g_base + R_STATUS));
    return -1;
  }
  g_tx_count++;
  return 0;
}

uint32_t e1000_recv(void *buf, uint32_t cap) {
  uint32_t h, len;
  if (!g_ok || !buf) return 0;
  /* Consume at our own head (RDH is hardware's next-to-fill, always one
   * past the newest arrival). */
  h = g_rx_next;
  if (!(g_rx[h].status & DESC_DD)) return 0; /* nothing yet */
  len = g_rx[h].len;
  if (len > cap) len = cap;
  naos_memcpy(buf, g_rxbuf[h], len);
  g_rx[h].status = 0;
  w32(g_base + R_RDT, h); /* hand it back */
  g_rx_next = (h + 1) % RX_N;
  g_rx_count++;
  return len;
}

int e1000_init(void) {
  uint8_t bus, slot, func;
  uint32_t bar_lo, bar_hi, cmd;
  uint16_t did;
  uint64_t bar, p;
  uint32_t ral, rah, t;
  int i;
  g_ok = 0;
  if (!pci_find_class(0x02, 0x00, &bus, &slot, &func)) return 0;
  kprintf("e1000: PCI %x:%x.%u\n", (unsigned)bus, (unsigned)slot,
          (unsigned)func);
  /* Only the verified 8254x parts. Anything else (e.g. I219 on real
   * boards) gets NO MMIO at all: poking the wrong chip turns bounded
   * polls into forever on dead registers. Net is optional; boot first. */
  did = pci_read16(bus, slot, func, 0x02);
  if (did != 0x100Eu && did != 0x10D3u) {
    kprintf("e1000: unsupported 8086:%x, net down (need 100E/10D3)\n",
            (unsigned)did);
    return 0;
  }
  cmd = pci_read16(bus, slot, func, 0x04);
  pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd | 0x06u));
  kprintf("e1000: pci cmd %x -> %x\n", (unsigned)cmd,
          (unsigned)pci_read16(bus, slot, func, 0x04));
  bar_lo = pci_read(bus, slot, func, 0x10);
  if (bar_lo & 1u) return 0;
  if ((bar_lo & 6u) == 4u) {
    bar_hi = pci_read(bus, slot, func, 0x14);
    bar = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFu);
  } else {
    bar = bar_lo & ~0xFu;
  }
  if (bar == 0) return 0;
  for (uint64_t o = 0; o < 0x20000u; o += 0x200000u) {
    if (vmm_map_mmio((bar + o) & ~0x1FFFFFULL) != 0) return 0;
  }
  g_base = bar;
  /* Mask all interrupts: polling only. */
  w32(g_base + R_IMC, 0xFFFFFFFFu);
  /* Reset, wait for self-clear. */
  w32(g_base + R_CTRL, r32(g_base + R_CTRL) | CTRL_RST);
  t = 4000000;
  while ((r32(g_base + R_CTRL) & CTRL_RST) && t--) { }
  if (r32(g_base + R_CTRL) & CTRL_RST) return 0;
  w32(g_base + R_IMC, 0xFFFFFFFFu);
  /* Disable manageability (BMC ARP/RMCP redirect would steal packets). */
  w32(g_base + R_MANC, 0);
  /* PBA: keep the reset default (RX 48KB / TX 16KB). */
  /* Link up (autoneg first, then full duplex like Linux). */
  phy_autoneg_kick();
  w32(g_base + R_CTRL, r32(g_base + R_CTRL) | CTRL_SLU | CTRL_FD);
  /* Clear the multicast table. */
  for (i = 0; i < 128; i++) w32(g_base + R_MTA + (uint64_t)i * 4u, 0);
  /* MAC from the receive address registers; make sure AV is set. */
  ral = r32(g_base + R_RAL0);
  rah = r32(g_base + R_RAH0);
  w32(g_base + R_RAH0, rah | 0x80000000u);
  for (i = 0; i < 4; i++) g_mac[i] = (uint8_t)(ral >> (i * 8));
  g_mac[4] = (uint8_t)rah;
  g_mac[5] = (uint8_t)(rah >> 8);
  /* RX ring: buffers + descriptors, all valid, tail at the end.
   * 128 descs = 2 pages (contiguous). */
  p = pmm_alloc_contig(2);
  if (!p) return 0;
  g_rx = (rxd_t *)(uintptr_t)p;
  naos_memset((void *)(uintptr_t)p, 0, 0x2000);
  for (i = 0; i < RX_N; i++) {
    uint64_t b = pmm_alloc_page();
    if (!b) return 0;
    g_rxbuf[i] = (uint8_t *)(uintptr_t)b;
    g_rx[i].addr = b;
    g_rx[i].status = 0;
  }
  /* TX ring + buffers. */
  p = pmm_alloc_page();
  if (!p) return 0;
  g_tx = (txd_t *)(uintptr_t)p;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  for (i = 0; i < TX_N; i++) {
    uint64_t b = pmm_alloc_page();
    if (!b) return 0;
    g_txbuf[i] = (uint8_t *)(uintptr_t)b;
    g_tx[i].addr = b;
    g_tx[i].status = DESC_DD; /* free */
  }
  g_tx_next = 0;
  w32(g_base + R_TDH, 0);
  w32(g_base + R_TDT, 0);
  /* Rings live below 4G (PMM cap): 64-bit bases, high halves zero. */
  w32(g_base + R_RDBAL, (uint32_t)(uintptr_t)g_rx);
  w32(g_base + R_RDBAH, 0);
  w32(g_base + R_RDLEN, RX_N * 16u);
  w32(g_base + R_RDH, 0);
  w32(g_base + R_RDT, RX_N - 1);
  w32(g_base + R_TDBAL, (uint32_t)(uintptr_t)g_tx);
  w32(g_base + R_TDBAH, 0);
  w32(g_base + R_TDLEN, TX_N * 16u);
  w32(g_base + R_TXDCTL, 0x00400000u); /* COUNT_DESC, thresholds 0 */
  w32(g_base + R_TCTL, TCTL_INIT); /* EN comes on at first send */
  w32(g_base + R_TIPG, TIPG_VAL);
  w32(g_base + R_RCTL, RCTL_VAL);
  t = 2000000;
  while (!e1000_link() && t--) { }
  kprintf("e1000: mac %x:%x:%x:%x:%x:%x link=%d\n",
          (unsigned)g_mac[0], (unsigned)g_mac[1], (unsigned)g_mac[2],
          (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5],
          e1000_link());
  g_ok = 1;
  return 1;
}
