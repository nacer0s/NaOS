/* NaOS AES-128 + GCM interface. Portable (stdint only). Own code. */
#ifndef NAOS_TLS_AES_H
#define NAOS_TLS_AES_H

#include <stdint.h>

void aes128_expand(const uint8_t key[16], uint8_t rk[176]);
void aes128_block(const uint8_t rk[176], const uint8_t in[16],
                  uint8_t out[16]);
void gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                 const uint8_t *aad, uint32_t aad_len,
                 const uint8_t *pt, uint8_t *ct, uint32_t len,
                 uint8_t tag[16]);
int gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                const uint8_t *aad, uint32_t aad_len,
                const uint8_t *ct, uint8_t *pt, uint32_t len,
                const uint8_t tag[16]); /* 0 ok, -1 auth failure */
int aes128_selftest(void); /* FIPS-197 Appendix B. 0 ok. */
int gcm_selftest(void);    /* SP800-38D case 2 + round trip. 0 ok. */

#endif /* NAOS_TLS_AES_H */
