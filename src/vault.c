#include "vault.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "crypto.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/rand.h"

// Flash layout, at the end of the chip:
//   [ wrong-PIN counter, 1 sector ][ slot 0, 16 KB ][ slot 1, 16 KB ]
// Each save goes to the older slot and its header page is programmed last, so losing power
// mid-write leaves the previous copy intact.
#define SLOT_SIZE      (16 * 1024)
#define REGION_SIZE    (FLASH_SECTOR_SIZE + 2 * SLOT_SIZE)
#define REGION_OFFSET  (PICO_FLASH_SIZE_BYTES - REGION_SIZE)
#define COUNTER_OFFSET REGION_OFFSET
#define SLOT_OFFSET(i) (REGION_OFFSET + FLASH_SECTOR_SIZE + (i) * SLOT_SIZE)

#define MAGIC          0x31564B50u   // "PKV1"
#define KDF_ITERATIONS 20000

typedef struct {
    uint32_t magic;
    uint32_t seq;          // newer copy has the larger value
    uint32_t iterations;
    uint16_t count;
    uint16_t entry_size;
    uint8_t salt[16];
    uint8_t nonce[16];     // fresh for every save
    uint8_t mac[32];       // HMAC over the header up to here and the ciphertext
} header_t;

_Static_assert(sizeof(vault_entry_t) == 160, "entry layout is stored in flash");
_Static_assert(sizeof(header_t) + VAULT_MAX_ENTRIES * sizeof(vault_entry_t) <= SLOT_SIZE, "slot too small");

static vault_entry_t entries[VAULT_MAX_ENTRIES];
static int count;
static bool unlocked;

static uint8_t key_enc[32], key_mac[32];
static uint8_t salt[16];
static uint32_t iterations;
static uint32_t seq;
static int current_slot = -1;

static uint8_t image[SLOT_SIZE];   // staging buffer for one slot, wiped after use

// ---------------------------------------------------------------------------
// Flash access

static const uint8_t *flash_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}

typedef struct {
    uint32_t offset;
    const uint8_t *data;   // NULL: erase only
    uint32_t len;
    bool erase;
} flash_op_t;

static void flash_op_cb(void *param) {
    const flash_op_t *op = param;
    if (op->erase) flash_range_erase(op->offset, op->len);
    if (op->data) flash_range_program(op->offset, op->data, op->len);
}

static bool flash_run(uint32_t offset, const uint8_t *data, uint32_t len, bool erase) {
    flash_op_t op = {offset, data, len, erase};
    return flash_safe_execute(flash_op_cb, &op, 1000) == PICO_OK;
}

// Wrong-PIN counter: one zero byte per attempt in the first page of its sector. NOR flash
// can clear more bits in an already programmed page, so counting up needs no erase.
static int fail_count(void) {
    const uint8_t *p = flash_ptr(COUNTER_OFFSET);
    int n = 0;
    while (n < FLASH_PAGE_SIZE && p[n] == 0) n++;
    return n;
}

static void fail_count_add(void) {
    uint8_t page[FLASH_PAGE_SIZE];
    int n = fail_count();
    if (n >= FLASH_PAGE_SIZE) return;
    memset(page, 0xFF, sizeof(page));
    memset(page, 0, (size_t)n + 1);
    flash_run(COUNTER_OFFSET, page, FLASH_PAGE_SIZE, false);
}

static void fail_count_reset(void) {
    if (fail_count() > 0) flash_run(COUNTER_OFFSET, NULL, FLASH_SECTOR_SIZE, true);
}

// ---------------------------------------------------------------------------
// Slots

static const header_t *slot_header(int slot) {
    const header_t *h = (const header_t *)flash_ptr(SLOT_OFFSET(slot));
    if (h->magic != MAGIC || h->entry_size != sizeof(vault_entry_t) || h->count > VAULT_MAX_ENTRIES ||
        h->iterations == 0)
        return NULL;
    return h;
}

static int newest_slot(void) {
    const header_t *a = slot_header(0), *b = slot_header(1);
    if (a && b) return b->seq > a->seq ? 1 : 0;
    return a ? 0 : b ? 1 : -1;
}

static void compute_mac(const header_t *h, const uint8_t *cipher, size_t len, uint8_t out[32]) {
    hmac_ctx_t c;
    hmac_init(&c, key_mac, sizeof(key_mac));
    hmac_update(&c, h, offsetof(header_t, mac));
    hmac_update(&c, cipher, len);
    hmac_final(&c, out);
}

static void derive_keys(const char *pin, const uint8_t s[16], uint32_t iter) {
    uint8_t k[64];
    pbkdf2_sha256(pin, strlen(pin), s, 16, iter, k, sizeof(k));
    memcpy(key_enc, k, 32);
    memcpy(key_mac, k + 32, 32);
    secure_zero(k, sizeof(k));
}

// Decrypts a slot with the current keys. Fails if the PIN was wrong or the data is damaged.
static bool load_slot(int slot) {
    const header_t *h = slot_header(slot);
    if (!h) return false;
    size_t len = h->count * sizeof(vault_entry_t);
    const uint8_t *cipher = (const uint8_t *)(h + 1);

    uint8_t mac[32];
    compute_mac(h, cipher, len, mac);
    if (ct_compare(mac, h->mac, sizeof(mac)) != 0) return false;

    memcpy(entries, cipher, len);
    stream_xor(key_enc, h->nonce, (uint8_t *)entries, len);
    count = h->count;
    seq = h->seq;
    current_slot = slot;
    return true;
}

static bool save(void) {
    int slot = current_slot == 0 ? 1 : 0;
    header_t *h = (header_t *)image;
    uint8_t *cipher = image + sizeof(header_t);
    size_t len = (size_t)count * sizeof(vault_entry_t);

    memset(image, 0xFF, sizeof(image));
    h->magic = MAGIC;
    h->seq = seq + 1;
    h->iterations = iterations;
    h->count = (uint16_t)count;
    h->entry_size = sizeof(vault_entry_t);
    memcpy(h->salt, salt, sizeof(salt));
    rng_128_t r;
    get_rand_128(&r);
    memcpy(h->nonce, &r, sizeof(h->nonce));

    memcpy(cipher, entries, len);
    stream_xor(key_enc, h->nonce, cipher, len);
    compute_mac(h, cipher, len, h->mac);

    uint32_t used = (uint32_t)(sizeof(header_t) + len);
    uint32_t prog = (used + FLASH_PAGE_SIZE - 1) & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    uint32_t base = SLOT_OFFSET(slot);
    bool ok = flash_run(base, NULL, SLOT_SIZE, true);
    if (ok && prog > FLASH_PAGE_SIZE)
        ok = flash_run(base + FLASH_PAGE_SIZE, image + FLASH_PAGE_SIZE, prog - FLASH_PAGE_SIZE, false);
    if (ok) ok = flash_run(base, image, FLASH_PAGE_SIZE, false);   // header last
    secure_zero(image, sizeof(image));

    // Only trust the write once it reads back correctly.
    ok = ok && slot_header(slot) && slot_header(slot)->seq == seq + 1;
    if (ok) {
        seq++;
        current_slot = slot;
    }
    return ok;
}

static void forget_keys(void) {
    secure_zero(entries, sizeof(entries));
    secure_zero(key_enc, sizeof(key_enc));
    secure_zero(key_mac, sizeof(key_mac));
    count = 0;
    unlocked = false;
}

// ---------------------------------------------------------------------------

bool vault_exists(void) {
    return newest_slot() >= 0;
}

bool vault_is_unlocked(void) {
    return unlocked;
}

int vault_attempts_left(void) {
    if (VAULT_MAX_ATTEMPTS == 0) return -1;
    int left = VAULT_MAX_ATTEMPTS - fail_count();
    return left < 0 ? 0 : left;
}

bool vault_create(const char *pin) {
    forget_keys();
    rng_128_t r;
    get_rand_128(&r);
    memcpy(salt, &r, sizeof(salt));
    iterations = KDF_ITERATIONS;
    derive_keys(pin, salt, iterations);
    seq = 0;
    current_slot = newest_slot();
    if (current_slot >= 0) seq = slot_header(current_slot)->seq;
    if (!save()) {
        forget_keys();
        return false;
    }
    fail_count_reset();
    unlocked = true;
    return true;
}

unlock_result_t vault_unlock(const char *pin) {
    forget_keys();
    int slot = newest_slot();
    if (slot < 0) return UNLOCK_WRONG_PIN;

    // Count the attempt before checking it, so pulling the plug mid-check doesn't give a free try.
    fail_count_add();

    const header_t *h = slot_header(slot);
    memcpy(salt, h->salt, sizeof(salt));
    iterations = h->iterations;
    derive_keys(pin, salt, iterations);

    bool ok = load_slot(slot);
    if (!ok) {
        // The newest copy may be damaged; the older one is still good if it uses the same key.
        int other = slot ^ 1;
        const header_t *o = slot_header(other);
        if (o && o->iterations == iterations && memcmp(o->salt, salt, sizeof(salt)) == 0) ok = load_slot(other);
    }
    if (ok) {
        fail_count_reset();
        unlocked = true;
        return UNLOCK_OK;
    }

    forget_keys();
    if (VAULT_MAX_ATTEMPTS > 0 && fail_count() >= VAULT_MAX_ATTEMPTS) {
        vault_wipe();
        return UNLOCK_WIPED;
    }
    return UNLOCK_WRONG_PIN;
}

void vault_lock(void) {
    forget_keys();
}

bool vault_change_pin(const char *new_pin) {
    if (!unlocked) return false;
    uint8_t old_salt[16], old_enc[32], old_mac[32];
    uint32_t old_iter = iterations;
    memcpy(old_salt, salt, sizeof(salt));
    memcpy(old_enc, key_enc, sizeof(key_enc));
    memcpy(old_mac, key_mac, sizeof(key_mac));

    rng_128_t r;
    get_rand_128(&r);
    memcpy(salt, &r, sizeof(salt));
    iterations = KDF_ITERATIONS;
    derive_keys(new_pin, salt, iterations);
    bool ok = save();
    if (!ok) {
        memcpy(salt, old_salt, sizeof(salt));
        memcpy(key_enc, old_enc, sizeof(key_enc));
        memcpy(key_mac, old_mac, sizeof(key_mac));
        iterations = old_iter;
    } else {
        // Don't leave the previous copy around, it opens with the old PIN.
        flash_run(SLOT_OFFSET(current_slot ^ 1), NULL, SLOT_SIZE, true);
    }
    secure_zero(old_enc, sizeof(old_enc));
    secure_zero(old_mac, sizeof(old_mac));
    return ok;
}

void vault_wipe(void) {
    forget_keys();
    flash_run(REGION_OFFSET, NULL, REGION_SIZE, true);
    current_slot = -1;
    seq = 0;
}

int vault_count(void) {
    return unlocked ? count : 0;
}

const vault_entry_t *vault_get(int index) {
    if (!unlocked || index < 0 || index >= count) return NULL;
    return &entries[index];
}

int vault_put(int index, const vault_entry_t *e) {
    if (!unlocked || index >= count) return -1;
    if (index < 0 && count >= VAULT_MAX_ENTRIES) return -1;

    vault_entry_t item = *e;
    item.name[VAULT_NAME_LEN - 1] = 0;
    item.login[VAULT_LOGIN_LEN - 1] = 0;
    item.pass[VAULT_PASS_LEN - 1] = 0;

    if (index >= 0) {
        memmove(&entries[index], &entries[index + 1], (size_t)(count - index - 1) * sizeof(item));
        count--;
    }
    int pos = 0;
    while (pos < count && strcasecmp(entries[pos].name, item.name) <= 0) pos++;
    memmove(&entries[pos + 1], &entries[pos], (size_t)(count - pos) * sizeof(item));
    entries[pos] = item;
    count++;
    secure_zero(&item, sizeof(item));
    secure_zero(&entries[count], (size_t)(VAULT_MAX_ENTRIES - count) * sizeof(item));

    return save() ? pos : -1;
}

bool vault_delete(int index) {
    if (!unlocked || index < 0 || index >= count) return false;
    memmove(&entries[index], &entries[index + 1], (size_t)(count - index - 1) * sizeof(vault_entry_t));
    count--;
    secure_zero(&entries[count], sizeof(vault_entry_t));
    return save();
}
