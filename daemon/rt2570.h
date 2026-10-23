/*
 * Userspace driver for the Ralink RT2570 USB 802.11b/g chip, the chip inside
 * the Nintendo Wi-Fi USB Connector (USB 0411:008b).
 *
 * Register layout and init sequences are derived from the Linux rt2500usb
 * driver (Copyright (C) 2004 - 2009 Ivo van Doorn, GPL-2.0-or-later).
 */
#ifndef RT2570_H
#define RT2570_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <libusb.h>

/* USB vendor requests */
#define USB_DEVICE_MODE   1
#define USB_SINGLE_WRITE  2
#define USB_SINGLE_READ   3
#define USB_MULTI_WRITE   6
#define USB_MULTI_READ    7
#define USB_EEPROM_WRITE  8
#define USB_EEPROM_READ   9
#define USB_RX_CONTROL    12
#define USB_MODE_TEST     4

/* RF chips */
#define RF2522  0x0000
#define RF2523  0x0001
#define RF2524  0x0002
#define RF2525  0x0003
#define RF2525E 0x0005
#define RF5222  0x0010

/* CSR registers */
#define MAC_CSR0   0x0400
#define MAC_CSR1   0x0402
#define MAC_CSR2   0x0404
#define MAC_CSR5   0x040a
#define MAC_CSR8   0x0410
#define MAC_CSR9   0x0412
#define MAC_CSR10  0x0414
#define MAC_CSR11  0x0416
#define MAC_CSR12  0x0418
#define MAC_CSR13  0x041a
#define MAC_CSR14  0x041c
#define MAC_CSR15  0x041e
#define MAC_CSR16  0x0420
#define MAC_CSR17  0x0422
#define MAC_CSR18  0x0424
#define MAC_CSR19  0x0426
#define MAC_CSR20  0x0428
#define MAC_CSR21  0x042a
#define MAC_CSR22  0x042c
#define TXRX_CSR0  0x0440
#define TXRX_CSR1  0x0442
#define TXRX_CSR2  0x0444
#define TXRX_CSR5  0x044a
#define TXRX_CSR6  0x044c
#define TXRX_CSR7  0x044e
#define TXRX_CSR8  0x0450
#define TXRX_CSR10 0x0454
#define TXRX_CSR11 0x0456
#define TXRX_CSR18 0x0464
#define TXRX_CSR19 0x0466
#define TXRX_CSR20 0x0468
#define TXRX_CSR21 0x046a
#define PHY_CSR2   0x04c4
#define PHY_CSR4   0x04c8
#define PHY_CSR5   0x04ca
#define PHY_CSR6   0x04cc
#define PHY_CSR7   0x04ce
#define PHY_CSR8   0x04d0
#define PHY_CSR9   0x04d2
#define PHY_CSR10  0x04d4
#define STA_CSR0   0x04e0
#define STA_CSR3   0x04e6
#define STA_CSR5   0x04ea

/* EEPROM word offsets */
#define EEPROM_SIZE            0x006e
#define EEPROM_MAC_ADDR_0      0x0002
#define EEPROM_ANTENNA         0x000b
#define EEPROM_NIC             0x000c
#define EEPROM_BBP_START       0x000e
#define EEPROM_BBP_SIZE        16
#define EEPROM_TXPOWER_START   0x001e
#define EEPROM_BBPTUNE_R24     0x0031
#define EEPROM_BBPTUNE_R25     0x0032
#define EEPROM_BBPTUNE_R61     0x0033
#define EEPROM_BBPTUNE_VGC     0x0034

#define TXD_DESC_SIZE 20
#define RXD_DESC_SIZE 16
#define DATA_FRAME_SIZE 2432

/* TX rates (CCK only; the DS speaks 1 and 2 Mbit/s) */
enum rt_rate { RATE_1M = 10, RATE_2M = 20, RATE_5M5 = 55, RATE_11M = 110 };

typedef struct rt2570 {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint8_t ep_in, ep_out;
    uint16_t maxpacket_out;
    uint16_t vid, pid;
    uint16_t asic_rev;
    uint16_t rf;
    uint16_t eeprom[EEPROM_SIZE / 2];
    uint8_t mac[6];
    int channel;
    int txpower;
    pthread_mutex_t csr_lock;   /* serialises indirect BBP/RF access */
    pthread_mutex_t tx_lock;    /* serialises bulk-out writes */
    uint16_t seq;               /* software sequence numbers */
    uint8_t r17, r17_min, r17_max; /* receiver sensitivity (VGC) and its tuning range */
} rt2570_t;

typedef struct rt_rx_frame {
    uint8_t *data;   /* 802.11 frame, FCS stripped */
    size_t len;
    int rssi_dbm;
    int crc_error;
    int signal;
} rt_rx_frame_t;

typedef struct rt_usb_id { uint16_t vid, pid; const char *name; } rt_usb_id_t;
extern const rt_usb_id_t rt2570_ids[];

/* True if a supported adapter is plugged in (does not open it). */
int  rt2570_present(void);

/* Opens the first supported adapter (Nintendo connector preferred). */
int  rt2570_open(rt2570_t *dev);
void rt2570_close(rt2570_t *dev);

/* Brings up the radio as an access point on `channel` with our own MAC as BSSID. */
int  rt2570_start_ap(rt2570_t *dev, int channel);
void rt2570_stop(rt2570_t *dev);
int  rt2570_set_channel(rt2570_t *dev, int channel);

/* Transmits an 802.11 frame (no FCS). `ack` requests hardware retries/ACK wait. */
int  rt2570_tx(rt2570_t *dev, const uint8_t *frame, size_t len, int rate, int ack, unsigned timeout_ms);

/* Uploads a beacon frame for the hardware beacon generator. */
int  rt2570_set_beacon(rt2570_t *dev, const uint8_t *frame, size_t len, int interval_tu);

/* Blocking receive; returns 1 when a frame was read, 0 on timeout, <0 on error. */
int  rt2570_rx(rt2570_t *dev, uint8_t *buf, size_t bufsize, rt_rx_frame_t *out, unsigned timeout_ms);

/* Adjusts receiver sensitivity from the false-CCA rate; call about once per second.
 * Returns the false-CCA count since the previous call. */
int  rt2570_tune(rt2570_t *dev);

/* Activity / link LEDs */
void rt2570_set_led(rt2570_t *dev, int link, int activity);

/* Low level helpers exposed for the probe tool */
int      rt2570_reg_read(rt2570_t *dev, uint16_t off, uint16_t *val);
int      rt2570_reg_write(rt2570_t *dev, uint16_t off, uint16_t val);
uint8_t  rt2570_bbp_read(rt2570_t *dev, uint8_t reg);
void     rt2570_bbp_write(rt2570_t *dev, uint8_t reg, uint8_t value);
const char *rt2570_rf_name(uint16_t rf);

#endif
