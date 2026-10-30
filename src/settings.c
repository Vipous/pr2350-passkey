#include "settings.h"

#include <string.h>

#include "flash_store.h"
#include "hardware/flash.h"

#define MAGIC 0x31535350u   // "PSS1"

typedef struct {
    uint32_t magic;
    settings_t s;
    uint32_t crc;   // over s
} store_t;

static settings_t current = {.brightness = 70, .character = 0};

void settings_init(void) {
    const store_t *f = (const store_t *)flash_ptr(SETTINGS_FLASH_OFFSET);
    if (f->magic == MAGIC && crc32(&f->s, sizeof(f->s)) == f->crc) current = f->s;
    if (current.brightness < 10 || current.brightness > 100) current.brightness = 70;
}

const settings_t *settings_get(void) {
    return &current;
}

void settings_save(const settings_t *s) {
    if (memcmp(s, &current, sizeof(current)) == 0) return;
    current = *s;
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    store_t *st = (store_t *)page;
    st->magic = MAGIC;
    st->s = current;
    st->crc = crc32(&st->s, sizeof(st->s));
    if (flash_run(SETTINGS_FLASH_OFFSET, NULL, FLASH_SECTOR_SIZE, true))
        flash_run(SETTINGS_FLASH_OFFSET, page, sizeof(page), false);
}
