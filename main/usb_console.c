#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "usb_console.h"
#include "status.h"
#include "sys_mode.h"
#include "wifi_creds.h"
#include "remote_config.h"
#include "obd_ble.h"
#include "version.h"
#include "ml_params.h"

static const char *TAG = "usbcon";

static int ok(void) { printf("OK\n"); return 0; }
/* The "ERR" line is the status. Returning 0 keeps the console from adding its own "non-zero error code" line. */
static int err(const char *why) { printf("ERR %s\n", why); return 0; }

/* Changes wait until the car link is down. */
static bool parked(void)
{
    if (sys_mode_get() == MODE_DRIVE) { err("busy: the car link is up, try again when parked"); return false; }
    return true;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    STATUS_LOCK();
    bool sd = g_status.sd_mounted, synced = g_status.time_synced, saved = g_status.adapter_saved;
    uint32_t session = g_status.session_id;
    char adapter[32];
    snprintf(adapter, sizeof adapter, "%s", g_status.adapter_name);
    STATUS_UNLOCK();
    printf("firmware=%s\n", CARMIND_VERSION);
    printf("model=%s\n", ML_MODEL_VERSION);
    printf("uptime_s=%lld\n", (long long)(esp_timer_get_time() / 1000000));
    printf("mode=%s\n", sys_mode_name(sys_mode_get()));
    printf("sd=%s\n", sd ? "mounted" : "missing");
    printf("session=%lu\n", (unsigned long)session);
    printf("clock=%s\n", synced ? "set" : "not set");
    printf("wifi_networks=%u\n", (unsigned)wifi_creds_count());
    printf("adapter_saved=%s\n", saved ? "yes" : "no");
    printf("adapter_name=%s\n", adapter[0] ? adapter : "-");
    return ok();
}

/* wifi list | wifi add <ssid> [password] | wifi forget <ssid> */
static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        for (size_t i = 0; i < wifi_creds_count(); i++) printf("%u %s\n", (unsigned)i, wifi_creds_get(i)->ssid);
        return ok();
    }
    if (argc >= 3 && strcmp(argv[1], "add") == 0 && argc <= 4) {
        if (!parked()) return 0;
        if (!wifi_creds_add(argv[2], argc == 4 ? argv[3] : "")) return err("rejected: SSID 1-32 bytes, password empty or 8-64 bytes, no control characters, list not full");
        return ok();
    }
    if (argc == 3 && strcmp(argv[1], "forget") == 0) {
        if (!parked()) return 0;
        return wifi_creds_remove(argv[2]) ? ok() : err("no such network");
    }
    return err("usage: wifi list | wifi add <ssid> [password] | wifi forget <ssid>");
}

static int cmd_adapter(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        if (!parked()) return 0;
        obd_ble_forget_adapter();
        return ok();
    }
    return err("usage: adapter forget");
}

/* config show | config set <key> <number> */
static int cmd_config(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        const remote_config_t *c = remote_config_get();
        printf("poll_interval_ms=%u\nslow_group_every=%u\ndtc_interval_s=%u\nbacklight_pct=%u\ntheme_mode=%u\nday_start_hour=%u\nday_end_hour=%u\n",
               c->poll_interval_ms, c->slow_group_every, c->dtc_interval_s, c->backlight_pct, c->theme_mode, c->day_start_hour, c->day_end_hour);
        return ok();
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        if (!parked()) return 0;
        char *end = NULL;
        double v = strtod(argv[3], &end);
        if (end == argv[3] || *end) return err("the value must be a number");
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, argv[2], v);
        int applied = remote_config_apply(root);
        cJSON_Delete(root);
        return applied > 0 ? ok() : err("unknown key or value out of range");
    }
    return err("usage: config show | config set <key> <number>");
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    ok();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static void add(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    esp_console_cmd_t cmd = { .command = name, .help = help, .hint = NULL, .func = fn, .argtable = NULL };
    if (esp_console_cmd_register(&cmd) != ESP_OK) ESP_LOGW(TAG, "could not register '%s'", name);
}

void usb_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "ocm> ";
    cfg.max_cmdline_length = 200;
    cfg.task_stack_size = 6144;
    if (esp_console_new_repl_stdio(&cfg, &repl) != ESP_OK) { ESP_LOGW(TAG, "console not started"); return; }
    esp_console_register_help_command();
    add("status", "Firmware, model, mode, card, clock and adapter", cmd_status);
    add("wifi", "wifi list | add <ssid> [password] | forget <ssid>", cmd_wifi);
    add("adapter", "adapter forget: go back to searching for an adapter", cmd_adapter);
    add("config", "config show | set <key> <number>", cmd_config);
    add("reboot", "Restart the board", cmd_reboot);
    if (esp_console_start_repl(repl) != ESP_OK) ESP_LOGW(TAG, "console did not start");
    else ESP_LOGI(TAG, "settings console ready on the USB port");
}
