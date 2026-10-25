#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vault.h"

// Backup files: all entries, encrypted with a key derived from the board's 24-digit recovery
// code. The code never leaves the board, so the computer only ever sees ciphertext.
//
// Layout: "PKB1", u16 count, u16 entry size, 16-byte salt, 16-byte nonce, entries (encrypted),
// 32-byte HMAC over everything before it.

#define BACKUP_HEADER_SIZE 40
#define BACKUP_MAC_SIZE    32
#define BACKUP_MAX_SIZE    (BACKUP_HEADER_SIZE + VAULT_MAX_ENTRIES * sizeof(vault_entry_t) + BACKUP_MAC_SIZE)

// Writes a backup of the unlocked vault; returns its size, or 0 when locked or out of space.
size_t backup_export(uint8_t *out, size_t size);

// Number of entries in a backup, or -1 if it isn't one. Doesn't need the code.
int backup_peek(const uint8_t *data, size_t len);

typedef enum { RESTORE_OK, RESTORE_WRONG_CODE, RESTORE_BAD_FILE, RESTORE_SAVE_FAILED } restore_result_t;

// Decrypts with the code typed on the board and merges into (or replaces) the vault.
restore_result_t backup_import(const uint8_t *data, size_t len, const char *code, bool replace,
                               int *added, int *skipped);
