#include "backup.h"

#include <string.h>

#include "crypto.h"
#include "pico/rand.h"

#define MAGIC "PKB1"
// The code already has ~80 bits of entropy; the stretching only makes guessing it slower still.
#define KDF_ITERATIONS 10000

static vault_entry_t list[VAULT_MAX_ENTRIES];   // decrypted backup, wiped after use

static void derive(const char *code, const uint8_t salt[16], uint8_t enc[32], uint8_t mac[32]) {
    uint8_t k[64];
    pbkdf2_sha256(code, strlen(code), salt, 16, KDF_ITERATIONS, k, sizeof(k));
    memcpy(enc, k, 32);
    memcpy(mac, k + 32, 32);
    secure_zero(k, sizeof(k));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

size_t backup_export(uint8_t *out, size_t size) {
    const char *code = vault_recovery_code();
    if (!code) return 0;
    int n = vault_count();
    size_t body = (size_t)n * sizeof(vault_entry_t);
    size_t total = BACKUP_HEADER_SIZE + body + BACKUP_MAC_SIZE;
    if (total > size) return 0;

    memcpy(out, MAGIC, 4);
    out[4] = (uint8_t)n;
    out[5] = (uint8_t)(n >> 8);
    out[6] = (uint8_t)sizeof(vault_entry_t);
    out[7] = (uint8_t)(sizeof(vault_entry_t) >> 8);
    rng_128_t r;
    get_rand_128(&r);
    memcpy(out + 8, &r, 16);    // salt
    get_rand_128(&r);
    memcpy(out + 24, &r, 16);   // nonce

    uint8_t enc[32], mac[32];
    derive(code, out + 8, enc, mac);
    for (int i = 0; i < n; i++) memcpy(out + BACKUP_HEADER_SIZE + i * sizeof(vault_entry_t), vault_get(i), sizeof(vault_entry_t));
    stream_xor(enc, out + 24, out + BACKUP_HEADER_SIZE, body);
    hmac_sha256(mac, 32, out, BACKUP_HEADER_SIZE + body, out + BACKUP_HEADER_SIZE + body);
    secure_zero(enc, sizeof(enc));
    secure_zero(mac, sizeof(mac));
    return total;
}

int backup_peek(const uint8_t *data, size_t len) {
    if (len < BACKUP_HEADER_SIZE + BACKUP_MAC_SIZE || memcmp(data, MAGIC, 4) != 0) return -1;
    int n = get16(data + 4);
    if (get16(data + 6) != sizeof(vault_entry_t) || n > VAULT_MAX_ENTRIES) return -1;
    if (len != BACKUP_HEADER_SIZE + n * sizeof(vault_entry_t) + BACKUP_MAC_SIZE) return -1;
    return n;
}

restore_result_t backup_import(const uint8_t *data, size_t len, const char *code, bool replace,
                               int *added, int *skipped) {
    *added = *skipped = 0;
    int n = backup_peek(data, len);
    if (n < 0) return RESTORE_BAD_FILE;
    size_t body = (size_t)n * sizeof(vault_entry_t);

    uint8_t enc[32], mac[32], check[32];
    derive(code, data + 8, enc, mac);
    hmac_sha256(mac, 32, data, BACKUP_HEADER_SIZE + body, check);
    bool ok = ct_compare(check, data + BACKUP_HEADER_SIZE + body, 32) == 0;
    restore_result_t res = RESTORE_WRONG_CODE;
    if (ok) {
        memcpy(list, data + BACKUP_HEADER_SIZE, body);
        stream_xor(enc, data + 24, (uint8_t *)list, body);
        res = vault_merge(list, n, replace, added, skipped) ? RESTORE_OK : RESTORE_SAVE_FAILED;
        secure_zero(list, sizeof(list));
    }
    secure_zero(enc, sizeof(enc));
    secure_zero(mac, sizeof(mac));
    return res;
}
