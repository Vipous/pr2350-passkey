#pragma once

// Display, touch, PIN lock, password list, hotkeys and the security key confirmation.
// Runs on core 0 next to USB: ui_init() once, then ui_task() on every main loop pass.
void ui_init(void);
void ui_task(void);
