/* NaOS heap: K&R-style free list over PMM pages. */
#include "kmalloc.h"
#include "pmm.h"
#include "../task.h"
#include "../../common/cstring.h"

typedef struct block {
  size_t size;          /* usable bytes after this header */
  struct block *next;
  int free;
} block_t;

#define ALIGN 16u
#define HDR_SZ ((sizeof(block_t) + (ALIGN - 1)) & ~(ALIGN - 1))
#define MIN_SPLIT (HDR_SZ + ALIGN)

static block_t *g_head;
static uint64_t g_used;

static size_t align_up(size_t n) {
  return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

/* Carve one PMM page into the list as a single free block. */
static void add_page(void) {
  uint64_t pg = pmm_alloc_page();
  if (pg == 0) return;
  block_t *b = (block_t *)(uintptr_t)pg;
  b->size = 0x1000ULL - HDR_SZ;
  b->next = 0;
  b->free = 1;
  /* Append to keep address order (helps coalescing). */
  if (!g_head || b < g_head) {
    b->next = g_head;
    g_head = b;
    return;
  }
  block_t *cur = g_head;
  while (cur->next && cur->next < b) cur = cur->next;
  b->next = cur->next;
  cur->next = b;
}

void kmalloc_init(void) {
  g_head = 0;
  g_used = 0;
}

static void coalesce(void) {
  block_t *cur = g_head;
  while (cur && cur->next) {
    uint8_t *end = (uint8_t *)cur + HDR_SZ + cur->size;
    if (cur->free && cur->next->free && end == (uint8_t *)cur->next) {
      cur->size += HDR_SZ + cur->next->size;
      cur->next = cur->next->next;
    } else {
      cur = cur->next;
    }
  }
}

void *kmalloc(size_t n) {
  if (n == 0) n = 1;
  n = align_up(n);
  if (n > 0x1000ULL - HDR_SZ) return 0; /* bigger than one page: refuse v0.1 */
  sched_lock();
  void *out = 0;
  for (;;) {
    for (block_t *cur = g_head; cur; cur = cur->next) {
      if (cur->free && cur->size >= n) {
        if (cur->size >= n + MIN_SPLIT) {
          block_t *split = (block_t *)((uint8_t *)cur + HDR_SZ + n);
          split->size = cur->size - n - HDR_SZ;
          split->next = cur->next;
          split->free = 1;
          cur->size = n;
          cur->next = split;
        }
        cur->free = 0;
        g_used += cur->size;
        naos_memset((uint8_t *)cur + HDR_SZ, 0, cur->size);
        out = (uint8_t *)cur + HDR_SZ;
        break;
      }
    }
    if (out) break;
    /* No fit: grow by one page (guaranteed >= n by the check above). */
    uint64_t fp = pmm_free_pages();
    add_page();
    if (pmm_free_pages() == fp) break; /* pool exhausted */
  }
  sched_unlock();
  return out;
}

void kfree(void *p) {
  if (!p) return;
  sched_lock();
  block_t *b = (block_t *)((uint8_t *)p - HDR_SZ);
  if (!b->free) {
    b->free = 1;
    g_used -= b->size;
    coalesce();
  }
  sched_unlock();
}

uint64_t kmalloc_used_bytes(void) { return g_used; }
