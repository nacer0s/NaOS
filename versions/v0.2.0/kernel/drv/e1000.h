/* NaOS E1000 (82540EM) driver: polled TX/RX rings. Own code. */
#ifndef NAOS_E1000_H
#define NAOS_E1000_H

#include <stdint.h>

int e1000_init(void); /* find NIC, reset, link, rings. 1 ok */
int e1000_link(void); /* 1 while link is up */
/* Send one frame (len ≤ 2048). 0 ok. */
int e1000_send(const void *data, uint32_t len);
/* Receive one frame into buf (cap 2048). Returns length, 0 = none. */
uint32_t e1000_recv(void *buf, uint32_t cap);
void e1000_mac(uint8_t mac[6]);
uint64_t e1000_tx_count(void);
uint64_t e1000_rx_count(void);

#endif /* NAOS_E1000_H */
