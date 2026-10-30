#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flash layout (the board config says 4 MB; everything stays inside that):
//
//   0x000000  firmware, at most FIRMWARE_MAX_SIZE
//   0x180000  pico-fido data (credentials, FIDO settings), 2 MB
//   0x380000  free
//   end-56K   board settings (brightness, home screen character), 1 sector
//   end-52K   FIDO passkey dates and use counts, 2 sectors
//   end-44K   FIDO master key, 1 sector
//   end-40K   hotkeys, 1 sector
//   end-36K   vault: attempt counter, 1 sector | slot 0, 16 KB | slot 1, 16 KB
//
// UF2 firmware updates only write the firmware area, so everything else survives reflashing.

#define FIRMWARE_MAX_SIZE      (0x180000)
#define FIDO_DATA_FLASH_START  (0x180000)
#define FIDO_DATA_FLASH_END    (0x380000)

#define VAULT_FLASH_SIZE       (36 * 1024)
#define VAULT_FLASH_OFFSET     (PICO_FLASH_SIZE_BYTES - VAULT_FLASH_SIZE)
#define HOTKEYS_FLASH_OFFSET   (VAULT_FLASH_OFFSET - FLASH_SECTOR_SIZE)
#define FIDO_KEY_FLASH_OFFSET  (HOTKEYS_FLASH_OFFSET - FLASH_SECTOR_SIZE)
#define FIDO_USE_FLASH_SIZE    (2 * FLASH_SECTOR_SIZE)
#define FIDO_USE_FLASH_OFFSET  (FIDO_KEY_FLASH_OFFSET - FIDO_USE_FLASH_SIZE)
#define SETTINGS_FLASH_OFFSET  (FIDO_USE_FLASH_OFFSET - FLASH_SECTOR_SIZE)

// Memory-mapped read access to a flash offset.
const uint8_t *flash_ptr(uint32_t offset);

// Erases (if asked) and then programs (if data isn't NULL), with the other core paused.
// Offsets and lengths follow the flash rules: sectors for erasing, pages for programming.
bool flash_run(uint32_t offset, const uint8_t *data, uint32_t len, bool erase);

// Runs func with flash access safe: other core paused (once it's running), interrupts off.
// func and everything it calls must run from RAM if it touches the flash interface.
bool flash_safe_call(void (*func)(void *), void *param);

// Core 1 calls this once it can be paused; until then it must not be running from flash.
void flash_store_core1_started(void);
bool flash_store_core1_ready(void);

uint32_t crc32(const void *data, size_t n);
