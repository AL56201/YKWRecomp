// What the controls hold right now: written by the window thread (runtime/ui.c), read by the game (hle/input.c).
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// nn::hid::NpadButton bit numbers
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_STICKL, BTN_STICKR, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };

typedef struct {
    _Atomic uint64_t buttons;
    _Atomic int32_t lx, ly, rx, ry;
    _Atomic int32_t touch_x, touch_y;  // 0..1279, 0..719
    _Atomic bool touch_down;
} InputState;

extern InputState g_input;
