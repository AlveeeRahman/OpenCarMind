/* Host stub for test_wifi_creds.c: one in-memory blob per key, enough for wifi_creds.c */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
