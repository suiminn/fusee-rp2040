cmake_minimum_required(VERSION 3.13)

set(PICO_DEFAULT_BOARD_rp2040 waveshare_rp2040_one CACHE STRING "Default RP2040 board")

include(pico_sdk_import.cmake)

project(fusee C CXX ASM)

pico_sdk_init()

add_subdirectory(src)
