cmake_minimum_required(VERSION 3.16)

# -DMINI_BOARD=matrix: the Waveshare ESP32-S3-Matrix (8 x 8 RGB LEDs, no display, 4 MB flash,
# 2 MB PSRAM) instead of the target's default board. Its settings come on top of the S3's
# (sdkconfig.defaults.esp32s3 is read right after sdkconfig.defaults).
if(MINI_BOARD STREQUAL "matrix")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.matrix")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(MINI_BOARD STREQUAL "matrix")
    idf_build_set_property(COMPILE_DEFINITIONS "MINI_BOARD_MATRIX=1" APPEND)
endif()
project(minischeme_esp32)
