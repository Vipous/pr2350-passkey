#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define BOARD_TUD_RHPORT        0   // used by pico-keys-sdk's HID code
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_HID             3   // FIDO (instance 0), keyboard, media keys
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_AUDIO           0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// CTAPHID packets are 64 bytes; the keyboard and media endpoints still use 8-byte packets.
#define CFG_TUD_HID_EP_BUFSIZE  64

// Serial port for the editor page (Web Serial).
#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  512
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif
