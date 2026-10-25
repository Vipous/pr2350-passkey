// Pass Key: the board shows up as a USB keyboard and types the password picked on screen,
// sends hotkeys, and works as a FIDO2 security key (pico-fido). The list is managed from a
// computer: tools/passkey.html talks to the board's serial port.
//
// core 0: TinyUSB (FIDO, keyboard, media keys, CDC serial), AMOLED + touch UI (LVGL),
//         password vault, pico-fido's USB and flash tasks
// core 1: pico-fido's command handlers (they block while waiting for the user)

#include "DEV_Config.h"
#include "editor.h"
#include "fido_glue.h"
#include "flash_store.h"

#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "ui.h"
#include "usb_kbd.h"

// The SDK gives core 0 a 2 KB stack; LVGL wants more, so main() moves onto this one.
static uint32_t main_stack[4096];
// pico-fido's handlers do the elliptic-curve maths on core 1. Registration with a PIN peaks
// near 11 KB (8 KB used to overflow and hang the board); the rest is headroom.
static uint32_t core1_stack[8192];

static void core1_entry(void) {
    // Like core 0: an overflow becomes a fault instead of overwriting whatever lies below.
    __asm volatile("msr msplim, %0" : : "r"(core1_stack));
    fido_core1_main();
}

static void __attribute__((noreturn, used)) run(void) {
    DEV_Module_Init();   // I2C1 for touch, display DMA channel

    // pico-fido reads the flash chip ID while setting up, which briefly cuts off code running
    // from flash: do it before core 1 or any display and touch interrupts are running.
    fido_start();
    ui_init();
    tusb_init();

    multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));
    // Flash writes from here on pause core 1, which it must be ready for.
    while (!flash_store_core1_ready()) tight_loop_contents();

    // If the loop ever stops (a deadlock), reboot instead of leaving a dead key plugged in.
    // Long enough for the slowest step, the ~2 s PIN key derivation.
    // A crash (hard fault, panic) ends the same way: the watchdog notices and reboots.
    watchdog_enable(8000, true);
    while (true) {
        watchdog_update();
        tud_task();
        usb_kbd_task();
        editor_task();
        fido_poll();
        ui_task();
    }
}

int main(void) {
    // Switch to the big stack and never come back; MSPLIM turns an overflow into a fault.
    __asm volatile(
        "msr msplim, %0\n"
        "msr msp, %1\n"
        "b run\n"
        :
        : "r"(main_stack), "r"(main_stack + sizeof(main_stack) / sizeof(main_stack[0]))
        : "memory");
    __builtin_unreachable();
}
