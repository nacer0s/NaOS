/* NaOS XHCI driver: polled USB, no interrupts. Own code. */
#ifndef NAOS_XHCI_H
#define NAOS_XHCI_H

#include <stdint.h>

/* One popped event-ring entry. */
typedef struct {
  uint8_t type; /* 32 transfer, 33 command completion, else other */
  uint8_t cc;   /* completion code (1 = success) */
  uint8_t slot;
  uint8_t ep;   /* endpoint DCI for transfer events */
  uint32_t len; /* residual byte count for transfer events */
  uint64_t ptr; /* source TRB address */
} xhci_ev_t;

#define XHCI_CC_SUCCESS 1
#define XHCI_EV_XFER 32
#define XHCI_EV_CMD  33

int xhci_init(void); /* find HC, reset, start, rings. 1 ok */
int xhci_port_count(void);
int xhci_port_device(int port);  /* 1-based port; 1 if connected */
int xhci_port_speed(int port);   /* PORTSC speed, 0 if none */
int xhci_port_reset(int port);   /* power + reset, wait for enable */
int xhci_next_event(xhci_ev_t *ev); /* 1 = got one, 0 = ring empty */

/* Slot commands (synchronous, poll for completion inside). */
int xhci_enable_slot(uint8_t *slot_out);
int xhci_disable_slot(uint8_t slot); /* free a slot after a failed setup */
int xhci_address_device(uint8_t slot, int port, int speed);
int xhci_control(uint8_t slot, const uint8_t setup[8], void *data,
                 uint32_t len, int dir_in);
int xhci_open_ep(uint8_t slot, int dci); /* alloc transfer ring */
int xhci_configure_ep(uint8_t slot, int config, int iface, int ep_num,
                      int ep_type, int mps, int interval);

/* Queue one interrupt-IN transfer (async completion via events). */
int xhci_int_in(uint8_t slot, int dci, void *buf, uint32_t len);

/* Completion code of the last waited command/transfer (-1 = timeout). */
int xhci_last_cc(void);

#endif /* NAOS_XHCI_H */
