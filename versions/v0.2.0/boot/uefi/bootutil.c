/* NaOS bootloader GOP + ExitBootServices helpers. */
#include "bootutil.h"
#include "../../common/cstring.h"
#include "../../common/serial.h"

EFI_STATUS boot_get_gop(EFI_BOOT_SERVICES *bs, naos_bootinfo_t *bi) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
  serial_puts("gop: LocateProtocol...\n");
  EFI_STATUS st = bs->LocateProtocol(&kEfiGraphicsOutputProtocolGuid, 0,
                                    (VOID **)&gop);
  serial_puts("gop: LocateProtocol st=");
  serial_put_hex64((uint64_t)st);
  serial_puts(" gop=");
  serial_put_hex64((uint64_t)(uintptr_t)gop);
  serial_puts("\n");
  if (st != EFI_SUCCESS || !gop || !gop->Mode || !gop->Mode->Info) return 200;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;

  if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
      info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) return 201;

  bi->fb_base   = gop->Mode->FrameBufferBase;
  bi->fb_size   = gop->Mode->FrameBufferSize;
  bi->fb_width  = info->HorizontalResolution;
  bi->fb_height = info->VerticalResolution;
  bi->fb_pitch  = info->PixelsPerScanLine * 4u;
  bi->fb_bpp    = 32;
  bi->fb_format = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
                  ? 1u : 0u;
  if (bi->fb_base == 0 || bi->fb_width == 0 || bi->fb_height == 0) return 202;
  return EFI_SUCCESS;
}

EFI_STATUS boot_exit_services(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                              naos_bootinfo_t *bi) {
  /* Classic retry loop: keep ONE LoaderData buffer (survives ExitBootServices),
   * re-read the map into it and exit with no allocations in between. */
  UINTN size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_STATUS st = bs->GetMemoryMap(&size, 0, &key, &desc_size, &desc_ver);
  if (st != EFI_BUFFER_TOO_SMALL) {
    serial_puts("getmap probe st=");
    serial_put_hex64((uint64_t)st);
    serial_puts(" size=");
    serial_put_hex64((uint64_t)size);
    serial_puts("\n");
    return 300;
  }
  size += 4096 * 4; /* slack so the buffer never needs regrowing below */

  EFI_MEMORY_DESCRIPTOR *map = 0;
  st = bs->AllocatePool(EfiLoaderData, size, (VOID **)&map);
  if (st != EFI_SUCCESS) return 301;

  for (int attempt = 0; attempt < 5; attempt++) {
    UINTN sz = size;
    st = bs->GetMemoryMap(&sz, map, &key, &desc_size, &desc_ver);
    if (st != EFI_SUCCESS) return 302;
    st = bs->ExitBootServices(image, key);
    if (st == EFI_SUCCESS) {
      bi->mmap_addr = (uint64_t)(uintptr_t)map;
      bi->mmap_count = (desc_size != 0) ? (uint64_t)(sz / desc_size) : 0;
      bi->mmap_entry_size = (uint64_t)desc_size;
      bi->mmap_version = desc_ver;
      return EFI_SUCCESS;
    }
    /* stale key (something changed the map) — retry with fresh key */
  }
  return 304;
}
