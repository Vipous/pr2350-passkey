#include "editor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "vault.h"

#define PROTOCOL_VERSION 3
#define LINE_MAX 320
#define MAX_FIELDS 6

// ---------------------------------------------------------------------------
// Mailbox between the cores: core 0 posts, core 1 runs it against the vault and marks it done.

typedef enum { OP_GET, OP_PUT, OP_DEL, OP_WIPE, OP_BACKUP, OP_RESTORE } op_t;
enum { REQ_IDLE, REQ_POSTED, REQ_DONE };

static struct {
    volatile int state;
    op_t op;
    int index;
    bool keep_pass;
    vault_entry_t entry;
    bool replace;  // RESTORE: drop the current list first
    int result;   // new index for PUT, file size for BACKUP, entries for RESTORE, 1 otherwise;
                  // -1 flash/full, -2 no such entry, -3 locked, -4 not a backup
    int count;    // entries after the operation
} req;

static void post(op_t op, int index) {
    req.op = op;
    req.index = index;
    __dmb();
    req.state = REQ_POSTED;
}

static volatile bool locked;
static volatile uint8_t lock_mode;

// Backup file on its way to or from the computer. Core 0 fills it during RESTORE and reads it
// during BACKUP; core 1 only touches it inside a request or while a restore waits for the code.
static uint8_t xfer[BACKUP_MAX_SIZE];
static size_t xfer_len, xfer_expected;

typedef enum { RS_IDLE, RS_RECEIVING, RS_WAITING, RS_DONE, RS_CANCELLED, RS_FAILED } restore_state_t;
static volatile restore_state_t restore_state;
static bool restore_replace;
static int restore_entries, restore_added, restore_skipped;

bool editor_restore_waiting(void) {
    return restore_state == RS_WAITING;
}

const uint8_t *editor_restore_data(size_t *len, bool *replace, int *entries) {
    *len = xfer_len;
    *replace = restore_replace;
    *entries = restore_entries;
    return xfer;
}

void editor_restore_finish(bool done, int added, int skipped) {
    restore_added = added;
    restore_skipped = skipped;
    memset(xfer, 0, sizeof(xfer));
    xfer_len = 0;
    __dmb();
    restore_state = done ? RS_DONE : RS_CANCELLED;
}

void editor_set_state(bool is_locked, int mode) {
    locked = is_locked;
    lock_mode = (uint8_t)mode;
}

editor_activity_t editor_service(void) {
    if (req.state != REQ_POSTED) return EDITOR_IDLE;
    __dmb();
    bool changed = false;
    const vault_entry_t *e;
    // Wiping works while locked: it's the way out of a forgotten PIN.
    if (!vault_is_unlocked() && req.op != OP_WIPE) {
        req.result = -3;
        req.count = 0;
        __dmb();
        req.state = REQ_DONE;
        return EDITOR_READ;
    }
    switch (req.op) {
    case OP_GET:
        e = vault_get(req.index);
        req.result = e ? 1 : -1;
        if (e) req.entry = *e;
        break;
    case OP_PUT:
        if (req.index >= vault_count()) {
            req.result = -2;
            break;
        }
        if (req.keep_pass) {
            e = vault_get(req.index);
            if (e) memcpy(req.entry.pass, e->pass, sizeof(req.entry.pass));
        }
        req.result = vault_put(req.index, &req.entry);
        changed = true;
        break;
    case OP_DEL:
        if (req.index >= vault_count()) {
            req.result = -2;
            break;
        }
        req.result = vault_delete(req.index) ? 1 : -1;
        changed = true;
        break;
    case OP_WIPE:
        req.result = vault_wipe() ? 1 : -1;
        changed = true;
        break;
    case OP_BACKUP: {
        size_t n = backup_export(xfer, sizeof(xfer));
        xfer_len = n;
        req.result = n ? (int)n : -1;
        break;
    }
    case OP_RESTORE:
        req.result = backup_peek(xfer, xfer_len);
        if (req.result < 0) {
            req.result = -4;
            break;
        }
        restore_entries = req.result;
        restore_replace = req.replace;
        __dmb();
        restore_state = RS_WAITING;   // the UI now asks for the recovery code
        break;
    }
    req.count = vault_count();
    __dmb();
    req.state = REQ_DONE;
    return changed ? EDITOR_CHANGED : EDITOR_READ;
}

// ---------------------------------------------------------------------------
// Core 0: serial I/O

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;

static char out[1024];
static size_t out_len, out_pos;

static volatile bool connected;

typedef enum { ST_READ, ST_WAIT, ST_LIST, ST_BACKUP } state_t;
static state_t state = ST_READ;
static op_t waiting_op;
static int list_index;
static size_t backup_pos;

bool editor_connected(void) {
    return connected;
}

static void reply(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    va_end(ap);
    if (n > 0) out_len += (size_t)n < sizeof(out) - out_len ? (size_t)n : sizeof(out) - out_len - 1;
}

static void flush_out(void) {
    if (out_pos < out_len) {
        out_pos += tud_cdc_write(out + out_pos, (uint32_t)(out_len - out_pos));
        tud_cdc_write_flush();
    }
    if (out_pos == out_len) out_pos = out_len = 0;
}

#define B64_CHUNK 96   // bytes per line of backup data, 128 characters of base64

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void reply_base64(const uint8_t *p, size_t n) {
    char buf[B64_CHUNK / 3 * 4 + 8], *o = buf;
    *o++ = 'B';
    *o++ = '\t';
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *o++ = b64[v >> 18 & 63];
        *o++ = b64[v >> 12 & 63];
        *o++ = i + 1 < n ? b64[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? b64[v & 63] : '=';
    }
    *o = 0;
    reply("%s\n", buf);
}

// Appends decoded base64 to xfer; false on bad input or overflow.
static bool append_base64(const char *s) {
    uint32_t v = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        const char *c = strchr(b64, *s);
        if (!c || !*s) return false;
        v = v << 6 | (uint32_t)(c - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (xfer_len >= xfer_expected) return false;
            xfer[xfer_len++] = (uint8_t)(v >> bits);
        }
    }
    return true;
}

static bool printable(const char *s) {
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7E) return false;
    return true;
}

static bool parse_index(const char *s, int *out_index) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 0 || v >= VAULT_MAX_ENTRIES) return false;
    *out_index = (int)v;
    return true;
}

// Fills req.entry from name, login, flags and an optional password.
static const char *fill_entry(char **f, int n) {
    if (n < 3) return "missing fields";
    if (!f[0][0]) return "name is empty";
    if (strlen(f[0]) >= VAULT_NAME_LEN) return "name too long";
    if (strlen(f[1]) >= VAULT_LOGIN_LEN) return "login too long";
    if (n > 3 && strlen(f[3]) >= VAULT_PASS_LEN) return "password too long";
    for (int i = 0; i < n; i++)
        if (!printable(f[i])) return "only printable ASCII is supported";

    memset(&req.entry, 0, sizeof(req.entry));
    strcpy(req.entry.name, f[0]);
    strcpy(req.entry.login, f[1]);
    req.entry.flags = (uint8_t)(atoi(f[2]) & (VAULT_FLAG_ENTER | VAULT_FLAG_LOGIN));
    if (n > 3) strcpy(req.entry.pass, f[3]);
    req.keep_pass = n <= 3;
    return NULL;
}

static void handle_line(char *s) {
    char *f[MAX_FIELDS + 1];
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n <= MAX_FIELDS; p++) {
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    const char *cmd = f[0];
    const char *err = NULL;

    if (strcmp(cmd, "HELLO") == 0) {
        static const char *const modes[] = {"none", "pin"};
        reply("OK PassKey %d %d %s %s\n", PROTOCOL_VERSION, VAULT_MAX_ENTRIES, locked ? "locked" : "unlocked",
              modes[lock_mode <= LOCK_PIN ? lock_mode : 0]);
        return;
    } else if (strcmp(cmd, "LIST") == 0) {
        list_index = 0;
        state = ST_LIST;
        post(OP_GET, 0);
        return;
    } else if (strcmp(cmd, "ADD") == 0) {
        err = n < 5 ? "missing fields" : fill_entry(f + 1, 4);
        if (!err) {
            waiting_op = OP_PUT;
            post(OP_PUT, -1);
        }
    } else if (strcmp(cmd, "SET") == 0) {
        int idx;
        if (n < 5 || !parse_index(f[1], &idx)) err = "bad index";
        else err = fill_entry(f + 2, n - 2);
        if (!err) {
            waiting_op = OP_PUT;
            post(OP_PUT, idx);
        }
    } else if (strcmp(cmd, "DEL") == 0) {
        int idx;
        if (n < 2 || !parse_index(f[1], &idx)) err = "bad index";
        else {
            waiting_op = OP_DEL;
            post(OP_DEL, idx);
        }
    } else if (strcmp(cmd, "WIPE") == 0) {
        waiting_op = OP_WIPE;
        post(OP_WIPE, 0);
    } else if (strcmp(cmd, "BACKUP") == 0) {
        if (restore_state == RS_WAITING || restore_state == RS_RECEIVING) err = "busy with a restore";
        else {
            waiting_op = OP_BACKUP;
            post(OP_BACKUP, 0);
        }
    } else if (strcmp(cmd, "RESTORE") == 0) {
        long size = n > 1 ? strtol(f[1], NULL, 10) : 0;
        if (restore_state == RS_WAITING) err = "busy with a restore";
        else if (size <= 0 || size > (long)sizeof(xfer)) err = "bad size";
        else {
            xfer_len = 0;
            xfer_expected = (size_t)size;
            restore_state = RS_RECEIVING;
            reply("OK\n");
        }
        if (!err) return;
    } else if (strcmp(cmd, "R") == 0) {
        if (restore_state != RS_RECEIVING) err = "no restore in progress";
        else if (n < 2 || !append_base64(f[1])) err = "bad data";
        else reply("OK\n");
        if (!err) return;
    } else if (strcmp(cmd, "RESTORE_END") == 0) {
        if (restore_state != RS_RECEIVING || xfer_len != xfer_expected) err = "incomplete file";
        else {
            req.replace = n > 1 && strcmp(f[1], "replace") == 0;
            waiting_op = OP_RESTORE;
            post(OP_RESTORE, 0);
        }
    } else if (strcmp(cmd, "RESTORE_STATUS") == 0) {
        switch (restore_state) {
        case RS_WAITING: reply("OK waiting\n"); break;
        case RS_DONE: reply("OK done %d %d\n", restore_added, restore_skipped); break;
        case RS_CANCELLED: reply("OK cancelled\n"); break;
        case RS_FAILED: reply("OK failed\n"); break;
        default: reply("OK idle\n"); break;
        }
        return;
    } else {
        err = "unknown command";
    }

    if (err) reply("ERR %s\n", err);
    else state = ST_WAIT;
}

void editor_task(void) {
    connected = tud_cdc_connected();
    flush_out();

    if (state == ST_WAIT || state == ST_LIST) {
        if (req.state != REQ_DONE) return;
        __dmb();
        if (state == ST_WAIT && waiting_op == OP_BACKUP && req.result > 0) {
            backup_pos = 0;
            state = ST_BACKUP;
            req.state = REQ_IDLE;
            return;
        }
        if (state == ST_WAIT) {
            if (waiting_op == OP_RESTORE && req.result < 0) restore_state = RS_FAILED;
            if (req.result == -4) reply("ERR not a Pass Key backup\n");
            else if (req.result == -3) reply("ERR locked\n");
            else if (req.result == -2) reply("ERR no such entry\n");
            else if (req.result < 0) reply("ERR %s\n", waiting_op == OP_PUT && req.count >= VAULT_MAX_ENTRIES ? "list is full" : "flash write failed");
            else if (waiting_op == OP_PUT || waiting_op == OP_RESTORE) reply("OK %d\n", req.result);
            else reply("OK\n");
            memset(&req.entry, 0, sizeof(req.entry));
            req.state = REQ_IDLE;
            state = ST_READ;
            return;
        }
        // LIST: one entry per round trip, and only once the previous line is on its way.
        if (out_len > sizeof(out) / 2) return;
        if (req.result > 0) {
            const vault_entry_t *e = &req.entry;
            reply("E\t%d\t%s\t%s\t%d\t%d\n", list_index, e->name, e->login, e->flags, (int)strlen(e->pass));
            memset(&req.entry, 0, sizeof(req.entry));
        }
        if (req.result > 0 && ++list_index < req.count) {
            post(OP_GET, list_index);
        } else {
            reply(req.result == -3 ? "ERR locked\n" : "OK\n");
            req.state = REQ_IDLE;
            state = ST_READ;
        }
        return;
    }

    if (state == ST_BACKUP) {
        // A line at a time, as the serial buffer drains.
        while (out_len < sizeof(out) / 2 && backup_pos < xfer_len) {
            size_t k = xfer_len - backup_pos < B64_CHUNK ? xfer_len - backup_pos : B64_CHUNK;
            reply_base64(xfer + backup_pos, k);
            backup_pos += k;
        }
        if (backup_pos >= xfer_len && out_len < sizeof(out) / 2) {
            reply("OK %u\n", (unsigned)xfer_len);
            memset(xfer, 0, sizeof(xfer));
            xfer_len = 0;
            state = ST_READ;
        }
        return;
    }

    while (state == ST_READ && tud_cdc_available() && out_len < sizeof(out) / 2) {
        char ch = (char)tud_cdc_read_char();
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (line_len < LINE_MAX) line[line_len++] = ch;
            else line_overflow = true;
            continue;
        }
        line[line_len] = 0;
        if (line_overflow) reply("ERR line too long\n");
        else if (line_len) handle_line(line);
        memset(line, 0, sizeof(line));
        line_len = 0;
        line_overflow = false;
    }
}
