#pragma once

#include <stdbool.h>
#include <stdint.h>

// Password storage in the last 36 KB of flash. Firmware updates via UF2 don't touch that area,
// so entries survive reflashing.
//
// Protection is chosen in the settings: none (plain text) or a PIN. With a PIN the entries are
// encrypted with a key derived from it, so the PIN is needed after every power-up.
//
// Call from one core only (the UI core); writes lock out the other core while flash is busy.

#define VAULT_NAME_LEN    32    // buffer sizes, NUL included
#define VAULT_LOGIN_LEN   63
#define VAULT_PASS_LEN    64
#define VAULT_MAX_ENTRIES 96
#define VAULT_SECRET_MAX  32    // longest PIN, with room to spare
#define VAULT_RECOVERY_LEN 24   // digits of the code that encrypts backup files

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

typedef enum { LOCK_NONE, LOCK_PIN } lock_mode_t;
typedef enum { UNLOCK_OK, UNLOCK_WRONG, UNLOCK_WIPED } unlock_result_t;

void vault_init(void);   // loads the newest copy; unlocked right away if there is no protection

lock_mode_t vault_lock_mode(void);
bool vault_is_unlocked(void);
int vault_attempts_left(void);
unlock_result_t vault_unlock(const char *secret);   // slow on purpose: about a second of key derivation
void vault_lock(void);                              // does nothing without protection
// Switches protection or the PIN (needs the vault unlocked) and re-saves everything under the new key.
bool vault_set_lock(lock_mode_t mode, const char *secret);
bool vault_wipe(void);   // erases entries and protection

int vault_count(void);
const vault_entry_t *vault_get(int index);
// Adds (index < 0) or replaces an entry and saves. Entries stay sorted by name;
// returns the entry's new index, or -1 if it could not be saved.
int vault_put(int index, const vault_entry_t *entry);
bool vault_delete(int index);

// Adds entries from a backup in one save; with replace, the current list is dropped first.
// Entries with the same name and login as an existing one are skipped, as are those that
// don't fit.
bool vault_merge(const vault_entry_t *list, int n, bool replace, int *added, int *skipped);

// Code for backup files, 24 digits. Made (and saved) the first time it's asked for; stored
// with the entries, so it is encrypted along with them when a PIN is set. NULL while locked.
const char *vault_recovery_code(void);
bool vault_has_recovery_code(void);
// Takes over the code of a restored backup, so it keeps working for new backups.
bool vault_set_recovery_code(const char *code);
