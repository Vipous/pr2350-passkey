#include "flash_store.h"

#include "hardware/flash.h"
#include "pico/flash.h"

const uint8_t *flash_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}

typedef struct {
    uint32_t offset;
    const uint8_t *data;   // NULL: erase only
    uint32_t len;
    bool erase;
} flash_op_t;

static void flash_op_cb(void *param) {
    const flash_op_t *op = param;
    if (op->erase) flash_range_erase(op->offset, op->len);
    if (op->data) flash_range_program(op->offset, op->data, op->len);
}

bool flash_run(uint32_t offset, const uint8_t *data, uint32_t len, bool erase) {
    flash_op_t op = {offset, data, len, erase};
    return flash_safe_execute(flash_op_cb, &op, 1000) == PICO_OK;
}

uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}
