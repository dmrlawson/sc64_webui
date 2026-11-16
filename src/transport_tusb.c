// SC64 byte transport over TinyUSB host + its FTDI serial driver.
//
// Reading goes through the driver's stream. Writing does not: the stream
// sends large multi-packet transfers (and zero-length packets after them),
// which stalled the link to the SC64 on the first SD card write. Data is
// instead sent one packet per transfer, like the short commands that always
// worked.

#include <string.h>

#include "tusb.h"

#include "platform.h"

#define SC64_VID 0x0403
#define SC64_PID 0x6014
#define SC64_EP_OUT 0x02    // FT232H bulk OUT endpoint, fixed by the chip
#define OUT_PACKET_SIZE 64  // full-speed bulk packet
#define WRITE_TIMEOUT_MS 5000

#define FTDI_REQUEST_GET_MODEM_STATUS 5
#define FTDI_MODEM_STATUS_DSR (1u << 5)

static volatile bool mounted;
static uint8_t cdc_index;
static uint8_t device_address;
static uint32_t generation;
static volatile bool out_busy;
static xfer_result_t out_result;

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    log_printf("usb: device attached %04x:%04x", vid, pid);
}

void tuh_umount_cb(uint8_t daddr) {
    (void)daddr;
    log_printf("usb: device removed");
}

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t info = {0};
    uint16_t vid = 0, pid = 0;
    if (!tuh_cdc_itf_get_info(idx, &info) || !tuh_vid_pid_get(info.daddr, &vid, &pid)) {
        return;
    }
    if (vid != SC64_VID || pid != SC64_PID) {
        log_printf("usb: ignoring serial device %04x:%04x", vid, pid);
        return;
    }
    cdc_index = idx;
    device_address = info.daddr;
    generation++;
    mounted = true;
    log_printf("usb: SC64 serial interface ready");
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (mounted && idx == cdc_index) {
        mounted = false;
    }
}

bool tr_connected(void) {
    return mounted;
}

uint32_t tr_generation(void) {
    return generation;
}

static void out_complete(tuh_xfer_t *xfer) {
    out_result = xfer->result;
    out_busy = false;
}

bool tr_write(const uint8_t *data, size_t len) {
    CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t packet[OUT_PACKET_SIZE];
    while (len > 0) {
        if (!mounted) {
            return false;
        }
        size_t n = len < OUT_PACKET_SIZE ? len : OUT_PACKET_SIZE;
        memcpy(packet, data, n);
        tuh_xfer_t xfer = {
            .daddr = device_address,
            .ep_addr = SC64_EP_OUT,
            .buflen = n,
            .buffer = packet,
            .complete_cb = out_complete,
        };
        out_busy = true;
        if (!tuh_edpt_xfer(&xfer)) {
            out_busy = false;
            log_printf("usb: could not start a write");
            return false;
        }
        uint32_t start = plat_millis();
        while (out_busy) {
            if (!mounted) {
                return false;
            }
            if ((plat_millis() - start) > WRITE_TIMEOUT_MS) {
                log_printf("usb: write stalled with %u bytes left", (unsigned)len);
                tuh_edpt_abort_xfer(device_address, SC64_EP_OUT);
                out_busy = false;
                return false;
            }
            plat_poll();
        }
        if (out_result != XFER_RESULT_SUCCESS) {
            log_printf("usb: write failed (%d) with %u bytes left", (int)out_result, (unsigned)len);
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

size_t tr_read(uint8_t *data, size_t max) {
    if (!mounted) {
        return 0;
    }
    return tuh_cdc_read(cdc_index, data, max);
}

void tr_purge(void) {
    // Let anything still in flight arrive, then throw it all away.
    uint32_t start = plat_millis();
    while (mounted && (plat_millis() - start) < 20) {
        plat_poll();
        tuh_cdc_read_clear(cdc_index);
    }
}

bool tr_set_dtr(bool level) {
    return mounted && tuh_cdc_set_dtr_sync(cdc_index, level) == XFER_RESULT_SUCCESS;
}

int tr_get_dsr(void) {
    if (!mounted) {
        return -1;
    }
    static uint8_t status[2];
    tusb_control_request_t request = {
        .bmRequestType_bit = {.recipient = TUSB_REQ_RCPT_DEVICE, .type = TUSB_REQ_TYPE_VENDOR, .direction = TUSB_DIR_IN},
        .bRequest = FTDI_REQUEST_GET_MODEM_STATUS,
        .wValue = 0,
        .wIndex = 1,   // FT232H channel A
        .wLength = sizeof(status),
    };
    tuh_xfer_t xfer = {
        .daddr = device_address,
        .ep_addr = 0,
        .setup = &request,
        .buffer = status,
        .complete_cb = NULL,   // NULL = blocking
        .user_data = 0,
    };
    if (!tuh_control_xfer(&xfer) || xfer.result != XFER_RESULT_SUCCESS) {
        return -1;
    }
    return (status[0] & FTDI_MODEM_STATUS_DSR) ? 1 : 0;
}
