// Pass Key: the board shows up as a USB keyboard and types the password picked on screen.
// The list is managed from a computer: tools/passkey.html talks to the board's serial port.
//
// core 0: TinyUSB (HID keyboard + CDC serial for the editor)
// core 1: AMOLED + touch UI (LVGL), password list in flash

#include "DEV_Config.h"
#include "editor.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "ui.h"
#include "usb_kbd.h"

// LVGL needs more than the SDK's default 2 KB core 1 stack.
static uint32_t core1_stack[4096];

int main(void) {
    DEV_Module_Init();   // I2C1 for touch, display DMA channel

    tusb_init();
    // Lets the vault on core 1 pause this core while it erases or programs flash.
    flash_safe_execute_core_init();

    multicore_launch_core1_with_stack(ui_main, core1_stack, sizeof(core1_stack));

    while (true) {
        tud_task();
        usb_kbd_task();
        editor_task();
    }
}
