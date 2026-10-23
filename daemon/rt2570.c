/*
 * Userspace RT2570 driver over libusb. See rt2570.h for provenance.
 */
#include "rt2570.h"
#include "log.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

#define REGISTER_TIMEOUT   500
#define EEPROM_TIMEOUT     2000
#define BUSY_COUNT         20
#define BUSY_DELAY_US      100

const rt_usb_id_t rt2570_ids[] = {
    { 0x0411, 0x008b, "Nintendo Wi-Fi USB Connector" },
    { 0x0411, 0x005e, "Buffalo WLI-U2-KG54" },
    { 0x0411, 0x0066, "Buffalo WLI-U2-KG54-AI" },
    { 0x0411, 0x0067, "Buffalo WLI-U2-KG54-YB" },
    { 0x0411, 0x0097, "Buffalo WLI-U2-KG54-BB" },
    { 0x148f, 0x1706, "Ralink RT2570" },
    { 0x148f, 0x2570, "Ralink RT2570" },
    { 0x148f, 0x9020, "Ralink RT2570" },
    { 0x0b05, 0x1706, "ASUS WL-167g" },
    { 0x0b05, 0x1707, "ASUS WL-167g" },
    { 0x050d, 0x7050, "Belkin F5D7050" },
    { 0x050d, 0x7051, "Belkin F5D7051" },
    { 0x13b1, 0x000d, "Linksys WUSB54G" },
    { 0x13b1, 0x0011, "Linksys WUSB54GP" },
    { 0x13b1, 0x001a, "Linksys HU200TS" },
    { 0x14b2, 0x3c02, "Conceptronic C54RU" },
    { 0x2001, 0x3c00, "D-Link DWL-G122" },
    { 0x1044, 0x8001, "Gigabyte GN-WBKG" },
    { 0x1044, 0x8007, "Gigabyte GN-WBKG" },
    { 0x06f8, 0xe000, "Hercules HWGUSB2-54" },
    { 0x0db0, 0x6861, "MSI" }, { 0x0db0, 0x6865, "MSI" }, { 0x0db0, 0x6869, "MSI" },
    { 0x079b, 0x004b, "Sagem" },
    { 0x0681, 0x3c06, "Siemens" },
    { 0x0707, 0xee13, "SMC" },
    { 0x114b, 0x0110, "Spairon" },
    { 0x0769, 0x11f3, "Surecom" },
    { 0x0eb0, 0x9020, "Trust" },
    { 0x0f88, 0x3012, "VTech" },
    { 0x5a57, 0x0260, "Zinwell" },
    { 0, 0, NULL }
};

/* rf1, rf2, rf3, rf4 for channels 1..14 */
typedef struct { uint32_t rf1, rf2, rf3, rf4; } rf_vals_t;

static const rf_vals_t rf_2522[14] = {
    {0x00002050,0x000c1fda,0x00000101,0},{0x00002050,0x000c1fee,0x00000101,0},
    {0x00002050,0x000c2002,0x00000101,0},{0x00002050,0x000c2016,0x00000101,0},
    {0x00002050,0x000c202a,0x00000101,0},{0x00002050,0x000c203e,0x00000101,0},
    {0x00002050,0x000c2052,0x00000101,0},{0x00002050,0x000c2066,0x00000101,0},
    {0x00002050,0x000c207a,0x00000101,0},{0x00002050,0x000c208e,0x00000101,0},
    {0x00002050,0x000c20a2,0x00000101,0},{0x00002050,0x000c20b6,0x00000101,0},
    {0x00002050,0x000c20ca,0x00000101,0},{0x00002050,0x000c20fa,0x00000101,0},
};
static const rf_vals_t rf_2523[14] = {
    {0x00022010,0x00000c9e,0x000e0111,0x00000a1b},{0x00022010,0x00000ca2,0x000e0111,0x00000a1b},
    {0x00022010,0x00000ca6,0x000e0111,0x00000a1b},{0x00022010,0x00000caa,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cae,0x000e0111,0x00000a1b},{0x00022010,0x00000cb2,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cb6,0x000e0111,0x00000a1b},{0x00022010,0x00000cba,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cbe,0x000e0111,0x00000a1b},{0x00022010,0x00000d02,0x000e0111,0x00000a1b},
    {0x00022010,0x00000d06,0x000e0111,0x00000a1b},{0x00022010,0x00000d0a,0x000e0111,0x00000a1b},
    {0x00022010,0x00000d0e,0x000e0111,0x00000a1b},{0x00022010,0x00000d1a,0x000e0111,0x00000a03},
};
static const rf_vals_t rf_2524[14] = {
    {0x00032020,0x00000c9e,0x00000101,0x00000a1b},{0x00032020,0x00000ca2,0x00000101,0x00000a1b},
    {0x00032020,0x00000ca6,0x00000101,0x00000a1b},{0x00032020,0x00000caa,0x00000101,0x00000a1b},
    {0x00032020,0x00000cae,0x00000101,0x00000a1b},{0x00032020,0x00000cb2,0x00000101,0x00000a1b},
    {0x00032020,0x00000cb6,0x00000101,0x00000a1b},{0x00032020,0x00000cba,0x00000101,0x00000a1b},
    {0x00032020,0x00000cbe,0x00000101,0x00000a1b},{0x00032020,0x00000d02,0x00000101,0x00000a1b},
    {0x00032020,0x00000d06,0x00000101,0x00000a1b},{0x00032020,0x00000d0a,0x00000101,0x00000a1b},
    {0x00032020,0x00000d0e,0x00000101,0x00000a1b},{0x00032020,0x00000d1a,0x00000101,0x00000a03},
};
static const rf_vals_t rf_2525[14] = {
    {0x00022020,0x00080c9e,0x00060111,0x00000a1b},{0x00022020,0x00080ca2,0x00060111,0x00000a1b},
    {0x00022020,0x00080ca6,0x00060111,0x00000a1b},{0x00022020,0x00080caa,0x00060111,0x00000a1b},
    {0x00022020,0x00080cae,0x00060111,0x00000a1b},{0x00022020,0x00080cb2,0x00060111,0x00000a1b},
    {0x00022020,0x00080cb6,0x00060111,0x00000a1b},{0x00022020,0x00080cba,0x00060111,0x00000a1b},
    {0x00022020,0x00080cbe,0x00060111,0x00000a1b},{0x00022020,0x00080d02,0x00060111,0x00000a1b},
    {0x00022020,0x00080d06,0x00060111,0x00000a1b},{0x00022020,0x00080d0a,0x00060111,0x00000a1b},
    {0x00022020,0x00080d0e,0x00060111,0x00000a1b},{0x00022020,0x00080d1a,0x00060111,0x00000a03},
};
static const rf_vals_t rf_2525e[14] = {
    {0x00022010,0x0000089a,0x00060111,0x00000e1b},{0x00022010,0x0000089e,0x00060111,0x00000e07},
    {0x00022010,0x0000089e,0x00060111,0x00000e1b},{0x00022010,0x000008a2,0x00060111,0x00000e07},
    {0x00022010,0x000008a2,0x00060111,0x00000e1b},{0x00022010,0x000008a6,0x00060111,0x00000e07},
    {0x00022010,0x000008a6,0x00060111,0x00000e1b},{0x00022010,0x000008aa,0x00060111,0x00000e07},
    {0x00022010,0x000008aa,0x00060111,0x00000e1b},{0x00022010,0x000008ae,0x00060111,0x00000e07},
    {0x00022010,0x000008ae,0x00060111,0x00000e1b},{0x00022010,0x000008b2,0x00060111,0x00000e07},
    {0x00022010,0x000008b2,0x00060111,0x00000e1b},{0x00022010,0x000008b6,0x00060111,0x00000e23},
};
static const rf_vals_t rf_5222[14] = {
    {0x00022020,0x00001136,0x00000101,0x00000a0b},{0x00022020,0x0000113a,0x00000101,0x00000a0b},
    {0x00022020,0x0000113e,0x00000101,0x00000a0b},{0x00022020,0x00001182,0x00000101,0x00000a0b},
    {0x00022020,0x00001186,0x00000101,0x00000a0b},{0x00022020,0x0000118a,0x00000101,0x00000a0b},
    {0x00022020,0x0000118e,0x00000101,0x00000a0b},{0x00022020,0x00001192,0x00000101,0x00000a0b},
    {0x00022020,0x00001196,0x00000101,0x00000a0b},{0x00022020,0x0000119a,0x00000101,0x00000a0b},
    {0x00022020,0x0000119e,0x00000101,0x00000a0b},{0x00022020,0x000011a2,0x00000101,0x00000a0b},
    {0x00022020,0x000011a6,0x00000101,0x00000a0b},{0x00022020,0x000011ae,0x00000101,0x00000a1b},
};

static inline uint16_t set_field16(uint16_t reg, uint16_t mask, uint16_t val)
{
    return (uint16_t)((reg & ~mask) | ((val << __builtin_ctz(mask)) & mask));
}
static inline uint16_t get_field16(uint16_t reg, uint16_t mask)
{
    return (uint16_t)((reg & mask) >> __builtin_ctz(mask));
}
static inline uint32_t set_field32(uint32_t reg, uint32_t mask, uint32_t val)
{
    return (reg & ~mask) | ((val << __builtin_ctz(mask)) & mask);
}

const char *rt2570_rf_name(uint16_t rf)
{
    switch (rf) {
    case RF2522: return "RF2522";
    case RF2523: return "RF2523";
    case RF2524: return "RF2524";
    case RF2525: return "RF2525";
    case RF2525E: return "RF2525E";
    case RF5222: return "RF5222";
    default: return "unknown";
    }
}

/* ---- register access ---------------------------------------------------- */

static int vendor_in(rt2570_t *d, uint8_t req, uint16_t off, void *buf, uint16_t len, unsigned to)
{
    for (int i = 0; i < 3; i++) {
        int r = libusb_control_transfer(d->h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                        LIBUSB_RECIPIENT_DEVICE, req, 0, off, buf, len, to);
        if (r == len) return 0;
        if (r == LIBUSB_ERROR_NO_DEVICE) return r;
    }
    return -EIO;
}

static int vendor_out(rt2570_t *d, uint8_t req, uint16_t off, uint16_t value, void *buf, uint16_t len)
{
    for (int i = 0; i < 3; i++) {
        int r = libusb_control_transfer(d->h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                        LIBUSB_RECIPIENT_DEVICE, req, value, off, buf, len,
                                        REGISTER_TIMEOUT);
        if (r == len) return 0;
        if (r == LIBUSB_ERROR_NO_DEVICE) return r;
    }
    return -EIO;
}

int rt2570_reg_read(rt2570_t *d, uint16_t off, uint16_t *val)
{
    uint8_t b[2] = {0};
    int r = vendor_in(d, USB_MULTI_READ, off, b, 2, REGISTER_TIMEOUT);
    *val = (uint16_t)(b[0] | (b[1] << 8));
    return r;
}

int rt2570_reg_write(rt2570_t *d, uint16_t off, uint16_t val)
{
    uint8_t b[2] = { (uint8_t)(val & 0xff), (uint8_t)(val >> 8) };
    return vendor_out(d, USB_MULTI_WRITE, off, 0, b, 2);
}

static uint16_t rd(rt2570_t *d, uint16_t off)
{
    uint16_t v = 0;
    rt2570_reg_read(d, off, &v);
    return v;
}
#define wr rt2570_reg_write

static int multiwrite(rt2570_t *d, uint16_t off, const void *buf, uint16_t len)
{
    return vendor_out(d, USB_MULTI_WRITE, off, 0, (void *)buf, len);
}

static int wait_not_busy(rt2570_t *d, uint16_t off, uint16_t busy_mask, uint16_t *reg)
{
    for (int i = 0; i < BUSY_COUNT; i++) {
        *reg = rd(d, off);
        if (!(*reg & busy_mask)) return 1;
        usleep(BUSY_DELAY_US);
    }
    log_warn("indirect register 0x%04x stuck busy (0x%04x)", off, *reg);
    *reg = 0xffff;
    return 0;
}

void rt2570_bbp_write(rt2570_t *d, uint8_t reg_id, uint8_t value)
{
    uint16_t reg;
    pthread_mutex_lock(&d->csr_lock);
    if (wait_not_busy(d, PHY_CSR8, 0x0001, &reg)) {
        reg = (uint16_t)(value | ((reg_id & 0x7f) << 8));
        wr(d, PHY_CSR7, reg);
    }
    pthread_mutex_unlock(&d->csr_lock);
}

uint8_t rt2570_bbp_read(rt2570_t *d, uint8_t reg_id)
{
    uint16_t reg;
    pthread_mutex_lock(&d->csr_lock);
    if (wait_not_busy(d, PHY_CSR8, 0x0001, &reg)) {
        reg = (uint16_t)(((reg_id & 0x7f) << 8) | 0x8000);
        wr(d, PHY_CSR7, reg);
        if (wait_not_busy(d, PHY_CSR8, 0x0001, &reg))
            reg = rd(d, PHY_CSR7);
    }
    pthread_mutex_unlock(&d->csr_lock);
    return (uint8_t)(reg & 0xff);
}

static void rf_write(rt2570_t *d, uint32_t value)
{
    uint16_t reg;
    pthread_mutex_lock(&d->csr_lock);
    if (wait_not_busy(d, PHY_CSR10, 0x8000, &reg)) {
        wr(d, PHY_CSR9, (uint16_t)(value & 0xffff));
        reg = 0;
        reg = set_field16(reg, 0x00ff, (uint16_t)((value >> 16) & 0xff));
        reg = set_field16(reg, 0x1f00, 20);     /* number of bits */
        reg = set_field16(reg, 0x2000, 0);      /* RF, not IF */
        reg = set_field16(reg, 0x8000, 1);      /* busy */
        wr(d, PHY_CSR10, reg);
    }
    pthread_mutex_unlock(&d->csr_lock);
}

/* ---- device discovery ---------------------------------------------------- */

static const char *known_name(uint16_t vid, uint16_t pid)
{
    for (const rt_usb_id_t *id = rt2570_ids; id->name; id++)
        if (id->vid == vid && id->pid == pid) return id->name;
    return NULL;
}

int rt2570_present(void)
{
    libusb_context *ctx;
    if (libusb_init(&ctx)) return 0;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor dd;
        if (!libusb_get_device_descriptor(list[i], &dd) && known_name(dd.idVendor, dd.idProduct))
            found = 1;
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}

int rt2570_open(rt2570_t *d)
{
    memset(d, 0, sizeof(*d));
    pthread_mutex_init(&d->csr_lock, NULL);
    pthread_mutex_init(&d->tx_lock, NULL);

    int r = libusb_init(&d->ctx);
    if (r) { log_err("libusb_init: %s", libusb_strerror(r)); return r; }

    libusb_device **list;
    ssize_t n = libusb_get_device_list(d->ctx, &list);
    libusb_device *pick = NULL;
    int pick_is_nintendo = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd)) continue;
        if (!known_name(dd.idVendor, dd.idProduct)) continue;
        int nin = (dd.idVendor == 0x0411 && dd.idProduct == 0x008b);
        if (!pick || (nin && !pick_is_nintendo)) {
            pick = list[i];
            pick_is_nintendo = nin;
            d->vid = dd.idVendor;
            d->pid = dd.idProduct;
        }
    }
    if (!pick) {
        libusb_free_device_list(list, 1);
        log_err("no RT2570 adapter found (plug in the Nintendo Wi-Fi USB Connector)");
        return -ENODEV;
    }

    r = libusb_open(pick, &d->h);
    if (r) {
        libusb_free_device_list(list, 1);
        log_err("libusb_open: %s", libusb_strerror(r));
        return r;
    }

    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(pick, &cfg) == 0) {
        const struct libusb_interface_descriptor *ifd = &cfg->interface[0].altsetting[0];
        for (int i = 0; i < ifd->bNumEndpoints; i++) {
            const struct libusb_endpoint_descriptor *ep = &ifd->endpoint[i];
            if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                if (!d->ep_in) d->ep_in = ep->bEndpointAddress;
            } else if (!d->ep_out) {
                d->ep_out = ep->bEndpointAddress;
                d->maxpacket_out = ep->wMaxPacketSize;
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_free_device_list(list, 1);
    if (!d->ep_in || !d->ep_out) { log_err("bulk endpoints not found"); return -ENODEV; }
    if (!d->maxpacket_out) d->maxpacket_out = 64;

    libusb_set_auto_detach_kernel_driver(d->h, 1);
    r = libusb_claim_interface(d->h, 0);
    if (r) { log_err("claim interface: %s", libusb_strerror(r)); return r; }

    /* EEPROM */
    uint8_t ee[EEPROM_SIZE];
    r = vendor_in(d, USB_EEPROM_READ, 0, ee, EEPROM_SIZE, EEPROM_TIMEOUT);
    if (r) { log_err("EEPROM read failed (%d)", r); return r; }
    for (int i = 0; i < EEPROM_SIZE / 2; i++)
        d->eeprom[i] = (uint16_t)(ee[2 * i] | (ee[2 * i + 1] << 8));
    memcpy(d->mac, &ee[EEPROM_MAC_ADDR_0 * 2], 6);

    d->asic_rev = rd(d, MAC_CSR0);
    if ((d->asic_rev & 0xfff0) != 0 || (d->asic_rev & 0x000f) == 0) {
        log_err("not an RT2570 (MAC_CSR0=0x%04x)", d->asic_rev);
        return -ENODEV;
    }
    uint16_t ant = d->eeprom[EEPROM_ANTENNA];
    d->rf = (ant == 0xffff) ? RF2522 : get_field16(ant, 0xf800);

    /* rfkill GPIO as input, like the Linux driver */
    wr(d, MAC_CSR19, set_field16(rd(d, MAC_CSR19), 0x0100, 0));

    log_info("found %s (%04x:%04x) rev %d, %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, ep in 0x%02x out 0x%02x/%u",
             known_name(d->vid, d->pid), d->vid, d->pid, d->asic_rev, rt2570_rf_name(d->rf),
             d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
             d->ep_in, d->ep_out, d->maxpacket_out);
    return 0;
}

void rt2570_close(rt2570_t *d)
{
    if (d->h) {
        libusb_release_interface(d->h, 0);
        libusb_close(d->h);
        d->h = NULL;
    }
    if (d->ctx) { libusb_exit(d->ctx); d->ctx = NULL; }
}

/* ---- init ---------------------------------------------------------------- */

static int set_state_awake(rt2570_t *d)
{
    const uint16_t state = 3; /* awake */
    uint16_t reg = 0;
    reg = set_field16(reg, 0x0006, state);
    reg = set_field16(reg, 0x0018, state);
    reg = set_field16(reg, 0x0200, 0);
    wr(d, MAC_CSR17, reg);
    reg = set_field16(reg, 0x0001, 1);
    wr(d, MAC_CSR17, reg);
    for (int i = 0; i < BUSY_COUNT; i++) {
        uint16_t r2 = rd(d, MAC_CSR17);
        if (get_field16(r2, 0x0060) == state && get_field16(r2, 0x0180) == state) return 0;
        wr(d, MAC_CSR17, reg);
        usleep(30000);
    }
    log_err("device did not wake up");
    return -EBUSY;
}

static int init_registers(rt2570_t *d)
{
    uint16_t reg;

    vendor_out(d, USB_DEVICE_MODE, 0x0001, USB_MODE_TEST, NULL, 0);
    vendor_out(d, USB_SINGLE_WRITE, 0x0308, 0x00f0, NULL, 0);

    wr(d, TXRX_CSR2, set_field16(rd(d, TXRX_CSR2), 0x0001, 1));
    wr(d, MAC_CSR13, 0x1111);
    wr(d, MAC_CSR14, 0x1e11);

    reg = rd(d, MAC_CSR1);
    reg = set_field16(reg, 0x0001, 1);
    reg = set_field16(reg, 0x0002, 1);
    reg = set_field16(reg, 0x0004, 0);
    wr(d, MAC_CSR1, reg);
    reg = rd(d, MAC_CSR1);
    reg = set_field16(reg, 0x0001, 0);
    reg = set_field16(reg, 0x0002, 0);
    reg = set_field16(reg, 0x0004, 0);
    wr(d, MAC_CSR1, reg);

    /* BBP IDs: TXRX_CSR5..8 = (id0 | valid) | (id1 | valid) << 8 */
    wr(d, TXRX_CSR5, (uint16_t)((13 | 0x80) | ((12 | 0x80) << 8)));
    wr(d, TXRX_CSR6, (uint16_t)((10 | 0x80) | ((11 | 0x80) << 8)));
    wr(d, TXRX_CSR7, (uint16_t)((7 | 0x80) | ((6 | 0x80) << 8)));
    wr(d, TXRX_CSR8, (uint16_t)(5 | 0x80));

    wr(d, TXRX_CSR19, 0);
    wr(d, TXRX_CSR21, 0xe78f);
    wr(d, MAC_CSR9, 0xff1d);

    if (set_state_awake(d)) return -EBUSY;

    reg = rd(d, MAC_CSR1);
    reg = set_field16(reg, 0x0001, 0);
    reg = set_field16(reg, 0x0002, 0);
    reg = set_field16(reg, 0x0004, 1);
    wr(d, MAC_CSR1, reg);

    if (d->asic_rev >= 3) {
        reg = set_field16(rd(d, PHY_CSR2), 0x0002, 0);
    } else {
        reg = 0;
        reg = set_field16(reg, 0x0002, 1);
        reg = set_field16(reg, 0x3000, 3);
    }
    wr(d, PHY_CSR2, reg);

    wr(d, MAC_CSR11, 0x0002);
    wr(d, MAC_CSR22, 0x0053);
    wr(d, MAC_CSR15, 0x01ee);
    wr(d, MAC_CSR16, 0x0000);

    wr(d, MAC_CSR8, set_field16(rd(d, MAC_CSR8), 0x0fff, DATA_FRAME_SIZE));

    reg = rd(d, TXRX_CSR0);
    reg = set_field16(reg, 0x0007, 0);   /* no cipher */
    reg = set_field16(reg, 0x01f8, 24);  /* IV offset = 802.11 header */
    reg = set_field16(reg, 0x1e00, 0);
    wr(d, TXRX_CSR0, reg);

    wr(d, MAC_CSR18, set_field16(rd(d, MAC_CSR18), 0x00ff, 90));
    wr(d, PHY_CSR4, set_field16(rd(d, PHY_CSR4), 0x0001, 1));
    wr(d, TXRX_CSR1, set_field16(rd(d, TXRX_CSR1), 0x8000, 1));
    return 0;
}

static int init_bbp(rt2570_t *d)
{
    int ok = 0;
    for (int i = 0; i < BUSY_COUNT; i++) {
        uint8_t v = rt2570_bbp_read(d, 0);
        if (v != 0xff && v != 0x00) { ok = 1; break; }
        usleep(BUSY_DELAY_US);
    }
    if (!ok) { log_err("BBP not responding"); return -EIO; }

    static const uint8_t init[][2] = {
        {3,0x02},{4,0x19},{14,0x1c},{15,0x30},{16,0xac},{18,0x18},{19,0xff},{20,0x1e},
        {21,0x08},{22,0x08},{23,0x08},{24,0x80},{25,0x50},{26,0x08},{27,0x23},{30,0x10},
        {31,0x2b},{32,0xb9},{34,0x12},{35,0x50},{39,0xc4},{40,0x02},{41,0x60},{53,0x10},
        {54,0x18},{56,0x08},{57,0x10},{58,0x08},{61,0x60},{62,0x10},{75,0xff},
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++)
        rt2570_bbp_write(d, init[i][0], init[i][1]);

    for (int i = 0; i < EEPROM_BBP_SIZE; i++) {
        uint16_t e = d->eeprom[EEPROM_BBP_START + i];
        if (e != 0xffff && e != 0x0000)
            rt2570_bbp_write(d, (uint8_t)(e >> 8), (uint8_t)(e & 0xff));
    }
    return 0;
}

static void config_antenna(rt2570_t *d)
{
    uint16_t ee = d->eeprom[EEPROM_ANTENNA];
    int tx = (ee == 0xffff) ? 0 : get_field16(ee, 0x000c);
    int rx = (ee == 0xffff) ? 0 : get_field16(ee, 0x0030);
    /* EEPROM: 0 = diversity, 1 = A, 2 = B. Register: 0 = A, 1 = diversity, 2 = B. */
    static const uint8_t map[4] = { 1, 0, 2, 2 };

    uint8_t r2 = rt2570_bbp_read(d, 2);
    uint8_t r14 = rt2570_bbp_read(d, 14);
    uint16_t csr5 = rd(d, PHY_CSR5);
    uint16_t csr6 = rd(d, PHY_CSR6);

    r2 = (uint8_t)((r2 & ~0x03) | map[tx & 3]);
    csr5 = set_field16(csr5, 0x0003, map[tx & 3]);
    csr6 = set_field16(csr6, 0x0003, map[tx & 3]);
    r14 = (uint8_t)((r14 & ~0x03) | map[rx & 3]);

    if (d->rf == RF2525E || d->rf == RF5222) {
        r2 |= 0x04;
        csr5 = set_field16(csr5, 0x0004, 1);
        csr6 = set_field16(csr6, 0x0004, 1);
        if (d->rf == RF2525E) r14 &= (uint8_t)~0x04;
    } else {
        csr5 = set_field16(csr5, 0x0004, 0);
        csr6 = set_field16(csr6, 0x0004, 0);
    }
    rt2570_bbp_write(d, 2, r2);
    rt2570_bbp_write(d, 14, r14);
    wr(d, PHY_CSR5, csr5);
    wr(d, PHY_CSR6, csr6);
}

#define EEPROM_BBPTUNE_R17 0x0035

/*
 * Receiver sensitivity. With the VGC value Linux uses (EEPROM VGC upper, 0x38 on the
 * Nintendo connector) the receiver hears so much noise that the MAC sees the channel as
 * busy and transmissions stall for up to a second. Measured on the connector: from 0x48
 * false CCA drops ~3x and TX latency falls from ~1 s to ~3 ms, while frames at -70 dBm are
 * still received; above 0x58 weak stations start to disappear. So start at the EEPROM's
 * R17 tuning value (0x46 here) and let rt2570_tune() move within [start, 0x58].
 */
static void reset_tuner(rt2570_t *d)
{
    uint16_t e;
    e = d->eeprom[EEPROM_BBPTUNE_R24]; rt2570_bbp_write(d, 24, e == 0xffff ? 0x40 : (e & 0xff));
    e = d->eeprom[EEPROM_BBPTUNE_R25]; rt2570_bbp_write(d, 25, e == 0xffff ? 0x40 : (e & 0xff));
    e = d->eeprom[EEPROM_BBPTUNE_R61]; rt2570_bbp_write(d, 61, e == 0xffff ? 0x60 : (e & 0xff));

    e = d->eeprom[EEPROM_BBPTUNE_R17];
    uint8_t start = (uint8_t)(e & 0xff);
    if (e == 0xffff || start < 0x40 || start > 0x58) start = 0x48;
    if (start < 0x4c) start = 0x4c;   /* below this the connector stalled in a busy 2.4 GHz band */
    d->r17 = d->r17_min = start;
    d->r17_max = 0x58;
    rt2570_bbp_write(d, 17, d->r17);
    uint16_t junk;
    rt2570_reg_read(d, STA_CSR3, &junk);  /* clear the false-CCA counter */
}

int rt2570_tune(rt2570_t *d)
{
    uint16_t cca = 0;
    if (rt2570_reg_read(d, STA_CSR3, &cca)) return -1;
    uint8_t r17 = d->r17;
    if (cca > 1500 && r17 + 2 <= d->r17_max) r17 += 2;
    else if (cca < 300 && r17 - 2 >= d->r17_min) r17 -= 2;
    if (r17 != d->r17) {
        d->r17 = r17;
        rt2570_bbp_write(d, 17, r17);
        log_debug("false CCA %u/s, receiver VGC now 0x%02x", cca, r17);
    }
    return cca;
}

static int channel_txpower(rt2570_t *d, int channel)
{
    const uint8_t *bytes = (const uint8_t *)&d->eeprom[EEPROM_TXPOWER_START];
    uint8_t p = bytes[channel - 1];
    return p > 31 ? 24 : p;
}

int rt2570_set_channel(rt2570_t *d, int channel)
{
    if (channel < 1 || channel > 14) return -EINVAL;
    const rf_vals_t *tbl;
    switch (d->rf) {
    case RF2523: tbl = rf_2523; break;
    case RF2524: tbl = rf_2524; break;
    case RF2525: tbl = rf_2525; break;
    case RF2525E: tbl = rf_2525e; break;
    case RF5222: tbl = rf_5222; break;
    default: tbl = rf_2522; break;
    }
    rf_vals_t rf = tbl[channel - 1];
    d->txpower = channel_txpower(d, channel);
    rf.rf3 = set_field32(rf.rf3, 0x00003e00, (uint32_t)d->txpower);

    if (d->rf == RF2525E) {
        static const uint32_t half[14] = {
            0x000008aa, 0x000008ae, 0x000008ae, 0x000008b2, 0x000008b2, 0x000008b6, 0x000008b6,
            0x000008ba, 0x000008ba, 0x000008be, 0x000008b7, 0x00000902, 0x00000902, 0x00000906
        };
        rf_write(d, half[channel - 1]);
        if (rf.rf4) rf_write(d, rf.rf4);
    }
    rf_write(d, rf.rf1);
    rf_write(d, rf.rf2);
    rf_write(d, rf.rf3);
    if (rf.rf4) rf_write(d, rf.rf4);
    d->channel = channel;
    return 0;
}

int rt2570_start_ap(rt2570_t *d, int channel)
{
    uint16_t reg;
    if (init_registers(d) || init_bbp(d)) return -EIO;

    /* ERP for 802.11b, long preamble, basic rates 1 + 2 Mbit/s */
    wr(d, MAC_CSR10, 20);                 /* slot time */
    wr(d, MAC_CSR11, 10);                 /* SIFS */
    wr(d, MAC_CSR12, 10 + 50 + 304);      /* EIFS = SIFS + DIFS + ACK@1M (with preamble) */
    wr(d, TXRX_CSR10, set_field16(rd(d, TXRX_CSR10), 0x0004, 0));
    wr(d, TXRX_CSR11, 0x0003);

    config_antenna(d);
    if (rt2570_set_channel(d, channel)) return -EINVAL;
    reset_tuner(d);

    /* Our MAC doubles as the BSSID */
    multiwrite(d, MAC_CSR2, d->mac, 6);
    multiwrite(d, MAC_CSR5, d->mac, 6);

    /* RX filter for an access point: keep PS-Poll (control) and ToDS frames */
    reg = rd(d, TXRX_CSR2);
    reg = set_field16(reg, 0x0002, 1);  /* drop CRC errors */
    reg = set_field16(reg, 0x0004, 1);  /* drop PHY errors */
    reg = set_field16(reg, 0x0008, 0);  /* keep control */
    reg = set_field16(reg, 0x0010, 1);  /* drop not-to-me unicast */
    reg = set_field16(reg, 0x0020, 0);  /* keep ToDS */
    reg = set_field16(reg, 0x0040, 1);  /* drop version errors */
    reg = set_field16(reg, 0x0200, 0);  /* keep multicast */
    reg = set_field16(reg, 0x0400, 0);  /* keep broadcast */
    reg = set_field16(reg, 0x0001, 0);  /* enable RX */
    wr(d, TXRX_CSR2, reg);

    /* Run the TSF timer in AP mode so beacons/probe responses carry a live timestamp */
    reg = rd(d, TXRX_CSR19);
    reg = set_field16(reg, 0x0006, 3);
    reg = set_field16(reg, 0x0001, 1);
    wr(d, TXRX_CSR19, reg);

    rt2570_set_led(d, 1, 0);
    log_info("radio up as AP on channel %d (tx power %d)", channel, d->txpower);
    return 0;
}

void rt2570_stop(rt2570_t *d)
{
    if (!d->h) return;
    rt2570_set_led(d, 0, 0);
    wr(d, TXRX_CSR2, set_field16(rd(d, TXRX_CSR2), 0x0001, 1));
    wr(d, MAC_CSR13, 0x2121);
    wr(d, MAC_CSR14, 0x2121);
    wr(d, TXRX_CSR19, 0);
    vendor_out(d, USB_RX_CONTROL, 0, 0, NULL, 0);
}

void rt2570_set_led(rt2570_t *d, int link, int activity)
{
    /* The Nintendo connector's LED is active-high: confirmed on hardware, it stays dark
     * unless the polarity bit is set. */
    uint16_t reg = 0x0004;
    reg = set_field16(reg, 0x0002, link ? 1 : 0);
    reg = set_field16(reg, 0x0001, activity ? 1 : 0);
    wr(d, MAC_CSR20, reg);
}

/* ---- TX ------------------------------------------------------------------ */

static size_t build_txd(rt2570_t *d, uint8_t *out, const uint8_t *frame, size_t len,
                        int rate, int ack, int timestamp)
{
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    size_t data_len = len + 4; /* + FCS */
    unsigned duration = (unsigned)(data_len * 8 * 10 / (unsigned)rate);
    unsigned residual = (unsigned)(data_len * 8 * 10 % (unsigned)rate);
    uint8_t service = 0x04;
    if (residual) {
        duration++;
        if (rate == RATE_11M && residual <= 30) service |= 0x80;
    }
    /* The chip wants the rt2x00 CCK rate code here, not the 802.11b SIGNAL value. */
    uint8_t signal = rate == RATE_11M ? 0x03 : rate == RATE_5M5 ? 0x02 : rate == RATE_2M ? 0x01 : 0x00;

    w0 = set_field32(w0, 0x000000f0, ack ? 7 : 0);   /* retry limit */
    w0 = set_field32(w0, 0x00000200, ack ? 1 : 0);
    w0 = set_field32(w0, 0x00000400, timestamp ? 1 : 0);
    w0 = set_field32(w0, 0x00001000, 1);             /* new sequence (first fragment) */
    w0 = set_field32(w0, 0x00006000, 0);             /* IFS_BACKOFF */
    w0 = set_field32(w0, 0x0fff0000, (uint32_t)len);
    w1 = set_field32(w1, 0x0000003f, 24);
    w1 = set_field32(w1, 0x000000c0, 2);             /* AIFS */
    w1 = set_field32(w1, 0x00000f00, 5);             /* CWmin */
    w1 = set_field32(w1, 0x0000f000, 10);            /* CWmax */
    w2 = set_field32(w2, 0x000000ff, signal);
    w2 = set_field32(w2, 0x0000ff00, service);
    w2 = set_field32(w2, 0x00ff0000, duration & 0xff);
    w2 = set_field32(w2, 0xff000000, (duration >> 8) & 0xff);

    memset(out, 0, TXD_DESC_SIZE);
    uint32_t w[3] = { w0, w1, w2 };
    for (int i = 0; i < 3; i++) {
        out[i * 4 + 0] = (uint8_t)(w[i]);
        out[i * 4 + 1] = (uint8_t)(w[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(w[i] >> 24);
    }
    memcpy(out + TXD_DESC_SIZE, frame, len);

    /* Length must be even and not a multiple of the USB packet size. */
    size_t total = TXD_DESC_SIZE + len;
    if (total & 1) out[total++] = 0;
    if (total % d->maxpacket_out == 0) { out[total++] = 0; out[total++] = 0; }
    return total;
}

static void stamp_seq(rt2570_t *d, uint8_t *frame, size_t len)
{
    if (len < 24) return;
    uint8_t type = (frame[0] >> 2) & 3;
    if (type == 1) return; /* control frames carry no sequence number */
    uint16_t sc = (uint16_t)((d->seq++ & 0x0fff) << 4);
    frame[22] = (uint8_t)(sc & 0xff);
    frame[23] = (uint8_t)(sc >> 8);
}

int rt2570_tx(rt2570_t *d, const uint8_t *frame, size_t len, int rate, int ack, unsigned timeout_ms)
{
    if (len > DATA_FRAME_SIZE) return -EMSGSIZE;
    uint8_t buf[TXD_DESC_SIZE + DATA_FRAME_SIZE + 4];
    pthread_mutex_lock(&d->tx_lock);
    size_t total = build_txd(d, buf, frame, len, rate, ack,
                             len > 0 && ((frame[0] & 0xfc) == 0x50 || (frame[0] & 0xfc) == 0x80));
    stamp_seq(d, buf + TXD_DESC_SIZE, len);
    int sent = 0;
    int r = libusb_bulk_transfer(d->h, d->ep_out, buf, (int)total, &sent, timeout_ms);
    pthread_mutex_unlock(&d->tx_lock);
    if (r) { log_debug("bulk TX failed: %s", libusb_strerror(r)); return r; }
    return 0;
}

int rt2570_set_beacon(rt2570_t *d, const uint8_t *frame, size_t len, int interval_tu)
{
    uint16_t reg;
    uint8_t buf[TXD_DESC_SIZE + DATA_FRAME_SIZE + 4];

    pthread_mutex_lock(&d->tx_lock);

    /* Beacon timing: interval in 1/16 TU units >> 2, preload offset, expect window */
    reg = rd(d, TXRX_CSR18);
    reg = set_field16(reg, 0x000f, 0);
    reg = set_field16(reg, 0xfff0, (uint16_t)(interval_tu * 4));
    wr(d, TXRX_CSR18, reg);
    unsigned preload = 144 + (24 * 8 * 10) / 20;
    reg = rd(d, TXRX_CSR20);
    reg = set_field16(reg, 0x1fff, (uint16_t)(preload >> 6));
    reg = set_field16(reg, 0xe000, 2);
    wr(d, TXRX_CSR20, reg);

    /* Like Linux: the beacon queue is already running (TSF count + TBCN on) when the
     * frame is loaded; only the generator itself is paused during the upload. */
    reg = rd(d, TXRX_CSR19);
    reg = set_field16(reg, 0x0006, 3);  /* TSF sync: AP */
    reg = set_field16(reg, 0x0001, 1);
    reg = set_field16(reg, 0x0008, 1);
    reg = set_field16(reg, 0x0010, 1);
    wr(d, TXRX_CSR19, reg);
    reg = set_field16(reg, 0x0010, 0);  /* stop beacon generator while reloading */
    wr(d, TXRX_CSR19, reg);

    size_t total = build_txd(d, buf, frame, len, RATE_1M, 0, 1);
    stamp_seq(d, buf + TXD_DESC_SIZE, len);

    /* Guardian byte, then the beacon itself (see rt2500usb_write_beacon). */
    uint8_t guard = 0;
    int sent = 0, r;
    r = libusb_bulk_transfer(d->h, d->ep_out, &guard, 1, &sent, 1000);
    if (!r) r = libusb_bulk_transfer(d->h, d->ep_out, buf, (int)total, &sent, 1000);

    reg = set_field16(reg, 0x0001, 1);  /* TSF count */
    reg = set_field16(reg, 0x0008, 1);  /* TBCN */
    uint16_t reg0 = reg;
    reg = set_field16(reg, 0x0010, 1);  /* beacon gen */
    /* The generator needs a few toggles before it starts reliably. */
    wr(d, TXRX_CSR19, reg);
    wr(d, TXRX_CSR19, reg0);
    wr(d, TXRX_CSR19, reg);
    wr(d, TXRX_CSR19, reg0);
    wr(d, TXRX_CSR19, reg);

    pthread_mutex_unlock(&d->tx_lock);
    if (r) log_warn("beacon upload failed: %s", libusb_strerror(r));
    return r;
}

/* ---- RX ------------------------------------------------------------------ */

int rt2570_rx(rt2570_t *d, uint8_t *buf, size_t bufsize, rt_rx_frame_t *out, unsigned timeout_ms)
{
    int got = 0;
    int r = libusb_bulk_transfer(d->h, d->ep_in, buf, (int)bufsize, &got, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r) return r;
    if (got < RXD_DESC_SIZE) return 0;

    const uint8_t *rxd = buf + got - RXD_DESC_SIZE;
    uint32_t w0 = (uint32_t)(rxd[0] | (rxd[1] << 8) | (rxd[2] << 16) | ((uint32_t)rxd[3] << 24));
    uint32_t w1 = (uint32_t)(rxd[4] | (rxd[5] << 8) | (rxd[6] << 16) | ((uint32_t)rxd[7] << 24));
    size_t size = (w0 >> 16) & 0x0fff;
    if (size < 4 || size > (size_t)(got - RXD_DESC_SIZE)) return 0;

    out->data = buf;
    out->len = size - 4; /* strip FCS */
    out->crc_error = (w0 & 0x20) || (w0 & 0x80);
    out->rssi_dbm = (int)(w1 & 0xff) - 120;
    out->signal = (int)((w1 >> 8) & 0xff);
    return 1;
}
