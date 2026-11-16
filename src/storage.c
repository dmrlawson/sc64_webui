#include <stdlib.h>

#include "ff.h"
#include "diskio.h"

#include "platform.h"
#include "sc64.h"
#include "storage.h"

// How long the card stays mounted (and locked away from the N64) after the
// last request. Short enough that switching the console on just works, long
// enough that browsing does not re-initialise the card on every click.
#define HOLD_MS 3000
// While mounted, how often to check that the console is still off.
#define WATCH_MS 300

static FATFS fs;
static bool mounted;
static uint32_t last_use;
static uint32_t last_watch;
static DWORD fattime = ((DWORD)(2026 - 1980) << 25) | (1u << 21) | (1u << 16);

const char *storage_state_name(storage_state_t state) {
    switch (state) {
        case STORAGE_READY: return "ready";
        case STORAGE_NO_USB: return "no_usb";
        case STORAGE_NO_LINK: return "no_link";
        case STORAGE_CONSOLE_ON: return "console_on";
        case STORAGE_BUSY: return "busy";
        case STORAGE_NO_CARD: return "no_card";
        case STORAGE_SD_ERROR: return "sd_error";
        case STORAGE_NO_FS: return "no_fs";
    }
    return "unknown";
}

const char *storage_state_message(storage_state_t state) {
    switch (state) {
        case STORAGE_READY: return "Ready";
        case STORAGE_NO_USB: return "SummerCart64 not detected on the USB port. Check the cable.";
        case STORAGE_NO_LINK: return "A USB device is connected but it is not responding as a SummerCart64.";
        case STORAGE_CONSOLE_ON: return "Please turn off the console to manage files.";
        case STORAGE_BUSY: return "The SD card is in use by the console. Turn the console off and try again.";
        case STORAGE_NO_CARD: return "No SD card in the SummerCart64.";
        case STORAGE_SD_ERROR: return "The SD card failed to initialise.";
        case STORAGE_NO_FS: return "No FAT32 or exFAT filesystem found on the SD card.";
    }
    return "Unknown state";
}

static void refresh_time(void) {
    sc64_time_t t;
    if (sc64_time_get(&t) && t.year >= 1980 && t.month >= 1 && t.month <= 12 && t.day >= 1) {
        fattime = ((DWORD)(t.year - 1980) << 25) | ((DWORD)t.month << 21) | ((DWORD)t.day << 16) |
                  ((DWORD)t.hour << 11) | ((DWORD)t.minute << 5) | ((DWORD)t.second >> 1);
    }
}

storage_state_t storage_probe(void) {
    if (!tr_connected()) {
        return STORAGE_NO_USB;
    }
    bool on;
    if (!sc64_link() || !sc64_console_on(&on)) {
        return tr_connected() ? STORAGE_NO_LINK : STORAGE_NO_USB;
    }
    if (on) {
        return STORAGE_CONSOLE_ON;
    }
    uint32_t status;
    if (!sc64_sd_status(&status)) {
        return STORAGE_NO_LINK;
    }
    return (status & SC64_SD_STATUS_INSERTED) ? STORAGE_READY : STORAGE_NO_CARD;
}

static void drop(bool polite) {
    if (!mounted) {
        return;
    }
    f_mount(NULL, "", 0);
    mounted = false;
    if (polite) {
        sc64_sd_deinit();
    }
    log_printf("storage: released SD card");
}

storage_state_t storage_acquire(void) {
    storage_state_t state = storage_probe();
    if (state != STORAGE_READY) {
        drop(state == STORAGE_CONSOLE_ON || state == STORAGE_NO_CARD);
        return state;
    }
    if (mounted) {
        storage_touch();
        return STORAGE_READY;
    }

    uint32_t result;
    if (!sc64_sd_init(&result)) {
        return STORAGE_NO_LINK;
    }
    if (result == SC64_SD_LOCKED) {
        return STORAGE_BUSY;
    }
    if (result == SC64_SD_NO_CARD) {
        return STORAGE_NO_CARD;
    }
    if (result != SC64_SD_OK) {
        log_printf("storage: SD init failed, error %lu", (unsigned long)result);
        return STORAGE_SD_ERROR;
    }
    refresh_time();
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        log_printf("storage: mount failed, FatFs error %d", (int)fr);
        f_mount(NULL, "", 0);
        sc64_sd_deinit();
        return STORAGE_NO_FS;
    }
    mounted = true;
    storage_touch();
    last_watch = plat_millis();
    log_printf("storage: SD card mounted");
    return STORAGE_READY;
}

void storage_touch(void) {
    last_use = plat_millis();
}

bool storage_mounted(void) {
    return mounted;
}

void storage_release(void) {
    drop(true);
}

void storage_task(void) {
    if (!mounted) {
        return;
    }
    uint32_t now = plat_millis();
    if (!tr_connected()) {
        drop(false);
    } else if ((now - last_use) > HOLD_MS) {
        drop(true);
    } else if ((now - last_watch) > WATCH_MS) {
        last_watch = now;
        bool on;
        if (!sc64_console_on(&on)) {
            drop(false);
        } else if (on) {
            log_printf("storage: console switched on");
            drop(true);
        }
    }
}

// ---- FatFs glue ----

// Heap for FatFs (FF_USE_LFN == 3). Apart from file name buffers, this lets
// it clear a new directory's cluster in a few large writes; without a heap
// it writes the cluster one sector at a time, which takes many seconds here.
void *ff_memalloc(UINT size) {
    return malloc(size);
}

void ff_memfree(void *block) {
    free(block);
}

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    return 0;   // the card is initialised by storage_acquire()
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (sector > 0xFFFFFFFFull) {
        return RES_PARERR;
    }
    return sc64_sd_read(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (sector > 0xFFFFFFFFull) {
        return RES_PARERR;
    }
    return sc64_sd_write(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = SC64_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return fattime;
}
