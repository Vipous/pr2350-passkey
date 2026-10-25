#include "hotkeys.h"

#include <stdio.h>
#include <string.h>

#include "flash_store.h"
#include "hardware/flash.h"
#include "usb_kbd.h"

#define MAGIC 0x31484B50u   // "PKH1"

#define STORE_SLOTS 12   // room in the flash format; earlier builds kept up to 12 tiles

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t crc;   // over keys[0..count)
    hotkey_t keys[STORE_SLOTS];
} store_t;

_Static_assert(sizeof(hotkey_t) == 24, "hotkey layout is stored in flash");
_Static_assert(sizeof(store_t) <= FLASH_PAGE_SIZE * 2, "fits two flash pages");

// A Mac set to start with; the editor page has a Windows one too.
static const hotkey_t defaults[HOTKEYS_MAX] = {
    {"Lock screen", HOTKEY_COMBO, KBD_MOD_CTRL | KBD_MOD_GUI, 0x14},   // Ctrl+Cmd+Q
    {"Screenshot", HOTKEY_COMBO, KBD_MOD_GUI | KBD_MOD_SHIFT, 0x21},   // Cmd+Shift+4
    {"Mute Zoom", HOTKEY_COMBO, KBD_MOD_GUI | KBD_MOD_SHIFT, 0x04},    // Cmd+Shift+A
    {"Play / Pause", HOTKEY_MEDIA, 0, 0xCD},
};

static hotkey_t keys[HOTKEYS_MAX];
static int count;

static void load_defaults(void) {
    memset(keys, 0, sizeof(keys));
    count = sizeof(defaults) / sizeof(defaults[0]);
    memcpy(keys, defaults, sizeof(defaults));
}

void hotkeys_init(void) {
    const store_t *s = (const store_t *)flash_ptr(HOTKEYS_FLASH_OFFSET);
    if (s->magic != MAGIC || s->count > STORE_SLOTS || crc32(s->keys, s->count * sizeof(hotkey_t)) != s->crc) {
        load_defaults();
        return;
    }
    // A panel saved by an earlier build may have more tiles than fit now: keep the first ones.
    count = s->count < HOTKEYS_MAX ? (int)s->count : HOTKEYS_MAX;
    memset(keys, 0, sizeof(keys));
    memcpy(keys, s->keys, count * sizeof(hotkey_t));
}

int hotkeys_count(void) {
    return count;
}

const hotkey_t *hotkeys_get(int index) {
    return index >= 0 && index < count ? &keys[index] : NULL;
}

bool hotkeys_set(const hotkey_t *list, int n) {
    if (n < 0 || n > HOTKEYS_MAX) return false;
    static uint8_t page[FLASH_PAGE_SIZE * 2];
    store_t *s = (store_t *)page;
    memset(page, 0xFF, sizeof(page));
    memset(s->keys, 0, sizeof(s->keys));
    for (int i = 0; i < n; i++) {
        s->keys[i] = list[i];
        s->keys[i].label[HOTKEY_LABEL_LEN - 1] = 0;
    }
    s->magic = MAGIC;
    s->count = (uint32_t)n;
    s->crc = crc32(s->keys, n * sizeof(hotkey_t));
    // A torn write fails the CRC and the panel falls back to the defaults, which is harmless.
    if (!flash_run(HOTKEYS_FLASH_OFFSET, NULL, FLASH_SECTOR_SIZE, true) ||
        !flash_run(HOTKEYS_FLASH_OFFSET, page, sizeof(page), false))
        return false;
    hotkeys_init();
    return count == n;
}

bool hotkeys_reset(void) {
    bool ok = flash_run(HOTKEYS_FLASH_OFFSET, NULL, FLASH_SECTOR_SIZE, true);
    load_defaults();
    return ok;
}

static const char *key_name(uint16_t code, char *buf) {
    static const struct { uint8_t code; const char *name; } names[] = {
        {0x28, "Enter"}, {0x29, "Esc"}, {0x2A, "Backspace"}, {0x2B, "Tab"}, {0x2C, "Space"},
        {0x2D, "-"}, {0x2E, "="}, {0x2F, "["}, {0x30, "]"}, {0x31, "\\"}, {0x33, ";"}, {0x34, "'"},
        {0x35, "`"}, {0x36, ","}, {0x37, "."}, {0x38, "/"}, {0x46, "PrtSc"}, {0x49, "Insert"},
        {0x4A, "Home"}, {0x4B, "PgUp"}, {0x4C, "Delete"}, {0x4D, "End"}, {0x4E, "PgDn"},
        {0x4F, "Right"}, {0x50, "Left"}, {0x51, "Down"}, {0x52, "Up"},
    };
    if (code >= 0x04 && code <= 0x1D) sprintf(buf, "%c", 'A' + code - 0x04);
    else if (code >= 0x1E && code <= 0x26) sprintf(buf, "%c", '1' + code - 0x1E);
    else if (code == 0x27) strcpy(buf, "0");
    else if (code >= 0x3A && code <= 0x45) sprintf(buf, "F%d", code - 0x3A + 1);
    else if (code >= 0x68 && code <= 0x73) sprintf(buf, "F%d", code - 0x68 + 13);
    else {
        sprintf(buf, "0x%02X", code);
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (names[i].code == code) strcpy(buf, names[i].name);
    }
    return buf;
}

void hotkey_describe(const hotkey_t *k, char *out, int size) {
    if (k->type == HOTKEY_MEDIA) {
        static const struct { uint16_t code; const char *name; } media[] = {
            {0xCD, "Play / Pause"}, {0xB5, "Next track"}, {0xB6, "Previous track"}, {0xB7, "Stop"},
            {0xE2, "Mute"}, {0xE9, "Volume up"}, {0xEA, "Volume down"},
            {0x6F, "Brightness up"}, {0x70, "Brightness down"},
        };
        snprintf(out, size, "Media 0x%02X", k->code);
        for (unsigned i = 0; i < sizeof(media) / sizeof(media[0]); i++)
            if (media[i].code == k->code) snprintf(out, size, "%s", media[i].name);
        return;
    }
    char key[12];
    snprintf(out, size, "%s%s%s%s%s", k->mods & KBD_MOD_CTRL ? "Ctrl+" : "", k->mods & KBD_MOD_ALT ? "Opt+" : "",
             k->mods & KBD_MOD_SHIFT ? "Shift+" : "", k->mods & KBD_MOD_GUI ? "Cmd+" : "", key_name(k->code, key));
}
