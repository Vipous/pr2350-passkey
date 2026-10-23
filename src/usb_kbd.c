#include "usb_kbd.h"

#include <ctype.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "tusb.h"

// Pause between HID reports. 1 ms polling would allow faster typing, but remote desktops,
// VMs and some login screens drop keys that come in too quickly.
#define KEY_DELAY_US 6000

#define TEXT_MAX 256

static const uint8_t ascii_to_key[128][2] = {HID_ASCII_TO_KEYCODE};

// Core 1 fills `text` and then raises `pending`; core 0 types it, wipes it and clears `pending`.
static char text[TEXT_MAX];
static volatile bool pending;
static volatile bool mounted;
static volatile uint8_t host_leds;   // Caps Lock etc., reported by the host

static size_t pos;
static bool key_down;
static absolute_time_t next_report;

void usb_kbd_init(void) {
    tusb_init();
}

bool usb_kbd_type(const char *s) {
    if (pending || !mounted) return false;
    size_t n = strlen(s);
    if (n >= TEXT_MAX) n = TEXT_MAX - 1;
    memcpy(text, s, n);
    text[n] = 0;
    __dmb();
    pending = true;
    return true;
}

bool usb_kbd_busy(void) {
    return pending;
}

bool usb_kbd_mounted(void) {
    return mounted;
}

static void finish(void) {
    memset(text, 0, sizeof(text));
    pos = 0;
    key_down = false;
    __dmb();
    pending = false;
}

static void type_step(void) {
    if (!pending) return;

    // Never keep a password around to type it later into whatever is focused then.
    if (!tud_mounted() || tud_suspended()) {
        finish();
        return;
    }
    if (!time_reached(next_report) || !tud_hid_ready()) return;

    if (key_down) {
        tud_hid_keyboard_report(0, 0, NULL);
        key_down = false;
        pos++;
    } else {
        uint8_t ch = (uint8_t)text[pos];
        if (ch == 0) {
            finish();
            return;
        }
        uint8_t keycode[6] = {0};
        uint8_t modifier = 0;
        if (ch < 128) {
            keycode[0] = ascii_to_key[ch][1];
            if (ascii_to_key[ch][0]) modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
            // With Caps Lock on, Shift gives lowercase letters.
            if ((host_leds & KEYBOARD_LED_CAPSLOCK) && isalpha(ch)) modifier ^= KEYBOARD_MODIFIER_LEFTSHIFT;
        }
        if (keycode[0] == 0) {   // nothing to type for this character
            pos++;
            return;
        }
        tud_hid_keyboard_report(0, modifier, keycode);
        key_down = true;
    }
    next_report = make_timeout_time_us(KEY_DELAY_US);
}

void usb_kbd_task(void) {
    tud_task();
    mounted = tud_mounted();
    type_step();
}

// ---------------------------------------------------------------------------
// TinyUSB HID callbacks

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) host_leds = buffer[0];
}
