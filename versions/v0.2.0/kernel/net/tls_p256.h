/* NaOS P-256 interface. Own code.
 * TIMING: scalar mult is double-and-add (secret-dependent). Fine for
 * ephemeral ECDHE; do NOT use with long-term keys. */
#ifndef NAOS_TLS_P256_H
#define NAOS_TLS_P256_H

#include <stdint.h>

int p256_priv_ok(const uint8_t priv[32]); /* 1 <= priv < n */
int p256_pub(const uint8_t priv[32], uint8_t pub[64]); /* x||y. 0 ok */
int p256_shared(const uint8_t priv[32], const uint8_t peer[64],
                uint8_t secret[32]); /* 0 ok */
int p256_ecdsa_verify(const uint8_t pub[64], const uint8_t hash[32],
                      const uint8_t rbin[32], const uint8_t sbin[32]);
/* 1 valid, 0 invalid, <0 malformed */

#endif /* NAOS_TLS_P256_H */
