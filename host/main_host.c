// Host build of the web UI firmware: real HTTP server, FatFs and SC64
// protocol code talking to a simulated SC64 (see transport_sim.c).
//
// usage: sc64webui-sim <sd-image> [port]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "platform.h"
#include "storage.h"

void sim_init(int fd);

uint32_t plat_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

void plat_poll(void) {
    usleep(200);
}

// Firmware update: staged in memory; "applying" writes staged_firmware.bin.
// SIM_FAMILY selects the UF2 family ID accepted (default: RP2350).
#define FW_CAPACITY (2 * 1024 * 1024)
static uint8_t fw_staged[FW_CAPACITY];

uint32_t plat_fw_family(void) {
    const char *family = getenv("SIM_FAMILY");
    return family ? (uint32_t)strtoul(family, NULL, 0) : 0xe48bff59u;
}

uint32_t plat_fw_capacity(void) {
    return FW_CAPACITY;
}

const uint8_t *plat_fw_staged(void) {
    return fw_staged;
}

bool plat_fw_stage(uint32_t sector, const uint8_t *data) {
    if (data) {
        memcpy(fw_staged + sector * FW_SECTOR_SIZE, data, FW_SECTOR_SIZE);
    } else {
        memset(fw_staged + sector * FW_SECTOR_SIZE, 0xFF, FW_SECTOR_SIZE);
    }
    return true;
}

void plat_fw_apply(uint32_t sectors) {
    FILE *out = fopen("staged_firmware.bin", "wb");
    fwrite(fw_staged, FW_SECTOR_SIZE, sectors, out);
    fclose(out);
    log_printf("update: wrote staged_firmware.bin (%u sectors); a real device would restart now", sectors);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <sd-image> [port]\n", argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    int port = argc > 2 ? atoi(argv[2]) : 8064;
    sim_init(fd);
    if (!http_init((uint16_t)port)) {
        return 1;
    }
    log_printf("simulator listening on http://localhost:%d", port);
    wifi_config_t config;
    if (storage_acquire() == STORAGE_READY) {
        if (wifi_config_read(&config)) {
            log_printf("wifi settings: ssid \"%s\" password \"%s\"", config.ssid, config.password);
        } else {
            log_printf("no wifi settings at %s", WIFI_CONFIG_PATH);
        }
        storage_release();
    }
    for (;;) {
        plat_poll();
        storage_task();
        http_task();
    }
}
