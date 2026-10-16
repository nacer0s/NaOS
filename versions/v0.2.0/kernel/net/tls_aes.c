/* NaOS AES-128 + GCM (FIPS-197, SP800-38D). Portable C (stdint only):
 * builds into the kernel and into host unit tests. S-box/Rcon are spec
 * constants. GCM uses a 128-step GF(2^128) multiply (no carryless hw).
 * Own code from the specs. */
#include "tls_aes.h"

static const uint8_t sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t rcon[10] =
  { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static uint8_t xtime(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

/* Expand a 16-byte key into 11 round keys (176 bytes). */
void aes128_expand(const uint8_t key[16], uint8_t rk[176]) {
  int i;
  for (i = 0; i < 16; i++) rk[i] = key[i];
  for (i = 16; i < 176; i += 4) {
    uint8_t t[4];
    t[0] = rk[i - 4];
    t[1] = rk[i - 3];
    t[2] = rk[i - 2];
    t[3] = rk[i - 1];
    if ((i / 4) % 4 == 0) {
      uint8_t u = t[0];
      t[0] = sbox[t[1]] ^ rcon[i / 16 - 1];
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[u];
    }
    rk[i + 0] = rk[i - 16] ^ t[0];
    rk[i + 1] = rk[i - 15] ^ t[1];
    rk[i + 2] = rk[i - 14] ^ t[2];
    rk[i + 3] = rk[i - 13] ^ t[3];
  }
}

static void addkey(uint8_t s[16], const uint8_t *rk) {
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

/* Encrypt one 16-byte block with expanded keys. */
void aes128_block(const uint8_t rk[176], const uint8_t in[16],
                  uint8_t out[16]) {
  uint8_t s[16];
  int round, i;
  for (i = 0; i < 16; i++) s[i] = in[i];
  addkey(s, rk);
  for (round = 1; round <= 9; round++) {
    uint8_t t[16];
    for (i = 0; i < 16; i++) t[i] = sbox[s[i]]; /* SubBytes */
    /* ShiftRows + MixColumns fused per column. State is column-major:
     * output (column i, row r) comes from input (column (i+r)%4, row r). */
    for (i = 0; i < 4; i++) {
      uint8_t a = t[i * 4], b = t[((i + 1) & 3) * 4 + 1];
      uint8_t c = t[((i + 2) & 3) * 4 + 2], d = t[((i + 3) & 3) * 4 + 3];
      s[i * 4] = (uint8_t)(xtime(a) ^ (uint8_t)(xtime(b) ^ b) ^ c ^ d);
      s[i * 4 + 1] = (uint8_t)(a ^ xtime(b) ^ (uint8_t)(xtime(c) ^ c) ^ d);
      s[i * 4 + 2] = (uint8_t)(a ^ b ^ xtime(c) ^ (uint8_t)(xtime(d) ^ d));
      s[i * 4 + 3] = (uint8_t)((uint8_t)(xtime(a) ^ a) ^ b ^ c ^ xtime(d));
    }
    addkey(s, rk + round * 16);
  }
  for (i = 0; i < 16; i++) s[i] = sbox[s[i]];
  /* Final ShiftRows (column-major, same mapping). */
  {
    uint8_t t[16];
    for (i = 0; i < 16; i++) t[i] = s[i];
    for (i = 0; i < 4; i++) {
      s[i * 4] = t[i * 4];
      s[i * 4 + 1] = t[((i + 1) & 3) * 4 + 1];
      s[i * 4 + 2] = t[((i + 2) & 3) * 4 + 2];
      s[i * 4 + 3] = t[((i + 3) & 3) * 4 + 3];
    }
  }
  addkey(s, rk + 160);
  for (i = 0; i < 16; i++) out[i] = s[i];
}

/* ---- GCM ---- */

/* 128-bit field element: hi = x^127..x^64, lo = x^63..x^0.
 * Plain u64 pairs (no 129-bit ghosts possible). */
typedef struct {
  uint64_t hi, lo;
} gf128_t;

static gf128_t gf_add(gf128_t a, gf128_t b) {
  gf128_t r;
  r.hi = a.hi ^ b.hi;
  r.lo = a.lo ^ b.lo;
  return r;
}

/* Multiply in GF(2^128), R = x^128 + x^7 + x^2 + x + 1, GCM bit order:
 * string bit i (MSB-first) is x^i, i.e. int bit (127-k) <-> x^k.
 * Hence MSB-first accumulation with right shifts; underflow (x^-1)
 * reduces by R = 0xE1 in the top byte. */
static gf128_t gf_mul(gf128_t a, gf128_t b) {
  gf128_t z, v;
  int i;
  z.hi = 0;
  z.lo = 0;
  v = a;
  for (i = 0; i < 128; i++) {
    uint64_t bit = (i < 64) ? ((b.hi >> (63 - i)) & 1u) :
                              ((b.lo >> (127 - i)) & 1u);
    if (bit) {
      z.hi ^= v.hi;
      z.lo ^= v.lo;
    }
    {
      uint64_t carry = v.lo & 1u;
      v.lo = (v.lo >> 1) | (v.hi << 63);
      v.hi >>= 1;
      if (carry) v.hi ^= 0xE100000000000000ULL;
    }
  }
  return z;
}

static gf128_t gf_load(const uint8_t b[16]) {
  gf128_t r;
  int i;
  r.hi = 0;
  r.lo = 0;
  for (i = 0; i < 8; i++) {
    r.hi |= (uint64_t)b[i] << (56 - 8 * i);
    r.lo |= (uint64_t)b[8 + i] << (56 - 8 * i);
  }
  return r;
}

static void gf_store(gf128_t a, uint8_t b[16]) {
  int i;
  for (i = 0; i < 8; i++) {
    b[i] = (uint8_t)(a.hi >> (56 - 8 * i));
    b[8 + i] = (uint8_t)(a.lo >> (56 - 8 * i));
  }
}

/* GHASH(H, aad, ct) -> 16-byte tag mask input. */
static void ghash(const uint8_t h[16], const uint8_t *aad, uint32_t aad_len,
                  const uint8_t *ct, uint32_t ct_len, uint8_t out[16]) {
  gf128_t H = gf_load(h);
  gf128_t y;
  uint32_t i, n;
  y.hi = 0;
  y.lo = 0;
  n = (aad_len + 15) / 16;
  for (i = 0; i < n; i++) {
    uint8_t blk[16];
    uint32_t k, take = aad_len - i * 16;
    if (take > 16) take = 16;
    for (k = 0; k < 16; k++) blk[k] = (k < take) ? aad[i * 16 + k] : 0;
    y = gf_mul(gf_add(y, gf_load(blk)), H);
  }
  n = (ct_len + 15) / 16;
  for (i = 0; i < n; i++) {
    uint8_t blk[16];
    uint32_t k, take = ct_len - i * 16;
    if (take > 16) take = 16;
    for (k = 0; k < 16; k++) blk[k] = (k < take) ? ct[i * 16 + k] : 0;
    y = gf_mul(gf_add(y, gf_load(blk)), H);
  }
  {
    uint8_t blk[16];
    uint64_t al = (uint64_t)aad_len * 8u, cl = (uint64_t)ct_len * 8u;
    for (i = 0; i < 8; i++) {
      blk[i] = (uint8_t)(al >> (56 - 8 * i));
      blk[8 + i] = (uint8_t)(cl >> (56 - 8 * i));
    }
    y = gf_mul(gf_add(y, gf_load(blk)), H);
  }
  gf_store(y, out);
}

static void gctr(const uint8_t rk[176], const uint8_t iv[12],
                 const uint8_t *in, uint8_t *out, uint32_t len,
                 uint32_t icb0) {
  uint8_t ctr[16], pad[16];
  uint32_t i, off = 0, ctrv = icb0;
  for (i = 0; i < 12; i++) ctr[i] = iv[i];
  while (off < len) {
    uint32_t k, take = len - off;
    ctr[12] = (uint8_t)(ctrv >> 24);
    ctr[13] = (uint8_t)(ctrv >> 16);
    ctr[14] = (uint8_t)(ctrv >> 8);
    ctr[15] = (uint8_t)ctrv;
    ctrv++;
    aes128_block(rk, ctr, pad);
    if (take > 16) take = 16;
    for (k = 0; k < take; k++) out[off + k] = in[off + k] ^ pad[k];
    off += take;
  }
}

/* AES-128-GCM encrypt. iv[12], tag[16] out. ct may alias pt. */
void gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                 const uint8_t *aad, uint32_t aad_len,
                 const uint8_t *pt, uint8_t *ct, uint32_t len,
                 uint8_t tag[16]) {
  uint8_t rk[176], h[16], z[16], s[16], t[16];
  uint32_t i;
  aes128_expand(key, rk);
  for (i = 0; i < 16; i++) z[i] = 0;
  aes128_block(rk, z, h);
  gctr(rk, iv, pt, ct, len, 2);
  ghash(h, aad, aad_len, ct, len, t);
  gctr(rk, iv, z, s, 16, 1);
  for (i = 0; i < 16; i++) tag[i] = t[i] ^ s[i];
}

/* AES-128-GCM decrypt. Returns 0 ok, -1 auth failure (ct untouched
 * on failure only if out != ct... caller beware: decrypts then checks). */
int gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                const uint8_t *aad, uint32_t aad_len,
                const uint8_t *ct, uint8_t *pt, uint32_t len,
                const uint8_t tag[16]) {
  uint8_t rk[176], h[16], z[16], s[16], t[16], check[16];
  uint32_t i;
  int diff = 0;
  aes128_expand(key, rk);
  for (i = 0; i < 16; i++) z[i] = 0;
  aes128_block(rk, z, h);
  ghash(h, aad, aad_len, ct, len, t);
  gctr(rk, iv, z, s, 16, 1);
  for (i = 0; i < 16; i++) check[i] = t[i] ^ s[i];
  for (i = 0; i < 16; i++) diff |= (check[i] ^ tag[i]);
  if (diff) return -1;
  gctr(rk, iv, ct, pt, len, 2);
  return 0;
}

/* FIPS-197 Appendix B vector (also exercises the key schedule). */
int aes128_selftest(void) {
  static const uint8_t key[16] = {
    0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
    0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
  static const uint8_t pt[16] = {
    0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
    0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34 };
  static const uint8_t want[16] = {
    0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
    0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32 };
  uint8_t rk[176], out[16];
  aes128_expand(key, rk);
  aes128_block(rk, pt, out);
  for (int i = 0; i < 16; i++)
    if (out[i] != want[i]) return -1;
  return 0;
}

/* SP800-38D GCM test case 2 (all-zero key/iv, empty pt, 16-byte... ).
 * Uses the 16-byte-tag zero-length vector + a 64-byte round trip. */
int gcm_selftest(void) {
  static const uint8_t key[16] = { 0 };
  static const uint8_t iv[12] = { 0 };
  static const uint8_t want[16] = {
    0x58,0xe2,0xfc,0xce,0xfa,0x7e,0x30,0x61,
    0x36,0x7f,0x1d,0x57,0xa4,0xe7,0x45,0x5a };
  uint8_t tag[16], pt[64], ct[64], back[64];
  int i;
  if (aes128_selftest() != 0) return -1;
  gcm_encrypt(key, iv, 0, 0, 0, ct, 0, tag);
  for (i = 0; i < 16; i++)
    if (tag[i] != want[i]) return -1;
  /* NIST case 1 (zero key/iv, 16 zero bytes): exercises GHASH over a
   * real ciphertext block, not just the empty-input path. */
  {
    static const uint8_t c1[16] = {
      0x03,0x88,0xdA,0xce,0x60,0xb6,0xa3,0x92,
      0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78 };
    static const uint8_t t1[16] = {
      0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
      0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf };
    uint8_t z[16], c[16], t[16];
    for (i = 0; i < 16; i++) z[i] = 0;
    gcm_encrypt(key, iv, 0, 0, z, c, 16, t);
    for (i = 0; i < 16; i++)
      if (c[i] != c1[i] || t[i] != t1[i]) return -1;
    if (gcm_decrypt(key, iv, 0, 0, c, back, 16, t) != 0) return -1;
    for (i = 0; i < 16; i++)
      if (back[i] != 0) return -1;
  }
  for (i = 0; i < 64; i++) pt[i] = (uint8_t)(i * 7 + 3);
  gcm_encrypt(key, iv, pt, 13, pt, ct, 64, tag);
  if (gcm_decrypt(key, iv, pt, 13, ct, back, 64, tag) != 0) return -1;
  for (i = 0; i < 64; i++)
    if (back[i] != pt[i]) return -1;
  tag[0] ^= 1;
  if (gcm_decrypt(key, iv, pt, 13, ct, back, 64, tag) == 0) return -1;
  return 0;
}
