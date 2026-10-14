/* NaOS XHCI driver: controller bring-up, rings, slot commands, control
 * and interrupt transfers. Polled (no interrupts). USB 1.x/2.0/3.x HID
 * keyboards via usb_kbd.c. Own code, written from the xHCI spec. */
#include "xhci.h"
#include "pci.h"
#include "../lib/kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../../common/cstring.h"

/* ---- capability registers (BAR0 +) ---- */
#define CAP_CAPLEN  0x00
#define CAP_HCIV    0x02
#define CAP_HCSP1   0x04
#define CAP_HCSP2   0x08
#define CAP_HCCP1   0x10
#define CAP_DBOFF   0x14
#define CAP_RTSOFF  0x18

/* ---- operational registers (BAR0 + caplen +) ---- */
#define OP_USBCMD  0x00
#define OP_USBSTS  0x04
#define OP_PAGESZ  0x08
#define OP_DNCTRL  0x14
#define OP_CRCR    0x18
#define OP_DCBAAP  0x30
#define OP_CONFIG  0x38
#define OP_PORTSC  0x400

#define CMD_RS    0x01u
#define CMD_HCRST 0x02u
#define STS_HCH   0x01u
#define STS_CNR   0x800u

/* PORTSC bits. */
#define PS_CCS 0x00000001u
#define PS_PED 0x00000002u
#define PS_PR  0x00000010u
#define PS_PP  0x00000200u
#define PS_CSC 0x00020000u
#define PS_PEC 0x00040000u
#define PS_WRC 0x00080000u
#define PS_OCC 0x00100000u
#define PS_PRC 0x00200000u
#define PS_PLC 0x00400000u
#define PS_CEC 0x00800000u
#define PS_CAS 0x01000000u
#define PS_CHG (PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | \
                PS_CEC | PS_CAS)

/* ---- runtime registers (BAR0 + rtsoff +) ---- */
#define RT_IMAN   0x20
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP   0x38

/* ---- TRB types: transfers 1-8, commands 9+ ---- */
#define T_NORMAL  1u
#define T_SETUP   2u
#define T_DATA    3u
#define T_STATUS  4u
#define T_LINK    6u
#define T_ENSLOT  9u
#define T_DISSLOT 10u
#define T_ADDRDEV 11u
#define T_CONFEP  12u
#define T_NOOP    23u

/* ---- endpoint types ---- */
#define EP_CONTROL 4u
#define EP_INT_IN  7u

#define TRBS_PER_RING 256
#define XHCI_MAX_SLOTS 16

static uint64_t g_base;   /* MMIO base (mapped, VA == phys) */
static uint64_t g_op;     /* operational base */
static uint64_t g_db;     /* doorbell base */
static uint64_t g_rt;     /* runtime base (interrupter 0) */
static int g_nports;
static int g_ctx_size;    /* 32 or 64 from HCCPARAMS1.CSZ */
static int g_ok;
static int g_live;        /* rings programmed: events may be consumed */

/* MMIO helpers take ABSOLUTE addresses (g_base/g_op/g_db/g_rt are
 * absolute; only the CAP probe adds g_base explicitly). */
static uint32_t r32(uint64_t addr) {
  return *(volatile uint32_t *)(uintptr_t)addr;
}

static void w32(uint64_t addr, uint32_t v) {
  *(volatile uint32_t *)(uintptr_t)addr = v;
}

static void w64(uint64_t addr, uint64_t v) {
  w32(addr, (uint32_t)v);
  w32(addr + 4, (uint32_t)(v >> 32));
}

/* ---- transfer/command ring ---- */
typedef struct {
  uint64_t base;
  uint32_t idx;
  uint32_t cycle;
} ring_t;

static ring_t g_cmd;
static int g_last_cc; /* completion code of the last waited command */
static uint64_t g_dcbaa; /* 256-entry array page */
static uint64_t g_erst;  /* 1-entry segment table */
static uint64_t g_ering; /* event ring page */
static uint32_t g_ev_idx;
static uint32_t g_ev_cycle;

/* Per-slot state. */
static uint64_t g_out_ctx[XHCI_MAX_SLOTS];
static uint64_t g_in_ctx;
static ring_t g_ep0[XHCI_MAX_SLOTS];
static ring_t g_epx[XHCI_MAX_SLOTS]; /* one extra EP per slot (INT-IN) */
static int g_ep0_mps[XHCI_MAX_SLOTS];

static void trb_write(uint64_t addr, uint32_t dw0, uint32_t dw1,
                      uint32_t dw2, uint32_t dw3) {
  volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)addr;
  t[0] = dw0;
  t[1] = dw1;
  t[2] = dw2;
  t[3] = dw3;
}

/* Enqueue on a producer ring (command or transfer). */
static void ring_push(ring_t *r, uint32_t dw0, uint32_t dw1, uint32_t dw2,
                      uint32_t dw3_nocycle) {
  trb_write(r->base + (uint64_t)r->idx * 16u, dw0, dw1, dw2,
            dw3_nocycle | (r->cycle & 1u));
  r->idx++;
  if (r->idx == TRBS_PER_RING - 1) {
    /* Link back to the start (toggle cycle). */
    trb_write(r->base + (uint64_t)r->idx * 16u, (uint32_t)r->base,
              (uint32_t)(r->base >> 32), 0,
              (T_LINK << 10) | (1u << 1) | (r->cycle & 1u));
    r->cycle ^= 1u;
    r->idx = 0;
  }
}

static void doorbell(uint32_t slot, uint32_t dci) {
  *(volatile uint32_t *)(uintptr_t)(g_db + (uint64_t)slot * 4u) = dci & 0xFFu;
}

/* Pop one event; 1 = got it. Advances ERDP. */
int xhci_next_event(xhci_ev_t *ev) {
  volatile uint32_t *t;
  uint32_t dw2, dw3;
  if (!g_live || !ev) return 0;
  t = (volatile uint32_t *)(uintptr_t)(g_ering + (uint64_t)g_ev_idx * 16u);
  dw3 = t[3];
  if (((dw3 & 1u)) != (g_ev_cycle & 1u)) return 0; /* empty */
  dw2 = t[2];
  ev->type = (uint8_t)((dw3 >> 10) & 0x3Fu);
  ev->cc = (uint8_t)((dw2 >> 24) & 0xFFu);
  ev->slot = (uint8_t)((dw3 >> 24) & 0xFFu);
  ev->ep = (uint8_t)((dw3 >> 16) & 0x1Fu);
  ev->len = dw2 & 0xFFFFFFu;
  ev->ptr = ((uint64_t)t[1] << 32) | t[0];
  g_ev_idx++;
  if (g_ev_idx == TRBS_PER_RING) {
    /* No Link TRB on event rings: the HC wraps at ERST size. */
    g_ev_idx = 0;
    g_ev_cycle ^= 1u;
  }
  w64(g_rt + RT_ERDP,
      (g_ering + (uint64_t)g_ev_idx * 16u) | 8u /* EHB */);
  return 1;
}

/* Wait for a command completion for slot (any slot if want_slot < 0).
 * Returns completion code, or -1 on timeout. Consumes events.
 * NOTE: the event ring lives in cached RAM, so this spins fast: the
 * count buys ~0.5s for the emulated HC to answer. */
static int cmd_wait(int want_slot, uint8_t *slot_out) {
  uint32_t t = 100000000;
  xhci_ev_t ev;
  g_last_cc = -1;
  while (t--) {
    if (!xhci_next_event(&ev)) continue;
    if (ev.type != XHCI_EV_CMD) continue; /* drop transfers/others here */
    if (want_slot >= 0 && ev.slot != (uint8_t)want_slot) continue;
    if (slot_out) *slot_out = ev.slot;
    g_last_cc = ev.cc;
    return ev.cc;
  }
  return -1;
}

/* Wait for a transfer completion on (slot, dci). Returns CC or -1. */
static uint64_t g_want_ptr; /* status-TRB addr to match (0 = first match) */
static int xfer_wait(uint8_t slot, int dci, uint32_t *resid) {
  uint32_t t = 100000000;
  xhci_ev_t ev;
  g_last_cc = -1;
  while (t--) {
    if (!xhci_next_event(&ev)) continue;
    if (ev.type != XHCI_EV_XFER || ev.slot != slot ||
        ev.ep != (uint8_t)dci ||
        (g_want_ptr != 0 && ev.ptr != g_want_ptr))
      continue;
    if (resid) *resid = ev.len;
    g_last_cc = ev.cc;
    g_want_ptr = 0;
    return ev.cc;
  }
  g_want_ptr = 0;
  return -1;
}

int xhci_last_cc(void) { return g_last_cc; }

static void cmd_trb(uint32_t type, uint8_t slot, uint64_t p0, uint64_t p1) {
  ring_push(&g_cmd, (uint32_t)p0, (uint32_t)(p0 >> 32),
            (uint32_t)p1, /* DW2 unused by our commands */
            (type << 10) | ((uint32_t)slot << 24));
  doorbell(0, 0);
}

int xhci_enable_slot(uint8_t *slot_out) {
  uint8_t slot = 0;
  int cc;
  if (!g_ok) return -1;
  cmd_trb(T_ENSLOT, 0, 0, 0);
  cc = cmd_wait(-1, &slot);
  if (cc != XHCI_CC_SUCCESS || slot == 0 ||
      slot >= XHCI_MAX_SLOTS)
    return -1;
  if (slot_out) *slot_out = slot;
  return 0;
}

int xhci_disable_slot(uint8_t slot) {
  int cc;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS) return -1;
  cmd_trb(T_DISSLOT, slot, 0, 0);
  cc = cmd_wait(slot, 0);
  g_out_ctx[slot] = 0;
  ((uint64_t *)(uintptr_t)g_dcbaa)[slot] = 0;
  g_ep0[slot].base = 0;
  g_epx[slot].base = 0;
  return (cc == XHCI_CC_SUCCESS) ? 0 : -1;
}

/* Slot context DW0: route 0, speed, entries. DW1: root port at 23:16. */
static void slot_ctx(uint8_t *ctx, int port, int speed, int entries) {
  uint32_t *s = (uint32_t *)(uintptr_t)ctx;
  naos_memset(ctx, (size_t)g_ctx_size, 0);
  s[0] = ((uint32_t)speed << 20) | ((uint32_t)(entries & 0x1F) << 27);
  s[1] = (uint32_t)(port & 0xFF) << 16;
}

/* Endpoint context: type, CErr=3, TR ring + DCS, packet/interval.
 * (DCS is bit 0 of the 64-bit TR pointer, i.e. DW2 bit 0.) Periodic
 * endpoints also need bandwidth fields (avg length + max ESIT payload).
 * Max packet goes in DW4 and DW1 (spec vs older readers disagree). */
static void ep_ctx(uint8_t *ctx, uint64_t ring, int type, int mps,
                   int interval) {
  uint32_t *e = (uint32_t *)(uintptr_t)ctx;
  uint32_t mp = (uint32_t)(mps & 0xFFFF);
  naos_memset(ctx, (size_t)g_ctx_size, 0);
  e[0] = (uint32_t)(interval & 0xFF) << 16;
  e[1] = ((uint32_t)(type & 0x7) << 3) | (3u << 1) | /* CErr=3 */
         (mp << 16);
  e[2] = (uint32_t)ring | 1u; /* DCS=1 */
  e[3] = (uint32_t)(ring >> 32);
  e[4] = mp << 16;
  if (type != 4u)
    e[5] = (mp << 16) | mp;
}

int xhci_address_device(uint8_t slot, int port, int speed) {
  uint8_t *in;
  int cc;
  /* EP0 maxpacket 8 for the initial address (mirrors firmware: the first
   * descriptor probe is 8 bytes, which reveals the true mps). */
  const int ep0_mps = 8;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS) return -1;
  if (!g_ep0[slot].base) {
    uint64_t rp = pmm_alloc_page();
    if (!rp) return -1;
    naos_memset((void *)(uintptr_t)rp, 0, 0x1000);
    g_ep0[slot].base = rp;
    g_ep0[slot].idx = 0;
    g_ep0[slot].cycle = 1;
    trb_write(rp + (TRBS_PER_RING - 1) * 16u, (uint32_t)rp,
              (uint32_t)(rp >> 32), 0, (T_LINK << 10) | (1u << 1) | 1u);
  }
  in = (uint8_t *)(uintptr_t)g_in_ctx;
  naos_memset(in, 0x1000, 0);
  /* Input control (always 32B): DW0 drop = none, DW1 add = slot + EP0. */
  ((uint32_t *)in)[0] = 0x00u;
  ((uint32_t *)in)[1] = 0x03u;
  slot_ctx(in + 32, port, speed, 1);
  ep_ctx(in + 32 + (size_t)g_ctx_size, g_ep0[slot].base, EP_CONTROL,
         ep0_mps, 0);
  g_ep0_mps[slot] = ep0_mps;
  /* Output device context for the slot. */
  if (!g_out_ctx[slot]) {
    uint64_t p = pmm_alloc_page();
    if (!p) return -1;
    g_out_ctx[slot] = p;
  }
  naos_memset((void *)(uintptr_t)g_out_ctx[slot], 0, 0x1000);
  ((uint64_t *)(uintptr_t)g_dcbaa)[slot] = g_out_ctx[slot];
  cmd_trb(T_ADDRDEV, slot, g_in_ctx, 0);
  cc = cmd_wait(slot, 0);
  return (cc == XHCI_CC_SUCCESS) ? 0 : -1;
}

/* Queue a control transfer on EP0 (DCI 1). Status dir auto: IN unless
 * the data stage is IN. Waits for the single (IOC) completion. */
int xhci_control(uint8_t slot, const uint8_t setup[8], void *data,
                 uint32_t len, int dir_in) {
  uint32_t dw0, dw1, trt;
  int cc;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS || !setup) return -1;
  dw0 = setup[0] | ((uint32_t)setup[1] << 8) | ((uint32_t)setup[2] << 16) |
        ((uint32_t)setup[3] << 24);
  dw1 = setup[4] | ((uint32_t)setup[5] << 8) | ((uint32_t)setup[6] << 16) |
        ((uint32_t)setup[7] << 24);
  /* Layout mirrors EDK2 (proven against QEMU): length-only DW2; the
   * data-stage direction rides in DW3 (TRT: 3 = IN, 2 = OUT, 0 = none;
   * data/status carry bit 16). IOC on every stage. */
  trt = (len == 0) ? 0u : (dir_in ? 3u : 2u);
  ring_push(&g_ep0[slot], dw0, dw1, 8u,
            (T_SETUP << 10) | (1u << 6) /* IDT */ | (1u << 5) /* IOC */ |
            (trt << 16));
  if (len > 0 && data) {
    uint64_t buf = (uint64_t)(uintptr_t)data;
    ring_push(&g_ep0[slot], (uint32_t)buf, (uint32_t)(buf >> 32),
              len, (T_DATA << 10) | (1u << 5) /* IOC */ |
              (1u << 2) /* ISP */ | (1u << 16));
  }
  /* Status: DW2 0, DW3 like data (bit 16 set), IOC. Wait for its
   * completion (the TD's last event); earlier stages are skipped. */
  g_want_ptr =
    g_ep0[slot].base + (uint64_t)g_ep0[slot].idx * 16u;
  ring_push(&g_ep0[slot], 0, 0, 0,
            (T_STATUS << 10) | (1u << 5) /* IOC */ | (1u << 16));
  doorbell(slot, 1);
  cc = xfer_wait(slot, 1, 0);
  return (cc == XHCI_CC_SUCCESS) ? 0 : -1;
}

int xhci_open_ep(uint8_t slot, int dci) {
  uint64_t p;
  (void)dci;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS) return -1;
  if (g_epx[slot].base) return 0;
  p = pmm_alloc_page();
  if (!p) return -1;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  g_epx[slot].base = p;
  g_epx[slot].idx = 0;
  g_epx[slot].cycle = 1;
  /* Link TRB at the end (producer cycle 1). */
  trb_write(p + (TRBS_PER_RING - 1) * 16u, (uint32_t)p,
            (uint32_t)(p >> 32), 0, (T_LINK << 10) | (1u << 1) | 1u);
  return 0;
}

int xhci_configure_ep(uint8_t slot, int config, int iface, int ep_num,
                      int ep_type, int mps, int interval) {
  uint8_t *in;
  int idx, cc;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS) return -1;
  if (!g_epx[slot].base) return -1;
  /* Device contexts are DCI-indexed (slot=0, EP0=1, EP1-IN=3). */
  idx = (ep_num == 0) ? 1 : ep_num * 2 + 1;
  in = (uint8_t *)(uintptr_t)g_in_ctx;
  naos_memset(in, 0x1000, 0);
  /* DW0 drop = none; DW1 add = slot + EPx. EP0 is deliberately NOT
   * re-added: it is unchanged since Address Device, and re-adding the
   * live EP0 context makes QEMU reject the command (real HCs tolerate
   * both forms). */
  ((uint32_t *)in)[0] = 0x00u;
  ((uint32_t *)in)[1] = 0x01u | (1u << idx);
  ((uint32_t *)in)[2] = (uint32_t)(config & 0xFF) |
                        ((uint32_t)(iface & 0xFF) << 8);
  /* Copy the live EP0 context back in (HC owns the output copy). */
  {
    uint8_t *out = (uint8_t *)(uintptr_t)g_out_ctx[slot];
    naos_memcpy(in + 32 + (size_t)g_ctx_size, out + (size_t)g_ctx_size,
                (size_t)g_ctx_size);
  }
  /* Slot: copy the live output (address + state must survive), widen
   * entries to the new endpoint. */
  {
    uint32_t *o = (uint32_t *)(uintptr_t)g_out_ctx[slot];
    uint32_t *s = (uint32_t *)(uintptr_t)(in + 32);
    for (int k = 0; k < 8; k++) s[k] = o[k];
    s[0] = (s[0] & ~(0x1Fu << 27)) | ((uint32_t)(idx & 0x1F) << 27);
  }
  ep_ctx(in + 32 + (size_t)g_ctx_size * (size_t)idx,
         g_epx[slot].base, ep_type, mps, interval);
  cmd_trb(T_CONFEP, slot, g_in_ctx, 0);
  cc = cmd_wait(slot, 0);
  return (cc == XHCI_CC_SUCCESS) ? 0 : -1;
}

int xhci_int_in(uint8_t slot, int dci, void *buf, uint32_t len) {
  uint64_t b;
  if (!g_ok || slot == 0 || slot >= XHCI_MAX_SLOTS || !buf || len == 0)
    return -1;
  if (!g_epx[slot].base) return -1;
  b = (uint64_t)(uintptr_t)buf;
  ring_push(&g_epx[slot], (uint32_t)b, (uint32_t)(b >> 32), len,
            (T_NORMAL << 10) | (1u << 5) /* IOC */ | (1u << 2) /* ISP */ |
            (1u << 16));
  doorbell(slot, (uint32_t)dci);
  return 0;
}

int xhci_port_count(void) { return g_nports; }

int xhci_port_device(int port) {
  uint32_t ps;
  if (!g_ok || port < 1 || port > g_nports) return 0;
  ps = r32(g_op + OP_PORTSC + (uint64_t)(port - 1) * 0x10u);
  /* Clear stale change bits (write-1-to-clear), keep the rest. */
  w32(g_op + OP_PORTSC + (uint64_t)(port - 1) * 0x10u, ps | PS_CHG);
  ps = r32(g_op + OP_PORTSC + (uint64_t)(port - 1) * 0x10u);
  return (ps & PS_CCS) ? 1 : 0;
}

int xhci_port_speed(int port) {
  uint32_t ps;
  if (!g_ok || port < 1 || port > g_nports) return 0;
  ps = r32(g_op + OP_PORTSC + (uint64_t)(port - 1) * 0x10u);
  if (!(ps & PS_CCS)) return 0;
  return (int)((ps >> 10) & 0xFu);
}

/* Reset one port (power on, PR, wait PRC). 0 ok. */
int xhci_port_reset(int port) {
  uint64_t off = g_op + OP_PORTSC + (uint64_t)(port - 1) * 0x10u;
  uint32_t ps = r32(off);
  uint32_t t;
  if (!(ps & PS_PP)) {
    w32(off, ps | PS_PP);
    {
      volatile uint32_t d = 2000000;
      while (d--) { }
    }
    ps = r32(off);
  }
  w32(off, (ps & ~(0x0000003Cu)) | PS_PR | PS_CHG);
  t = 4000000;
  while (t--) {
    ps = r32(off);
    if ((ps & PS_PRC) && !(ps & PS_PR)) break;
  }
  w32(off, ps | PS_CHG); /* clear PRC + any raised change */
  ps = r32(off);
  if (!(ps & PS_CCS) || !(ps & PS_PED)) return -1;
  return 0;
}

int xhci_init(void) {
  uint8_t bus, slot, func;
  uint32_t bar_lo, bar_hi, cmd;
  uint64_t bar;
  uint8_t caplen;
  uint32_t hcs1, hcc1;
  uint64_t p;
  uint32_t t;
  g_ok = 0;
  g_live = 0;
  if (!pci_find_class_pi(0x0C, 0x03, 0x30, &bus, &slot, &func)) return 0;
  kprintf("xhci: PCI %x:%x.%u\n", (unsigned)bus, (unsigned)slot,
          (unsigned)func);
  cmd = pci_read16(bus, slot, func, 0x04);
  pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd | 0x06u));
  /* BAR0 (assume 64-bit MMIO; fall back to 32-bit). */
  bar_lo = pci_read(bus, slot, func, 0x10);
  if (bar_lo & 1u) return 0;
  if ((bar_lo & 6u) == 4u) {
    bar_hi = pci_read(bus, slot, func, 0x14);
    bar = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFu);
  } else {
    bar = bar_lo & ~0xFu;
  }
  if (bar == 0) return 0; /* 64-bit-high BARs are fine: vmm maps them */
  /* Map the register window (cap/op/ports/doors/runtime live low). */
  for (uint64_t o = 0; o < 0x8000u; o += 0x200000u) {
    if (vmm_map_mmio((bar + o) & ~0x1FFFFFULL) != 0) return 0;
  }
  g_base = bar;
  caplen = (uint8_t)(r32(g_base + CAP_CAPLEN) & 0xFFu);
  kprintf("xhci: bar=%p caplen=%x dboff=%x rtsoff=%x\n",
          (void *)(uintptr_t)bar, (unsigned)caplen,
          (unsigned)(r32(g_base + CAP_DBOFF) & ~0x3u),
          (unsigned)(r32(g_base + CAP_RTSOFF) & ~0x1Fu));
  if (caplen < 0x20) return 0;
  g_op = bar + caplen;
  g_db = bar + (r32(g_base + CAP_DBOFF) & ~0x3u);
  g_rt = bar + (r32(g_base + CAP_RTSOFF) & ~0x1Fu);
  hcs1 = r32(g_base + CAP_HCSP1);
  hcc1 = r32(g_base + CAP_HCCP1);
  g_nports = (int)((hcs1 >> 24) & 0xFFu);
  g_ctx_size = (hcc1 & 0x04u) ? 64 : 32;
  kprintf("xhci: ports=%d ctx=%d slots=%u\n", g_nports, g_ctx_size,
          (unsigned)(hcs1 & 0xFFu));
  if (g_nports <= 0 || g_nports > 64) return 0;
  if (!(r32(g_op + OP_PAGESZ) & 1u)) return 0; /* need 4K pages */
  /* BIOS->OS ownership handoff (USBLEGCTLSTS). Firmware owns USB on real
   * machines; without this our port resets fight it and fail. Absent on
   * QEMU (no legacy cap) -> skipped silently. xECP is a byte offset;
   * chain Next pointers count DWORDs. Fully guarded: worst case we skip. */
  {
    uint32_t xecp = (hcc1 >> 16) & 0xFFFFu;
    uint32_t ptr = xecp;
    int guard = 0;
    while (ptr != 0 && ptr < 0x1000 && guard++ < 16) {
      uint32_t cap = r32(g_base + ptr);
      uint32_t next = ((cap >> 8) & 0xFFu) * 4u; /* DWORDs -> bytes */
      if ((cap & 0xFFu) == 1u) {                 /* USB Legacy Support */
        uint32_t leg = cap;
        if (leg & (1u << 16)) { /* BIOS owned: take over */
          uint32_t t2;
          w32(g_base + ptr, leg | (1u << 24));
          t2 = 4000000;
          while ((r32(g_base + ptr) & (1u << 16)) && t2--) { }
          if (r32(g_base + ptr) & (1u << 16))
            kprintf("xhci: BIOS would not release\n");
          else
            kprintf("xhci: took over from BIOS\n");
        }
        break;
      }
      if (next == 0) break;
      ptr += next;
    }
  }
  /* Halt, reset, wait ready. */
  w32(g_op + OP_USBCMD, r32(g_op + OP_USBCMD) & ~CMD_RS);
  t = 1000000;
  while (!(r32(g_op + OP_USBSTS) & STS_HCH) && t--) { }
  w32(g_op + OP_USBCMD, r32(g_op + OP_USBCMD) | CMD_HCRST);
  t = 4000000;
  while ((r32(g_op + OP_USBCMD) & CMD_HCRST) && t--) { }
  if (r32(g_op + OP_USBCMD) & CMD_HCRST) {
    kprintf("xhci: HCRST stuck\n");
    return 0;
  }
  t = 4000000;
  while ((r32(g_op + OP_USBSTS) & STS_CNR) && t--) { }
  /* Rings + tables. */
  p = pmm_alloc_page();
  if (!p) return 0;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  g_cmd.base = p;
  g_cmd.idx = 0;
  g_cmd.cycle = 1;
  trb_write(p + (TRBS_PER_RING - 1) * 16u, (uint32_t)p,
            (uint32_t)(p >> 32), 0, (T_LINK << 10) | (1u << 1) | 1u);
  p = pmm_alloc_page();
  if (!p) return 0;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  g_dcbaa = p;
  w64(g_op + OP_DCBAAP, g_dcbaa);
  p = pmm_alloc_page();
  if (!p) return 0;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  g_ering = p;
  p = pmm_alloc_page();
  if (!p) return 0;
  {
    uint64_t *erst = (uint64_t *)(uintptr_t)p;
    naos_memset(erst, 0, 0x1000);
    erst[0] = g_ering;
    erst[1] = TRBS_PER_RING;
    g_erst = p;
  }
  g_ev_idx = 0;
  g_ev_cycle = 1;
  {
    uint64_t maxslots = hcs1 & 0xFFu;
    uint64_t en = (maxslots > XHCI_MAX_SLOTS) ? XHCI_MAX_SLOTS : maxslots;
    w32(g_op + OP_CONFIG, (uint32_t)en);
  }
  w64(g_op + OP_CRCR, g_cmd.base | 1u /* RCS */);
  w32(g_rt + RT_ERSTSZ, 1);
  w64(g_rt + RT_ERSTBA, g_erst);
  w64(g_rt + RT_ERDP, g_ering);
  p = pmm_alloc_page();
  if (!p) return 0;
  naos_memset((void *)(uintptr_t)p, 0, 0x1000);
  g_in_ctx = p;
  g_live = 1; /* rings are programmed: command waits may consume */
  /* Start. */
  w32(g_op + OP_USBCMD, r32(g_op + OP_USBCMD) | CMD_RS);
  t = 4000000;
  while ((r32(g_op + OP_USBSTS) & STS_HCH) && t--) { }
  kprintf("xhci: usbsts=%x\n", (unsigned)r32(g_op + OP_USBSTS));
  if (r32(g_op + OP_USBSTS) & STS_HCH) return 0;
  /* Self-test: No-Op command must complete. */
  cmd_trb(T_NOOP, 0, 0, 0);
  {
    int cc = cmd_wait(0, 0);
    if (cc != XHCI_CC_SUCCESS) {
      kprintf("xhci: noop cc=%d\n", cc);
      return 0;
    }
  }
  kprintf("xhci: running, noop ok\n");
  g_ok = 1;
  return 1;
}
