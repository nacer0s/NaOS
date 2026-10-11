/* NaOS bootloader: GOP query + ExitBootServices with memory-map copy. */
#ifndef NAOS_BOOTUTIL_H
#define NAOS_BOOTUTIL_H

#include "uefi.h"
#include "../../common/bootinfo.h"

EFI_STATUS boot_get_gop(EFI_BOOT_SERVICES *bs, naos_bootinfo_t *bi);
EFI_STATUS boot_exit_services(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                              naos_bootinfo_t *bi);

#endif /* NAOS_BOOTUTIL_H */
