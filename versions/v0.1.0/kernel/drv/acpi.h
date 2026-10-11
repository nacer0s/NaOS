/* NaOS ACPI: RSDP scan, FADT/DSDT walk, S5 sleep values for poweroff. */
#ifndef NAOS_ACPI_H
#define NAOS_ACPI_H

#include <stdint.h>

int acpi_init(uint64_t rsdp_hint); /* 1 if S5 values found, else 0 */
int acpi_has_poweroff(void);
void acpi_poweroff(void); /* returns only if it failed */
void cpu_reboot(void);    /* 8042 pulse; halts if it fails */

#endif /* NAOS_ACPI_H */
