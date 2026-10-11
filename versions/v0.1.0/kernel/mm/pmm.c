/* NaOS PMM: collects EfiConventionalMemory pages (>=1M, outside the kernel
 * image) into a LIFO stack. No locking yet (single task, v0.1). */
#include "pmm.h"

#define PAGE 0x1000ULL
#define PMM_STACK_MAX 131072u
#define EFI_CONVENTIONAL 7u
#define KERN_BASE 0x100000ULL

/* Minimal view of EFI_MEMORY_DESCRIPTOR (must match boot/uefi/uefi.h). */
typedef struct {
  uint32_t type;
  uint32_t pad;
  uint64_t phys;
  uint64_t virt;
  uint64_t pages;
  uint64_t attr;
} memdesc_t;

static uint64_t g_stack[PMM_STACK_MAX];
static uint64_t g_count;
static uint64_t g_total;

void pmm_init(uint64_t mmap_addr, uint64_t mmap_count, uint64_t mmap_entry_size,
              uint64_t kern_end) {
  g_count = 0;
  g_total = 0;
  if (mmap_addr == 0 || mmap_entry_size < sizeof(memdesc_t)) return;
  uint64_t kend = (kern_end + PAGE - 1) & ~(PAGE - 1);
  if (kend < KERN_BASE + PAGE) kend = KERN_BASE + PAGE;

  for (uint64_t i = 0; i < mmap_count; i++) {
    const memdesc_t *d = (const memdesc_t *)(uintptr_t)
      (mmap_addr + i * mmap_entry_size);
    if (d->type != EFI_CONVENTIONAL || d->pages == 0) continue;
    for (uint64_t p = 0; p < d->pages; p++) {
      uint64_t addr = d->phys + p * PAGE;
      if (addr < KERN_BASE) continue;
      if (addr < kend) continue;
      if (g_count >= PMM_STACK_MAX) return; /* QEMU 256M fits; rest ignored */
      g_stack[g_count++] = addr;
      g_total++;
    }
  }
}

uint64_t pmm_alloc_page(void) {
  if (g_count == 0) return 0;
  return g_stack[--g_count];
}

/* Physically contiguous run of n pages. Scans for an ascending run and
 * removes it (order of the rest is preserved). Spawn-time only: O(n). */
uint64_t pmm_alloc_contig(uint32_t n) {
  if (n == 0 || n > 1024 || (uint64_t)n > g_count) return 0;
  for (uint64_t i = 0; i + n <= g_count; i++) {
    uint64_t base = g_stack[i];
    uint32_t j = 1;
    while (j < n && g_stack[i + j] == base + (uint64_t)j * PAGE) j++;
    if (j == n) {
      for (uint64_t k = i; k + n < g_count; k++)
        g_stack[k] = g_stack[k + n];
      g_count -= n;
      return base;
    }
  }
  return 0;
}

void pmm_free_page(uint64_t addr) {
  if (addr == 0 || (addr & (PAGE - 1)) != 0) return;
  if (g_count >= PMM_STACK_MAX) return;
  g_stack[g_count++] = addr;
}

uint64_t pmm_total_pages(void) { return g_total; }
uint64_t pmm_free_pages(void) { return g_count; }
