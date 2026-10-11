/* NaOS freestanding string/memory helpers. Own code, shared by bootloader+kernel. */
#ifndef NAOS_CSTRING_H
#define NAOS_CSTRING_H

#include <stddef.h>
#include <stdint.h>

void *naos_memcpy(void *dst, const void *src, size_t n);
void *naos_memset(void *s, int c, size_t n);
void *naos_memmove(void *dst, const void *src, size_t n);
int naos_memcmp(const void *a, const void *b, size_t n);
size_t naos_strlen(const char *s);
int naos_strcmp(const char *a, const char *b);

#endif /* NAOS_CSTRING_H */
