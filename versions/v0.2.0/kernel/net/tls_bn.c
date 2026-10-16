/* NaOS bignum: little-endian u64 limbs, generic Montgomery multiply
 * (CIOS), powmod, helpers. Serves P-256 (4 limbs) and RSA-2048 (32).
 * Portable (stdint only; __int128 for limb products). Own code. */
#include "tls_bn.h"

/* limbs <-> bytes (big-endian wire order: in[0] most significant). */
void bn_from_bytes(const uint8_t *in, int nbytes, uint64_t *out, int n) {
  int i, j;
  for (j = 0; j < n; j++) out[j] = 0;
  for (i = 0; i < nbytes; i++) {
    int k = nbytes - 1 - i; /* byte position from the low end */
    if (k / 8 < n) out[k / 8] |= (uint64_t)in[i] << ((k % 8) * 8);
  }
}

void bn_to_bytes(const uint64_t *in, int n, uint8_t *out, int nbytes) {
  int i;
  for (i = 0; i < nbytes; i++) {
    int k = nbytes - 1 - i;
    out[i] = (k / 8 < n) ? (uint8_t)(in[k / 8] >> ((k % 8) * 8)) : 0;
  }
}

int bn_cmp(const uint64_t *a, const uint64_t *b, int n) {
  int i;
  for (i = n - 1; i >= 0; i--) {
    if (a[i] > b[i]) return 1;
    if (a[i] < b[i]) return -1;
  }
  return 0;
}

int bn_is_zero(const uint64_t *a, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (a[i]) return 0;
  return 1;
}

/* out = (a + b) mod m. Returns carry-out (0/1) before reduction. */
static uint64_t bn_add_raw(const uint64_t *a, const uint64_t *b, int n,
                           uint64_t *out) {
  unsigned __int128 c = 0;
  int i;
  for (i = 0; i < n; i++) {
    c = (unsigned __int128)a[i] + b[i] + (uint64_t)c;
    out[i] = (uint64_t)c;
    c >>= 64;
  }
  return (uint64_t)c;
}

static uint64_t bn_sub_raw(const uint64_t *a, const uint64_t *b, int n,
                           uint64_t *out) {
  unsigned __int128 c = 0;
  int i;
  for (i = 0; i < n; i++) {
    /* bi = b[i] + borrow_in (up to 2^64, hence __int128). */
    unsigned __int128 bi = (unsigned __int128)b[i] + (uint64_t)c;
    out[i] = a[i] - (uint64_t)bi;
    c = (a[i] < (uint64_t)bi) || ((uint64_t)(bi >> 64) != 0);
  }
  return (uint64_t)c;
}

/* Double a (a = a*2 mod m), little-endian limbs: carry flows UP. */
void bn_dblmod(uint64_t *a, const uint64_t *m, int n) {
  uint64_t c = 0;
  int j;
  for (j = 0; j < n; j++) {
    uint64_t bit = (a[j] >> 63) & 1u;
    a[j] = (a[j] << 1) | c;
    c = bit;
  }
  if (c || bn_cmp(a, m, n) >= 0) {
    unsigned __int128 cc = 0;
    for (j = 0; j < n; j++) {
      unsigned __int128 bi = (unsigned __int128)m[j] + (uint64_t)cc;
      uint64_t r = a[j] - (uint64_t)bi;
      cc = (a[j] < (uint64_t)bi) || ((uint64_t)(bi >> 64) != 0);
      a[j] = r;
    }
  }
}

void bn_addmod(const uint64_t *a, const uint64_t *b, const uint64_t *m,
               int n, uint64_t *out) {
  uint64_t t[40];
  uint64_t carry = bn_add_raw(a, b, n, t);
  if (carry || bn_cmp(t, m, n) >= 0) bn_sub_raw(t, m, n, out);
  else
    for (int i = 0; i < n; i++) out[i] = t[i];
}

void bn_submod(const uint64_t *a, const uint64_t *b, const uint64_t *m,
               int n, uint64_t *out) {
  uint64_t t[40];
  uint64_t borrow = bn_sub_raw(a, b, n, t);
  if (borrow) bn_add_raw(t, m, n, out);
  else
    for (int i = 0; i < n; i++) out[i] = t[i];
}

/* -a[0]^-1 mod 2^64 (Newton: 6 iterations double the bits). */
uint64_t mont_n0(uint64_t a0) {
  uint64_t x = 1;
  /* x *= 2 - a0*x, mod 2^64 (wraps automatically). */
  x *= 2u - a0 * x;
  x *= 2u - a0 * x;
  x *= 2u - a0 * x;
  x *= 2u - a0 * x;
  x *= 2u - a0 * x;
  x *= 2u - a0 * x;
  return (uint64_t)(0u - x);
}

/* Montgomery multiply: out = a*b*R^-1 mod m (R = 2^(64n)).
 * t must hold 2n+2 limbs of scratch (caller: uint64_t t[70]). */
void mont_mul(const uint64_t *a, const uint64_t *b, const uint64_t *m,
              uint64_t n0, int n, uint64_t *out, uint64_t *t) {
  int i, j;
  for (i = 0; i < 2 * n + 2; i++) t[i] = 0;
  for (i = 0; i < n; i++) {
    unsigned __int128 c = 0;
    uint64_t mi;
    for (j = 0; j < n; j++) {
      c += (unsigned __int128)a[j] * b[i] + t[i + j];
      t[i + j] = (uint64_t)c;
      c >>= 64;
    }
    /* propagate the row carry. */
    {
      unsigned __int128 cc = c;
      int k = i + n;
      while (cc && k < 2 * n + 2) {
        cc += t[k];
        t[k] = (uint64_t)cc;
        cc >>= 64;
        k++;
      }
    }
    mi = t[i] * n0;
    c = 0;
    for (j = 0; j < n; j++) {
      c += (unsigned __int128)mi * m[j] + t[i + j];
      t[i + j] = (uint64_t)c;
      c >>= 64;
    }
    {
      unsigned __int128 cc = c;
      int k = i + n;
      while (cc && k < 2 * n + 2) {
        cc += t[k];
        t[k] = (uint64_t)cc;
        cc >>= 64;
        k++;
      }
    }
  }
  /* Result sits in t[n..2n] (low n limbs are zero by construction). */
  for (i = 0; i < n; i++) out[i] = t[n + i];
  if (t[2 * n] || bn_cmp(out, m, n) >= 0) {
    uint64_t tmp[40];
    bn_sub_raw(out, m, n, tmp);
    for (i = 0; i < n; i++) out[i] = tmp[i];
  }
}

/* out = a^e mod m (square-multiply, e big-endian bytes). Montgomery form
 * throughout (r = R·a^v invariant holds for zero bits too, so the loop
 * is uniform); scratch t (2n+2 limbs) reused. */
void mont_pow(const uint64_t *a, const uint8_t *e, int elen,
              const uint64_t *m, uint64_t n0, int n, uint64_t *out,
              uint64_t *t) {
  uint64_t base[40], r2[40], rmod[40];
  int i;
  for (i = 0; i < 40; i++) {
    base[i] = 0;
    r2[i] = 0;
    rmod[i] = 0;
  }
  /* R, then R^2 (R = 2^(64n)): 64n doublings each. */
  rmod[0] = 1;
  for (i = 0; i < 64 * n; i++) bn_dblmod(rmod, m, n);
  for (i = 0; i < 64 * n; i++) bn_dblmod(rmod, m, n);
  for (i = 0; i < n; i++) r2[i] = rmod[i];
  mont_mul(a, r2, m, n0, n, base, t); /* base = a·R mod m */
  mont_pow_mont(base, e, elen, m, n0, n, out, t);
}

/* Core: base is ALREADY Montgomery-form (base = a·R). Result plain. */
void mont_pow_mont(const uint64_t *base, const uint8_t *e, int elen,
                   const uint64_t *m, uint64_t n0, int n, uint64_t *out,
                   uint64_t *t) {
  uint64_t r[40], one[40], rmod[40];
  int i;
  for (i = 0; i < 40; i++) {
    r[i] = 0;
    one[i] = 0;
    rmod[i] = 0;
  }
  one[0] = 1;
  rmod[0] = 1;
  for (i = 0; i < 64 * n; i++) bn_dblmod(rmod, m, n); /* R */
  for (i = 0; i < n; i++) r[i] = rmod[i]; /* Montgomery 1 */
  for (i = 0; i < elen; i++) {
    uint8_t byte = e[i];
    for (int bit = 7; bit >= 0; bit--) {
      mont_mul(r, r, m, n0, n, r, t);
      if ((byte >> bit) & 1) mont_mul(r, base, m, n0, n, r, t);
    }
  }
  /* Back to plain: mont_mul(r, 1). */
  mont_mul(r, one, m, n0, n, out, t);
}
