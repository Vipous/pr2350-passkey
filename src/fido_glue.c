#include "fido_glue.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "button.h"
#include "event.h"
#include "file.h"
#include "flash.h"
#include "flash_store.h"
#include "hardware/sync.h"
#include "hwrng.h"
#include "led.h"
#include "otp.h"
#include "phy.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico_time.h"
#include "picokeys.h"
#include "random.h"
#include "serial.h"
#include "signal.h"
#include "usb.h"

// From pico-keys-sdk / pico-fido sources that don't declare them in a header.
extern int rescue_migrate_keydev(void);
extern void execute_tasks(void);
extern char *rp_id;
extern size_t rp_id_len;
extern uint8_t current_fido_operation;

static void log_add(char kind, uint32_t cid, uint8_t cmd, uint16_t len, uint8_t data0);

// ---------------------------------------------------------------------------
// Core 1: runs card threads handed over by pico-keys (see picokeys_core1_launch).

static void *(*volatile core1_job)(void *);

void picokeys_core1_launch(void *(*func)(void *)) {
    extern uint32_t core1_stack_peak(void);
    // pico-keys starts a card thread on core 1; len = deepest core 1 stack use so far
    log_add('J', 0, 0, (uint16_t)core1_stack_peak(), 0);
    core1_job = func;
    __sev();
}

void fido_core1_main(void) {
    // Lets core 0 pause this core for flash writes, both pico-keys' and the vault's.
    low_flash_init_core1();
    flash_store_core1_started();
    while (true) {
        void *(*job)(void *) = core1_job;
        if (!job) {
            __wfe();
            continue;
        }
        core1_job = NULL;
        job(NULL);   // returns when pico-keys sends EV_EXIT
    }
}

// ---------------------------------------------------------------------------
// Diagnostic log

#define LOG_SIZE 96

typedef struct {
    uint32_t ms;
    uint32_t cid;
    uint16_t len;
    char kind;      // '<' from host, '>' to host, 'P' presence event
    uint8_t cmd;    // CTAPHID command, or signal code for 'P'
    uint8_t data0;  // first data byte: CTAP command (request) or status (response)
} log_entry_t;

// Kept in RAM the boot code doesn't clear, so a watchdog reset after a hang leaves it readable.
#define LOG_MAGIC 0x464C4F47u   // "FLOG"
typedef struct {
    uint32_t magic;
    int head, count;
    volatile uint8_t stage;   // main loop step running now (see main.c)
    char crash[64];           // panic message or hard fault address, "" if none
    uint32_t pc[16];          // core 0 program counter, sampled every 10 ms (see main.c)
    uint32_t pc_head;
    uint32_t hang_pc[16];     // the samples from just before the last watchdog reset
    log_entry_t buf[LOG_SIZE];
} persistent_log_t;

static persistent_log_t __uninitialized_ram(plog);
#define log_buf   plog.buf
#define log_head  plog.head
#define log_count plog.count

void fido_log_boot(bool after_watchdog) {
    uint8_t last_stage = plog.stage;
    bool valid = plog.magic == LOG_MAGIC && plog.head >= 0 && plog.head < LOG_SIZE && plog.count >= 0 &&
                 plog.count <= LOG_SIZE;
    if (!after_watchdog || !valid) {
        plog.magic = LOG_MAGIC;
        plog.head = plog.count = 0;
        plog.crash[0] = 0;
        memset(plog.hang_pc, 0, sizeof(plog.hang_pc));
        last_stage = 0;
    } else {
        // Freeze where core 0 was spinning before the sampler starts again.
        for (int i = 0; i < 16; i++) plog.hang_pc[i] = plog.pc[(plog.pc_head + i) % 16];
    }
    plog.crash[sizeof(plog.crash) - 1] = 0;
    plog.stage = 0;
    log_add('B', 0, after_watchdog, 0, last_stage);
}

void fido_log_stage(uint8_t stage) {
    plog.stage = stage;
}

const char *fido_log_crash(void) {
    return plog.crash;
}

void fido_log_pc_sample(uint32_t pc) {
    plog.pc[plog.pc_head++ % 16] = pc;
}

int fido_log_pcs(uint32_t out[16]) {
    memcpy(out, plog.hang_pc, sizeof(plog.hang_pc));   // oldest first
    return 16;
}

// Panic and hard fault both end up here: note what happened, then let the watchdog reboot.
void __attribute__((noreturn)) fido_log_crash_and_wait(const char *what) {
    snprintf(plog.crash, sizeof(plog.crash), "core %u, step %u: %s", get_core_num(), plog.stage, what);
    while (true) tight_loop_contents();
}

void __attribute__((noreturn)) passkey_panic(const char *fmt, ...) {
    char msg[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "panic", ap);
    va_end(ap);
    fido_log_crash_and_wait(msg);
}

static void log_add(char kind, uint32_t cid, uint8_t cmd, uint16_t len, uint8_t data0) {
    log_entry_t *e = &log_buf[log_head];
    e->ms = to_ms_since_boot(get_absolute_time());
    e->cid = cid;
    e->len = len;
    e->kind = kind;
    e->cmd = cmd;
    e->data0 = data0;
    log_head = (log_head + 1) % LOG_SIZE;
    if (log_count < LOG_SIZE) log_count++;
}

void fido_log_packet(bool from_host, const uint8_t *p, int len) {
    if (len < 8 || !(p[4] & 0x80) || p[4] == 0xBB) return;   // only initial packets, no keepalives
    uint32_t cid = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    log_add(from_host ? '<' : '>', cid, p[4], (uint16_t)(p[5] << 8 | p[6]), p[7]);
}

int fido_log_count(void) {
    return log_count;
}

void fido_log_format(int index, char *out, int size) {
    const log_entry_t *e = &log_buf[(log_head - log_count + index + LOG_SIZE) % LOG_SIZE];
    snprintf(out, size, "%lu\t%c\t%08lx\t%02x\t%u\t%02x", (unsigned long)e->ms, e->kind,
             (unsigned long)e->cid, e->cmd, e->len, e->data0);
}

void fido_log_clear(void) {
    log_head = log_count = 0;
}

// ---------------------------------------------------------------------------
// User presence

static volatile fido_ask_t ask;
static char site[64];
// Allow acts like holding the button for a moment: pico-keys polls it every 10 ms (twice per poll)
// and confirms once it has seen it pressed and then released.
#define PRESS_MS 150
static volatile uint32_t press_until;   // board time in ms, 0 when not pressed

bool picokeys_board_button_read(void) {
    uint32_t until = press_until;
    if (!until) return false;
    if ((int32_t)(to_ms_since_boot(get_absolute_time()) - until) < 0) return true;
    press_until = 0;
    return false;
}

static int on_presence(signal_code_t code, void *data) {
    (void)data;
    log_add('P', 0, (uint8_t)code, 0, current_fido_operation);
    if (code == SIGNAL_USER_PRESENCE_REQUEST) {
        // rp_id points into the request, which stays put while core 1 waits for the answer.
        size_t n = rp_id && rp_id_len ? rp_id_len : 0;
        if (n >= sizeof(site)) n = sizeof(site) - 1;
        memcpy(site, n ? rp_id : "", n);
        site[n] = 0;
        press_until = 0;
#ifdef FIDO_AUTO_ALLOW   // test builds only (tests from the computer): confirms every request
        press_until = to_ms_since_boot(get_absolute_time()) + PRESS_MS;
#endif
        ask = current_fido_operation == OP_GA ? FIDO_ASK_SIGN_IN
            : current_fido_operation == OP_MC ? FIDO_ASK_REGISTER : FIDO_ASK_OTHER;
    } else {
        ask = FIDO_ASK_NONE;   // completed, cancelled or timed out
    }
    return 0;
}

fido_ask_t fido_presence_pending(void) {
    return ask;
}

const char *fido_presence_site(void) {
    return site;
}

void fido_presence_answer(bool allow) {
    if (ask == FIDO_ASK_NONE) return;
    log_add('P', 0, allow ? 0xA1 : 0xD0, 0, 0);   // A1 = Allow tapped, D0 = Deny tapped
    if (allow) press_until = to_ms_since_boot(get_absolute_time()) + PRESS_MS;
    else cancel_button = true;
}

// ---------------------------------------------------------------------------

void fido_start(void) {
    // The same steps as pico-keys' own main(), minus board, stdio and TinyUSB setup.
    serial_init();
    random_init();
    otp_init();          // keys from flash: see fido_keys.c
    low_flash_init();    // region fixed by PICOKEYS_FLASH_DATA_START/END
    file_scan_flash();
    rescue_migrate_keydev();
    init_rtc();
    phy_init();
    led_init();          // dummy driver (PICOKEYS_NO_LED)

    // Only the FIDO HID interface: the keyboard pico-keys could add (for OTP) isn't ours to use.
    // RAM only; whatever is saved in its settings stays as it is.
    phy_data.enabled_usb_itf = PHY_USB_ITF_HID;
    phy_data.enabled_usb_itf_present = true;
    // pico-fido treats user presence as optional unless a "button" timeout is configured; with
    // it every signature waits for Allow on the screen. U2F and reset use it too. pico-keys
    // treats a press after 15 s as a timeout, so the window lasts exactly that long.
    phy_data.up_btn = 15;
    phy_data.up_btn_present = true;
    usb_init();

    signal_add(SIGNAL_USER_PRESENCE_REQUEST, SIGNAL_FLAG_NONE, on_presence);
    signal_add(SIGNAL_USER_PRESENCE_COMPLETED, SIGNAL_FLAG_NONE, on_presence);
    signal_add(SIGNAL_USER_PRESENCE_CANCELLED, SIGNAL_FLAG_NONE, on_presence);
    signal_add(SIGNAL_USER_PRESENCE_TIMEOUT, SIGNAL_FLAG_NONE, on_presence);

    picokey_init();      // registers the FIDO application
}

void fido_poll(void) {
    fido_log_stage(41);
    execute_tasks();     // tud_task (again, harmless), USB/HID and LED tasks
    fido_log_stage(42);
    hwrng_task();
    fido_log_stage(43);
    flash_task();
    fido_log_stage(44);
    button_task();
}
