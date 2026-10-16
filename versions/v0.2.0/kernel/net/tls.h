/* NaOS TLS 1.2 client interface (ECDHE_RSA_AES_128_GCM_SHA256). Own code.
 * Scope: TLS 1.2 only, one suite, direct-issued leaf, no client certs,
 * no renegotiation/resume. Test CA today; real root + rotation later. */
#ifndef NAOS_TLS_H
#define NAOS_TLS_H

#include <stdint.h>

/* Connect + handshake. host NULL when dialing by IP (ipb set); ipb NULL
 * when dialing by name. 0 ok (session live). */
int tls_connect(uint32_t ip, uint16_t port, const char *host,
                const uint8_t ipb[4]);
/* Send application bytes (chunked). 0 ok. */
int tls_send(const uint8_t *buf, uint32_t len);
/* One application fragment. >0 bytes, 0 = clean close, <0 error. */
int tls_recv(uint8_t *buf, uint32_t cap);
void tls_close(void);
/* HTTPS GET, streaming the body through cb(chunk, len, done). 0 ok. */
int tls_fetch(uint32_t ip, uint16_t port, const char *host,
              const uint8_t ipb[4], const char *path,
              void (*cb)(const uint8_t *, uint32_t, int));
/* HTTPS GET printing the body (shell). 0 ok. */
int tls_fetch_print(uint32_t ip, uint16_t port, const char *host,
                    const uint8_t ipb[4], const char *path);

#endif /* NAOS_TLS_H */
