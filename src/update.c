// Firmware self-update for the Pico: flash access behind the plat_fw_*
// interface (see platform.h).
//
// A new image is first written to the upper half of the flash while the
// current firmware keeps running. Applying it then copies that staging area
// over the running firmware from a routine that lives entirely in RAM, and
// resets. If power is lost during that final copy (a few seconds) the Pico
// needs reflashing over USB with BOOTSEL held, which always remains possible.

#include "hardware/flash.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "pico.h"

#include "platform.h"

#define STAGE_OFFSET (PICO_FLASH_SIZE_BYTES / 2)

#if PICO_RP2040
#define UF2_FAMILY 0xe48bff56u   // RP2040
#else
#define UF2_FAMILY 0xe48bff59u   // RP2350, Arm secure image
#endif

uint32_t plat_fw_family(void) {
    return UF2_FAMILY;
}

uint32_t plat_fw_capacity(void) {
    return STAGE_OFFSET;
}

const uint8_t *plat_fw_staged(void) {
    return (const uint8_t *)(XIP_BASE + STAGE_OFFSET);
}

bool plat_fw_stage(uint32_t sector, const uint8_t *data) {
    uint32_t offset = STAGE_OFFSET + sector * FW_SECTOR_SIZE;
    if ((offset + FW_SECTOR_SIZE) > PICO_FLASH_SIZE_BYTES) {
        return false;
    }
    // Code cannot run from flash while it is being written.
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, FW_SECTOR_SIZE);
    if (data) {
        flash_range_program(offset, data, FW_SECTOR_SIZE);
    }
    restore_interrupts(irq);
    if (data) {
        const uint8_t *written = (const uint8_t *)(XIP_BASE + offset);
        for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) {
            if (written[i] != data[i]) {
                return false;
            }
        }
    }
    return true;
}

// Runs from RAM with interrupts off, and must not call anything in flash:
// from the first erase on, the code there is no longer ours.
static void __no_inline_not_in_flash_func(copy_and_reset)(uint32_t sectors) {
    static uint8_t sector_data[FW_SECTOR_SIZE];
    // volatile keeps the compiler from turning the copy loop into a memcpy() call.
    const volatile uint8_t *staged = (const volatile uint8_t *)(XIP_BASE + STAGE_OFFSET);
    for (uint32_t sector = 0; sector < sectors; sector++) {
        uint32_t offset = sector * FW_SECTOR_SIZE;
        for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) {
            sector_data[i] = staged[offset + i];
        }
        flash_range_erase(offset, FW_SECTOR_SIZE);
        flash_range_program(offset, sector_data, FW_SECTOR_SIZE);
    }
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;) {
    }
}

void plat_fw_apply(uint32_t sectors) {
    log_printf("update: installing %lu kB and restarting", (unsigned long)(sectors * FW_SECTOR_SIZE / 1024));
    save_and_disable_interrupts();
    // Prepare a full watchdog reset (as watchdog_reboot() does) now, while
    // the SDK code in flash is still intact; the RAM routine only triggers it.
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    hw_set_bits(&psm_hw->wdsel, PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    watchdog_hw->scratch[4] = 0;
    copy_and_reset(sectors);
}
