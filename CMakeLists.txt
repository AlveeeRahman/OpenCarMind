cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# OpenCarMind EdgeOS (2.0.0; mossREV up to 1.2): the ESP-Mosaico (esp32s31) build is its own release with CarMind R.E.V and E.V; the esp32s3 car build keeps version.txt.
if(IDF_TARGET STREQUAL "esp32s31")
    set(PROJECT_VER "EdgeOS 2.0.0")
endif()
project(opencarmind)
