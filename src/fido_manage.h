#pragma once

#include <stdbool.h>
#include <stdint.h>

// Passkeys stored on the FIDO key, for the management page (editor commands FIDO_*): listing,
// deleting, and when each one was made and last used. Core 0 only, except where noted.
//
// The board has no clock of its own. The page sets it when it connects; until then, uses are
// remembered by uptime and dated once the clock is known (if the board stays powered).

void fido_manage_init(void);    // after fido_start()
void fido_manage_task(void);    // every main loop pass: records uses, saves them to flash

void fido_manage_set_time(uint32_t unix_time);
bool fido_manage_has_time(void);

typedef struct {
    bool pin_set;
    uint8_t retries;    // PIN attempts left
    int count, max;     // passkeys stored, room for
} fido_info_t;
void fido_manage_info(fido_info_t *info);

// Checks the FIDO PIN (counts as an attempt, like a browser's). 0 when right, else a CTAP error
// code: 0x31 wrong (retries left in *retries), 0x32 blocked, 0x34 retry after replugging,
// 0x35 not set.
int fido_manage_unlock(const char *pin, uint8_t *retries);

typedef struct {
    int slot;
    char id[17];          // short ID of the passkey, hex; FIDO_DEL checks it against the slot
    char rp[129];         // site, e.g. "github.com"
    char user[65], display[65];
    uint32_t created, last_used;   // Unix time, 0 if unknown
    uint32_t uses;        // sign-ins since this firmware started counting
    int32_t alg;          // COSE algorithm, -7 = ES256
    uint8_t cred_protect;
} fido_passkey_t;

// After a successful unlock: the next stored passkey at or after *slot, then *slot moves past it.
// False when there are no more.
bool fido_manage_next(int *slot, fido_passkey_t *out);

// After a successful unlock. 0 deleted, -1 no such passkey (or the ID doesn't match), -2 failed.
int fido_manage_delete(int slot, const char *id);

// FIDO busy with a browser request; the commands above wait until it isn't.
bool fido_manage_busy(void);

// Core 1, from pico-fido: a passkey was created or used.
void fido_credential_event(bool created, const uint8_t *resident_id);
