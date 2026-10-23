#include "vault.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "hardware/flash.h"
#include "pico/flash.h"

// Flash layout, at the end of the chip: [ slot 0, 16 KB ][ slot 1, 16 KB ]
// Each save goes to the older slot and its header page is programmed last, so losing power
// mid-write leaves the previous copy intact.
#define SLOT_SIZE      (16 * 1024)
#define REGION_SIZE    (2 * SLOT_SIZE)
#define REGION_OFFSET  (PICO_FLASH_SIZE_BYTES - REGION_SIZE)
#define SLOT_OFFSET(i) (REGION_OFFSET + (i) * SLOT_SIZE)

#define MAGIC 0x32564B50u   // "PKV2"

typedef struct {
    uint32_t magic;
    uint32_t seq;          // newer copy has the larger value
    uint16_t count;
    uint16_t entry_size;
    uint32_t crc;          // over the entries
} header_t;

_Static_assert(sizeof(vault_entry_t) == 160, "entry layout is stored in flash");
_Static_assert(sizeof(header_t) + VAULT_MAX_ENTRIES * sizeof(vault_entry_t) <= SLOT_SIZE, "slot too small");

static vault_entry_t entries[VAULT_MAX_ENTRIES];
static int count;
static uint32_t seq;
static int current_slot = -1;

static uint8_t image[SLOT_SIZE];   // staging buffer for one slot

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

static uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Slots

static const header_t *slot_header(int slot) {
    const header_t *h = (const header_t *)flash_ptr(SLOT_OFFSET(slot));
    if (h->magic != MAGIC || h->entry_size != sizeof(vault_entry_t) || h->count > VAULT_MAX_ENTRIES) return NULL;
    if (crc32(h + 1, h->count * sizeof(vault_entry_t)) != h->crc) return NULL;
    return h;
}

static bool save(void) {
    int slot = current_slot == 0 ? 1 : 0;
    header_t *h = (header_t *)image;
    size_t len = (size_t)count * sizeof(vault_entry_t);

    memset(image, 0xFF, sizeof(image));
    h->magic = MAGIC;
    h->seq = seq + 1;
    h->count = (uint16_t)count;
    h->entry_size = sizeof(vault_entry_t);
    memcpy(h + 1, entries, len);
    h->crc = crc32(h + 1, len);

    uint32_t used = (uint32_t)(sizeof(header_t) + len);
    uint32_t prog = (used + FLASH_PAGE_SIZE - 1) & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    uint32_t base = SLOT_OFFSET(slot);
    bool ok = flash_run(base, NULL, SLOT_SIZE, true);
    if (ok && prog > FLASH_PAGE_SIZE)
        ok = flash_run(base + FLASH_PAGE_SIZE, image + FLASH_PAGE_SIZE, prog - FLASH_PAGE_SIZE, false);
    if (ok) ok = flash_run(base, image, FLASH_PAGE_SIZE, false);   // header last

    // Only trust the write once it reads back correctly.
    const header_t *w = ok ? slot_header(slot) : NULL;
    if (!w || w->seq != seq + 1) return false;
    seq++;
    current_slot = slot;
    return true;
}

// ---------------------------------------------------------------------------

void vault_init(void) {
    const header_t *a = slot_header(0), *b = slot_header(1);
    current_slot = a && b ? (b->seq > a->seq ? 1 : 0) : a ? 0 : b ? 1 : -1;
    count = 0;
    seq = 0;
    if (current_slot < 0) return;
    const header_t *h = current_slot ? b : a;
    memcpy(entries, h + 1, h->count * sizeof(vault_entry_t));
    count = h->count;
    seq = h->seq;
}

int vault_count(void) {
    return count;
}

const vault_entry_t *vault_get(int index) {
    if (index < 0 || index >= count) return NULL;
    return &entries[index];
}

int vault_put(int index, const vault_entry_t *e) {
    if (index >= count) return -1;
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
    memset(&entries[count], 0, (size_t)(VAULT_MAX_ENTRIES - count) * sizeof(item));

    return save() ? pos : -1;
}

bool vault_delete(int index) {
    if (index < 0 || index >= count) return false;
    memmove(&entries[index], &entries[index + 1], (size_t)(count - index - 1) * sizeof(vault_entry_t));
    count--;
    memset(&entries[count], 0, sizeof(vault_entry_t));
    return save();
}

bool vault_wipe(void) {
    memset(entries, 0, sizeof(entries));
    count = 0;
    seq = 0;
    current_slot = -1;
    // Also clears the sector in front of the slots, where the PIN version kept its attempt counter.
    return flash_run(REGION_OFFSET - FLASH_SECTOR_SIZE, NULL, REGION_SIZE + FLASH_SECTOR_SIZE, true);
}
