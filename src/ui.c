#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "DEV_Config.h"   // must precede the Waveshare headers (UBYTE/UWORD)
#include "AMOLED_1in8.h"
#include "FT3168.h"
#include "crypto.h"
#include "lvgl.h"
#include "pico/rand.h"
#include "usb_kbd.h"
#include "vault.h"

#define HOR_RES AMOLED_1IN8_WIDTH
#define VER_RES AMOLED_1IN8_HEIGHT
#define BUF_LINES 40
#define MARGIN 24   // keeps content clear of the panel's rounded corners

#define DIM_AFTER_MS   30000
#define LOCK_AFTER_MS  (5 * 60 * 1000)
#define DIM_BRIGHTNESS 5
#define GEN_LENGTH     16

#define COLOR_CARD   0x1C1C1E
#define COLOR_ACCENT 0x0A84FF
#define COLOR_DANGER 0xFF453A
#define COLOR_OK     0x30D158
#define COLOR_MUTED  0x8E8E93

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
static lv_obj_t *usb_label;   // on screens that show the USB state, else NULL
static int usb_shown = -1;
static lv_obj_t *mbox;        // open confirmation dialog, if any

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

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, lv_coord_t w, lv_coord_t h,
                        lv_event_cb_t cb, void *user_data) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    if (text) {
        lv_obj_t *l = label(b, text, &lv_font_montserrat_20, 0xFFFFFF);
        lv_obj_center(l);
    }
    return b;
}

static void wipe_textarea(lv_obj_t *ta) {
    const char *t = lv_textarea_get_text(ta);
    if (t) secure_zero((char *)t, strlen(t));
}

static lv_obj_t *new_screen(void) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    usb_label = NULL;
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
        button(scr, LV_SYMBOL_LEFT, COLOR_CARD, 48, 44, back_cb, NULL);
        lv_obj_set_pos(lv_obj_get_child(scr, -1), MARGIN, 14);
        x += 60;
    }
    lv_obj_t *t = label(scr, title, &lv_font_montserrat_24, 0xFFFFFF);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, HOR_RES - x - MARGIN - 56);
    lv_obj_set_pos(t, x, 22);
    return t;
}

static void usb_status(lv_obj_t *scr) {
    usb_label = label(scr, LV_SYMBOL_USB, &lv_font_montserrat_20, COLOR_MUTED);
    usb_shown = -1;
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

typedef void (*confirm_fn)(void);

static void confirm_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_current_target(e);
    confirm_fn fn = (confirm_fn)lv_event_get_user_data(e);
    bool yes = lv_msgbox_get_active_btn(m) == 1;
    lv_msgbox_close_async(m);
    mbox = NULL;
    if (yes) fn();
}

static void confirm(const char *title, const char *text, const char *yes, confirm_fn fn) {
    static const char *btns[3];
    btns[0] = "Cancel";
    btns[1] = yes;
    btns[2] = "";
    mbox = lv_msgbox_create(NULL, title, text, btns, false);
    lv_obj_set_width(mbox, HOR_RES - 2 * MARGIN);
    lv_obj_t *bm = lv_msgbox_get_btns(mbox);
    lv_obj_set_height(bm, 50);
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
        toast_show(LV_SYMBOL_USB "  Not connected to a computer", COLOR_DANGER, 2000);
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
    secure_zero(buf, sizeof(buf));
    if (ok) {
        typing = true;
        toast_show(LV_SYMBOL_KEYBOARD "  Typing...", COLOR_ACCENT, 0);
    } else {
        toast_show("Busy, try again", COLOR_DANGER, 1500);
    }
}

// ---------------------------------------------------------------------------
// Screens

static void show_pin_screen(int mode, const char *info);
static void show_list(void);
static void show_detail(int index);
static void show_edit(int index);
static void show_settings(void);

static int cur_index;   // entry on the detail/edit screen, -1 for a new one

// --- PIN ---

enum { PIN_UNLOCK, PIN_CREATE, PIN_CREATE_CONFIRM, PIN_CHANGE, PIN_CHANGE_CONFIRM };

static int pin_mode;
static bool pin_busy;
static char pin_first[VAULT_PIN_MAX + 1];
static lv_obj_t *pin_ta, *pin_info;

static const char *pin_pad_map[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};

// Runs from a one-shot timer, so "Unlocking..." is on screen during the slow key derivation.
static void pin_work_cb(lv_timer_t *t) {
    char pin[VAULT_PIN_MAX + 1];
    strncpy(pin, lv_textarea_get_text(pin_ta), sizeof(pin) - 1);
    pin[sizeof(pin) - 1] = 0;
    wipe_textarea(pin_ta);
    pin_busy = false;

    if (pin_mode == PIN_UNLOCK) {
        unlock_result_t r = vault_unlock(pin);
        if (r == UNLOCK_OK) {
            show_list();
        } else if (r == UNLOCK_WIPED) {
            show_pin_screen(PIN_CREATE, "Too many wrong PINs.\nAll passwords were erased.");
        } else {
            char msg[48];
            int left = vault_attempts_left();
            if (left >= 0) snprintf(msg, sizeof(msg), "Wrong PIN. Attempts left: %d", left);
            else snprintf(msg, sizeof(msg), "Wrong PIN");
            show_pin_screen(PIN_UNLOCK, msg);
        }
    } else if (pin_mode == PIN_CREATE_CONFIRM) {
        if (vault_create(pin)) show_list();
        else show_pin_screen(PIN_CREATE, "Flash write failed, try again");
    } else if (pin_mode == PIN_CHANGE_CONFIRM) {
        bool ok = vault_change_pin(pin);
        show_settings();
        toast_show(ok ? "PIN changed" : "Flash write failed", ok ? COLOR_OK : COLOR_DANGER, 2000);
    }
    secure_zero(pin, sizeof(pin));
    secure_zero(pin_first, sizeof(pin_first));
}

static void pin_submit(void) {
    const char *pin = lv_textarea_get_text(pin_ta);
    if (strlen(pin) < VAULT_PIN_MIN) {
        lv_label_set_text_fmt(pin_info, "PIN must be %d to %d digits", VAULT_PIN_MIN, VAULT_PIN_MAX);
        return;
    }
    switch (pin_mode) {
    case PIN_CREATE:
    case PIN_CHANGE:
        strcpy(pin_first, pin);
        wipe_textarea(pin_ta);
        show_pin_screen(pin_mode + 1, NULL);
        return;
    case PIN_CREATE_CONFIRM:
    case PIN_CHANGE_CONFIRM:
        if (strcmp(pin, pin_first) != 0) {
            secure_zero(pin_first, sizeof(pin_first));
            wipe_textarea(pin_ta);
            show_pin_screen(pin_mode - 1, "PINs did not match, try again");
            return;
        }
        lv_label_set_text(pin_info, "Saving...");
        break;
    default:
        lv_label_set_text(pin_info, "Unlocking...");
        break;
    }
    pin_busy = true;
    lv_timer_t *t = lv_timer_create(pin_work_cb, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void pin_pad_cb(lv_event_t *e) {
    if (pin_busy) return;
    lv_obj_t *pad = lv_event_get_target(e);
    const char *txt = lv_btnmatrix_get_btn_text(pad, lv_btnmatrix_get_selected_btn(pad));
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) lv_textarea_del_char(pin_ta);
    else if (strcmp(txt, LV_SYMBOL_OK) == 0) pin_submit();
    else lv_textarea_add_text(pin_ta, txt);
}

static void pin_back_cb(lv_event_t *e) {
    secure_zero(pin_first, sizeof(pin_first));
    wipe_textarea(pin_ta);
    show_settings();
}

static void show_pin_screen(int mode, const char *info) {
    static const char *titles[] = {"Enter PIN", "Create a PIN", "Repeat the PIN", "New PIN", "Repeat new PIN"};
    pin_mode = mode;
    pin_busy = false;
    lv_obj_t *scr = new_screen();
    bool changing = mode == PIN_CHANGE || mode == PIN_CHANGE_CONFIRM;

    lv_obj_t *t = header(scr, titles[mode], changing ? pin_back_cb : NULL);
    if (!changing) {
        lv_obj_set_width(t, HOR_RES - 2 * MARGIN);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }

    pin_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(pin_ta, true);
    lv_textarea_set_password_mode(pin_ta, true);
    lv_textarea_set_password_show_time(pin_ta, 0);
    lv_textarea_set_max_length(pin_ta, VAULT_PIN_MAX);
    lv_textarea_set_accepted_chars(pin_ta, "0123456789");
    lv_obj_set_style_text_font(pin_ta, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(pin_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(pin_ta, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_width(pin_ta, 0, 0);
    lv_obj_clear_flag(pin_ta, LV_OBJ_FLAG_CLICKABLE);   // the pad below is the only input
    lv_obj_set_size(pin_ta, HOR_RES - 2 * MARGIN - 40, 58);
    lv_obj_align(pin_ta, LV_ALIGN_TOP_MID, 0, 66);

    pin_info = label(scr, info ? info : "", &lv_font_montserrat_16, info ? COLOR_DANGER : COLOR_MUTED);
    lv_obj_set_width(pin_info, HOR_RES - 2 * MARGIN);
    lv_obj_set_style_text_align(pin_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(pin_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(pin_info, LV_ALIGN_TOP_MID, 0, 132);
    if (!info && mode == PIN_CREATE) lv_label_set_text_fmt(pin_info, "%d to %d digits", VAULT_PIN_MIN, VAULT_PIN_MAX);

    lv_obj_t *pad = lv_btnmatrix_create(scr);
    lv_btnmatrix_set_map(pad, pin_pad_map);
    lv_obj_set_size(pad, HOR_RES - 2 * MARGIN - 20, 250);
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

    show(scr);
}

// --- List ---

static void lock_now(void) {
    vault_lock();
    show_pin_screen(PIN_UNLOCK, NULL);
}

static void lock_cb(lv_event_t *e) {
    lock_now();
}

static void settings_cb(lv_event_t *e) {
    show_settings();
}

static void add_cb(lv_event_t *e) {
    if (vault_count() >= VAULT_MAX_ENTRIES) {
        toast_show("The list is full", COLOR_DANGER, 2000);
        return;
    }
    show_edit(-1);
}

static void row_cb(lv_event_t *e) {
    show_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void quick_type_cb(lv_event_t *e) {
    type_entry(vault_get((int)(intptr_t)lv_event_get_user_data(e)), TYPE_DEFAULT);
}

static void show_list(void) {
    lv_obj_t *scr = new_screen();
    lv_obj_t *t = header(scr, "Passwords", NULL);
    lv_obj_set_width(t, LV_SIZE_CONTENT);
    usb_status(scr);
    lv_obj_align_to(usb_label, t, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    button(scr, LV_SYMBOL_POWER, COLOR_CARD, 48, 44, lock_cb, NULL);
    lv_obj_align(lv_obj_get_child(scr, -1), LV_ALIGN_TOP_RIGHT, -MARGIN, 14);
    button(scr, LV_SYMBOL_SETTINGS, COLOR_CARD, 48, 44, settings_cb, NULL);
    lv_obj_align(lv_obj_get_child(scr, -1), LV_ALIGN_TOP_RIGHT, -MARGIN - 58, 14);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, HOR_RES - 2 * MARGIN + 8, VER_RES - 70 - 84);
    lv_obj_set_pos(list, MARGIN - 4, 70);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int n = vault_count();
    for (int i = 0; i < n; i++) {
        const vault_entry_t *en = vault_get(i);
        lv_obj_t *row = button(list, NULL, COLOR_CARD, lv_pct(100), 66, row_cb, (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);

        lv_obj_t *name = label(row, en->name, &lv_font_montserrat_20, 0xFFFFFF);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, HOR_RES - 2 * MARGIN - 100);
        lv_obj_align(name, en->login[0] ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, en->login[0] ? -2 : 0);
        if (en->login[0]) {
            lv_obj_t *login = label(row, en->login, &lv_font_montserrat_14, COLOR_MUTED);
            lv_label_set_long_mode(login, LV_LABEL_LONG_DOT);
            lv_obj_set_width(login, HOR_RES - 2 * MARGIN - 100);
            lv_obj_align(login, LV_ALIGN_BOTTOM_LEFT, 0, 2);
        }
        lv_obj_t *kb = button(row, LV_SYMBOL_KEYBOARD, COLOR_ACCENT, 60, 46, quick_type_cb, (void *)(intptr_t)i);
        lv_obj_align(kb, LV_ALIGN_RIGHT_MID, 6, 0);
    }
    if (n == 0) {
        lv_obj_t *empty = label(list, "No passwords yet.\nTap \"Add\" to create one.", &lv_font_montserrat_16, COLOR_MUTED);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 100, 0);
    }

    button(scr, LV_SYMBOL_PLUS "  Add", COLOR_ACCENT, HOR_RES - 2 * MARGIN - 40, 54, add_cb, NULL);
    lv_obj_align(lv_obj_get_child(scr, -1), LV_ALIGN_BOTTOM_MID, 0, -20);

    show(scr);
}

// --- Detail ---

static lv_obj_t *det_pass;
static bool det_revealed;

static void back_to_list_cb(lv_event_t *e) {
    show_list();
}

static void det_type_cb(lv_event_t *e) {
    type_entry(vault_get(cur_index), (type_what_t)(intptr_t)lv_event_get_user_data(e));
}

static void det_reveal_cb(lv_event_t *e) {
    const vault_entry_t *en = vault_get(cur_index);
    det_revealed = !det_revealed;
    lv_label_set_text(det_pass, det_revealed ? en->pass : "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), det_revealed ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void det_edit_cb(lv_event_t *e) {
    show_edit(cur_index);
}

static void do_delete(void) {
    if (!vault_delete(cur_index)) toast_show("Flash write failed", COLOR_DANGER, 2000);
    show_list();
}

static void det_delete_cb(lv_event_t *e) {
    confirm("Delete?", vault_get(cur_index)->name, "Delete", do_delete);
}

static void show_detail(int index) {
    const vault_entry_t *en = vault_get(index);
    if (!en) {
        show_list();
        return;
    }
    cur_index = index;
    det_revealed = false;
    lv_obj_t *scr = new_screen();
    header(scr, en->name, back_to_list_cb);
    usb_status(scr);
    lv_obj_align(usb_label, LV_ALIGN_TOP_RIGHT, -MARGIN, 24);

    const int w = HOR_RES - 2 * MARGIN;
    lv_obj_t *l = label(scr, "LOGIN", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 72);
    l = label(scr, en->login[0] ? en->login : "-", &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, MARGIN, 92);

    l = label(scr, "PASSWORD", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 128);
    det_pass = label(scr, "", &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_long_mode(det_pass, LV_LABEL_LONG_DOT);
    lv_obj_set_width(det_pass, w - 64);
    lv_obj_set_pos(det_pass, MARGIN, 150);
    lv_obj_t *eye = button(scr, LV_SYMBOL_EYE_OPEN, COLOR_CARD, 54, 44, det_reveal_cb, NULL);
    lv_obj_set_pos(eye, HOR_RES - MARGIN - 54, 138);
    det_revealed = true;
    lv_event_send(eye, LV_EVENT_CLICKED, NULL);   // start masked

    // What the big button types, e.g. "login  TAB  password  ENTER".
    char seq[48];
    snprintf(seq, sizeof(seq), "%s%s%s", (en->flags & VAULT_FLAG_LOGIN) && en->login[0] ? "login  " LV_SYMBOL_RIGHT "  " : "",
             "password", en->flags & VAULT_FLAG_ENTER ? "  " LV_SYMBOL_NEW_LINE : "");
    lv_obj_t *big = button(scr, NULL, COLOR_ACCENT, w, 84, det_type_cb, (void *)(intptr_t)TYPE_DEFAULT);
    lv_obj_set_pos(big, MARGIN, 202);
    l = label(big, LV_SYMBOL_KEYBOARD "  Type", &lv_font_montserrat_26, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -12);
    l = label(big, seq, &lv_font_montserrat_14, 0xD0E4FF);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 20);

    const int hw = (w - 10) / 2;
    lv_obj_t *b = button(scr, "Login", COLOR_CARD, hw, 50, det_type_cb, (void *)(intptr_t)TYPE_LOGIN);
    lv_obj_set_pos(b, MARGIN, 298);
    if (!en->login[0]) lv_obj_add_state(b, LV_STATE_DISABLED);
    b = button(scr, "Password", COLOR_CARD, hw, 50, det_type_cb, (void *)(intptr_t)TYPE_PASS);
    lv_obj_set_pos(b, MARGIN + hw + 10, 298);

    b = button(scr, LV_SYMBOL_EDIT "  Edit", COLOR_CARD, hw, 50, det_edit_cb, NULL);
    lv_obj_set_pos(b, MARGIN, 362);
    b = button(scr, LV_SYMBOL_TRASH "  Delete", 0x3A1414, hw, 50, det_delete_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(COLOR_DANGER), 0);
    lv_obj_set_pos(b, MARGIN + hw + 10, 362);

    show(scr);
}

// --- Edit ---

#define KB_HEIGHT 230

static lv_obj_t *ed_name, *ed_login, *ed_pass, *ed_sw_login, *ed_sw_enter, *ed_form, *ed_kb;

// Every printable ASCII character, so any password can be entered.
static const char *kb_spec_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "-", "_", "=", "+", "[", "]", "{", "}", "\\", "|", ";", "\n",
    ":", "'", "\"", ",", ".", "<", ">", "/", "?", "`", "~", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};

static const lv_btnmatrix_ctrl_t kb_spec_ctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2};

static void ed_kb_show(lv_obj_t *ta) {
    lv_keyboard_set_textarea(ed_kb, ta);
    lv_obj_clear_flag(ed_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(ed_form, VER_RES - 64 - KB_HEIGHT);
    lv_obj_update_layout(ed_form);
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void ed_kb_hide(void) {
    lv_obj_t *ta = lv_keyboard_get_textarea(ed_kb);
    if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(ed_kb, NULL);
    lv_obj_add_flag(ed_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(ed_form, VER_RES - 64);
}

static void ed_ta_cb(lv_event_t *e) {
    ed_kb_show(lv_event_get_target(e));
}

static void ed_kb_cb(lv_event_t *e) {
    ed_kb_hide();
}

static void ed_eye_cb(lv_event_t *e) {
    bool hidden = lv_textarea_get_password_mode(ed_pass);
    lv_textarea_set_password_mode(ed_pass, !hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void ed_gen_cb(lv_event_t *e) {
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*-_=+?";
    const uint32_t n = sizeof(charset) - 1;
    char pw[GEN_LENGTH + 1];
    for (int i = 0; i < GEN_LENGTH; i++) {
        uint32_t r;
        do r = get_rand_32() & 0x7F;   // rejection sampling keeps every character equally likely
        while (r >= n);
        pw[i] = charset[r];
    }
    pw[GEN_LENGTH] = 0;
    wipe_textarea(ed_pass);
    lv_textarea_set_text(ed_pass, pw);
    secure_zero(pw, sizeof(pw));
    if (lv_textarea_get_password_mode(ed_pass)) {
        lv_obj_t *eye = lv_obj_get_child(lv_obj_get_parent(lv_event_get_target(e)), 1);
        lv_event_send(eye, LV_EVENT_CLICKED, NULL);
    }
}

static void ed_leave(void) {
    wipe_textarea(ed_login);
    wipe_textarea(ed_pass);
}

static void ed_cancel_cb(lv_event_t *e) {
    ed_leave();
    if (cur_index >= 0) show_detail(cur_index);
    else show_list();
}

static void ed_save_cb(lv_event_t *e) {
    const char *name = lv_textarea_get_text(ed_name);
    while (*name == ' ') name++;
    if (!*name) {
        toast_show("Enter a name", COLOR_DANGER, 1500);
        return;
    }
    vault_entry_t en;
    memset(&en, 0, sizeof(en));
    strncpy(en.name, name, sizeof(en.name) - 1);
    strncpy(en.login, lv_textarea_get_text(ed_login), sizeof(en.login) - 1);
    strncpy(en.pass, lv_textarea_get_text(ed_pass), sizeof(en.pass) - 1);
    if (lv_obj_has_state(ed_sw_login, LV_STATE_CHECKED)) en.flags |= VAULT_FLAG_LOGIN;
    if (lv_obj_has_state(ed_sw_enter, LV_STATE_CHECKED)) en.flags |= VAULT_FLAG_ENTER;

    int idx = vault_put(cur_index, &en);
    secure_zero(&en, sizeof(en));
    if (idx < 0) {
        toast_show("Could not save", COLOR_DANGER, 2000);
        return;
    }
    ed_leave();
    show_detail(idx);
    toast_show("Saved", COLOR_OK, 1200);
}

static lv_obj_t *ed_field(const char *caption, const char *value, int max_len, lv_obj_t *parent) {
    if (caption) label(ed_form, caption, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, value);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(COLOR_CARD), 0);
    lv_obj_add_event_cb(ta, ed_ta_cb, LV_EVENT_FOCUSED, NULL);
    return ta;
}

static lv_obj_t *ed_switch(const char *text, bool on) {
    lv_obj_t *row = lv_obj_create(ed_form);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = label(row, text, &lv_font_montserrat_16, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static void show_edit(int index) {
    static const vault_entry_t blank;
    const vault_entry_t *en = index >= 0 ? vault_get(index) : &blank;
    if (!en) {
        show_list();
        return;
    }
    cur_index = index;
    lv_obj_t *scr = new_screen();

    button(scr, LV_SYMBOL_CLOSE, COLOR_CARD, 48, 44, ed_cancel_cb, NULL);
    lv_obj_set_pos(lv_obj_get_child(scr, -1), MARGIN, 12);
    lv_obj_t *t = label(scr, index >= 0 ? "Edit" : "New", &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    button(scr, LV_SYMBOL_OK, COLOR_ACCENT, 60, 44, ed_save_cb, NULL);
    lv_obj_align(lv_obj_get_child(scr, -1), LV_ALIGN_TOP_RIGHT, -MARGIN, 12);

    ed_form = lv_obj_create(scr);
    lv_obj_set_size(ed_form, HOR_RES, VER_RES - 64);
    lv_obj_set_pos(ed_form, 0, 64);
    lv_obj_set_style_bg_opa(ed_form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ed_form, 0, 0);
    lv_obj_set_style_radius(ed_form, 0, 0);
    lv_obj_set_style_pad_hor(ed_form, MARGIN, 0);
    lv_obj_set_style_pad_top(ed_form, 4, 0);
    lv_obj_set_style_pad_bottom(ed_form, 40, 0);
    lv_obj_set_style_pad_row(ed_form, 6, 0);
    lv_obj_set_flex_flow(ed_form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ed_form, LV_DIR_VER);

    ed_name = ed_field("NAME", en->name, VAULT_NAME_LEN - 1, ed_form);
    ed_login = ed_field("LOGIN", en->login, VAULT_LOGIN_LEN - 1, ed_form);

    // Password field with show/hide and generate buttons beside it.
    label(ed_form, "PASSWORD", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_t *row = lv_obj_create(ed_form);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ed_pass = ed_field(NULL, en->pass, VAULT_PASS_LEN - 1, row);
    lv_obj_set_width(ed_pass, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ed_pass, 1);
    lv_textarea_set_password_mode(ed_pass, true);
    lv_textarea_set_password_show_time(ed_pass, 0);
    button(row, LV_SYMBOL_EYE_OPEN, COLOR_CARD, 50, 46, ed_eye_cb, NULL);
    button(row, LV_SYMBOL_SHUFFLE, COLOR_CARD, 50, 46, ed_gen_cb, NULL);

    ed_sw_login = ed_switch("Type login + Tab first", en->flags & VAULT_FLAG_LOGIN);
    ed_sw_enter = ed_switch("Press Enter at the end", en->flags & VAULT_FLAG_ENTER);
    lv_obj_t *hint = label(ed_form, LV_SYMBOL_SHUFFLE " generates a random password", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_style_pad_top(hint, 6, 0);

    ed_kb = lv_keyboard_create(scr);
    lv_keyboard_set_map(ed_kb, LV_KEYBOARD_MODE_SPECIAL, kb_spec_map, kb_spec_ctrl);
    lv_obj_set_size(ed_kb, HOR_RES, KB_HEIGHT);
    lv_obj_align(ed_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_bottom(ed_kb, 14, 0);   // bottom corners are rounded
    lv_obj_add_event_cb(ed_kb, ed_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ed_kb, ed_kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(ed_kb, LV_OBJ_FLAG_HIDDEN);

    show(scr);
    if (index < 0) {
        lv_obj_add_state(ed_name, LV_STATE_FOCUSED);
        ed_kb_show(ed_name);
    }
}

// --- Settings ---

static void brightness_cb(lv_event_t *e) {
    brightness = lv_slider_get_value(lv_event_get_target(e));
    AMOLED_1IN8_SetBrightness(brightness);
}

static void change_pin_cb(lv_event_t *e) {
    show_pin_screen(PIN_CHANGE, NULL);
}

static void do_wipe(void) {
    vault_wipe();
    show_pin_screen(PIN_CREATE, "All passwords erased");
}

static void wipe_cb(lv_event_t *e) {
    confirm("Erase everything?", "All passwords and the PIN will be deleted.", "Erase", do_wipe);
}

static void show_settings(void) {
    lv_obj_t *scr = new_screen();
    header(scr, "Settings", back_to_list_cb);
    const int w = HOR_RES - 2 * MARGIN;

    lv_obj_t *l = label(scr, "BRIGHTNESS", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(l, MARGIN, 80);
    lv_obj_t *s = lv_slider_create(scr);
    lv_obj_set_size(s, w - 24, 10);
    lv_obj_set_pos(s, MARGIN + 12, 110);
    lv_slider_set_range(s, 10, 100);
    lv_slider_set_value(s, brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *b = button(scr, "Change PIN", COLOR_CARD, w, 54, change_pin_cb, NULL);
    lv_obj_set_pos(b, MARGIN, 150);
    b = button(scr, "Erase all passwords", 0x3A1414, w, 54, wipe_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(COLOR_DANGER), 0);
    lv_obj_set_pos(b, MARGIN, 216);

    l = label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    lv_label_set_text_fmt(l,
                          "Entries: %d of %d\n\n"
                          "Switch the computer to the English (US)\n"
                          "keyboard layout before typing.\n"
                          "Auto-lock after %d min of inactivity.",
                          vault_count(), VAULT_MAX_ENTRIES, LOCK_AFTER_MS / 60000);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, MARGIN, 292);

    show(scr);
}

// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
    if (typing && !usb_kbd_busy()) {
        typing = false;
        toast_show(LV_SYMBOL_OK "  Done", COLOR_OK, 1200);
    }

    if (usb_label) {
        int m = usb_kbd_mounted();
        if (m != usb_shown) {
            usb_shown = m;
            lv_obj_set_style_text_color(usb_label, lv_color_hex(m ? COLOR_OK : COLOR_MUTED), 0);
        }
    }

    uint32_t idle = lv_disp_get_inactive_time(NULL);
    if (vault_is_unlocked() && idle > LOCK_AFTER_MS && !typing) lock_now();

    // AMOLED: dim when idle to avoid burn-in.
    if (!dimmed && idle > DIM_AFTER_MS) {
        dimmed = true;
        AMOLED_1IN8_SetBrightness(DIM_BRIGHTNESS);
    } else if (dimmed && idle < DIM_AFTER_MS) {
        dimmed = false;
        AMOLED_1IN8_SetBrightness(brightness);
    }
}

void ui_main(void) {
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

    // Registered from core 1, so the display DMA interrupt is serviced here.
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
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    if (vault_exists()) show_pin_screen(PIN_UNLOCK, NULL);
    else show_pin_screen(PIN_CREATE, NULL);
    lv_timer_create(tick_cb, 100, NULL);

    while (true) {
        lv_timer_handler();
        sleep_ms(5);
    }
}
