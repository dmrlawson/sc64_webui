// SummerCart64 USB protocol client (see SummerCart64/docs/03_usb_interface.md).
#ifndef SC64_H
#define SC64_H

#include <stdbool.h>
#include <stdint.h>

#define SC64_SECTOR_SIZE 512

// sd_error_t values from the SC64 controller firmware that we care about.
#define SC64_SD_OK 0
#define SC64_SD_NO_CARD 1
#define SC64_SD_LOCKED 30

#define SC64_SD_STATUS_INSERTED (1u << 0)

typedef struct {
    uint8_t weekday;   // 1-7, 1 = Monday
    uint8_t hour, minute, second;
    uint16_t year;     // full year, e.g. 2026
    uint8_t month, day;
} sc64_time_t;

// Bring the link up if needed (resets the protocol state machine after a
// (re)connect or a failed command). Cheap when already up.
bool sc64_link(void);
// Firmware version string, valid after sc64_link() succeeded.
const char *sc64_version(void);

// Each returns false on a transport/protocol failure.
bool sc64_console_on(bool *on);
bool sc64_time_get(sc64_time_t *t);
bool sc64_time_set(const sc64_time_t *t);

// SD card. *result receives the SC64 sd_error_t (SC64_SD_OK on success).
bool sc64_sd_init(uint32_t *result);
bool sc64_sd_deinit(void);
bool sc64_sd_status(uint32_t *status);
bool sc64_sd_read(uint8_t *data, uint32_t sector, uint32_t count);
bool sc64_sd_write(const uint8_t *data, uint32_t sector, uint32_t count);

#endif
