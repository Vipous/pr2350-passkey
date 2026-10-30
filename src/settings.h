#pragma once

#include <stdint.h>

// Board settings that survive a reboot, in their own flash sector (SETTINGS_FLASH_OFFSET).

typedef struct {
    uint8_t brightness;   // 10..100
    uint8_t character;    // pet_character_t: who lives on the home screen
} settings_t;

void settings_init(void);            // loads them, or the defaults
const settings_t *settings_get(void);
void settings_save(const settings_t *s);   // writes only when something changed
