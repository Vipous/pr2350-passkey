#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_HID             1
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_AUDIO           0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// Boot keyboard report: modifiers, reserved, 6 keycodes.
#define CFG_TUD_HID_EP_BUFSIZE  8

#endif
