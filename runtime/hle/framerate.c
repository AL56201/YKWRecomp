// Frame rate. The engine waits a whole number of vsyncs per frame (its "interval", 2 = 30 fps at boot), runs one logic
// step per vsync (replaying the ones a slow frame missed) and scales movement and timers by a time scale it stores at
// +0x7dc: interval / base, in 60 Hz steps (tools/patches.py, FRAME_INTERVAL_SITES / TIME_SCALE_SITES).
//
// Unlocked, the interval is the base (one frame per vsync), our vsync timer (hle/vi.c) runs at the target rate, and the
// stored time scale is multiplied by 60 / rate, so the game keeps its speed at any rate. "display" matches the
// monitor's refresh (a 75 Hz monitor at 60 fps showed every 4th frame twice).
//   ykwrecomp.ini: fps = 30 (the game's own, default) | 60 | display | <number>   (unlock_fps = 1: display)
//   YKW_FPS overrides it for a run.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

double g_display_hz = 60.0;  // (runtime/ui.c, from the window's display mode)

static double g_target = -1;  // vsyncs per second: 60 = the game's own clock

static double parse(const char *s) {
    if (!s || !*s) return -1;
    if (!strcmp(s, "display")) return 0;  // (resolved below)
    return atof(s);
}

double ykw_vsync_hz(void) {
    if (g_target < 0) {
        double t = parse(getenv("YKW_FPS"));
        if (t < 0) t = g_settings.fps;
        if (t == 0) t = g_display_hz;       // display
        if (t < 0 || (t > 0 && t <= 30.5)) t = 30;  // the game's own
        if (t > 500) t = 500;
        g_target = t;
        LOG("[fps] target %.2f fps (display %.2f Hz)%s\n", t, g_display_hz, t <= 30.5 ? ": the game's own 30" : "");
    }
    return g_target <= 30.5 ? 60.0 : g_target;  // (locked: the Switch's 60 Hz vsync, two per frame)
}

static bool unlocked(void) { ykw_vsync_hz(); return g_target > 30.5; }
double ykw_target_fps(void) { ykw_vsync_hz(); return g_target; }  // (frames per second we aim for: 30 when locked)

uint32_t ykw_frame_interval(uint32_t requested, uint32_t base) {
    uint32_t r = unlocked() && requested > base ? base : requested;
    static uint32_t last_req = ~0u, last_r = ~0u;
    if (requested != last_req || r != last_r)
        LOG("[fps] game asks for %u vsync(s) per frame (logic step %u): %u%s\n", requested, base, r,
            r != requested ? " (unlocked)" : "");
    last_req = requested, last_r = r;
    return r;
}

// the engine's time scale (interval / base, in 60 Hz steps) for our vsync rate
float ykw_time_scale(float game_scale) {
    if (!unlocked()) return game_scale;
    return (float)(game_scale * 60.0 / ykw_vsync_hz());
}
