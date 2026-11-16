#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "sc64.h"

// SD sectors are staged through SC64 memory. Like sc64deployer we borrow the
// top 128 kiB of SDRAM (the save region), which is only safe while the console
// is powered off - storage.c enforces that.
#define BUFFER_ADDRESS 0x03FE0000u
#define BUFFER_SECTORS 256u

#define IO_TIMEOUT_MS 5000
#define RESET_TIMEOUT_MS 1000
#define RELINK_INTERVAL_MS 1000

static bool synced;
static uint32_t synced_generation;
static uint32_t last_link_attempt;
static bool link_attempted;
static char version[24] = "?";

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool rx_exact(uint8_t *buf, size_t len) {
    uint32_t last = plat_millis();
    while (len > 0) {
        size_t n = tr_read(buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
            last = plat_millis();
            continue;
        }
        if (!tr_connected() || (plat_millis() - last) > IO_TIMEOUT_MS) {
            return false;
        }
        plat_poll();
    }
    return true;
}

static bool rx_discard(size_t len) {
    uint8_t scratch[64];
    while (len > 0) {
        size_t n = len < sizeof(scratch) ? len : sizeof(scratch);
        if (!rx_exact(scratch, n)) {
            return false;
        }
        len -= n;
    }
    return true;
}

// Execute one command. Returns 0 on CMP, 1 on ERR, -1 on link failure (the
// link is then marked for a reset). Up to rx_cap response bytes are stored.
static int exec(uint8_t id, uint32_t arg0, uint32_t arg1, const uint8_t *tx, size_t tx_len,
                uint8_t *rx, size_t rx_cap, size_t *rx_len) {
    if (!synced) {
        return -1;   // the link already failed; sc64_link() has to bring it back first
    }
    uint8_t header[12] = {'C', 'M', 'D', id};
    put_be32(&header[4], arg0);
    put_be32(&header[8], arg1);
    if (!tr_write(header, sizeof(header)) || (tx_len > 0 && !tr_write(tx, tx_len))) {
        log_printf("sc64: could not send command '%c'", id);
        synced = false;
        return -1;
    }
    for (;;) {
        uint8_t head[8];
        if (!rx_exact(head, sizeof(head))) {
            log_printf("sc64: no response to command '%c'", id);
            break;
        }
        uint32_t length = get_be32(&head[4]);
        if (memcmp(head, "PKT", 3) == 0) {
            // Asynchronous packet from the N64 side; nothing here consumes them.
            if (!rx_discard(length)) {
                break;
            }
            continue;
        }
        bool error = (memcmp(head, "ERR", 3) == 0);
        if ((!error && memcmp(head, "CMP", 3) != 0) || head[3] != id) {
            log_printf("sc64: unexpected response %02x %02x %02x %02x to '%c'", head[0], head[1],
                       head[2], head[3], id);
            break;
        }
        size_t keep = length < rx_cap ? length : rx_cap;
        if (!rx_exact(rx, keep) || !rx_discard(length - keep)) {
            break;
        }
        if (rx_len) {
            *rx_len = keep;
        }
        return error ? 1 : 0;
    }
    synced = false;
    return -1;
}

static bool wait_dsr(int level) {
    uint32_t start = plat_millis();
    for (;;) {
        int dsr = tr_get_dsr();
        if (dsr == level) {
            return true;
        }
        if (dsr < 0) {
            // Can't read DSR: give the SC64 a generous fixed time instead.
            while ((plat_millis() - start) < 100) {
                plat_poll();
            }
            return true;
        }
        if ((plat_millis() - start) > RESET_TIMEOUT_MS) {
            return false;
        }
        plat_poll();
    }
}

static bool reset_link(void) {
    if (!tr_set_dtr(true) || !wait_dsr(1)) {
        tr_set_dtr(false);
        return false;
    }
    tr_purge();
    if (!tr_set_dtr(false) || !wait_dsr(0)) {
        return false;
    }
    tr_purge();
    return true;
}

bool sc64_link(void) {
    if (!tr_connected()) {
        synced = false;
        link_attempted = false;
        return false;
    }
    if (synced && synced_generation == tr_generation()) {
        return true;
    }
    if (link_attempted && (plat_millis() - last_link_attempt) < RELINK_INTERVAL_MS) {
        return false;
    }
    link_attempted = true;
    last_link_attempt = plat_millis();
    synced = false;
    uint32_t generation = tr_generation();

    if (!reset_link()) {
        log_printf("sc64: link reset failed");
        return false;
    }
    synced = true;   // exec() clears it again on failure
    uint8_t data[8];
    size_t len = 0;
    if (exec('v', 0, 0, NULL, 0, data, 4, &len) != 0 || len != 4 || memcmp(data, "SCv2", 4) != 0) {
        log_printf("sc64: device did not identify as SCv2");
        synced = false;
        return false;
    }
    if (exec('V', 0, 0, NULL, 0, data, 8, &len) != 0 || len != 8) {
        synced = false;
        return false;
    }
    snprintf(version, sizeof(version), "%u.%u.%lu", (unsigned)((data[0] << 8) | data[1]),
             (unsigned)((data[2] << 8) | data[3]), (unsigned long)get_be32(&data[4]));
    synced_generation = generation;
    log_printf("sc64: connected, firmware %s", version);
    return true;
}

const char *sc64_version(void) {
    return version;
}

bool sc64_console_on(bool *on) {
    uint8_t data[8];
    size_t len = 0;
    if (!sc64_link() || exec('?', 0, 0, NULL, 0, data, sizeof(data), &len) != 0 || len != 8) {
        return false;
    }
    // CIC step: 0 = unavailable, 1 = power off, anything else means the
    // console is driving the cartridge.
    uint8_t cic_step = (data[7] >> 4) & 0x0F;
    *on = (cic_step > 1);
    return true;
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

static uint8_t to_bcd(unsigned v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool sc64_time_get(sc64_time_t *t) {
    uint8_t d[8];
    size_t len = 0;
    if (!sc64_link() || exec('t', 0, 0, NULL, 0, d, sizeof(d), &len) != 0 || len != 8) {
        return false;
    }
    t->weekday = from_bcd(d[0]);
    t->hour = from_bcd(d[1]);
    t->minute = from_bcd(d[2]);
    t->second = from_bcd(d[3]);
    t->year = (uint16_t)(1900 + (from_bcd(d[4]) * 100) + from_bcd(d[5]));
    t->month = from_bcd(d[6]);
    t->day = from_bcd(d[7]);
    return true;
}

bool sc64_time_set(const sc64_time_t *t) {
    unsigned year = t->year >= 1900 ? t->year - 1900u : 0;
    uint32_t arg0 = ((uint32_t)to_bcd(t->weekday) << 24) | ((uint32_t)to_bcd(t->hour) << 16) |
                    ((uint32_t)to_bcd(t->minute) << 8) | to_bcd(t->second);
    uint32_t arg1 = ((uint32_t)to_bcd(year / 100) << 24) | ((uint32_t)to_bcd(year % 100) << 16) |
                    ((uint32_t)to_bcd(t->month) << 8) | to_bcd(t->day);
    return sc64_link() && exec('T', arg0, arg1, NULL, 0, NULL, 0, NULL) == 0;
}

static bool sd_op(uint32_t op, uint32_t *result, uint32_t *status) {
    uint8_t d[8];
    size_t len = 0;
    if (!sc64_link() || exec('i', 0, op, NULL, 0, d, sizeof(d), &len) < 0 || len != 8) {
        return false;
    }
    if (result) {
        *result = get_be32(&d[0]);
    }
    if (status) {
        *status = get_be32(&d[4]);
    }
    return true;
}

// Operation codes follow sd_op_t in the controller firmware (sd.h). Note that
// docs/03_usb_interface.md lists init and deinit the other way round.
bool sc64_sd_init(uint32_t *result) {
    return sd_op(1, result, NULL);
}

bool sc64_sd_deinit(void) {
    return sd_op(0, NULL, NULL);
}

bool sc64_sd_status(uint32_t *status) {
    return sd_op(2, NULL, status);
}

static bool sd_transfer(uint8_t id, uint32_t sector, uint32_t count) {
    uint8_t arg[4], d[4];
    size_t len = 0;
    put_be32(arg, sector);
    int r = exec(id, BUFFER_ADDRESS, count, arg, sizeof(arg), d, sizeof(d), &len);
    if (r != 0) {
        if (r > 0 && len == 4) {
            log_printf("sc64: SD %s error %lu at sector %lu", id == 's' ? "read" : "write",
                       (unsigned long)get_be32(d), (unsigned long)sector);
        }
        return false;
    }
    return true;
}

bool sc64_sd_read(uint8_t *data, uint32_t sector, uint32_t count) {
    while (count > 0) {
        uint32_t n = count < BUFFER_SECTORS ? count : BUFFER_SECTORS;
        size_t bytes = (size_t)n * SC64_SECTOR_SIZE, len = 0;
        if (!sd_transfer('s', sector, n)) {
            return false;
        }
        if (exec('m', BUFFER_ADDRESS, (uint32_t)bytes, NULL, 0, data, bytes, &len) != 0 || len != bytes) {
            return false;
        }
        data += bytes;
        sector += n;
        count -= n;
    }
    return true;
}

bool sc64_sd_write(const uint8_t *data, uint32_t sector, uint32_t count) {
    while (count > 0) {
        uint32_t n = count < BUFFER_SECTORS ? count : BUFFER_SECTORS;
        size_t bytes = (size_t)n * SC64_SECTOR_SIZE;
        if (exec('M', BUFFER_ADDRESS, (uint32_t)bytes, data, bytes, NULL, 0, NULL) != 0) {
            return false;
        }
        if (!sd_transfer('S', sector, n)) {
            return false;
        }
        data += bytes;
        sector += n;
        count -= n;
    }
    return true;
}
