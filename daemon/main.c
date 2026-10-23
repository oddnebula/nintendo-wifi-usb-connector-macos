/*
 * wfcd - Nintendo Wi-Fi USB Connector daemon for macOS.
 *
 * Drives the RT2570-based connector from userspace, runs an open access point for
 * Nintendo DS consoles, shares the Mac's internet connection through vmnet, and
 * redirects console DNS to a Nintendo WFC replacement server such as Wiimmfi.
 *
 *   wfcd --gui-socket PATH --owner UID      controlled by the WFC Connector app
 *   wfcd --ssid NAME --channel N --dns IP  headless, starts immediately
 *   wfcd --simulate ...                    no hardware: fake adapter and consoles
 */
#include "rt2570.h"
#include "ap.h"
#include "netbridge.h"
#include "netutil.h"
#include "control.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <getopt.h>
#include <arpa/inet.h>

#define DEFAULT_SSID "NintendoWFC"
#define DEFAULT_DNS  "178.62.43.212"   /* Wiimmfi */

static struct {
    pthread_mutex_t lock;           /* guards start/stop transitions */
    int simulate;
    int gui;
    int no_uplink;

    rt2570_t dev;
    int have_dev;

    ap_t ap;
    int running;
    netbridge_t *nb;
    dnsrw_t dns;
    char dns1[64], dns2[64];
    char ssid[33];
    int channel;
    int auto_accept;
    char net_info[128];

    pthread_t rx_thread, beacon_thread, sim_thread, tx_thread;
    volatile int threads_stop;
    volatile int quit;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- events to the GUI --------------------------------------------------- */

static void emit_log(int level, const char *msg)
{
    static const char *names[] = { "debug", "info", "warn", "error" };
    char esc[1200], buf[1400];
    json_escape(esc, sizeof esc, msg);
    snprintf(buf, sizeof buf, "{\"event\":\"log\",\"level\":\"%s\",\"msg\":\"%s\"}", names[level], esc);
    ctl_emit(buf);
}

static void emit_error(const char *msg)
{
    char esc[600], buf[700];
    json_escape(esc, sizeof esc, msg);
    snprintf(buf, sizeof buf, "{\"event\":\"error\",\"msg\":\"%s\"}", esc);
    ctl_emit(buf);
}

static void emit_adapter(void)
{
    char buf[512];
    if (G.simulate) {
        snprintf(buf, sizeof buf, "{\"event\":\"adapter\",\"present\":true,\"name\":\"Nintendo Wi-Fi USB "
                 "Connector (simulated)\",\"mac\":\"00:0d:0b:00:00:01\",\"rf\":\"simulated\"}");
    } else if (G.have_dev) {
        char mac[18];
        mac_fmt(mac, G.dev.mac);
        const char *name = "RT2570 adapter";
        for (const rt_usb_id_t *id = rt2570_ids; id->name; id++)
            if (id->vid == G.dev.vid && id->pid == G.dev.pid) name = id->name;
        snprintf(buf, sizeof buf, "{\"event\":\"adapter\",\"present\":true,\"name\":\"%s\",\"mac\":\"%s\","
                 "\"rf\":\"%s\",\"usb\":\"%04x:%04x\"}", name, mac, rt2570_rf_name(G.dev.rf), G.dev.vid, G.dev.pid);
    } else {
        snprintf(buf, sizeof buf, "{\"event\":\"adapter\",\"present\":false}");
    }
    ctl_emit(buf);
}

static void emit_state(void)
{
    char ssid[80], net[200], d1[100], d2[100], buf[800];
    json_escape(ssid, sizeof ssid, G.ssid);
    json_escape(net, sizeof net, G.net_info);
    json_escape(d1, sizeof d1, G.dns1);
    json_escape(d2, sizeof d2, G.dns2);
    snprintf(buf, sizeof buf, "{\"event\":\"state\",\"running\":%s,\"ssid\":\"%s\",\"channel\":%d,"
             "\"network\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\",\"auto_accept\":%s,\"dns_redirected\":%llu}",
             G.running ? "true" : "false", ssid, G.channel, net, d1, d2, G.auto_accept ? "true" : "false",
             (unsigned long long)G.dns.redirected);
    ctl_emit(buf);
}

static void emit_station(void *ctx, const station_t *s, int gone)
{
    (void)ctx;
    char mac[18], buf[512];
    mac_fmt(mac, s->mac);
    if (gone) {
        snprintf(buf, sizeof buf, "{\"event\":\"station_gone\",\"mac\":\"%s\"}", mac);
    } else {
        static const char *pol[] = { "pending", "allowed", "denied" };
        char ip[20] = "";
        if (s->ip) {
            struct in_addr a = { htonl(s->ip) };
            inet_ntop(AF_INET, &a, ip, sizeof ip);
        }
        snprintf(buf, sizeof buf, "{\"event\":\"station\",\"mac\":\"%s\",\"policy\":\"%s\",\"aid\":%u,"
                 "\"ip\":\"%s\",\"rssi\":%d,\"rx\":%llu,\"tx\":%llu,\"ps\":%s}",
                 mac, pol[s->policy], s->aid, ip, s->rssi, (unsigned long long)s->rx_bytes,
                 (unsigned long long)s->tx_bytes, s->ps ? "true" : "false");
    }
    ctl_emit(buf);
}

static void emit_station_cb(void *ctx, const station_t *s) { emit_station(ctx, s, 0); }

/* ---- plumbing between radio, AP and network ------------------------------ */

/*
 * Transmit queue. The AP never talks to USB directly: frames are queued here and a
 * dedicated thread sends them, so a slow transmit can never hold up reception (which
 * would overflow the chip's RX FIFO). Frames that sit too long are dropped as stale.
 */
#define TXQ_LEN 128
#define TXQ_MAX_AGE_MS 500
typedef struct { uint8_t data[2400]; size_t len; int rate, ack; int64_t queued_ms; } txq_item_t;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    txq_item_t items[TXQ_LEN];
    int head, count;
    int beacon_queued;
    uint64_t sent, failed, dropped;
} TXQ = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int radio_tx(void *ctx, const uint8_t *f, size_t len, int rate, int ack)
{
    (void)ctx;
    if (G.simulate || !G.have_dev || len > sizeof TXQ.items[0].data) return 0;
    int beacon = len && f[0] == FC_BEACON;
    pthread_mutex_lock(&TXQ.lock);
    if ((beacon && TXQ.beacon_queued) || TXQ.count == TXQ_LEN) {
        TXQ.dropped++;
        pthread_mutex_unlock(&TXQ.lock);
        return -1;
    }
    txq_item_t *it = &TXQ.items[(TXQ.head + TXQ.count) % TXQ_LEN];
    memcpy(it->data, f, len);
    it->len = len;
    it->rate = rate;
    it->ack = ack;
    it->queued_ms = now_ms();
    TXQ.count++;
    if (beacon) TXQ.beacon_queued = 1;
    pthread_cond_signal(&TXQ.cond);
    pthread_mutex_unlock(&TXQ.lock);
    return 0;
}

static void *tx_loop(void *arg)
{
    (void)arg;
    static txq_item_t it;
    for (;;) {
        pthread_mutex_lock(&TXQ.lock);
        while (!TXQ.count && !G.threads_stop) pthread_cond_wait(&TXQ.cond, &TXQ.lock);
        if (!TXQ.count && G.threads_stop) { pthread_mutex_unlock(&TXQ.lock); break; }
        it = TXQ.items[TXQ.head];
        TXQ.head = (TXQ.head + 1) % TXQ_LEN;
        TXQ.count--;
        if (it.len && it.data[0] == FC_BEACON) TXQ.beacon_queued = 0;
        pthread_mutex_unlock(&TXQ.lock);

        if (now_ms() - it.queued_ms > TXQ_MAX_AGE_MS) { TXQ.dropped++; continue; }
        int r = rt2570_tx(&G.dev, it.data, it.len, it.rate, it.ack, 250);
        if (r == LIBUSB_ERROR_NO_DEVICE) { G.have_dev = 0; break; }
        if (r) TXQ.failed++; else TXQ.sent++;
    }
    return NULL;
}

static void txq_reset(void)
{
    pthread_mutex_lock(&TXQ.lock);
    TXQ.head = TXQ.count = TXQ.beacon_queued = 0;
    TXQ.sent = TXQ.failed = TXQ.dropped = 0;
    pthread_mutex_unlock(&TXQ.lock);
}

static void uplink(void *ctx, const uint8_t *eth, size_t len)
{
    (void)ctx;
    uint8_t buf[2048];
    if (len > sizeof buf) return;
    memcpy(buf, eth, len);
    dnsrw_outbound(&G.dns, buf, len);
    nb_send(G.nb, buf, len);
}

static void downlink(void *ctx, uint8_t *eth, size_t len)
{
    (void)ctx;
    uint8_t mac[6];
    uint32_t ip;
    if (dhcp_snoop_ack(eth, len, mac, &ip)) ap_set_station_ip(&G.ap, mac, ip);
    dnsrw_inbound(&G.dns, eth, len);
    ap_deliver_ethernet(&G.ap, eth, len);
}

static void *rx_loop(void *arg)
{
    (void)arg;
    uint8_t buf[DATA_FRAME_SIZE + RXD_DESC_SIZE];
    while (!G.threads_stop) {
        rt_rx_frame_t rx;
        int r = rt2570_rx(&G.dev, buf, sizeof buf, &rx, 200);
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            log_err("the USB connector was unplugged");
            G.have_dev = 0;
            emit_adapter();
            break;
        }
        if (r == 1 && !rx.crc_error) ap_handle_rx(&G.ap, rx.data, rx.len, rx.rssi_dbm);
    }
    return NULL;
}

static void *beacon_loop(void *arg)
{
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int tick = 0;
    while (!G.threads_stop) {
        /* 100 TU = 102.4 ms */
        next.tv_nsec += 102400000;
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        struct timespec now, rem;
        clock_gettime(CLOCK_MONOTONIC, &now);
        rem.tv_sec = next.tv_sec - now.tv_sec;
        rem.tv_nsec = next.tv_nsec - now.tv_nsec;
        if (rem.tv_nsec < 0) { rem.tv_sec--; rem.tv_nsec += 1000000000; }
        if (rem.tv_sec >= 0) nanosleep(&rem, NULL);
        else next = now;   /* fell behind (e.g. USB stall): resynchronise */
        if (!G.simulate) ap_send_beacon(&G.ap);
        if (++tick % 10 == 0) {
            ap_housekeeping(&G.ap);
            int cca = (!G.simulate && G.have_dev) ? rt2570_tune(&G.dev) : 0;
            if (!G.simulate && G.have_dev) {
                /* LED: steady while broadcasting, activity flash while consoles exchange data */
                static uint64_t last_sent;
                rt2570_set_led(&G.dev, 1, TXQ.sent - last_sent > 12);
                last_sent = TXQ.sent;
            }
            if (tick % 50 == 0) emit_state();
            if (tick % 300 == 0 && !G.simulate) {
                static uint64_t last_sent, last_failed, last_dropped;
                log_info("radio: sent %llu, failed %llu, dropped %llu in the last 30 s; noise %d/s, VGC 0x%02x",
                         (unsigned long long)(TXQ.sent - last_sent), (unsigned long long)(TXQ.failed - last_failed),
                         (unsigned long long)(TXQ.dropped - last_dropped), cca, G.dev.r17);
                last_sent = TXQ.sent; last_failed = TXQ.failed; last_dropped = TXQ.dropped;
            }
        }
    }
    return NULL;
}

/* ---- simulation: fake DS consoles talking real 802.11 to the AP ---------- */

static void sim_frame(uint8_t fc0, uint8_t fc1, const uint8_t *sa, const uint8_t *body, size_t blen)
{
    uint8_t f[512] = { fc0, fc1 };
    memcpy(f + 4, G.ap.bss.bssid, 6);
    memcpy(f + 10, sa, 6);
    memcpy(f + 16, G.ap.bss.bssid, 6);
    memcpy(f + 24, body, blen);
    ap_handle_rx(&G.ap, f, 24 + blen, -40 - (rand() % 20));
}

static void sim_join(const uint8_t *mac)
{
    uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };
    sim_frame(FC_AUTH, 0, mac, auth, sizeof auth);
    uint8_t assoc[64] = { 0x01, 0x00, 0x0a, 0x00 };
    size_t n = 4;
    assoc[n++] = 0;
    assoc[n++] = (uint8_t)strlen(G.ap.bss.ssid);
    memcpy(assoc + n, G.ap.bss.ssid, strlen(G.ap.bss.ssid));
    n += strlen(G.ap.bss.ssid);
    sim_frame(FC_ASSOC_REQ, 0, mac, assoc, n);
}

static void *sim_loop(void *arg)
{
    (void)arg;
    uint8_t ds[2][6] = { { 0x00, 0x09, 0xbf, 0x12, 0x34, 0x56 }, { 0x00, 0x1f, 0x32, 0xab, 0xcd, 0xef } };
    int joined[2] = { 0, 0 };
    for (int t = 0; !G.threads_stop; t++) {
        sleep(1);
        for (int i = 0; i < 2; i++) {
            if (!joined[i] && t == 3 + i * 8) {
                sim_join(ds[i]);
                joined[i] = 1;
            }
            if (joined[i]) {
                uint8_t llc[8 + 64] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00 };
                uint8_t f[128] = { FC_DATA, FCF_TODS };
                memcpy(f + 4, G.ap.bss.bssid, 6);
                memcpy(f + 10, ds[i], 6);
                memset(f + 16, 0xff, 6);
                memcpy(f + 24, llc, sizeof llc);
                ap_handle_rx(&G.ap, f, 24 + sizeof llc, -45);
                if (t == 6 + i * 8) ap_set_station_ip(&G.ap, ds[i], 0xc0a84002u + (uint32_t)i);
            }
        }
    }
    return NULL;
}

/* ---- start / stop -------------------------------------------------------- */

static uint32_t parse_ip(const char *s)
{
    struct in_addr a;
    if (!s || !*s || inet_pton(AF_INET, s, &a) != 1) return 0;
    return a.s_addr;
}

static int open_adapter(void)
{
    if (G.simulate || G.have_dev) return 0;
    if (rt2570_open(&G.dev) == 0) {
        G.have_dev = 1;
        emit_adapter();
        return 0;
    }
    rt2570_close(&G.dev);
    return -1;
}

typedef struct { int n; uint8_t (*macs)[6]; } maclist_t;
static void collect_mac(void *ctx, const char *s)
{
    maclist_t *l = ctx;
    uint8_t m[6];
    if (mac_parse(s, m)) return;
    l->macs = realloc(l->macs, (size_t)(l->n + 1) * 6);
    memcpy(l->macs[l->n++], m, 6);
}

static void stop_ap(void)
{
    if (!G.running) return;
    ap_shutdown(&G.ap);
    G.threads_stop = 1;
    pthread_join(G.beacon_thread, NULL);
    if (!G.simulate) {
        pthread_join(G.rx_thread, NULL);
        pthread_mutex_lock(&TXQ.lock);
        pthread_cond_signal(&TXQ.cond);   /* the TX thread drains (deauths) and exits */
        pthread_mutex_unlock(&TXQ.lock);
        pthread_join(G.tx_thread, NULL);
    } else {
        pthread_join(G.sim_thread, NULL);
    }
    if (G.have_dev) rt2570_stop(&G.dev);
    nb_stop(G.nb);
    G.nb = NULL;
    ap_free(&G.ap);
    G.running = 0;
    G.net_info[0] = 0;
    log_info("access point stopped");
}

static int start_ap(maclist_t *allow, maclist_t *deny)
{
    if (open_adapter()) {
        emit_error("The Nintendo Wi-Fi USB Connector was not found. Plug it in and try again.");
        return -1;
    }
    uint8_t bssid[6] = { 0x00, 0x0d, 0x0b, 0x00, 0x00, 0x01 };
    if (!G.simulate) {
        if (rt2570_start_ap(&G.dev, G.channel)) {
            emit_error("The connector did not respond while starting the radio. Unplug it, plug it back in and try again.");
            return -1;
        }
        memcpy(bssid, G.dev.mac, 6);
    }
    ap_init(&G.ap, bssid, G.ssid, G.channel);
    G.ap.tx = radio_tx;
    G.ap.uplink = uplink;
    G.ap.on_station = emit_station;
    G.ap.auto_accept = G.auto_accept;
    for (int i = 0; allow && i < allow->n; i++) ap_set_policy(&G.ap, allow->macs[i], POLICY_ALLOWED);
    for (int i = 0; deny && i < deny->n; i++) ap_set_policy(&G.ap, deny->macs[i], POLICY_DENIED);

    dnsrw_set(&G.dns, parse_ip(G.dns1), parse_ip(G.dns2));

    if (G.no_uplink) {
        snprintf(G.net_info, sizeof G.net_info, "local only (internet sharing off)");
    } else if (!G.simulate) {
        if (nb_start(&G.nb, downlink, NULL, G.net_info, sizeof G.net_info)) {
            rt2570_stop(&G.dev);
            ap_free(&G.ap);
            emit_error("Could not share this Mac's internet connection (vmnet). The helper must run as root.");
            return -1;
        }
    } else {
        snprintf(G.net_info, sizeof G.net_info, "192.168.64.2 - 192.168.64.254 / 255.255.255.0 (simulated)");
    }

    G.threads_stop = 0;
    txq_reset();
    if (!G.simulate) {
        pthread_create(&G.tx_thread, NULL, tx_loop, NULL);
        pthread_create(&G.rx_thread, NULL, rx_loop, NULL);
    }
    else pthread_create(&G.sim_thread, NULL, sim_loop, NULL);
    pthread_create(&G.beacon_thread, NULL, beacon_loop, NULL);
    G.running = 1;
    log_info("access point \"%s\" running on channel %d, DNS %s%s%s", G.ssid, G.channel,
             G.dns1[0] ? G.dns1 : "(Mac default)", G.dns2[0] ? " / " : "", G.dns2);
    return 0;
}

/* ---- GUI commands -------------------------------------------------------- */

static void apply_mac_cmd(const char *line, sta_policy_t policy, int kick, int forget)
{
    char s[32];
    uint8_t m[6];
    if (!json_str(line, "mac", s, sizeof s) || mac_parse(s, m)) return;
    if (!G.running) return;
    if (forget) ap_forget(&G.ap, m);
    else if (kick) ap_kick(&G.ap, m);
    else ap_set_policy(&G.ap, m, policy);
}

static void on_command(const char *line)
{
    char cmd[32] = "";
    json_str(line, "cmd", cmd, sizeof cmd);
    pthread_mutex_lock(&G.lock);

    if (!strcmp(cmd, "hello") || !strcmp(cmd, "status")) {
        open_adapter();
        emit_adapter();
        emit_state();
        if (G.running) ap_foreach_station(&G.ap, emit_station_cb, NULL);
    } else if (!strcmp(cmd, "start")) {
        maclist_t allow = {0}, deny = {0};
        int v;
        json_str(line, "ssid", G.ssid, sizeof G.ssid);
        if (!G.ssid[0]) strcpy(G.ssid, DEFAULT_SSID);
        if (json_int(line, "channel", &v) && v >= 1 && v <= 13) G.channel = v;
        json_str(line, "dns1", G.dns1, sizeof G.dns1);
        json_str(line, "dns2", G.dns2, sizeof G.dns2);
        if (json_bool(line, "auto_accept", &v)) G.auto_accept = v;
        json_str_array(line, "allow", collect_mac, &allow);
        json_str_array(line, "deny", collect_mac, &deny);
        stop_ap();
        start_ap(&allow, &deny);
        free(allow.macs);
        free(deny.macs);
        emit_state();
    } else if (!strcmp(cmd, "stop")) {
        stop_ap();
        emit_state();
    } else if (!strcmp(cmd, "set_dns")) {
        json_str(line, "dns1", G.dns1, sizeof G.dns1);
        json_str(line, "dns2", G.dns2, sizeof G.dns2);
        dnsrw_set(&G.dns, parse_ip(G.dns1), parse_ip(G.dns2));
        log_info("DNS set to %s%s%s", G.dns1[0] ? G.dns1 : "(Mac default)", G.dns2[0] ? " / " : "", G.dns2);
        emit_state();
    } else if (!strcmp(cmd, "set_auto_accept")) {
        int v;
        if (json_bool(line, "on", &v)) {
            G.auto_accept = v;
            if (G.running) ap_set_auto_accept(&G.ap, v);
        }
        emit_state();
    } else if (!strcmp(cmd, "allow")) {
        apply_mac_cmd(line, POLICY_ALLOWED, 0, 0);
    } else if (!strcmp(cmd, "deny")) {
        apply_mac_cmd(line, POLICY_DENIED, 0, 0);
    } else if (!strcmp(cmd, "kick")) {
        apply_mac_cmd(line, POLICY_PENDING, 1, 0);
    } else if (!strcmp(cmd, "forget")) {
        apply_mac_cmd(line, POLICY_PENDING, 0, 1);
    } else if (!strcmp(cmd, "quit")) {
        G.quit = 1;
    } else if (!strcmp(cmd, "goodbye")) {
        /* GUI closed; keep running so consoles stay online until told to quit */
    }
    pthread_mutex_unlock(&G.lock);
}

static void on_signal(int sig) { (void)sig; G.quit = 1; }

static void usage(void)
{
    fprintf(stderr,
        "usage: wfcd [options]\n"
        "  --gui-socket PATH   wait for the WFC Connector app on this Unix socket\n"
        "  --owner UID         user allowed to connect to the socket\n"
        "  --ssid NAME         network name (default " DEFAULT_SSID ")\n"
        "  --channel N         2.4 GHz channel 1-13 (default 6)\n"
        "  --dns IP            DNS server for consoles (default " DEFAULT_DNS ", Wiimmfi; \"\" = Mac default)\n"
        "  --dns2 IP           optional DNS for non-Nintendo names\n"
        "  --auto-accept       let any console connect without approval\n"
        "  --simulate          no hardware; fake adapter and consoles\n"
        "  --no-internet       radio only, without vmnet internet sharing (no root needed)\n"
        "  -v                  verbose\n");
}

int main(int argc, char **argv)
{
    const char *sock = NULL;
    uid_t owner = (uid_t)-1;
    strcpy(G.ssid, DEFAULT_SSID);
    strcpy(G.dns1, DEFAULT_DNS);
    G.channel = 6;

    static struct option opts[] = {
        { "gui-socket", required_argument, 0, 'g' }, { "owner", required_argument, 0, 'o' },
        { "ssid", required_argument, 0, 's' },       { "channel", required_argument, 0, 'c' },
        { "dns", required_argument, 0, 'd' },        { "dns2", required_argument, 0, 'D' },
        { "auto-accept", no_argument, 0, 'a' },      { "simulate", no_argument, 0, 'S' },
        { "no-internet", no_argument, 0, 'N' },      { "help", no_argument, 0, 'h' },             { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
        switch (c) {
        case 'g': sock = optarg; break;
        case 'o': owner = (uid_t)atoi(optarg); break;
        case 's': strncpy(G.ssid, optarg, 32); break;
        case 'c': G.channel = atoi(optarg); break;
        case 'd': strncpy(G.dns1, optarg, sizeof G.dns1 - 1); break;
        case 'D': strncpy(G.dns2, optarg, sizeof G.dns2 - 1); break;
        case 'a': G.auto_accept = 1; break;
        case 'S': G.simulate = 1; break;
        case 'N': G.no_uplink = 1; break;
        case 'v': log_set_verbose(1); break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    dnsrw_init(&G.dns);

    if (sock) {
        G.gui = 1;
        log_set_sink(emit_log);
        if (ctl_start(sock, owner, on_command)) return 1;
        log_info("waiting for the WFC Connector app on %s", sock);
    } else {
        pthread_mutex_lock(&G.lock);
        int r = start_ap(NULL, NULL);
        pthread_mutex_unlock(&G.lock);
        if (r) return 1;
    }

    while (!G.quit) {
        sleep(1);
        if (G.simulate) continue;
        pthread_mutex_lock(&G.lock);
        if (G.running && !G.have_dev) {
            /* Unplugged while running: tear down and tell the GUI. */
            stop_ap();
            rt2570_close(&G.dev);
            emit_error("The Nintendo Wi-Fi USB Connector was unplugged.");
            emit_state();
            if (!G.gui) G.quit = 1;
        } else if (G.gui && !G.running && !G.have_dev && rt2570_present()) {
            open_adapter();   /* pick the adapter up as soon as it is plugged in */
        }
        pthread_mutex_unlock(&G.lock);
    }

    pthread_mutex_lock(&G.lock);
    stop_ap();
    pthread_mutex_unlock(&G.lock);
    if (G.gui) ctl_stop();
    if (G.have_dev) rt2570_close(&G.dev);
    log_info("bye");
    return 0;
}
