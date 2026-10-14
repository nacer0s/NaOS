/* NaOS DHCP client (discover/offer/request/ack). Proves UDP. Own code. */
#ifndef NAOS_DHCP_H
#define NAOS_DHCP_H

/* Run DHCP (up to ~3s). 0 = got lease (addrs applied), -1 = keep static. */
int net_dhcp(void);

#endif /* NAOS_DHCP_H */
