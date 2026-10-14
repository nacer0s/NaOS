/* NaOS physical memory manager: free-page stack from the UEFI memmap. */
#ifndef NAOS_PMM_H
#define NAOS_PMM_H

#include <stdint.h>

void pmm_init(uint64_t mmap_addr, uint64_t mmap_count, uint64_t mmap_entry_size,
              uint64_t kern_end);
uint64_t pmm_alloc_page(void);   /* 0 = out of memory */
uint64_t pmm_alloc_contig(uint32_t n); /* n physically contiguous pages, 0=fail */
void pmm_free_page(uint64_t addr);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);

#endif /* NAOS_PMM_H */
