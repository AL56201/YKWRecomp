// Frame rate. The engine waits a whole number of vsyncs per frame (its "interval", 2 = 30 fps at boot) and scales
// game logic by interval / base, where base is its logic step in vsyncs (tools/patches.py, FRAME_INTERVAL_SITES).
// With the frame rate unlocked the interval is the base: one frame per vsync of our 60 Hz timer (hle/vi.c), each
// frame one logic step, so the game runs at 60 fps at its normal speed.
//   settings: unlock_fps = 1 (default 0); YKW_FPS=60|30 overrides it for a run.
#include "../hle.h"
#include "../settings.h"

static bool unlocked(void) {
    const char *e = getenv("YKW_FPS");
    if (e && *e) return atoi(e) >= 60;
    return g_settings.unlock_fps;
}

uint32_t ykw_frame_interval(uint32_t requested, uint32_t base) {
    uint32_t r = unlocked() && requested > base ? base : requested;
    static uint32_t last_req = ~0u, last_r = ~0u;
    if (requested != last_req || r != last_r)
        LOG("[fps] game asks for %u vsync(s) per frame (logic step %u): %u%s\n", requested, base, r,
            r != requested ? " (unlocked)" : "");
    last_req = requested, last_r = r;
    return r;
}
