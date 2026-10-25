#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "DEV_Config.h"   // must precede the Waveshare headers (UBYTE/UWORD)
#include "AMOLED_1in8.h"
#include "FT3168.h"
#include "backup.h"
#include "buttons.h"
#include "crypto.h"
#include "editor.h"
#include "fido_glue.h"
#include "hotkeys.h"
#include "lvgl.h"
#include "usb_kbd.h"
#include "pass_vault.h"

#define HOR_RES AMOLED_1IN8_WIDTH
#define VER_RES AMOLED_1IN8_HEIGHT
#define BUF_LINES 40
#define MARGIN 24   // keeps content clear of the panel's rounded corners
#define TOP    28   // where the list and the hotkey grid start, now that there's no top bar

#define DIM_AFTER_MS   30000
#define DIM_BRIGHTNESS 5
#define LOCK_AFTER_MS  (5 * 60 * 1000)   // with a PIN set

#define PIN_MIN 4
#define PIN_MAX 12

#define COLOR_CARD   0x1C1C1E
#define COLOR_ACCENT 0x0A84FF
#define COLOR_DANGER 0xFF453A
#define COLOR_OK     0x30D158
#define COLOR_MUTED  0x8E8E93

#define BULLETS "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_a[HOR_RES * BUF_LINES];
static lv_color_t buf_b[HOR_RES * BUF_LINES];
static lv_indev_drv_t indev_drv;

static int brightness = 70;
static bool dimmed;
static bool typing;

static lv_obj_t *toast;
static lv_timer_t *toast_timer;
static lv_obj_t *usb_label;      // on screens that show the connection state, else NULL
static lv_obj_t *editor_label;
static int usb_shown = -1, editor_shown = -1;
static lv_obj_t *mbox;               // open confirmation dialog, if any
static lv_obj_t *fido_overlay;       // security key confirmation, over everything else
static lv_timer_t *screen_timer;     // belongs to the current screen, deleted with it

// Which screen is up, so it can be rebuilt when the editor changes the list.
static enum { SCR_LOCK, SCR_ENROLL, SCR_RESTORE, SCR_LIST, SCR_KEYS, SCR_DETAIL, SCR_SETTINGS, SCR_RECOVERY, SCR_MENU } screen;
static int main_screen = SCR_LIST;   // Passwords or Hotkeys, whichever was opened last
static int menu_from;                // where the menu was opened, to go back on a second press

// ---------------------------------------------------------------------------
// Display and touch glue

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    AMOLED_1IN8_SetWindows(area->x1, area->y1, area->x2 + 1, area->y2 + 1);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(dma_tx, &c, &qspi.pio->txf[qspi.sm], color_p,
                          lv_area_get_size(area) * 2, true);
    // flush_ready comes from dma_handler.
}

static void dma_handler(void) {
    if (dma_channel_get_irq0_status(dma_tx)) {
        dma_channel_acknowledge_irq0(dma_tx);
        QSPI_Deselect(qspi);
        lv_disp_flush_ready(&disp_drv);
    }
}

// The SH8601 wants windows that start on an even pixel and have an even size.
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// The FT3168 signals touches on its INT pin; reading it blindly can hit the controller
// while it dozes and return garbage that looks like a finger that never lifts.
static volatile uint32_t touch_irqs;

static void touch_irq_cb(uint gpio, uint32_t events) {
    if (gpio == Touch_INT_PIN) touch_irqs++;
}

static bool touch_read(lv_point_t *p) {
    static uint32_t seen_irqs;
    uint32_t irqs = touch_irqs;
    bool active = irqs != seen_irqs || !gpio_get(Touch_INT_PIN);
    seen_irqs = irqs;
    if (!active) return false;

    // TD_STATUS, X1_H, X1_L, Y1_H, Y1_L in one transaction.
    uint8_t reg = REG_FINGER_NUM, buf[5];
    int r = i2c_write_blocking(I2C_PORT, FT3168_I2C_ADDR, &reg, 1, true);
    if (r == 1) r = i2c_read_blocking(I2C_PORT, FT3168_I2C_ADDR, buf, sizeof(buf), false);
    if (r != (int)sizeof(buf)) return false;

    uint8_t fingers = buf[0] & 0x0F;
    uint16_t x = (uint16_t)((buf[1] & 0x0F) << 8 | buf[2]);
    uint16_t y = (uint16_t)((buf[3] & 0x0F) << 8 | buf[4]);
    if (fingers < 1 || fingers > 2 || x >= HOR_RES || y >= VER_RES) return false;
    p->x = x;
    p->y = y;
    return true;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static lv_point_t last;
    static bool waking;

    bool pressed = touch_read(&last);
    data->point = last;
    data->state = LV_INDEV_STATE_RELEASED;

    // A touch on a dimmed screen only wakes it: the finger might be on a "Type" button.
    if (pressed && (dimmed || waking)) {
        waking = true;
        lv_disp_trig_activity(NULL);
        return;
    }
    if (!pressed) {
        waking = false;
        return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
}

// ---------------------------------------------------------------------------
// Small helpers

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// A transition with no properties: state changes apply at once. LVGL 8.1 dereferences the
// transition descriptor without a NULL check, so "no transition" has to be a real, empty one.
static const lv_style_prop_t no_props[] = {0};
static lv_style_transition_dsc_t no_transition;

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, lv_coord_t w, lv_coord_t h,
                        lv_event_cb_t cb, void *user_data) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    // The theme grows a pressed button and fades its colour; both repaint it several times over
    // QSPI, which is what makes large buttons feel sluggish. Change state in one frame instead.
    lv_obj_set_style_transform_width(b, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transition(b, &no_transition, 0);
    lv_obj_set_style_transition(b, &no_transition, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    if (text) {
        lv_obj_t *l = label(b, text, &lv_font_montserrat_24, 0xFFFFFF);
        lv_obj_center(l);
    }
    return b;
}

static lv_obj_t *new_screen(void) {
    if (screen_timer) {
        lv_timer_del(screen_timer);
        screen_timer = NULL;
    }
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    usb_label = editor_label = NULL;
    return s;
}

static void show(lv_obj_t *s) {
    if (mbox) {
        lv_msgbox_close_async(mbox);
        mbox = NULL;
    }
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(s);
    // Async: show() is usually called from an event of a widget on the old screen.
    lv_obj_del_async(old);
}

// Title row with an optional back button on the left.
static lv_obj_t *header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb) {
    lv_coord_t x = MARGIN;
    if (back_cb) {
        lv_obj_t *b = button(scr, LV_SYMBOL_LEFT, COLOR_CARD, 60, 52, back_cb, NULL);
        lv_obj_set_pos(b, MARGIN, 12);
        x += 72;
    }
    lv_obj_t *t = label(scr, title, &lv_font_montserrat_26, 0xFFFFFF);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, HOR_RES - x - MARGIN - 60);
    lv_obj_set_pos(t, x, 24);
    return t;
}

// USB and (optionally) editor connection icons, top right.
static void status_icons(lv_obj_t *scr, lv_coord_t right, bool with_editor) {
    usb_label = label(scr, LV_SYMBOL_USB, &lv_font_montserrat_24, COLOR_MUTED);
    lv_obj_align(usb_label, LV_ALIGN_TOP_RIGHT, -right, 26);
    if (with_editor) {
        editor_label = label(scr, LV_SYMBOL_EDIT, &lv_font_montserrat_20, COLOR_MUTED);
        lv_obj_align(editor_label, LV_ALIGN_TOP_RIGHT, -right - 40, 28);
    }
    usb_shown = editor_shown = -1;
}

static void toast_hide_cb(lv_timer_t *t) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = NULL;
}

// Short message over everything; ms = 0 keeps it until the next one.
static void toast_show(const char *msg, uint32_t color, uint32_t ms) {
    lv_label_set_text(toast, msg);
    lv_obj_set_style_bg_color(toast, lv_color_hex(color), 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) {
        lv_timer_del(toast_timer);
        toast_timer = NULL;
    }
    if (ms) {
        toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
        lv_timer_set_repeat_count(toast_timer, 1);
    }
}

typedef void (*confirm_fn)(bool yes);

static void confirm_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_current_target(e);
    confirm_fn fn = (confirm_fn)lv_event_get_user_data(e);
    bool yes = lv_msgbox_get_active_btn(m) == 1;
    lv_msgbox_close_async(m);
    mbox = NULL;
    fn(yes);
}

static void confirm(const char *title, const char *text, const char *yes, confirm_fn fn) {
    static const char *btns[3];
    btns[0] = "Cancel";
    btns[1] = yes;
    btns[2] = "";
    mbox = lv_msgbox_create(NULL, title, text, btns, false);
    lv_obj_set_width(mbox, HOR_RES - 2 * MARGIN);
    lv_obj_t *bm = lv_msgbox_get_btns(mbox);
    lv_obj_set_height(bm, 56);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COLOR_DANGER), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_btnmatrix_set_btn_ctrl(bm, 1, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(mbox, confirm_cb, LV_EVENT_VALUE_CHANGED, (void *)fn);
    lv_obj_center(mbox);
}

// ---------------------------------------------------------------------------
// Typing

typedef enum { TYPE_DEFAULT, TYPE_LOGIN, TYPE_PASS } type_what_t;

static void type_entry(const vault_entry_t *e, type_what_t what) {
    if (!e) return;
    if (!usb_kbd_mounted()) {
        toast_show(LV_SYMBOL_USB "  Not connected", COLOR_DANGER, 2000);
        return;
    }
    char buf[VAULT_LOGIN_LEN + VAULT_PASS_LEN + 4];
    size_t n = 0;
    bool with_login = what == TYPE_LOGIN || (what == TYPE_DEFAULT && (e->flags & VAULT_FLAG_LOGIN) && e->login[0]);
    if (with_login) {
        n = strlen(e->login);
        memcpy(buf, e->login, n);
        if (what == TYPE_DEFAULT) buf[n++] = '\t';
    }
    if (what != TYPE_LOGIN) {
        size_t k = strlen(e->pass);
        memcpy(buf + n, e->pass, k);
        n += k;
    }
    if (what == TYPE_DEFAULT && (e->flags & VAULT_FLAG_ENTER)) buf[n++] = '\n';
    buf[n] = 0;

    bool ok = usb_kbd_type(buf);
    memset(buf, 0, sizeof(buf));
    if (ok) {
        typing = true;
        toast_show(LV_SYMBOL_KEYBOARD "  Typing...", COLOR_ACCENT, 0);
    } else {
        toast_show("Busy, try again", COLOR_DANGER, 1500);
    }
}

// ---------------------------------------------------------------------------
// Screens

static void show_list(void);
static void show_detail(int index);
static void show_settings(void);
static void show_recovery(void);

static int cur_index;   // entry on the detail screen
static lv_coord_t list_scroll;

// --- Digit entry: unlocking, setting a PIN, restoring a backup ---

// What the digits being entered are for.
enum { PURPOSE_UNLOCK, PURPOSE_NEW, PURPOSE_REPEAT, PURPOSE_RESTORE };

static int sec_purpose;
static bool sec_busy;
static char sec_first[VAULT_SECRET_MAX + 1];   // first entry while setting a new secret
static char sec_work[VAULT_SECRET_MAX + 1];    // handed to the slow vault call
static lv_obj_t *sec_info;

static void show_pin_screen(int purpose, const char *info);

static void show_unlock(const char *info) {
    show_pin_screen(PURPOSE_UNLOCK, info);
}

// Runs from a one-shot timer, so "Unlocking..." is on screen during the slow key derivation.
static void sec_work_cb(lv_timer_t *t) {
    sec_busy = false;
    if (sec_purpose == PURPOSE_UNLOCK) {
        unlock_result_t r = vault_unlock(sec_work);
        if (r == UNLOCK_OK) {
            show_list();
        } else if (r == UNLOCK_WIPED) {
            show_list();
            toast_show("Too many attempts: all erased", COLOR_DANGER, 4000);
        } else {
            char msg[48];
            int left = vault_attempts_left();
            if (left >= 0) snprintf(msg, sizeof(msg), "Wrong PIN. Attempts left: %d", left);
            else snprintf(msg, sizeof(msg), "Wrong PIN");
            show_unlock(msg);
        }
    } else if (sec_purpose == PURPOSE_RESTORE) {
        size_t len;
        bool replace;
        int entries, added, skipped;
        const uint8_t *data = editor_restore_data(&len, &replace, &entries);
        // A board that never showed its own code takes over the backup's, so the code on paper
        // keeps working. A code once shown never changes behind the user's back.
        bool adopt = !vault_has_recovery_code();
        restore_result_t r = backup_import(data, len, sec_work, replace, &added, &skipped);
        if (r == RESTORE_WRONG_CODE) {
            show_pin_screen(PURPOSE_RESTORE, "Wrong code, try again");
        } else {
            if (r == RESTORE_OK && adopt) vault_set_recovery_code(sec_work);
            editor_restore_finish(r == RESTORE_OK, added, skipped);
            list_scroll = 0;
            show_list();
            if (r == RESTORE_OK) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Restored %d, skipped %d", added, skipped);
                toast_show(msg, COLOR_OK, 3000);
            } else {
                toast_show(r == RESTORE_BAD_FILE ? "Not a Pass Key backup" : "Flash write failed", COLOR_DANGER, 3000);
            }
        }
    } else {
        bool ok = vault_set_lock(LOCK_PIN, sec_work);
        show_settings();
        if (ok) toast_show("PIN set", COLOR_OK, 2000);
        else toast_show("Flash write failed", COLOR_DANGER, 2000);
    }
    secure_zero(sec_work, sizeof(sec_work));
    secure_zero(sec_first, sizeof(sec_first));
}

static void sec_submit(const char *secret) {
    switch (sec_purpose) {
    case PURPOSE_NEW:
        snprintf(sec_first, sizeof(sec_first), "%s", secret);
        show_pin_screen(PURPOSE_REPEAT, NULL);
        return;
    case PURPOSE_REPEAT:
        if (strcmp(secret, sec_first) != 0) {
            secure_zero(sec_first, sizeof(sec_first));
            show_pin_screen(PURPOSE_NEW, "Did not match, start again");
            return;
        }
        lv_label_set_text(sec_info, "Saving...");
        break;
    case PURPOSE_RESTORE:
        lv_label_set_text(sec_info, "Checking the code...");
        break;
    default:
        lv_label_set_text(sec_info, "Unlocking...");
        break;
    }
    lv_obj_set_style_text_color(sec_info, lv_color_hex(COLOR_MUTED), 0);
    snprintf(sec_work, sizeof(sec_work), "%s", secret);
    sec_busy = true;
    lv_timer_t *t = lv_timer_create(sec_work_cb, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void sec_back_cb(lv_event_t *e) {
    secure_zero(sec_first, sizeof(sec_first));
    if (sec_purpose == PURPOSE_RESTORE) {
        editor_restore_finish(false, 0, 0);
        show_list();
        toast_show("Restore cancelled", COLOR_CARD, 1500);
        return;
    }
    show_settings();
}

// --- PIN pad ---

static lv_obj_t *pin_ta;

static const char *pin_pad_map[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};

static void pin_pad_cb(lv_event_t *e) {
    if (sec_busy) return;
    lv_obj_t *pad = lv_event_get_target(e);
    const char *txt = lv_btnmatrix_get_btn_text(pad, lv_btnmatrix_get_selected_btn(pad));
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(pin_ta);
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        const char *pin = lv_textarea_get_text(pin_ta);
        if (sec_purpose == PURPOSE_RESTORE && strlen(pin) != VAULT_RECOVERY_LEN) {
            lv_label_set_text_fmt(sec_info, "The code has %d digits", VAULT_RECOVERY_LEN);
            lv_obj_set_style_text_color(sec_info, lv_color_hex(COLOR_DANGER), 0);
            return;
        }
        if (sec_purpose != PURPOSE_RESTORE && strlen(pin) < PIN_MIN) {
            lv_label_set_text_fmt(sec_info, "PIN must be %d to %d digits", PIN_MIN, PIN_MAX);
            lv_obj_set_style_text_color(sec_info, lv_color_hex(COLOR_DANGER), 0);
            return;
        }
        char buf[VAULT_SECRET_MAX + 1];
        strncpy(buf, pin, sizeof(buf) - 1);
        buf[VAULT_SECRET_MAX] = 0;
        secure_zero((char *)pin, strlen(pin));
        sec_submit(buf);
        secure_zero(buf, sizeof(buf));
    } else {
        lv_textarea_add_text(pin_ta, txt);
    }
}

static void build_pin(lv_obj_t *scr) {
    // The recovery code is long and not secret on its own (it only opens a file), so it's shown.
    bool code = sec_purpose == PURPOSE_RESTORE;
    pin_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(pin_ta, true);
    lv_textarea_set_password_mode(pin_ta, !code);
    lv_textarea_set_password_show_time(pin_ta, 0);
    lv_textarea_set_max_length(pin_ta, code ? VAULT_RECOVERY_LEN : PIN_MAX);
    lv_textarea_set_accepted_chars(pin_ta, "0123456789");
    lv_obj_set_style_text_font(pin_ta, code ? &lv_font_montserrat_20 : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(pin_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(pin_ta, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_width(pin_ta, 0, 0);
    lv_obj_clear_flag(pin_ta, LV_OBJ_FLAG_CLICKABLE);   // the pad below is the only input
    lv_obj_set_size(pin_ta, HOR_RES - 2 * MARGIN - 40, 58);
    lv_obj_align(pin_ta, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t *pad = lv_btnmatrix_create(scr);
    lv_btnmatrix_set_map(pad, pin_pad_map);
    lv_obj_set_size(pad, HOR_RES - 2 * MARGIN - 20, 244);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_set_style_pad_all(pad, 0, 0);
    lv_obj_set_style_pad_gap(pad, 10, 0);
    lv_obj_set_style_radius(pad, 14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pad, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_text_font(pad, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_btnmatrix_set_btn_ctrl(pad, 11, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_style_bg_color(pad, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(pad, pin_pad_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void show_pin_screen(int purpose, const char *info) {
    static const char *const titles[] = {"Enter PIN", "New PIN", "Repeat the PIN", "Restore backup"};
    sec_purpose = purpose;
    sec_busy = false;
    screen = purpose == PURPOSE_UNLOCK ? SCR_LOCK : purpose == PURPOSE_RESTORE ? SCR_RESTORE : SCR_ENROLL;

    lv_obj_t *scr = new_screen();
    lv_obj_t *t = header(scr, titles[purpose], purpose == PURPOSE_UNLOCK ? NULL : sec_back_cb);
    if (purpose == PURPOSE_UNLOCK) {
        lv_obj_set_width(t, HOR_RES - 2 * MARGIN);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }

    char hint[96] = "";
    if (purpose == PURPOSE_NEW) {
        strcpy(hint, "4 to 12 digits");
    } else if (purpose == PURPOSE_RESTORE) {
        size_t len;
        bool replace;
        int entries;
        editor_restore_data(&len, &replace, &entries);
        snprintf(hint, sizeof(hint), "%d entries, %s.\nEnter the backup's recovery code.", entries,
                 replace ? "replacing the list" : "added to the list");
    }
    sec_info = label(scr, info ? info : hint, &lv_font_montserrat_16, info ? COLOR_DANGER : COLOR_MUTED);
    lv_obj_set_width(sec_info, HOR_RES - 2 * MARGIN);
    lv_obj_set_style_text_align(sec_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sec_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(sec_info, LV_ALIGN_TOP_MID, 0, 140);

    build_pin(scr);
    show(scr);
}

static void lock_now(void) {
    vault_lock();
    show_unlock(NULL);
}

// --- List ---

static void row_cb(lv_event_t *e) {
    show_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void quick_type_cb(lv_event_t *e) {
    type_entry(vault_get((int)(intptr_t)lv_event_get_user_data(e)), TYPE_DEFAULT);
}

static void list_scroll_cb(lv_event_t *e) {
    list_scroll = lv_obj_get_scroll_y(lv_event_get_target(e));
}

static void show_keys(void);

static void show_list(void) {
    screen = main_screen = SCR_LIST;
    lv_obj_t *scr = new_screen();

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, HOR_RES - 2 * MARGIN + 8, VER_RES - TOP - 16);
    lv_obj_set_pos(list, MARGIN - 4, TOP);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_bottom(list, 30, 0);   // last row clear of the rounded corners
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int n = vault_count();
    const lv_coord_t text_w = HOR_RES - 2 * MARGIN - 116;
    for (int i = 0; i < n; i++) {
        const vault_entry_t *en = vault_get(i);
        lv_obj_t *row = button(list, NULL, COLOR_CARD, lv_pct(100), 84, row_cb, (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);

        lv_obj_t *name = label(row, en->name, &lv_font_montserrat_24, 0xFFFFFF);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, text_w);
        lv_obj_align(name, en->login[0] ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
        if (en->login[0]) {
            lv_obj_t *login = label(row, en->login, &lv_font_montserrat_16, COLOR_MUTED);
            lv_label_set_long_mode(login, LV_LABEL_LONG_DOT);
            lv_obj_set_width(login, text_w);
            lv_obj_align(login, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
        // One tap types the entry; the rest of the row opens its details.
        lv_obj_t *kb = button(row, LV_SYMBOL_KEYBOARD, COLOR_ACCENT, 80, 64, quick_type_cb, (void *)(intptr_t)i);
        lv_obj_align(kb, LV_ALIGN_RIGHT_MID, 8, 0);
    }
    if (n == 0) {
        lv_obj_t *empty = label(list,
                                "No passwords yet.\n\n"
                                "Connect the board to a computer\n"
                                "and open passkey.html\n"
                                "in Chrome or Edge to add them.\n\n"
                                "BOOT or PWR button:\n"
                                "modes and settings.",
                                &lv_font_montserrat_16, COLOR_MUTED);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 80, 0);
    }

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, list_scroll, LV_ANIM_OFF);
    lv_obj_add_event_cb(list, list_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    show(scr);
}

// --- Hotkeys ---

static void hotkey_cb(lv_event_t *e) {
    const hotkey_t *k = hotkeys_get((int)(intptr_t)lv_event_get_user_data(e));
    if (!k) return;
    if (!usb_kbd_mounted()) {
        toast_show(LV_SYMBOL_USB "  Not connected", COLOR_DANGER, 2000);
        return;
    }
    bool ok = k->type == HOTKEY_MEDIA ? usb_kbd_media(k->code) : usb_kbd_combo(k->mods, (uint8_t)k->code);
    if (!ok) toast_show("Busy, try again", COLOR_DANGER, 1500);
}

static void show_keys(void) {
    screen = main_screen = SCR_KEYS;
    lv_obj_t *scr = new_screen();

    // Four big tiles, 2 x 2, filling the screen: nothing to scroll.
    const lv_coord_t gap = 12;
    const lv_coord_t tile_w = (HOR_RES - 2 * MARGIN - gap) / 2;
    const lv_coord_t tile_h = (VER_RES - 2 * TOP - gap) / 2;
    int n = hotkeys_count();
    for (int i = 0; i < n; i++) {
        const hotkey_t *k = hotkeys_get(i);
        bool media = k->type == HOTKEY_MEDIA;
        // Media keys in purple, so they stand apart from shortcuts.
        lv_obj_t *tile = button(scr, NULL, media ? 0x3A2A5C : COLOR_CARD, tile_w, tile_h, hotkey_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(tile, MARGIN + (i % 2) * (tile_w + gap), TOP + (i / 2) * (tile_h + gap));
        lv_obj_set_style_radius(tile, 22, 0);
        lv_obj_set_style_pad_all(tile, 12, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);

        lv_obj_t *l = label(tile, media ? LV_SYMBOL_AUDIO : LV_SYMBOL_KEYBOARD, &lv_font_montserrat_28, COLOR_MUTED);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8);
        l = label(tile, k->label, &lv_font_montserrat_24, 0xFFFFFF);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, tile_w - 24);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 4);
        char desc[40];
        hotkey_describe(k, desc, sizeof(desc));
        l = label(tile, desc, &lv_font_montserrat_14, COLOR_MUTED);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, tile_w - 24);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
    if (n == 0) {
        lv_obj_t *empty = label(scr, "No hotkeys yet.\n\nSet them up in passkey.html\non the computer.",
                                &lv_font_montserrat_16, COLOR_MUTED);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
    }
    show(scr);
}

// --- Mode menu (BOOT or PWR button) ---

static void menu_pick_cb(lv_event_t *e) {
    int target = (int)(intptr_t)lv_event_get_user_data(e);
    if (target == SCR_LIST) show_list();
    else if (target == SCR_KEYS) show_keys();
    else if (target == SCR_SETTINGS) show_settings();
    else lock_now();
}

static void show_menu(void) {
    menu_from = screen;
    screen = SCR_MENU;
    lv_obj_t *scr = new_screen();
    lv_obj_t *t = label(scr, "Mode", &lv_font_montserrat_26, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);

    static const struct { const char *text; int target; } items[] = {
        {LV_SYMBOL_LIST "   Passwords", SCR_LIST},
        {LV_SYMBOL_KEYBOARD "   Hotkeys", SCR_KEYS},
        {LV_SYMBOL_SETTINGS "   Settings", SCR_SETTINGS},
        {LV_SYMBOL_POWER "   Lock", SCR_LOCK},
    };
    int n = vault_lock_mode() != LOCK_NONE ? 4 : 3;   // Lock only means something with a PIN
    const lv_coord_t h = 72, gap = 14, y0 = 80;
    for (int i = 0; i < n; i++) {
        bool current = items[i].target == menu_from || (items[i].target == SCR_LIST && menu_from == SCR_DETAIL) ||
                       (items[i].target == SCR_SETTINGS && menu_from == SCR_RECOVERY);
        lv_obj_t *b = button(scr, items[i].text, current ? COLOR_ACCENT : COLOR_CARD, HOR_RES - 2 * MARGIN - 20, h,
                             menu_pick_cb, (void *)(intptr_t)items[i].target);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y0 + i * (h + gap));
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_26, 0);
    }
    lv_obj_t *hint = label(scr, "Press the button again to go back", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
    show(scr);
}

static void show_main(void) {
    if (main_screen == SCR_KEYS) show_keys();
    else show_list();
}

// Button press: open the menu, or close it again.
static void menu_toggle(void) {
    if (!vault_is_unlocked() || sec_busy || fido_overlay) return;   // never a way around the PIN screen
    if (screen == SCR_LOCK) return;
    if (screen != SCR_MENU) {
        show_menu();
    } else if (menu_from == SCR_SETTINGS || menu_from == SCR_RECOVERY) {
        show_settings();
    } else {
        show_main();
    }
}

// --- Detail ---

static lv_obj_t *det_pass;
static bool det_revealed;

static void back_to_list_cb(lv_event_t *e) {
    show_list();
}

static void settings_done_cb(lv_event_t *e) {
    show_main();
}

static void det_type_cb(lv_event_t *e) {
    type_entry(vault_get(cur_index), (type_what_t)(intptr_t)lv_event_get_user_data(e));
}

static void det_reveal_cb(lv_event_t *e) {
    const vault_entry_t *en = vault_get(cur_index);
    if (!en) return;
    det_revealed = !det_revealed;
    lv_label_set_text(det_pass, det_revealed ? en->pass : BULLETS);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), det_revealed ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void show_detail(int index) {
    const vault_entry_t *en = vault_get(index);
    if (!en) {
        show_list();
        return;
    }
    screen = SCR_DETAIL;
    cur_index = index;
    det_revealed = false;
    lv_obj_t *scr = new_screen();
    header(scr, en->name, back_to_list_cb);

    const int w = HOR_RES - 2 * MARGIN;
    lv_obj_t *l = label(scr, "LOGIN", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 76);
    l = label(scr, en->login[0] ? en->login : "-", &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, MARGIN, 96);

    l = label(scr, "PASSWORD", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 130);
    det_pass = label(scr, BULLETS, &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_long_mode(det_pass, LV_LABEL_LONG_DOT);
    lv_obj_set_width(det_pass, w - 76);
    lv_obj_set_pos(det_pass, MARGIN, 152);
    lv_obj_t *eye = button(scr, LV_SYMBOL_EYE_OPEN, COLOR_CARD, 64, 52, det_reveal_cb, NULL);
    lv_obj_set_pos(eye, HOR_RES - MARGIN - 64, 134);

    // What the big button types, e.g. "login  >  password  ENTER".
    char seq[48];
    snprintf(seq, sizeof(seq), "%s%s%s", (en->flags & VAULT_FLAG_LOGIN) && en->login[0] ? "login  " LV_SYMBOL_RIGHT "  " : "",
             "password", en->flags & VAULT_FLAG_ENTER ? "  " LV_SYMBOL_NEW_LINE : "");
    lv_obj_t *big = button(scr, NULL, COLOR_ACCENT, w, 110, det_type_cb, (void *)(intptr_t)TYPE_DEFAULT);
    lv_obj_set_pos(big, MARGIN, 204);
    l = label(big, LV_SYMBOL_KEYBOARD "  Type", &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);
    l = label(big, seq, &lv_font_montserrat_16, 0xD0E4FF);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 22);

    const int hw = (w - 12) / 2;
    lv_obj_t *b = button(scr, "Login", COLOR_CARD, hw, 76, det_type_cb, (void *)(intptr_t)TYPE_LOGIN);
    lv_obj_set_pos(b, MARGIN, 328);
    if (!en->login[0]) lv_obj_add_state(b, LV_STATE_DISABLED);
    b = button(scr, "Password", COLOR_CARD, hw, 76, det_type_cb, (void *)(intptr_t)TYPE_PASS);
    lv_obj_set_pos(b, MARGIN + hw + 12, 328);

    show(scr);
}

// --- Settings ---

static void brightness_cb(lv_event_t *e) {
    brightness = lv_slider_get_value(lv_event_get_target(e));
    AMOLED_1IN8_SetBrightness(brightness);
}

static void lock_off_confirmed(bool yes) {
    if (yes && !vault_set_lock(LOCK_NONE, NULL)) toast_show("Flash write failed", COLOR_DANGER, 2000);
    else if (yes) toast_show("Protection off", COLOR_OK, 2000);
    show_settings();   // also puts the selector back after "Cancel"
}

static void protection_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    lock_mode_t want = (lock_mode_t)lv_btnmatrix_get_selected_btn(bm);
    if (want == vault_lock_mode()) {
        lv_btnmatrix_set_btn_ctrl(bm, want, LV_BTNMATRIX_CTRL_CHECKED);   // a second tap would uncheck it
        return;
    }
    if (want == LOCK_NONE) {
        confirm("Turn off protection?", "Passwords will be stored unencrypted and open without a PIN.",
                "Turn off", lock_off_confirmed);
    } else {
        show_pin_screen(PURPOSE_NEW, NULL);
    }
}

static void recovery_cb(lv_event_t *e) {
    show_recovery();
}

static void settings_back_cb(lv_event_t *e) {
    show_settings();
}

static void change_secret_cb(lv_event_t *e) {
    show_pin_screen(PURPOSE_NEW, NULL);
}

static void show_recovery(void) {
    const char *code = vault_recovery_code();   // made and saved on first use
    if (!code) {
        toast_show("Flash write failed", COLOR_DANGER, 2000);
        return;
    }
    screen = SCR_RECOVERY;
    lv_obj_t *scr = new_screen();
    header(scr, "Recovery code", settings_back_cb);
    const int w = HOR_RES - 2 * MARGIN;

    char text[VAULT_RECOVERY_LEN + 8];
    size_t n = 0;
    for (int i = 0; i < VAULT_RECOVERY_LEN; i++) {
        if (i && i % 4 == 0) text[n++] = i % 12 == 0 ? '\n' : ' ';
        text[n++] = code[i];
    }
    text[n] = 0;
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, w, 120);
    lv_obj_set_pos(card, MARGIN, 80);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = label(card, text, &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 10, 0);
    lv_obj_center(l);
    secure_zero(text, sizeof(text));

    l = label(scr,
              "Write it down on paper and keep it\n"
              "away from the board. It opens the\n"
              "backup files saved with passkey.html:\n"
              "you type it here when restoring.\n\n"
              "The code and a backup file together\n"
              "give access to all passwords.",
              &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, MARGIN, 220);
    show(scr);
}

static void show_settings(void) {
    screen = SCR_SETTINGS;
    lv_obj_t *scr = new_screen();
    header(scr, "Settings", settings_done_cb);
    status_icons(scr, MARGIN, true);
    const int w = HOR_RES - 2 * MARGIN;

    lv_obj_t *l = label(scr, "BRIGHTNESS", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 90);
    lv_obj_t *s = lv_slider_create(scr);
    lv_obj_set_size(s, w - 40, 20);
    lv_obj_set_pos(s, MARGIN + 20, 128);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);   // a knob big enough for a finger
    lv_obj_set_ext_click_area(s, 20);
    lv_slider_set_range(s, 10, 100);
    lv_slider_set_value(s, brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    l = label(scr, "PROTECTION", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 172);
    static const char *prot_map[] = {"Off", "PIN", ""};
    lv_obj_t *bm = lv_btnmatrix_create(scr);
    lv_btnmatrix_set_map(bm, prot_map);
    lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm, true);
    lv_btnmatrix_set_btn_ctrl(bm, vault_lock_mode(), LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, w, 64);
    lv_obj_set_pos(bm, MARGIN, 194);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_radius(bm, 16, 0);
    lv_obj_set_style_radius(bm, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, protection_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_coord_t y = 272;
    if (vault_lock_mode() != LOCK_NONE) {
        const int hw = (w - 12) / 2;
        lv_obj_t *b = button(scr, "Change PIN", COLOR_CARD, hw, 56, change_secret_cb, NULL);
        lv_obj_set_pos(b, MARGIN, y);
        b = button(scr, "Backup code", COLOR_CARD, hw, 56, recovery_cb, NULL);
        lv_obj_set_pos(b, MARGIN + hw + 12, y);
    } else {
        lv_obj_t *b = button(scr, "Backup recovery code", COLOR_CARD, w, 56, recovery_cb, NULL);
        lv_obj_set_pos(b, MARGIN, y);
    }
    y += 70;

    l = label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    if (vault_lock_mode() == LOCK_NONE)
        lv_label_set_text_fmt(l, "Entries: %d of %d. Without protection\nthe passwords are stored unencrypted.",
                              vault_count(), VAULT_MAX_ENTRIES);
    else
        lv_label_set_text_fmt(l, "Entries: %d of %d, encrypted. Locks after\n%d min idle; %d wrong tries erase all.",
                              vault_count(), VAULT_MAX_ENTRIES, LOCK_AFTER_MS / 60000, VAULT_MAX_ATTEMPTS);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, MARGIN, y);

    show(scr);
}

// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
    if (buttons_pressed()) {
        lv_disp_trig_activity(NULL);   // also wakes a dimmed screen
        menu_toggle();
    }

    if (typing && !usb_kbd_busy()) {
        typing = false;
        toast_show(LV_SYMBOL_OK "  Done", COLOR_OK, 1200);
    }

    int m = usb_kbd_mounted();
    if (usb_label && m != usb_shown) {
        usb_shown = m;
        lv_obj_set_style_text_color(usb_label, lv_color_hex(m ? COLOR_OK : COLOR_MUTED), 0);
    }
    static bool was_editing;
    bool editing = editor_connected();
    if (editor_label && editing != editor_shown) {
        editor_shown = editing;
        lv_obj_set_style_text_color(editor_label, lv_color_hex(editing ? COLOR_OK : COLOR_MUTED), 0);
    }
    if (editing != was_editing) {
        was_editing = editing;
        toast_show(editing ? LV_SYMBOL_EDIT "  Editor connected" : LV_SYMBOL_EDIT "  Editor closed", COLOR_CARD, 1500);
    }

    uint32_t idle = lv_disp_get_inactive_time(NULL);
    if (vault_lock_mode() != LOCK_NONE && vault_is_unlocked() && idle > LOCK_AFTER_MS && !typing) lock_now();

    // AMOLED: dim when idle to avoid burn-in.
    if (!dimmed && idle > DIM_AFTER_MS) {
        dimmed = true;
        AMOLED_1IN8_SetBrightness(DIM_BRIGHTNESS);
    } else if (dimmed && idle < DIM_AFTER_MS) {
        dimmed = false;
        AMOLED_1IN8_SetBrightness(brightness);
    }
}

// --- Security key confirmation (pico-fido asks for user presence) ---

static lv_obj_t *fido_buttons[2], *fido_status;

static void fido_answer_cb(lv_event_t *e) {
    bool allow = (bool)(intptr_t)lv_event_get_user_data(e);
    fido_presence_answer(allow);
    // Visible right away; the overlay closes when pico-fido has taken the answer.
    for (int i = 0; i < 2; i++) lv_obj_add_flag(fido_buttons[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(fido_status, allow ? LV_SYMBOL_OK "  Confirmed" : "Denied");
    lv_obj_set_style_text_color(fido_status, lv_color_hex(allow ? COLOR_OK : COLOR_DANGER), 0);
}

// Shown over whatever screen is up, so nothing underneath is lost; removed when pico-fido
// reports the request done, cancelled or timed out.
static void fido_overlay_update(void) {
    fido_ask_t ask = fido_presence_pending();
    if (ask == FIDO_ASK_NONE) {
        if (fido_overlay) {
            lv_obj_del(fido_overlay);
            fido_overlay = NULL;
        }
        return;
    }
    if (fido_overlay) return;

    lv_disp_trig_activity(NULL);   // wakes a dimmed screen
    fido_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(fido_overlay, HOR_RES, VER_RES);
    lv_obj_set_style_bg_color(fido_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fido_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fido_overlay, 0, 0);
    lv_obj_set_style_radius(fido_overlay, 0, 0);
    lv_obj_clear_flag(fido_overlay, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const titles[] = {"", "Sign in?", "Create a passkey?", "Security key"};
    lv_obj_t *l = label(fido_overlay, LV_SYMBOL_WARNING, &lv_font_montserrat_28, COLOR_ACCENT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 34);
    l = label(fido_overlay, titles[ask], &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 84);

    const char *site = fido_presence_site();
    l = label(fido_overlay, site[0] ? site : "a website or app", &lv_font_montserrat_24, site[0] ? 0xFFFFFF : COLOR_MUTED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, HOR_RES - 2 * MARGIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 140);
    l = label(fido_overlay, "Confirm only if you started this\non the computer.", &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 214);

    fido_status = label(fido_overlay, "", &lv_font_montserrat_26, COLOR_OK);
    lv_obj_align(fido_status, LV_ALIGN_BOTTOM_MID, 0, -110);

    const lv_coord_t w = HOR_RES - 2 * MARGIN;
    fido_buttons[0] = button(fido_overlay, LV_SYMBOL_OK "  Allow", COLOR_OK, w, 76, fido_answer_cb, (void *)(intptr_t)true);
    lv_obj_align(fido_buttons[0], LV_ALIGN_BOTTOM_MID, 0, -112);
    fido_buttons[1] = button(fido_overlay, "Deny", COLOR_CARD, w, 64, fido_answer_cb, (void *)(intptr_t)false);
    lv_obj_align(fido_buttons[1], LV_ALIGN_BOTTOM_MID, 0, -32);
}

void ui_init(void) {
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    AMOLED_1IN8_Init();
    AMOLED_1IN8_SetBrightness(brightness);
    AMOLED_1IN8_Clear(BLACK);
    FT3168_Init(FT3168_Point_Mode);
    // INT is open-drain on the controller side; same setup as Waveshare's LVGL demo.
    DEV_KEY_Config(Touch_INT_PIN);
    gpio_set_irq_enabled_with_callback(Touch_INT_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, touch_irq_cb);

    lv_init();
    lv_style_transition_dsc_init(&no_transition, no_props, lv_anim_path_linear, 0, 0, NULL);
    lv_disp_draw_buf_init(&draw_buf, buf_a, buf_b, HOR_RES * BUF_LINES);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = HOR_RES;
    disp_drv.ver_res = VER_RES;
    disp_drv.flush_cb = flush_cb;
    disp_drv.rounder_cb = rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    // The display DMA interrupt is serviced on this core, like everything else in the UI.
    dma_channel_set_irq0_enabled(dma_tx, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 22, 0);
    lv_obj_set_style_pad_hor(toast, 22, 0);
    lv_obj_set_style_pad_ver(toast, 10, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    hotkeys_init();
    buttons_init();   // AXP2101 on I2C1, next to touch
    vault_init();
    if (vault_is_unlocked()) show_list();
    else show_unlock(NULL);
    lv_timer_create(tick_cb, 100, NULL);
}

void ui_task(void) {
    // Rebuilt shortly after the editor's last change, not once per entry during an import.
    static absolute_time_t refresh_at;
    fido_overlay_update();
    // Editor requests are handled here rather than in a timer so listing stays quick.
    editor_set_state(!vault_is_unlocked(), vault_lock_mode());
    editor_activity_t act = editor_service();
    if (act != EDITOR_IDLE) lv_disp_trig_activity(NULL);   // editing counts as use: no auto-lock
    if (act == EDITOR_CHANGED) refresh_at = make_timeout_time_ms(300);
    // A backup was uploaded: ask for its code here on the board.
    if (editor_restore_waiting() && vault_is_unlocked() && screen != SCR_RESTORE && !sec_busy) {
        lv_disp_trig_activity(NULL);
        show_pin_screen(PURPOSE_RESTORE, NULL);
    }
    if (!is_nil_time(refresh_at) && time_reached(refresh_at)) {
        refresh_at = nil_time;
        list_scroll = 0;
        // A wipe from the editor also clears the lock, so the lock screen can go too.
        if (screen == SCR_LIST || screen == SCR_DETAIL || (screen == SCR_LOCK && vault_is_unlocked())) show_list();
        else if (screen == SCR_KEYS) show_keys();
        else if (screen == SCR_SETTINGS) show_settings();
    }
    lv_timer_handler();
}
