/* NaOS bootloader ConOut helpers: ASCII -> UCS-2 printing + numbers. */
#ifndef NAOS_CONOUT_H
#define NAOS_CONOUT_H

#include "uefi.h"

void co_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con);
void co_puts(const char *ascii);
void co_put_hex64(uint64_t v);
void co_put_uint64(uint64_t v);

#endif /* NAOS_CONOUT_H */
