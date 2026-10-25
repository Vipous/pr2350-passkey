# Fails the build if the firmware image is larger than MAX bytes.
file(SIZE ${BIN} size)
if(size GREATER MAX)
    message(FATAL_ERROR "Firmware is ${size} bytes, over the ${MAX} reserved for it (src/flash_store.h)")
endif()
message(STATUS "Firmware size: ${size} of ${MAX} bytes")
