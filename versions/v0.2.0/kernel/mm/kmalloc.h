/* NaOS heap: first-fit free-list allocator, 16-aligned, split+coalesce.
 * Backed by PMM pages (identity-mapped, so phys == virt). */
#ifndef NAOS_KMALLOC_H
#define NAOS_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

void kmalloc_init(void);
void *kmalloc(size_t n);
void kfree(void *p);
uint64_t kmalloc_used_bytes(void);

#endif /* NAOS_KMALLOC_H */
