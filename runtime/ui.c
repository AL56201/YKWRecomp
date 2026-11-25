// The window and the controls: SDL on the main thread. The game draws into the window from its own threads through
// EGL (runtime/gl/egl.c); this thread only pumps events and keeps the input state the game polls (runtime/hle/input.c).
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>

#include "hle.h"
#include "input_state.h"
#include "settings.h"

extern SDL_Window *g_window;
void egl_host_init(void);
void ui_note_refresh(void);
void ui_set_fullscreen(bool on);

InputState g_input;

void ui_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
        FATAL("SDL_Init: %s\n", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (getenv("YKW_GL_DEBUG")) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    int s = g_settings.window_scale;
    g_window = SDL_CreateWindow("YKW Recomp", 1280 * s, 720 * s, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    if (g_settings.fullscreen) ui_set_fullscreen(true);
    else ui_note_refresh();
    egl_host_init();
}

static double mode_hz(const SDL_DisplayMode *m) {
    if (m && m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0)
        return (double)m->refresh_rate_numerator / m->refresh_rate_denominator;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 60.0;
}

// The refresh rate the window is shown at now (g_display_hz: hle/framerate.c, hle/vi.c)
void ui_note_refresh(void) {
    extern double g_display_hz;
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode *fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) ? SDL_GetWindowFullscreenMode(g_window) : NULL;
    const SDL_DisplayMode *m = fs ? fs : SDL_GetCurrentDisplayMode(d);
    g_display_hz = mode_hz(m);
    LOG("[ui] %s %dx%d at %.3f Hz\n", fs ? "fullscreen" : "display", m ? m->w : 0, m ? m->h : 0, g_display_hz);
    const char *sim = getenv("YKW_SIM_HZ");  // (test) a display this fast: gl/interp.c paces images to a timer
    if (sim && atof(sim) > 0) {
        g_display_hz = atof(sim);
        LOG("[ui] simulating a %.0f Hz display (YKW_SIM_HZ)\n", g_display_hz);
    }
}

// Fullscreen. Without frame interpolation, on a display that isn't at 60 Hz, a 60 Hz mode at the desktop's
// resolution (exclusive fullscreen), so every frame is shown once: in a window 60 frames on a 75 Hz display judder.
// Interpolating (gl/interp.c), or without such a mode, borderless fullscreen at the desktop's rate: every refresh gets
// an image of its own (measured with YKW_SIM_HZ up to 240 Hz).
void ui_set_fullscreen(bool on) {
    extern bool interp_enabled(void);
    if (on) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
        SDL_DisplayMode mode;
        bool exclusive = false;
        if (desk && fabs(mode_hz(desk) - 60.0) > 0.5 && !interp_enabled() &&
            SDL_GetClosestFullscreenDisplayMode(d, desk->w, desk->h, 60.0f, false, &mode) && fabs(mode_hz(&mode) - 60.0) < 0.5)
            exclusive = SDL_SetWindowFullscreenMode(g_window, &mode);
        if (!exclusive) SDL_SetWindowFullscreenMode(g_window, NULL);
        if (desk && fabs(mode_hz(desk) - 60.0) > 0.5)
            LOG("[ui] fullscreen: %s\n", exclusive ? "60 Hz mode" : interp_enabled() ? "the desktop's rate (interpolating)"
                                                                 : "no 60 Hz mode at this resolution: the desktop's rate");
    }
    SDL_SetWindowFullscreen(g_window, on);
    SDL_SyncWindow(g_window);
    ui_note_refresh();
}

// Keyboard layout (positions, so it works on any keyboard language):
//   WASD left stick, arrows d-pad, IJKL right stick, K/L/J/I... taken by the stick, so the face buttons sit on the
//   right hand's home row below: Space = A, Left Shift = B, F = X, R = Y (the left hand keeps the stick),
//   Q = L, E = R, 1 = ZL, 3 = ZR, Enter = Plus, Backspace = Minus. The mouse's left button is the touch screen.
static const struct { SDL_Scancode key; int button; } KEYMAP[] = {
    {SDL_SCANCODE_SPACE, BTN_A}, {SDL_SCANCODE_LSHIFT, BTN_B}, {SDL_SCANCODE_F, BTN_X}, {SDL_SCANCODE_R, BTN_Y},
    {SDL_SCANCODE_Q, BTN_L}, {SDL_SCANCODE_E, BTN_R}, {SDL_SCANCODE_1, BTN_ZL}, {SDL_SCANCODE_3, BTN_ZR},
    {SDL_SCANCODE_RETURN, BTN_PLUS}, {SDL_SCANCODE_BACKSPACE, BTN_MINUS},
    {SDL_SCANCODE_LEFT, BTN_LEFT}, {SDL_SCANCODE_UP, BTN_UP}, {SDL_SCANCODE_RIGHT, BTN_RIGHT}, {SDL_SCANCODE_DOWN, BTN_DOWN},
};
// Gamepads by position (the Switch's A is on the right): east = A, south = B, north = X, west = Y.
static const struct { SDL_GamepadButton b; int button; } PADMAP[] = {
    {SDL_GAMEPAD_BUTTON_EAST, BTN_A}, {SDL_GAMEPAD_BUTTON_SOUTH, BTN_B}, {SDL_GAMEPAD_BUTTON_NORTH, BTN_X},
    {SDL_GAMEPAD_BUTTON_WEST, BTN_Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R}, {SDL_GAMEPAD_BUTTON_START, BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_STICKL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_STICKR},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
};

static int32_t axis(int neg, int pos) { return (pos - neg) * 32767; }
static int32_t pad_axis(SDL_Gamepad *g, SDL_GamepadAxis a, bool invert) {
    int v = SDL_GetGamepadAxis(g, a);
    if (v > -6000 && v < 6000) return 0;  // dead zone
    if (invert) v = -v;
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}
static int32_t clamp_axis(int64_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : (int32_t)v; }

static SDL_Gamepad *g_pad;

static void poll_input(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint64_t b = 0;
    for (size_t i = 0; i < sizeof KEYMAP / sizeof *KEYMAP; i++) if (k[KEYMAP[i].key]) b |= 1ull << KEYMAP[i].button;
    int64_t lx = axis(k[SDL_SCANCODE_A], k[SDL_SCANCODE_D]), ly = axis(k[SDL_SCANCODE_S], k[SDL_SCANCODE_W]);
    int64_t rx = axis(k[SDL_SCANCODE_J], k[SDL_SCANCODE_L]), ry = axis(k[SDL_SCANCODE_K], k[SDL_SCANCODE_I]);
    if (g_pad) {
        for (size_t i = 0; i < sizeof PADMAP / sizeof *PADMAP; i++)
            if (SDL_GetGamepadButton(g_pad, PADMAP[i].b)) b |= 1ull << PADMAP[i].button;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= 1ull << BTN_ZL;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= 1ull << BTN_ZR;
        lx += pad_axis(g_pad, SDL_GAMEPAD_AXIS_LEFTX, false);
        ly += pad_axis(g_pad, SDL_GAMEPAD_AXIS_LEFTY, true);
        rx += pad_axis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX, false);
        ry += pad_axis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    // mouse -> touch screen, in the Switch's 1280x720 screen space
    float mx, my;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    int ww, wh, x0, y0, x1, y1;
    SDL_GetWindowSize(g_window, &ww, &wh);
    extern void fb0_dest_rect(int w, int h, int *x0, int *y0, int *x1, int *y1);  // (gl/fb0.c: the letterboxed image)
    fb0_dest_rect(ww, wh, &x0, &y0, &x1, &y1);
    bool inside = mx >= x0 && mx < x1 && my >= y0 && my < y1;
    bool touch = (mb & SDL_BUTTON_LMASK) && inside && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    // (the image's rectangle in window coordinates -> the touch screen's 1280x720)
    atomic_store(&g_input.touch_x, x1 > x0 ? (int32_t)((mx - x0) * 1280 / (x1 - x0)) : 0);
    atomic_store(&g_input.touch_y, y1 > y0 ? (int32_t)((my - y0) * 720 / (y1 - y0)) : 0);
    atomic_store(&g_input.touch_down, touch);
    if (!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) b = 0, lx = ly = rx = ry = 0;
    atomic_store(&g_input.lx, clamp_axis(lx));
    atomic_store(&g_input.ly, clamp_axis(ly));
    atomic_store(&g_input.rx, clamp_axis(rx));
    atomic_store(&g_input.ry, clamp_axis(ry));
    atomic_store(&g_input.buttons, b);
}

int ui_run(void) {
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return RT_REBOOT_NONE;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_pad) { g_pad = SDL_OpenGamepad(e.gdevice.which); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(g_pad)); }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) { SDL_CloseGamepad(g_pad); g_pad = NULL; }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                    g_settings.fullscreen = !g_settings.fullscreen;
                    ui_set_fullscreen(g_settings.fullscreen);
                }
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                ui_note_refresh();  // (another monitor, or its mode changed)
                break;
            }
        }
        poll_input();
        if (rt_reboot_requested()) return rt_reboot_requested();
        SDL_Delay(4);
    }
}
