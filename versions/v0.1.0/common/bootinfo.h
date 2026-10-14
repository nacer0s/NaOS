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
  /* RAM disk (live boot): up to 4 page runs below 1G (EfiLoaderData).
   * All zero = absent. Kernel mounts it only if no real disk works.
   * rd_status says why when absent (1 = staged ok). */
  uint64_t rd_base[4];
  uint32_t rd_pages[4];
  uint32_t rd_status; /* 0=file absent/open fail 1=ready 2=page alloc fail
                       * 3=chunk buffer fail 4=read fail (see counts)
                       * 5=open failed 6=file-info failed 7=size rejected */
  uint32_t rd_done;   /* bytes staged before a read failure (status 4) */
  uint32_t rd_size;   /* file bytes total (status 4/7) */
  uint64_t rd_estatus; /* failing EFI status for 5/6 (0 when staged) */
} naos_bootinfo_t;

#endif /* NAOS_BOOTINFO_H */
