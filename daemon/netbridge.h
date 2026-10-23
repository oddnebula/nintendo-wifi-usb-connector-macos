#ifndef WFC_NETBRIDGE_H
#define WFC_NETBRIDGE_H

#include <stdint.h>
#include <stddef.h>

/* Uplink to the Mac's internet connection via vmnet.framework (shared/NAT mode). The Mac
 * acts as DHCP server, DNS proxy and NAT router for the consoles, exactly like Internet
 * Sharing did for the original connector on Windows. Requires root. */
typedef struct netbridge netbridge_t;
typedef void (*nb_rx_fn)(void *ctx, uint8_t *eth, size_t len);

int  nb_start(netbridge_t **out, nb_rx_fn rx, void *ctx, char *info, size_t infolen);
void nb_send(netbridge_t *nb, const uint8_t *eth, size_t len);
void nb_stop(netbridge_t *nb);

#endif
