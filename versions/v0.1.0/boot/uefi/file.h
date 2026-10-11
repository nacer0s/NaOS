/* NaOS bootloader file loader: read \NaOS\KERNEL.ELF from our own ESP volume. */
#ifndef NAOS_FILE_H
#define NAOS_FILE_H

#include "uefi.h"

EFI_STATUS file_read_all(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         const CHAR16 *path, uint8_t **buf_out, UINTN *size_out);

#endif /* NAOS_FILE_H */
