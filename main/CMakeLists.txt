idf_component_register(
    SRCS "app_main.c"
         # OBD-II: Bluetooth LE adapter, ELM327 protocol, field table, custom PIDs
         "obd/obd_ble.c" "obd/elm327.c" "obd/adapter_match.c" "obd/obd_fresh.c" "obd/custom_pids.c"
         # On-board models: ESP-DL inference, CarMind R.E.V learners (fuel, airflow, tank, calibration), physics torque
         "model/ml_infer.cpp" "model/rev_task.c" "model/rev_learn.c" "model/rev_cal.c" "model/rev_state.c"
         "model/rev_tank.c" "model/rev_torque.c"
         # Derived values: fuel economy, fault engine, pattern windows, units
         "analysis/econ.c" "analysis/fault_engine.c" "analysis/pattern.c" "analysis/units.c"
         # Screen and LED
         "ui/display.c" "ui/status_led.c"
         # SD card: session logs, event log, config.json
         "storage/sdlog.c" "storage/events.c" "storage/remote_config.c"
         # Network: Wi-Fi credentials, clock sync, upload client
         "net/wifi_creds.c" "net/wifi_sync.c" "net/uploader.c"
         # System: scheduler, modes, power, counters, input checks, USB door
         "system/sched_mon.c" "system/sys_mode.c" "system/power_state.c" "system/cpu_monitor.c"
         "system/harden.c" "system/usb_door.c"
    INCLUDE_DIRS "." "obd" "model" "analysis" "ui" "storage" "net" "system"
    REQUIRES bt nvs_flash driver esp_driver_tsens perfmon esp_wifi esp_netif esp_event esp_http_client mbedtls esp_driver_usb_serial_jtag
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
