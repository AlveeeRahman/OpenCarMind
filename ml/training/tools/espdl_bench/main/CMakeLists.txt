idf_component_register(SRCS "main.cpp"
                       REQUIRES esp-dl esp_driver_usb_serial_jtag esp_timer esp_hw_support)
# ESP-DL's helper places the model on a 16-byte boundary in flash (plain EMBED_FILES does not)
set(cmake_dir ${CMAKE_CURRENT_LIST_DIR}/../components/esp-dl/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)
target_add_aligned_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/model/onboard.espdl" BINARY)
