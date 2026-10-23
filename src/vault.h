#pragma once

#include <stdbool.h>
#include <stdint.h>

// Password storage: an encrypted, PIN-protected list kept in the last 36 KB of flash.
// Firmware updates via UF2 don't touch that area, so entries survive reflashing.
//
// Call from one core only (the UI core); writes lock out the other core while flash is busy.

#define VAULT_NAME_LEN    32    // buffer sizes, NUL included
#define VAULT_LOGIN_LEN   63
#define VAULT_PASS_LEN    64
#define VAULT_MAX_ENTRIES 96

#define VAULT_PIN_MIN 4
#define VAULT_PIN_MAX 12

// Wrong PINs in a row before everything is erased; 0 disables wiping.
#define VAULT_MAX_ATTEMPTS 10

#define VAULT_FLAG_ENTER 0x01   // press Enter after typing
#define VAULT_FLAG_LOGIN 0x02   // type login and Tab before the password

typedef struct {
    char name[VAULT_NAME_LEN];
    char login[VAULT_LOGIN_LEN];
    char pass[VAULT_PASS_LEN];
    uint8_t flags;
} vault_entry_t;

typedef enum { UNLOCK_OK, UNLOCK_WRONG_PIN, UNLOCK_WIPED } unlock_result_t;

bool vault_exists(void);   // false before the first PIN is set
bool vault_is_unlocked(void);
int vault_attempts_left(void);

bool vault_create(const char *pin);
unlock_result_t vault_unlock(const char *pin);   // slow on purpose: about a second of key derivation
void vault_lock(void);
bool vault_change_pin(const char *new_pin);
void vault_wipe(void);

int vault_count(void);
const vault_entry_t *vault_get(int index);
// Adds (index < 0) or replaces an entry and saves. Entries stay sorted by name;
// returns the entry's new index, or -1 if it could not be saved.
int vault_put(int index, const vault_entry_t *entry);
bool vault_delete(int index);
