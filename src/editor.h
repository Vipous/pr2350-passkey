#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Line protocol on the USB serial port, used by tools/passkey.html to manage the list.
//
// Requests are one line each, fields separated by Tab. Values are printable ASCII.
//   HELLO                                  -> OK PassKey <protocol> <max entries> locked|unlocked none|pin
//   LIST                                   -> E <i> <name> <login> <flags> <password length> ... OK
//   ADD  <name> <login> <flags> <password> -> OK <i>
//   SET  <i> <name> <login> <flags> [<password>]   (no password: keep the stored one) -> OK <i>
//   DEL  <i>                               -> OK
//   WIPE                                   -> OK
//   BACKUP                                 -> B <base64> ... OK <bytes>   (encrypted backup file)
//   RESTORE <bytes>, then R <base64> per chunk, then RESTORE_END merge|replace -> OK <entries>;
//     the board then asks for the recovery code on screen, and
//   RESTORE_STATUS                         -> OK waiting | done <added> <skipped> | cancelled | failed | idle
// Errors come back as "ERR <message>"; "ERR locked" means the board must be unlocked on screen
// first (WIPE still works). Passwords are never sent back to the computer.
//
// editor_task() runs on core 0 after tud_task(); the vault lives on core 1, which carries out
// the requests in editor_service().

void editor_task(void);
bool editor_connected(void);

typedef enum { EDITOR_IDLE, EDITOR_READ, EDITOR_CHANGED } editor_activity_t;

// Core 1: runs a pending vault request and says whether there was one and if the list changed.
editor_activity_t editor_service(void);
// Core 1: lock state reported by HELLO.
void editor_set_state(bool locked, int lock_mode);

// Core 1: a restore is uploaded and waits for the recovery code typed on the board.
bool editor_restore_waiting(void);
const uint8_t *editor_restore_data(size_t *len, bool *replace, int *entries);
void editor_restore_finish(bool done, int added, int skipped);
