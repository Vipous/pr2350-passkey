#pragma once

#include <stdbool.h>

// Physical buttons used to open the mode menu: BOOT (read through the flash chip-select line)
// and, if the board has an AXP2101 power chip, a short press of its PWR key.
// Call from the UI core; the touch controller shares I2C1 with the AXP2101.

void buttons_init(void);
bool buttons_pressed(void);   // true once per press, polled every ~100 ms
