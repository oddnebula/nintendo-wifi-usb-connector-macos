#ifndef WFC_NETUTIL_H
#define WFC_NETUTIL_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/*
 * DNS redirection: consoles get the normal DHCP-assigned resolver, but every DNS query
 * they send is transparently re-addressed to the configured server (e.g. Wiimmfi), and
 * the replies are re-addressed back. No console-side DNS settings are required, though
 * manually entered DNS on the console also works since all port-53 traffic is caught.
 */
#define DNS_NAT_SLOTS 256

typedef struct dns_nat {
    uint32_t client_ip, orig_dst, new_dst;  /* network order */
    uint16_t client_port, txid;
    int64_t when;
} dns_nat_t;

typedef struct dnsrw {
    pthread_mutex_t lock;
    uint32_t primary;    /* network order, 0 = no redirection */
    uint32_t secondary;  /* optional: used for non-Nintendo names */
    dns_nat_t slots[DNS_NAT_SLOTS];
    int next;
    uint64_t redirected;
} dnsrw_t;

void dnsrw_init(dnsrw_t *rw);
void dnsrw_set(dnsrw_t *rw, uint32_t primary, uint32_t secondary);
/* Station -> network. Returns 1 if the frame was modified. */
int  dnsrw_outbound(dnsrw_t *rw, uint8_t *eth, size_t len);
/* Network -> station. Returns 1 if the frame was modified. */
int  dnsrw_inbound(dnsrw_t *rw, uint8_t *eth, size_t len);

/* True for names served by the Nintendo WFC replacement (nintendowifi.net etc.). */
int  dns_is_nintendo_name(const char *name);
/* Extracts the first question name of a DNS message into `out`. */
int  dns_qname(const uint8_t *dns, size_t len, char *out, size_t outsize);

/* Inspects a network->station frame for a DHCP ACK; returns 1 and fills mac/ip if found. */
int  dhcp_snoop_ack(const uint8_t *eth, size_t len, uint8_t mac[6], uint32_t *ip_host_order);

uint16_t ip_checksum(const void *data, size_t len);

#endif
