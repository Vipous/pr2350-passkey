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

// Diagnostic log of CTAPHID traffic and presence events (core 0 only), read by the editor's
// FIDOLOG command: one line per entry, oldest first.
void fido_log_packet(bool from_host, const uint8_t *packet, int len);
int fido_log_count(void);
void fido_log_format(int index, char *out, int size);
void fido_log_clear(void);
// At boot: keeps the previous log after a watchdog reset (a hang), otherwise starts a new one.
void fido_log_boot(bool after_watchdog);
// Main loop step now running; recorded in the log's boot entry if the watchdog fires.
void fido_log_stage(uint8_t stage);
// What stopped the board before the last watchdog reset (panic text or fault address), or "".
const char *fido_log_crash(void);
void __attribute__((noreturn)) fido_log_crash_and_wait(const char *what);
// Core 0 program counter samples; fido_log_pcs() returns those from before the last watchdog reset.
void fido_log_pc_sample(uint32_t pc);
int fido_log_pcs(uint32_t out[16]);
