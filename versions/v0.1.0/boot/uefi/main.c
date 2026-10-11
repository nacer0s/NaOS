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
