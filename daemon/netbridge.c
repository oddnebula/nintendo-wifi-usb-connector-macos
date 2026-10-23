#include "netbridge.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/uio.h>
#include <dispatch/dispatch.h>
#include <vmnet/vmnet.h>

struct netbridge {
    interface_ref iface;
    dispatch_queue_t q;
    nb_rx_fn rx;
    void *ctx;
    size_t max_packet;
};

int nb_start(netbridge_t **out, nb_rx_fn rx, void *ctx, char *info, size_t infolen)
{
    netbridge_t *nb = calloc(1, sizeof(*nb));
    nb->rx = rx;
    nb->ctx = ctx;
    nb->q = dispatch_queue_create("wfc.vmnet", DISPATCH_QUEUE_SERIAL);

    xpc_object_t desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(desc, vmnet_operation_mode_key, VMNET_SHARED_MODE);

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block vmnet_return_t status = VMNET_FAILURE;
    char start_buf[64] = "", end_buf[64] = "", mask_buf[64] = "";
    char *start = start_buf, *end = end_buf, *mask = mask_buf;
    __block uint64_t maxp = 1514;
    nb->iface = vmnet_start_interface(desc, nb->q, ^(vmnet_return_t st, xpc_object_t params) {
        status = st;
        if (st == VMNET_SUCCESS && params) {
            const char *s;
            if ((s = xpc_dictionary_get_string(params, vmnet_start_address_key))) strncpy(start, s, 63);
            if ((s = xpc_dictionary_get_string(params, vmnet_end_address_key))) strncpy(end, s, 63);
            if ((s = xpc_dictionary_get_string(params, vmnet_subnet_mask_key))) strncpy(mask, s, 63);
            maxp = xpc_dictionary_get_uint64(params, vmnet_max_packet_size_key);
        }
        dispatch_semaphore_signal(sem);
    });
    xpc_release(desc);
    if (!nb->iface) {
        log_err("vmnet_start_interface failed (the daemon must run as root)");
        free(nb);
        return -1;
    }
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (status != VMNET_SUCCESS) {
        log_err("vmnet failed to start (status %d); the daemon must run as root", status);
        free(nb);
        return -1;
    }
    nb->max_packet = maxp ? maxp : 1514;
    snprintf(info, infolen, "%s - %s / %s", start, end, mask);
    log_info("internet sharing up: consoles get %s", info);

    vmnet_interface_set_event_callback(nb->iface, VMNET_INTERFACE_PACKETS_AVAILABLE, nb->q,
                                       ^(interface_event_t ev, xpc_object_t e) {
        (void)ev; (void)e;
        enum { BATCH = 16 };
        struct vmpktdesc pk[BATCH];
        struct iovec iov[BATCH];
        uint8_t *bufs = malloc(nb->max_packet * BATCH);
        for (;;) {
            for (int i = 0; i < BATCH; i++) {
                iov[i].iov_base = bufs + i * nb->max_packet;
                iov[i].iov_len = nb->max_packet;
                pk[i].vm_pkt_size = nb->max_packet;
                pk[i].vm_pkt_iov = &iov[i];
                pk[i].vm_pkt_iovcnt = 1;
                pk[i].vm_flags = 0;
            }
            int count = BATCH;
            if (vmnet_read(nb->iface, pk, &count) != VMNET_SUCCESS || count == 0) break;
            for (int i = 0; i < count; i++)
                nb->rx(nb->ctx, iov[i].iov_base, pk[i].vm_pkt_size);
            if (count < BATCH) break;
        }
        free(bufs);
    });
    *out = nb;
    return 0;
}

void nb_send(netbridge_t *nb, const uint8_t *eth, size_t len)
{
    if (!nb || len > nb->max_packet) return;
    struct iovec iov = { .iov_base = (void *)eth, .iov_len = len };
    struct vmpktdesc pk = { .vm_pkt_size = len, .vm_pkt_iov = &iov, .vm_pkt_iovcnt = 1, .vm_flags = 0 };
    int count = 1;
    vmnet_write(nb->iface, &pk, &count);
}

void nb_stop(netbridge_t *nb)
{
    if (!nb) return;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    vmnet_interface_set_event_callback(nb->iface, VMNET_INTERFACE_PACKETS_AVAILABLE, NULL, NULL);
    vmnet_stop_interface(nb->iface, nb->q, ^(vmnet_return_t st) {
        (void)st;
        dispatch_semaphore_signal(sem);
    });
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    free(nb);
}
