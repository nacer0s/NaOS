/* NaOS TLS PRF/HMAC/DRBG interface. Portable (stdint only). */
#ifndef NAOS_TLS_PRF_H
#define NAOS_TLS_PRF_H

#include <stdint.h>

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len, uint8_t out[32]);
void tls_prf(const uint8_t *secret, uint32_t secret_len,
             const char *label,
             const uint8_t *seed, uint32_t seed_len,
             uint8_t *out, uint32_t out_len);
void tls_master(const uint8_t *premaster, uint32_t pm_len,
                const uint8_t cli_rand[32], const uint8_t srv_rand[32],
                uint8_t master[48]);
void tls_keyblock(const uint8_t master[48],
                  const uint8_t srv_rand[32], const uint8_t cli_rand[32],
                  uint8_t out[48]);
void tls_finished(const uint8_t master[48], int is_client,
                  const uint8_t *hs_hash, uint8_t out[12]);
void tls_rand_seed(const uint8_t *seed, uint32_t len);
void tls_rand(uint8_t *out, uint32_t len);
int hmac_selftest(void); /* RFC 4231 case 1. 0 ok. */
/* Handshake KDF chain on fixed vectors (independent Python reference):
 * master from PMS, key block, both Finished. 0 ok. */
int tls_hs_selftest(void);

#endif /* NAOS_TLS_PRF_H */
