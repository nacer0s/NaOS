/* NaOS virtio-net (legacy 0.9x PCI): polled queues, no offloads.
 * 1AF4:1000 with an I/O BAR. Frames carry a 10-byte header on the
 * wire (stripped/added here); the stack sees plain Ethernet. */
#ifndef NAOS_VIRTIO_H
#define NAOS_VIRTIO_H

#include <stdint.h>

int virtio_init(void); /* find NIC, queues, MAC. 1 ok */
int virtio_link(void); /* always 1 (no legacy link bit) */
/* Send one frame (len + 10B header <= 4K). 0 ok. */
int virtio_send(const void *data, uint32_t len);
/* Receive one frame into buf. Returns length, 0 = none. */
uint32_t virtio_recv(void *buf, uint32_t cap);
void virtio_mac(uint8_t mac[6]);
uint64_t virtio_tx_count(void);
uint64_t virtio_rx_count(void);

#endif /* NAOS_VIRTIO_H */
