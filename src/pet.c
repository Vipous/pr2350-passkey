#include "pet.h"

#include <stdlib.h>

#include "pet_sprites.h"
#include "pico/time.h"

#define TICK_MS 150
#define CAT_SCALE 10   // screen pixels per sprite pixel
#define BOMB_SCALE 8

// The scene: the cat, one prop (yarn or bowl) and a few floating particles (hearts, Zs).
typedef struct {
    int sprite;   // -1 when not shown
    int x, y;     // top-left, screen pixels
    int scale;
} item_t;

enum { IT_CAT, IT_PROP, IT_P0, IT_COUNT = IT_P0 + 4 };

typedef enum { A_IDLE, A_WALK, A_GROOM, A_PLAY, A_EAT, A_MEOW, A_SLEEP, A_HAPPY, A_PANIC, A_POOF } act_t;

static lv_obj_t *obj;
static lv_timer_t *timer;
static item_t items[IT_COUNT], drawn[IT_COUNT];
static int part_life[IT_COUNT];
static lv_coord_t scr_w, scr_h, floor_y;

static pet_character_t chr;
static act_t act;
static int act_left;     // ticks left in the activity; -1 = until told otherwise
static uint32_t tick;
static int cat_x, target_x;
static int yarn_x, yarn_v, yarn_lo, yarn_hi;   // the yarn rolls between lo and hi
static bool sleepy;

static uint32_t rnd_state;
static int rnd(int n) {   // 0..n-1
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return (int)(rnd_state % (uint32_t)n);
}

static int sprite_w(int s, int scale) { return pet_sprites[s].w * scale; }
static int sprite_h(int s, int scale) { return pet_sprites[s].h * scale; }

static void set_item(int i, int sprite, int x, int y, int scale) {
    items[i].sprite = sprite;
    items[i].x = x;
    items[i].y = y;
    items[i].scale = scale;
}

static void invalidate(const item_t *it) {
    if (it->sprite < 0) return;
    lv_area_t a = {it->x, it->y, it->x + sprite_w(it->sprite, it->scale) - 1, it->y + sprite_h(it->sprite, it->scale) - 1};
    lv_obj_invalidate_area(obj, &a);
}

// Repaints whatever changed since the last frame: where it was and where it is now.
static void commit(void) {
    for (int i = 0; i < IT_COUNT; i++) {
        item_t *a = &drawn[i], *b = &items[i];
        if (a->sprite == b->sprite && a->x == b->x && a->y == b->y && a->scale == b->scale) continue;
        invalidate(a);
        invalidate(b);
        *a = *b;
    }
}

static void draw_item(const item_t *it, const lv_area_t *clip) {
    const pet_sprite_t *sp = &pet_sprites[it->sprite];
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    for (int y = 0; y < sp->h; y++) {
        const char *row = sp->rows[y];
        lv_coord_t y1 = it->y + y * it->scale;
        if (y1 > clip->y2 || y1 + it->scale - 1 < clip->y1) continue;
        // Runs of one colour become one rectangle.
        for (int x = 0; x < sp->w;) {
            char c = row[x];
            int run = 1;
            while (x + run < sp->w && row[x + run] == c) run++;
            if (c != '.') {
                lv_area_t a = {it->x + x * it->scale, y1, it->x + (x + run) * it->scale - 1, y1 + it->scale - 1};
                dsc.bg_color = lv_color_hex(pet_color(c));
                lv_draw_rect(&a, clip, &dsc);
            }
            x += run;
        }
    }
}

static void draw_cb(lv_event_t *e) {
    const lv_area_t *clip = lv_event_get_param(e);
    // Cat, then the prop in front of it (the bowl), then particles on top.
    for (int i = 0; i < IT_COUNT; i++)
        if (drawn[i].sprite >= 0) draw_item(&drawn[i], clip);
}

// ---------------------------------------------------------------------------
// Behaviour

static int char_scale(void) { return chr == PET_CHAR_BOMB ? BOMB_SCALE : CAT_SCALE; }
static int cat_w(void) { return sprite_w(chr == PET_CHAR_BOMB ? PET_BOMB_STAND : PET_SIT_A, char_scale()); }
static int cat_y(int sprite) { return floor_y - sprite_h(sprite, char_scale()); }

static void spawn(int sprite, int x, int y, int scale, int life) {
    for (int i = IT_P0; i < IT_COUNT; i++) {
        if (items[i].sprite >= 0) continue;
        set_item(i, sprite, x, y, scale);
        part_life[i] = life;
        return;
    }
}

static void start(act_t a, int ticks) {
    act = a;
    act_left = ticks;
    items[IT_PROP].sprite = -1;
    if (a == A_WALK) {
        int max_x = scr_w - cat_w();
        do target_x = rnd(max_x + 1);
        while (abs(target_x - cat_x) < 40);
    }
    if (a == A_PLAY) {
        // On whichever side has more room, from the cat's paw to the edge of the screen.
        int yw = sprite_w(PET_YARN, 6);
        bool right = cat_x < scr_w - cat_x - cat_w();
        yarn_lo = right ? cat_x + cat_w() - 40 : 0;
        yarn_hi = right ? scr_w - yw : cat_x + 20;
        yarn_x = right ? yarn_lo : yarn_hi;
        yarn_v = right ? 6 : -6;
    }
}

static void pick_next(void) {
    int r = rnd(100);
    if (chr == PET_CHAR_BOMB) {   // the bomb mostly strolls about
        if (r < 55) start(A_WALK, -1);
        else start(A_IDLE, 15 + rnd(40));
        return;
    }
    if (r < 30) start(A_WALK, -1);
    else if (r < 45) start(A_GROOM, 18 + rnd(12));
    else if (r < 60) start(A_PLAY, 30 + rnd(20));
    else if (r < 70) start(A_EAT, 24 + rnd(12));
    else if (r < 80) start(A_MEOW, 8);
    else if (r < 88) start(A_SLEEP, 70 + rnd(60));   // a nap, 10-20 s
    else start(A_IDLE, 20 + rnd(40));
}

// The bomb: walks around with its fuse fizzing; touched, it panics, then goes off with a puff.
static void step_bomb(void) {
    int frame = PET_BOMB_STAND, x_off = 0, y_off = 0;
    bool spark = true, wild = false;
    switch (act) {
    case A_WALK: {
        int d = target_x - cat_x;
        cat_x += d > 0 ? (d < 6 ? d : 6) : (d > -6 ? d : -6);
        frame = (tick / 2) % 2 ? PET_BOMB_WALK_B : PET_BOMB_WALK_A;
        y_off = (tick % 2) ? -6 : 0;
        if (cat_x == target_x) start(A_IDLE, 15 + rnd(30));
        break;
    }
    case A_PANIC:
        frame = tick % 2 ? PET_BOMB_PANIC_B : PET_BOMB_PANIC;
        x_off = tick % 2 ? 6 : -6;   // shaking
        wild = true;
        break;
    case A_POOF:
        frame = PET_BOMB_SOOT;
        spark = false;
        break;
    case A_SLEEP:
        frame = PET_BOMB_BLINK;
        spark = false;
        if (tick % 9 == 0) spawn(PET_ZED, cat_x + cat_w() - 60 + rnd(20), cat_y(frame) + 40, 4, 14);
        break;
    default:
        if (tick % 29 == 0 || tick % 41 == 0) frame = PET_BOMB_BLINK;
        break;
    }
    int x = cat_x + x_off, y = cat_y(frame) + y_off;
    set_item(IT_CAT, frame, x, y, BOMB_SCALE);
    if (spark) {
        // On the wick's tip (sprite pixel 19, 1); bigger and wilder in a panic.
        int sc = wild ? 8 : 5, half = sprite_w(PET_SPARK_A, sc) / 2;
        int sp = wild ? (tick % 2 ? PET_SPARK_A : PET_SPARK_B) : ((tick / 2) % 2 ? PET_SPARK_A : PET_SPARK_B);
        set_item(IT_PROP, sp, x + 19 * BOMB_SCALE + BOMB_SCALE / 2 - half, y + BOMB_SCALE + BOMB_SCALE / 2 - half, sc);
    } else {
        items[IT_PROP].sprite = -1;
    }
}

static void step(void) {
    tick++;
    if (chr == PET_CHAR_BOMB) {
        step_bomb();
        goto common;
    }
    int frame = PET_SIT_A, y_off = 0;

    switch (act) {
    case A_IDLE:
        // Tail flicks now and then, blinks every few seconds.
        frame = (tick / 4) % 6 == 5 ? PET_SIT_B : PET_SIT_A;
        if (tick % 23 == 0 || tick % 37 == 0) frame = PET_SIT_BLINK;
        break;
    case A_WALK: {
        int d = target_x - cat_x;
        int s = d > 0 ? (d < 8 ? d : 8) : (d > -8 ? d : -8);
        cat_x += s;
        frame = (tick / 2) % 2 ? PET_SIT_B : PET_SIT_A;
        y_off = (tick % 2) ? -8 : 0;   // a little hop each step
        if (cat_x == target_x) start(A_IDLE, 15 + rnd(30));
        break;
    }
    case A_GROOM:
        frame = (tick / 3) % 2 ? PET_GROOM : PET_PAW;
        break;
    case A_PLAY: {
        frame = (tick / 2) % 2 ? PET_PAW : PET_SIT_A;
        // The yarn rolls away and back along the floor, bouncing a little.
        yarn_x += yarn_v;
        if (yarn_x <= yarn_lo) { yarn_x = yarn_lo; yarn_v = 6; }
        if (yarn_x >= yarn_hi) { yarn_x = yarn_hi; yarn_v = -6; }
        int bounce = (tick % 4 == 1 || tick % 4 == 2) ? 10 : 0;
        set_item(IT_PROP, PET_YARN, yarn_x, floor_y - sprite_h(PET_YARN, 6) - bounce, 6);
        break;
    }
    case A_EAT: {
        frame = (tick / 3) % 2 ? PET_EAT : PET_SIT_BLINK;
        int bw = sprite_w(PET_BOWL, 8);
        set_item(IT_PROP, PET_BOWL, cat_x + (cat_w() - bw) / 2 - 10, floor_y - sprite_h(PET_BOWL, 8) + 10, 8);
        break;
    }
    case A_MEOW:
        frame = PET_MEOW;
        break;
    case A_SLEEP:
        frame = PET_SLEEP;
        if (tick % 9 == 0) spawn(PET_ZED, cat_x + cat_w() - 70 + rnd(20), cat_y(PET_SLEEP) + 10, 4, 14);
        break;
    case A_HAPPY:
        frame = PET_SIT_HAPPY;
        if (tick % 3 == 0) spawn(PET_HEART, cat_x + 40 + rnd(cat_w() - 100), cat_y(PET_SIT_A) + 20, 4, 10);
        break;
    default:   // the bomb's own states
        break;
    }

    set_item(IT_CAT, frame, cat_x, cat_y(frame) + y_off, CAT_SCALE);

common:
    // Particles drift up and fade out.
    for (int i = IT_P0; i < IT_COUNT; i++) {
        if (items[i].sprite < 0) continue;
        int sp = items[i].sprite;
        items[i].y -= sp == PET_ZED ? 5 : sp == PET_SMOKE ? 4 : 8;
        items[i].x += sp == PET_ZED ? 3 : (rnd(3) - 1) * 3;
        if (--part_life[i] <= 0 || items[i].y < 0) items[i].sprite = -1;
    }

    if (act_left > 0 && --act_left == 0) {
        if (act == A_PANIC) {
            // Boom (a small one): smoke from the wick, a sooty face for a moment.
            start(A_POOF, 14);
            for (int k = 0; k < 3; k++)
                spawn(PET_SMOKE, cat_x + 19 * BOMB_SCALE - 40 + rnd(60), cat_y(PET_BOMB_SOOT) - 10 + rnd(30), 8, 12);
        } else if (act == A_SLEEP || act == A_HAPPY || act == A_EAT || act == A_POOF) {
            start(A_IDLE, 20 + rnd(30));
        } else {
            pick_next();
        }
    }
    if (act == A_IDLE && act_left < 0) start(A_IDLE, 20 + rnd(30));
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    step();
    commit();
}

static void delete_cb(lv_event_t *e) {
    (void)e;
    if (timer) lv_timer_del(timer);
    timer = NULL;
    obj = NULL;
}

lv_obj_t *pet_create(lv_obj_t *parent, pet_character_t character) {
    chr = character;
    obj = lv_obj_create(parent);
    scr_w = lv_obj_get_width(parent);
    scr_h = lv_obj_get_height(parent);
    lv_obj_set_size(obj, scr_w, scr_h);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, NULL);

    floor_y = scr_h - 60;
    if (!rnd_state) rnd_state = time_us_32() | 1;
    for (int i = 0; i < IT_COUNT; i++) items[i].sprite = drawn[i].sprite = -1;
    if (cat_x <= 0 || cat_x > scr_w - cat_w()) cat_x = (scr_w - cat_w()) / 2;
    if (sleepy) start(A_SLEEP, -1);
    else if (chr == PET_CHAR_BOMB) start(A_WALK, -1);
    else start(A_MEOW, 6);   // says hello
    step();
    commit();
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
    return obj;
}

void pet_stroke(void) {
    if (!obj) return;
    if (chr == PET_CHAR_BOMB) {
        if (act != A_PANIC && act != A_POOF) start(A_PANIC, 18);   // about to blow!
        return;
    }
    for (int i = IT_P0; i < IT_COUNT; i++) items[i].sprite = -1;
    if (act == A_SLEEP) start(A_MEOW, 8);   // woken up
    else start(A_HAPPY, 14);
}

void pet_set_sleepy(bool s) {
    if (s == sleepy) return;
    sleepy = s;
    if (!obj) return;
    if (s) start(A_SLEEP, -1);
    else if (act == A_SLEEP) start(chr == PET_CHAR_BOMB ? A_IDLE : A_MEOW, 8);
}
