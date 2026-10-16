/* NaOS bignum interface (u64 LE limbs, generic Montgomery). Own code. */
#ifndef NAOS_TLS_BN_H
#define NAOS_TLS_BN_H

#include <stdint.h>

void bn_from_bytes(const uint8_t *in, int nbytes, uint64_t *out, int n);
void bn_to_bytes(const uint64_t *in, int n, uint8_t *out, int nbytes);
int bn_cmp(const uint64_t *a, const uint64_t *b, int n); /* -1/0/1 */
int bn_is_zero(const uint64_t *a, int n);
void bn_addmod(const uint64_t *a, const uint64_t *b, const uint64_t *m,
               int n, uint64_t *out);
void bn_dblmod(uint64_t *a, const uint64_t *m, int n); /* a = 2a mod m */
void bn_submod(const uint64_t *a, const uint64_t *b, const uint64_t *m,
               int n, uint64_t *out);
uint64_t mont_n0(uint64_t a0); /* -a0^-1 mod 2^64 */
void mont_mul(const uint64_t *a, const uint64_t *b, const uint64_t *m,
              uint64_t n0, int n, uint64_t *out, uint64_t *t);
void mont_pow(const uint64_t *a, const uint8_t *e, int elen,
              const uint64_t *m, uint64_t n0, int n, uint64_t *out,
              uint64_t *t); /* t: 2n+2 scratch limbs; a PLAIN */
void mont_pow_mont(const uint64_t *base, const uint8_t *e, int elen,
                   const uint64_t *m, uint64_t n0, int n, uint64_t *out,
                   uint64_t *t); /* base ALREADY Montgomery (a·R) */

#endif /* NAOS_TLS_BN_H */
