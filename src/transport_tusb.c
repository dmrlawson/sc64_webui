// SC64 byte transport over TinyUSB host + its FTDI serial driver.

#include "tusb.h"

#include "platform.h"

#define SC64_VID 0x0403
#define SC64_PID 0x6014
#define WRITE_TIMEOUT_MS 5000

#define FTDI_REQUEST_GET_MODEM_STATUS 5
#define FTDI_MODEM_STATUS_DSR (1u << 5)

static volatile bool mounted;
static uint8_t cdc_index;
static uint8_t device_address;
static uint32_t generation;

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

bool tr_write(const uint8_t *data, size_t len) {
    uint32_t last = plat_millis();
    while (len > 0) {
        if (!mounted) {
            return false;
        }
        uint32_t n = tuh_cdc_write(cdc_index, data, len);
        if (n > 0) {
            tuh_cdc_write_flush(cdc_index);
            data += n;
            len -= n;
            last = plat_millis();
            continue;
        }
        if ((plat_millis() - last) > WRITE_TIMEOUT_MS) {
            return false;
        }
        plat_poll();
    }
    tuh_cdc_write_flush(cdc_index);
    return mounted;
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
