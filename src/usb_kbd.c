#include "usb_kbd.h"

#include <ctype.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "fido_glue.h"
#include "tusb.h"

// Pause between HID reports. 1 ms polling would allow faster typing, but remote desktops,
// VMs and some login screens drop keys that come in too quickly.
#define KEY_DELAY_US 6000

#define TEXT_MAX 256

static const uint8_t ascii_to_key[128][2] = {HID_ASCII_TO_KEYCODE};

// HID instances, in descriptor order (see usb_descriptors.c).
#define HID_FIDO     0   // pico-fido's CTAPHID
#define HID_KEYBOARD 1
#define HID_MEDIA    2

// Core 1 fills in the action and then raises `pending`; core 0 sends it, wipes the text and
// clears `pending`.
typedef enum { ACT_TEXT, ACT_COMBO, ACT_MEDIA } action_t;
static action_t action;
static char text[TEXT_MAX];
static uint8_t combo_mods, combo_key;
static uint16_t media_usage;
static volatile bool pending;
static volatile bool mounted;
static volatile uint8_t host_leds;   // Caps Lock etc., reported by the host

static size_t pos;
static bool key_down;
static absolute_time_t next_report;

bool usb_kbd_type(const char *s) {
    if (pending || !mounted) return false;
    size_t n = strlen(s);
    if (n >= TEXT_MAX) n = TEXT_MAX - 1;
    action = ACT_TEXT;
    memcpy(text, s, n);
    text[n] = 0;
    __dmb();
    pending = true;
    return true;
}

bool usb_kbd_combo(uint8_t modifiers, uint8_t keycode) {
    if (pending || !mounted) return false;
    action = ACT_COMBO;
    combo_mods = modifiers;
    combo_key = keycode;
    __dmb();
    pending = true;
    return true;
}

bool usb_kbd_media(uint16_t usage) {
    if (pending || !mounted) return false;
    action = ACT_MEDIA;
    media_usage = usage;
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

// One key combination or media key: press, then release on the next step.
static void press_step(void) {
    if (action == ACT_MEDIA) {
        if (!tud_hid_n_ready(HID_MEDIA)) return;
        uint16_t usage = key_down ? 0 : media_usage;
        tud_hid_n_report(HID_MEDIA, 0, &usage, sizeof(usage));
    } else {
        if (!tud_hid_n_ready(HID_KEYBOARD)) return;
        uint8_t keycode[6] = {key_down ? 0 : combo_key};
        tud_hid_n_keyboard_report(HID_KEYBOARD, 0, key_down ? 0 : combo_mods, keycode);
    }
    if (key_down) {
        finish();
        return;
    }
    key_down = true;
    next_report = make_timeout_time_us(KEY_DELAY_US);
}

static void type_step(void) {
    if (!pending) return;

    // Never keep a password around to type it later into whatever is focused then.
    if (!tud_mounted() || tud_suspended()) {
        finish();
        return;
    }
    if (!time_reached(next_report)) return;
    if (action != ACT_TEXT) {
        press_step();
        return;
    }
    if (!tud_hid_n_ready(HID_KEYBOARD)) return;

    if (key_down) {
        tud_hid_n_keyboard_report(HID_KEYBOARD, 0, 0, NULL);
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
        tud_hid_n_keyboard_report(HID_KEYBOARD, 0, modifier, keycode);
        key_down = true;
    }
    next_report = make_timeout_time_us(KEY_DELAY_US);
}

void usb_kbd_task(void) {
    mounted = tud_mounted();
    type_step();
}

// ---------------------------------------------------------------------------
// TinyUSB HID callbacks. pico-fido's hid.c is built with its callbacks renamed to picok_hid_*;
// reports on the FIDO instance go there.

uint16_t picok_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                                 uint8_t *buffer, uint16_t reqlen);
void picok_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                             uint8_t const *buffer, uint16_t bufsize);
void picok_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len);

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    if (instance == HID_FIDO) return picok_hid_get_report_cb(instance, report_id, report_type, buffer, reqlen);
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    if (instance == HID_FIDO) {
        fido_log_packet(true, buffer, bufsize);
        picok_hid_set_report_cb(instance, report_id, report_type, buffer, bufsize);
        return;
    }
    (void)report_id;
    if (instance == HID_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) host_leds = buffer[0];
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    if (instance == HID_FIDO) {
        fido_log_packet(false, report, len);
        picok_hid_report_complete_cb(instance, report, len);
    }
}
