#pragma once
/* ESP-Mosaico 128 MB SPI NAND (GigaDevice GD5F1GM7UEYIGR, 3.3 V) for the drive logs (F1, CONFIG_OCM_NAND_LOGS).
 *
 * Wiring (CoreBoard V1.2.1 schematic, MCU page): NAND_CLK = SD_D0 = GPIO20, NAND_D (SI/SIO0) = SD_D1 = GPIO21,
 * NAND_Q (SO/SIO1) = SD_D2 = GPIO22, NAND_CS = SD_D3 = GPIO23 (10 k pull-up to VCC_NAND), NAND_HD (HOLD#/SIO3) = SD_CLK =
 * GPIO24, NAND_WP (WP#/SIO2) = SD_CMD = GPIO25. VCC_NAND = MCU_3V3 through R16 (0 R): always powered, no enable GPIO.
 * GPIO20-25 are the chip's SDIO pads (VDDPST_SD domain) and SPI2's IO MUX quad set, but SPI2 drives the AMOLED (GPIO matrix),
 * so the NAND runs on SPI3 through the GPIO matrix (SPI3 has no IO MUX pins on the S31).
 *
 * The driver is espressif/spi_nand_flash (Dhara FTL: wear levelling, bad blocks, power-fail rollback to the last sync)
 * with espressif/spi_nand_flash_fatfs. Every access is an ordinary GPSPI transaction (spi_device_transmit): no flash
 * cache lock, unlike the NOR FAT. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_OCM_NAND_LOGS
#define NAND_LOG_DIR       CONFIG_OCM_NAND_MOUNT_POINT "/obd"
#define NAND_SESSIONS_DIR  NAND_LOG_DIR "/sessions"

typedef enum {
    NAND_ABSENT = 0,    /* not started, or no chip answered on SPI3 */
    NAND_MOUNTED,       /* the drive store */
    NAND_UNREADABLE,    /* holds data FatFs cannot mount (or both FAT copies damaged): left as it is, Format storage erases it */
    NAND_FAILED,        /* the chip answered but the driver or the format failed */
} nand_state_t;

/* SPI3 bus, chip, FAT checks (fat_mirror, as on the flash FAT), mount. A blank chip (every good block's first page unused)
   is formatted (2 FATs, 16 KB clusters); a chip that holds data and does not mount is never formatted here. */
esp_err_t    nand_store_init(void);
nand_state_t nand_store_state(void);
static inline bool nand_store_mounted(void) { return nand_store_state() == NAND_MOUNTED; }
bool         nand_store_info(uint64_t *total, uint64_t *free_b);
/* Settings > Format storage: erase every good block (Dhara map cleared), make the FAT, mount. ~15 s. */
esp_err_t    nand_store_format(char *why, size_t why_n);
/* Before a reset (sdlog_quiesce_for_reset, after the logger flushed its files): wait for a mount or format in progress,
   take FatFs's lock of the NAND volume and keep it, then a Dhara sync, so everything written so far is durable. */
void         nand_store_quiesce(void);
/* Deep sleep: unmount and sync (the logger closed its files first). */
void         nand_store_unmount(void);
/* One line for the console and the storage card: "GD5F1GM7 C8/91, 1024 blocks, 0 bad, SIO 20 MHz" */
const char  *nand_store_describe(void);
#endif
