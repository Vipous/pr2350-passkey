#pragma once

#include <stdbool.h>
#include <stdint.h>

// Modifier bits of a boot keyboard report (left-hand keys).
#define KBD_MOD_CTRL  0x01
#define KBD_MOD_SHIFT 0x02
#define KBD_MOD_ALT   0x04   // Option on a Mac
#define KBD_MOD_GUI   0x08   // Cmd on a Mac, Win on Windows

// USB HID keyboard that types ASCII strings ('\t' = Tab, '\n' = Enter), presses key
// combinations and sends media keys (volume, play/pause) through a second HID interface.
//
// usb_kbd_task() runs on core 0 after tud_task(), the rest is safe to call from core 1.

void usb_kbd_task(void);

// Queues text for typing. Returns false if the host isn't connected or typing is in progress.
// The text is copied and the copy is wiped once typed.
bool usb_kbd_type(const char *text);
// Presses and releases one combination, e.g. KBD_MOD_GUI | KBD_MOD_SHIFT with HID usage 0x21 ("4").
bool usb_kbd_combo(uint8_t modifiers, uint8_t keycode);
// Presses and releases a consumer-control key (HID usage page 0x0C, e.g. 0xE9 volume up).
bool usb_kbd_media(uint16_t usage);
bool usb_kbd_busy(void);
bool usb_kbd_mounted(void);
