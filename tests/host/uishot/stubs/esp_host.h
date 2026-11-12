/* The ESP-IDF, FreeRTOS and driver names that mosaico_display.c and mosaico_apps.c use, for the host renderer. The
   renderer never runs the panel, touch or task code: these only let the two files compile. Values that reach a screen
   come from uishot/fw_stubs.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERROR_CHECK(x) ((void)(x))
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)
const char *esp_err_to_name(esp_err_t e);

#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))

/* log */
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
static inline void esp_log_level_set(const char *t, esp_log_level_t l) { (void)t; (void)l; }

/* heap */
#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
void *heap_caps_malloc(size_t n, uint32_t caps);
void *heap_caps_calloc(size_t c, size_t n, uint32_t caps);
void *heap_caps_realloc(void *p, size_t n, uint32_t caps);
void *heap_caps_aligned_alloc(size_t a, size_t n, uint32_t caps);
void *heap_caps_aligned_calloc(size_t a, size_t c, size_t n, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);

/* timer, cpu, random, system */
int64_t esp_timer_get_time(void);
typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct { esp_timer_cb_t callback; void *arg; int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
uint32_t esp_cpu_get_cycle_count(void);
uint32_t esp_random(void);
void esp_restart(void);
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT,
               ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE,
               ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
typedef struct { int model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *ci);
typedef struct { uint32_t magic_word; uint32_t secure_version; uint32_t reserv1[2]; char version[32]; char project_name[32];
                 char time[16]; char date[16]; char idf_ver[32]; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
#define ESP_IDF_VERSION_MAJOR 6
#define ESP_IDF_VERSION_MINOR 1
#define ESP_IDF_VERSION_PATCH 0
const char *esp_get_idf_version(void);
size_t esp_psram_get_size(void);
typedef struct esp_flash_t esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out);
esp_err_t esp_task_wdt_add(void *task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(void *task);
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 0)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C (1 << 1)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED (1 << 2)
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA (1 << 3)
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);

/* FreeRTOS */
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10))
#define tskNO_AFFINITY 0x7fffffff
typedef struct { volatile uint32_t owner; volatile uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0, 0}
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
void vTaskDelay(TickType_t t);
void vTaskDelete(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);

/* drivers: only names */
typedef int gpio_num_t;
typedef int spi_host_device_t;
#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, data4_io_num, data5_io_num,
                 data6_io_num, data7_io_num, max_transfer_sz; uint32_t flags; int intr_flags; } spi_bus_config_t;
esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int dma);
typedef void *spi_device_handle_t;
esp_err_t spi_device_acquire_bus(spi_device_handle_t d, TickType_t t);
void spi_device_release_bus(spi_device_handle_t d);
typedef void *i2c_master_bus_handle_t;

/* LCD panel, touch and the LVGL port */
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef intptr_t esp_lcd_spi_bus_handle_t;
typedef void *esp_lcd_i2c_bus_handle_t;
typedef struct { int cs_gpio_num, dc_gpio_num, spi_mode; unsigned pclk_hz; size_t trans_queue_depth; void *on_color_trans_done;
                 void *user_ctx; int lcd_cmd_bits, lcd_param_bits;
                 struct { unsigned dc_low_on_data : 1, octal_mode : 1, quad_mode : 1, sio_mode : 1, lsb_first : 1,
                          cs_high_active : 1, psram_dma_direct : 1; } flags; } esp_lcd_panel_io_spi_config_t;
typedef struct { uint32_t dev_addr; void *on_color_trans_done; void *user_ctx; size_t control_phase_bytes;
                 unsigned dc_bit_offset; int lcd_cmd_bits, lcd_param_bits; struct { unsigned dc_low_on_data : 1,
                 disable_control_phase : 1; } flags; uint32_t scl_speed_hz; } esp_lcd_panel_io_i2c_config_t;
typedef enum { LCD_RGB_ELEMENT_ORDER_RGB, LCD_RGB_ELEMENT_ORDER_BGR } lcd_rgb_element_order_t;
typedef struct { int reset_gpio_num; lcd_rgb_element_order_t rgb_ele_order; int data_endian; uint32_t bits_per_pixel;
                 struct { unsigned reset_active_high : 1; } flags; void *vendor_config; } esp_lcd_panel_dev_config_t;
typedef struct { struct { unsigned use_qspi_interface : 1; } flags; const void *init_cmds; uint16_t init_cmds_size; } co5300_vendor_config_t;
#define CO5300_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max) { .sclk_io_num = (sclk), .data4_io_num = (d0), .max_transfer_sz = (max) }
#define CO5300_PANEL_IO_QSPI_CONFIG(cs, cb, ctx) { .cs_gpio_num = (cs) }
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t b, const esp_lcd_panel_io_spi_config_t *c, esp_lcd_panel_io_handle_t *io);
esp_err_t esp_lcd_new_panel_io_i2c(esp_lcd_i2c_bus_handle_t b, const esp_lcd_panel_io_i2c_config_t *c, esp_lcd_panel_io_handle_t *io);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
esp_err_t esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *c, esp_lcd_panel_handle_t *p);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on);
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t p, bool sleep);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *data);
esp_err_t esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t p, uint8_t pct);
typedef struct esp_lcd_touch_s *esp_lcd_touch_handle_t;
typedef struct { uint16_t x, y, strength; uint8_t track_id; } esp_lcd_touch_point_data_t;
typedef struct { uint16_t x_max, y_max; int rst_gpio_num, int_gpio_num; struct { unsigned reset, interrupt; } levels;
                 struct { unsigned swap_xy : 1, mirror_x : 1, mirror_y : 1; } flags; } esp_lcd_touch_config_t;
esp_err_t esp_lcd_touch_new_i2c_cst9220(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *c, esp_lcd_touch_handle_t *t);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t t, esp_lcd_touch_point_data_t *d, uint8_t *n, uint8_t max);
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t t);
#define ESP_LCD_TOUCH_IO_I2C_CST9220_CONFIG() { 0 }
#define ESP_LCD_TOUCH_IO_I2C_CST9220_ADDRESS 0x5a
typedef struct { int task_priority, task_stack, task_affinity, task_max_sleep_ms, timer_period_ms; } lvgl_port_cfg_t;
#define ESP_LVGL_PORT_INIT_CONFIG() { .task_priority = 4, .task_stack = 6144, .task_affinity = -1, .task_max_sleep_ms = 500, .timer_period_ms = 5 }
typedef struct { esp_lcd_panel_io_handle_t io_handle; esp_lcd_panel_handle_t panel_handle; void *control_handle;
                 uint32_t buffer_size; bool double_buffer; uint32_t trans_size; uint32_t hres, vres; bool monochrome;
                 int color_format; struct { unsigned swap_xy : 1, mirror_x : 1, mirror_y : 1; } rotation;
                 struct { unsigned buff_dma : 1, buff_spiram : 1, sw_rotate : 1, swap_bytes : 1, full_refresh : 1,
                          direct_mode : 1; } flags; } lvgl_port_display_cfg_t;
typedef struct { void *disp; esp_lcd_touch_handle_t handle; } lvgl_port_touch_cfg_t;
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *c);
void *lvgl_port_add_disp(const lvgl_port_display_cfg_t *c);
void *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *c);
bool lvgl_port_lock(uint32_t ms);
void lvgl_port_unlock(void);
#define GPIO_NUM_NC (-1)
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_23 23
#define GPIO_NUM_24 24
#define GPIO_NUM_25 25
#define GPIO_NUM_26 26
#define GPIO_NUM_27 27
#define GPIO_NUM_28 28
#define GPIO_NUM_29 29
#define GPIO_NUM_30 30
#define GPIO_NUM_31 31
#define GPIO_NUM_32 32
#define GPIO_NUM_33 33
#define GPIO_NUM_34 34
#define GPIO_NUM_35 35
#define GPIO_NUM_36 36
#define GPIO_NUM_37 37
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_43 43
#define GPIO_NUM_44 44
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define GPIO_NUM_49 49
#define GPIO_NUM_50 50
#define GPIO_NUM_51 51
#define GPIO_NUM_52 52
#define GPIO_NUM_53 53
#define GPIO_NUM_54 54
#define GPIO_NUM_55 55
#define GPIO_NUM_56 56
#define GPIO_NUM_57 57
#define GPIO_NUM_58 58
#define GPIO_NUM_59 59
#define GPIO_NUM_60 60
#define GPIO_NUM_61 61
#define GPIO_NUM_62 62
#define GPIO_NUM_63 63
