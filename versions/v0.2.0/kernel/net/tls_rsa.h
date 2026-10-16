/* NaOS RSA verify interface. Own code. */
#ifndef NAOS_TLS_RSA_H
#define NAOS_TLS_RSA_H

#include <stdint.h>

/* RSA-2048 public-verify: sig^e mod n == EMSA-SHA256(hash).
 * e = cert exponent (usually 65537). Returns 1 valid, else 0. */
int rsa_verify(const uint8_t nbytes[256], uint32_t e,
               const uint8_t sig[256], const uint8_t hash[32]);
int rsa_selftest(void); /* negative-path check. 0 ok. */

#endif /* NAOS_TLS_RSA_H */
