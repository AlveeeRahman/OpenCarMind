// On-device bench for the CarMind on-board model (manoeuvre + estimator heads).
// Boot: load the .espdl from flash, run ESP-DL's self-test against the embedded test values, print memory and
// per-layer latency. Then serve windows over the USB-Serial/JTAG cable:
//   host -> "CMBENCH RUN <n>\n" then n little-endian float32 values, NHWC order [1][1][W][30]
//   board -> "CMBENCH OUT <man0..3> <est0..2> <us> <cycles>\n"   (outputs de-quantised to float)
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "dl_model_base.hpp"

static const char *TAG = "bench";
extern const uint8_t model_start[] asm("_binary_onboard_espdl_start");

static void reply(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + 1, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[0] = '\n'; line[n + 1] = '\n';
    usb_serial_jtag_write_bytes(line, n + 2, pdMS_TO_TICKS(200));
}

static int read_line(char *buf, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') { if (n == 0) continue; break; }
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

static void quantize_into(dl::TensorBase *t, const float *src, int n)
{
    float inv = ldexpf(1.0f, -(int)t->exponent);           // q = round(x / 2^exp)
    if (t->dtype == dl::DATA_TYPE_INT16) {
        int16_t *d = (int16_t *)t->data;
        for (int i = 0; i < n; i++) { long q = lroundf(src[i] * inv); d[i] = q > 32767 ? 32767 : q < -32768 ? -32768 : q; }
    } else if (t->dtype == dl::DATA_TYPE_INT8) {
        int8_t *d = (int8_t *)t->data;
        for (int i = 0; i < n; i++) { long q = lroundf(src[i] * inv); d[i] = q > 127 ? 127 : q < -128 ? -128 : q; }
    } else {
        memcpy(t->data, src, n * sizeof(float));
    }
}

static float value_at(dl::TensorBase *t, int i)
{
    float s = ldexpf(1.0f, (int)t->exponent);
    if (t->dtype == dl::DATA_TYPE_INT16) return ((int16_t *)t->data)[i] * s;
    if (t->dtype == dl::DATA_TYPE_INT8) return ((int8_t *)t->data)[i] * s;
    return ((float *)t->data)[i];
}

extern "C" void app_main(void)
{
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 2048, .rx_buffer_size = 16384};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int64_t t0 = esp_timer_get_time();
    dl::Model *model = new dl::Model((const char *)model_start, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    int64_t t_load = esp_timer_get_time() - t0;
    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t st = model->test();
    model->profile();
    dl::TensorBase *in = model->get_input();
    std::map<std::string, dl::TensorBase *> &outs = model->get_outputs();
    dl::TensorBase *man = outs["man"], *est = outs["est"];
    ESP_LOGW(TAG, "load %lld us, internal heap used %u B, PSRAM used %u B, self-test %s", t_load,
             (unsigned)(heap0 - heap1), (unsigned)(ps0 - ps1), st == ESP_OK ? "PASS" : "FAIL");
    reply("CMBENCH READY selftest=%s load_us=%lld internal_bytes=%u psram_bytes=%u in_shape=%d,%d,%d,%d in_dtype=%s in_exp=%d",
          st == ESP_OK ? "PASS" : "FAIL", t_load, (unsigned)(heap0 - heap1), (unsigned)(ps0 - ps1),
          in->shape[0], in->shape[1], in->shape[2], in->shape[3], in->get_dtype_string(), (int)in->exponent);

    std::vector<float> buf(in->get_size());
    char line[96];
    for (;;) {
        if (read_line(line, sizeof line) <= 0) continue;
        if (strncmp(line, "CMBENCH PING", 12) == 0) {
            reply("CMBENCH READY selftest=%s in_size=%d", st == ESP_OK ? "PASS" : "FAIL", in->get_size());
            continue;
        }
        int n = 0;
        if (sscanf(line, "CMBENCH RUN %d", &n) != 1 || n != in->get_size()) { reply("CMBENCH ERR size %d want %d", n, in->get_size()); continue; }
        uint8_t *p = (uint8_t *)buf.data();
        int need = n * 4, got = 0;
        while (got < need) {
            int r = usb_serial_jtag_read_bytes(p + got, need - got, pdMS_TO_TICKS(3000));
            if (r <= 0) break;
            got += r;
        }
        if (got != need) { reply("CMBENCH ERR short %d", got); continue; }
        quantize_into(in, buf.data(), n);
        uint32_t c0 = esp_cpu_get_cycle_count(); int64_t u0 = esp_timer_get_time();
        model->run();
        int64_t us = esp_timer_get_time() - u0; uint32_t cyc = esp_cpu_get_cycle_count() - c0;
        reply("CMBENCH OUT %.5f %.5f %.5f %.5f %.5f %.5f %.5f %lld %lu", value_at(man, 0), value_at(man, 1), value_at(man, 2),
              value_at(man, 3), value_at(est, 0), value_at(est, 1), value_at(est, 2), us, (unsigned long)cyc);
    }
}
