# Board by target: esp32s3 = Waveshare ESP32-S3-LCD-1.47B (the car board), esp32s31 = Espressif ESP-Mosaico V1.2 (board.h).
idf_build_get_property(ocm_target IDF_TARGET)
if(ocm_target STREQUAL "esp32s31")
    set(ocm_board_srcs "mosaico_console.c" "mosaico_board.c" "mosaico_display.c" "mosaico_apps.c" "mosaico_files.c" "mosaico_drive.c" "mosaico_logos.c" "mosaico_icons.c")
    set(ocm_board_reqs esp_driver_i2c wear_levelling esp_pm esp_psram spi_flash esp_mm)
    list(APPEND ocm_board_srcs "swipe_intent.c")                   # smart swipe (CONFIG_OCM_SWIPE_INTENT)
    list(APPEND ocm_board_srcs "ui_metrics.c")                     # Smart UI metrics + gate (smart swipe's histograms too)
    list(APPEND ocm_board_srcs "ui_prefetch.c")                    # Smart UI arm A1: press-time prefetch (CONFIG_OCM_UI_PREFETCH)
    list(APPEND ocm_board_srcs "mosaico_clock.c" "clock_set.c")    # Settings > Clock: set the clock by hand
    if(CONFIG_OCM_NAND_LOGS)                                       # drive logs on the 128 MB SPI NAND (F1)
        list(APPEND ocm_board_srcs "nand_store.c" "log_store.c" "log_move.c")
    endif()
    list(APPEND ocm_board_srcs "ui_settings_logic.c")              # Settings: blob migration, order moves, debounce
    list(APPEND ocm_board_srcs "wifi_ui_logic.c")                  # Wi-Fi app: search list merge, password rules, join texts
    list(APPEND ocm_board_reqs esp_app_format)                    # its firmware tag: the app ELF SHA
    set(ocm_ml_dir "${CMAKE_CURRENT_LIST_DIR}/../ml/onboard_s31")   # CarMind E.V (ev/) and R.E.V (rev/) re-quantized for the S31
else()
    set(ocm_board_srcs "display.c")
    set(ocm_board_reqs perfmon)                                      # Xtensa PMU (cpu_monitor.c)
    set(ocm_ml_dir "${CMAKE_CURRENT_LIST_DIR}/../ml/onboard")
endif()

idf_component_register(
    SRCS ${ocm_board_srcs}
         "app_main.c"
         "obd_ble.c"
         "adapter_match.c"
         "econ.c"
         "obd_fresh.c"
         "rev_torque.c"
         "rev_tank.c"
         "units.c"
         "harden.c"
         "power_state.c"
         "uploader.c"
         "usb_door.c"
         "elm327.c"
         "ecu_select.c"
         "sdlog.c" "fat_mirror.c" "session_next.c" "storage_rt.c"
         "wifi_sync.c"
         "status_led.c"
         "custom_pids.c"
         "fault_engine.c"
         "pattern.c"
         "cpu_monitor.c"
         "wifi_creds.c"
         "events.c"
         "remote_config.c"
         "sched_mon.c" "reset_class.c"
         "sys_mode.c"
         "ml_infer.cpp"
         "rev_learn.c"
         "rev_cal.c"
         "rev_state.c"
         "rev_task.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_driver_tsens esp_wifi esp_netif esp_event esp_http_client mbedtls esp_driver_usb_serial_jtag
             esp_lcd fatfs sdmmc vfs esp_timer esp_driver_gpio esp_app_format
             esp_driver_spi esp_driver_sdmmc esp_driver_ledc ${ocm_board_reqs}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CARMIND_VERSION="${PROJECT_VER}")
if(ocm_target STREQUAL "esp32s31")
    # the RISC-V GCC flags snprintf("%s") copies whose length the code checks just before (custom_pids.c): warn, not fail
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
    # swipe step e: a short page snap (LVGL clamps the scroll animation to 200..400 ms; lv_obj_scroll.c lets a build set it)
    idf_component_get_property(ocm_lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${ocm_lvgl_lib} PRIVATE SCROLL_ANIM_TIME_MIN=150 SCROLL_ANIM_TIME_MAX=180)
endif()

# On-board model (ml/onboard/onboard.espdl), embedded on a 16-byte boundary with ESP-DL's own helper so the loader
# does not warn or copy. Symbol: _binary_onboard_espdl_start.
if(CONFIG_CARMIND_ML_ENABLE)
    idf_component_get_property(esp_dl_dir espressif__esp-dl COMPONENT_DIR)
    set(cmake_dir ${esp_dl_dir}/fbs_loader/cmake)
    include(${cmake_dir}/utilities.cmake)
    if(ocm_target STREQUAL "esp32s31")       # CarMind E.V for the S31 (ml/onboard_s31/ev), the same symbol
        target_add_aligned_binary_data(${COMPONENT_LIB} "${ocm_ml_dir}/ev/onboard.espdl" BINARY)
    else()
        target_add_aligned_binary_data(${COMPONENT_LIB} "${ocm_ml_dir}/onboard.espdl" BINARY)
    endif()
    # CarMind R.E.V (ml/onboard/rev/onboard.espdl). The helper names the symbol and its generated .S after the file name, so the
    # file is copied into the build folder under its own name first. Symbol: _binary_onboard_rev_espdl_start.
    if(CONFIG_CARMIND_REV_ENABLE)
        idf_build_get_property(build_dir BUILD_DIR)
        configure_file("${ocm_ml_dir}/rev/onboard.espdl" "${build_dir}/onboard_rev.espdl" COPYONLY)
        target_add_aligned_binary_data(${COMPONENT_LIB} "${build_dir}/onboard_rev.espdl" BINARY)
    endif()
endif()
