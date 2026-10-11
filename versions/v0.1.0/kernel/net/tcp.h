/* NaOS TCP: minimal client (active open, in-order RX, graceful close).
 * One TCB per connection; polled RX in tick context, waits in caller
 * context. Retransmits SYN/data on 1s RTO. Own code. */
#ifndef NAOS_TCP_H
#define NAOS_TCP_H

#include <stdint.h>

/* Connect (SYN, ~4s max). Returns conn id or -1. */
int tcp_connect(uint32_t ip, uint16_t port);
/* Send (ESTABLISHED only, <=1460/call). Returns len or -1. */
int tcp_send(int t, const void *buf, uint32_t len);
/* Receive (copies buffered data). >0 bytes, 0 = closed+empty, -1 = timeout. */
int tcp_recv(int t, void *buf, uint32_t cap, uint64_t timeout_ticks);
/* Graceful close (FIN exchange, best effort). Always frees. 0 ok. */
int tcp_close(int t);
/* RX handler (called from net.c for TCP packets for us). */
void tcp_on_seg(uint32_t srcip, const uint8_t *seg, uint32_t len);
/* HTTP/1.0 GET, print text body. 0 ok. */
int net_fetch(uint32_t ip, uint16_t port, const char *path);

#endif /* NAOS_TCP_H */
