/* Unit tests for the AP state machine, frame conversion and DNS/DHCP handling. */
#include "ap.h"
#include "ieee80211.h"
#include "netutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- captured radio/uplink traffic --------------------------------------- */
typedef struct { uint8_t data[2400]; size_t len; int rate, ack; } cap_t;
static cap_t txq[64]; static int ntx;
static cap_t upq[64]; static int nup;
static int station_events, gone_events;
static sta_policy_t last_policy;

static int fake_tx(void *c, const uint8_t *f, size_t len, int rate, int ack)
{ (void)c; if (ntx < 64) { memcpy(txq[ntx].data, f, len); txq[ntx].len = len; txq[ntx].rate = rate; txq[ntx].ack = ack; ntx++; } return 0; }
static void fake_up(void *c, const uint8_t *e, size_t len)
{ (void)c; if (nup < 64) { memcpy(upq[nup].data, e, len); upq[nup].len = len; nup++; } }
static void fake_sta(void *c, const station_t *s, int gone)
{ (void)c; if (gone) gone_events++; else { station_events++; last_policy = s->policy; } }

static const uint8_t BSSID[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t DS[6] = { 0x00, 0x09, 0xbf, 0x01, 0x02, 0x03 };

static size_t mk(uint8_t *f, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *sa, const uint8_t *a3,
                 const uint8_t *body, size_t blen)
{
    memset(f, 0, 24);
    f[0] = fc0; f[1] = fc1;
    memcpy(f + 4, a1, 6); memcpy(f + 10, sa, 6); memcpy(f + 16, a3, 6);
    memcpy(f + 24, body, blen);
    return 24 + blen;
}

static void join(ap_t *ap, const uint8_t *mac, const char *ssid)
{
    uint8_t f[256], body[64];
    uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };
    size_t n = mk(f, FC_AUTH, 0, BSSID, mac, BSSID, auth, 6);
    ap_handle_rx(ap, f, n, -40);
    size_t b = 0;
    body[b++] = 1; body[b++] = 0; body[b++] = 10; body[b++] = 0;
    body[b++] = 0; body[b++] = (uint8_t)strlen(ssid); memcpy(body + b, ssid, strlen(ssid)); b += strlen(ssid);
    n = mk(f, FC_ASSOC_REQ, 0, BSSID, mac, BSSID, body, b);
    ap_handle_rx(ap, f, n, -40);
}

static size_t data_frame(uint8_t *f, const uint8_t *sa, const uint8_t *da, uint8_t fc1, size_t payload)
{
    uint8_t body[1600] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00 };
    for (size_t i = 0; i < payload; i++) body[8 + i] = (uint8_t)i;
    return mk(f, FC_DATA, (uint8_t)(FCF_TODS | fc1), BSSID, sa, da, body, 8 + payload);
}

static void reset_caps(void) { ntx = nup = 0; }

static void test_frames(void)
{
    ap_bss_t bss = { .channel = 6, .beacon_interval = 100 };
    memcpy(bss.bssid, BSSID, 6);
    strcpy(bss.ssid, "NintendoWFC");
    uint8_t f[512];
    uint8_t aids[2] = { 1, 9 };
    size_t n = wlan_build_beacon(f, &bss, aids, 2);
    CHECK(f[0] == 0x80);
    const uint8_t *ies = f + 24 + 12;
    size_t il = n - 36;
    const uint8_t *e = wlan_find_ie(ies, il, 0);
    CHECK(e && e[1] == 11 && !memcmp(e + 2, "NintendoWFC", 11));
    e = wlan_find_ie(ies, il, 1);
    CHECK(e && e[2] == 0x82 && e[3] == 0x84);            /* 1 + 2 Mbit/s basic */
    e = wlan_find_ie(ies, il, 3);
    CHECK(e && e[2] == 6);
    e = wlan_find_ie(ies, il, 5);
    CHECK(e && e[1] == 3 + 2 && e[5] == 0x02 && e[6] == 0x02); /* AID 1 -> byte0 bit1, AID 9 -> byte1 bit1 */

    /* Ethernet round trip */
    uint8_t eth[64], eth2[64], wf[128];
    for (int i = 0; i < 64; i++) eth[i] = (uint8_t)(i * 3);
    eth[12] = 0x08; eth[13] = 0x06;
    size_t wl = wlan_from_ethernet(wf, sizeof wf, &bss, eth, 64, 0);
    CHECK(wl == 24 + 8 + 50);
    CHECK(wf[1] == FCF_FROMDS && !memcmp(wf + 4, eth, 6) && !memcmp(wf + 16, eth + 6, 6));
    /* turn it into a ToDS frame from the station and convert back */
    wf[1] = FCF_TODS;
    uint8_t da[6], sa[6];
    memcpy(da, wf + 4, 6); memcpy(sa, wf + 16, 6);
    memcpy(wf + 4, BSSID, 6); memcpy(wf + 10, sa, 6); memcpy(wf + 16, da, 6);
    size_t el = wlan_to_ethernet(eth2, sizeof eth2, wf, wl);
    CHECK(el == 64 && !memcmp(eth, eth2, 64));
}

static void test_ap_flow(void)
{
    ap_t ap;
    ap_init(&ap, BSSID, "NintendoWFC", 6);
    ap.tx = fake_tx; ap.uplink = fake_up; ap.on_station = fake_sta;
    uint8_t f[2048];
    reset_caps();

    /* probe request with wildcard SSID gets a probe response */
    uint8_t ps[2] = { 0, 0 };
    uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    size_t n = mk(f, FC_PROBE_REQ, 0, bc, DS, bc, ps, 2);
    ap_handle_rx(&ap, f, n, -50);
    CHECK(ntx == 1 && txq[0].data[0] == FC_PROBE_RESP && !memcmp(txq[0].data + 4, DS, 6));

    /* auth + assoc -> pending */
    reset_caps();
    join(&ap, DS, "NintendoWFC");
    CHECK(ntx == 2);
    CHECK(txq[0].data[0] == FC_AUTH && txq[0].data[24 + 2] == 2 && txq[0].data[24 + 4] == 0);
    CHECK(txq[1].data[0] == FC_ASSOC_RESP && txq[1].data[24 + 2] == 0);
    CHECK((txq[1].data[24 + 4] | (txq[1].data[24 + 5] << 8)) == (1 | 0xc000));
    CHECK(station_events == 1 && last_policy == POLICY_PENDING);

    /* data from a pending station is dropped */
    uint8_t gw[6] = { 0x02, 0, 0, 0, 0, 1 };
    reset_caps();
    n = data_frame(f, DS, gw, 0, 40);
    ap_handle_rx(&ap, f, n, -40);
    CHECK(nup == 0);

    /* allow -> data flows */
    ap_set_policy(&ap, DS, POLICY_ALLOWED);
    CHECK(last_policy == POLICY_ALLOWED);
    ap_handle_rx(&ap, f, n, -40);
    CHECK(nup == 1 && upq[0].len == 14 + 40 && !memcmp(upq[0].data, gw, 6) && !memcmp(upq[0].data + 6, DS, 6));

    /* downlink unicast to the station at 2 Mbit/s with ACK */
    uint8_t eth[100] = {0};
    memcpy(eth, DS, 6); memcpy(eth + 6, gw, 6); eth[12] = 0x08;
    reset_caps();
    ap_deliver_ethernet(&ap, eth, sizeof eth);
    CHECK(ntx == 1 && txq[0].rate == 20 && txq[0].ack == 1 && txq[0].data[1] == FCF_FROMDS);

    /* power save: frames are buffered, TIM is set, PS-Poll releases one with MoreData */
    n = mk(f, FC_NULLFUNC, FCF_TODS | FCF_PM, BSSID, DS, BSSID, NULL, 0);
    ap_handle_rx(&ap, f, n, -40);
    reset_caps();
    ap_deliver_ethernet(&ap, eth, sizeof eth);
    ap_deliver_ethernet(&ap, eth, sizeof eth);
    CHECK(ntx == 0);
    ap_send_beacon(&ap);
    CHECK(ntx == 1);
    {
        const uint8_t *ies = txq[0].data + 36;
        const uint8_t *tim = wlan_find_ie(ies, txq[0].len - 36, 5);
        CHECK(tim && (tim[5] & 0x02));
    }
    reset_caps();
    uint8_t poll[16] = { FC_PSPOLL, 0, 0x01, 0xc0 };
    memcpy(poll + 4, BSSID, 6); memcpy(poll + 10, DS, 6);
    ap_handle_rx(&ap, poll, 16, -40);
    CHECK(ntx == 1 && (txq[0].data[1] & FCF_MOREDATA));
    ap_handle_rx(&ap, poll, 16, -40);
    CHECK(ntx == 2 && !(txq[1].data[1] & FCF_MOREDATA));
    /* waking flushes nothing more */
    n = mk(f, FC_NULLFUNC, FCF_TODS, BSSID, DS, BSSID, NULL, 0);
    ap_handle_rx(&ap, f, n, -40);
    CHECK(ntx == 2);

    /* deny -> deauth and removal; new auth refused */
    reset_caps();
    ap_set_policy(&ap, DS, POLICY_DENIED);
    CHECK(ntx == 1 && txq[0].data[0] == FC_DEAUTH && gone_events == 1);
    reset_caps();
    join(&ap, DS, "NintendoWFC");
    CHECK(txq[0].data[0] == FC_AUTH && txq[0].data[24 + 4] == 1);

    /* wrong SSID on association is refused */
    uint8_t other[6] = { 0x00, 0x1f, 0x32, 9, 9, 9 };
    reset_caps();
    join(&ap, other, "SomethingElse");
    CHECK(ntx == 2 && txq[1].data[0] == FC_ASSOC_RESP && txq[1].data[24 + 2] == 1);

    /* auto-accept admits unknown consoles straight away */
    uint8_t third[6] = { 0x00, 0x1f, 0x32, 7, 7, 7 };
    ap_set_auto_accept(&ap, 1);
    join(&ap, third, "NintendoWFC");
    CHECK(last_policy == POLICY_ALLOWED);

    /* data from an unknown station triggers a deauth */
    uint8_t stranger[6] = { 0x02, 0x99, 0, 0, 0, 1 };
    reset_caps();
    n = data_frame(f, stranger, gw, 0, 10);
    ap_handle_rx(&ap, f, n, -40);
    CHECK(ntx == 1 && txq[0].data[0] == FC_DEAUTH);
    ap_free(&ap);
}

/* Builds Ethernet+IPv4+UDP with valid checksums. */
static size_t mk_udp(uint8_t *out, const char *src, const char *dst, uint16_t sport, uint16_t dport,
                     const uint8_t *payload, size_t plen)
{
    memset(out, 0, 14 + 28 + plen);
    out[12] = 0x08;
    uint8_t *ip = out + 14;
    ip[0] = 0x45; ip[2] = (uint8_t)((28 + plen) >> 8); ip[3] = (uint8_t)(28 + plen); ip[8] = 64; ip[9] = 17;
    inet_pton(AF_INET, src, ip + 12);
    inet_pton(AF_INET, dst, ip + 16);
    uint16_t c = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    uint8_t *u = ip + 20;
    u[0] = (uint8_t)(sport >> 8); u[1] = (uint8_t)sport; u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
    u[4] = (uint8_t)((8 + plen) >> 8); u[5] = (uint8_t)(8 + plen);
    memcpy(u + 8, payload, plen);
    u[6] = 0x12; u[7] = 0x34;  /* non-zero so the rewriter recomputes it */
    return 14 + 28 + plen;
}

static int udp_sum_ok(const uint8_t *eth)
{
    const uint8_t *ip = eth + 14, *u = ip + 20;
    size_t ul = (size_t)(u[4] << 8 | u[5]);
    uint32_t sum = 0;
    for (int i = 12; i < 20; i += 2) sum += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    sum += 17 + (uint32_t)ul;
    for (size_t i = 0; i + 1 < ul; i += 2) sum += (uint32_t)(u[i] << 8 | u[i + 1]);
    if (ul & 1) sum += (uint32_t)(u[ul - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum == 0xffff;
}

static void test_dns(void)
{
    CHECK(dns_is_nintendo_name("nas.nintendowifi.net"));
    CHECK(dns_is_nintendo_name("gpcm.gs.nintendowifi.net."));
    CHECK(dns_is_nintendo_name("NINTENDOWIFI.NET"));
    CHECK(!dns_is_nintendo_name("notnintendowifi.net"));
    CHECK(!dns_is_nintendo_name("example.com"));

    /* query for conntest.nintendowifi.net */
    uint8_t q[64] = { 0xbe, 0xef, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
    size_t ql = 12;
    const char *labels[] = { "conntest", "nintendowifi", "net" };
    for (int i = 0; i < 3; i++) { q[ql++] = (uint8_t)strlen(labels[i]); memcpy(q + ql, labels[i], strlen(labels[i])); ql += strlen(labels[i]); }
    q[ql++] = 0; q[ql++] = 0; q[ql++] = 1; q[ql++] = 0; q[ql++] = 1;
    char name[128];
    CHECK(dns_qname(q, ql, name, sizeof name) && !strcmp(name, "conntest.nintendowifi.net"));

    dnsrw_t rw;
    dnsrw_init(&rw);
    struct in_addr p, s;
    inet_pton(AF_INET, "178.62.43.212", &p);
    inet_pton(AF_INET, "1.1.1.1", &s);

    uint8_t pkt[200];
    size_t len = mk_udp(pkt, "192.168.64.5", "192.168.64.1", 4000, 53, q, ql);
    CHECK(dnsrw_outbound(&rw, pkt, len) == 0);             /* disabled until set */

    dnsrw_set(&rw, p.s_addr, 0);
    CHECK(dnsrw_outbound(&rw, pkt, len) == 1);
    CHECK(!memcmp(pkt + 14 + 16, &p.s_addr, 4));
    CHECK(ip_checksum(pkt + 14, 20) == 0);
    CHECK(udp_sum_ok(pkt));

    /* reply from Wiimmfi comes back addressed as the original resolver */
    uint8_t reply[200];
    q[2] = 0x81; q[3] = 0x80;
    size_t rl = mk_udp(reply, "178.62.43.212", "192.168.64.5", 53, 4000, q, ql);
    CHECK(dnsrw_inbound(&rw, reply, rl) == 1);
    char src[20];
    inet_ntop(AF_INET, reply + 14 + 12, src, sizeof src);
    CHECK(!strcmp(src, "192.168.64.1"));
    CHECK(ip_checksum(reply + 14, 20) == 0);
    CHECK(udp_sum_ok(reply));
    CHECK(dnsrw_inbound(&rw, reply, rl) == 0);            /* not rewritten twice */

    /* with a secondary set, non-Nintendo names go there */
    dnsrw_set(&rw, p.s_addr, s.s_addr);
    uint8_t q2[64] = { 0x11, 0x22, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    len = mk_udp(pkt, "192.168.64.5", "192.168.64.1", 4001, 53, q2, 29);
    CHECK(dnsrw_outbound(&rw, pkt, len) == 1 && !memcmp(pkt + 14 + 16, &s.s_addr, 4));
    len = mk_udp(pkt, "192.168.64.5", "192.168.64.1", 4002, 53, q, ql);
    CHECK(dnsrw_outbound(&rw, pkt, len) == 1 && !memcmp(pkt + 14 + 16, &p.s_addr, 4));
}

static void test_dhcp(void)
{
    uint8_t bootp[300] = {0};
    bootp[0] = 2;
    inet_pton(AF_INET, "192.168.64.7", bootp + 16);
    memcpy(bootp + 28, DS, 6);
    bootp[236] = 99; bootp[237] = 130; bootp[238] = 83; bootp[239] = 99;
    bootp[240] = 53; bootp[241] = 1; bootp[242] = 5; bootp[243] = 255;
    uint8_t pkt[400];
    size_t len = mk_udp(pkt, "192.168.64.1", "255.255.255.255", 67, 68, bootp, 244);
    uint8_t mac[6];
    uint32_t ip;
    CHECK(dhcp_snoop_ack(pkt, len, mac, &ip) == 1);
    CHECK(!memcmp(mac, DS, 6) && ip == 0xc0a84007);
    bootp[242] = 2; /* OFFER is ignored */
    len = mk_udp(pkt, "192.168.64.1", "255.255.255.255", 67, 68, bootp, 244);
    CHECK(dhcp_snoop_ack(pkt, len, mac, &ip) == 0);
}

int main(void)
{
    test_frames();
    test_ap_flow();
    test_dns();
    test_dhcp();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
