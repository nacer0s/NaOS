/* NaOS X.509 chain verify (TLS server side). Own code.
 * Trust anchor: local TEST CA (tls_ca_der, generated for tests).
 * Production use needs a real root + rotation story (documented). */
#ifndef NAOS_TLS_X509_H
#define NAOS_TLS_X509_H

#include <stdint.h>

/* Verify a leaf chain against an explicit trust anchor (no globals:
 * test CA today, real root + rotation later). host NULL when dialing
 * by IP (pass ip); ip NULL when dialing by name. now = yyyymmddhhmmss
 * digits (RTC clock). Returns 0 ok, <0 reason. */
int x509_verify_chain(const uint8_t *leaf_der, uint32_t leaf_len,
                      const uint8_t *ca_der, uint32_t ca_len,
                      const char *host, const uint8_t ip[4], uint64_t now);
uint64_t x509_leaf_expiry(const uint8_t *leaf_der, uint32_t leaf_len);
/* Extract the leaf SPKI (RSA n + e) without verifying. 0 ok. */
int x509_spki(const uint8_t *leaf_der, uint32_t leaf_len,
              uint8_t n_out[256], uint32_t *e_out);

#endif /* NAOS_TLS_X509_H */
