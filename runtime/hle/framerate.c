// Frame rate. The engine waits a whole number of vsyncs per frame (its "interval": 2 = 30 fps at boot) and advances its
// logic a fixed 1/60 s per vsync: nothing in the game reads a time delta (a watch on its frame-rate fields, and on the
// float time scale it stores next to them, which no code reads). So the game runs at the right speed at 30 and 60 fps
// only; above 60 it runs fast (measured with the field camera's target: 30 and 60 fps 58-60 units/s, 75 fps 72,
// 144 fps 138), and the camera fell behind the faster game.
//
// Unlocked (fps > 30), the interval is the base (tools/patches.py FRAME_INTERVAL_SITES): one frame per vsync of our
// 60 Hz timer (hle/vi.c). On a display that isn't 60 Hz that judders in a window (60 frames on 75 refreshes); in
// fullscreen runtime/ui.c switches the display to a 60 Hz mode, where it is smooth.
//   ykwrecomp.ini: fps = 30 (the game's own, default) | 60 (display and higher numbers mean 60 too)
//   YKW_FPS overrides it for a run.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

double g_display_hz = 60.0;  // (runtime/ui.c, from the window's display mode)

static double g_target = -1;  // frames per second: 30 or 60

static double parse(const char *s) {
    if (!s || !*s) return -1;
    if (!strcmp(s, "display")) return 0;
    return atof(s);
}

static void resolve(void) {
    if (g_target >= 0) return;
    double t = parse(getenv("YKW_FPS"));
    if (t < 0) t = g_settings.fps;
    double asked = t;
    g_target = t == 0 || t > 30.5 ? 60 : 30;  // (display = 0)
    if (asked > 60.5 || asked == 0)
        LOG("[fps] %s asked: capped at 60 (the game's logic steps 1/60 s per frame; faster would run the game fast)\n",
            asked == 0 ? "display" : "above 60");
    LOG("[fps] target %.0f fps (display %.2f Hz)\n", g_target, g_display_hz);
}

double ykw_target_fps(void) { resolve(); return g_target; }
bool ykw_fps_unlocked(void) { resolve(); return g_target > 30.5; }
double ykw_vsync_hz(void) { return 60.0; }  // (the Switch's: two per frame at 30 fps, one at 60)

uint32_t ykw_frame_interval(uint32_t requested, uint32_t base) {
    uint32_t r = ykw_fps_unlocked() && requested > base ? base : requested;
    static uint32_t last_req = ~0u, last_r = ~0u;
    if (requested != last_req || r != last_r)
        LOG("[fps] game asks for %u vsync(s) per frame (logic step %u): %u%s\n", requested, base, r,
            r != requested ? " (unlocked)" : "");
    last_req = requested, last_r = r;
    return r;
}

// (debug) YKW_CAM_LOG=<first tick>-<last tick>: the field camera's two points (+0x2cc: where it is, +0x624: where it
// is heading) and their distance, every update in that range of test ticks (gl/egl.c ykw_test_tick). The target's
// speed is the game's speed (tools/patches.py CAM_PROBE_SITE).
void ykw_cam_probe(uint64_t cam, uint32_t dt_bits) {
    static long lo = -2, hi;
    if (lo == -2) {
        const char *e = getenv("YKW_CAM_LOG");
        lo = -1;
        if (e && sscanf(e, "%ld-%ld", &lo, &hi) != 2) lo = -1;
    }
    if (lo < 0 || !cam) return;
    extern uint64_t ykw_test_tick(void);
    long t = (long)ykw_test_tick();
    static bool watching;
    if (!watching && getenv("YKW_CAM_WATCH") && t >= lo) {  // (who writes the camera's position: platform/windows.c)
        extern void plat_watch_write(uint64_t addr);
        plat_watch_write(cam + 0x2cc);
        watching = true;
        LOG("[watch] on %llx\n", (unsigned long long)(cam + 0x2cc));
    }
    if (t < lo || t > hi) return;
    const float *a = (const float *)(uintptr_t)(cam + 0x2cc), *b = (const float *)(uintptr_t)(cam + 0x624);
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float dt;
    memcpy(&dt, &dt_bits, 4);
    LOG("[cam] tick %ld t %.3f dt %g a %.2f %.2f %.2f b %.2f %.2f %.2f dist %.3f\n", t, plat_now_ns() / 1e9, dt, a[0],
        a[1], a[2], b[0], b[1], b[2], sqrtf(dx * dx + dy * dy + dz * dz));
    if (getenv("YKW_CAM_RAW")) {  // (the follow object at +0x2b0: its words, to see what gates its update)
        const uint32_t *w = (const uint32_t *)(uintptr_t)(cam + 0x2b0);
        char s[512];
        int k = 0;
        for (int i = 0; i < 12; i++) k += snprintf(s + k, sizeof s - k, " %08x", w[i]);
        LOG("[camraw] +2b0:%s\n", s);
    }
}
