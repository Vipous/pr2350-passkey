#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "DEV_Config.h"   // must precede the Waveshare headers (UBYTE/UWORD)
#include "AMOLED_1in8.h"
#include "FT3168.h"
#include "editor.h"
#include "lvgl.h"
#include "usb_kbd.h"
#include "vault.h"

#define HOR_RES AMOLED_1IN8_WIDTH
#define VER_RES AMOLED_1IN8_HEIGHT
#define BUF_LINES 40
#define MARGIN 24   // keeps content clear of the panel's rounded corners

#define DIM_AFTER_MS   30000
#define DIM_BRIGHTNESS 5

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

// Which screen is up, so it can be rebuilt when the editor changes the list.
static enum { SCR_LIST, SCR_DETAIL, SCR_SETTINGS } screen;

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
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    if (text) {
        lv_obj_t *l = label(b, text, &lv_font_montserrat_24, 0xFFFFFF);
        lv_obj_center(l);
    }
    return b;
}

static lv_obj_t *new_screen(void) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    usb_label = editor_label = NULL;
    return s;
}

static void show(lv_obj_t *s) {
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

// USB and editor connection icons, top right.
static void status_icons(lv_obj_t *scr, lv_coord_t right) {
    usb_label = label(scr, LV_SYMBOL_USB, &lv_font_montserrat_24, COLOR_MUTED);
    lv_obj_align(usb_label, LV_ALIGN_TOP_RIGHT, -right, 26);
    editor_label = label(scr, LV_SYMBOL_EDIT, &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_align(editor_label, LV_ALIGN_TOP_RIGHT, -right - 40, 28);
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

static int cur_index;   // entry on the detail screen
static lv_coord_t list_scroll;

// --- List ---

static void settings_cb(lv_event_t *e) {
    show_settings();
}

static void row_cb(lv_event_t *e) {
    show_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void quick_type_cb(lv_event_t *e) {
    type_entry(vault_get((int)(intptr_t)lv_event_get_user_data(e)), TYPE_DEFAULT);
}

static void list_scroll_cb(lv_event_t *e) {
    list_scroll = lv_obj_get_scroll_y(lv_event_get_target(e));
}

static void show_list(void) {
    screen = SCR_LIST;
    lv_obj_t *scr = new_screen();
    header(scr, "Passwords", NULL);
    status_icons(scr, MARGIN + 74);
    lv_obj_t *b = button(scr, LV_SYMBOL_SETTINGS, COLOR_CARD, 60, 52, settings_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -MARGIN, 12);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, HOR_RES - 2 * MARGIN + 8, VER_RES - 76 - 16);
    lv_obj_set_pos(list, MARGIN - 4, 76);
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
                                "in Chrome or Edge to add them.",
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

static void show_settings(void) {
    screen = SCR_SETTINGS;
    lv_obj_t *scr = new_screen();
    header(scr, "Settings", back_to_list_cb);
    status_icons(scr, MARGIN);
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

    l = label(scr, "", &lv_font_montserrat_16, COLOR_MUTED);
    lv_label_set_text_fmt(l,
                          "Entries: %d of %d\n\n"
                          "To add or change passwords, open\n"
                          "passkey.html in Chrome or Edge\n"
                          "on the computer.\n\n"
                          "Switch the computer to the\n"
                          "English (US) keyboard layout\n"
                          "before typing.",
                          vault_count(), VAULT_MAX_ENTRIES);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, MARGIN, 180);

    show(scr);
}

// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
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

    // AMOLED: dim when idle to avoid burn-in.
    uint32_t idle = lv_disp_get_inactive_time(NULL);
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
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    vault_init();
    show_list();
    lv_timer_create(tick_cb, 100, NULL);

    // Rebuilt shortly after the editor's last change, not once per entry during an import.
    absolute_time_t refresh_at = nil_time;
    while (true) {
        // Editor requests are handled here rather than in a timer so listing stays quick.
        if (editor_service()) refresh_at = make_timeout_time_ms(300);
        if (!is_nil_time(refresh_at) && time_reached(refresh_at)) {
            refresh_at = nil_time;
            list_scroll = 0;
            if (screen != SCR_SETTINGS) show_list();
        }
        lv_timer_handler();
        sleep_ms(2);
    }
}
