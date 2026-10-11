/* NaOS ELF64 loader: parse KERNEL.ELF, AllocatePages at p_paddr, copy LOAD segs. */
#ifndef NAOS_ELF_H
#define NAOS_ELF_H

#include "uefi.h"

EFI_STATUS elf_load(EFI_BOOT_SERVICES *bs, const uint8_t *img, UINTN size,
                    EFI_PHYSICAL_ADDRESS *entry_out);

#endif /* NAOS_ELF_H */
