// nn::hid on the host: controller and touch screen states from runtime/ui.c. One controller, player 1, as a Pro
// Controller (FullKey) in docked mode; the touch screen is the mouse.
#include "../hle.h"
#include "../input_state.h"
#include "../kernel.h"
#include "../rumble.h"

static int64_t g_sampling;

// YKW_INPUT (test runs): presses at presented frames, "FRAME:BUTTON" or "FIRST-LAST/EVERY:BUTTON", comma separated,
// e.g. "600:A,900-3000/90:A". Each press is held for 6 frames. Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT
// RIGHT, and LUP LDOWN LLEFT LRIGHT for the left stick.
typedef struct { long first, last, every; uint64_t mask; int stick; } ScriptPress;
static ScriptPress g_script[64];
static int g_nscript = -1;

static void script_parse(void) {
    g_nscript = 0;
    const char *e = getenv("YKW_INPUT");
    if (!e) return;
    static const struct { const char *n; int bit, stick; } names[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R}, {"ZL", BTN_ZL},
        {"ZR", BTN_ZR}, {"PLUS", BTN_PLUS}, {"MINUS", BTN_MINUS}, {"UP", BTN_UP}, {"DOWN", BTN_DOWN},
        {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT}, {"LUP", -1, 1}, {"LDOWN", -1, 2}, {"LLEFT", -1, 3}, {"LRIGHT", -1, 4},
    };
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ","); tok && g_nscript < 64; tok = strtok(NULL, ",")) {
        ScriptPress p = {0};
        char name[16];
        if (sscanf(tok, "%ld-%ld/%ld:%15s", &p.first, &p.last, &p.every, name) != 4) {
            if (sscanf(tok, "%ld:%15s", &p.first, name) != 2) continue;
            p.last = p.first, p.every = 1;
        }
        for (size_t k = 0; k < sizeof names / sizeof *names; k++)
            if (!strcmp(name, names[k].n)) {
                if (names[k].bit >= 0) p.mask = 1ull << names[k].bit;
                p.stick = names[k].stick;
            }
        if (p.mask || p.stick) g_script[g_nscript++] = p;
    }
    LOG("[input] YKW_INPUT: %d scripted presses\n", g_nscript);
}

static uint64_t script_buttons(int32_t *lx, int32_t *ly) {
    if (g_nscript < 0) script_parse();
    if (!g_nscript) return 0;
    extern uint64_t ykw_test_tick(void);  // (gl/egl.c: frames, or 1/30 s ticks with YKW_TEST_CLOCK=1)
    long f = (long)ykw_test_tick();
    uint64_t b = 0;
    for (int i = 0; i < g_nscript; i++) {
        ScriptPress *p = &g_script[i];
        if (f < p->first || f > p->last + 6 || p->every <= 0) continue;
        if ((f - p->first) % p->every >= 6) continue;
        b |= p->mask;
        if (p->stick == 1) *ly = 32767;
        if (p->stick == 2) *ly = -32767;
        if (p->stick == 3) *lx = -32767;
        if (p->stick == 4) *lx = 32767;
    }
    static uint64_t last;
    if (b != last) LOG("[input] frame %ld: scripted buttons %#llx\n", f, (unsigned long long)b);
    last = b;
    return b;
}

static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 40);
    *(int64_t *)s = ++g_sampling;
    if (id != 0) return;  // (player 1 only)
    uint64_t b = atomic_load(&g_input.buttons);
    int32_t slx = 0, sly = 0;
    b |= script_buttons(&slx, &sly);
    int32_t lx = atomic_load(&g_input.lx), ly = atomic_load(&g_input.ly);
    int32_t rx = atomic_load(&g_input.rx), ry = atomic_load(&g_input.ry);
    if (slx) lx = slx;
    if (sly) ly = sly;
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid14InitializeNpadEv", hid_init_npad) { (void)c; }
HLE_FUNC("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", hid_set_ids) { (void)c; }
HLE_FUNC("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_set_styles) { (void)c; }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }

// FullKey for player 1 only (docked: no handheld controller)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    c->x[0] = id == 0 ? 1 : 0;
}

// ---- touch screen
HLE_FUNC("_ZN2nn3hid21InitializeTouchScreenEv", hid_init_touch) { (void)c; }
// GetTouchScreenStates<1>(TouchScreenState<1> *out, int count) -> states written. State: sampling (8), count (4),
// reserved (4), touch: delta time (8), attributes (4), finger id (4), x, y, diameter x, y, angle (4 each), reserved.
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    if (count < 1) { c->x[0] = 0; return; }
    static int64_t sampling;
    memset(s, 0, 0x10 + 0x28);
    *(int64_t *)s = ++sampling;
    if (atomic_load(&g_input.touch_down)) {
        *(int32_t *)(s + 8) = 1;
        uint8_t *t = s + 0x10;
        *(int32_t *)(t + 0xC) = 0;  // finger id
        *(int32_t *)(t + 0x10) = atomic_load(&g_input.touch_x);
        *(int32_t *)(t + 0x14) = atomic_load(&g_input.touch_y);
        *(int32_t *)(t + 0x18) = 15;
        *(int32_t *)(t + 0x1C) = 15;
    }
    c->x[0] = 1;
}

// ---- vibration: accepted and dropped for now
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", hid_send_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = 3u | ((id & 0xFF) << 8) | ((uint32_t)i << 16);
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;  // LinearResonantActuator
    int side = rumble_side(h);
    info[1] = side < 0 ? 0 : (uint32_t)side + 1;
}

// HID IPC's vibration commands (services/hid_set.c) end here
void input_rumble_send(uint32_t handle, const RumbleValue *value) { (void)handle, (void)value; }
RumbleValue input_rumble_get(uint32_t handle) { (void)handle; return (RumbleValue){0, 160, 0, 320}; }
static bool g_rumble_ok = true;
void input_rumble_permit(bool permitted) { g_rumble_ok = permitted; }
bool input_rumble_permitted(void) { return g_rumble_ok; }

// (hle/movie.c's opening-skip support, off for this game)
uint64_t input_buttons_now(void) { return atomic_load(&g_input.buttons); }
volatile int g_inject_plus;
