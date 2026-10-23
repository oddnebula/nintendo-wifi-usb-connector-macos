#include "netutil.h"

#include <string.h>
#include <strings.h>
#include <time.h>
#include <arpa/inet.h>

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void fix_checksums(uint8_t *ip, size_t ihl, uint8_t *udp, size_t udp_len)
{
    ip[10] = ip[11] = 0;
    uint16_t c = ip_checksum(ip, ihl);
    ip[10] = (uint8_t)(c >> 8);
    ip[11] = (uint8_t)c;

    if (udp[6] == 0 && udp[7] == 0) return;  /* sender did not use a UDP checksum */
    udp[6] = udp[7] = 0;
    uint32_t sum = 0;
    for (int i = 12; i < 20; i += 2) sum += (uint32_t)(ip[i] << 8 | ip[i + 1]); /* src+dst */
    sum += 17;
    sum += (uint32_t)udp_len;
    for (size_t i = 0; i + 1 < udp_len; i += 2) sum += (uint32_t)(udp[i] << 8 | udp[i + 1]);
    if (udp_len & 1) sum += (uint32_t)(udp[udp_len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t u = (uint16_t)~sum;
    if (u == 0) u = 0xffff;
    udp[6] = (uint8_t)(u >> 8);
    udp[7] = (uint8_t)u;
}

/* Locates IPv4/UDP inside an Ethernet frame. */
static int find_udp(uint8_t *eth, size_t len, uint8_t **ip, size_t *ihl, uint8_t **udp, size_t *udp_len)
{
    if (len < 14 + 20 + 8) return 0;
    if (eth[12] != 0x08 || eth[13] != 0x00) return 0;
    uint8_t *p = eth + 14;
    if ((p[0] >> 4) != 4) return 0;
    size_t hl = (size_t)(p[0] & 0x0f) * 4;
    if (hl < 20 || 14 + hl + 8 > len) return 0;
    if (p[9] != 17) return 0;
    if ((p[6] & 0x3f) || p[7]) return 0;      /* fragments */
    size_t total = (size_t)(p[2] << 8 | p[3]);
    if (total < hl + 8 || 14 + total > len) return 0;
    *ip = p;
    *ihl = hl;
    *udp = p + hl;
    *udp_len = total - hl;
    return 1;
}

static const char *NINTENDO_SUFFIXES[] = {
    "nintendowifi.net", "nintendo.net", "gamespy.com", "wiimmfi.de", NULL
};

int dns_is_nintendo_name(const char *name)
{
    size_t n = strlen(name);
    while (n && name[n - 1] == '.') n--;
    for (int i = 0; NINTENDO_SUFFIXES[i]; i++) {
        size_t s = strlen(NINTENDO_SUFFIXES[i]);
        if (n < s) continue;
        if (strncasecmp(name + n - s, NINTENDO_SUFFIXES[i], s)) continue;
        if (n == s || name[n - s - 1] == '.') return 1;
    }
    return 0;
}

int dns_qname(const uint8_t *dns, size_t len, char *out, size_t outsize)
{
    if (len < 13 || outsize == 0) return 0;
    size_t i = 12, o = 0;
    while (i < len && dns[i]) {
        size_t l = dns[i++];
        if (l > 63 || i + l > len) return 0;
        if (o && o < outsize - 1) out[o++] = '.';
        for (size_t k = 0; k < l && o < outsize - 1; k++) out[o++] = (char)dns[i + k];
        i += l;
    }
    out[o] = 0;
    return i < len;
}

void dnsrw_init(dnsrw_t *rw)
{
    memset(rw, 0, sizeof(*rw));
    pthread_mutex_init(&rw->lock, NULL);
}

void dnsrw_set(dnsrw_t *rw, uint32_t primary, uint32_t secondary)
{
    pthread_mutex_lock(&rw->lock);
    rw->primary = primary;
    rw->secondary = secondary;
    pthread_mutex_unlock(&rw->lock);
}

static int64_t now_s(void) { return (int64_t)time(NULL); }

int dnsrw_outbound(dnsrw_t *rw, uint8_t *eth, size_t len)
{
    uint8_t *ip, *udp;
    size_t ihl, ul;
    if (!find_udp(eth, len, &ip, &ihl, &udp, &ul)) return 0;
    if ((udp[2] << 8 | udp[3]) != 53 || ul < 8 + 12) return 0;

    pthread_mutex_lock(&rw->lock);
    uint32_t target = rw->primary;
    if (rw->secondary) {
        char name[256];
        if (dns_qname(udp + 8, ul - 8, name, sizeof name) && !dns_is_nintendo_name(name))
            target = rw->secondary;
    }
    uint32_t dst;
    memcpy(&dst, ip + 16, 4);
    if (!target || dst == target) { pthread_mutex_unlock(&rw->lock); return 0; }

    dns_nat_t *e = &rw->slots[rw->next];
    rw->next = (rw->next + 1) % DNS_NAT_SLOTS;
    memcpy(&e->client_ip, ip + 12, 4);
    e->client_port = (uint16_t)(udp[0] << 8 | udp[1]);
    e->txid = (uint16_t)(udp[8] << 8 | udp[9]);
    e->orig_dst = dst;
    e->new_dst = target;
    e->when = now_s();
    rw->redirected++;
    pthread_mutex_unlock(&rw->lock);

    memcpy(ip + 16, &target, 4);
    fix_checksums(ip, ihl, udp, ul);
    return 1;
}

int dnsrw_inbound(dnsrw_t *rw, uint8_t *eth, size_t len)
{
    uint8_t *ip, *udp;
    size_t ihl, ul;
    if (!find_udp(eth, len, &ip, &ihl, &udp, &ul)) return 0;
    if ((udp[0] << 8 | udp[1]) != 53 || ul < 8 + 12) return 0;

    uint32_t src, dst;
    memcpy(&src, ip + 12, 4);
    memcpy(&dst, ip + 16, 4);
    uint16_t port = (uint16_t)(udp[2] << 8 | udp[3]);
    uint16_t txid = (uint16_t)(udp[8] << 8 | udp[9]);
    int64_t now = now_s();

    pthread_mutex_lock(&rw->lock);
    dns_nat_t *hit = NULL;
    for (int i = 0; i < DNS_NAT_SLOTS; i++) {
        dns_nat_t *e = &rw->slots[i];
        if (e->new_dst == src && e->client_ip == dst && e->client_port == port &&
            e->txid == txid && now - e->when < 30) { hit = e; break; }
    }
    uint32_t orig = hit ? hit->orig_dst : 0;
    if (hit) hit->new_dst = 0;  /* one reply per query */
    pthread_mutex_unlock(&rw->lock);
    if (!hit) return 0;

    memcpy(ip + 12, &orig, 4);
    fix_checksums(ip, ihl, udp, ul);
    return 1;
}

int dhcp_snoop_ack(const uint8_t *eth, size_t len, uint8_t mac[6], uint32_t *ip_host)
{
    uint8_t *ip, *udp;
    size_t ihl, ul;
    if (!find_udp((uint8_t *)eth, len, &ip, &ihl, &udp, &ul)) return 0;
    if ((udp[0] << 8 | udp[1]) != 67 || (udp[2] << 8 | udp[3]) != 68) return 0;
    const uint8_t *b = udp + 8;
    size_t bl = ul - 8;
    if (bl < 240 || b[0] != 2) return 0;
    if (!(b[236] == 99 && b[237] == 130 && b[238] == 83 && b[239] == 99)) return 0;
    size_t i = 240;
    int type = 0;
    while (i < bl && b[i] != 255) {
        if (b[i] == 0) { i++; continue; }
        if (i + 1 >= bl) break;
        size_t l = b[i + 1];
        if (i + 2 + l > bl) break;
        if (b[i] == 53 && l == 1) type = b[i + 2];
        i += 2 + l;
    }
    if (type != 5) return 0;
    memcpy(mac, b + 28, 6);
    uint32_t yi;
    memcpy(&yi, b + 16, 4);
    *ip_host = ntohl(yi);
    return 1;
}
