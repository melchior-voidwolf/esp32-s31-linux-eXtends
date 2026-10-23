// SPDX-License-Identifier: BSD-2-Clause
//
// Load the Linux kernel Image from the FAT partition of the microSD card.
// A file named IMAGE.BIN (8.3, case-insensitive on FAT) on partition 1 wins
// over the kernel in the SPI flash "linux" partition: dropping a file onto
// the card replaces esptool flashing and the size/CRC manifest.

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "loader.h"

#if CONFIG_S31_LINUX_BOARD_KORVO1

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "soc/sdmmc_pins.h"

#define KERNEL_LOAD_ADDR            0x50000000U
#define OPENSBI_LOAD_ADDR           0x50F00000U
#define KERNEL_IMAGE_SIZE_OFFSET    0x10U
#define KERNEL_MAGIC2_OFFSET        0x38U
#define KERNEL_MAGIC2               0x05435352U
#define SD_KERNEL_PATH              "/sd/IMAGE.BIN"

static const char *TAG = "s31-linux-sd-kernel";

/* Same finish as the flash path: size header, BSS clear, magic check. */
static bool check_kernel_image(uint32_t image_size)
{
    uint64_t memory_size;
    uint32_t magic2 =
        *(const volatile uint32_t *)(KERNEL_LOAD_ADDR + KERNEL_MAGIC2_OFFSET);

    if (magic2 != KERNEL_MAGIC2) {
        ESP_LOGE(TAG, "kernel image magic 0x%08" PRIx32
                      " != RISC-V image magic 0x%08" PRIx32,
                 magic2, (uint32_t)KERNEL_MAGIC2);
        return false;
    }

    memcpy(&memory_size,
           (const void *)(KERNEL_LOAD_ADDR + KERNEL_IMAGE_SIZE_OFFSET),
           sizeof(memory_size));
    if (memory_size < image_size ||
        KERNEL_LOAD_ADDR + memory_size > OPENSBI_LOAD_ADDR) {
        ESP_LOGE(TAG, "invalid kernel memory size 0x%08" PRIx32,
                 (uint32_t)memory_size);
        return false;
    }
    memset((void *)(KERNEL_LOAD_ADDR + image_size), 0,
           (size_t)memory_size - image_size);
    return true;
}

bool load_kernel_sd(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    sdmmc_card_t *card = NULL;
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
    };
    esp_err_t err;
    FILE *f;
    long image_size;
    size_t got;
    uint32_t crc;
    bool ok = false;

    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    /* Korvo-1 wired all four data lines; the slot has no CD/WP contacts. */
    slot.width = 4;
    slot.clk = SDMMC_SLOT0_IOMUX_PIN_NUM_CLK;
    slot.cmd = SDMMC_SLOT0_IOMUX_PIN_NUM_CMD;
    slot.d0 = SDMMC_SLOT0_IOMUX_PIN_NUM_D0;
    slot.d1 = SDMMC_SLOT0_IOMUX_PIN_NUM_D1;
    slot.d2 = SDMMC_SLOT0_IOMUX_PIN_NUM_D2;
    slot.d3 = SDMMC_SLOT0_IOMUX_PIN_NUM_D3;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (kernel from flash)",
                 esp_err_to_name(err));
        goto out;
    }

    f = fopen(SD_KERNEL_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "no %s on the card (kernel from flash)", SD_KERNEL_PATH);
        goto out_unmount;
    }
    if (fseek(f, 0, SEEK_END) || (image_size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET)) {
        ESP_LOGE(TAG, "cannot size %s", SD_KERNEL_PATH);
        goto out_close;
    }
    if (KERNEL_LOAD_ADDR + (uint32_t)image_size > OPENSBI_LOAD_ADDR) {
        ESP_LOGE(TAG, "kernel file %ld bytes exceeds the load window",
                 image_size);
        goto out_close;
    }

    got = fread((void *)KERNEL_LOAD_ADDR, 1, (size_t)image_size, f);
    if (got != (size_t)image_size) {
        ESP_LOGE(TAG, "short read: %u of %ld", (unsigned)got, image_size);
        goto out_close;
    }
    fclose(f);

    if (!check_kernel_image((uint32_t)image_size)) {
        goto out_unmount;
    }

    crc = esp_rom_crc32_le(0, (const uint8_t *)KERNEL_LOAD_ADDR,
                           (uint32_t)image_size);
    ESP_LOGI(TAG, "kernel from SD: %s %" PRIu32 " bytes, crc32 0x%08" PRIx32,
             SD_KERNEL_PATH, (uint32_t)image_size, crc);
    ok = true;
    goto out_unmount;

out_close:
    fclose(f);
out_unmount:
    esp_vfs_fat_sdcard_unmount("/sd", card);
out:
    /*
     * The sdmmc driver reroutes the slot pins through the GPIO matrix and
     * disconnects them again on deinit.  The kernel has no pinctrl for the
     * SD host: its dw_mmc driver needs the low-level state that
     * init_sd_card() sets up (IOMUX pins, clock, pad control).  Without
     * this restore, the controller reset of the kernel times out and the
     * boot hangs before the rootfs.
     */
    init_sd_card();
    return ok;
}

#else /* !CONFIG_S31_LINUX_BOARD_KORVO1 */

bool load_kernel_sd(void)
{
    return false;
}

#endif /* CONFIG_S31_LINUX_BOARD_KORVO1 */
