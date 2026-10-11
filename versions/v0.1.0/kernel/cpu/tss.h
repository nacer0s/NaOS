/* NaOS TSS: RSP0 for CPL3->0 transitions; I/O bitmap denies user port I/O. */
#ifndef NAOS_TSS_H
#define NAOS_TSS_H

#include <stdint.h>

void tss_init(uint64_t rsp0);
void tss_set_rsp0(uint64_t rsp0);

#endif /* NAOS_TSS_H */
