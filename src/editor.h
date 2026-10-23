#pragma once

#include <stdbool.h>

// Line protocol on the USB serial port, used by tools/passkey.html to manage the list.
//
// Requests are one line each, fields separated by Tab. Values are printable ASCII.
//   HELLO                                  -> OK PassKey <protocol> <max entries>
//   LIST                                   -> E <i> <name> <login> <flags> <password length> ... OK
//   ADD  <name> <login> <flags> <password> -> OK <i>
//   SET  <i> <name> <login> <flags> [<password>]   (no password: keep the stored one) -> OK <i>
//   DEL  <i>                               -> OK
//   WIPE                                   -> OK
// Errors come back as "ERR <message>". Passwords are never sent back to the computer.
//
// editor_task() runs on core 0 after tud_task(); the vault lives on core 1, which carries out
// the requests in editor_service().

void editor_task(void);
bool editor_connected(void);

// Core 1: runs a pending vault request. Returns true if the list changed.
bool editor_service(void);
