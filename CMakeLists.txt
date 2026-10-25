cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pass_key C CXX ASM)
set(CMAKE_C_STANDARD 11)

add_executable(pass_key
    src/main.c
    src/usb_kbd.c
    src/usb_descriptors.c
    src/editor.c
    src/flash_store.c
    src/crypto.c
    src/vault.c
    src/backup.c
    src/hotkeys.c
    src/buttons.c
    src/fido_glue.c
    src/fido_keys.c
    src/fido_manage.c
    src/ui.c
)

# ---------------------------------------------------------------------------
# pico-fido: FIDO2 / passkeys (AGPL-3.0, https://github.com/polhenarejos/pico-fido), pinned
# as a submodule. Small changes to its SDK live in patches/ and are applied here once.

set(PICO_FIDO_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-fido)
set(PICO_KEYS_DIR ${PICO_FIDO_DIR}/pico-keys-sdk)
if(NOT EXISTS ${PICO_KEYS_DIR}/picokeys_sdk_import.cmake)
    message(FATAL_ERROR "pico-fido is missing: run git submodule update --init --recursive")
endif()

# patch name -> directory it applies to
set(PATCH_DIR_pico-keys-sdk ${PICO_KEYS_DIR})
set(PATCH_DIR_pico-fido ${PICO_FIDO_DIR})
foreach(patch pico-keys-sdk pico-fido)
    set(patch_file ${CMAKE_CURRENT_LIST_DIR}/patches/${patch}.patch)
    set(patch_dir ${PATCH_DIR_${patch}})
    execute_process(COMMAND git apply --reverse --check ${patch_file}
                    WORKING_DIRECTORY ${patch_dir} RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(COMMAND git apply ${patch_file} WORKING_DIRECTORY ${patch_dir} RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch_file}")
        endif()
        message(STATUS "Applied ${patch}.patch")
    endif()
endforeach()

# pico-keys-sdk calls pico_sdk_init() itself, and needs the executable to exist already.
# Only the FIDO HID interface; no smart card (OATH/OTP) apps, no reboot into BOOTSEL on reset,
# and our own flash layout instead of the SDK's partition table (see src/flash_store.h).
set(USB_ITF_HID 1)
set(USB_ITF_CCID 0)
set(ENABLE_EMULATION 0)
set(PICOKEYS_NO_PARTITION_TABLE ON)
include(${PICO_KEYS_DIR}/cmake/options.cmake)
include(${PICO_KEYS_DIR}/cmake/version.cmake)
include(${PICO_KEYS_DIR}/picokeys_sdk_import.cmake)
SET_BUILD_NUMBER("${PICO_FIDO_DIR}")

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

target_include_directories(pass_key PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(pass_key PRIVATE -O2 -Wall)
target_link_libraries(pass_key PRIVATE
    pico_stdlib pico_multicore pico_unique_id pico_rand pico_flash
    hardware_flash hardware_watchdog hardware_exception
    tinyusb_device
    waveshare lvgl
)
# One TinyUSB configuration for everything, pico-keys-sdk's sources included (it has its own).
add_compile_definitions(CFG_TUSB_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/src/tusb_config.h")

set(FIDO_SOURCES)
foreach(f fido object_authorization object_provider resident_container files cmd_register
          cmd_authenticate cmd_version cbor cbor_reset cbor_get_info cbor_make_credential event
          known_apps cbor_client_pin credential cbor_get_assertion cbor_selection cbor_cred_mgmt
          cbor_config cbor_vendor fido_vault cbor_large_blobs management defs)
    list(APPEND FIDO_SOURCES ${PICO_FIDO_DIR}/src/fido/${f}.c)
endforeach()

# The SDK's RP2350 OTP backend would burn keys into OTP for good; src/fido_keys.c replaces it.
# Its USB descriptors are replaced by ours (src/usb_descriptors.c), which include the FIDO one.
get_target_property(PICOKEYS_SRCS picokeys_sdk INTERFACE_SOURCES)
list(FILTER PICOKEYS_SRCS EXCLUDE REGEX "/otp/otp_rp2350\\.c$|/usb/usb_descriptors\\.c$")
set_target_properties(picokeys_sdk PROPERTIES INTERFACE_SOURCES "${PICOKEYS_SRCS}")

target_sources(pass_key PRIVATE ${FIDO_SOURCES})
target_include_directories(pass_key PRIVATE ${PICO_FIDO_DIR}/src/fido)
target_link_libraries(pass_key PRIVATE picokeys_sdk mbedtls tinycbor)
target_compile_definitions(pass_key PRIVATE
    PICOKEYS_FLASH_DATA_START=0x180000   # FIDO_DATA_FLASH_START in src/flash_store.h
    PICOKEYS_FLASH_DATA_END=0x380000     # FIDO_DATA_FLASH_END
    PICOKEYS_NO_LED=1                    # GPIO 25 isn't a status LED on this board
    PICOKEYS_CUSTOM_BUTTON=1             # presence comes from the screen, see src/fido_glue.c
    PICO_PANIC_FUNCTION=passkey_panic    # panics are recorded in the FIDO log, src/fido_glue.c
    FORCE_BUTTON_WAIT=1                  # always ask on screen before signing (patches/pico-fido.patch)
    DEFAULT_ALWAYS_UV=1                  # FIDO PIN required for every passkey (patches/pico-fido.patch)
)

# Test builds for driving FIDO from the computer: every request is confirmed without the screen.
option(PASSKEY_AUTO_ALLOW "Confirm FIDO requests automatically (testing only)" OFF)
if(PASSKEY_AUTO_ALLOW)
    target_compile_definitions(pass_key PRIVATE FIDO_AUTO_ALLOW=1)
endif()

# Their code, their warnings: keep our -Wall output about our own files.
set_source_files_properties(${FIDO_SOURCES} ${PICOKEYS_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
# Our main() and TinyUSB HID callbacks are the real ones; theirs are renamed and called from ours.
set_source_files_properties(${PICO_KEYS_DIR}/src/main.c PROPERTIES COMPILE_DEFINITIONS "main=picokeys_sdk_main")
set_source_files_properties(${PICO_KEYS_DIR}/src/usb/hid/hid.c PROPERTIES COMPILE_DEFINITIONS
    "tud_hid_get_report_cb=picok_hid_get_report_cb;tud_hid_set_report_cb=picok_hid_set_report_cb;tud_hid_report_complete_cb=picok_hid_report_complete_cb")

# ---------------------------------------------------------------------------

pico_set_program_name(pass_key "Pass Key")
pico_enable_stdio_usb(pass_key 0)
pico_enable_stdio_uart(pass_key 0)
pico_add_extra_outputs(pass_key)

# pico-fido's data starts at FIRMWARE_MAX_SIZE: a bigger firmware would overwrite it.
add_custom_command(TARGET pass_key POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:pass_key>/pass_key.bin -DMAX=1572864
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_size.cmake
    VERBATIM)
