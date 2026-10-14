/* NaOS bootloader file loader: read \NaOS\KERNEL.ELF from our own ESP volume. */
#ifndef NAOS_FILE_H
#define NAOS_FILE_H

#include "uefi.h"

EFI_STATUS file_read_all(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         const CHAR16 *path, uint8_t **buf_out, UINTN *size_out);

/* Open `path` for sequential reads: returns the open file (caller Closes
 * it) plus the file size. For big files: Read in chunks instead of one
 * giant pool allocation (stingy firmware pools fail 40MB shots).
 * stage_out (may be 0): 1=loaded-image 2=fs 3=volume 4=open 5/6=info. */
EFI_STATUS file_open_stream(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                            const CHAR16 *path, EFI_FILE_PROTOCOL **file_out,
                            UINTN *size_out, int *stage_out);

#endif /* NAOS_FILE_H */
