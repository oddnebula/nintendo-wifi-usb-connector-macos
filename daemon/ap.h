#ifndef WFC_AP_H
#define WFC_AP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "ieee80211.h"

#define AP_MAX_STATIONS 16
#define AP_PSQ_LEN      64

typedef enum { POLICY_PENDING = 0, POLICY_ALLOWED, POLICY_DENIED } sta_policy_t;

typedef struct psq_entry { uint8_t *data; size_t len; } psq_entry_t;

typedef struct station {
    int in_use;
    uint8_t mac[6];
    int authed, assoc;
    uint16_t aid;
    sta_policy_t policy;
    int ps;                         /* station is dozing */
    psq_entry_t psq[AP_PSQ_LEN];    /* frames buffered while dozing */
    int psq_head, psq_count;
    time_t last_seen;
    int rssi;
    uint32_t ip;                    /* host order, learned from DHCP */
    uint64_t rx_bytes, tx_bytes;
    int dirty;                      /* needs a GUI update */
} station_t;

/* Transmit hook (radio) and uplink hook (network). */
typedef int  (*ap_tx_fn)(void *ctx, const uint8_t *frame, size_t len, int rate, int ack);
typedef void (*ap_uplink_fn)(void *ctx, const uint8_t *eth, size_t len);
typedef void (*ap_station_fn)(void *ctx, const station_t *sta, int gone);

typedef struct ap {
    ap_bss_t bss;
    station_t sta[AP_MAX_STATIONS];
    int auto_accept;                /* allow unknown stations without asking */
    uint8_t (*allow)[6]; int n_allow;
    uint8_t (*deny)[6];  int n_deny;
    ap_tx_fn tx; void *tx_ctx;
    ap_uplink_fn uplink; void *uplink_ctx;
    ap_station_fn on_station; void *station_ctx;
    int inactivity_timeout;         /* seconds */
} ap_t;

void ap_init(ap_t *ap, const uint8_t bssid[6], const char *ssid, int channel);
void ap_free(ap_t *ap);

/* All entry points below take the AP lock internally. */
void ap_handle_rx(ap_t *ap, const uint8_t *frame, size_t len, int rssi);
void ap_deliver_ethernet(ap_t *ap, const uint8_t *eth, size_t len); /* from the network */
void ap_send_beacon(ap_t *ap);
void ap_housekeeping(ap_t *ap);                                      /* call ~1 Hz */

void ap_set_policy(ap_t *ap, const uint8_t mac[6], sta_policy_t policy);
void ap_kick(ap_t *ap, const uint8_t mac[6]);
void ap_forget(ap_t *ap, const uint8_t mac[6]);
void ap_set_auto_accept(ap_t *ap, int on);
void ap_set_station_ip(ap_t *ap, const uint8_t mac[6], uint32_t ip);
void ap_shutdown(ap_t *ap);                                          /* deauth everyone */

/* Snapshot helpers for status reporting */
void ap_foreach_station(ap_t *ap, void (*fn)(void *ctx, const station_t *sta), void *ctx);

#endif
