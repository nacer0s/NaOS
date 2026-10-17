/* NaOS virtio-net driver (legacy 0.9x PCI interface). Polled queues,
 * no interrupts, no offloads: plain 10-byte headers, one TX slot,
 * 256 RX buffers. Identity-mapped PMM pages (phys == VA, below 4G). */
#include "virtio.h"
#include "pci.h"
#include "../mm/pmm.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"
#include "../../common/io.h"

/* Legacy PCI I/O BAR0 registers. */
#define V_IO_FEATURES  0x00
#define V_IO_GUEST_FE  0x04
#define V_IO_QADDR     0x08 /* PFN of descriptor table */
#define V_IO_QSIZE     0x0C /* 16-bit */
#define V_IO_QSEL      0x0E /* 16-bit */
#define V_IO_QNOTIFY   0x10 /* 16-bit */
#define V_IO_STATUS    0x12 /* 8-bit */
#define V_IO_CFG       0x14 /* device config (MAC here) */

#define V_ST_ACK  1
#define V_ST_DRV  2
#define V_ST_OK   4

#define VQ_RX 0
#define VQ_TX 1
#define VQ_MAX 256
#define VQ_PAGES 3 /* desc + avail + 4K-aligned used fit in 12KB */
#define RX_BUF 2048
#define TX_BUF 4096
#define VNET_HDR 10 /* legacy header, all zeros = plain frame */

#define DESC_F_NEXT  1
#define DESC_F_WRITE 2

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} vdesc_t;

typedef struct {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VQ_MAX];
  uint16_t used_event;
} vavail_t;

typedef struct {
  uint32_t id;
  uint32_t len;
} vused_elem_t;

typedef struct {
  uint16_t flags;
  uint16_t idx;
  vused_elem_t ring[VQ_MAX];
  uint16_t avail_event;
} vused_t;

typedef struct {
  vdesc_t *desc;   /* [N] @ page base */
  vavail_t *avail; /* @ base + 16N */
  vused_t *used;   /* @ base + 8192 */
  uint16_t n;
  uint16_t last_used; /* consumed up to here */
  uint16_t avail_next; /* next avail slot to fill (TX bookkeeping) */
} vq_t;

static uint16_t g_io; /* BAR0 I/O base */
static vq_t g_rx, g_tx;
static uint8_t *g_rxbuf[VQ_MAX];
static uint8_t *g_txbuf;
static uint8_t g_mac[6];
static uint64_t g_tx_count, g_rx_count;

static void io32w(uint16_t off, uint32_t v) {
  naos_outl((uint16_t)(g_io + off), v);
}
static uint16_t io16(uint16_t off) { return naos_inw((uint16_t)(g_io + off)); }
static void io16w(uint16_t off, uint16_t v) {
  naos_outw((uint16_t)(g_io + off), v);
}
static uint8_t io8(uint16_t off) { return naos_inb((uint16_t)(g_io + off)); }
static void io8w(uint16_t off, uint8_t v) {
  naos_outb((uint16_t)(g_io + off), v);
}

/* Set up one queue: layout in 3 zeroed contiguous pages, publish PFN. */
static int vq_setup(vq_t *q, uint16_t sel) {
  uint64_t base;
  uint16_t n;
  io16w(V_IO_QSEL, sel);
  n = io16(V_IO_QSIZE);
  if (n == 0 || n > VQ_MAX) return -1;
  base = pmm_alloc_contig(VQ_PAGES);
  if (!base) return -1;
  naos_memset((void *)(uintptr_t)base, 0, VQ_PAGES * 0x1000u);
  q->desc = (vdesc_t *)(uintptr_t)base;
  q->avail = (vavail_t *)(uintptr_t)(base + 16u * n);
  q->used = (vused_t *)(uintptr_t)(base + 0x2000u);
  q->n = n;
  q->last_used = 0;
  q->avail_next = 0;
  io32w(V_IO_QADDR, (uint32_t)(base >> 12));
  return 0;
}

int virtio_init(void) {
  uint8_t bus, slot, func;
  uint32_t bar;
  uint16_t cmd;
  uint16_t i;
  /* Transitional network device (legacy interface). */
  if (!pci_find_vendor(0x1AF4u, 0x1000u, &bus, &slot, &func)) return 0;
  bar = pci_read(bus, slot, func, 0x10);
  if (!(bar & 1u)) return 0; /* no I/O BAR: modern-only, out of scope */
  g_io = (uint16_t)(bar & 0xFFFCu);
  if (g_io == 0) return 0;
  /* Enable I/O + bus mastering, keep the rest. */
  cmd = pci_read16(bus, slot, func, 0x04);
  pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd | 0x05u));
  /* Reset, then ACK + DRIVER. No feature bits (plain frames). */
  io8w(V_IO_STATUS, 0);
  io8w(V_IO_STATUS, (uint8_t)(io8(V_IO_STATUS) | V_ST_ACK));
  io8w(V_IO_STATUS, (uint8_t)(io8(V_IO_STATUS) | V_ST_DRV));
  io32w(V_IO_GUEST_FE, 0);
  if (vq_setup(&g_rx, VQ_RX) != 0) return 0;
  if (vq_setup(&g_tx, VQ_TX) != 0) return 0;
  for (i = 0; i < 6; i++)
    g_mac[i] = naos_inb((uint16_t)(g_io + V_IO_CFG + i));
  /* RX buffers: every descriptor device-writable, all posted. */
  for (i = 0; i < g_rx.n; i++) {
    uint64_t b = pmm_alloc_page();
    if (!b) return 0;
    g_rxbuf[i] = (uint8_t *)(uintptr_t)b;
    g_rx.desc[i].addr = b;
    g_rx.desc[i].len = RX_BUF;
    g_rx.desc[i].flags = DESC_F_WRITE;
    g_rx.desc[i].next = 0;
    g_rx.avail->ring[g_rx.avail->idx % g_rx.n] = i;
    g_rx.avail->idx++;
  }
  io16w(V_IO_QNOTIFY, VQ_RX);
  g_txbuf = (uint8_t *)(uintptr_t)pmm_alloc_page();
  if (!g_txbuf) return 0;
  g_tx_count = 0;
  g_rx_count = 0;
  io8w(V_IO_STATUS, (uint8_t)(io8(V_IO_STATUS) | V_ST_OK));
  kprintf("virtio-net: %x:%x.%u io=%x mac %x:%x:%x:%x:%x:%x q=%u/%u\n",
          (unsigned)bus, (unsigned)slot, (unsigned)func, (unsigned)g_io,
          (unsigned)g_mac[0], (unsigned)g_mac[1], (unsigned)g_mac[2],
          (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5],
          (unsigned)g_rx.n, (unsigned)g_tx.n);
  return 1;
}

int virtio_link(void) { return 1; } /* no link bit on legacy: assume up */

void virtio_mac(uint8_t mac[6]) {
  int i;
  for (i = 0; i < 6; i++) mac[i] = g_mac[i];
}

uint64_t virtio_tx_count(void) { return g_tx_count; }
uint64_t virtio_rx_count(void) { return g_rx_count; }

/* Send one frame (len + 10B header fits the 4K TX page). 0 ok. */
int virtio_send(const void *data, uint32_t len) {
  uint32_t base;
  if (len == 0 || len + VNET_HDR > TX_BUF) return -1;
  naos_memset(g_txbuf, 0, VNET_HDR);
  naos_memcpy(g_txbuf + VNET_HDR, data, len);
  g_tx.desc[0].addr = (uint64_t)(uintptr_t)g_txbuf;
  g_tx.desc[0].len = len + VNET_HDR;
  g_tx.desc[0].flags = 0;
  g_tx.desc[0].next = 0;
  base = g_tx.last_used;
  g_tx.avail->ring[g_tx.avail->idx % g_tx.n] = 0;
  g_tx.avail->idx++;
  io16w(V_IO_QNOTIFY, VQ_TX);
  /* One slot: wait (bounded) for the device to take it. */
  {
    uint32_t spins = 0;
    while (g_tx.used->idx == base && spins < 1000000u) spins++;
    if (g_tx.used->idx == base) return -1;
    g_tx.last_used = g_tx.used->idx;
  }
  g_tx_count++;
  return 0;
}

/* Receive one frame into buf (cap). Returns length, 0 = none. */
uint32_t virtio_recv(void *buf, uint32_t cap) {
  vused_elem_t *e;
  uint32_t id, len;
  if (g_rx.used->idx == g_rx.last_used) return 0;
  e = &g_rx.used->ring[g_rx.last_used % g_rx.n];
  g_rx.last_used++;
  id = e->id;
  if (id >= (uint32_t)g_rx.n) return 0;
  len = e->len;
  if (len < VNET_HDR) len = VNET_HDR;
  len -= VNET_HDR;
  if (len > cap) len = cap;
  if (len > RX_BUF - VNET_HDR) len = RX_BUF - VNET_HDR;
  naos_memcpy(buf, g_rxbuf[id] + VNET_HDR, len);
  /* Re-post the buffer. */
  g_rx.avail->ring[g_rx.avail->idx % g_rx.n] = (uint16_t)id;
  g_rx.avail->idx++;
  io16w(V_IO_QNOTIFY, VQ_RX);
  g_rx_count++;
  return len;
}
