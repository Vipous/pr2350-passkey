#include "pass_vault.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "crypto.h"
#include "flash_store.h"
#include "hardware/flash.h"
#include "pico/rand.h"

// Flash layout, at the end of the chip:
//   [ wrong-secret counter, 1 sector ][ slot 0, 16 KB ][ slot 1, 16 KB ]
// Each save goes to the older slot and its header page is programmed last, so losing power
// mid-write leaves the previous copy intact.
#define SLOT_SIZE      (16 * 1024)
#define REGION_SIZE    (FLASH_SECTOR_SIZE + 2 * SLOT_SIZE)
#define REGION_OFFSET  VAULT_FLASH_OFFSET
#define COUNTER_OFFSET REGION_OFFSET
#define SLOT_OFFSET(i) (REGION_OFFSET + FLASH_SECTOR_SIZE + (i) * SLOT_SIZE)

#define MAGIC          0x33564B50u   // "PKV3"
#define MAGIC_V2       0x32564B50u   // plain-text format of the build without protection
#define KDF_ITERATIONS 20000

#define FLAG_RECOVERY  0x01   // the data ends with the recovery code block
#define RECOVERY_BLOCK 32     // code digits, zero padded

typedef struct {
    uint32_t magic;
    uint32_t seq;          // newer copy has the larger value
    uint32_t iterations;   // PBKDF2 rounds, 0 without protection
    uint16_t count;
    uint16_t entry_size;
    uint8_t lock;          // lock_mode_t
    uint8_t flags;         // FLAG_*
    uint8_t reserved[2];
    uint8_t salt[16];
    uint8_t nonce[16];     // fresh for every save
    uint8_t mac[32];       // HMAC over the header up to here and the data; all-zero key without protection
} header_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint16_t count;
    uint16_t entry_size;
    uint32_t crc;
} header_v2_t;

_Static_assert(REGION_SIZE == VAULT_FLASH_SIZE, "layout in flash_store.h");
_Static_assert(sizeof(vault_entry_t) == 160, "entry layout is stored in flash");
_Static_assert(sizeof(header_t) + VAULT_MAX_ENTRIES * sizeof(vault_entry_t) + RECOVERY_BLOCK <= SLOT_SIZE,
               "slot too small");
_Static_assert(VAULT_RECOVERY_LEN < RECOVERY_BLOCK, "recovery code must fit its block");

static vault_entry_t entries[VAULT_MAX_ENTRIES];
static int count;
static char recovery[RECOVERY_BLOCK];   // empty until first asked for
static bool unlocked;
static lock_mode_t lock_mode;

static uint8_t key_enc[32], key_mac[32];   // all zero without protection
static uint8_t salt[16];
static uint32_t iterations;
static uint32_t seq;
static int current_slot = -1;

static uint8_t image[SLOT_SIZE];   // staging buffer for one slot, wiped after use

// ---------------------------------------------------------------------------
// Wrong-PIN counter

// Wrong-secret counter: one zero byte per attempt in the first page of its sector. NOR flash
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
        h->lock > LOCK_PIN || (h->lock != LOCK_NONE && h->iterations == 0))
        return NULL;
    return h;
}

static const header_v2_t *slot_header_v2(int slot) {
    const header_v2_t *h = (const header_v2_t *)flash_ptr(SLOT_OFFSET(slot));
    if (h->magic != MAGIC_V2 || h->entry_size != sizeof(vault_entry_t) || h->count > VAULT_MAX_ENTRIES) return NULL;
    if (crc32(h + 1, h->count * sizeof(vault_entry_t)) != h->crc) return NULL;
    return h;
}

static uint32_t slot_seq(int slot) {
    if (slot_header(slot)) return slot_header(slot)->seq;
    if (slot_header_v2(slot)) return slot_header_v2(slot)->seq;
    return 0;
}

static bool slot_valid(int slot) {
    return slot_header(slot) || slot_header_v2(slot);
}

static int newest_slot(void) {
    bool a = slot_valid(0), b = slot_valid(1);
    if (a && b) return slot_seq(1) > slot_seq(0) ? 1 : 0;
    return a ? 0 : b ? 1 : -1;
}

static void compute_mac(const header_t *h, const uint8_t *data, size_t len, uint8_t out[32]) {
    hmac_ctx_t c;
    hmac_init(&c, key_mac, sizeof(key_mac));
    hmac_update(&c, h, offsetof(header_t, mac));
    hmac_update(&c, data, len);
    hmac_final(&c, out);
}

static void derive_keys(const char *secret, const uint8_t s[16], uint32_t iter) {
    uint8_t k[64];
    pbkdf2_sha256(secret, strlen(secret), s, 16, iter, k, sizeof(k));
    memcpy(key_enc, k, 32);
    memcpy(key_mac, k + 32, 32);
    secure_zero(k, sizeof(k));
}

static void forget(void) {
    secure_zero(entries, sizeof(entries));
    secure_zero(recovery, sizeof(recovery));
    secure_zero(key_enc, sizeof(key_enc));
    secure_zero(key_mac, sizeof(key_mac));
    count = 0;
    unlocked = false;
}

// Checks and decrypts a slot with the current keys. Fails on a wrong secret or damaged data.
static bool load_slot(int slot) {
    const header_v2_t *v2 = slot_header_v2(slot);
    if (v2) {
        memcpy(entries, v2 + 1, v2->count * sizeof(vault_entry_t));
        count = v2->count;
        seq = v2->seq;
        current_slot = slot;
        return true;
    }
    const header_t *h = slot_header(slot);
    if (!h) return false;
    size_t len = h->count * sizeof(vault_entry_t);
    size_t total = len + (h->flags & FLAG_RECOVERY ? RECOVERY_BLOCK : 0);
    const uint8_t *data = (const uint8_t *)(h + 1);

    uint8_t mac[32];
    compute_mac(h, data, total, mac);
    if (ct_compare(mac, h->mac, sizeof(mac)) != 0) return false;

    // Entries and the recovery code are one encrypted stream, so decrypt them together.
    memcpy(image, data, total);
    if (h->lock != LOCK_NONE) stream_xor(key_enc, h->nonce, image, total);
    memcpy(entries, image, len);
    memset(recovery, 0, sizeof(recovery));
    if (h->flags & FLAG_RECOVERY) {
        memcpy(recovery, image + len, RECOVERY_BLOCK);
        recovery[RECOVERY_BLOCK - 1] = 0;
    }
    secure_zero(image, total);
    count = h->count;
    seq = h->seq;
    current_slot = slot;
    return true;
}

static bool save(void) {
    int slot = current_slot == 0 ? 1 : 0;
    header_t *h = (header_t *)image;
    uint8_t *data = image + sizeof(header_t);
    size_t len = (size_t)count * sizeof(vault_entry_t);

    memset(image, 0xFF, sizeof(image));
    h->magic = MAGIC;
    h->seq = seq + 1;
    h->iterations = iterations;
    h->count = (uint16_t)count;
    h->entry_size = sizeof(vault_entry_t);
    h->lock = (uint8_t)lock_mode;
    h->flags = recovery[0] ? FLAG_RECOVERY : 0;
    memset(h->reserved, 0, sizeof(h->reserved));
    memcpy(h->salt, salt, sizeof(salt));
    rng_128_t r;
    get_rand_128(&r);
    memcpy(h->nonce, &r, sizeof(h->nonce));

    memcpy(data, entries, len);
    if (h->flags & FLAG_RECOVERY) {
        memcpy(data + len, recovery, RECOVERY_BLOCK);
        len += RECOVERY_BLOCK;
    }
    if (lock_mode != LOCK_NONE) stream_xor(key_enc, h->nonce, data, len);
    compute_mac(h, data, len, h->mac);

    uint32_t used = (uint32_t)(sizeof(header_t) + len);
    uint32_t prog = (used + FLASH_PAGE_SIZE - 1) & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    uint32_t base = SLOT_OFFSET(slot);
    bool ok = flash_run(base, NULL, SLOT_SIZE, true);
    if (ok && prog > FLASH_PAGE_SIZE)
        ok = flash_run(base + FLASH_PAGE_SIZE, image + FLASH_PAGE_SIZE, prog - FLASH_PAGE_SIZE, false);
    if (ok) ok = flash_run(base, image, FLASH_PAGE_SIZE, false);   // header last
    secure_zero(image, sizeof(image));

    // Only trust the write once it reads back correctly.
    const header_t *w = ok ? slot_header(slot) : NULL;
    if (!w || w->seq != seq + 1) return false;
    seq++;
    current_slot = slot;
    return true;
}

// ---------------------------------------------------------------------------

void vault_init(void) {
    forget();
    lock_mode = LOCK_NONE;
    iterations = 0;
    seq = 0;
    current_slot = newest_slot();
    if (current_slot < 0) {   // nothing stored yet
        unlocked = true;
        return;
    }
    const header_t *h = slot_header(current_slot);
    if (h && h->lock != LOCK_NONE) {   // stays locked until vault_unlock()
        lock_mode = (lock_mode_t)h->lock;
        seq = h->seq;
        return;
    }
    unlocked = load_slot(current_slot) || load_slot(current_slot ^ 1);
    if (!unlocked) {   // both copies damaged: start over rather than get stuck
        current_slot = -1;
        unlocked = true;
    }
}

lock_mode_t vault_lock_mode(void) {
    return lock_mode;
}

bool vault_is_unlocked(void) {
    return unlocked;
}

int vault_attempts_left(void) {
    if (VAULT_MAX_ATTEMPTS == 0) return -1;
    int left = VAULT_MAX_ATTEMPTS - fail_count();
    return left < 0 ? 0 : left;
}

unlock_result_t vault_unlock(const char *secret) {
    if (unlocked) return UNLOCK_OK;
    int slot = newest_slot();
    const header_t *h = slot >= 0 ? slot_header(slot) : NULL;
    if (!h) return UNLOCK_WRONG;

    // Count the attempt before checking it, so pulling the plug mid-check doesn't give a free try.
    fail_count_add();

    memcpy(salt, h->salt, sizeof(salt));
    iterations = h->iterations;
    derive_keys(secret, salt, iterations);

    bool ok = load_slot(slot);
    if (!ok) {
        // The newest copy may be damaged; the older one is still good if it uses the same key.
        const header_t *o = slot_header(slot ^ 1);
        if (o && o->lock == h->lock && o->iterations == iterations && memcmp(o->salt, salt, sizeof(salt)) == 0)
            ok = load_slot(slot ^ 1);
    }
    if (ok) {
        fail_count_reset();
        unlocked = true;
        return UNLOCK_OK;
    }

    forget();
    if (VAULT_MAX_ATTEMPTS > 0 && fail_count() >= VAULT_MAX_ATTEMPTS) {
        vault_wipe();
        return UNLOCK_WIPED;
    }
    return UNLOCK_WRONG;
}

void vault_lock(void) {
    if (lock_mode != LOCK_NONE) forget();
}

bool vault_set_lock(lock_mode_t mode, const char *secret) {
    if (!unlocked) return false;
    lock_mode_t old_mode = lock_mode;
    uint8_t old_salt[16], old_enc[32], old_mac[32];
    uint32_t old_iter = iterations;
    memcpy(old_salt, salt, sizeof(salt));
    memcpy(old_enc, key_enc, sizeof(key_enc));
    memcpy(old_mac, key_mac, sizeof(key_mac));

    lock_mode = mode;
    if (mode == LOCK_NONE) {
        memset(salt, 0, sizeof(salt));
        iterations = 0;
        secure_zero(key_enc, sizeof(key_enc));
        secure_zero(key_mac, sizeof(key_mac));
    } else {
        rng_128_t r;
        get_rand_128(&r);
        memcpy(salt, &r, sizeof(salt));
        iterations = KDF_ITERATIONS;
        derive_keys(secret, salt, iterations);
    }

    bool ok = save();
    if (ok) {
        // The previous copy opens with the old secret (or none): don't leave it around.
        flash_run(SLOT_OFFSET(current_slot ^ 1), NULL, SLOT_SIZE, true);
        fail_count_reset();
    } else {
        lock_mode = old_mode;
        memcpy(salt, old_salt, sizeof(salt));
        memcpy(key_enc, old_enc, sizeof(key_enc));
        memcpy(key_mac, old_mac, sizeof(key_mac));
        iterations = old_iter;
    }
    secure_zero(old_enc, sizeof(old_enc));
    secure_zero(old_mac, sizeof(old_mac));
    return ok;
}

bool vault_wipe(void) {
    forget();
    lock_mode = LOCK_NONE;
    iterations = 0;
    memset(salt, 0, sizeof(salt));
    seq = 0;
    current_slot = -1;
    unlocked = true;
    return flash_run(REGION_OFFSET, NULL, REGION_SIZE, true);
}

int vault_count(void) {
    return unlocked ? count : 0;
}

const vault_entry_t *vault_get(int index) {
    if (!unlocked || index < 0 || index >= count) return NULL;
    return &entries[index];
}

// Adds an entry at its place by name, in RAM only. The caller checks there is room.
static int insert_sorted(const vault_entry_t *e) {
    vault_entry_t item = *e;
    item.name[VAULT_NAME_LEN - 1] = 0;
    item.login[VAULT_LOGIN_LEN - 1] = 0;
    item.pass[VAULT_PASS_LEN - 1] = 0;
    int pos = 0;
    while (pos < count && strcasecmp(entries[pos].name, item.name) <= 0) pos++;
    memmove(&entries[pos + 1], &entries[pos], (size_t)(count - pos) * sizeof(item));
    entries[pos] = item;
    count++;
    secure_zero(&item, sizeof(item));
    return pos;
}

int vault_put(int index, const vault_entry_t *e) {
    if (!unlocked || index >= count) return -1;
    if (index < 0 && count >= VAULT_MAX_ENTRIES) return -1;

    if (index >= 0) {
        memmove(&entries[index], &entries[index + 1], (size_t)(count - index - 1) * sizeof(vault_entry_t));
        count--;
    }
    int pos = insert_sorted(e);
    secure_zero(&entries[count], (size_t)(VAULT_MAX_ENTRIES - count) * sizeof(vault_entry_t));

    return save() ? pos : -1;
}

bool vault_merge(const vault_entry_t *list, int n, bool replace, int *added, int *skipped) {
    *added = *skipped = 0;
    if (!unlocked) return false;
    if (replace) {
        secure_zero(entries, sizeof(entries));
        count = 0;
    }
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < count && !dup; j++)
            dup = strcasecmp(entries[j].name, list[i].name) == 0 && strcasecmp(entries[j].login, list[i].login) == 0;
        if (dup || count >= VAULT_MAX_ENTRIES) {
            (*skipped)++;
            continue;
        }
        insert_sorted(&list[i]);
        (*added)++;
    }
    return save();
}

const char *vault_recovery_code(void) {
    if (!unlocked) return NULL;
    if (!recovery[0]) {
        // 24 random digits, about 80 bits: enough to keep a stolen backup file closed.
        for (int i = 0; i < VAULT_RECOVERY_LEN; i++) {
            uint32_t r;
            do r = get_rand_32() & 0x0F;
            while (r >= 10);
            recovery[i] = (char)('0' + r);
        }
        recovery[VAULT_RECOVERY_LEN] = 0;
        if (!save()) {
            secure_zero(recovery, sizeof(recovery));
            return NULL;
        }
    }
    return recovery;
}

bool vault_has_recovery_code(void) {
    return unlocked && recovery[0];
}

bool vault_set_recovery_code(const char *code) {
    if (!unlocked || strlen(code) != VAULT_RECOVERY_LEN) return false;
    memset(recovery, 0, sizeof(recovery));
    memcpy(recovery, code, VAULT_RECOVERY_LEN);
    return save();
}

bool vault_delete(int index) {
    if (!unlocked || index < 0 || index >= count) return false;
    memmove(&entries[index], &entries[index + 1], (size_t)(count - index - 1) * sizeof(vault_entry_t));
    count--;
    secure_zero(&entries[count], sizeof(vault_entry_t));
    return save();
}
