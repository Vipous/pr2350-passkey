#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

// (pico-keys-sdk defines USB_VID/USB_PID for itself, hence the different names.)
#define PASSKEY_VID 0xCafe   // TinyUSB test VID, fine for a personal device
#define PASSKEY_PID 0x4A23   // FIDO + keyboard + media keys + CDC; a new PID for each interface
                             // layout, so hosts don't reuse a cached one. Add new ones to PIDS in
                             // tools/passkey.html.

// FIDO comes first: pico-keys-sdk expects its HID interface (and HID instance) to be number 0.
// HID instances follow the order of the HID interfaces: 0 FIDO, 1 keyboard, 2 media keys.
enum { ITF_NUM_FIDO = 0, ITF_NUM_HID, ITF_NUM_MEDIA, ITF_NUM_CDC, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_FIDO_OUT  0x01
#define EPNUM_FIDO_IN   0x81
#define EPNUM_HID       0x82
#define EPNUM_MEDIA     0x83
#define EPNUM_CDC_NOTIF 0x84
#define EPNUM_CDC_OUT   0x05
#define EPNUM_CDC_IN    0x85
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // CDC uses an IAD, so the device class is "miscellaneous / common class".
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PASSKEY_VID,
    .idProduct = PASSKEY_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t desc_fido_report[] = {
    TUD_HID_REPORT_DESC_FIDO_U2F(CFG_TUD_HID_EP_BUFSIZE),
};

static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

// Media keys get their own interface, so the keyboard stays a plain boot keyboard.
static const uint8_t desc_media_report[] = {
    TUD_HID_REPORT_DESC_CONSUMER(),
};

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // Security key (CTAPHID), handled by pico-fido.
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_FIDO, 7, HID_ITF_PROTOCOL_NONE, sizeof(desc_fido_report),
                             EPNUM_FIDO_OUT, EPNUM_FIDO_IN, CFG_TUD_HID_EP_BUFSIZE, 10),
    // Boot protocol keyboard, so it also works in BIOS/UEFI and FileVault prompts.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report),
                       EPNUM_HID, 8, 1),
    TUD_HID_DESCRIPTOR(ITF_NUM_MEDIA, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_media_report),
                       EPNUM_MEDIA, 8, 10),
    // Serial port that the editor page talks to.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

static const char *const string_desc[] = {
    NULL,                 // 0: language, handled below
    "RP2350 Pass Key",    // 1: manufacturer
    "Pass Key Keyboard",  // 2: product
    NULL,                 // 3: serial, from the flash unique id
    "Pass Key",           // 4: HID interface
    "Pass Key Editor",    // 5: CDC interface
    "Pass Key Media",     // 6: media keys
    "Pass Key FIDO",      // 7: security key
};

// pico-keys-sdk's usb_init() expects these from its own usb_descriptors.c, which isn't built:
// it records the VID/PID and interface names there. Ours above are what the host sees.
tusb_desc_device_t desc_device;
char *string_desc_itf[5];
char *string_desc_arr[11];

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    static const uint8_t *const reports[] = {desc_fido_report, desc_hid_report, desc_media_report};
    return instance < 3 ? reports[instance] : NULL;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[33];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    size_t len;

    if (index == 0) {
        desc_str[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
        const char *str = string_desc[index];
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            str = serial;
        }
        len = strlen(str);
        if (len > 32) len = 32;
        for (size_t i = 0; i < len; i++) desc_str[1 + i] = (uint8_t)str[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
