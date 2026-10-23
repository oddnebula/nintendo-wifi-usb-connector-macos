/* Hardware smoke test: brings the radio up, beacons a test SSID, prints received frames. */
#include "rt2570.h"
#include "ieee80211.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int channel = argc > 2 ? atoi(argv[2]) : 6;
    int interval = argc > 3 ? atoi(argv[3]) : 100;
    int sw = argc > 4 && !strcmp(argv[4], "sw");
    rt2570_t dev;
    if (rt2570_open(&dev)) return 1;

    uint8_t bbp0 = rt2570_bbp_read(&dev, 0);
    printf("BBP R0 = 0x%02x, EEPROM antenna = 0x%04x\n", bbp0, dev.eeprom[EEPROM_ANTENNA]);

    if (rt2570_start_ap(&dev, channel)) return 1;

    ap_bss_t bss = { .channel = channel, .beacon_interval = interval };
    memcpy(bss.bssid, dev.mac, 6);
    snprintf(bss.ssid, sizeof bss.ssid, "WFC-HWTEST");
    uint8_t frame[512];
    size_t n = wlan_build_beacon(frame, &bss, NULL, 0);
    if (!sw) rt2570_set_beacon(&dev, frame, n, interval);
    struct timespec last = {0};

    uint8_t buf[DATA_FRAME_SIZE + RXD_DESC_SIZE];
    time_t end = time(NULL) + seconds;
    int count = 0, probes = 0;
    while (time(NULL) < end) {
        rt_rx_frame_t rx;
        if (sw) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            double ms = (now.tv_sec - last.tv_sec) * 1e3 + (now.tv_nsec - last.tv_nsec) / 1e6;
            if (ms >= interval * 1.024) {
                last = now;
                n = wlan_build_beacon(frame, &bss, NULL, 0);
                rt2570_tx(&dev, frame, n, RATE_1M, 0, 1000);
            }
        }
        int r = rt2570_rx(&dev, buf, sizeof buf, &rx, sw ? 20 : 200);
        if (r < 0) { printf("rx error %d\n", r); break; }
        if (r == 0) continue;
        count++;
        char a2[18];
        mac_fmt(a2, rx.data + 10);
        uint8_t fc = rx.data[0];
        if ((fc & 0xfc) == FC_PROBE_REQ) {
            probes++;
            const uint8_t *s = wlan_find_ie(rx.data + 24, rx.len - 24, 0);
            char ssid[33] = "";
            if (s) { memcpy(ssid, s + 2, s[1] > 32 ? 32 : s[1]); ssid[s[1] > 32 ? 32 : s[1]] = 0; }
            printf("probe req from %s ssid='%s' rssi=%d\n", a2, ssid, rx.rssi_dbm);
            size_t pn = wlan_build_probe_resp(frame, &bss, rx.data + 10);
            rt2570_tx(&dev, frame, pn, RATE_1M, 1, 1000);
        } else if (0) {
            printf("frame fc=0x%02x%02x len=%zu from %s rssi=%d\n", fc, rx.data[1], rx.len, a2, rx.rssi_dbm);
        }
    }
    uint16_t sent = 0;
    rt2570_reg_read(&dev, STA_CSR5, &sent);
    printf("received %d frames (%d probe requests); beacon counter STA_CSR5=%u\n", count, probes, sent);
    rt2570_stop(&dev);
    rt2570_close(&dev);
    return 0;
}
