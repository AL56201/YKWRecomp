// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include <math.h>

#include "../hle.h"
#include "../kernel.h"
#include "../settings.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c, runtime/nvn/pace.c)
volatile uint64_t g_vsync_ns;     // plat_now_ns() of the latest (stored after the count: count >= the vsync it's from;
                                  // also frame interpolation's game timeline)
uint64_t g_native_window;

// The game's vsync: 60 Hz, as on the Switch (hle/framerate.c: its logic steps 1/60 s per vsync).
//
// At 60 fps on a 60 Hz display (vsync on), a timer at 60.000 Hz drifts against the display's real refresh and a frame
// now and then just misses its refresh (measured at 75 Hz with a 75 Hz timer: 72-73 fps, ~2 stutters a second). There
// the game's vsync is each finished presentation (vi_presented, after eglSwapBuffers); the timer is only the fallback
// for frames that miss, so the game still counts the vsyncs a slow frame took and catches its logic up (measured at
// 75 Hz the same way: a steady 75.0 fps). YKW_VSYNC_TIMER=1: the plain timer.
static pthread_mutex_t g_pres_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pres_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_presented;
static bool g_present_locked, g_interp_locked;

// With frame interpolation on a display faster than 60 Hz (gl/interp.c, YKW_INTERP_LOCK=1 only), the same idea: each
// image is shown at its own refresh, and the display asks for the game frame its next refresh needs
// (vi_request_vsyncs), so the game runs on the display's clock.
//
// (re-checked all the time: F11 and moving the window change the refresh the frames are shown at - presentations at
// 75 Hz would run the game 25% fast)
bool vi_present_locked(void) {
    extern double g_display_hz;
    extern bool ykw_fps_unlocked(void), interp_enabled(void);
    bool on = ykw_fps_unlocked() && g_settings.vsync && !getenv("YKW_VSYNC_TIMER");
    // (Interpolating, the game stays on its own vsync timer by default: each image is paced to the compositor and
    // stale in-betweens are skipped (gl/interp.c). Measured on a quiet machine, 60 s: 4 images late by >20 ms, none
    // by >30, steady 60 fps. Locked to the display instead (YKW_INTERP_LOCK=1) it has two steady states - a frame
    // that takes over a refresh to render gets a second vsync, spans two steps, takes longer still - and fell to 30
    // fps with 167 images >30 ms late.)
    g_interp_locked = on && interp_enabled() && g_display_hz > 60.5 && getenv("YKW_INTERP_LOCK");
    g_present_locked = on && (fabs(g_display_hz - 60.0) < 0.5 || g_interp_locked);
    return g_present_locked;
}
static uint64_t g_pres_seen;  // (the presentations the vsync thread has answered)
static KThread *g_frame_thread;  // (the thread that calls eglSwapBuffers: the game's frame loop)
// (tools/patches.py VSYNC_COUNT_FIRST_SITE) YKW_PACING_RACE=1: the game's own order - its vsync thread wakes the
// frame pacing before it counts the vsync (now: counts it first)
int ykw_pacing_race(void) { return ENV("YKW_PACING_RACE") != NULL; }

KThread *vi_frame_thread(void) { return __atomic_load_n(&g_frame_thread, __ATOMIC_ACQUIRE); }
void vi_set_frame_thread(void) {
    extern KThread *kernel_current_thread(void);
    __atomic_store_n(&g_frame_thread, kernel_current_thread(), __ATOMIC_RELEASE);
}
static volatile uint64_t g_image_ns;  // (frame interpolation: when the latest image reached the display)
void vi_image_shown(void) { __atomic_store_n(&g_image_ns, plat_now_ns(), __ATOMIC_RELEASE); }
void vi_presented(void) {
    pthread_mutex_lock(&g_pres_lock);
    g_presented++;
    pthread_cond_signal(&g_pres_cond);
    pthread_mutex_unlock(&g_pres_lock);
}
// (frame interpolation) vsyncs up to this many in all (g_vsync_count): one more now, if fewer than that have gone out
// or are on their way (more than one at once would be one: the game counts vsyncs as they come)
// true: one asked for (it is vsync number *which, in g_vsync_count's count)
bool vi_request_vsyncs(uint64_t total, uint64_t *which) {
    pthread_mutex_lock(&g_pres_lock);
    uint64_t have = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE) + (g_presented - g_pres_seen);
    bool asked = total > have;
    if (asked) {
        g_presented++;
        *which = have + 1;
        pthread_cond_signal(&g_pres_cond);
    }
    pthread_mutex_unlock(&g_pres_lock);
    return asked;
}

static void *vsync_thread(void *arg) {
    (void)arg;
    extern double ykw_vsync_hz(void);
    vi_present_locked();
    const double period = 1e9 / ykw_vsync_hz();
    plat_thread_realtime((uint64_t)period);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    uint64_t start = plat_now_ns(), k = 0;
    uint64_t last = start, seen = 0;
    LOG("[vi] vsync at %.3f Hz%s\n", ykw_vsync_hz(), g_present_locked ? ", locked to the display's presentations" : "");
    for (;;) {
        uint64_t now;
        if (g_present_locked) {
            // (interpolating: images go out every refresh while the game keeps up, so the timer counts from the latest
            // image - past the next refresh without one, the game is late (a slow frame, or a vsync it didn't see:
            // now and then one is taken without the frame going on, measured every ~2 s) and gets the next vsync, as
            // the display's timeline has moved on. Counted from the last vsync it also fired in the two or three
            // images of a frame that spans two steps, and kept the game at two steps a frame: 30 fps)
            extern double g_display_hz;
            uint64_t deadline = last + (uint64_t)(period * 1.25);
            pthread_mutex_lock(&g_pres_lock);
            for (;;) {
                if (g_interp_locked) {
                    uint64_t img = __atomic_load_n(&g_image_ns, __ATOMIC_ACQUIRE);
                    deadline = (img > last ? img : last) + (uint64_t)(1.5e9 / g_display_hz);
                }
                if (g_presented != seen || plat_now_ns() >= deadline) break;
                struct timespec ts;  // (the condition variable's clock is the wall clock)
                clock_gettime(CLOCK_REALTIME, &ts);
                uint64_t d = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (deadline - plat_now_ns());
                ts.tv_sec = (time_t)(d / 1000000000ull), ts.tv_nsec = (long)(d % 1000000000ull);
                pthread_cond_timedwait(&g_pres_cond, &g_pres_lock, &ts);
            }
            bool presented = g_presented != seen;
            seen = g_pres_seen = g_presented;
            pthread_mutex_unlock(&g_pres_lock);
            now = plat_now_ns();
            last = presented ? now : last + (uint64_t)period;
            if (now > last + (uint64_t)(period * 3)) last = now;  // (don't burst-signal to catch up)
            if (!presented && g_interp_locked)  // (in a stall: the next one a vsync period on, not at once)
                last = now + (uint64_t)period - (uint64_t)(1.5e9 / g_display_hz);
            if (ENV("YKW_VSYNC_TRACE") && g_interp_locked)
                LOG("[vs] signal %llu: %s\n", (unsigned long long)g_vsync_count + 1, presented ? "asked" : "timer");
        } else {
            uint64_t next = start + (uint64_t)(++k * period);
            plat_sleep_until_ns(next);
            now = plat_now_ns();
            if (now > next + (uint64_t)(period * 3)) {  // don't burst-signal to catch up
                start = now, k = 0;
            }
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_vsync_ns, now, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}
// CreateLayer(layer, display, width, height)
HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", vi_create_layer_wh) {
    LOG("[vi] CreateLayer %dx%d\n", (int)c->x[2], (int)c->x[3]);
    extern void fb0_set_size(int w, int h);  // (gl/fb0.c: the size of the game's framebuffer 0)
    fb0_set_size((int)c->x[2], (int)c->x[3]);
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_scaling) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
