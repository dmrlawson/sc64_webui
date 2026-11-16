// Platform abstraction shared by the Pico firmware and the host simulator.
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Firmware version, set by the build (-DWEBUI_VERSION=...).
#ifndef WEBUI_VERSION
#define WEBUI_VERSION "dev"
#endif

// Milliseconds since boot (wraps after ~49 days; only use for differences).
uint32_t plat_millis(void);

// Pump the USB host stack and the network stack once. Safe to call from any
// blocking wait loop: it never runs HTTP request handlers.
void plat_poll(void);

// Restart the device shortly (replies already queued still go out).
void plat_restart(void);
// A flag that survives plat_restart(), used to limit automatic restarts.
bool plat_restart_flag(void);
void plat_set_restart_flag(bool value);

// printf-style logging into a ring buffer (served at /api/log) and stdout/UART.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Copy the log (oldest first) into out, returns number of bytes.
size_t log_snapshot(char *out, size_t cap);

// ---- Firmware self-update (update.c on the Pico) ----

#define FW_SECTOR_SIZE 4096

// UF2 family ID of images this board can run.
uint32_t plat_fw_family(void);
// Largest image that can be staged, in bytes.
uint32_t plat_fw_capacity(void);
// Write one sector of the new image to the staging area; NULL data leaves it blank.
bool plat_fw_stage(uint32_t sector, const uint8_t *data);
// The staging area, readable as plain memory.
const uint8_t *plat_fw_staged(void);
// Replace the running firmware with the staged image and restart. Does not return.
void plat_fw_apply(uint32_t sectors);

// ---- Byte transport to the SC64 (FTDI serial over USB) ----

// True while the SC64's USB interface is enumerated.
bool tr_connected(void);
// Incremented on every (re)connect so upper layers can detect a replug.
uint32_t tr_generation(void);
// Blocking write of the whole buffer. False on disconnect or timeout.
bool tr_write(const uint8_t *data, size_t len);
// Non-blocking read, returns the number of bytes copied (0 if none).
size_t tr_read(uint8_t *data, size_t max);
// Drop everything buffered in the receive direction.
void tr_purge(void);
bool tr_set_dtr(bool level);
// 1/0 = DSR level, -1 = could not be read.
int tr_get_dsr(void);

#endif
