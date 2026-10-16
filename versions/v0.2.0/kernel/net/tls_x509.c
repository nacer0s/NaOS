/* NaOS X.509 parser (just enough for TLS server chains): DER walk,
 * tbsCertificate bytes, subject/issuer, SPKI RSA key, SAN (dNS/iP),
 * validity, sha256RSA signature verify against a trust anchor.
 * Test CA embedded below (LOCAL TEST ONLY — see tls.c for rotation).
 * Portable (stdint only). Own code from RFC 5280 + X.690. */
#include "tls_x509.h"
#include "tls_rsa.h"
#include "tls_bn.h"
#include "../lib/sha256.h"

/* ---- DER reader ---- */

typedef struct {
  const uint8_t *p;
  uint32_t left;
} der_t;

static int der_peek(der_t *d, uint8_t *tag, uint32_t *len,
                    const uint8_t **val) {
  uint32_t n;
  if (d->left < 2) return -1;
  *tag = d->p[0];
  if ((d->p[1] & 0x80) == 0) {
    *len = d->p[1];
    n = 2;
  } else {
    uint32_t cnt = d->p[1] & 0x7F;
    uint32_t i;
    if (cnt == 0 || cnt > 4 || d->left < 2u + cnt) return -1;
    *len = 0;
    for (i = 0; i < cnt; i++) *len = (*len << 8) | d->p[2 + i];
    n = 2 + cnt;
  }
  if (d->left < n + *len) return -1;
  *val = d->p + n;
  return 0;
}

static int der_enter(der_t *d, uint8_t want, der_t *inner) {
  uint8_t tag;
  uint32_t len;
  const uint8_t *val;
  if (der_peek(d, &tag, &len, &val) != 0 || tag != want) return -1;
  inner->p = val;
  inner->left = len;
  return 0;
}

static void der_skip(der_t *d) {
  uint8_t tag;
  uint32_t len;
  const uint8_t *val;
  uint32_t n;
  if (der_peek(d, &tag, &len, &val) != 0) {
    d->left = 0;
    return;
  }
  n = (uint32_t)(val - d->p) + len;
  d->p += n;
  d->left -= n;
}

/* Expect an OID with the given body bytes. 0 match. */
static int der_oid_is(der_t *d, const uint8_t *body, uint32_t body_len) {
  uint8_t tag;
  uint32_t len;
  const uint8_t *val;
  uint32_t i;
  if (der_peek(d, &tag, &len, &val) != 0 || tag != 0x06) return -1;
  if (len != body_len) return -1;
  for (i = 0; i < len; i++)
    if (val[i] != body[i]) return -1;
  der_skip(d);
  return 0;
}

/* sha256WithRSAEncryption 1.2.840.113549.1.1.11 */
static const uint8_t OID_SHA256RSA[] =
  { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
/* rsaEncryption 1.2.840.113549.1.1.1 */
static const uint8_t OID_RSA[] =
  { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
/* subjectAltName 2.5.29.17 */
static const uint8_t OID_SAN[] = { 0x55, 0x1D, 0x11 };

/* Parse an AlgorithmIdentifier, requiring sha256RSA. 0 ok. */
static int parse_sigalg(der_t *d) {
  der_t in;
  if (der_enter(d, 0x30, &in) != 0) return -1;
  if (der_oid_is(&in, OID_SHA256RSA, sizeof(OID_SHA256RSA)) != 0) return -1;
  /* Optional NULL params (most CAs include it). */
  if (in.left >= 2 && in.p[0] == 0x05) der_skip(&in);
  der_skip(d);
  return 0;
}

/* Parse an RSA SPKI: SEQ { SEQ { OID rsa, NULL }, BIT STRING key }.
 * key = DER SEQ { INTEGER n, INTEGER e }. e must fit 32 bits. */
static int parse_spki(der_t *d, uint8_t n[256], uint32_t *e_out) {
  der_t in, alg, key, seq;
  uint8_t tag;
  uint32_t len, e = 0;
  const uint8_t *val;
  uint32_t i;
  if (der_enter(d, 0x30, &in) != 0) return -1;
  if (der_enter(&in, 0x30, &alg) != 0) return -1;
  if (der_oid_is(&alg, OID_RSA, sizeof(OID_RSA)) != 0) return -1;
  if (alg.left >= 2 && alg.p[0] == 0x05) der_skip(&alg);
  der_skip(&in); /* consume the algorithm SEQ itself */
  if (der_peek(&in, &tag, &len, &val) != 0 || tag != 0x03) return -1;
  if (len < 1 || val[0] != 0) return -1; /* unused bits must be 0 */
  key.p = val + 1;
  key.left = len - 1;
  if (der_enter(&key, 0x30, &seq) != 0) return -1;
  /* INTEGER n (may carry a leading zero). */
  if (der_peek(&seq, &tag, &len, &val) != 0 || tag != 0x02) return -1;
  {
    uint32_t off = 0;
    while (off + 1 < len && val[off] == 0) off++;
    if (len - off != 256) return -1; /* we only do RSA-2048 */
    for (i = 0; i < 256; i++) n[i] = val[off + i];
  }
  der_skip(&seq);
  /* INTEGER e. */
  if (der_peek(&seq, &tag, &len, &val) != 0 || tag != 0x02) return -1;
  {
    uint32_t off = 0;
    while (off + 1 < len && val[off] == 0) off++;
    if (len - off > 4) return -1;
    for (i = off; i < len; i++) e = (e << 8) | val[i];
  }
  if (e < 3 || (e & 1u) == 0) return -1;
  der_skip(&seq);
  der_skip(&in);
  der_skip(d);
  return (int)(*e_out = e, 0);
}

/* UTCTime/GeneralizedTime -> comparable yyyymmddhhmmss (as u64 digits).
 * Returns 0 + *out on success. */
static int parse_time(der_t *d, uint64_t *out) {
  uint8_t tag;
  uint32_t len, i;
  const uint8_t *val;
  uint64_t v = 0;
  int full_year;
  if (der_peek(d, &tag, &len, &val) != 0) return -1;
  if (tag == 0x17) { /* UTCTime YYMMDDHHMMSSZ */
    uint32_t yy;
    if (len < 13) return -1;
    for (i = 0; i < 12; i++)
      if (val[i] < '0' || val[i] > '9') return -1;
    yy = (uint32_t)((val[0] - '0') * 10 + (val[1] - '0'));
    full_year = (int)(yy >= 50 ? 1900 + yy : 2000 + yy);
    v = (uint64_t)full_year;
    for (i = 2; i < 12; i += 2)
      v = v * 100u + (uint64_t)((val[i] - '0') * 10 + (val[i + 1] - '0'));
  } else if (tag == 0x18) { /* GeneralizedTime YYYYMMDDHHMMSSZ */
    if (len < 15) return -1;
    v = 0;
    for (i = 0; i < 14; i += 2) {
      if (val[i] < '0' || val[i] > '9' || val[i + 1] < '0' ||
          val[i + 1] > '9')
        return -1;
      v = v * 100u + (uint64_t)((val[i] - '0') * 10 + (val[i + 1] - '0'));
    }
  } else {
    return -1;
  }
  der_skip(d);
  *out = v;
  return 0;
}

/* Collect SAN entries: first dNSName (up to 63B) and first iPAddress. */
typedef struct {
  char dns[64];
  uint8_t ip[4];
  int has_dns, has_ip;
} san_t;

static void san_collect(der_t *exts, san_t *san) {
  /* extensions: SEQ of SEQ { OID, [critical], OCTET STRING }. */
  while (exts->left > 0) {
    der_t e, oid_c;
    uint8_t tag;
    uint32_t len;
    const uint8_t *val;
    if (der_enter(exts, 0x30, &e) != 0) return;
    if (der_peek(&e, &tag, &len, &val) != 0 || tag != 0x06) {
      der_skip(exts);
      continue;
    }
    oid_c = e;
    if (der_oid_is(&oid_c, OID_SAN, sizeof(OID_SAN)) == 0) {
      /* Found SAN: consume the OID itself, then optional critical. */
      der_t oct, names;
      der_skip(&e);
      if (e.left >= 2 && e.p[0] == 0x01) der_skip(&e); /* BOOLEAN */
      if (der_enter(&e, 0x04, &oct) == 0 &&
          der_enter(&oct, 0x30, &names) == 0) {
        while (names.left > 0) {
          uint8_t nt;
          uint32_t nl;
          const uint8_t *nv;
          uint32_t k;
          if (der_peek(&names, &nt, &nl, &nv) != 0) break;
          if (nt == 0x82 && !san->has_dns && nl < sizeof(san->dns)) {
            /* dNSName (IA5String). */
            for (k = 0; k < nl; k++) san->dns[k] = (char)nv[k];
            san->dns[nl] = '\0';
            san->has_dns = 1;
          } else if (nt == 0x87 && !san->has_ip && nl == 4) {
            for (k = 0; k < 4; k++) san->ip[k] = nv[k];
            san->has_ip = 1;
          }
          der_skip(&names);
        }
      }
    }
    der_skip(exts);
  }
}

/* Parsed certificate pieces (views into the caller's buffer). */
typedef struct {
  const uint8_t *tbs;
  uint32_t tbs_len;      /* exact bytes hashed+signed */
  const uint8_t *subject;
  uint32_t subject_len;  /* DER Name, for issuer match */
  const uint8_t *issuer;
  uint32_t issuer_len;
  uint64_t not_before, not_after;
  uint8_t spki_n[256];
  uint32_t spki_e;
  const uint8_t *sig;
  uint32_t sig_len;      /* raw signature bytes (no BIT STRING head) */
  san_t san;
} cert_t;

/* Parse one DER certificate. 0 ok. */
static int cert_parse(const uint8_t *der, uint32_t len, cert_t *c) {
  der_t outer, tbs;
  uint8_t tag;
  uint32_t tlen;
  const uint8_t *tval;
  uint32_t i;
  for (i = 0; i < 256; i++) c->spki_n[i] = 0;
  c->spki_e = 0;
  c->san.has_dns = 0;
  c->san.has_ip = 0;
  c->sig = 0;
  c->sig_len = 0;
  outer.p = der;
  outer.left = len;
  /* Outer SEQ: [ tbsCertificate | signatureAlgorithm | signatureValue ].
   * The SIGNED bytes are the whole tbsCertificate TLV; the field walk
   * starts inside it (hence two levels of enter). */
  {
    der_t t, body;
    t.p = outer.p;
    t.left = outer.left;
    if (der_enter(&t, 0x30, &body) != 0) return -1;
    c->tbs = body.p; /* TBS SEQ tag: TLV start for hashing */
    {
      uint8_t tg;
      uint32_t tl;
      const uint8_t *tv;
      der_t h;
      h.p = body.p;
      h.left = body.left;
      if (der_peek(&h, &tg, &tl, &tv) != 0 || tg != 0x30) return -1;
      c->tbs_len = (uint32_t)(tv - body.p) + tl;
    }
    if (der_enter(&body, 0x30, &tbs) != 0) return -1;
  }
  /* [0] version (optional explicit). */
  if (tbs.left >= 2 && tbs.p[0] == 0xA0) der_skip(&tbs);
  /* serial INTEGER. */
  if (der_peek(&tbs, &tag, &tlen, &tval) != 0 || tag != 0x02) return -1;
  der_skip(&tbs);
  /* signature AlgorithmIdentifier. */
  if (parse_sigalg(&tbs) != 0) return -1;
  /* issuer Name (keep raw TLV). */
  if (der_peek(&tbs, &tag, &tlen, &tval) != 0 || tag != 0x30) return -1;
  {
    der_t tmp;
    c->issuer = tbs.p;
    tmp.p = tbs.p;
    tmp.left = tbs.left;
    der_skip(&tmp);
    c->issuer_len = tbs.left - tmp.left;
  }
  der_skip(&tbs);
  /* validity SEQ { notBefore, notAfter }. */
  {
    der_t val;
    if (der_enter(&tbs, 0x30, &val) != 0) return -1;
    if (parse_time(&val, &c->not_before) != 0) return -1;
    if (parse_time(&val, &c->not_after) != 0) return -1;
    der_skip(&tbs);
  }
  /* subject Name (keep raw TLV). */
  if (der_peek(&tbs, &tag, &tlen, &tval) != 0 || tag != 0x30) return -1;
  {
    der_t tmp;
    c->subject = tbs.p;
    tmp.p = tbs.p;
    tmp.left = tbs.left;
    der_skip(&tmp);
    c->subject_len = tbs.left - tmp.left;
  }
  der_skip(&tbs);
  /* subjectPublicKeyInfo. */
  if (parse_spki(&tbs, c->spki_n, &c->spki_e) != 0) return -1;
  /* Optional [1] issuerUniqueID / [2] subjectUniqueID (skip). */
  while (tbs.left >= 2 && (tbs.p[0] == 0x81 || tbs.p[0] == 0x82))
    der_skip(&tbs);
  /* Optional [3] extensions. */
  if (tbs.left >= 2 && tbs.p[0] == 0xA3) {
    der_t exwrap, exts;
    if (der_enter(&tbs, 0xA3, &exwrap) != 0) return -1;
    if (der_enter(&exwrap, 0x30, &exts) != 0) return -1;
    san_collect(&exts, &c->san);
    der_skip(&tbs);
  }
  /* Outer signatureAlgorithm + signature BIT STRING follow tbs. */
  {
    der_t outer;
    outer.p = c->tbs + c->tbs_len;
    outer.left = len - (uint32_t)(outer.p - der);
    if (parse_sigalg(&outer) != 0) return -1;
    if (der_peek(&outer, &tag, &tlen, &tval) != 0 || tag != 0x03)
      return -1;
    if (tlen < 1 || tval[0] != 0) return -1;
    c->sig = tval + 1;
    c->sig_len = tlen - 1;
  }
  return 0;
}

/* Name match: exact DER bytes. */
static int name_eq(const uint8_t *a, uint32_t al, const uint8_t *b,
                   uint32_t bl) {
  uint32_t i;
  if (al != bl) return 0;
  for (i = 0; i < al; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* Hostname check: exact dNSName (no wildcards) or IPv4 bytes. 1 ok. */
static int host_ok(const san_t *san, const char *host, const uint8_t ip[4]) {
  uint32_t i;
  if (ip) {
    if (san->has_ip && san->ip[0] == ip[0] && san->ip[1] == ip[1] &&
        san->ip[2] == ip[2] && san->ip[3] == ip[3])
      return 1;
    return 0;
  }
  if (!host || !san->has_dns) return 0;
  for (i = 0; host[i] && san->dns[i]; i++)
    if (host[i] != san->dns[i]) return 0;
  return host[i] == '\0' && san->dns[i] == '\0';
}

/* Verify a leaf chain: leaf signed by the trust anchor, anchor
 * self-consistent, names/host/dates sane. host NULL when dialing by IP
 * (pass ip too); ip NULL when dialing by name. now = yyyymmddhhmmss.
 * Returns 0 ok, <0 reason. */
int x509_verify_chain(const uint8_t *leaf_der, uint32_t leaf_len,
                      const uint8_t *ca_der, uint32_t ca_len,
                      const char *host, const uint8_t ip[4], uint64_t now) {
  cert_t leaf, ca;
  uint8_t digest[32];
  sha256_t sh;
  if (!leaf_der || leaf_len == 0 || leaf_len > 8192) return -1;
  if (!ca_der || ca_len == 0 || ca_len > 8192) return -1;
  if (cert_parse(leaf_der, leaf_len, &leaf) != 0) return -2;
  if (cert_parse(ca_der, ca_len, &ca) != 0) return -3;
  /* Issuer match (leaf.issuer == ca.subject). */
  if (!name_eq(leaf.issuer, leaf.issuer_len, ca.subject, ca.subject_len))
    return -4;
  /* Dates. */
  if (now < leaf.not_before || now > leaf.not_after) return -5;
  if (now < ca.not_before || now > ca.not_after) return -6;
  /* Leaf signature over its tbs, with the CA key. */
  sha256_init(&sh);
  sha256_update(&sh, leaf.tbs, leaf.tbs_len);
  sha256_final(&sh, digest);
  if (leaf.sig_len != 256) return -7;
  if (rsa_verify(ca.spki_n, ca.spki_e, leaf.sig, digest) != 1) return -8;
  /* Host binding. */
  if (!host_ok(&leaf.san, host, ip)) return -9;
  return 0;
}

/* Expiry text for logs (caller prints now vs notAfter). */
uint64_t x509_leaf_expiry(const uint8_t *leaf_der, uint32_t leaf_len) {
  cert_t leaf;
  if (cert_parse(leaf_der, leaf_len, &leaf) != 0) return 0;
  return leaf.not_after;
}

/* Extract the leaf SPKI (RSA n + e) without verifying. 0 ok. */
int x509_spki(const uint8_t *leaf_der, uint32_t leaf_len,
              uint8_t n_out[256], uint32_t *e_out) {
  cert_t leaf;
  uint32_t i;
  if (cert_parse(leaf_der, leaf_len, &leaf) != 0) return -1;
  for (i = 0; i < 256; i++) n_out[i] = leaf.spki_n[i];
  *e_out = leaf.spki_e;
  return 0;
}
