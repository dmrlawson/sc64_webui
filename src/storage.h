// SD card session management: decides when it is safe to touch the card,
// takes/releases the SC64's SD lock and mounts the FAT/exFAT volume.
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STORAGE_READY,        // card can be (or is) mounted
    STORAGE_NO_USB,       // SC64 not enumerated on the USB port
    STORAGE_NO_LINK,      // USB device present but not answering as an SC64
    STORAGE_CONSOLE_ON,   // N64 is powered on, hands off
    STORAGE_BUSY,         // SC64 reports the card locked by the N64 side
    STORAGE_NO_CARD,      // no SD card in the slot
    STORAGE_SD_ERROR,     // card failed to initialise
    STORAGE_NO_FS,        // no FAT32/exFAT filesystem found
} storage_state_t;

// Short machine name ("ready", "console_on", ...) and a sentence for humans.
const char *storage_state_name(storage_state_t state);
const char *storage_state_message(storage_state_t state);

// Check availability without taking the SD lock.
storage_state_t storage_probe(void);
// Ensure the card is mounted. Only STORAGE_READY allows FatFs calls.
storage_state_t storage_acquire(void);
// Mark the session as in use (call during long transfers).
void storage_touch(void);
bool storage_mounted(void);
// Unmount and give the card back to the SC64.
void storage_release(void);
// Call regularly from the main loop: releases the card when idle or as soon
// as the console is switched on.
void storage_task(void);

#endif
