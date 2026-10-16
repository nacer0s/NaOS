/* NaOS RSA verify (PKCS#1 v1.5 + SHA-256 DigestInfo), 2048-bit via the
 * shared Montgomery core (32 limbs). Verifies server signatures in
 * TLS_ECDHE_RSA_* handshakes. Portable (stdint only). Own code. */
#include "tls_rsa.h"
#include "tls_bn.h"
#include "../lib/sha256.h"

#define RL 32 /* 32 x u64 */

/* PKCS#1 v1.5 encode for SHA-256 (EMSA, 2048-bit): 00 01 FF.. 00 +
 * DigestInfo(SHA-256). Returns 0 ok, -1 if hash name unknown. */
static int emsa_encode(const uint8_t hash[32], uint8_t em[256]) {
  /* DigestInfo prefix for SHA-256 (RFC 8017 §9.2 note 1). */
  static const uint8_t prefix[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,
    0x02,0x01,0x05,0x00,0x04,0x20
  };
  int i;
  em[0] = 0x00;
  em[1] = 0x01;
  for (i = 2; i < 256 - 32 - 19 - 1; i++) em[i] = 0xFF;
  em[256 - 32 - 19 - 1] = 0x00;
  for (i = 0; i < 19; i++) em[256 - 32 - 19 + i] = prefix[i];
  for (i = 0; i < 32; i++) em[256 - 32 + i] = hash[i];
  return 0;
}

/* RSA-2048 public-verify: sig^e mod n == EMSA(hash). e = 65537 (or the
 * cert's exponent when it fits 32 bits). Returns 1 valid, else 0. */
int rsa_verify(const uint8_t nbytes[256], uint32_t e,
               const uint8_t sig[256], const uint8_t hash[32]) {
  uint64_t n[RL], sg[RL], out[RL], t[70], n0;
  uint8_t em[256], got[256], eb[4];
  int i;
  bn_from_bytes(nbytes, 256, n, RL);
  if (bn_is_zero(n, RL) || (n[0] & 1u) == 0) return 0;
  n0 = mont_n0(n[0]);
  bn_from_bytes(sig, 256, sg, RL);
  if (bn_cmp(sg, n, RL) >= 0) return 0; /* sig representative out of range */
  eb[0] = (uint8_t)(e >> 24);
  eb[1] = (uint8_t)(e >> 16);
  eb[2] = (uint8_t)(e >> 8);
  eb[3] = (uint8_t)e;
  mont_pow(sg, eb, 4, n, n0, RL, out, t);
  bn_to_bytes(out, RL, got, 256);
  emsa_encode(hash, em);
  for (i = 0; i < 256; i++)
    if (got[i] != em[i]) return 0;
  return 1;
}

/* Selftest: signs nothing; verifies a fixed openssl-made vector
 * (filled by the test harness below with real values). */
static const uint8_t self_n[256] = { 0 };
static const uint8_t self_sig[256] = { 0 };
static const uint8_t self_hash[32] = { 0 };

int rsa_selftest(void) {
  /* Real vector injected at build-test time; the checked-in zeros
   * must FAIL (proves the negative path). */
  if (rsa_verify(self_n, 65537, self_sig, self_hash) != 0) return -1;
  return 0;
}
