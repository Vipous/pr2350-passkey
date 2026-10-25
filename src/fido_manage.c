#include "fido_manage.h"

#include <stdio.h>
#include <string.h>

#include "credential.h"
#include "fido.h"
#include "fido_glue.h"
#include "files.h"
#include "flash.h"
#include "flash_store.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "mbedtls/sha256.h"
#include "pico/time.h"
#include "pico_time.h"
#include "resident_container.h"
#include "usb.h"

// In patches/pico-fido.patch (cbor_client_pin.c).
extern int fido_pin_check(const uint8_t *pin, size_t pin_len, uint8_t *retries_left);

// ---------------------------------------------------------------------------
// When each passkey was made and used, in its own flash area (FIDO_USE_FLASH_OFFSET), keyed by
// the first bytes of SHA-256 of its resident ID. pico-fido keeps no such dates itself.

#define MAGIC 0x31554B50u   // "PKU1"
#define MAX_USES MAX_RESIDENT_CREDENTIALS

typedef struct {
    uint8_t key[8];
    uint32_t created, last_used, uses;
} use_t;

typedef struct {
    uint32_t magic;
    uint16_t count, reserved;
    uint32_t crc;          // over entries[0..count)
    use_t entries[MAX_USES];
} table_t;

#define TABLE_FLASH_SIZE ((sizeof(table_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
_Static_assert(TABLE_FLASH_SIZE <= FIDO_USE_FLASH_SIZE, "fits its flash area");

static union {
    table_t t;
    uint8_t raw[TABLE_FLASH_SIZE];
} table;

// Uses seen before the clock was set: uptime in ms, dated when the page sets the clock.
static uint32_t created_ms[MAX_USES], used_ms[MAX_USES];
static bool dirty;
static uint32_t dirty_since;

// Core 1 -> core 0.
typedef struct {
    bool created;
    uint8_t key[8];
    uint32_t ms;
} event_t;
static event_t events[8];
static volatile uint32_t ev_head, ev_tail;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void key_of(const uint8_t *resident_id, uint8_t key[8]) {
    uint8_t h[32];
    mbedtls_sha256(resident_id, CRED_RESIDENT_LEN, h, 0);
    memcpy(key, h, 8);
}

void fido_credential_event(bool created, const uint8_t *resident_id) {
    uint32_t head = ev_head;
    if (head - ev_tail >= count_of(events)) return;   // core 0 is behind; drop it
    event_t *e = &events[head % count_of(events)];
    e->created = created;
    key_of(resident_id, e->key);
    e->ms = now_ms();
    __dmb();
    ev_head = head + 1;
}

void fido_manage_init(void) {
    const table_t *f = (const table_t *)flash_ptr(FIDO_USE_FLASH_OFFSET);
    if (f->magic == MAGIC && f->count <= MAX_USES && crc32(f->entries, f->count * sizeof(use_t)) == f->crc) {
        memcpy(&table.t, f, sizeof(table_t));
    } else {
        memset(&table, 0, sizeof(table));
        table.t.magic = MAGIC;
    }
}

static int find(const uint8_t key[8]) {
    for (int i = 0; i < table.t.count; i++)
        if (memcmp(table.t.entries[i].key, key, 8) == 0) return i;
    return -1;
}

static void remove_at(int i) {
    int last = table.t.count - 1;
    table.t.entries[i] = table.t.entries[last];
    created_ms[i] = created_ms[last];
    used_ms[i] = used_ms[last];
    table.t.count--;
}

static int find_or_add(const uint8_t key[8]) {
    int i = find(key);
    if (i >= 0) return i;
    if (table.t.count == MAX_USES) {
        // Full (passkeys deleted elsewhere leave entries behind): forget the least recently used.
        int oldest = 0;
        for (int k = 1; k < table.t.count; k++)
            if (table.t.entries[k].last_used < table.t.entries[oldest].last_used) oldest = k;
        remove_at(oldest);
    }
    i = table.t.count++;
    memset(&table.t.entries[i], 0, sizeof(use_t));
    memcpy(table.t.entries[i].key, key, 8);
    created_ms[i] = used_ms[i] = 0;
    return i;
}

static void mark_dirty(void) {
    dirty = true;
    dirty_since = now_ms();
}

bool fido_manage_has_time(void) {
    return has_set_rtc();
}

void fido_manage_set_time(uint32_t unix_time) {
    set_rtc_time((time_t)unix_time);
    uint32_t ms = now_ms();
    for (int i = 0; i < table.t.count; i++) {
        use_t *u = &table.t.entries[i];
        if (created_ms[i]) u->created = unix_time - (ms - created_ms[i]) / 1000;
        if (used_ms[i]) u->last_used = unix_time - (ms - used_ms[i]) / 1000;
        if (created_ms[i] || used_ms[i]) mark_dirty();
        created_ms[i] = used_ms[i] = 0;
    }
}

bool fido_manage_busy(void) {
    return is_busy() || fido_presence_pending() != FIDO_ASK_NONE;
}

void fido_manage_task(void) {
    while (ev_tail != ev_head) {
        __dmb();
        const event_t *e = &events[ev_tail % count_of(events)];
        int i = find_or_add(e->key);
        use_t *u = &table.t.entries[i];
        uint32_t t = has_set_rtc() ? (uint32_t)get_rtc_time() : 0;
        if (e->created) {
            u->created = t;
            created_ms[i] = t ? 0 : e->ms;
        } else {
            u->last_used = t;
            used_ms[i] = t ? 0 : e->ms;
            u->uses++;
        }
        ev_tail++;
        mark_dirty();
    }
    // Saved a moment later, between browser requests: the erase pauses the other core.
    if (dirty && now_ms() - dirty_since > 1000 && !fido_manage_busy()) {
        dirty = false;
        table.t.crc = crc32(table.t.entries, table.t.count * sizeof(use_t));
        flash_run(FIDO_USE_FLASH_OFFSET, NULL, FIDO_USE_FLASH_SIZE, true);
        flash_run(FIDO_USE_FLASH_OFFSET, table.raw, TABLE_FLASH_SIZE, false);
    }
}

// ---------------------------------------------------------------------------
// Listing and deleting, with pico-fido's own storage functions (the same ones its credential
// management commands use).

// pico-fido sets up its files when a browser first talks to the key; the page may come first.
static void ensure_init(void) {
    if (!ef_pin) init_fido();
}

void fido_manage_info(fido_info_t *info) {
    ensure_init();
    memset(info, 0, sizeof(*info));
    info->pin_set = file_has_data(ef_pin);
    if (info->pin_set) info->retries = *file_get_data(ef_pin) & PIN_RETRY_COUNT_MASK;
    for (int i = 0; i < MAX_RESIDENT_CREDENTIALS; i++)
        if (file_has_data(file_search((uint16_t)(EF_CRED + i)))) info->count++;
    info->max = MAX_RESIDENT_CREDENTIALS;
}

int fido_manage_unlock(const char *pin, uint8_t *retries) {
    ensure_init();
    return fido_pin_check((const uint8_t *)pin, strlen(pin), retries);
}

// Text for a tab-separated line: control characters (tabs, newlines) become spaces.
static void copy_text(char *dst, size_t size, const CborCharString *s) {
    size_t n = s->present ? s->len : 0;
    if (n >= size) n = size - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (unsigned char)s->data[i] < 0x20 ? ' ' : s->data[i];
    dst[n] = 0;
}

// Loads the passkey in a slot; its short key goes to key[8]. The caller frees cred.
static bool load(file_t *ef, Credential *cred, uint8_t key[8]) {
    uint8_t rp_hash[RP_ID_HASH_LEN];
    memset(cred, 0, sizeof(*cred));
    if (credential_resident_rp_id_hash(ef, rp_hash) != PICOKEYS_OK) return false;
    if (credential_load_resident(ef, rp_hash, cred) != 0) {
        credential_free(cred);
        return false;
    }
    uint8_t rid[CRED_RESIDENT_LEN];
    if (cred->residentId.present && cred->residentId.len == CRED_RESIDENT_LEN) memcpy(rid, cred->residentId.data, sizeof(rid));
    else credential_derive_resident(cred->id.data, cred->id.len, rid);
    key_of(rid, key);
    return true;
}

bool fido_manage_next(int *slot, fido_passkey_t *out) {
    for (int i = *slot; i < MAX_RESIDENT_CREDENTIALS; i++) {
        file_t *ef = file_search((uint16_t)(EF_CRED + i));
        if (!file_has_data(ef)) continue;
        *slot = i + 1;
        memset(out, 0, sizeof(*out));
        out->slot = i;
        Credential cred;
        uint8_t key[8];
        if (!load(ef, &cred, key)) {
            strcpy(out->rp, "?");   // unreadable; still listed so it can be deleted
            return true;
        }
        for (int k = 0; k < 8; k++) snprintf(out->id + 2 * k, 3, "%02x", key[k]);
        copy_text(out->rp, sizeof(out->rp), &cred.rpId);
        copy_text(out->user, sizeof(out->user), &cred.userName);
        copy_text(out->display, sizeof(out->display), &cred.userDisplayName);
        out->alg = (int32_t)cred.alg;
        out->cred_protect = (uint8_t)cred.extensions.credProtect;
        out->created = (uint32_t)cred.rtc_creation;   // pico-fido's, if the clock was set then
        int u = find(key);
        if (u >= 0) {
            if (table.t.entries[u].created) out->created = table.t.entries[u].created;
            out->last_used = table.t.entries[u].last_used;
            out->uses = table.t.entries[u].uses;
        }
        credential_free(&cred);
        return true;
    }
    return false;
}

int fido_manage_delete(int slot, const char *id) {
    if (slot < 0 || slot >= MAX_RESIDENT_CREDENTIALS) return -1;
    file_t *ef = file_search((uint16_t)(EF_CRED + slot));
    if (!file_has_data(ef)) return -1;
    Credential cred;
    uint8_t key[8];
    char hex[17] = "";
    bool loaded = load(ef, &cred, key);
    if (loaded) {
        for (int k = 0; k < 8; k++) snprintf(hex + 2 * k, 3, "%02x", key[k]);
        credential_free(&cred);
    }
    if (strcmp(hex, id) != 0) return -1;   // unreadable ones have the empty ID

    uint8_t rp_hash[RP_ID_HASH_LEN];
    if (credential_resident_rp_id_hash(ef, rp_hash) != PICOKEYS_OK) return -2;
    bool legacy_rp = !resident_container_is_marker(ef);
    if (credential_resident_delete(ef) != PICOKEYS_OK) return -2;
    if (legacy_rp) {
        int r = credential_rp_legacy_decrement(rp_hash);
        if (r != PICOKEYS_OK && r != PICOKEYS_ERR_FILE_NOT_FOUND) return -2;
    }
    dev_state_update(DEV_STATE_CRED_STATE);
    flash_commit();
    if (loaded) {
        int u = find(key);
        if (u >= 0) {
            remove_at(u);
            mark_dirty();
        }
    }
    return 0;
}
