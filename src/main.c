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
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/exception.h"
#include "hardware/structs/systick.h"
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
#define STACK_PAINT 0x57AC57ACu

// Deepest core 1 stack use so far, in bytes (the stack is painted before core 1 starts).
uint32_t core1_stack_peak(void) {
    uint32_t i = 0;
    while (i < count_of(core1_stack) && core1_stack[i] == STACK_PAINT) i++;
    return (count_of(core1_stack) - i) * 4;
}

static void core1_entry(void) {
    // Like core 0: an overflow becomes a fault instead of overwriting whatever lies below.
    __asm volatile("msr msplim, %0" : : "r"(core1_stack));
    fido_core1_main();
}

// A hard fault on either core is recorded in the FIDO log (with the faulting address), then the
// watchdog reboots the board.
void __used hardfault_report(uint32_t *frame) {
    char what[40];
    snprintf(what, sizeof(what), "hard fault pc %08lx lr %08lx", (unsigned long)frame[6], (unsigned long)frame[5]);
    fido_log_crash_and_wait(what);
}

static void __attribute__((naked)) hardfault_entry(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b hardfault_report\n");
}

// SysTick samples where core 0 is every 10 ms (the stacked PC of the interrupted code), so
// after a hang the log shows the loop it was stuck in.
void __used systick_report(uint32_t *frame) {
    fido_log_pc_sample(frame[6]);
}

static void __attribute__((naked)) systick_entry(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b systick_report\n");
}

static void start_pc_sampler(void) {
    exception_set_exclusive_handler(SYSTICK_EXCEPTION, systick_entry);
    systick_hw->rvr = clock_get_hz(clk_sys) / 100 - 1;   // 10 ms
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7;                      // processor clock, interrupt, enable
}

static void __attribute__((noreturn, used)) run(void) {
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, hardfault_entry);
    fido_log_boot(watchdog_caused_reboot());
    DEV_Module_Init();   // I2C1 for touch, display DMA channel

    // pico-fido reads the flash chip ID while setting up, which briefly cuts off code running
    // from flash: do it before core 1 or any display and touch interrupts are running.
    fido_start();
    ui_init();
    tusb_init();

    for (uint32_t i = 0; i < count_of(core1_stack); i++) core1_stack[i] = STACK_PAINT;
    multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));
    // Flash writes from here on pause core 1, which it must be ready for.
    while (!flash_store_core1_ready()) tight_loop_contents();

    // If the loop ever stops (a deadlock), reboot instead of leaving a dead key plugged in.
    // Long enough for the slowest step, the ~2 s PIN key derivation.
    watchdog_enable(8000, true);
    start_pc_sampler();
    while (true) {
        watchdog_update();
        fido_log_stage(1);
        tud_task();
        fido_log_stage(2);
        usb_kbd_task();
        fido_log_stage(3);
        editor_task();
        fido_log_stage(4);
        fido_poll();
        fido_log_stage(5);
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
