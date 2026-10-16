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
