#include "flash_store.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
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

static volatile bool core1_started;

void flash_store_core1_started(void) {
    core1_started = true;
}

bool flash_store_core1_ready(void) {
    return core1_started;
}

bool flash_safe_call(void (*func)(void *), void *param) {
    if (core1_started) return flash_safe_execute(func, param, 1000) == PICO_OK;
    // Before core 1 runs anything nothing else can be using the flash; interrupts suffice.
    uint32_t irq = save_and_disable_interrupts();
    func(param);
    restore_interrupts(irq);
    return true;
}

bool flash_run(uint32_t offset, const uint8_t *data, uint32_t len, bool erase) {
    flash_op_t op = {offset, data, len, erase};
    return flash_safe_call(flash_op_cb, &op);
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
