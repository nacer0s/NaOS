/* NaOS handoff: bootloader -> kernel. Own ABI (not Multiboot). Both sides include this. */
#ifndef NAOS_BOOTINFO_H
#define NAOS_BOOTINFO_H

#include <stdint.h>

#define NAOS_BOOTINFO_MAGIC 0x544F4F42536F614EULL /* "NaOSBOOT" LE */

typedef struct {
  uint64_t magic;          /* NAOS_BOOTINFO_MAGIC */
  /* Framebuffer (GOP, identity-mapped by firmware) */
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;       /* bytes per scanline */
  uint32_t fb_bpp;         /* bits per pixel (expect 32) */
  uint32_t fb_format;      /* 0 = RGBX (R first), 1 = BGRX (B first) */
  uint32_t _reserved;
  /* UEFI memory map copy (EFI_MEMORY_DESCRIPTOR array, DescriptorSize stride) */
  uint64_t mmap_addr;
  uint64_t mmap_count;
  uint64_t mmap_entry_size;
  uint32_t mmap_version;
  uint32_t _reserved2;
  uint64_t rsdp;           /* ACPI RSDP address, 0 if unknown (P5+) */
} naos_bootinfo_t;

#endif /* NAOS_BOOTINFO_H */
