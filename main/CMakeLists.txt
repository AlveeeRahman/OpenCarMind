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
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_http_client esp_driver_usb_serial_jtag
             esp_lcd fatfs sdmmc vfs esp_timer esp_driver_gpio
             esp_driver_spi esp_driver_sdmmc esp_driver_ledc
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CARMIND_VERSION="${PROJECT_VER}")

# Model keys. The shipped models are encrypted (AES-128-CTR, ESP-DL's own format). An official build sets
# CARMIND_MODEL_KEY_EV in its environment; main/model_keys.cmake turns it into carmind_model_keys.h in the build folder
# at every build. Without the key the firmware builds and runs with the model locked.
set(model_keys_dir "${CMAKE_CURRENT_BINARY_DIR}/model_keys")
file(MAKE_DIRECTORY "${model_keys_dir}")
idf_build_get_property(model_keys_python PYTHON)
add_custom_target(carmind_model_keys
    COMMAND ${CMAKE_COMMAND} -DOUT=${model_keys_dir}/carmind_model_keys.h
            "-DMODELS=EV|${CMAKE_CURRENT_LIST_DIR}/../ml/onboard/onboard.espdl"
            -DPYTHON=${model_keys_python} -DCHECK=${CMAKE_CURRENT_LIST_DIR}/../tools/check_model_key.py
            -P ${CMAKE_CURRENT_LIST_DIR}/model_keys.cmake
    BYPRODUCTS ${model_keys_dir}/carmind_model_keys.h
    VERBATIM)
add_dependencies(${COMPONENT_LIB} carmind_model_keys)
target_include_directories(${COMPONENT_LIB} PRIVATE "${model_keys_dir}")

# On-board model (ml/onboard/onboard.espdl), embedded on a 16-byte boundary with ESP-DL's own helper so the loader
# does not warn or copy. Symbol: _binary_onboard_espdl_start.
if(CONFIG_CARMIND_ML_ENABLE)
    idf_component_get_property(esp_dl_dir espressif__esp-dl COMPONENT_DIR)
    set(cmake_dir ${esp_dl_dir}/fbs_loader/cmake)
    include(${cmake_dir}/utilities.cmake)
    target_add_aligned_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../ml/onboard/onboard.espdl" BINARY)
endif()
