/* NaOS VMM. Boot space (2MB pages, P+RW+US):
 *   PML4[0] -> PDPT_LO: PDPT_LO[0] -> PD_LO maps 0..1G identity
 *   Framebuffer + PCI MMIO: identity-mapped at any canonical address via
 *   per-(PML4,PDPT-index) page directories (uncacheable, supervisor-only).
 * Task spaces isolate: kernel supervisor-only except the .utext program
 * region; each task's user stack is mapped at USTACK_VA. Task spaces mirror
 * the kernel's MMIO mappings (shared pages) so syscalls can reach the
 * framebuffer and devices; all driver mappings happen at boot before any
 * task spawns, so the snapshot at task-create time is complete. */
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "../../common/cstring.h"
#include "../../common/serial.h"

#define E_PRESENT 0x001ULL
#define E_RW      0x002ULL
#define E_US      0x004ULL   /* user/supervisor: user may access */
#define E_PWT     0x008ULL   /* write-through (with PCD: uncacheable) */
#define E_PCD     0x010ULL   /* cache-disable (with PWT: uncacheable) */
#define E_PS      0x080ULL   /* 2MB page (PD entry) */
#define P2M       0x200000ULL

extern char utext_start[], utext_end[];

int vmm_map_mmio(uint64_t phys_2m); /* defined below; used by vmm_init */

static uint64_t *g_pml4;
static uint64_t g_cr3;
static uint64_t *g_pdpt;
static uint64_t *g_pd_lo;
static uint64_t *g_pd_lo_user; /* shared stripped copy for task spaces */

static uint64_t *new_table(void) {
  uint64_t pg = pmm_alloc_page();
  if (pg == 0) return 0;
  uint64_t *t = (uint64_t *)(uintptr_t)pg;
  naos_memset(t, 0, 0x1000);
  return t;
}

void vmm_init(uint64_t fb_base, uint64_t fb_size) {
  serial_puts("[vmm] building tables\n");
  uint64_t *pdpt = new_table();
  uint64_t *pd_lo = new_table();
  g_pml4 = new_table();
  if (!pdpt || !pd_lo || !g_pml4) {
    serial_puts("[vmm] OUT OF MEMORY\n");
    for (;;) { __asm__ volatile ("hlt"); }
  }

  /* Low RAM 0..1G identity, 2MB pages. */
  for (uint64_t i = 0; i < 512; i++)
    pd_lo[i] = (i * P2M) | E_PRESENT | E_RW | E_US | E_PS;
  pdpt[0] = ((uint64_t)(uintptr_t)pd_lo) | E_PRESENT | E_RW | E_US;

  g_pdpt = pdpt;
  g_pd_lo = pd_lo;
  g_cr3 = (uint64_t)(uintptr_t)g_pml4;

  /* Framebuffer at identity (any address): one uncacheable 2MB page per
   * covered page. Pages below 1G are already identity-mapped (firmware
   * never places a real framebuffer there). */
  if (fb_base != 0 && fb_size != 0) {
    uint64_t start = fb_base & ~(P2M - 1);
    uint64_t end = (fb_base + fb_size + P2M - 1) & ~(P2M - 1);
    for (uint64_t a = start; a < end; a += P2M) {
      if (a < 0x40000000ULL) continue;
      if (vmm_map_mmio(a) != 0) {
        serial_puts("[vmm] FB MAP FAILED\n");
        for (;;) { __asm__ volatile ("hlt"); }
      }
    }
  }
  g_pml4[0] = ((uint64_t)(uintptr_t)pdpt) | E_PRESENT | E_RW | E_US;
  serial_puts("[vmm] switching CR3\n");
  __asm__ volatile ("mov %0, %%cr3" :: "r"(g_cr3) : "memory");
  serial_puts("[vmm] paging on own tables\n");
}

uint64_t vmm_cr3(void) { return g_cr3; }

/* One-time: stripped PD_LO copy (supervisor except .utext). Flushes the
 * TLB since live entries change flags. */
static int build_user_pd_lo(void) {
  uint64_t *cp = new_table();
  if (!cp) return 0;
  for (uint64_t i = 0; i < 512; i++) cp[i] = g_pd_lo[i] & ~E_US;
  uint64_t s = (uint64_t)(uintptr_t)utext_start;
  uint64_t e = (uint64_t)(uintptr_t)utext_end;
  if (e > s && s >= 0x100000ULL && e < 0x40000000ULL) {
    for (uint64_t a = s & ~(P2M - 1); a < e; a += P2M)
      cp[(a >> 21) & 0x1FFULL] |= E_US;
  }
  g_pd_lo_user = cp;
  /* Flush stale TLB entries (flags changed under a live CR3). */
  __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3"
                    ::: "rax", "memory");
  return 1;
}

/* Map one 2MB MMIO window (phys 2M-aligned) uncacheable into the kernel
 * (boot) address space at identity. Kernel-only: task spaces stay clean
 * unless mirrored by vmm_share_mmio below. Works at any canonical
 * address: below 4G the window hangs off PDPT_LO, above 4G new PDPTs/PDs
 * are grown under the right PML4 entry (PCI 64-bit BARs live up there). */
static uint64_t *g_mmio_pdpt[512]; /* per-PML4-entry PDPTs, lazy */
typedef struct {
  uint64_t pml4i, pti;
  uint64_t *pd;
} mmio_pd_t;
static mmio_pd_t g_mmio_pds[16];
static int g_mmio_pds_used;

int vmm_map_mmio(uint64_t phys_2m) {
  uint64_t pml4i, pti, pdi, *pdpt, *pd = 0;
  int i;
  if ((phys_2m & 0x1FFFFFULL) || phys_2m >= 0x800000000000ULL) return -1;
  pml4i = (phys_2m >> 39) & 0x1FFULL;
  pti = (phys_2m >> 30) & 0x1FFULL;
  pdi = (phys_2m >> 21) & 0x1FFULL;
  if (pml4i == 0 && pti == 0) {
    pd = g_pd_lo; /* 0-1G identity area */
  } else {
    if (pml4i == 0) {
      pdpt = g_pdpt;
    } else {
      if (!g_mmio_pdpt[pml4i]) {
        uint64_t p = pmm_alloc_page();
        if (!p) return -1;
        g_mmio_pdpt[pml4i] = (uint64_t *)(uintptr_t)p;
        naos_memset(g_mmio_pdpt[pml4i], 0, 0x1000);
        g_pml4[pml4i] = p | E_PRESENT | E_RW; /* supervisor only */
      }
      pdpt = g_mmio_pdpt[pml4i];
    }
    for (i = 0; i < g_mmio_pds_used; i++) {
      if (g_mmio_pds[i].pml4i == pml4i && g_mmio_pds[i].pti == pti) break;
    }
    if (i == g_mmio_pds_used) {
      uint64_t p;
      if (g_mmio_pds_used >= 16) return -1;
      p = pmm_alloc_page();
      if (!p) return -1;
      pd = (uint64_t *)(uintptr_t)p;
      naos_memset(pd, 0, 0x1000);
      g_mmio_pds[g_mmio_pds_used].pml4i = pml4i;
      g_mmio_pds[g_mmio_pds_used].pti = pti;
      g_mmio_pds[g_mmio_pds_used].pd = pd;
      g_mmio_pds_used++;
      pdpt[pti] = p | E_PRESENT | E_RW; /* supervisor only */
    } else {
      pd = g_mmio_pds[i].pd;
    }
  }
  pd[pdi] = phys_2m | E_PRESENT | E_RW | E_PWT | E_PCD | E_PS;
  __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3"
                    ::: "rax", "memory");
  return 0;
}

/* Mirror every kernel MMIO mapping (framebuffer included) into a task
 * space, supervisor-only, sharing the kernel's PDPT/PD pages (never freed
 * with the task). All driver mappings happen at boot before any task
 * spawns, so a snapshot at task-create time is complete. PML4[0] slots 0
 * (user RAM) and 1 (ustacks) belong to the task: never overwrite them. */
static void vmm_share_mmio(uint64_t *task_pml4, uint64_t *task_pdpt0) {
  for (int i = 0; i < g_mmio_pds_used; i++) {
    uint64_t pml4i = g_mmio_pds[i].pml4i;
    uint64_t pti = g_mmio_pds[i].pti;
    if (pml4i == 0) {
      if (pti == 0 || pti == 1) continue;
      task_pdpt0[pti] =
          ((uint64_t)(uintptr_t)g_mmio_pds[i].pd) | E_PRESENT | E_RW;
    } else {
      task_pml4[pml4i] = g_pml4[pml4i]; /* shared PDPT, flags kept */
    }
  }
}

int vmm_create_space(uint64_t ustack_phys, uint32_t ustack_pages,
                     uint64_t *cr3_out, uint64_t tbl_out[4]) {
  if (!g_pd_lo_user && !build_user_pd_lo()) return 0;
  if (ustack_pages == 0 || ustack_pages > 512) return 0;
  /* 4-level walk for 4KB user pages: PML4 -> PDPT -> PD -> PT. (A PD entry
   * cannot map a 4KB page itself: PS=0 means "points to a PT".) */
  uint64_t *pml4 = new_table();
  uint64_t *pdpt = new_table();
  uint64_t *pd = new_table();
  uint64_t *pt = new_table();
  if (!pml4 || !pdpt || !pd || !pt) return 0;
  pdpt[0] = ((uint64_t)(uintptr_t)g_pd_lo_user) | E_PRESENT | E_RW | E_US;
  pd[0] = ((uint64_t)(uintptr_t)pt) | E_PRESENT | E_RW | E_US;
  for (uint32_t i = 0; i < ustack_pages; i++)
    pt[i] = (ustack_phys + (uint64_t)i * 0x1000ULL) |
            E_PRESENT | E_RW | E_US;
  pdpt[1] = ((uint64_t)(uintptr_t)pd) | E_PRESENT | E_RW | E_US;
  pml4[0] = ((uint64_t)(uintptr_t)pdpt) | E_PRESENT | E_RW | E_US;
  vmm_share_mmio(pml4, pdpt);
  *cr3_out = (uint64_t)(uintptr_t)pml4;
  tbl_out[0] = (uint64_t)(uintptr_t)pml4;
  tbl_out[1] = (uint64_t)(uintptr_t)pdpt;
  tbl_out[2] = (uint64_t)(uintptr_t)pd;
  tbl_out[3] = (uint64_t)(uintptr_t)pt;
  return 1;
}

/* Program window shares PD[0]'s 2MB with the stacks: programs live at
 * PT index >= 16 (0x40100000+), stacks at 0..3. */
int vmm_map_user(uint64_t pt_phys, uint64_t va, uint64_t phys, int writable) {
  if (va < 0x40100000ULL || va >= 0x40200000ULL) return -1;
  if (phys & 0xFFFULL) return -1;
  uint64_t idx = (va >> 12) & 0x1FFULL;
  if (idx < 16) return -1; /* stack area: hands off */
  uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;
  pt[idx] = phys | E_PRESENT | E_US | (writable ? E_RW : 0ULL);
  return 0;
}

void vmm_free_prog(uint64_t pt_phys) {
  uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;
  for (uint64_t i = 16; i < 512; i++) {
    if (pt[i] & E_PRESENT) {
      cow_release(pt[i] & ~0xFFFULL);
      pt[i] = 0;
    }
  }
}

/* Fork child tables: same shape as vmm_create_space, empty user PT. */
int vmm_fork_tables(uint64_t *cr3_out, uint64_t tbl_out[4]) {
  uint64_t *pml4, *pdpt, *pd, *pt;
  if (!g_pd_lo_user && !build_user_pd_lo()) return 0;
  pml4 = new_table();
  pdpt = new_table();
  pd = new_table();
  pt = new_table();
  if (!pml4 || !pdpt || !pd || !pt) {
    if (pml4) pmm_free_page((uint64_t)(uintptr_t)pml4);
    if (pdpt) pmm_free_page((uint64_t)(uintptr_t)pdpt);
    if (pd) pmm_free_page((uint64_t)(uintptr_t)pd);
    if (pt) pmm_free_page((uint64_t)(uintptr_t)pt);
    return 0;
  }
  pdpt[0] = ((uint64_t)(uintptr_t)g_pd_lo_user) | E_PRESENT | E_RW | E_US;
  pd[0] = ((uint64_t)(uintptr_t)pt) | E_PRESENT | E_RW | E_US;
  pdpt[1] = ((uint64_t)(uintptr_t)pd) | E_PRESENT | E_RW | E_US;
  pml4[0] = ((uint64_t)(uintptr_t)pdpt) | E_PRESENT | E_RW | E_US;
  vmm_share_mmio(pml4, pdpt);
  *cr3_out = (uint64_t)(uintptr_t)pml4;
  tbl_out[0] = (uint64_t)(uintptr_t)pml4;
  tbl_out[1] = (uint64_t)(uintptr_t)pdpt;
  tbl_out[2] = (uint64_t)(uintptr_t)pd;
  tbl_out[3] = (uint64_t)(uintptr_t)pt;
  return 1;
}
