#pragma once

#include <stdbool.h>
#include <stdint.h>

// Glue between this firmware and pico-fido (FIDO2 / passkeys, from pico-keys-sdk).
//
// pico-fido runs its "card" threads (the CTAP command handlers, which block waiting for the
// user) on core 1, through a loop that stays up so core 1 can always be paused for flash
// writes. Everything else, including its USB and flash tasks, runs on core 0 from fido_poll().

// Core 0, before tusb_init(): flash, keys, settings and USB state of pico-fido.
void fido_start(void);
// Core 0, every main loop pass.
void fido_poll(void);
// Entry point for core 1.
void fido_core1_main(void);

// User presence: pico-fido asks for confirmation (sign-in, registration, reset).
typedef enum { FIDO_ASK_NONE, FIDO_ASK_SIGN_IN, FIDO_ASK_REGISTER, FIDO_ASK_OTHER } fido_ask_t;

fido_ask_t fido_presence_pending(void);   // FIDO_ASK_NONE when nothing waits
const char *fido_presence_site(void);     // relying party, e.g. "github.com"; "" if unknown
void fido_presence_answer(bool allow);
