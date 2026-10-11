/* NaOS CoW refcounts. Fixed table (no allocator: it backs the
 * allocator's users). Single-threaded by the sched_lock callers hold. */
#include "cow.h"
#include "pmm.h"

#define COW_MAX 2048

typedef struct {
  uint64_t phys;
  uint32_t count;
} cow_ent_t;

static cow_ent_t g_tab[COW_MAX];
static uint32_t g_used;

static int find(uint64_t phys) {
  for (uint32_t i = 0; i < g_used; i++)
    if (g_tab[i].phys == phys) return (int)i;
  return -1;
}

int cow_share(uint64_t phys) {
  int i;
  if (phys == 0 || (phys & 0xFFFULL)) return -1;
  i = find(phys);
  if (i >= 0) {
    g_tab[i].count++;
    return 0;
  }
  if (g_used >= COW_MAX) return -1;
  g_tab[g_used].phys = phys;
  g_tab[g_used].count = 2; /* implicit sole owner + the new one */
  g_used++;
  return 0;
}

void cow_release(uint64_t phys) {
  int i;
  if (phys == 0 || (phys & 0xFFFULL)) return;
  i = find(phys);
  if (i < 0) {
    pmm_free_page(phys); /* never shared: sole owner lets go */
    return;
  }
  if (g_tab[i].count > 1) {
    g_tab[i].count--;
    return;
  }
  /* Last owner: drop the entry, free the page (swap-remove). */
  g_used--;
  g_tab[i] = g_tab[g_used];
  pmm_free_page(phys);
}

int cow_shared(uint64_t phys) {
  int i = find(phys);
  return (i >= 0 && g_tab[i].count > 1);
}
