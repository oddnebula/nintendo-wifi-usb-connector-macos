/*
 * Minimal open-system 802.11 access point: probe/auth/assoc handling, a per-station
 * allow/deny policy (the "grant access" prompt of the original connector), power-save
 * buffering with PS-Poll, and 802.11 <-> Ethernet bridging.
 */
#include "ap.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define RATE_MGMT 10   /* 1 Mbit/s */
#define RATE_DATA 20   /* 2 Mbit/s: the fastest rate every DS supports */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

void ap_init(ap_t *ap, const uint8_t bssid[6], const char *ssid, int channel)
{
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->bss.bssid, bssid, 6);
    strncpy(ap->bss.ssid, ssid, 32);
    ap->bss.channel = channel;
    ap->bss.beacon_interval = 100;
    ap->inactivity_timeout = 300;
}

static void psq_clear(station_t *s)
{
    for (int i = 0; i < s->psq_count; i++)
        free(s->psq[(s->psq_head + i) % AP_PSQ_LEN].data);
    s->psq_head = s->psq_count = 0;
}

void ap_free(ap_t *ap)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < AP_MAX_STATIONS; i++) psq_clear(&ap->sta[i]);
    free(ap->allow);
    free(ap->deny);
    ap->allow = ap->deny = NULL;
    ap->n_allow = ap->n_deny = 0;
    pthread_mutex_unlock(&lock);
}

/* ---- helpers (lock held) ------------------------------------------------- */

static int in_list(uint8_t (*list)[6], int n, const uint8_t *mac)
{
    for (int i = 0; i < n; i++)
        if (!memcmp(list[i], mac, 6)) return i;
    return -1;
}

static void list_add(uint8_t (**list)[6], int *n, const uint8_t *mac)
{
    if (in_list(*list, *n, mac) >= 0) return;
    *list = realloc(*list, (size_t)(*n + 1) * 6);
    memcpy((*list)[*n], mac, 6);
    (*n)++;
}

static void list_del(uint8_t (*list)[6], int *n, const uint8_t *mac)
{
    int i = in_list(list, *n, mac);
    if (i < 0) return;
    memmove(list[i], list[i + 1], (size_t)(*n - i - 1) * 6);
    (*n)--;
}

static station_t *find_sta(ap_t *ap, const uint8_t *mac)
{
    for (int i = 0; i < AP_MAX_STATIONS; i++)
        if (ap->sta[i].in_use && !memcmp(ap->sta[i].mac, mac, 6)) return &ap->sta[i];
    return NULL;
}

static station_t *new_sta(ap_t *ap, const uint8_t *mac)
{
    for (int i = 0; i < AP_MAX_STATIONS; i++) {
        station_t *s = &ap->sta[i];
        if (s->in_use) continue;
        memset(s, 0, sizeof(*s));
        s->in_use = 1;
        memcpy(s->mac, mac, 6);
        s->aid = (uint16_t)(i + 1);
        s->last_seen = time(NULL);
        return s;
    }
    return NULL;
}

static sta_policy_t policy_for(ap_t *ap, const uint8_t *mac)
{
    if (in_list(ap->deny, ap->n_deny, mac) >= 0) return POLICY_DENIED;
    if (in_list(ap->allow, ap->n_allow, mac) >= 0 || ap->auto_accept) return POLICY_ALLOWED;
    return POLICY_PENDING;
}

static void notify(ap_t *ap, station_t *s, int gone)
{
    if (ap->on_station) ap->on_station(ap->station_ctx, s, gone);
    s->dirty = 0;
}

static void remove_sta(ap_t *ap, station_t *s)
{
    psq_clear(s);
    notify(ap, s, 1);
    s->in_use = 0;
}

static void send_deauth(ap_t *ap, const uint8_t *da, uint16_t reason)
{
    uint8_t f[64];
    size_t n = wlan_build_deauth(f, &ap->bss, da, 0, reason);
    ap->tx(ap->tx_ctx, f, n, RATE_MGMT, !mac_is_multicast(da));
}

static void sta_tx_frame(ap_t *ap, station_t *s, const uint8_t *frame, size_t len)
{
    if (s->ps) {
        if (s->psq_count == AP_PSQ_LEN) {   /* drop the oldest */
            free(s->psq[s->psq_head].data);
            s->psq_head = (s->psq_head + 1) % AP_PSQ_LEN;
            s->psq_count--;
        }
        psq_entry_t *e = &s->psq[(s->psq_head + s->psq_count) % AP_PSQ_LEN];
        e->data = malloc(len);
        memcpy(e->data, frame, len);
        e->len = len;
        s->psq_count++;
        return;
    }
    ap->tx(ap->tx_ctx, frame, len, RATE_DATA, 1);
    s->tx_bytes += len;
}

static void sta_send_eth(ap_t *ap, station_t *s, const uint8_t *eth, size_t len)
{
    uint8_t f[2400];
    size_t n = wlan_from_ethernet(f, sizeof f, &ap->bss, eth, len, 0);
    if (n) sta_tx_frame(ap, s, f, n);
}

static void psq_flush(ap_t *ap, station_t *s)
{
    while (s->psq_count) {
        psq_entry_t *e = &s->psq[s->psq_head];
        e->data[1] &= (uint8_t)~FCF_MOREDATA;
        ap->tx(ap->tx_ctx, e->data, e->len, RATE_DATA, 1);
        s->tx_bytes += e->len;
        free(e->data);
        s->psq_head = (s->psq_head + 1) % AP_PSQ_LEN;
        s->psq_count--;
    }
}

static void update_pm(ap_t *ap, station_t *s, uint8_t fc1)
{
    int ps = (fc1 & FCF_PM) != 0;
    if (ps == s->ps) return;
    s->ps = ps;
    log_debug("station %02x:%02x:%02x power save %s", s->mac[3], s->mac[4], s->mac[5],
              ps ? "on" : "off");
    if (!ps) psq_flush(ap, s);
}

/* ---- management ---------------------------------------------------------- */

static int ssid_matches(ap_t *ap, const uint8_t *ies, size_t len, int allow_wildcard)
{
    const uint8_t *e = wlan_find_ie(ies, len, 0);
    if (!e) return allow_wildcard;
    if (e[1] == 0) return allow_wildcard;
    return e[1] == strlen(ap->bss.ssid) && !memcmp(e + 2, ap->bss.ssid, e[1]);
}

static void handle_mgmt(ap_t *ap, const uint8_t *f, size_t len, int rssi)
{
    const uint8_t *sa = f + 10;
    const uint8_t *body = f + WLAN_HDR_LEN;
    size_t blen = len - WLAN_HDR_LEN;
    uint8_t sub = f[0] & 0xfc;
    uint8_t out[512];
    size_t n;
    char mac[18];
    mac_fmt(mac, sa);

    if (memcmp(f + 4, ap->bss.bssid, 6) && memcmp(f + 4, BCAST, 6)) return;
    station_t *s = find_sta(ap, sa);
    if (s) { s->last_seen = time(NULL); s->rssi = rssi; }

    switch (sub) {
    case FC_PROBE_REQ:
        if (policy_for(ap, sa) == POLICY_DENIED) return;
        if (!ssid_matches(ap, body, blen, 1)) return;
        n = wlan_build_probe_resp(out, &ap->bss, sa);
        ap->tx(ap->tx_ctx, out, n, RATE_MGMT, 1);
        break;

    case FC_AUTH: {
        if (blen < 6) return;
        uint16_t alg = (uint16_t)(body[0] | (body[1] << 8));
        uint16_t seq = (uint16_t)(body[2] | (body[3] << 8));
        if (seq != 1) return;
        uint16_t status = 0;
        if (alg != 0) status = 13;                              /* only open system */
        else if (policy_for(ap, sa) == POLICY_DENIED) status = 1;
        else if (!s && !(s = new_sta(ap, sa))) status = 17;     /* AP full */
        if (status == 0) {
            s->authed = 1;
            s->rssi = rssi;
            log_info("%s authenticated", mac);
        } else {
            log_info("%s authentication refused (status %u)", mac, status);
        }
        n = wlan_build_auth(out, &ap->bss, sa, alg, 2, status);
        ap->tx(ap->tx_ctx, out, n, RATE_MGMT, 1);
        break;
    }

    case FC_ASSOC_REQ:
    case FC_REASSOC_REQ: {
        int re = sub == FC_REASSOC_REQ;
        if (!s || !s->authed) { send_deauth(ap, sa, REASON_NOT_AUTHED); return; }
        size_t fixed = re ? 10 : 4;  /* capability, listen interval [, current AP] */
        if (blen < fixed) return;
        uint16_t status = ssid_matches(ap, body + fixed, blen - fixed, 0) ? 0 : 1;
        n = wlan_build_assoc_resp(out, &ap->bss, sa, re, status, s->aid);
        ap->tx(ap->tx_ctx, out, n, RATE_MGMT, 1);
        if (status) { log_warn("%s association refused: SSID mismatch", mac); return; }
        int was = s->assoc;
        s->assoc = 1;
        s->ps = 0;
        s->policy = policy_for(ap, sa);
        if (!was)
            log_info("%s associated (AID %u, %s)", mac, s->aid,
                     s->policy == POLICY_ALLOWED ? "allowed" : "waiting for approval");
        notify(ap, s, 0);
        break;
    }

    case FC_DISASSOC:
    case FC_DEAUTH:
        if (!s) return;
        log_info("%s %s", mac, sub == FC_DEAUTH ? "left (deauth)" : "left (disassoc)");
        remove_sta(ap, s);
        break;

    default:
        break;
    }
}

/* ---- data ---------------------------------------------------------------- */

static void handle_data(ap_t *ap, const uint8_t *f, size_t len, int rssi)
{
    if ((f[1] & (FCF_TODS | FCF_FROMDS)) != FCF_TODS) return;
    if (memcmp(f + 4, ap->bss.bssid, 6)) return;
    const uint8_t *sa = f + 10;
    station_t *s = find_sta(ap, sa);
    if (!s || !s->assoc) {
        send_deauth(ap, sa, REASON_NOT_ASSOCED);
        return;
    }
    s->last_seen = time(NULL);
    s->rssi = rssi;
    update_pm(ap, s, f[1]);

    uint8_t sub = f[0] & 0xfc;
    if (sub == FC_NULLFUNC || sub == FC_QOS_NULL) return;
    if (s->policy != POLICY_ALLOWED) return;

    uint8_t eth[2400];
    size_t n = wlan_to_ethernet(eth, sizeof eth, f, len);
    if (!n) return;
    s->rx_bytes += n;
    s->dirty = 1;

    if (mac_is_multicast(eth)) {
        for (int i = 0; i < AP_MAX_STATIONS; i++) {
            station_t *o = &ap->sta[i];
            if (o->in_use && o != s && o->assoc && o->policy == POLICY_ALLOWED)
                sta_send_eth(ap, o, eth, n);
        }
        ap->uplink(ap->uplink_ctx, eth, n);
        return;
    }
    station_t *dst = find_sta(ap, eth);
    if (dst && dst->assoc && dst->policy == POLICY_ALLOWED) {
        sta_send_eth(ap, dst, eth, n);   /* station-to-station within our BSS */
        return;
    }
    ap->uplink(ap->uplink_ctx, eth, n);
}

static void handle_pspoll(ap_t *ap, const uint8_t *f, size_t len)
{
    if (len < 16 || memcmp(f + 4, ap->bss.bssid, 6)) return;
    station_t *s = find_sta(ap, f + 10);
    if (!s || !s->assoc) return;
    s->last_seen = time(NULL);
    if (!s->psq_count) return;
    psq_entry_t *e = &s->psq[s->psq_head];
    s->psq_head = (s->psq_head + 1) % AP_PSQ_LEN;
    s->psq_count--;
    if (s->psq_count) e->data[1] |= FCF_MOREDATA;
    else e->data[1] &= (uint8_t)~FCF_MOREDATA;
    ap->tx(ap->tx_ctx, e->data, e->len, RATE_DATA, 1);
    s->tx_bytes += e->len;
    free(e->data);
}

/* ---- public -------------------------------------------------------------- */

void ap_handle_rx(ap_t *ap, const uint8_t *f, size_t len, int rssi)
{
    if (len < 16) return;
    pthread_mutex_lock(&lock);
    int type = FC_TYPE(f[0]);
    if (type == WLAN_TYPE_CTRL) {
        if ((f[0] & 0xfc) == FC_PSPOLL) handle_pspoll(ap, f, len);
    } else if (len >= WLAN_HDR_LEN) {
        if (type == WLAN_TYPE_MGMT) handle_mgmt(ap, f, len, rssi);
        else if (type == WLAN_TYPE_DATA) handle_data(ap, f, len, rssi);
    }
    pthread_mutex_unlock(&lock);
}

void ap_deliver_ethernet(ap_t *ap, const uint8_t *eth, size_t len)
{
    if (len < 14) return;
    pthread_mutex_lock(&lock);
    if (mac_is_multicast(eth)) {
        int any = 0;
        for (int i = 0; i < AP_MAX_STATIONS; i++)
            if (ap->sta[i].in_use && ap->sta[i].assoc && ap->sta[i].policy == POLICY_ALLOWED) any = 1;
        if (any) {
            uint8_t f[2400];
            size_t n = wlan_from_ethernet(f, sizeof f, &ap->bss, eth, len, 0);
            if (n) ap->tx(ap->tx_ctx, f, n, RATE_MGMT, 0);
        }
    } else {
        station_t *s = find_sta(ap, eth);
        if (s && s->assoc && s->policy == POLICY_ALLOWED) {
            sta_send_eth(ap, s, eth, len);
            s->dirty = 1;
        }
    }
    pthread_mutex_unlock(&lock);
}

void ap_send_beacon(ap_t *ap)
{
    uint8_t aids[AP_MAX_STATIONS];
    int n_aids = 0;
    uint8_t f[512];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < AP_MAX_STATIONS; i++)
        if (ap->sta[i].in_use && ap->sta[i].ps && ap->sta[i].psq_count)
            aids[n_aids++] = (uint8_t)ap->sta[i].aid;
    size_t n = wlan_build_beacon(f, &ap->bss, aids, n_aids);
    pthread_mutex_unlock(&lock);
    ap->tx(ap->tx_ctx, f, n, RATE_MGMT, 0);
}

void ap_housekeeping(ap_t *ap)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < AP_MAX_STATIONS; i++) {
        station_t *s = &ap->sta[i];
        if (!s->in_use) continue;
        if (now - s->last_seen > (s->assoc ? ap->inactivity_timeout : 30)) {
            char mac[18];
            mac_fmt(mac, s->mac);
            log_info("%s timed out", mac);
            send_deauth(ap, s->mac, REASON_INACTIVITY);
            remove_sta(ap, s);
        } else if (s->dirty && s->assoc) {
            notify(ap, s, 0);
        }
    }
    pthread_mutex_unlock(&lock);
}

void ap_set_policy(ap_t *ap, const uint8_t mac[6], sta_policy_t policy)
{
    char m[18];
    mac_fmt(m, mac);
    pthread_mutex_lock(&lock);
    list_del(ap->allow, &ap->n_allow, mac);
    list_del(ap->deny, &ap->n_deny, mac);
    if (policy == POLICY_ALLOWED) list_add(&ap->allow, &ap->n_allow, mac);
    if (policy == POLICY_DENIED) list_add(&ap->deny, &ap->n_deny, mac);
    station_t *s = find_sta(ap, mac);
    if (s) {
        s->policy = policy;
        if (policy == POLICY_DENIED) {
            log_info("%s denied", m);
            send_deauth(ap, mac, REASON_UNSPECIFIED);
            remove_sta(ap, s);
        } else {
            if (policy == POLICY_ALLOWED) log_info("%s granted access", m);
            notify(ap, s, 0);
        }
    }
    pthread_mutex_unlock(&lock);
}

void ap_kick(ap_t *ap, const uint8_t mac[6])
{
    pthread_mutex_lock(&lock);
    station_t *s = find_sta(ap, mac);
    if (s) {
        send_deauth(ap, mac, REASON_UNSPECIFIED);
        remove_sta(ap, s);
    }
    pthread_mutex_unlock(&lock);
}

void ap_forget(ap_t *ap, const uint8_t mac[6])
{
    pthread_mutex_lock(&lock);
    list_del(ap->allow, &ap->n_allow, mac);
    list_del(ap->deny, &ap->n_deny, mac);
    station_t *s = find_sta(ap, mac);
    if (s) {
        send_deauth(ap, mac, REASON_UNSPECIFIED);
        remove_sta(ap, s);
    }
    pthread_mutex_unlock(&lock);
}

void ap_set_auto_accept(ap_t *ap, int on)
{
    pthread_mutex_lock(&lock);
    ap->auto_accept = on;
    pthread_mutex_unlock(&lock);
}

void ap_set_station_ip(ap_t *ap, const uint8_t mac[6], uint32_t ip)
{
    pthread_mutex_lock(&lock);
    station_t *s = find_sta(ap, mac);
    if (s && s->ip != ip) {
        s->ip = ip;
        notify(ap, s, 0);
    }
    pthread_mutex_unlock(&lock);
}

void ap_shutdown(ap_t *ap)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < AP_MAX_STATIONS; i++)
        if (ap->sta[i].in_use) {
            send_deauth(ap, ap->sta[i].mac, REASON_DEAUTH_LEAVING);
            remove_sta(ap, &ap->sta[i]);
        }
    pthread_mutex_unlock(&lock);
}

void ap_foreach_station(ap_t *ap, void (*fn)(void *ctx, const station_t *sta), void *ctx)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < AP_MAX_STATIONS; i++)
        if (ap->sta[i].in_use && ap->sta[i].assoc) fn(ctx, &ap->sta[i]);
    pthread_mutex_unlock(&lock);
}
