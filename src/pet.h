#pragma once

#include <stdbool.h>

#include "lvgl.h"

// The home screen's pixel cat. It picks its own activities (sitting, walking, grooming, playing
// with yarn, eating, meowing, napping) and is drawn straight from the sprites in pet_sprites.h,
// a few frames a second, repainting only what moved.

// A full-screen object with the cat on it; its own timer goes away with it.
lv_obj_t *pet_create(lv_obj_t *parent);

// The cat reacts to being touched: hearts and a happy face.
void pet_stroke(void);

// Screen dimmed: the cat falls asleep, and wakes up again when it lights up.
void pet_set_sleepy(bool sleepy);
