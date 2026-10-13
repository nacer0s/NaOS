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
static const CHAR16 kRamdiskPath[] =
  { '\\', 'N', 'a', 'O', 'S', '\\', 'R', 'A', 'M', 'D', 'I', 'S', 'K', '.', 'I', 'M', 'G', 0 };

/* Live-boot image: stream the file in 1MB pool chunks into <=1G
 * LoaderData page runs (kernel identity map), described in bootinfo.
 * One 40MB pool shot + one 40MB firmware Read fails on stingy real
 * firmware; small sequential reads work everywhere. Best effort: absence
 * or failure just means a normal disk boot (or diskless shell). */
#define RD_RUNS 4
#define RD_CHUNK (1024u * 1024u)
static void load_ramdisk(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES *bs,
                         naos_bootinfo_t *bi) {
  EFI_FILE_PROTOCOL *file = 0;
  UINTN size = 0, per, left;
  uint8_t *chunk = 0;
  EFI_STATUS st;
  uint64_t bases[RD_RUNS];
  uint32_t pages[RD_RUNS];
  UINTN shares[RD_RUNS];
  int i, n, run;
  UINTN run_len, run_off, done, co, got, want, m;

  st = file_open_stream(ImageHandle, bs, kRamdiskPath, &file, &size);
  if (st != EFI_SUCCESS || !file || size == 0 ||
      size > (64u * 1024u * 1024u) || (size & 511u)) {
    if (file) file->Close(file);
    co_puts("RAMDISK absent: disk boot\n");
    return;
  }
  /* Destination runs first (page allocs, never pool). */
  per = (size + RD_RUNS - 1) / RD_RUNS;
  per = (per + 0xFFFULL) & ~0xFFFULL;
  left = size;
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
    co_puts("RAMDISK alloc failed: disk boot\n");
    file->Close(file);
    return; /* staged runs stay zeroed; leaked pages are LoaderData (safe) */
  }
  st = bs->AllocatePool(EfiLoaderData, RD_CHUNK, (VOID **)&chunk);
  if (st != EFI_SUCCESS || !chunk) {
    co_puts("RAMDISK buffer failed: disk boot\n");
    file->Close(file);
    return;
  }
  /* Fill the runs sequentially, one firmware chunk at a time. */
  run = 0;
  run_len = (UINTN)pages[0] * 4096u;
  run_off = 0;
  done = 0;
  while (done < size) {
    want = size - done;
    if (want > RD_CHUNK) want = RD_CHUNK;
    got = want;
    st = file->Read(file, &got, chunk);
    if (st != EFI_SUCCESS || got != want) break;
    co = 0;
    while (co < got) {
      UINTN room = run_len - run_off;
      m = (got - co < room) ? (got - co) : room;
      naos_memcpy((VOID *)(uintptr_t)(bases[run] + run_off), chunk + co, m);
      co += m;
      run_off += m;
      if (run_off >= run_len && done + co < size) {
        run++;
        run_off = 0;
        run_len = (UINTN)pages[run] * 4096u;
      }
    }
    done += got;
  }
  bs->FreePool(chunk);
  file->Close(file);
  if (done != size) {
    co_puts("RAMDISK read failed: disk boot\n");
    return;
  }
  for (i = 0; i < n; i++) { /* zero each run's rounding tail */
    UINTN cap = (UINTN)pages[i] * 4096u;
    if (cap > shares[i])
      naos_memset((VOID *)(uintptr_t)(bases[i] + shares[i]), 0,
                  cap - shares[i]);
    bi->rd_base[i] = bases[i];
    bi->rd_pages[i] = pages[i];
  }
  co_puts("RAMDISK live image ready\n");
  serial_puts("ramdisk: ");
  serial_put_hex64(size);
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

  /* Live-boot RAM disk (optional file, staged below 1G for the kernel). */
  co_puts("Loading \\NaOS\\RAMDISK.IMG...\n");
  load_ramdisk(ImageHandle, bs, &bootinfo);

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
