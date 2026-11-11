cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# mossREV: the ESP-Mosaico (esp32s31) build is its own release, CarMind R.E.V only; the esp32s3 car build keeps version.txt.
if(IDF_TARGET STREQUAL "esp32s31")
    set(PROJECT_VER "mossREV 1.0.0")
endif()
project(opencarmind)
