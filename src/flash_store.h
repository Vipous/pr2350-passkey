#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flash helpers shared by the vault and the hotkey panel. Data lives at the end of the chip:
//
//   [ hotkeys, 1 sector ][ vault: attempt counter, 1 sector | slot 0, 16 KB | slot 1, 16 KB ]
//
// UF2 firmware updates never write there, so both survive reflashing.

#define VAULT_FLASH_SIZE   (36 * 1024)
#define VAULT_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - VAULT_FLASH_SIZE)
#define HOTKEYS_FLASH_OFFSET (VAULT_FLASH_OFFSET - FLASH_SECTOR_SIZE)

// Memory-mapped read access to a flash offset.
const uint8_t *flash_ptr(uint32_t offset);

// Erases (if asked) and then programs (if data isn't NULL), with the other core paused.
// Offsets and lengths follow the flash rules: sectors for erasing, pages for programming.
bool flash_run(uint32_t offset, const uint8_t *data, uint32_t len, bool erase);

uint32_t crc32(const void *data, size_t n);
