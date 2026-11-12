/* Console over the ESP-Mosaico's Type-C port (USB OTG, TinyUSB CDC-ACM through espressif/esp_tinyusb 3.x), taken from
 * the ESP-Mosaico bring-up console, which runs on this board. The S31 has no ROM CDC console on OTG, so TinyUSB
 * provides the port; stdout and the log go there (tinyusb_console).
 *
 * Received bytes: the CDC RX callback (TinyUSB task) copies them into a stream buffer with a zero timeout (drop when
 * full, the host repeats), and the USB door task blocks on that buffer. Nothing else reads stdin.
 *
 * Download door: the host opens the port at 1200 baud (mosaico.sh download, the Arduino upload convention). From a
 * timer (the USB stack must first answer the SET_LINE_CODING request) the board sets LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT
 * and resets the whole system with esp_rom_software_reset_system(), as Espressif's merged arduino-esp32 PR #12957 does
 * for the S31; esp_restart() only resets the CPU cores there. */
#include "mosaico_console.h"
#include "sdlog.h"
#include "sched_mon.h"
#include <stdio.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static StreamBufferHandle_t s_rx;
static bool s_ready;

void mosaico_console_download(void)
{
    printf("CONSOLE rebooting into download mode\n");
    fflush(stdout);
    /* marker (the boot after esptool's reset reads as a panic otherwise), storage quiesce (this ROM reset bypasses
       esp_restart()'s shutdown handlers), 100 ms, then the ROM download reset: os_core.c SYS_RESET_DOOR */
    sys_reset(SYS_RESET_DOOR, "USB door");
}

static void download_cb(void *arg) { (void)arg; mosaico_console_download(); }

static void line_coding_cb(int itf, cdcacm_event_t *ev)
{
    (void)itf;
    const cdc_line_coding_t *lc = ev->line_coding_changed_data.p_line_coding;
    if (!lc || lc->bit_rate != 1200) return;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = download_cb, .name = "touch1200" };
    if (!t && esp_timer_create(&a, &t) != ESP_OK) return;
    esp_timer_start_once(t, 100 * 1000);
}

static void rx_cb(int itf, cdcacm_event_t *ev)
{
    (void)ev;
    uint8_t buf[64];
    size_t got = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof buf, &got) == ESP_OK && got) {
        if (s_rx) xStreamBufferSend(s_rx, buf, got, 0);
        got = 0;
    }
}

void mosaico_console_init(void)
{
    s_rx = xStreamBufferCreate(2048, 1);
    const tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    if (tinyusb_driver_install(&tc) != ESP_OK) return;
    const tinyusb_config_cdcacm_t acm = { .cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = rx_cb,
                                          .callback_line_coding_changed = line_coding_cb };
    if (tinyusb_cdcacm_init(&acm) != ESP_OK) return;
    tinyusb_console_init(TINYUSB_CDC_ACM_0);
    s_ready = true;
    /* with a computer on the cable, give it up to 3 s to open the port so the boot lines reach it;
       on a charger only (never enumerated) this costs 1 s */
    for (int i = 0; i < 20 && !tud_mounted(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 60 && tud_mounted() && !tud_cdc_n_connected(0); i++) vTaskDelay(pdMS_TO_TICKS(50));
}

bool mosaico_console_ready(void) { return s_ready; }
bool mosaico_console_connected(void) { return s_ready && tud_cdc_n_connected(0); }

int mosaico_console_read(void *buf, size_t n, TickType_t wait)
{
    if (!s_rx) { vTaskDelay(wait == portMAX_DELAY ? pdMS_TO_TICKS(1000) : wait); return 0; }
    return (int)xStreamBufferReceive(s_rx, buf, n, wait);
}

/* through the console's file descriptor, so the VFS write lock keeps door replies and log lines from mixing bytes */
int mosaico_console_write(const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
    int64_t until = esp_timer_get_time() + 500 * 1000;
    while (done < n && esp_timer_get_time() < until) {
        ssize_t w = write(fileno(stdout), p + done, n - done);
        if (w > 0) { done += (size_t)w; continue; }
        if (!mosaico_console_connected()) break;      /* nobody reading: drop it */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return (int)done;
}
