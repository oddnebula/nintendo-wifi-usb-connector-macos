#ifndef WFC_IEEE80211_H
#define WFC_IEEE80211_H

#include <stdint.h>
#include <stddef.h>

#define WLAN_HDR_LEN 24

/* frame_control byte 0: subtype << 4 | type << 2 */
#define FC_ASSOC_REQ     0x00
#define FC_ASSOC_RESP    0x10
#define FC_REASSOC_REQ   0x20
#define FC_REASSOC_RESP  0x30
#define FC_PROBE_REQ     0x40
#define FC_PROBE_RESP    0x50
#define FC_BEACON        0x80
#define FC_DISASSOC      0xa0
#define FC_AUTH          0xb0
#define FC_DEAUTH        0xc0
#define FC_PSPOLL        0xa4
#define FC_DATA          0x08
#define FC_NULLFUNC      0x48
#define FC_QOS_DATA      0x88
#define FC_QOS_NULL      0xc8

/* frame_control byte 1 flags */
#define FCF_TODS     0x01
#define FCF_FROMDS   0x02
#define FCF_MOREFRAG 0x04
#define FCF_RETRY    0x08
#define FCF_PM       0x10
#define FCF_MOREDATA 0x20
#define FCF_PROTECTED 0x40

#define FC_TYPE(b0) (((b0) >> 2) & 3)
enum { WLAN_TYPE_MGMT = 0, WLAN_TYPE_CTRL = 1, WLAN_TYPE_DATA = 2 };

#define CAP_ESS            0x0001
#define CAP_SHORT_PREAMBLE 0x0020

#define REASON_UNSPECIFIED   1
#define REASON_DEAUTH_LEAVING 3
#define REASON_INACTIVITY    4
#define REASON_NOT_AUTHED    6
#define REASON_NOT_ASSOCED   7

typedef struct ap_bss {
    uint8_t bssid[6];
    char ssid[33];
    int channel;
    int beacon_interval;  /* TU */
} ap_bss_t;

/* All builders return the frame length written to `out` (at least 256 bytes). */
size_t wlan_build_beacon(uint8_t *out, const ap_bss_t *bss, const uint8_t *tim_aids, int n_aids);
size_t wlan_build_probe_resp(uint8_t *out, const ap_bss_t *bss, const uint8_t *da);
size_t wlan_build_auth(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, uint16_t alg,
                       uint16_t seq, uint16_t status);
size_t wlan_build_assoc_resp(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, int reassoc,
                             uint16_t status, uint16_t aid);
size_t wlan_build_deauth(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, int disassoc,
                         uint16_t reason);

/* Wraps an Ethernet frame (dst, src, ethertype, payload) as a FromDS data frame. */
size_t wlan_from_ethernet(uint8_t *out, size_t outsize, const ap_bss_t *bss,
                          const uint8_t *eth, size_t eth_len, int more_data);

/* Unwraps a ToDS data frame into an Ethernet frame. Returns 0 if not convertible. */
size_t wlan_to_ethernet(uint8_t *out, size_t outsize, const uint8_t *frame, size_t len);

/* Finds an information element in a management frame body; returns pointer to IE header. */
const uint8_t *wlan_find_ie(const uint8_t *ies, size_t len, uint8_t id);

int mac_is_multicast(const uint8_t *mac);
void mac_fmt(char out[18], const uint8_t *mac);
int mac_parse(const char *s, uint8_t out[6]);

#endif
