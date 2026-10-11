/* NaOS freestanding string/memory helpers. */
#include "cstring.h"

void *naos_memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void *naos_memset(void *s, int c, size_t n) {
  uint8_t *p = (uint8_t *)s;
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
  return s;
}

void *naos_memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
  else if (d > s) { for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
  return dst;
}

int naos_memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}

size_t naos_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int naos_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (unsigned char)*a - (unsigned char)*b;
}
