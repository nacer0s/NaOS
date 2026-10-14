/* NaOS kernel main: handoff -> serial/fb -> GDT/IDT/PIC/PIT/kbd/PMM -> shell. */
#include "../common/bootinfo.h"
#include "../common/cstring.h"
#include "../common/serial.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "drv/fb.h"
#include "drv/pit.h"
#include "drv/ps2_kbd.h"
#include "drv/blk.h"
#include "drv/ramblk.h"
#include "drv/usb_kbd.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "drv/acpi.h"
#include "fs/fat32.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "task.h"
#include "shell.h"
#include "version.h"

static naos_bootinfo_t g_bi;

extern char kernel_end[];
extern char stack_top[];

void kmain(naos_bootinfo_t *bi) {
  __asm__ volatile ("cli");

  /* Snapshot handoff first: the pointer aims at ex-firmware stack memory. */
  if (!bi) {
    for (;;) { __asm__ volatile ("hlt"); }
  }
  naos_memcpy(&g_bi, bi, sizeof(g_bi));

  serial_init();
  serial_puts("\n[NaOS] kernel starting\n");

  if (g_bi.magic != NAOS_BOOTINFO_MAGIC) {
    serial_puts("[NaOS] BAD bootinfo magic\n");
    for (;;) { __asm__ volatile ("hlt"); }
  }

  fb_init(g_bi.fb_base, g_bi.fb_width, g_bi.fb_height,
          g_bi.fb_pitch, g_bi.fb_bpp, g_bi.fb_format);
  if (!fb_ready()) {
    serial_puts("[NaOS] BAD framebuffer\n");
    for (;;) { __asm__ volatile ("hlt"); }
  }

  fb_clear(0x000000); /* plain black */
  kprintf("NaOS v%s - kernel OK\n", NAOS_VERSION);
  kprintf("fb %ux%u pitch=%u bpp=%u base=%p\n",
          (unsigned)g_bi.fb_width, (unsigned)g_bi.fb_height,
          (unsigned)g_bi.fb_pitch, (unsigned)g_bi.fb_bpp,
          (void *)(uintptr_t)g_bi.fb_base);

  gdt_init();
  idt_init();
  pic_init();
  pit_init(100);
  kbd_init();
  acpi_init(g_bi.rsdp);
  pmm_init(g_bi.mmap_addr, g_bi.mmap_count, g_bi.mmap_entry_size,
           (uint64_t)(uintptr_t)kernel_end);
  kmalloc_init();
  vmm_init(g_bi.fb_base, g_bi.fb_size);
  sched_init((uint64_t)(uintptr_t)stack_top);
  ramblk_init(g_bi.rd_base, g_bi.rd_pages); /* live image, maybe absent */
  if (ramblk_ok()) {
    kprintf("ramdisk: live image staged%s\n",
            g_bi.rd_status == 8 ? " (after retry)" : "");
  } else {
    static const char *const why[] = {
      "file not found", "ready?", "page alloc failed",
      "chunk buffer failed", "read failed", "open failed",
      "file info failed", "bad size"
    };
    uint32_t st = (g_bi.rd_status < 8) ? g_bi.rd_status : 0;
    if (st == 4)
      kprintf("ramdisk: absent (%s at %u/%u bytes)\n", why[st],
              (unsigned)g_bi.rd_done, (unsigned)g_bi.rd_size);
    else if (st == 5 || st == 6)
      kprintf("ramdisk: absent (%s, efi %p)\n", why[st],
              (void *)(uintptr_t)g_bi.rd_estatus);
    else if (st == 7)
      kprintf("ramdisk: absent (%s %u)\n", why[st],
              (unsigned)g_bi.rd_size);
    else
      kprintf("ramdisk: absent (%s)\n", why[st]);
  }
  blk_init(); /* AHCI, NVMe, probed ATA, then RAM-disk (needs PMM+VMM) */
  if (fat_init() != 0) {
    if (ramblk_ok()) {
      /* Chosen disk is foreign (e.g. a Windows volume): live session. */
      kprintf("fat: falling back to live image\n");
      blk_use_ramdisk();
      fat_init();
    } else {
      kprintf("fat: no usable disk (live image absent)\n");
    }
  }
  usb_kbd_init(); /* XHCI HID keyboard if present (optional) */
  net_init();     /* E1000 + ARP/ICMP if present (optional) */
  kprintf("memmap entries=%u entry_size=%u free=%u pages cr3=%p\n",
          (unsigned)g_bi.mmap_count, (unsigned)g_bi.mmap_entry_size,
          (unsigned)pmm_free_pages(), (void *)(uintptr_t)vmm_cr3());

  /* Heap smoke test before handing control to the shell. */
  {
    char *t = (char *)kmalloc(48);
    int ok = (t != 0);
    if (ok) {
      for (int i = 0; i < 48; i++) t[i] = (char)i;
      for (int i = 0; i < 48; i++) if (t[i] != (char)i) ok = 0;
      kfree(t);
      ok = ok && (kmalloc_used_bytes() == 0);
    }
    kprintf("heap selftest %s\n", ok ? "OK" : "FAIL");
  }

  pic_enable_irq(0); /* PIT */
  pic_enable_irq(1); /* PS/2 keyboard */
  __asm__ volatile ("sti");
  net_dhcp(); /* quick lease if a server answers, else keep static */

  shell_run(); /* never returns */
}
