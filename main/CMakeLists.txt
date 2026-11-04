idf_component_register(
    SRCS "app_main.c"
         "obd_ble.c"
         "adapter_match.c"
         "econ.c"
         "units.c"
         "harden.c"
         "power_state.c"
         "uploader.c"
         "usb_door.c"
         "elm327.c"
         "sdlog.c"
         "display.c"
         "wifi_sync.c"
         "status_led.c"
         "custom_pids.c"
         "fault_engine.c"
         "pattern.c"
         "cpu_monitor.c"
         "wifi_creds.c"
         "events.c"
         "remote_config.c"
         "sched_mon.c"
         "sys_mode.c"
         "ml_infer.cpp"
         "rev_learn.c"
         "rev_cal.c"
         "rev_state.c"
         "rev_task.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_http_client esp_driver_usb_serial_jtag
             esp_lcd fatfs sdmmc vfs esp_timer esp_driver_gpio
             esp_driver_spi esp_driver_sdmmc esp_driver_ledc
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CARMIND_VERSION="${PROJECT_VER}")

# On-board model (ml/onboard/onboard.espdl), embedded on a 16-byte boundary with ESP-DL's own helper so the loader
# does not warn or copy. Symbol: _binary_onboard_espdl_start.
if(CONFIG_CARMIND_ML_ENABLE)
    idf_component_get_property(esp_dl_dir espressif__esp-dl COMPONENT_DIR)
    set(cmake_dir ${esp_dl_dir}/fbs_loader/cmake)
    include(${cmake_dir}/utilities.cmake)
    target_add_aligned_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../ml/onboard/onboard.espdl" BINARY)
    # CarMind R.E.V (ml/onboard/rev/onboard.espdl). The helper names the symbol and its generated .S after the file name, so the
    # file is copied into the build folder under its own name first. Symbol: _binary_onboard_rev_espdl_start.
    if(CONFIG_CARMIND_REV_ENABLE)
        idf_build_get_property(build_dir BUILD_DIR)
        configure_file("${CMAKE_CURRENT_LIST_DIR}/../ml/onboard/rev/onboard.espdl" "${build_dir}/onboard_rev.espdl" COPYONLY)
        target_add_aligned_binary_data(${COMPONENT_LIB} "${build_dir}/onboard_rev.espdl" BINARY)
    endif()
endif()
