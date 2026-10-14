/* NaOS DNS client: A-record lookup over UDP (usually the gateway or the
 * DHCP-provided server). Blocking with retries; command context only. */
#ifndef NAOS_DNS_H
#define NAOS_DNS_H

#include <stdint.h>

/* Resolve a hostname to host-order IPv4. 0 ok, -1 on failure (~3s max). */
int dns_resolve(const char *name, uint32_t *out);

#endif /* NAOS_DNS_H */
