cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pass_key C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# Waveshare drivers and LVGL 8.1 from the board's demo package.
set(BOARD_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../RP2350-Touch-AMOLED-1.8/lib
    CACHE PATH "Directory with the waveshare/ and lvgl/ libraries")
if(NOT EXISTS ${BOARD_LIB_DIR}/lvgl/lvgl.h)
    message(FATAL_ERROR "Board libraries not found in ${BOARD_LIB_DIR}, pass -DBOARD_LIB_DIR=...")
endif()

# LVGL, configured by src/lv_conf.h
file(GLOB_RECURSE LVGL_SOURCES ${BOARD_LIB_DIR}/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${BOARD_LIB_DIR}/lvgl ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -O2 -w)
target_link_libraries(lvgl PUBLIC pico_stdlib)

# Board drivers: QSPI display over PIO, SH8601 AMOLED, FT3168 touch
set(WS ${BOARD_LIB_DIR}/waveshare)
add_library(waveshare STATIC
    ${WS}/Config/DEV_Config.c
    ${WS}/QSPI_PIO/qspi_pio.c
    ${WS}/AMOLED/AMOLED_1in8.c
    ${WS}/Touch/FT3168.c
)
pico_generate_pio_header(waveshare ${WS}/QSPI_PIO/qspi.pio)
target_include_directories(waveshare PUBLIC ${WS}/Config ${WS}/QSPI_PIO ${WS}/AMOLED ${WS}/Touch)
target_compile_options(waveshare PRIVATE -w)
target_link_libraries(waveshare PUBLIC
    pico_stdlib hardware_pio hardware_dma hardware_i2c hardware_spi hardware_pwm hardware_adc)

add_executable(pass_key
    src/main.c
    src/usb_kbd.c
    src/usb_descriptors.c
    src/editor.c
    src/vault.c
    src/ui.c
)
target_include_directories(pass_key PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(pass_key PRIVATE -O2 -Wall)
target_link_libraries(pass_key PRIVATE
    pico_stdlib pico_multicore pico_unique_id pico_rand pico_flash
    hardware_flash
    tinyusb_device
    waveshare lvgl
)

pico_set_program_name(pass_key "Pass Key")
pico_enable_stdio_usb(pass_key 0)
pico_enable_stdio_uart(pass_key 0)
pico_add_extra_outputs(pass_key)
