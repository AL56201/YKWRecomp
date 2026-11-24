// EGL on the host: the game's GL driver (exefs/subsdk0) isn't loaded. Its EGL entry points are implemented here on SDL's
// OpenGL contexts, and eglGetProcAddress hands out the generated GL thunks (gl_thunks.c), which call the host driver.
// The game asks for desktop OpenGL (eglBindAPI(EGL_OPENGL_API), "#version 450" shaders), so most calls pass straight
// through.
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include "gl_host.h"
#include "../settings.h"

#define EGL_SUCCESS 0x3000
#define EGL_NO_CONTEXT 0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB

SDL_Window *g_window;               // runtime/ui.c makes it before the game starts
static SDL_GLContext g_root;        // never current for the game: what every game context shares objects with
static pthread_mutex_t g_ctx_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_CTX 16
static SDL_GLContext g_ctx[MAX_CTX + 1];  // EGLContext n -> g_ctx[n]
static volatile uint64_t g_swaps;
uint64_t egl_swap_count(void) { return g_swaps; }

void *gl_proc(const char *name) { return (void *)SDL_GL_GetProcAddress(name); }

void gl_missing(const char *name) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[256];
    pthread_mutex_lock(&m);
    for (int i = 0; i < 256; i++) {
        if (seen[i] == name) break;
        if (!seen[i]) { seen[i] = name; LOG("[gl] %s: not in the host driver (ignored)\n", name); break; }
    }
    pthread_mutex_unlock(&m);
}

void gl_unsupported(const char *name) {
    static int n;
    if (n++ < 16) LOG("[gl] %s: not forwarded (callback argument)\n", name);
}

// main thread, after the window: the root context exists before any game thread asks for one
void egl_host_init(void) {
    g_root = SDL_GL_CreateContext(g_window);
    if (!g_root) FATAL("cannot create an OpenGL 4.5 context: %s\n", SDL_GetError());
    LOG("[gl] %s / %s / %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION));
    SDL_GL_MakeCurrent(g_window, NULL);
    const char *t = getenv("YKW_GL_TRACE");
    if (t && !strcmp(t, "all")) g_gl_trace_on = 1;  // (from the first call, before any frame)
}

HLE_FUNC("eglGetDisplay", egl_get_display) { c->x[0] = 1; }
HLE_FUNC("eglInitialize", egl_initialize) {
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 1;
    if (c->x[2]) *(int32_t *)(uintptr_t)c->x[2] = 5;
    c->x[0] = 1;
}
HLE_FUNC("eglTerminate", egl_terminate) { c->x[0] = 1; }
HLE_FUNC("eglReleaseThread", egl_release_thread) { c->x[0] = 1; }
HLE_FUNC("eglBindAPI", egl_bind_api) {
    LOG("[egl] eglBindAPI(%#llx)\n", (unsigned long long)c->x[0]);
    c->x[0] = 1;
}
HLE_FUNC("eglGetError", egl_get_error) { c->x[0] = EGL_SUCCESS; }

// one config: RGBA8, depth 24, stencil 8 (what the window has)
HLE_FUNC("eglChooseConfig", egl_choose_config) {
    uint64_t configs = c->x[2], num = c->x[4];
    int size = (int)c->x[3];
    if (configs && size > 0) *(uint64_t *)(uintptr_t)configs = 1;
    if (num) *(int32_t *)(uintptr_t)num = 1;
    c->x[0] = 1;
}

HLE_FUNC("eglCreateWindowSurface", egl_create_window_surface) { c->x[0] = 1; }
HLE_FUNC("eglDestroySurface", egl_destroy_surface) { c->x[0] = 1; }

// Every context shares objects with the root one (and so with each other), as the game's loading threads expect.
HLE_FUNC("eglCreateContext", egl_create_context) {
    pthread_mutex_lock(&g_ctx_lock);
    int id = 0;
    for (int i = 1; i <= MAX_CTX; i++) if (!g_ctx[i]) { id = i; break; }
    if (!id) { pthread_mutex_unlock(&g_ctx_lock); LOG("[egl] too many contexts\n"); c->x[0] = 0; return; }
    SDL_GLContext prev = SDL_GL_GetCurrentContext();
    SDL_GL_MakeCurrent(g_window, g_root);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext ctx = SDL_GL_CreateContext(g_window);
    SDL_GL_MakeCurrent(g_window, prev);
    pthread_mutex_unlock(&g_ctx_lock);
    if (!ctx) { LOG("[egl] eglCreateContext failed: %s\n", SDL_GetError()); c->x[0] = 0; return; }
    g_ctx[id] = ctx;
    LOG("[egl] context %d (share %llu)\n", id, (unsigned long long)c->x[2]);
    c->x[0] = (uint64_t)id;
}

HLE_FUNC("eglDestroyContext", egl_destroy_context) {
    int id = (int)c->x[1];
    if (id > 0 && id <= MAX_CTX && g_ctx[id]) {
        if (SDL_GL_GetCurrentContext() == g_ctx[id]) SDL_GL_MakeCurrent(g_window, NULL);
        SDL_GL_DestroyContext(g_ctx[id]);
        g_ctx[id] = NULL;
    }
    c->x[0] = 1;
}

HLE_FUNC("eglMakeCurrent", egl_make_current) {
    int id = (int)c->x[3];
    SDL_GLContext ctx = id > 0 && id <= MAX_CTX ? g_ctx[id] : NULL;
    bool ok = SDL_GL_MakeCurrent(g_window, ctx);
    if (!ok) LOG("[egl] eglMakeCurrent(%d) failed: %s\n", id, SDL_GetError());
    extern void fb0_make_current(int id);  // (fb0.c)
    fb0_make_current(ok && ctx ? id : 0);
    c->x[0] = ok;
}

HLE_FUNC("eglSwapInterval", egl_swap_interval) {
    static int last = -1;
    if ((int)c->x[1] != last) LOG("[egl] eglSwapInterval(%d)\n", (int)c->x[1]);
    last = (int)c->x[1];
    // (a frame rate above the display's: the driver's vsync would hold it to the display, so it's off; the game's own
    // pacing - our vsync timer - keeps the rate)
    extern double ykw_vsync_hz(void), g_display_hz;
    bool above = ykw_vsync_hz() > g_display_hz + 0.5;
    SDL_GL_SetSwapInterval(g_settings.vsync && !above ? (int)c->x[1] : 0);
    c->x[0] = 1;
}

// The test tick scripted input and screenshots count in: presented frames, or with YKW_TEST_CLOCK=1 thirtieths of a
// second since the first frame (so runs at different frame rates line up in time).
uint64_t ykw_test_tick(void) {
    static int clock = -1;
    static uint64_t t0;
    if (clock < 0) clock = getenv("YKW_TEST_CLOCK") != NULL;
    if (!clock) return g_swaps;
    if (!g_swaps) return 0;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    return (now - t0) / 33333333ull;
}

static int thunk_cmp_(const void *k, const void *e) { return strcmp((const char *)k, ((const GlThunk *)e)->name); }
volatile int g_gl_trace_on;
void gl_trace(const char *name, Cpu *c) {
    // YKW_GL_TRACE_ONLY=Tex,Framebuffer: only calls whose name contains one of these
    static char filt[256];
    static int init;
    if (!init) { const char *e = getenv("YKW_GL_TRACE_ONLY"); snprintf(filt, sizeof filt, "%s", e ? e : ""); init = 1; }
    if (filt[0]) {
        bool hit = false;
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", filt);
        for (char *save = NULL, *tok = strtok_r(tmp, ",", &save); tok && !hit; tok = strtok_r(NULL, ",", &save))
            hit = strstr(name, tok) != NULL;
        if (!hit) return;
    }
    LOG("[gltrace] %s(%llx, %llx, %llx, %llx, %llx, %llx) v0=%g\n", name, (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
        (unsigned long long)c->x[4], (unsigned long long)c->x[5], gl_vf(c->v[0]));
}

// YKW_SHOT_EVERY=<frames> (test runs): the presented image every that many frames, at half size, to
// YKW_SHOT_DIR (default "shots") as frame_<n>.bmp - from the game's own back buffer, so other windows don't matter.
static void shot_maybe(uint64_t n) {
    static long every = -1;
    static uint64_t last_slot;
    if (every < 0) { const char *e = getenv("YKW_SHOT_EVERY"); every = e ? atol(e) : 0; }
    if (every <= 0) return;
    if (getenv("YKW_TEST_CLOCK")) {  // (by time: the first frame of each slot)
        extern uint64_t ykw_test_tick(void);
        uint64_t slot = ykw_test_tick() / every;
        if (!slot || slot == last_slot) return;
        last_slot = slot;
        n = slot * every;
    } else if (n % every) return;
    const char *dir = getenv("YKW_SHOT_DIR");
    if (!dir || !*dir) dir = "shots";
    int w, h;
    static PFNGLBINDFRAMEBUFFERPROC bind_fb;
    if (!bind_fb) bind_fb = (PFNGLBINDFRAMEBUFFERPROC)gl_proc("glBindFramebuffer");
    GLint read_fb, pack, read_buf;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    extern bool fb0_read_bind(int *w, int *h);  // (fb0.c: the game's own image, whatever the window's size)
    if (!fb0_read_bind(&w, &h)) {
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        bind_fb(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
    }
    uint8_t *px = malloc((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glReadBuffer((GLenum)read_buf);
    {   // sharpness of the 3D area (the middle, clear of the HUD): mean |luma difference| between neighbours, x100
        uint64_t sum = 0, cnt = 0;
        for (int y = h * 35 / 100; y < h * 75 / 100; y++)
            for (int x = w * 20 / 100; x < w * 60 / 100; x++) {
                const uint8_t *a = px + ((size_t)y * w + x) * 4, *b = a + 4, *d = a + (size_t)w * 4;
                int la = a[0] * 3 + a[1] * 6 + a[2], lb = b[0] * 3 + b[1] * 6 + b[2], ld = d[0] * 3 + d[1] * 6 + d[2];
                sum += (uint64_t)(abs(la - lb) + abs(la - ld));
                cnt++;
            }
        LOG("[shot] frame %llu sharpness %.1f\n", (unsigned long long)n, cnt ? sum * 10.0 / cnt : 0.0);
    }
    SDL_Surface *s = SDL_CreateSurface(w / 2, h / 2, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < h / 2; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
        const uint32_t *src = (const uint32_t *)(px + (size_t)(h - 1 - 2 * y) * w * 4);
        for (int x = 0; x < w / 2; x++) dst[x] = src[2 * x] | 0xFF000000u;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05llu.bmp", dir, (unsigned long long)n);
    if (!SDL_SaveBMP(s, path)) LOG("[shot] %s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(s);
    free(px);
}

// YKW_DOF_PROBE=<frame> (debug): at that frame, the depth-of-field uniforms of every program that has them, and the
// depth buffer of framebuffer YKW_DOF_PROBE_FB (default 5) at a few points
static void dof_probe(uint64_t n) {
    static long at = -2;
    if (at == -2) { const char *e = getenv("YKW_DOF_PROBE"); at = e ? atol(e) : -1; }
    if (at < 0 || (long)n != at) return;
    typedef GLint (APIENTRY *LocFn)(GLuint, const GLchar *);
    typedef void (APIENTRY *GetUfFn)(GLuint, GLint, GLfloat *);
    typedef GLboolean (APIENTRY *IsProgFn)(GLuint);
    LocFn loc = (LocFn)gl_proc("glGetUniformLocation");
    GetUfFn getf = (GetUfFn)gl_proc("glGetUniformfv");
    IsProgFn isprog = (IsProgFn)gl_proc("glIsProgram");
    static const char *names[] = {"unf_Posteffect.dofUse", "unf_Posteffect.dofRate", "unf_Posteffect.dofFocusZ",
                                  "unf_Posteffect.dofMaxBokehZ", "unf_Posteffect.bloomUse", "unf_Posteffect.fogUse",
                                  "unf_camera_near_far", "unf_Gaussian.sampleRange", "unf_Gaussian.textureSize",
                                  "unf_Posteffect.bloomContrast", "unf_Posteffect.bloomColorScale",
                                  "unf_Posteffect.fogDepthMin", "unf_Posteffect.fogDepthMax", "unf_Posteffect.fogColor"};
    typedef void (APIENTRY *GetUiFn)(GLuint, GLint, GLint *);
    GetUiFn geti = (GetUiFn)gl_proc("glGetUniformiv");
    for (GLuint p = 1; p < 400; p++) {
        if (!isprog(p) || loc(p, "unf_Posteffect.dofFocusZ") < 0) continue;
        static const char *samplers[] = {"unf_frg_txt_2d_0", "unf_frg_txt_2d_1", "unf_frg_txt_2d_2", "unf_frg_txt_2d_3"};
        for (int i = 0; i < 4; i++) {
            GLint l = loc(p, samplers[i]), unit = -1;
            if (l >= 0) geti(p, l, &unit);
            LOG("[dofprobe] program %u %s: location %d, unit %d\n", p, samplers[i], l, unit);
        }
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            GLint l = loc(p, names[i]);
            if (l < 0) continue;
            GLfloat v[4] = {0};
            getf(p, l, v);
            LOG("[dofprobe] program %u %s = %g %g %g %g\n", p, names[i], v[0], v[1], v[2], v[3]);
        }
    }
    const char *fe = getenv("YKW_DOF_PROBE_FB");
    GLuint fb = fe ? (GLuint)atoi(fe) : 5;
    static PFNGLBINDFRAMEBUFFERPROC bind_fb;
    if (!bind_fb) bind_fb = (PFNGLBINDFRAMEBUFFERPROC)gl_proc("glBindFramebuffer");
    GLint read_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    bind_fb(GL_READ_FRAMEBUFFER, fb);
    static const int pts[][2] = {{960, 540}, {960, 900}, {960, 150}, {300, 540}, {1600, 540}, {480, 300}};
    for (size_t i = 0; i < sizeof pts / sizeof *pts; i++) {
        GLfloat d = -1;
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
        LOG("[dofprobe] fb %u depth at (%d,%d) = %.6f (gl error %#x)\n", fb, pts[i][0], pts[i][1], d, glGetError());
    }
    // the post effect's horizontal pass targets (framebuffer YKW_DOF_PROBE_FB6, default 6: colour, ext, depth, blur)
    // and the final mix in framebuffer 5's colour, at the same points
    const char *f6 = getenv("YKW_DOF_PROBE_FB6");
    GLuint fb6 = f6 ? (GLuint)atoi(f6) : 6;
    GLint read_buf;
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    for (size_t i = 0; i < sizeof pts / sizeof *pts; i++) {
        float v[5][4];
        bind_fb(GL_READ_FRAMEBUFFER, fb6);
        for (int a = 0; a < 4; a++) {
            glReadBuffer(GL_COLOR_ATTACHMENT0 + a);
            glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_FLOAT, v[a]);
        }
        bind_fb(GL_READ_FRAMEBUFFER, fb);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_FLOAT, v[4]);
        LOG("[dofprobe] (%d,%d) colour %.3f %.3f %.3f | depth %.5f | blur %.3f %.3f %.3f | final %.3f %.3f %.3f "
            "(gl error %#x)\n", pts[i][0], pts[i][1], v[0][0], v[0][1], v[0][2], v[2][0], v[3][0], v[3][1], v[3][2],
            v[4][0], v[4][1], v[4][2], glGetError());
    }
    glReadBuffer((GLenum)read_buf);
    bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
}

HLE_FUNC("eglSwapBuffers", egl_swap_buffers) {
    dof_probe(g_swaps + 1);
    shot_maybe(g_swaps + 1);
    extern void fb0_present(void);  // (fb0.c: the game's image, scaled into the window)
    fb0_present();
    SDL_GL_SwapWindow(g_window);
    extern bool vi_present_locked(void);
    extern void vi_presented(void);
    if (vi_present_locked()) {  // (hle/vi.c: the game's vsync is this frame reaching the display)
        glFinish();  // (returns once the swap is done: at the refresh it was shown at, not when it was queued)
        vi_presented();
    }
    uint64_t n = __atomic_add_fetch(&g_swaps, 1, __ATOMIC_RELEASE);
    {   // frames per second and frame-to-frame pacing, every 5 s (YKW_FPS_LOG=0: off). "uneven": frames that took more
        // than 1.5x the target frame time (each one is a visible stutter)
        static uint64_t t0, n0, last, uneven;
        static double sum, sum2, worst;
        static int off = -1;
        if (off < 0) { const char *e = getenv("YKW_FPS_LOG"); off = e && !strcmp(e, "0"); }
        uint64_t now = plat_now_ns();
        extern double ykw_vsync_hz(void);
        extern double ykw_target_fps(void);
        double target_ms = 1000.0 / ykw_target_fps();
        if (last) {
            double dt = (now - last) / 1e6;
            sum += dt, sum2 += dt * dt;
            if (dt > worst) worst = dt;
            if (dt > target_ms * 1.5 && dt < target_ms * 2.5 * 4) uneven++;
        }
        last = now;
        if (!t0) t0 = now, n0 = n;
        if (!off && now - t0 >= 5000000000ull) {
            double cnt = (double)(n - n0), mean = cnt ? sum / cnt : 0;
            double sd = cnt ? sqrt(fmax(0, sum2 / cnt - mean * mean)) : 0;
            LOG("[fps] %.1f (frame %llu): frame time %.2f ms +- %.2f, worst %.1f, %llu uneven\n",
                cnt * 1e9 / (double)(now - t0), (unsigned long long)n, mean, sd, worst, (unsigned long long)uneven);
            t0 = now, n0 = n, sum = sum2 = worst = 0, uneven = 0;
        }
    }
    static long trace_at = -2;
    if (trace_at == -2) {  // (YKW_GL_TRACE=all: every frame)
        const char *e = getenv("YKW_GL_TRACE");
        trace_at = e && !strcmp(e, "all") ? -1 : e && *e ? atol(e) : -1;
        if (e && !strcmp(e, "all")) g_gl_trace_on = 1;
    }
    if (n == 60 && getenv("YKW_FIND_GL")) {  // (debug: where main keeps these GL pointers: YKW_FIND_GL=glA,glB)
        char names[512];
        snprintf(names, sizeof names, "%s", getenv("YKW_FIND_GL"));
        for (char *nm = strtok(names, ","); nm; nm = strtok(NULL, ",")) {
            const GlThunk *t = bsearch(nm, gl_thunks, gl_thunk_count, sizeof *gl_thunks, thunk_cmp_);
            if (!t) continue;
            uint64_t want = hle_register(t->name, t->fn);
            Module *m = &g_modules[0];
            for (uint64_t a = m->base + m->data_off; a + 8 <= m->base + m->data_off + m->data_size; a += 8)
                if (*(uint64_t *)(uintptr_t)a == want) LOG("[findgl] %s at main+%#llx\n", nm, (unsigned long long)(a - m->base));
        }
    }
    if (trace_at >= 0) {
        g_gl_trace_on = (long)n == trace_at;
        if (g_gl_trace_on) LOG("[gltrace] ---- frame %llu ----\n", (unsigned long long)n);
    }
    c->x[0] = 1;
}

// GL names -> thunk addresses (the game loads every name it knows once, ~1500 of them)
static int thunk_cmp(const void *k, const void *e) { return strcmp((const char *)k, ((const GlThunk *)e)->name); }
HLE_FUNC("eglGetProcAddress", egl_get_proc_address) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    const GlThunk *t = name ? bsearch(name, gl_thunks, gl_thunk_count, sizeof *gl_thunks, thunk_cmp) : NULL;
    if (!t) {
        static int n;
        if (n++ < 32) LOG("[egl] eglGetProcAddress(%s): unknown\n", name ? name : "(null)");
        c->x[0] = 0;
        return;
    }
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    extern HostFn gl_fixup(const char *name);  // (gl_fixups.c)
    HostFn fix = gl_fixup(t->name);
    pthread_mutex_lock(&m);
    c->x[0] = hle_register(t->name, fix ? fix : t->fn);
    pthread_mutex_unlock(&m);
}

// frames shown so far (runtime/dispatch.c's stats)
uint64_t renderer_present_count(void) { return g_swaps; }
