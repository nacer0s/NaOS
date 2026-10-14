/* NaOS CoW refcounts: tracks physically-shared user pages for fork().
 * A page with no entry is solely owned (release frees it directly). */
#ifndef NAOS_COW_H
#define NAOS_COW_H

#include <stdint.h>

/* Page gains a second owner (fork). 0 ok, -1 when the table is full. */
int cow_share(uint64_t phys);
/* Drop an owner; frees the page when the last one goes. */
void cow_release(uint64_t phys);
/* 1 while more than one owner holds the page. */
int cow_shared(uint64_t phys);

#endif /* NAOS_COW_H */
