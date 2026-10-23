#include "ieee80211.h"

#include <string.h>
#include <stdio.h>

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
/* 1 and 2 Mbit/s basic (the DS only speaks these); 5.5/11 and 802.11g OFDM rates are
 * optional so modern devices also see the network. */
static const uint8_t RATES[] = { 0x82, 0x84, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24 };
static const uint8_t EXT_RATES[] = { 0x30, 0x48, 0x60, 0x6c };

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static size_t hdr(uint8_t *out, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2,
                  const uint8_t *a3)
{
    out[0] = fc0;
    out[1] = fc1;
    put16(out + 2, 0);  /* duration: hardware fills for ACKed frames */
    memcpy(out + 4, a1, 6);
    memcpy(out + 10, a2, 6);
    memcpy(out + 16, a3, 6);
    put16(out + 22, 0); /* sequence: driver stamps */
    return WLAN_HDR_LEN;
}

static size_t ie(uint8_t *p, uint8_t id, const void *data, size_t len)
{
    p[0] = id;
    p[1] = (uint8_t)len;
    memcpy(p + 2, data, len);
    return 2 + len;
}

static size_t common_body(uint8_t *p, const ap_bss_t *bss)
{
    size_t n = 0;
    memset(p, 0, 8);                       /* timestamp, filled in by hardware */
    n += 8;
    put16(p + n, (uint16_t)bss->beacon_interval);
    n += 2;
    put16(p + n, CAP_ESS);
    n += 2;
    n += ie(p + n, 0, bss->ssid, strlen(bss->ssid));
    n += ie(p + n, 1, RATES, sizeof(RATES));
    uint8_t ch = (uint8_t)bss->channel;
    n += ie(p + n, 3, &ch, 1);
    return n;
}

static size_t tail_ies(uint8_t *p)
{
    size_t n = 0;
    uint8_t erp = 0;                       /* no protection, no barker preamble needed */
    n += ie(p + n, 42, &erp, 1);
    n += ie(p + n, 50, EXT_RATES, sizeof(EXT_RATES));
    return n;
}

size_t wlan_build_beacon(uint8_t *out, const ap_bss_t *bss, const uint8_t *tim_aids, int n_aids)
{
    size_t n = hdr(out, FC_BEACON, 0, BCAST, bss->bssid, bss->bssid);
    n += common_body(out + n, bss);

    /* TIM: DTIM count 0, period 1, partial virtual bitmap of stations with buffered frames */
    uint8_t bitmap[251] = {0};
    int max_byte = 0;
    for (int i = 0; i < n_aids; i++) {
        int aid = tim_aids[i];
        bitmap[aid / 8] |= (uint8_t)(1 << (aid % 8));
        if (aid / 8 > max_byte) max_byte = aid / 8;
    }
    uint8_t tim[4 + 251];
    tim[0] = 0;
    tim[1] = 1;
    tim[2] = 0; /* bitmap offset 0 */
    memcpy(tim + 3, bitmap, (size_t)max_byte + 1);
    n += ie(out + n, 5, tim, 3 + (size_t)max_byte + 1);
    n += tail_ies(out + n);
    return n;
}

size_t wlan_build_probe_resp(uint8_t *out, const ap_bss_t *bss, const uint8_t *da)
{
    size_t n = hdr(out, FC_PROBE_RESP, 0, da, bss->bssid, bss->bssid);
    n += common_body(out + n, bss);
    n += tail_ies(out + n);
    return n;
}

size_t wlan_build_auth(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, uint16_t alg,
                       uint16_t seq, uint16_t status)
{
    size_t n = hdr(out, FC_AUTH, 0, da, bss->bssid, bss->bssid);
    put16(out + n, alg);
    put16(out + n + 2, seq);
    put16(out + n + 4, status);
    return n + 6;
}

size_t wlan_build_assoc_resp(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, int reassoc,
                             uint16_t status, uint16_t aid)
{
    size_t n = hdr(out, reassoc ? FC_REASSOC_RESP : FC_ASSOC_RESP, 0, da, bss->bssid, bss->bssid);
    put16(out + n, CAP_ESS);
    put16(out + n + 2, status);
    put16(out + n + 4, (uint16_t)(aid | 0xc000));
    n += 6;
    n += ie(out + n, 1, RATES, sizeof(RATES));
    n += ie(out + n, 50, EXT_RATES, sizeof(EXT_RATES));
    return n;
}

size_t wlan_build_deauth(uint8_t *out, const ap_bss_t *bss, const uint8_t *da, int disassoc,
                         uint16_t reason)
{
    size_t n = hdr(out, disassoc ? FC_DISASSOC : FC_DEAUTH, 0, da, bss->bssid, bss->bssid);
    put16(out + n, reason);
    return n + 2;
}

size_t wlan_from_ethernet(uint8_t *out, size_t outsize, const ap_bss_t *bss,
                          const uint8_t *eth, size_t eth_len, int more_data)
{
    if (eth_len < 14) return 0;
    size_t payload = eth_len - 14;
    if (WLAN_HDR_LEN + 8 + payload > outsize) return 0;
    size_t n = hdr(out, FC_DATA, FCF_FROMDS | (more_data ? FCF_MOREDATA : 0),
                   eth /* DA */, bss->bssid, eth + 6 /* SA */);
    static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    memcpy(out + n, snap, 6);
    out[n + 6] = eth[12];
    out[n + 7] = eth[13];
    n += 8;
    memcpy(out + n, eth + 14, payload);
    return n + payload;
}

size_t wlan_to_ethernet(uint8_t *out, size_t outsize, const uint8_t *f, size_t len)
{
    if (len < WLAN_HDR_LEN) return 0;
    if ((f[1] & (FCF_TODS | FCF_FROMDS)) != FCF_TODS) return 0;
    if (f[1] & FCF_PROTECTED) return 0;
    size_t hl = WLAN_HDR_LEN;
    if ((f[0] & 0xfc) == FC_QOS_DATA) hl += 2;
    if (len < hl + 8) return 0;
    const uint8_t *llc = f + hl;
    if (!(llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03)) return 0;
    size_t payload = len - hl - 8;
    if (14 + payload > outsize) return 0;
    memcpy(out, f + 16, 6);      /* DA = addr3 */
    memcpy(out + 6, f + 10, 6);  /* SA = addr2 */
    out[12] = llc[6];
    out[13] = llc[7];
    memcpy(out + 14, llc + 8, payload);
    return 14 + payload;
}

const uint8_t *wlan_find_ie(const uint8_t *ies, size_t len, uint8_t id)
{
    size_t i = 0;
    while (i + 2 <= len) {
        size_t l = ies[i + 1];
        if (i + 2 + l > len) return NULL;
        if (ies[i] == id) return ies + i;
        i += 2 + l;
    }
    return NULL;
}

int mac_is_multicast(const uint8_t *mac) { return mac[0] & 1; }

void mac_fmt(char out[18], const uint8_t *m)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int mac_parse(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return 0;
}
