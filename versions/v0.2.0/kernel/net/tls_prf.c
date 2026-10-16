/* NaOS TLS helpers: HMAC-SHA256, TLS 1.2 PRF/P_hash, key expansion,
 * SHA-256 counter-DRBG. Portable (stdint only). Own code from RFC 5246. */
#include "tls_prf.h"
#include "../lib/sha256.h"

/* HMAC-SHA256(key, msg) -> 32 bytes. Key longer than 64 hashed first. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len, uint8_t out[32]) {
  sha256_t c;
  uint8_t kb[64], ip[64], op[64];
  uint8_t kh[32];
  uint32_t i;
  const uint8_t *k = key;
  uint32_t kl = key_len;
  if (kl > 64) {
    sha256_init(&c);
    sha256_update(&c, key, key_len);
    sha256_final(&c, kh);
    k = kh;
    kl = 32;
  }
  for (i = 0; i < 64; i++) {
    uint8_t b = (i < kl) ? k[i] : 0;
    kb[i] = b;
    ip[i] = b ^ 0x36;
    op[i] = b ^ 0x5c;
  }
  sha256_init(&c);
  sha256_update(&c, ip, 64);
  sha256_update(&c, msg, msg_len);
  sha256_final(&c, kh);
  sha256_init(&c);
  sha256_update(&c, op, 64);
  sha256_update(&c, kh, 32);
  sha256_final(&c, out);
  (void)kb;
}

/* P_hash(secret, seed): out[0..out_len) = HMAC(secret, A(1)+seed) first. */
static void p_hash(const uint8_t *secret, uint32_t secret_len,
                   const uint8_t *seed, uint32_t seed_len,
                   uint8_t *out, uint32_t out_len) {
  uint8_t a[32], chunk[32], buf[32 + 128];
  uint32_t i, done = 0;
  /* A(1) = HMAC(secret, seed). Seed cap: 128 (fits buf with A). */
  uint8_t s0[128];
  for (i = 0; i < seed_len && i < 128; i++) s0[i] = seed[i];
  hmac_sha256(secret, secret_len, s0, seed_len > 128 ? 128 : seed_len, a);
  while (done < out_len) {
    uint32_t n;
    for (i = 0; i < 32; i++) buf[i] = a[i];
    for (i = 0; i < seed_len && 32 + i < sizeof(buf); i++)
      buf[32 + i] = seed[i];
    hmac_sha256(secret, secret_len, buf, 32 + seed_len, chunk);
    n = out_len - done;
    if (n > 32) n = 32;
    for (i = 0; i < n; i++) out[done + i] = chunk[i];
    done += n;
    hmac_sha256(secret, secret_len, a, 32, a); /* A(i+1) */
  }
}

/* TLS 1.2 PRF(secret, label, seed) with SHA-256. */
void tls_prf(const uint8_t *secret, uint32_t secret_len,
             const char *label,
             const uint8_t *seed, uint32_t seed_len,
             uint8_t *out, uint32_t out_len) {
  uint8_t lab[128];
  uint32_t i, ll = 0;
  while (label[ll] && ll < 120) {
    lab[ll] = (uint8_t)label[ll];
    ll++;
  }
  for (i = 0; i < seed_len && ll + i < sizeof(lab); i++)
    lab[ll + i] = seed[i];
  p_hash(secret, secret_len, lab, ll + seed_len, out, out_len);
}

/* Master secret (48B) from pre-master + client/server random. */
void tls_master(const uint8_t *premaster, uint32_t pm_len,
                const uint8_t cli_rand[32], const uint8_t srv_rand[32],
                uint8_t master[48]) {
  uint8_t seed[64];
  uint32_t i;
  for (i = 0; i < 32; i++) {
    seed[i] = cli_rand[i];
    seed[32 + i] = srv_rand[i];
  }
  tls_prf(premaster, pm_len, "master secret", seed, 64, master, 48);
}

/* Key block for AES_128_GCM_SHA256 (44B used of 48): RFC 5246 order
 * client_write_key[16], server_write_key[16], client_write_IV[4],
 * server_write_IV[4]. Caller splits. Seed = server_random + client_random. */
void tls_keyblock(const uint8_t master[48],
                  const uint8_t srv_rand[32], const uint8_t cli_rand[32],
                  uint8_t out[48]) {
  uint8_t seed[64];
  uint32_t i;
  for (i = 0; i < 32; i++) {
    seed[i] = srv_rand[i];
    seed[32 + i] = cli_rand[i];
  }
  tls_prf(master, 48, "key expansion", seed, 64, out, 48);
}

/* Finished verify_data (12B) for client/server. */
void tls_finished(const uint8_t master[48], int is_client,
                  const uint8_t *hs_hash, uint8_t out[12]) {
  tls_prf(master, 48, is_client ? "client finished" : "server finished",
          hs_hash, 32, out, 12);
}

/* ---- DRBG (SHA-256 counter mode, NOT a certified CSPRNG) ----
 * Seeded from platform entropy (caller mixes PIT/TSC/RTC/MAC).
 * Rekeyed from its own output; adequate for handshake nonces/keys
 * in this experimental stack (documented, review before real use). */
static uint8_t drbg_v[32];
static uint64_t drbg_ctr;
static int drbg_on;

void tls_rand_seed(const uint8_t *seed, uint32_t len) {
  sha256_t c;
  sha256_init(&c);
  sha256_update(&c, seed, len);
  sha256_final(&c, drbg_v);
  drbg_ctr = 0;
  drbg_on = 1;
}

void tls_rand(uint8_t *out, uint32_t len) {
  uint8_t blk[32];
  uint32_t i, done = 0;
  sha256_t c;
  if (!drbg_on) {
    /* Unseeded: deterministic zeros are worse than nothing — but the
     * caller must seed; this only avoids uninitialized reads. */
    for (i = 0; i < len; i++) out[i] = 0;
    return;
  }
  while (done < len) {
    uint8_t msg[40];
    uint32_t n;
    for (i = 0; i < 32; i++) msg[i] = drbg_v[i];
    for (i = 0; i < 8; i++)
      msg[32 + i] = (uint8_t)(drbg_ctr >> (56 - 8 * i));
    drbg_ctr++;
    sha256_init(&c);
    sha256_update(&c, msg, 40);
    sha256_final(&c, blk);
    /* Forward secrecy of the state: roll V every block. */
    sha256_init(&c);
    sha256_update(&c, blk, 32);
    sha256_update(&c, drbg_v, 32);
    sha256_final(&c, drbg_v);
    n = len - done;
    if (n > 32) n = 32;
    for (i = 0; i < n; i++) out[done + i] = blk[i];
    done += n;
  }
}

/* RFC 4231 case 1 (key=0x0b*20, data="Hi There" ->
 * b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7). */
int hmac_selftest(void) {
  static const uint8_t want[32] = {
    0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,
    0x0b,0xf1,0x2b,0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,
    0x37,0x6c,0x2e,0x32,0xcf,0xf7 };
  uint8_t key[20], out[32];
  int i;
  for (i = 0; i < 20; i++) key[i] = 0x0b;
  hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, out);
  for (i = 0; i < 32; i++)
    if (out[i] != want[i]) return -1;
  return 0;
}

/* Handshake KDF selftest: PMS 00..1F, cli A0.., srv 50... Expected
 * values from an independent Python RFC-5246 reference (kept in the
 * commit message tooling, not in the tree). Catches seed-order and
 * truncation bugs like the p_hash 96B over-read (Oct-16). */
int tls_hs_selftest(void) {
  static const uint8_t v_pms[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
  static const uint8_t v_cli[32] = {
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,
    0xac,0xad,0xae,0xaf,0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
    0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf };
  static const uint8_t v_srv[32] = {
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,
    0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f };
  static const uint8_t v_master[48] = {
    0xd6,0xd4,0xf0,0xfe,0x5d,0x6c,0xe5,0xeb,0xb3,0xc3,0x88,0xa9,
    0x69,0x1b,0x0f,0xe6,0x61,0x02,0x61,0xa2,0xc4,0x74,0xa2,0x35,
    0xee,0xef,0x04,0xd6,0x95,0x6a,0x9b,0x13,0x18,0x09,0x4c,0x25,
    0x07,0x8c,0x34,0xdf,0xba,0x30,0x1e,0x11,0x66,0x31,0xed,0xf3 };
  static const uint8_t v_kb[48] = {
    0x79,0xe4,0x47,0x73,0x98,0x50,0x58,0x0c,0x06,0x5f,0xa7,0x9e,
    0x2a,0xf4,0x4e,0x60,0x1f,0x13,0xef,0xcf,0xd6,0xe8,0xc2,0x85,
    0x20,0xdb,0x78,0x06,0x6c,0x81,0xa1,0x2b,0xca,0x8f,0xac,0x10,
    0x20,0x62,0x2b,0x10,0xd3,0x9a,0xb9,0x44,0x8e,0x87,0x2f,0x12 };
  static const uint8_t v_hsh[32] = {
    0x1b,0x3d,0x53,0x37,0x65,0x9a,0x4e,0x81,0x09,0xf1,0x10,0x71,
    0xe2,0x89,0x69,0xbd,0x0e,0xb3,0xf8,0x9d,0x35,0x49,0x47,0xb7,
    0xba,0xfd,0xe4,0xde,0x53,0x48,0x3d,0x43 };
  static const uint8_t v_cfin[12] = {
    0x17,0xce,0xdf,0xd6,0xba,0x73,0x6a,0x8d,0xca,0x0a,0xd5,0xa5 };
  static const uint8_t v_sfin[12] = {
    0x16,0x89,0xd1,0x10,0x3b,0xd1,0xe0,0x8d,0xc4,0x19,0x43,0xfd };
  uint8_t master[48], kb[48], fin[12];
  uint32_t i;
  tls_master(v_pms, 32, v_cli, v_srv, master);
  for (i = 0; i < 48; i++)
    if (master[i] != v_master[i]) return -1;
  tls_keyblock(master, v_srv, v_cli, kb);
  for (i = 0; i < 48; i++)
    if (kb[i] != v_kb[i]) return -2;
  tls_finished(master, 1, v_hsh, fin);
  for (i = 0; i < 12; i++)
    if (fin[i] != v_cfin[i]) return -3;
  tls_finished(master, 0, v_hsh, fin);
  for (i = 0; i < 12; i++)
    if (fin[i] != v_sfin[i]) return -4;
  return 0;
}
