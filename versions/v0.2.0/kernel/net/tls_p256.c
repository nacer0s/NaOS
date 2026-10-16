/* NaOS P-256 (FIPS 186-4 §D.2.3): ECDHE keygen/shared + ECDSA verify.
 * Affine coordinates, Montgomery field arithmetic (bn_*), Fermat inverse.
 * Double-and-add scalar mult (NOT constant-time: documented, see header).
 * Portable (stdint only). Own code. */
#include "tls_p256.h"
#include "tls_bn.h"

#define NL 4 /* 4 x u64 */

static const uint8_t P256_P[32] = {
  0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF
};
static const uint8_t P256_N[32] = {
  0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,
  0xFC,0x63,0x25,0x51
};
static const uint8_t P256_GX[32] = {
  0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,
  0x40,0xF2,0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,
  0xD8,0x98,0xC2,0x96
};
static const uint8_t P256_GY[32] = {
  0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,
  0x9E,0x16,0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,
  0x37,0xBF,0x51,0xF5
};
/* a = -3 (for doubling/addition formulas). */

static uint64_t g_p[NL], g_n[NL];
static uint64_t g_n0p, g_n0n, g_r2p[NL];
static int g_init;

static void p256_init(void) {
  uint64_t rm[NL];
  int i;
  if (g_init) return;
  g_init = 1;
  bn_from_bytes(P256_P, 32, g_p, NL);
  bn_from_bytes(P256_N, 32, g_n, NL);
  g_n0p = mont_n0(g_p[0]);
  g_n0n = mont_n0(g_n[0]);
  /* R^2 mod p: 512 doublings of 1 (R = 2^256, so R^2 = 2^512). */
  for (i = 0; i < NL; i++) rm[i] = 0;
  rm[0] = 1;
  for (i = 0; i < 512; i++) bn_dblmod(rm, g_p, NL);
  for (i = 0; i < NL; i++) g_r2p[i] = rm[i];
}

/* Montgomery-domain field multiply. */
static void fmul(const uint64_t *a, const uint64_t *b, uint64_t *out) {
  uint64_t t[70];
  p256_init();
  mont_mul(a, b, g_p, g_n0p, NL, out, t);
}

static void fadd(const uint64_t *a, const uint64_t *b, uint64_t *out) {
  p256_init();
  bn_addmod(a, b, g_p, NL, out);
}

static void fsub(const uint64_t *a, const uint64_t *b, uint64_t *out) {
  p256_init();
  bn_submod(a, b, g_p, NL, out);
}

/* to/from Montgomery domain. */
static void to_mont(const uint64_t *a, uint64_t *out) {
  uint64_t t[70];
  p256_init();
  mont_mul(a, g_r2p, g_p, g_n0p, NL, out, t);
}

static void from_mont(const uint64_t *a, uint64_t *out) {
  uint64_t t[70], one[NL];
  int i;
  p256_init();
  for (i = 0; i < NL; i++) one[i] = 0;
  one[0] = 1;
  mont_mul(a, one, g_p, g_n0p, NL, out, t);
}

/* Inverse mod p (Fermat) of a Montgomery-domain value.
 * mont_pow_mont returns PLAIN, so convert back to Montgomery. */
static void finv(const uint64_t *a, uint64_t *out) {
  /* p - 2. */
  static const uint8_t pm2[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFD
  };
  uint64_t t[70], tmp[NL];
  p256_init();
  mont_pow_mont(a, pm2, 32, g_p, g_n0p, NL, tmp, t);
  to_mont(tmp, out);
}

typedef struct {
  uint64_t x[NL], y[NL];
  int inf;
} point_t; /* affine, Montgomery domain */

typedef struct {
  uint64_t x[NL], y[NL], z[NL];
  int inf;
} jpoint_t; /* Jacobian (X/Z^2, Y/Z^3), Montgomery domain */

static void small_mont(uint64_t v, uint64_t out[NL]) {
  uint64_t pl[NL];
  int i;
  for (i = 0; i < NL; i++) pl[i] = 0;
  pl[0] = v;
  to_mont(pl, out);
}

/* Jacobian doubling, a = -3 (dbl-1986-jm-1). */
static void jdouble(const jpoint_t *p, jpoint_t *r) {
  uint64_t xx[NL], yy[NL], yyyy[NL], zz[NL], s[NL], m[NL], t[NL];
  uint64_t xr[NL], yr[NL], zr[NL];
  uint64_t two[NL], three[NL], eight[NL];
  int i;
  if (p->inf) {
    r->inf = 1;
    return;
  }
  small_mont(2, two);
  small_mont(3, three);
  small_mont(8, eight);
  fmul(p->x, p->x, xx);
  fmul(p->y, p->y, yy);
  fmul(yy, yy, yyyy);
  fmul(p->z, p->z, zz);
  /* S = 2((X+YY)^2 - XX - YYYY). */
  fadd(p->x, yy, s);
  fmul(s, s, s);
  fsub(s, xx, s);
  fsub(s, yyyy, s);
  fmul(s, two, s);
  /* M = 3XX + aZZ^2 (a = -3). */
  fmul(xx, three, m);
  fmul(zz, zz, t);
  fmul(t, three, t);
  fsub(m, t, m);
  /* T = M^2 - 2S. */
  fmul(m, m, xr);
  fmul(s, two, t);
  fsub(xr, t, xr);
  /* Y' = M(S-X') - 8YYYY. */
  fsub(s, xr, yr);
  fmul(yr, m, yr);
  fmul(yyyy, eight, t);
  fsub(yr, t, yr);
  /* Z' = (Y+Z)^2 - YY - ZZ. */
  fadd(p->y, p->z, zr);
  fmul(zr, zr, zr);
  fsub(zr, yy, zr);
  fsub(zr, zz, zr);
  for (i = 0; i < NL; i++) {
    r->x[i] = xr[i];
    r->y[i] = yr[i];
    r->z[i] = zr[i];
  }
  r->inf = 0;
}

/* Jacobian addition (add-2007-bl). */
static void jadd(const jpoint_t *p, const jpoint_t *q, jpoint_t *r) {
  uint64_t z1z1[NL], z2z2[NL], u1[NL], u2[NL], s1[NL], s2[NL];
  uint64_t h[NL], i2[NL], j[NL], rr[NL], v[NL];
  uint64_t xr[NL], yr[NL], zr[NL], t[NL];
  uint64_t two[NL];
  int k;
  if (p->inf) {
    *r = *q;
    return;
  }
  if (q->inf) {
    *r = *p;
    return;
  }
  small_mont(2, two);
  fmul(p->z, p->z, z1z1);
  fmul(q->z, q->z, z2z2);
  fmul(p->x, z2z2, u1);
  fmul(q->x, z1z1, u2);
  fmul(q->z, z2z2, t);
  fmul(p->y, t, s1);               /* S1 = Y1·Z2^3 */
  fmul(p->z, z1z1, t);
  fmul(q->y, t, s2);               /* S2 = Y2·Z1^3 */
  if (bn_cmp(u1, u2, NL) == 0) {
    if (bn_cmp(s1, s2, NL) == 0) {
      jdouble(p, r);
      return;
    }
    r->inf = 1;
    return;
  }
  fsub(u2, u1, h);                 /* H */
  fmul(h, two, t);
  fmul(t, t, i2);                  /* I = (2H)^2 */
  fmul(h, i2, j);                  /* J */
  fsub(s2, s1, rr);
  fmul(rr, two, rr);               /* r = 2(S2-S1) */
  fmul(u1, i2, v);                 /* V */
  fmul(rr, rr, xr);
  fsub(xr, j, xr);
  fmul(v, two, t);
  fsub(xr, t, xr);                 /* X3 */
  fsub(v, xr, yr);
  fmul(yr, rr, yr);
  fmul(s1, j, t);
  fmul(t, two, t);
  fsub(yr, t, yr);                 /* Y3 */
  fadd(p->z, q->z, zr);
  fmul(zr, zr, zr);
  fsub(zr, z1z1, zr);
  fsub(zr, z2z2, zr);
  fmul(zr, h, zr);                 /* Z3 */
  for (k = 0; k < NL; k++) {
    r->x[k] = xr[k];
    r->y[k] = yr[k];
    r->z[k] = zr[k];
  }
  r->inf = 0;
}

/* R^2 mod n (R = 2^256), cached. Needed for Montgomery conversion. */
static void n_r2(uint64_t out[NL]) {
  static uint64_t cache[NL];
  static int have;
  uint64_t rm[NL];
  int i;
  if (have) {
    for (i = 0; i < NL; i++) out[i] = cache[i];
    return;
  }
  for (i = 0; i < NL; i++) rm[i] = 0;
  rm[0] = 1;
  for (i = 0; i < 512; i++) bn_dblmod(rm, g_n, NL); /* R^2 = 2^512 */
  for (i = 0; i < NL; i++) out[i] = rm[i];
  for (i = 0; i < NL; i++) cache[i] = out[i];
  have = 1;
}

/* Plain (a*b) mod n via a Montgomery round-trip. */
static void mulmod_n(const uint64_t *a, const uint64_t *b, uint64_t *out) {
  uint64_t r2[NL], am[NL], bm[NL], cm[NL], one[NL], t[70];
  int i;
  n_r2(r2);
  for (i = 0; i < NL; i++) one[i] = 0;
  one[0] = 1;
  mont_mul(a, r2, g_n, g_n0n, NL, am, t);
  mont_mul(b, r2, g_n, g_n0n, NL, bm, t);
  mont_mul(am, bm, g_n, g_n0n, NL, cm, t);
  mont_mul(cm, one, g_n, g_n0n, NL, out, t);
}

/* Scalar mult (double-and-add, MSB first) in Jacobian coordinates,
 * single inverse at the end. Secret-dependent timing: acceptable for
 * ECDHE here (documented); NOT for static keys. */
static void point_mul(const point_t *p, const uint8_t scalar[32],
                      point_t *r) {
  jpoint_t acc, base, dbl, sum;
  uint64_t one[NL], zi[NL], z2[NL], z3[NL];
  int i, started = 0, k;
  for (k = 0; k < NL; k++) one[k] = 0;
  one[0] = 1;
  to_mont(one, base.z);
  for (k = 0; k < NL; k++) {
    base.x[k] = p->x[k];
    base.y[k] = p->y[k];
  }
  base.inf = p->inf;
  acc.inf = 1;
  for (i = 0; i < 32; i++) {
    uint8_t byte = scalar[i];
    for (int bit = 7; bit >= 0; bit--) {
      if (started) {
        jdouble(&acc, &dbl);
        acc = dbl;
      }
      if ((byte >> bit) & 1) {
        if (!started) {
          acc = base;
          started = 1;
        } else {
          jadd(&acc, &base, &sum);
          acc = sum;
        }
      }
    }
  }
  if (!started || acc.inf) {
    r->inf = 1;
    return;
  }
  /* Back to affine: x = X/Z^2, y = Y/Z^3 (one inverse). */
  finv(acc.z, zi);
  fmul(zi, zi, z2);
  fmul(z2, zi, z3);
  fmul(acc.x, z2, r->x);
  fmul(acc.y, z3, r->y);
  r->inf = 0;
}

/* Load the generator (Montgomery domain). */
static void load_g(point_t *g) {
  uint64_t x[NL], y[NL];
  bn_from_bytes(P256_GX, 32, x, NL);
  bn_from_bytes(P256_GY, 32, y, NL);
  to_mont(x, g->x);
  to_mont(y, g->y);
  g->inf = 0;
}

/* Load a peer point from 64 uncompressed bytes (no prefix).
 * Validates on-curve + not infinity. 0 ok. */
static int load_point(const uint8_t xy[64], point_t *p) {
  uint64_t x[NL], y[NL], lhs[NL], rhs[NL], t[NL], b[NL], ax[NL];
  uint64_t three[NL], two[NL];
  int i;
  static const uint8_t P256_B[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,
    0x86,0xBC,0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,
    0x27,0xD2,0x60,0x4B
  };
  bn_from_bytes(xy, 32, x, NL);
  bn_from_bytes(xy + 32, 32, y, NL);
  if (bn_cmp(x, g_p, NL) >= 0 || bn_cmp(y, g_p, NL) >= 0) return -1;
  to_mont(x, p->x);
  to_mont(y, p->y);
  p->inf = 0;
  /* y^2 == x^3 - 3x + b (all Montgomery). */
  fmul(p->y, p->y, lhs);
  fmul(p->x, p->x, t);
  fmul(t, p->x, rhs);              /* x^3 */
  for (i = 0; i < NL; i++) {
    three[i] = 0;
    two[i] = 0;
  }
  {
    uint64_t pl[NL];
    for (i = 0; i < NL; i++) pl[i] = 0;
    pl[0] = 3;
    to_mont(pl, three);
  }
  fmul(p->x, three, ax);           /* 3x */
  fsub(rhs, ax, rhs);              /* x^3 - 3x */
  bn_from_bytes(P256_B, 32, b, NL);
  to_mont(b, t);
  fadd(rhs, t, rhs);               /* + b */
  if (bn_cmp(lhs, rhs, NL) != 0) return -1;
  /* Reject infinity encoding (0,0 never on curve anyway) + order
   * subgroup check skipped (cofactor 1: any curve point qualifies). */
  (void)two;
  return 0;
}

/* 1 if 1 <= priv < n (32 big-endian bytes). */
int p256_priv_ok(const uint8_t priv[32]) {
  uint64_t v[NL];
  int i, zero = 1;
  p256_init();
  bn_from_bytes(priv, 32, v, NL);
  for (i = 0; i < 32; i++)
    if (priv[i]) {
      zero = 0;
      break;
    }
  if (zero) return 0;
  return bn_cmp(v, g_n, NL) < 0;
}

/* Public key (64B x||y) from private. 0 ok. */
int p256_pub(const uint8_t priv[32], uint8_t pub[64]) {
  point_t g, r;
  uint64_t x[NL], y[NL];
  if (!p256_priv_ok(priv)) return -1;
  load_g(&g);
  point_mul(&g, priv, &r);
  if (r.inf) return -1;
  from_mont(r.x, x);
  from_mont(r.y, y);
  bn_to_bytes(x, NL, pub, 32);
  bn_to_bytes(y, NL, pub + 32, 32);
  return 0;
}

/* Shared secret x-coordinate (32B) from private + peer public. 0 ok. */
int p256_shared(const uint8_t priv[32], const uint8_t peer[64],
                uint8_t secret[32]) {
  point_t q, r;
  uint64_t x[NL];
  if (!p256_priv_ok(priv)) return -1;
  if (load_point(peer, &q) != 0) return -1;
  point_mul(&q, priv, &r);
  if (r.inf) return -1;
  from_mont(r.x, x);
  bn_to_bytes(x, NL, secret, 32);
  return 0;
}

/* ECDSA verify (P-256/SHA-256): pub 64B, hash 32B, r/s 32B each.
 * Returns 1 valid, 0 invalid, <0 malformed. */
int p256_ecdsa_verify(const uint8_t pub[64], const uint8_t hash[32],
                      const uint8_t rbin[32], const uint8_t sbin[32]) {
  point_t q, g, p1, p2;
  uint64_t r[NL], s[NL], e[NL], w[NL], u1[NL], u2[NL], v[NL];
  uint64_t t[70];
  uint8_t wbin[32];
  int i;
  p256_init();
  if (load_point(pub, &q) != 0) return -1;
  bn_from_bytes(rbin, 32, r, NL);
  bn_from_bytes(sbin, 32, s, NL);
  if (bn_is_zero(r, NL) || bn_is_zero(s, NL)) return 0;
  if (bn_cmp(r, g_n, NL) >= 0 || bn_cmp(s, g_n, NL) >= 0) return 0;
  bn_from_bytes(hash, 32, e, NL);
  /* w = s^-1 mod n (Fermat, Montgomery n-domain). */
  {
    static const uint8_t nm2[32] = {
      0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,
      0xFF,0xFF,0xFF,0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,
      0xCA,0xC2,0xFC,0x63,0x25,0x4F
    };
    uint64_t sm[NL], r2[NL];
    n_r2(r2);
    mont_mul(s, r2, g_n, g_n0n, NL, sm, t);  /* s·R */
    mont_pow_mont(sm, nm2, 32, g_n, g_n0n, NL, w, t); /* plain w */
  }
  bn_to_bytes(w, NL, wbin, 32);
  (void)wbin;
  /* u1 = e·w mod n, u2 = r·w mod n. */
  mulmod_n(e, w, u1);
  mulmod_n(r, w, u2);
  {
    uint8_t u1b[32], u2b[32];
    jpoint_t j1, j2, js;
    uint64_t one[NL];
    bn_to_bytes(u1, NL, u1b, 32);
    bn_to_bytes(u2, NL, u2b, 32);
    load_g(&g);
    point_mul(&g, u1b, &p1);
    point_mul(&q, u2b, &p2);
    /* Add in Jacobian (single conversion): Z = 1. */
    for (i = 0; i < NL; i++) one[i] = 0;
    one[0] = 1;
    to_mont(one, j1.z);
    to_mont(one, j2.z);
    for (i = 0; i < NL; i++) {
      j1.x[i] = p1.x[i];
      j1.y[i] = p1.y[i];
      j2.x[i] = p2.x[i];
      j2.y[i] = p2.y[i];
    }
    j1.inf = p1.inf;
    j2.inf = p2.inf;
    jadd(&j1, &j2, &js);
    if (js.inf) return 0;
    {
      uint64_t zi[NL], z2[NL];
      finv(js.z, zi);
      fmul(zi, zi, z2);
      fmul(js.x, z2, v); /* v = X/Z^2, still Montgomery */
    }
    from_mont(v, v);
    /* v mod n, compare with r. */
    if (bn_cmp(v, g_n, NL) >= 0) {
      uint64_t tmp[NL];
      unsigned __int128 cc = 0;
      for (i = 0; i < NL; i++) {
        unsigned __int128 bi = (unsigned __int128)g_n[i] + (uint64_t)cc;
        tmp[i] = v[i] - (uint64_t)bi;
        cc = (v[i] < (uint64_t)bi) || ((uint64_t)(bi >> 64) != 0);
      }
      for (i = 0; i < NL; i++) v[i] = tmp[i];
    }
    return bn_cmp(v, r, NL) == 0 ? 1 : 0;
  }
}
