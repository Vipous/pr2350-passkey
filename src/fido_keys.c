// Replaces pico-keys-sdk's RP2350 OTP backend: the two device keys it would burn into OTP
// (permanently, and then lock the OTP page) are kept in a flash sector instead. They are random
// per board and made on first start. Weaker than OTP, since a flash dump reveals them, but it
// leaves the chip unchanged.
//
// Secure boot is never enabled from here: enabling it with pico-keys' key would stop this
// firmware from booting.

#include <stddef.h>
#include <string.h>

#include "flash_store.h"
#include "hardware/flash.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/platform_util.h"
#include "otp_platform.h"
#include "picokeys.h"
#include "random.h"

// "PKF2". The first format ("PKFK") was written before the data region got cleared on first
// start; the new magic makes those boards start over once, which also clears the region.
#define MAGIC 0x32464B50u

typedef struct {
    uint32_t magic;
    uint8_t key1[32];   // master key encryption key
    uint8_t key2[32];   // device key, secp256k1
    uint32_t crc;
} fido_keys_t;

_Static_assert(sizeof(fido_keys_t) <= FLASH_PAGE_SIZE, "one flash page");

static uint8_t key1[32], key2[32];

static bool load(void) {
    const fido_keys_t *k = (const fido_keys_t *)flash_ptr(FIDO_KEY_FLASH_OFFSET);
    if (k->magic != MAGIC || crc32(k, offsetof(fido_keys_t, crc)) != k->crc) return false;
    memcpy(key1, k->key1, sizeof(key1));
    memcpy(key2, k->key2, sizeof(key2));
    return true;
}

static void generate_and_save(void) {
    random_fill_buffer(BYTE_ARRAY(key1, sizeof(key1)));

    // Same as the OTP backend: a secp256k1 private key of exactly 32 bytes.
    uint8_t pkey[MBEDTLS_ECP_MAX_BYTES];
    size_t olen = 0;
    while (olen != 32) {
        mbedtls_ecdsa_context ecdsa;
        mbedtls_ecdsa_init(&ecdsa);
        mbedtls_ecdsa_genkey(&ecdsa, MBEDTLS_ECP_DP_SECP256K1, random_fill_iterator, NULL);
        mbedtls_ecp_write_key_ext(&ecdsa, &olen, pkey, sizeof(pkey));
        mbedtls_ecdsa_free(&ecdsa);
    }
    memcpy(key2, pkey, sizeof(key2));
    mbedtls_platform_zeroize(pkey, sizeof(pkey));

    static uint8_t page[FLASH_PAGE_SIZE];
    fido_keys_t *k = (fido_keys_t *)page;
    memset(page, 0xFF, sizeof(page));
    k->magic = MAGIC;
    memcpy(k->key1, key1, sizeof(key1));
    memcpy(k->key2, key2, sizeof(key2));
    k->crc = crc32(k, offsetof(fido_keys_t, crc));
    flash_run(FIDO_KEY_FLASH_OFFSET, NULL, FLASH_SECTOR_SIZE, true);
    flash_run(FIDO_KEY_FLASH_OFFSET, page, sizeof(page), false);
    mbedtls_platform_zeroize(page, sizeof(page));
}

// pico-keys' file system expects its region to be erased the first time. On a board that ran
// other firmware it can hold leftovers (the factory demo's images), which the file system then
// reads as its own files. A new master key means a fresh start anyway, so clear it then.
static void erase_data_region(void) {
    const uint32_t block = 64 * 1024;   // block erase: much faster than sector by sector
    for (uint32_t off = FIDO_DATA_FLASH_START; off < FIDO_DATA_FLASH_END; off += block)
        flash_run(off, NULL, block, true);
}

void otp_platform_init(const uint8_t **otp_key_1_out, const uint8_t **otp_key_2_out) {
    if (!load()) {
        erase_data_region();
        generate_and_save();
    }
    *otp_key_1_out = key1;
    *otp_key_2_out = key2;
}

int otp_platform_enable_secure_boot(uint8_t bootkey, bool secure_lock) {
    (void)bootkey;
    (void)secure_lock;
    return PICOKEYS_EXEC_ERROR;
}

bool otp_platform_is_secure_boot_enabled(uint8_t *bootkey) {
    (void)bootkey;
    return false;
}

bool otp_platform_is_secure_boot_locked(void) {
    return false;
}
