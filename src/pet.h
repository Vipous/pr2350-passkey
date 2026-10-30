#pragma once

#include <stdbool.h>

#include "lvgl.h"

// The home screen's pixel character, drawn straight from the sprites in pet_sprites.h a few frames
// a second, repainting only what moved.
//   cat:  picks its own activities (sitting, walking, grooming, yarn, eating, meowing, napping);
//         a touch gets hearts and a happy face.
//   bomb: strolls about with its fuse fizzing; a touch makes it panic, then go off in a puff.

typedef enum { PET_CHAR_CAT, PET_CHAR_BOMB } pet_character_t;

// A full-screen object with the character on it; its own timer goes away with it.
lv_obj_t *pet_create(lv_obj_t *parent, pet_character_t character);

// The character was touched.
void pet_stroke(void);

// Screen dimmed: the character falls asleep, and wakes up again when it lights up.
void pet_set_sleepy(bool sleepy);
