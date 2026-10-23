#pragma once

#include <stdbool.h>

// USB HID keyboard that types ASCII strings ('\t' = Tab, '\n' = Enter).
//
// usb_kbd_init() and usb_kbd_task() run on core 0, the rest is safe to call from core 1.

void usb_kbd_init(void);
void usb_kbd_task(void);

// Queues text for typing. Returns false if the host isn't connected or typing is in progress.
// The text is copied and the copy is wiped once typed.
bool usb_kbd_type(const char *text);
bool usb_kbd_busy(void);
bool usb_kbd_mounted(void);
