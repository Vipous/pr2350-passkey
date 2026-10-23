#include "editor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/sync.h"
#include "tusb.h"
#include "vault.h"

#define PROTOCOL_VERSION 1
#define LINE_MAX 320
#define MAX_FIELDS 6

// ---------------------------------------------------------------------------
// Mailbox between the cores: core 0 posts, core 1 runs it against the vault and marks it done.

typedef enum { OP_GET, OP_PUT, OP_DEL, OP_WIPE } op_t;
enum { REQ_IDLE, REQ_POSTED, REQ_DONE };

static struct {
    volatile int state;
    op_t op;
    int index;
    bool keep_pass;
    vault_entry_t entry;
    int result;   // new index for PUT, 1 otherwise; -1 flash/full, -2 no such entry
    int count;    // entries after the operation
} req;

static void post(op_t op, int index) {
    req.op = op;
    req.index = index;
    __dmb();
    req.state = REQ_POSTED;
}

bool editor_service(void) {
    if (req.state != REQ_POSTED) return false;
    __dmb();
    bool changed = false;
    const vault_entry_t *e;
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
    }
    req.count = vault_count();
    __dmb();
    req.state = REQ_DONE;
    return changed;
}

// ---------------------------------------------------------------------------
// Core 0: serial I/O

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;

static char out[1024];
static size_t out_len, out_pos;

static volatile bool connected;

typedef enum { ST_READ, ST_WAIT, ST_LIST } state_t;
static state_t state = ST_READ;
static op_t waiting_op;
static int list_index;

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
        reply("OK PassKey %d %d\n", PROTOCOL_VERSION, VAULT_MAX_ENTRIES);
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
        if (state == ST_WAIT) {
            if (req.result == -2) reply("ERR no such entry\n");
            else if (req.result < 0) reply("ERR %s\n", waiting_op == OP_PUT && req.count >= VAULT_MAX_ENTRIES ? "list is full" : "flash write failed");
            else if (waiting_op == OP_PUT) reply("OK %d\n", req.result);
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
            reply("OK\n");
            req.state = REQ_IDLE;
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
