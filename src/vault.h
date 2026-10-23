#pragma once

#include <stdbool.h>
#include <stdint.h>

// Password storage: a list kept in the last 32 KB of flash, unencrypted.
// Firmware updates via UF2 don't touch that area, so entries survive reflashing.
//
// Call from one core only (the UI core); writes lock out the other core while flash is busy.

#define VAULT_NAME_LEN    32    // buffer sizes, NUL included
#define VAULT_LOGIN_LEN   63
#define VAULT_PASS_LEN    64
#define VAULT_MAX_ENTRIES 96

#define VAULT_FLAG_ENTER 0x01   // press Enter after typing
#define VAULT_FLAG_LOGIN 0x02   // type login and Tab before the password

typedef struct {
    char name[VAULT_NAME_LEN];
    char login[VAULT_LOGIN_LEN];
    char pass[VAULT_PASS_LEN];
    uint8_t flags;
} vault_entry_t;

void vault_init(void);   // loads the newest valid copy from flash

int vault_count(void);
const vault_entry_t *vault_get(int index);
// Adds (index < 0) or replaces an entry and saves. Entries stay sorted by name;
// returns the entry's new index, or -1 if it could not be saved.
int vault_put(int index, const vault_entry_t *entry);
bool vault_delete(int index);
bool vault_wipe(void);
