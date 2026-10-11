/* NaOS VMM: own PML4 with 2MB pages. Identity-maps low RAM, maps the
 * framebuffer. Switched on while already in long mode (UEFI tables replaced). */
#ifndef NAOS_VMM_H
#define NAOS_VMM_H

#include <stdint.h>

#define USTACK_VA 0x40000000ULL /* fixed user-stack base in task spaces */

/* PTE software bits (4KB entries): copy-on-write + was-writable. */
#define PTE_COW  0x200ULL
#define PTE_WASW 0x400ULL

void vmm_init(uint64_t fb_base, uint64_t fb_size);
uint64_t vmm_cr3(void);
/* Isolated task space: kernel supervisor-only except .utext, user stack
 * mapped at USTACK_VA. Returns 1 ok; tbl gets PML4/PDPT/PD/PT phys addrs. */
int vmm_create_space(uint64_t ustack_phys, uint32_t ustack_pages,
                     uint64_t *cr3_out, uint64_t tbl_out[4]);
/* Map one 4KB user page (va must be in the program window). */
int vmm_map_user(uint64_t pt_phys, uint64_t va, uint64_t phys, int writable);
/* Map one 2MB MMIO window (phys 2M-aligned, <4G) uncacheable, kernel-only. */
int vmm_map_mmio(uint64_t phys_2m);
/* Fresh tables for a fork child (same shape as create_space, no user
 * mappings yet). Returns 1 ok; tbl gets PML4/PDPT/PD/PT phys addrs. */
int vmm_fork_tables(uint64_t *cr3_out, uint64_t tbl_out[4]);
/* Free program pages mapped in entries [16..511), clearing them. */
void vmm_free_prog(uint64_t pt_phys);

#endif /* NAOS_VMM_H */
