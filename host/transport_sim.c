// In-process simulation of a SummerCart64 on the other end of the USB cable,
// backed by an SD card image file. Lets the whole firmware above the
// transport layer run and be tested on a PC.
//
// Behaviour is steered by marker files in the working directory:
//   console_on    - the N64 is powered on (and holds the SD lock)
//   no_card       - SD slot is empty
//   usb_unplugged - the SC64 is not connected
//   usb_stuck     - nothing gets through until the device restarts
//   cart_hung     - the SC64 does not answer, whatever we do
// SIM_KBPS=<n> throttles SD transfers to roughly n kB/s.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

#define MEMORY_LENGTH 0x05002C80u
#define SD_LOCKED 30

static int sd_fd = -1;
static uint8_t *memory;
static bool dtr, dsr;
static bool sd_initialized, usb_lock;
static bool was_connected;
static uint32_t generation;

static uint8_t *in_buf;
static size_t in_len, in_cap;
static uint8_t *out_buf;
static size_t out_len, out_pos, out_cap;

static bool marker(const char *name) {
    struct stat st;
    return stat(name, &st) == 0;
}

void sim_init(int fd) {
    sd_fd = fd;
    memory = calloc(1, MEMORY_LENGTH);
}

static void throttle(size_t bytes) {
    const char *kbps = getenv("SIM_KBPS");
    if (kbps && atoi(kbps) > 0) {
        usleep((useconds_t)((uint64_t)bytes * 1000 / (uint64_t)atoi(kbps)));
    }
}

static void emit(const void *data, size_t len) {
    if (out_len + len > out_cap) {
        out_cap = (out_len + len) * 2;
        out_buf = realloc(out_buf, out_cap);
    }
    memcpy(out_buf + out_len, data, len);
    out_len += len;
}

static void emit_be32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    emit(b, 4);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void respond(uint8_t id, bool error, const void *data, size_t len) {
    if (marker("console_on")) {
        // Sprinkle in asynchronous packets, as a running game might.
        emit("PKTI", 4);
        emit_be32(6);
        emit("hello\n", 6);
    }
    emit(error ? "ERR" : "CMP", 3);
    emit(&id, 1);
    emit_be32((uint32_t)len);
    emit(data, len);
}

static void respond_words(uint8_t id, bool error, const uint32_t *words, int count) {
    uint8_t b[16];
    for (int i = 0; i < count; i++) {
        b[i * 4] = (uint8_t)(words[i] >> 24);
        b[i * 4 + 1] = (uint8_t)(words[i] >> 16);
        b[i * 4 + 2] = (uint8_t)(words[i] >> 8);
        b[i * 4 + 3] = (uint8_t)words[i];
    }
    respond(id, error, b, (size_t)count * 4);
}

static bool bad_range(uint32_t address, uint64_t length) {
    return address >= MEMORY_LENGTH || (address + length) > MEMORY_LENGTH;
}

static uint32_t sd_status(void) {
    return (marker("no_card") ? 0 : 1u) | (sd_initialized ? 2u : 0);
}

// Returns bytes consumed, or 0 if the command is not complete yet.
static size_t process(const uint8_t *p, size_t len) {
    if (len < 12) {
        return 0;
    }
    if (memcmp(p, "CMD", 3) != 0) {
        return 1;   // resync byte by byte like the real firmware
    }
    uint8_t id = p[3];
    uint32_t arg0 = be32(p + 4), arg1 = be32(p + 8);
    size_t extra = (id == 'M') ? arg1 : ((id == 's' || id == 'S') ? 4 : 0);
    if (len < 12 + extra) {
        return 0;
    }
    const uint8_t *data = p + 12;
    bool n64_lock = marker("console_on");

    switch (id) {
        case 'v':
            respond(id, false, "SCv2", 4);
            break;
        case 'V': {
            uint32_t w[2] = {(2u << 16) | 20, 2};
            respond_words(id, false, w, 2);
            break;
        }
        case '?': {
            uint32_t w[2] = {0, marker("console_on") ? 0x70 : 0x10};
            respond_words(id, false, w, 2);
            break;
        }
        case 't': {
            static const uint8_t t[8] = {0x04, 0x21, 0x43, 0x05, 0x01, 0x26, 0x11, 0x12};
            respond(id, false, t, 8);
            break;
        }
        case 'T':
            printf("sim: RTC set %08x %08x\n", arg0, arg1);
            respond(id, false, NULL, 0);
            break;
        case 'i': {
            uint32_t error = 0;
            if (arg1 == 1) {          // init
                if (n64_lock) {
                    error = SD_LOCKED;
                } else if (marker("no_card")) {
                    error = 1;
                } else {
                    usleep(150000);
                    usb_lock = true;
                    sd_initialized = true;
                }
            } else if (arg1 == 0) {   // deinit
                if (n64_lock) {
                    error = SD_LOCKED;
                } else {
                    sd_initialized = false;
                    usb_lock = false;
                }
            } else if (arg1 != 2) {
                error = 5;
            }
            uint32_t w[2] = {error, sd_status()};
            respond_words(id, error != 0, w, 2);
            break;
        }
        case 's':
        case 'S': {
            uint32_t sector = be32(data), error = 0;
            uint64_t bytes = (uint64_t)arg1 * 512;
            if (bad_range(arg0, bytes)) {
                error = 4;
            } else if (!usb_lock) {
                error = SD_LOCKED;
            } else if (!sd_initialized || marker("no_card")) {
                error = 2;
            } else {
                ssize_t r = (id == 's') ? pread(sd_fd, memory + arg0, bytes, (off_t)sector * 512)
                                        : pwrite(sd_fd, memory + arg0, bytes, (off_t)sector * 512);
                if (r != (ssize_t)bytes) {
                    error = (id == 's') ? 20 : 23;
                }
                throttle(bytes);
            }
            respond_words(id, error != 0, &error, 1);
            break;
        }
        case 'm':
            if (bad_range(arg0, arg1)) {
                respond(id, true, NULL, 0);
            } else {
                respond(id, false, memory + arg0, arg1);
            }
            break;
        case 'M':
            if (bad_range(arg0, arg1)) {
                respond(id, true, NULL, 0);
            } else {
                memcpy(memory + arg0, data, arg1);
                respond(id, false, NULL, 0);
            }
            break;
        default: {
            uint32_t w = 0xFFFFFFFF;
            respond_words(id, true, &w, 1);
            break;
        }
    }
    return 12 + extra;
}

// What a restart of the device does to the USB side.
void sim_restart(void) {
    unlink("usb_stuck");
    was_connected = false;   // comes back as a fresh connection
    dtr = dsr = false;
}

bool tr_connected(void) {
    bool connected = !marker("usb_unplugged");
    if (connected && !was_connected) {
        generation++;
        in_len = out_len = out_pos = 0;
        sd_initialized = usb_lock = false;
    }
    was_connected = connected;
    return connected;
}

uint32_t tr_generation(void) {
    return generation;
}

bool tr_write(const uint8_t *data, size_t len) {
    if (!tr_connected()) {
        return false;
    }
    if (dtr || marker("usb_stuck") || marker("cart_hung")) {
        return true;   // interface held in reset or not listening: data is dropped
    }
    if (in_len + len > in_cap) {
        in_cap = (in_len + len) * 2;
        in_buf = realloc(in_buf, in_cap);
    }
    memcpy(in_buf + in_len, data, len);
    in_len += len;
    size_t pos = 0, used;
    while ((used = process(in_buf + pos, in_len - pos)) > 0) {
        pos += used;
    }
    memmove(in_buf, in_buf + pos, in_len - pos);
    in_len -= pos;
    return true;
}

size_t tr_read(uint8_t *data, size_t max) {
    size_t n = out_len - out_pos;
    if (n > max) {
        n = max;
    }
    memcpy(data, out_buf + out_pos, n);
    out_pos += n;
    if (out_pos == out_len) {
        out_pos = out_len = 0;
    }
    return n;
}

void tr_purge(void) {
    out_pos = out_len = 0;
}

bool tr_set_dtr(bool level) {
    if (!tr_connected()) {
        return false;
    }
    dtr = level;
    dsr = level;
    if (level) {
        in_len = 0;
        out_pos = out_len = 0;
        usb_lock = false;   // the real firmware releases the USB SD lock in reset
    }
    return true;
}

int tr_get_dsr(void) {
    return dsr ? 1 : 0;
}
