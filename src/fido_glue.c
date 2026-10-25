#include "fido_glue.h"
#include "fido_manage.h"

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

// ---------------------------------------------------------------------------
// Core 1: runs card threads handed over by pico-keys (see picokeys_core1_launch).

static void *(*volatile core1_job)(void *);

void picokeys_core1_launch(void *(*func)(void *)) {
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
    fido_manage_init();
}

void fido_poll(void) {
    execute_tasks();     // tud_task (again, harmless), USB/HID and LED tasks
    hwrng_task();
    flash_task();
    button_task();
    fido_manage_task();
}
