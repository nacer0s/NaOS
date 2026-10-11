/* NaOS USB HID boot keyboard. Enumerates the first boot-protocol
 * keyboard on the XHCI root hub, configures its interrupt-IN pipe, and
 * turns 8-byte reports into set-1 scancodes for the shared kbd ring. */
#include "usb_kbd.h"
#include "xhci.h"
#include "ps2_kbd.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

/* USB setup packets (8 bytes). */
static void setup_pkt(uint8_t b[8], uint8_t rt, uint8_t req, uint16_t val,
                      uint16_t idx, uint16_t len) {
  b[0] = rt;
  b[1] = req;
  b[2] = (uint8_t)val;
  b[3] = (uint8_t)(val >> 8);
  b[4] = (uint8_t)idx;
  b[5] = (uint8_t)(idx >> 8);
  b[6] = (uint8_t)len;
  b[7] = (uint8_t)(len >> 8);
}

static int get_desc(uint8_t slot, uint8_t type, uint8_t idx, void *buf,
                    uint16_t len) {
  uint8_t s[8];
  setup_pkt(s, 0x80, 6, (uint16_t)((type << 8) | idx), 0, len);
  return xhci_control(slot, s, buf, len, 1);
}

/* HID usage (boot table) -> set-1 make code; 0xE0xx = extended. */
static uint16_t hid_to_set1(uint8_t u) {
  static const uint8_t az[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C
  };
  static const uint8_t d09[10] = {
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
  };
  static const uint8_t fkeys[12] = {
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58
  };
  static const uint8_t kp[10] = {
    0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52
  };
  if (u >= 0x04 && u <= 0x1D) return az[u - 0x04];
  if (u >= 0x1E && u <= 0x27) return d09[(u - 0x1E) % 10];
  switch (u) {
    case 0x28: return 0x1C; /* enter */
    case 0x29: return 0x01; /* esc */
    case 0x2A: return 0x0E; /* backspace */
    case 0x2B: return 0x0F; /* tab */
    case 0x2C: return 0x39; /* space */
    case 0x2D: return 0x0C;
    case 0x2E: return 0x0D;
    case 0x2F: return 0x1A;
    case 0x30: return 0x1B;
    case 0x31: return 0x2B;
    case 0x33: return 0x27;
    case 0x34: return 0x28;
    case 0x35: return 0x29;
    case 0x36: return 0x33;
    case 0x37: return 0x34;
    case 0x38: return 0x35;
    case 0x39: return 0x3A; /* caps */
    case 0x46: return 0x46; /* scroll */
    case 0x49: return 0xE052; /* insert */
    case 0x4A: return 0xE047; /* home */
    case 0x4B: return 0xE049; /* pgup */
    case 0x4C: return 0xE053; /* delete */
    case 0x4D: return 0xE04F; /* end */
    case 0x4E: return 0xE051; /* pgdn */
    case 0x4F: return 0xE04D; /* right */
    case 0x50: return 0xE04B; /* left */
    case 0x51: return 0xE050; /* down */
    case 0x52: return 0xE048; /* up */
    case 0x53: return 0x45;   /* numlock */
    case 0x54: return 0xE035; /* kp / */
    case 0x55: return 0x37;   /* kp * */
    case 0x56: return 0x4A;   /* kp - */
    case 0x57: return 0x4E;   /* kp + */
    case 0x58: return 0xE01C; /* kp enter */
    case 0x63: return 0x53;   /* kp . */
    default: break;
  }
  if (u >= 0x3A && u <= 0x45) return fkeys[u - 0x3A];
  if (u >= 0x59 && u <= 0x62) return kp[u - 0x59]; /* kp 1..0 */
  return 0;
}

static void push_set1(uint16_t s1, int release) {
  if (s1 >= 0xE000) {
    kbd_push(0xE0);
    kbd_push((uint8_t)((s1 & 0xFFu) | (release ? 0x80u : 0u)));
  } else if (s1 != 0) {
    kbd_push((uint8_t)(s1 | (release ? 0x80u : 0u)));
  }
}

static int g_slot;
static int g_dci = 3; /* EP1-IN */
static int g_ready;
static int g_scan_cooldown;
static int g_scan_fails;
static uint8_t g_report[8];
static uint8_t g_prev[8];

static void usb_kbd_scan(void); /* defined below init */

/* Diff one report against the previous: releases then presses. */
static void handle_report(const uint8_t *r) {
  static const uint8_t mod_mask[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
  };
  static const uint16_t mod_key[8] = {
    0xE01D, 0x2A, 0xE038, 0x00, 0xE01D, 0x36, 0xE038, 0x00
  };
  int i, j;
  uint8_t mods = r[0] ^ g_prev[0];
  for (i = 0; i < 8; i++) {
    if ((mods & mod_mask[i]) && mod_key[i])
      push_set1(mod_key[i], (r[0] & mod_mask[i]) ? 0 : 1);
  }
  /* Releases: in prev, not now. */
  for (i = 0; i < 6; i++) {
    uint8_t k = g_prev[2 + i];
    int still = 0;
    if (k == 0 || k == 1) continue; /* 1 = rollover: drop */
    for (j = 0; j < 6; j++) {
      if (r[2 + j] == k) { still = 1; break; }
    }
    if (!still) push_set1(hid_to_set1(k), 1);
  }
  /* Presses: now, not in prev. */
  for (i = 0; i < 6; i++) {
    uint8_t k = r[2 + i];
    int was = 0;
    if (k == 0 || k == 1) continue;
    for (j = 0; j < 6; j++) {
      if (g_prev[2 + j] == k) { was = 1; break; }
    }
    if (!was) push_set1(hid_to_set1(k), 0);
  }
  for (i = 0; i < 8; i++) g_prev[i] = r[i];
}

/* Config descriptor walk: find HID boot-keyboard interface + INT-IN ep. */
static int find_kbd(const uint8_t *cfg, uint32_t total, int *iface_out,
                    int *ep_out, int *mps_out, int *ival_out) {
  uint32_t o = 0;
  int iface = -1, ep = -1, mps = 8, ival = 10;
  int is_kbd = 0;
  while (o + 2 <= total) {
    uint8_t len = cfg[o];
    uint8_t type = cfg[o + 1];
    if (len < 2 || o + len > total) break;
    if (type == 4 && len >= 9) { /* interface */
      iface = cfg[o + 2];
      is_kbd = (cfg[o + 5] == 3 && cfg[o + 6] == 1 && cfg[o + 7] == 1);
    } else if (type == 5 && len >= 7 && is_kbd && ep < 0) {
      uint8_t addr = cfg[o + 2];
      uint8_t attr = cfg[o + 3];
      if ((addr & 0x80) && (attr & 3) == 3) { /* interrupt IN */
        ep = addr & 0x0F;
        mps = cfg[o + 4] | (cfg[o + 5] << 8);
        if (mps > 64) mps = 64;
        if (mps < 1) mps = 8;
        ival = cfg[o + 6];
        if (ival < 1) ival = 1;
      }
    }
    o += len;
  }
  if (iface < 0 || ep < 0) return -1;
  *iface_out = iface;
  *ep_out = ep;
  *mps_out = mps;
  *ival_out = ival;
  return 0;
}

int usb_kbd_init(void) {
  g_ready = 0;
  g_scan_cooldown = 0;
  g_scan_fails = 0;
  if (!xhci_init()) return 0;
  usb_kbd_scan();
  return g_ready;
}

/* The port/device half of init, reusable for hotplug rescan. */
static int enum_port(int port) {
  uint8_t slot = 0;
  int speed, iface, ep, mps, ival;
  uint32_t total;
  uint8_t s[8];
  uint8_t dev[18];
  uint8_t cfg9[9];
  uint8_t cfgfull[256];
  if (!xhci_port_device(port)) return 0;
  kprintf("usb: device on port %d\n", port);
  if (xhci_port_reset(port) != 0) {
    kprintf("usb: port %d reset failed\n", port);
    return 0;
  }
  speed = xhci_port_speed(port);
  kprintf("usb: port %d speed %d\n", port, speed);
  if (speed == 0) return 0;
  if (xhci_enable_slot(&slot) != 0) {
    kprintf("usb: port %d enable-slot failed\n", port);
    return 0;
  }
  kprintf("usb: port %d slot %u\n", port, (unsigned)slot);
  if (xhci_address_device(slot, port, speed) != 0) {
    kprintf("usb: port %d address failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  /* 8-byte probe first (learns bMaxPacketSize0), then the full one —
   * the same sequence firmware uses. */
  if (get_desc(slot, 1, 0, dev, 8) != 0) {
    kprintf("usb: port %d dev-desc8 failed cc=%d\n", port,
            xhci_last_cc());
    goto fail;
  }
  if (get_desc(slot, 1, 0, dev, 18) != 0) {
    kprintf("usb: port %d dev-desc failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  if (get_desc(slot, 2, 0, cfg9, 9) != 0) {
    kprintf("usb: port %d cfg-desc failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  total = (uint32_t)cfg9[2] | ((uint32_t)cfg9[3] << 8);
  if (total < 9 || total > sizeof(cfgfull)) goto fail;
  if (get_desc(slot, 2, 0, cfgfull, (uint16_t)total) != 0) {
    kprintf("usb: port %d full-cfg failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  if (find_kbd(cfgfull, total, &iface, &ep, &mps, &ival) != 0) {
    kprintf("usb: port %d not a boot keyboard\n", port);
    goto fail;
  }
  kprintf("usb: port %d iface %d ep %d mps %d ival %d\n", port, iface, ep,
          mps, ival);
  /* Boot protocol + idle + configure. */
  setup_pkt(s, 0x21, 0x0B, 0, (uint16_t)iface, 0); /* SET_PROTOCOL boot */
  if (xhci_control(slot, s, 0, 0, 0) != 0) {
    kprintf("usb: port %d set-protocol failed cc=%d\n", port,
            xhci_last_cc());
    goto fail;
  }
  setup_pkt(s, 0x21, 0x0A, 0, (uint16_t)iface, 0); /* SET_IDLE */
  if (xhci_control(slot, s, 0, 0, 0) != 0) {
    kprintf("usb: port %d set-idle failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  setup_pkt(s, 0x00, 0x09, cfg9[5], 0, 0); /* SET_CONFIGURATION */
  if (xhci_control(slot, s, 0, 0, 0) != 0) {
    kprintf("usb: port %d set-config failed cc=%d\n", port,
            xhci_last_cc());
    goto fail;
  }
  if (ep != 1) goto fail; /* boot keyboards use EP1 */
  if (xhci_open_ep(slot, 3) != 0) goto fail;
  if (xhci_configure_ep(slot, cfg9[5], iface, ep, 7, mps, ival) != 0) {
    kprintf("usb: port %d configure failed cc=%d\n", port, xhci_last_cc());
    goto fail;
  }
  g_slot = slot;
  naos_memset(g_report, 0, sizeof(g_report));
  naos_memset(g_prev, 0, sizeof(g_prev));
  if (xhci_int_in(slot, g_dci, g_report, 8) != 0) goto fail;
  g_ready = 1;
  g_scan_fails = 0;
  kprintf("usb: HID keyboard on port %d (slot %u)\n", port,
          (unsigned)slot);
  return 1;
fail:
  xhci_disable_slot(slot);
  return 0;
}

static void usb_kbd_scan(void) {
  int nports, port;
  nports = xhci_port_count();
  for (port = 1; port <= nports; port++) {
    if (enum_port(port)) return;
  }
  g_scan_fails++;
}

void usb_kbd_rescan(void) {
  if (g_ready) {
    kprintf("usb: keyboard already up\n");
    return;
  }
  g_scan_fails = 0;
  g_scan_cooldown = 0;
  usb_kbd_scan();
  if (!g_ready) kprintf("usb: no keyboard found\n");
}

void usb_kbd_poll(void) {
  xhci_ev_t ev;
  if (!g_ready) {
    /* Hotplug: rescan for a late-attached keyboard, throttled, with
     * backoff (a present-but-broken device must not stall ticks). */
    if (g_scan_cooldown > 0) {
      g_scan_cooldown--;
      return;
    }
    if (g_scan_fails >= 3) return;
    g_scan_cooldown = 500; /* ~5s at 100Hz */
    usb_kbd_scan();
    return;
  }
  while (xhci_next_event(&ev)) {
    if (ev.type != XHCI_EV_XFER) continue;
    if (ev.slot != (uint8_t)g_slot || ev.ep != (uint8_t)g_dci) continue;
    if (ev.cc != XHCI_CC_SUCCESS) {
      /* Stall/restart: requeue and carry on. */
      xhci_int_in((uint8_t)g_slot, g_dci, g_report, 8);
      continue;
    }
    handle_report(g_report);
    xhci_int_in((uint8_t)g_slot, g_dci, g_report, 8);
  }
}
