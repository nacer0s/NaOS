/* NaOS UEFI bootloader — P2. 100% custom, no gnu-efi.
 * Flow: ConOut hello -> serial log -> GOP -> load \NaOS\KERNEL.ELF ->
 * ELF LOAD -> bootinfo -> ExitBootServices -> jump to kernel_entry(bootinfo).
 * Any failure: message on ConOut+serial, stall, halt (never return). */
#include "uefi.h"
#include "conout.h"
#include "file.h"
#include "elf.h"
#include "bootutil.h"
#include "../../common/bootinfo.h"
#include "../../common/cstring.h"
#include "../../common/serial.h"

static const CHAR16 kKernelPath[] =
  { '\\', 'N', 'a', 'O', 'S', '\\', 'K', 'E', 'R', 'N', 'E', 'L', '.', 'E', 'L', 'F', 0 };

/* Live-boot image: copy the WHOLE boot volume (our ESP) via Block I/O
 * into <=1G LoaderData page runs (kernel identity map), described in
 * bootinfo; the kernel mounts the copy as its last-resort disk.
 * Why blocks, not the RAMDISK.IMG file: some real firmware refuses to
 * open that file (NOT_FOUND/garbage status) while the small loader file
 * opens fine — filenames and file sizes are firmware quicksand. Block
 * reads are the most basic firmware disk op and carry no names at all.
 * Best effort: absence or failure just means a normal disk boot (or a
 * diskless shell). A staged copy that is not a FAT volume is discarded. */
#define RD_RUNS 4
#define RD_CHUNK (1024u * 1024u)
/* ESP image bytes, exactly: must match the `dd ... count=64` in mkesp.sh.
 * The drive may report more (whole stick) or unit-confused nonsense (some
 * CD emulation); we always read this many bytes from LBA 0 and let the
 * FAT validation below decide truth. */
#define RD_VOL_BYTES (64u * 1024u * 1024u)
static void load_ramdisk(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES *bs,
                         naos_bootinfo_t *bi) {
  EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
  EFI_BLOCK_IO_PROTOCOL *bio = 0;
  EFI_STATUS st;
  uint64_t bases[RD_RUNS];
  uint32_t pages[RD_RUNS];
  UINTN shares[RD_RUNS];
  int i, n, run;
  UINTN run_len, run_off, done, co, want, m;
  UINTN per, left, blksz;
  uint64_t total, lba;
  uint32_t media_id;
  uint8_t *chunk = 0;

  st = bs->HandleProtocol(ImageHandle, &kEfiLoadedImageProtocolGuid, (VOID **)&loaded);
  if (st != EFI_SUCCESS || !loaded || !loaded->DeviceHandle) {
    bi->rd_status = 0;
    co_puts("RAMDISK absent: disk boot\n");
    return;
  }
  st = bs->HandleProtocol(loaded->DeviceHandle, &kEfiBlockIoProtocolGuid,
                          (VOID **)&bio);
  if (st != EFI_SUCCESS || !bio || !bio->Media) {
    bi->rd_status = 0;
    bi->rd_estatus = (uint64_t)st;
    co_puts("RAMDISK no blockio: disk boot\n");
    return;
  }
  blksz = (UINTN)bio->Media->BlockSize;
  if (blksz == 0 || blksz > 4096u || (RD_CHUNK % blksz) != 0) {
    bi->rd_status = 7;
    bi->rd_size = (uint32_t)blksz;
    co_puts("RAMDISK bad block size: disk boot\n");
    return;
  }
  if (!bio->Media->MediaPresent) {
    bi->rd_status = 0;
    co_puts("RAMDISK absent: disk boot\n");
    return;
  }
  total = RD_VOL_BYTES;
  media_id = bio->Media->MediaId;
  /* Destination runs first (page allocs, never pool). */
  per = ((UINTN)total + RD_RUNS - 1) / RD_RUNS;
  per = (per + 0xFFFULL) & ~0xFFFULL;
  left = (UINTN)total;
  n = 0;
  for (i = 0; i < RD_RUNS && left > 0; i++) {
    EFI_PHYSICAL_ADDRESS addr = 0x40000000u; /* below 1G, kernel-mapped */
    want = left < per ? left : per;
    UINTN npages = (want + 0xFFFULL) >> 12;
    st = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, npages, &addr);
    if (st != EFI_SUCCESS) break;
    bases[n] = (uint64_t)addr;
    pages[n] = (uint32_t)npages;
    shares[n] = want;
    n++;
    left -= want;
  }
  if (left != 0) {
    bi->rd_status = 2;
    co_puts("RAMDISK alloc failed: disk boot\n");
    return; /* staged runs stay zeroed; leaked pages are LoaderData (safe) */
  }
  st = bs->AllocatePool(EfiLoaderData, RD_CHUNK, (VOID **)&chunk);
  if (st != EFI_SUCCESS || !chunk) {
    bi->rd_status = 3;
    co_puts("RAMDISK buffer failed: disk boot\n");
    return;
  }
  /* Copy the volume sequentially, one chunk at a time. */
  run = 0;
  run_len = (UINTN)pages[0] * 4096u;
  run_off = 0;
  done = 0;
  lba = 0;
  while (done < (UINTN)total) {
    UINTN bytes = (UINTN)total - done;
    if (bytes > RD_CHUNK) bytes = RD_CHUNK;
    st = bio->ReadBlocks(bio, media_id, lba, bytes, chunk);
    if (st != EFI_SUCCESS) break;
    co = 0;
    while (co < bytes) {
      UINTN room = run_len - run_off;
      m = (bytes - co < room) ? (bytes - co) : room;
      naos_memcpy((VOID *)(uintptr_t)(bases[run] + run_off), chunk + co, m);
      co += m;
      run_off += m;
      if (run_off >= run_len && done + co < (UINTN)total) {
        run++;
        run_off = 0;
        run_len = (UINTN)pages[run] * 4096u;
      }
    }
    done += bytes;
    lba += bytes / blksz;
  }
  bs->FreePool(chunk);
  if (done != (UINTN)total) {
    bi->rd_status = 4;
    bi->rd_done = (uint32_t)done;
    bi->rd_size = (uint32_t)total;
    co_puts("RAMDISK read failed at ");
    co_put_uint64((uint64_t)done);
    co_puts("/");
    co_put_uint64(total);
    co_puts(": disk boot\n");
    return;
  }
  /* The copy must be our FAT volume (El Torito image): boot signature +
   * 512B sectors. Anything else (e.g. an ISO9660 fallback boot) is not
   * mountable: discard like an absent image. */
  {
    uint8_t *b0 = (uint8_t *)(uintptr_t)bases[0];
    uint16_t bps = (uint16_t)b0[11] | ((uint16_t)b0[12] << 8);
    if (b0[510] != 0x55 || b0[511] != 0xAA || bps != 512) {
      bi->rd_status = 9;
      co_puts("RAMDISK not a FAT image: disk boot\n");
      return;
    }
  }
  for (i = 0; i < n; i++) { /* zero each run's rounding tail */
    UINTN cap = (UINTN)pages[i] * 4096u;
    if (cap > shares[i])
      naos_memset((VOID *)(uintptr_t)(bases[i] + shares[i]), 0,
                  cap - shares[i]);
    bi->rd_base[i] = bases[i];
    bi->rd_pages[i] = pages[i];
  }
  bi->rd_status = 1;
  bi->rd_done = (uint32_t)total;
  bi->rd_size = (uint32_t)total;
  co_puts("RAMDISK live image ready\n");
  serial_puts("ramdisk: ");
  serial_put_hex64(total);
  serial_puts(" bytes staged\n");
}

static void die(const char *msg, uint64_t code) {
  co_puts(msg);
  co_puts(" code=");
  co_put_hex64(code);
  co_puts("\nHALT\n");
  serial_puts(msg);
  serial_puts(" code=");
  serial_put_hex64(code);
  serial_puts("\nHALT\n");
  for (;;) { __asm__ volatile ("hlt"); }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS st;
  EFI_BOOT_SERVICES *bs;
  naos_bootinfo_t bootinfo;

  if (!SystemTable || !SystemTable->ConOut || !(bs = SystemTable->BootServices))
    return 1;

  naos_memset(&bootinfo, 0, sizeof(bootinfo));
  bootinfo.magic = NAOS_BOOTINFO_MAGIC;

  co_init(SystemTable->ConOut);
  serial_init();

  co_puts("NaOS bootloader P2\n");
  serial_puts("NaOS bootloader P2\n");

  /* ACPI RSDP from the UEFI configuration table (preferred over ROM scan). */
  {
    uint64_t rsdp = 0;
    EFI_CONFIGURATION_TABLE *ct = SystemTable->ConfigurationTable;
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries && ct; i++) {
      if (naos_memcmp(&ct[i].VendorGuid, &kEfiAcpi20TableGuid, 16) == 0 ||
          naos_memcmp(&ct[i].VendorGuid, &kEfiAcpiTableGuid, 16) == 0) {
        if (rsdp == 0 ||
            naos_memcmp(&ct[i].VendorGuid, &kEfiAcpi20TableGuid, 16) == 0)
          rsdp = (uint64_t)(uintptr_t)ct[i].VendorTable;
      }
    }
    bootinfo.rsdp = rsdp;
    co_puts("RSDP=");
    co_put_hex64(rsdp);
    co_puts("\n");
    serial_puts("RSDP=");
    serial_put_hex64(rsdp);
    serial_puts("\n");
  }

  /* GOP framebuffer */
  st = boot_get_gop(bs, &bootinfo);
  if (st != EFI_SUCCESS) die("! No GOP", (uint64_t)st);
  co_puts("GOP ");
  co_put_uint64(bootinfo.fb_width);
  co_puts("x");
  co_put_uint64(bootinfo.fb_height);
  co_puts(" base=");
  co_put_hex64(bootinfo.fb_base);
  co_puts("\n");
  serial_puts("GOP base=");
  serial_put_hex64(bootinfo.fb_base);
  serial_puts("\n");

  /* Live-boot RAM disk first: the optional image gets first dibs on the
   * volume (some firmware only honors early file opens); never fatal. */
  co_puts("Staging live image (whole disk)...\n");
  load_ramdisk(ImageHandle, bs, &bootinfo);

  /* Load kernel file */
  uint8_t *kimg = 0;
  UINTN ksize = 0;
  co_puts("Loading \\NaOS\\KERNEL.ELF...\n");
  st = file_read_all(ImageHandle, bs, kKernelPath, &kimg, &ksize);
  if (st != EFI_SUCCESS || !kimg) die("! KERNEL.ELF not found", (uint64_t)st);
  co_puts("ELF bytes=");
  co_put_uint64((uint64_t)ksize);
  co_puts("\n");

  /* Parse + copy LOAD segments */
  EFI_PHYSICAL_ADDRESS entry = 0;
  st = elf_load(bs, kimg, ksize, &entry);
  if (st != EFI_SUCCESS) die("! Bad ELF", (uint64_t)st);
  co_puts("Entry=");
  co_put_hex64((uint64_t)entry);
  co_puts("\n");
  serial_puts("kernel entry=");
  serial_put_hex64((uint64_t)entry);
  serial_puts("\n");

  /* Exit boot services */
  co_puts("ExitBootServices...\n");
  st = boot_exit_services(ImageHandle, bs, &bootinfo);
  if (st != EFI_SUCCESS) die("! ExitBootServices", (uint64_t)st);

  /* Jump. RDI = &bootinfo per SysV ABI. No returns. */
  serial_puts("jumping to kernel\n");
  {
    void (*kmain)(naos_bootinfo_t *) =
      (void (*)(naos_bootinfo_t *))(uintptr_t)entry;
    kmain(&bootinfo);
  }

  /* Unreachable — but if the kernel ever returns, halt loudly. */
  for (;;) { __asm__ volatile ("hlt"); }
}
