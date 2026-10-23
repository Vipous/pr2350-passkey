// Pass Key: the board shows up as a USB keyboard and types the password picked on screen.
//
// core 0: TinyUSB HID keyboard
// core 1: AMOLED + touch UI (LVGL), encrypted password vault in flash

#include "DEV_Config.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ui.h"
#include "usb_kbd.h"

// LVGL needs more than the SDK's default 2 KB core 1 stack; PBKDF2 adds a little on top.
static uint32_t core1_stack[4096];

int main(void) {
    DEV_Module_Init();   // I2C1 for touch, display DMA channel

    usb_kbd_init();
    // Lets the vault on core 1 pause this core while it erases or programs flash.
    flash_safe_execute_core_init();

    multicore_launch_core1_with_stack(ui_main, core1_stack, sizeof(core1_stack));

    while (true) {
        usb_kbd_task();
    }
}
