#pragma once

#include <stdbool.h>
#include <stdint.h>

// Hotkey panel: up to HOTKEYS_MAX tiles, each sending one key combination or media key.
// Kept in its own flash sector, unencrypted (shortcuts aren't secret), so the panel and the
// vault don't depend on each other. Call from the UI core.

#define HOTKEYS_MAX       4    // one screen, 2 x 2, no scrolling
#define HOTKEY_LABEL_LEN  20   // buffer size, NUL included

typedef enum { HOTKEY_COMBO, HOTKEY_MEDIA } hotkey_type_t;

typedef struct {
    char label[HOTKEY_LABEL_LEN];
    uint8_t type;   // hotkey_type_t
    uint8_t mods;   // KBD_MOD_* for HOTKEY_COMBO
    uint16_t code;  // HID keyboard usage (combo) or consumer usage (media)
} hotkey_t;

void hotkeys_init(void);   // loads the saved panel, or the Mac defaults
int hotkeys_count(void);
const hotkey_t *hotkeys_get(int index);
bool hotkeys_set(const hotkey_t *list, int n);   // replaces the panel and saves it
bool hotkeys_reset(void);                        // back to the defaults

// Readable form for a tile, e.g. "Ctrl+Cmd+Q" or "Volume up".
void hotkey_describe(const hotkey_t *k, char *out, int size);
