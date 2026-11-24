// The game's "framebuffer 0". It draws a fixed 1920x1080 image (the size of its vi layer) into the default
// framebuffer, so a window of any other size showed a corner of it. Each game context gets an offscreen framebuffer of
// that size instead: binding framebuffer 0 binds it (and queries of the binding say 0), and eglSwapBuffers scales it
// into the window, letterboxed to 16:9. Framebuffer objects aren't shared between contexts, hence one per context.
#include <pthread.h>

#include "gl_host.h"

extern SDL_Window *g_window;

#define MAX_CTX 16
static GLuint g_fb[MAX_CTX + 1], g_rb[MAX_CTX + 1][2];
static int g_w = 1920, g_h = 1080;
static __thread int t_ctx;  // the context current on this thread (0: none)

static PFNGLBINDFRAMEBUFFERPROC p_bind_fb;
static PFNGLGENFRAMEBUFFERSPROC p_gen_fb;
static PFNGLGENRENDERBUFFERSPROC p_gen_rb;
static PFNGLBINDRENDERBUFFERPROC p_bind_rb;
static PFNGLRENDERBUFFERSTORAGEPROC p_rb_storage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC p_fb_rb;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_check;
static PFNGLBLITFRAMEBUFFERPROC p_blit;
static PFNGLDRAWBUFFERSPROC p_draw_buffers;

static void load(void) {
    if (p_bind_fb) return;
    p_bind_fb = (PFNGLBINDFRAMEBUFFERPROC)gl_proc("glBindFramebuffer");
    p_gen_fb = (PFNGLGENFRAMEBUFFERSPROC)gl_proc("glGenFramebuffers");
    p_gen_rb = (PFNGLGENRENDERBUFFERSPROC)gl_proc("glGenRenderbuffers");
    p_bind_rb = (PFNGLBINDRENDERBUFFERPROC)gl_proc("glBindRenderbuffer");
    p_rb_storage = (PFNGLRENDERBUFFERSTORAGEPROC)gl_proc("glRenderbufferStorage");
    p_fb_rb = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)gl_proc("glFramebufferRenderbuffer");
    p_check = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)gl_proc("glCheckFramebufferStatus");
    p_blit = (PFNGLBLITFRAMEBUFFERPROC)gl_proc("glBlitFramebuffer");
    p_draw_buffers = (PFNGLDRAWBUFFERSPROC)gl_proc("glDrawBuffers");
}

bool fb0_enabled(void) {
    static int on = -1;
    if (on < 0) on = !getenv("YKW_DIRECT_FB0");
    return on;
}

// (vi CreateLayer) the size the game draws at
void fb0_set_size(int w, int h) {
    if (w > 0 && h > 0) g_w = w, g_h = h;
}

GLuint fb0_current(void) { return t_ctx > 0 && t_ctx <= MAX_CTX ? g_fb[t_ctx] : 0; }

// eglMakeCurrent: the new context's framebuffer, made the first time (left bound, as the default one was)
void fb0_make_current(int id) {
    t_ctx = id;
    if (!fb0_enabled() || id <= 0 || id > MAX_CTX) return;
    load();
    if (!g_fb[id]) {
        p_gen_fb(1, &g_fb[id]);
        p_gen_rb(2, g_rb[id]);
        p_bind_rb(GL_RENDERBUFFER, g_rb[id][0]);
        p_rb_storage(GL_RENDERBUFFER, GL_RGBA8, g_w, g_h);
        p_bind_rb(GL_RENDERBUFFER, g_rb[id][1]);
        p_rb_storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_w, g_h);
        p_bind_rb(GL_RENDERBUFFER, 0);
        p_bind_fb(GL_FRAMEBUFFER, g_fb[id]);
        p_fb_rb(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_rb[id][0]);
        p_fb_rb(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_rb[id][1]);
        GLenum st = p_check(GL_FRAMEBUFFER);
        LOG("[gl] context %d: framebuffer 0 is an offscreen %dx%d one (status %#x)\n", id, g_w, g_h, st);
    }
    p_bind_fb(GL_FRAMEBUFFER, g_fb[id]);
}

// The letterboxed rectangle the image fills in a window of w x h pixels.
void fb0_dest_rect(int w, int h, int *x0, int *y0, int *x1, int *y1) {
    int dw = w, dh = (int)((int64_t)w * g_h / g_w);
    if (dh > h) dh = h, dw = (int)((int64_t)h * g_w / g_h);
    *x0 = (w - dw) / 2, *y0 = (h - dh) / 2, *x1 = *x0 + dw, *y1 = *y0 + dh;
}

// eglSwapBuffers, before the swap: the image into the window. The game's bindings are put back afterwards.
void fb0_present(void) {
    GLuint fb = fb0_current();
    if (!fb) return;
    int w, h, x0, y0, x1, y1;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    fb0_dest_rect(w, h, &x0, &y0, &x1, &y1);
    GLint draw_fb, read_fb, scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean mask[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    p_bind_fb(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);  // (the bars)
    p_bind_fb(GL_READ_FRAMEBUFFER, fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    p_blit(0, 0, g_w, g_h, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_bind_fb(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    p_bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// (screenshots) the game's image: binds it for reading and gives its size
bool fb0_read_bind(int *w, int *h) {
    GLuint fb = fb0_current();
    if (!fb) return false;
    p_bind_fb(GL_READ_FRAMEBUFFER, fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    *w = g_w, *h = g_h;
    return true;
}

// ---- the game's calls (gl_fixups.c registers these)
void fb0_fix_bind_framebuffer(Cpu *c) {
    GL_TRACE("glBindFramebuffer", c);
    load();
    GLuint name = (GLuint)c->x[1];
    p_bind_fb((GLenum)c->x[0], name ? name : fb0_current());
}

static GLenum map_buffer(GLenum b) {  // (default-framebuffer names, on the offscreen one)
    return b == GL_BACK || b == GL_BACK_LEFT || b == GL_FRONT || b == GL_FRONT_LEFT ? GL_COLOR_ATTACHMENT0 : b;
}
static bool bound_is_fb0(GLenum which) {
    GLint cur;
    glGetIntegerv(which, &cur);
    return fb0_current() && (GLuint)cur == fb0_current();
}

void fb0_fix_draw_buffers(Cpu *c) {
    GL_TRACE("glDrawBuffers", c);
    load();
    GLsizei n = (GLsizei)c->x[0];
    const GLenum *bufs = (const GLenum *)(uintptr_t)c->x[1];
    if (n > 0 && n <= 8 && bufs && bound_is_fb0(GL_DRAW_FRAMEBUFFER_BINDING)) {
        GLenum m[8];
        for (int i = 0; i < n; i++) m[i] = map_buffer(bufs[i]);
        p_draw_buffers(n, m);
        return;
    }
    p_draw_buffers(n, bufs);
}

void fb0_fix_draw_buffer(Cpu *c) {
    GL_TRACE("glDrawBuffer", c);
    GLenum b = (GLenum)c->x[0];
    glDrawBuffer(bound_is_fb0(GL_DRAW_FRAMEBUFFER_BINDING) ? map_buffer(b) : b);
}

void fb0_fix_read_buffer(Cpu *c) {
    GL_TRACE("glReadBuffer", c);
    GLenum b = (GLenum)c->x[0];
    glReadBuffer(bound_is_fb0(GL_READ_FRAMEBUFFER_BINDING) ? map_buffer(b) : b);
}

// queries of the binding: the offscreen framebuffer is "0" to the game
void fb0_fix_get_integerv(Cpu *c) {
    GL_TRACE("glGetIntegerv", c);
    GLenum pname = (GLenum)c->x[0];
    GLint *out = (GLint *)(uintptr_t)c->x[1];
    glGetIntegerv(pname, out);
    if (out && (pname == GL_DRAW_FRAMEBUFFER_BINDING || pname == GL_READ_FRAMEBUFFER_BINDING) && fb0_current() &&
        (GLuint)out[0] == fb0_current())
        out[0] = 0;
}
