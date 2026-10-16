/* NaOS NVMe driver: polled admin + one I/O queue pair, NSID 1.
 * PCI 0x010802, 64-bit BAR0, single outstanding command per queue.
 * 512B and 4K namespaces via 512B-sector blk512() translation. */
#ifndef NAOS_NVME_H
#define NAOS_NVME_H

#include <stdint.h>

int nvme_init(void); /* 1 if a usable 512B/4K namespace 1 exists */
uint64_t nvme_sectors(void);
int nvme_read(uint32_t lba, uint8_t count, void *buf);
int nvme_write(uint32_t lba, uint8_t count, const void *buf);
void nvme_info(void);    /* `nvme` command: controller + namespace */
int nvme_selftest(void); /* save/pattern/verify/restore last LBA. 0 ok. */

#endif /* NAOS_NVME_H */
