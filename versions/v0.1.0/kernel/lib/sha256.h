/* NaOS SHA-256 (FIPS 180-4, streaming init/update/final). Used to verify
 * downloads. Public domain style; written from the spec. Own code. */
#ifndef NAOS_SHA256_H
#define NAOS_SHA256_H

#include <stdint.h>

typedef struct {
  uint32_t h[8];
  uint64_t len;   /* total message bytes */
  uint8_t buf[64];
  uint32_t buflen;
} sha256_t;

void sha256_init(sha256_t *c);
void sha256_update(sha256_t *c, const void *data, uint32_t len);
void sha256_final(sha256_t *c, uint8_t out[32]);
/* Built-in "abc" vector check. 0 ok. Safe to call anytime. */
int sha256_selftest(void);
/* Render 32 bytes as 64 lowercase hex chars (out needs 65B). */
void sha256_hex(const uint8_t in[32], char out[65]);

#endif /* NAOS_SHA256_H */
