/* NaOS ACPI shutdown (own table walker + minimal AML S5_ parser) and
 * 8042 reboot. Identity-mapped low RAM makes the BIOS-area scan trivial. */
#include "acpi.h"
#include "../lib/kprintf.h"
#include "../../common/io.h"
#include "../../common/cstring.h"

static int g_ok;
static uint16_t g_pm1a;
static uint16_t g_pm1b;
static uint16_t g_slp_a;
static uint16_t g_slp_b;

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int checksum(const uint8_t *p, uint32_t len) {
  uint8_t s = 0;
  for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
  return s == 0;
}

/* EBDA pointer lives at 0x40E (first KB, identity-mapped).
 * Absolute access trips -Warray-bounds, so silence it for this helper. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static uint16_t bda_word(uint64_t addr) {
  const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
#pragma GCC diagnostic pop

/* Find "RSD PTR " in EBDA low area + BIOS ROM 0xE0000-0xFFFFF. */
static uint64_t find_rsdp(void) {
  uint16_t ebda = bda_word(0x40E);
  uint32_t ebda_base = (uint32_t)ebda << 4;
  if (ebda_base >= 0x400 && ebda_base < 0xA0000) {
    for (uint32_t a = ebda_base; a < ebda_base + 0x400; a += 16) {
      const uint8_t *p = (const uint8_t *)(uintptr_t)a;
      if (naos_memcmp(p, "RSD PTR ", 8) == 0 && checksum(p, 20)) return a;
    }
  }
  for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    if (naos_memcmp(p, "RSD PTR ", 8) == 0 && checksum(p, 20)) return a;
  }
  return 0;
}

static const uint8_t *find_table(uint64_t rsdt, int is_xsdt, const char *sig) {
  const uint8_t *h = (const uint8_t *)(uintptr_t)rsdt;
  uint32_t len = rd32(h + 4);
  if (len < 36) return 0;
  uint32_t count = (len - 36) / (is_xsdt ? 8u : 4u);
  for (uint32_t i = 0; i < count; i++) {
    uint64_t addr = is_xsdt ? rd64(h + 36 + i * 8) : rd32(h + 36 + i * 4);
    if (addr < 0x1000 || addr >= 0x40000000ULL) continue; /* outside our map */
    const uint8_t *t = (const uint8_t *)(uintptr_t)addr;
    if (naos_memcmp(t, sig, 4) == 0) return t;
  }
  return 0;
}

/* Parse _S5_ NameOp package: NameOp _S5_ PkgOp len count v1 v2 ...
 * Values are Byte(0x0A)/Word(0x0B)/DWord(0x0C) prefixed. */
static int parse_s5(const uint8_t *dsdt, uint32_t aml_len,
                    uint16_t *a_out, uint16_t *b_out) {
  for (uint32_t i = 1; i + 8 < aml_len; i++) {
    if (dsdt[i] == '_' && dsdt[i + 1] == 'S' && dsdt[i + 2] == '5' &&
        dsdt[i + 3] == '_' && dsdt[i - 1] == 0x08) {
      uint32_t p = i + 4;
      if (dsdt[p++] != 0x12) continue; /* PackageOp */
      /* PkgLength: 1-4 bytes, top 2 bits = follow byte count. */
      uint8_t lead = dsdt[p];
      uint32_t nbytes = (uint32_t)(lead >> 6);
      if (nbytes > 3 || p + 1 + nbytes >= aml_len) continue;
      p += 1 + nbytes;
      p++; /* NumElements */
      uint16_t vals[2] = {0, 0};
      for (int v = 0; v < 2; v++) {
        if (p >= aml_len) return 0;
        uint8_t pre = dsdt[p++];
        if (pre == 0x00) { vals[v] = 0; }        /* ZeroOp */
        else if (pre == 0x0A) { vals[v] = dsdt[p++]; }
        else if (pre == 0x0B) {
          vals[v] = (uint16_t)(dsdt[p] | (dsdt[p + 1] << 8));
          p += 2;
        } else if (pre == 0x0C) {
          vals[v] = (uint16_t)rd32(dsdt + p);
          p += 4;
        } else if (pre == 0x01) { vals[v] = 1; } /* OneOp */
        else return 0;
      }
      *a_out = vals[0];
      *b_out = vals[1];
      return 1;
    }
  }
  return 0;
}

int acpi_init(uint64_t rsdp_hint) {
  g_ok = 0;
  uint64_t rsdp = 0;
  /* Prefer the bootloader's UEFI configuration-table address. */
  if (rsdp_hint >= 0x1000 && rsdp_hint < 0x400000000ULL) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)rsdp_hint;
    if (naos_memcmp(p, "RSD PTR ", 8) == 0 &&
        (checksum(p, 20) || checksum(p, 36)))
      rsdp = rsdp_hint;
  }
  if (rsdp == 0) rsdp = find_rsdp();
  if (rsdp == 0) {
    kprintf("acpi: no RSDP\n");
    return 0;
  }
  const uint8_t *r = (const uint8_t *)(uintptr_t)rsdp;
  int rev = r[15];
  uint64_t xsdt = 0;
  uint32_t rsdt = 0;
  if (rev >= 2 && checksum(r, 36)) {
    xsdt = rd64(r + 24);
  } else if (checksum(r, 20)) {
    rsdt = rd32(r + 16);
  } else {
    kprintf("acpi: bad RSDP checksum\n");
    return 0;
  }
  const uint8_t *facp = 0;
  if (xsdt) facp = find_table(xsdt, 1, "FACP");
  if (!facp && rsdt) facp = find_table(rsdt, 0, "FACP");
  if (!facp) {
    kprintf("acpi: no FACP\n");
    return 0;
  }
  uint32_t facp_len = rd32(facp + 4);
  if (facp_len < 116 || !checksum(facp, facp_len)) {
    kprintf("acpi: bad FACP\n");
    return 0;
  }
  /* Enable ACPI mode if the firmware offers the SMI doorbell. */
  uint32_t smi_cmd = rd32(facp + 48);
  uint8_t acpi_en = facp[52];
  g_pm1a = (uint16_t)(rd32(facp + 64) & 0xFFFFu);
  g_pm1b = (uint16_t)(rd32(facp + 68) & 0xFFFFu);
  if (smi_cmd && acpi_en) {
    naos_outb((uint16_t)smi_cmd, acpi_en);
    for (int i = 0; i < 1000; i++) {
      uint16_t pm1 = g_pm1a ? naos_inw(g_pm1a) : 0;
      if (pm1 & 1) break; /* SCI_EN */
      for (volatile int d = 0; d < 1000; d++) { }
    }
  }
  uint32_t dsdt = rd32(facp + 40);
  uint64_t x_dsdt = (facp_len >= 132) ? rd64(facp + 124) : 0;
  const uint8_t *d = 0;
  uint64_t daddr = x_dsdt ? x_dsdt : dsdt;
  if (daddr && daddr < 0x100000000ULL) d = (const uint8_t *)(uintptr_t)daddr;
  if (!d || naos_memcmp(d, "DSDT", 4) != 0) {
    kprintf("acpi: no DSDT\n");
    return 0;
  }
  uint32_t dlen = rd32(d + 4);
  if (dlen < 36 || !checksum(d, dlen)) {
    kprintf("acpi: bad DSDT\n");
    return 0;
  }
  uint32_t aml_off = 36;
  if (!parse_s5(d + aml_off, dlen - aml_off, &g_slp_a, &g_slp_b)) {
    kprintf("acpi: no _S5_\n");
    return 0;
  }
  g_ok = 1;
  kprintf("acpi: poweroff ready (PM1 %x/%x SLP %x/%x)\n",
          (unsigned)g_pm1a, (unsigned)g_pm1b,
          (unsigned)g_slp_a, (unsigned)g_slp_b);
  return 1;
}

int acpi_has_poweroff(void) { return g_ok; }

void acpi_poweroff(void) {
  if (!g_ok) return;
  if (g_pm1a) naos_outw(g_pm1a, (uint16_t)((g_slp_a << 10) | (1u << 13)));
  if (g_pm1b) naos_outw(g_pm1b, (uint16_t)((g_slp_b << 10) | (1u << 13)));
  /* If we're still here it didn't work; wait a moment then return. */
  for (volatile int i = 0; i < 1000000; i++) { }
}

void cpu_reboot(void) {
  /* 8042 reset pulse. */
  for (int i = 0; i < 100; i++) {
    if ((naos_inb(0x64) & 0x02) == 0) break;
  }
  naos_outb(0x64, 0xFE);
  for (volatile int i = 0; i < 1000000; i++) { }
}
