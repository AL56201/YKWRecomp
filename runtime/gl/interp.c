// Frame interpolation: images between the game's own frames, so the display can show more than the game's logic rate
// (60 steps a second: hle/framerate.c) without the game running faster.
//
// The game draws each logic step once. Every GL call of a frame passes through the thunks (gl_thunks.c) and is recorded
// here: which function, its registers and stack arguments, and copies of what its input pointers point at (uniform
// arrays, buffer uploads...). An in-between image is the frame's calls played again with the values that differ from
// the previous frame - uniforms, constant vertex attributes - blended between the two (as RT64 and LAS Recomp do).
//
// Replays have no side effects on the game: calls that write its memory (glGet*, glGen*) are skipped, and objects the
// frame deletes are deleted only after the frame's replays (the replays bind them by name). A frame with a call that
// can't be replayed (an input pointer without a size rule, a buffer mapping) shows no in-betweens.
//
//   YKW_INTERP=1                  record and replay (the setting interp = 1)
//   YKW_INTERP_VERIFY=<frame>     at that frame: replay it unchanged and compare the image with the game's own
//   YKW_INTERP_LOG=1              per-frame stats every 5 s
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "gl_host.h"
#include "../settings.h"

volatile int g_gl_hook_on;
static int g_replaying;

// ---- recording
typedef struct {
    uint16_t id;
    uint16_t flags;       // REC_*
    uint32_t payload[4];  // offsets into the arena for up to 4 input pointers (in parameter order), ~0u: none
    uint32_t psize[4];    // their sizes
    uint32_t prog;        // the program in use (glUseProgram), for matching uniforms across frames
    int32_t match;        // blendable: the same value's call in the previous frame (-1: none)
    uint64_t key;         // blendable: (function, program, location, count), hashed
    uint32_t occ;         // which call of this key in the frame
    HostFn fn;            // what the game called (a thunk, or a fixup: gl_fixups.c)
    uint64_t x[8];
    uint64_t v[8];        // low 64 bits of v0-v7
    uint64_t stack[6];
} Rec;
enum { REC_SKIP = 1, REC_DELETE = 2, REC_BLEND_V = 4, REC_BLEND_P = 8 };  // BLEND_V: floats in v0-v3, _P: a float array

typedef struct {
    Rec *recs;
    uint32_t n, cap;
    uint8_t *arena;
    size_t used, acap;
    bool bad;             // a call that can't be replayed
    char bad_name[64];
} Frame;

static Frame g_frames[2];
static int g_cur;  // the frame being recorded; g_cur ^ 1: the one before
static uint32_t g_prog;  // the program in use while recording
static uint64_t blend_key(const Rec *r);

// Which recorded calls carry values to blend: float uniforms and constant vertex attributes.
static int blend_kind(const char *name, const GlSig *s, int *loc_param) {
    bool prog = !strncmp(name, "glProgramUniform", 16);
    *loc_param = prog ? 1 : 0;
    if (!strncmp(name, "glUniform", 9) || prog) {
        if (strstr(name, "Block") || strstr(name, "Subroutine") || strstr(name, "Handle")) return 0;
        size_t l = strlen(name);
        if (name[l - 1] == 'v' && name[l - 2] == 'f') return REC_BLEND_P;          // ...fv
        if (name[l - 1] == 'f' && !strstr(name, "Matrix")) return REC_BLEND_V;    // ...1f - 4f
        return 0;
    }
    if (!strncmp(name, "glVertexAttrib", 14) && strchr("1234", name[14])) {
        *loc_param = 0;
        size_t l = strlen(name);
        if (!strcmp(name + 15, "fv")) return REC_BLEND_P;
        if (name[15] == 'f' && l == 16) return REC_BLEND_V;
    }
    (void)s;
    return 0;
}

static uint32_t arena_put(Frame *f, const void *src, size_t n) {
    if (f->used + n + 16 > f->acap) {
        size_t c = f->acap ? f->acap * 2 : 1 << 22;
        while (c < f->used + n + 16) c *= 2;
        f->arena = realloc(f->arena, c);
        f->acap = c;
    }
    uint32_t off = (uint32_t)f->used;
    memcpy(f->arena + off, src, n);
    f->used = (f->used + n + 15) & ~(size_t)15;
    return off;
}

static uint64_t arg_of(const Cpu *c, GlParam p) {
    if (p.loc < 8) return c->x[p.loc];
    if (p.loc < 16) return (uint64_t)c->v[p.loc - 8];
    return *(const uint64_t *)(uintptr_t)(c->sp + 8ull * (p.loc - 16));
}

// Bytes an input pointer of call `name` points at (0: none to copy; -1: unknown). k: which input pointer.
static long payload_size(const char *name, const Cpu *c, const GlSig *s, int pi) {
    (void)pi;
    uint64_t a0 = arg_of(c, s->p[0]), a1 = s->n > 1 ? arg_of(c, s->p[1]) : 0;
    const char *u = strstr(name, "Uniform");
    if (u && !strstr(name, "Block") && !strstr(name, "Subroutine")) {
        bool program = !strncmp(name, "glProgramUniform", 16);
        uint64_t count = program ? arg_of(c, s->p[2]) : a1;
        const char *m = strstr(name, "Matrix");
        int elem = strstr(name, "dv") ? 8 : 4;
        if (m) {
            int r = m[6] - '0', cc = r;
            if (m[7] == 'x') cc = m[8] - '0';
            return (long)(count * r * cc * elem);
        }
        const char *d = u + 7;
        int comps = *d >= '1' && *d <= '4' ? *d - '0' : 1;
        return (long)(count * comps * elem);
    }
    if (!strncmp(name, "glVertexAttrib", 14) && strchr("1234", name[14]) && name[strlen(name) - 1] == 'v') {
        int comps = name[14] - '0';
        const char *t = name + 15;
        if (*t == 'N') t++;
        int elem = !strncmp(t, "dv", 2) ? 8 : !strncmp(t, "fv", 2) || !strncmp(t, "iv", 2) || !strncmp(t, "uiv", 3) ? 4
                 : !strncmp(t, "sv", 2) || !strncmp(t, "usv", 3) ? 2 : 1;
        return comps * elem;
    }
    if (!strncmp(name, "glClearBuffer", 13)) return a0 == GL_COLOR ? 16 : 4;
    if (!strcmp(name, "glDrawBuffers")) return (long)(a0 * 4);
    if (!strcmp(name, "glBufferData")) return (long)a1;
    if (!strcmp(name, "glBufferSubData")) return (long)arg_of(c, s->p[2]);
    if (!strncmp(name, "glTexParameter", 14) || !strncmp(name, "glSamplerParameter", 18)) return 16;
    if (!strncmp(name, "glDelete", 8) && strstr(name, "s") && s->n == 2) return (long)(a0 * 4);
    return -1;
}

static bool is_delete(const char *name, const GlSig *s) {
    return !strncmp(name, "glDelete", 8) && s->n == 2 && s->p[0].k == 'i' && s->p[1].k == 'P';
}

static void record(int id, HostFn fn, Cpu *c) {
    Frame *f = &g_frames[g_cur];
    const GlSig *s = &gl_sigs[id];
    const char *name = gl_thunks[id].name;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4096;
        f->recs = realloc(f->recs, sizeof *f->recs * f->cap);
    }
    Rec *r = &f->recs[f->n++];
    memset(r, 0, sizeof *r);
    r->id = (uint16_t)id, r->fn = fn;
    memcpy(r->x, c->x, sizeof r->x);
    for (int i = 0; i < 8; i++) r->v[i] = (uint64_t)c->v[i];
    memcpy(r->stack, (const void *)(uintptr_t)c->sp, sizeof r->stack);
    for (int i = 0; i < 4; i++) r->payload[i] = ~0u;
    r->match = -1;
    if (!strcmp(name, "glUseProgram")) g_prog = (uint32_t)c->x[0];
    r->prog = g_prog;
    int lp;
    r->flags |= (uint16_t)blend_kind(name, s, &lp);
    if (s->ret == 'p') r->flags |= REC_SKIP;
    int pk = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->p[i].k == 'p') r->flags |= REC_SKIP;  // writes the game's memory: glGet*, glGen*
        if (s->p[i].k != 'P') continue;
        uint64_t ptr = arg_of(c, s->p[i]);
        if (ptr < 0x100000) { pk++; continue; }  // (an offset into a bound buffer, or none)
        long size = payload_size(name, c, s, pk);
        if (size < 0 || size > (64 << 20) || pk >= 4) {
            if (!f->bad) snprintf(f->bad_name, sizeof f->bad_name, "%s", name);
            f->bad = true;
            pk++;
            continue;
        }
        r->psize[pk] = (uint32_t)size;
        r->payload[pk++] = arena_put(f, (const void *)(uintptr_t)ptr, (size_t)size);
    }
    if (is_delete(name, s)) r->flags |= REC_DELETE | REC_SKIP;
    if (r->flags & (REC_BLEND_V | REC_BLEND_P)) r->key = blend_key(r);
}

// ---- the hook (every forwarded call when interpolation is on)
static int fixup_id(const char *name) {
    for (int i = 0; i < gl_thunk_count; i++)
        if (!strcmp(gl_thunks[i].name, name)) return i;
    return -1;
}

// (debug) YKW_CAM_UPLOADS=1: every upload of the camera's view matrix (unf_vtx_glb_cmr, as glUniform4fv) is logged
// with the test tick and its translation - what the game sends, with or without recording (g_cam_probe_only: the hook
// runs for this alone, nothing is recorded)
int g_cam_probe_only;
static const char *uniform_name(uint32_t prog, int loc);
static void cam_upload_probe(int id, Cpu *c) {
    static int uni4fv = -2;
    if (uni4fv == -2) {
        uni4fv = -1;
        for (int i = 0; i < gl_thunk_count; i++) if (!strcmp(gl_thunks[i].name, "glUniform4fv")) uni4fv = i;
    }
    if (id == uni4fv - 0 && id >= 0) {
        static int8_t is_cam[1024][256];  // [program][location]: 0 unknown, 1 yes, -1 no
        uint32_t p = g_prog;
        int loc = (int)c->x[0];
        if (p < 1024 && loc >= 0 && loc < 256) {
            if (!is_cam[p][loc]) is_cam[p][loc] = strstr(uniform_name(p, loc), "glb_cmr[0]") ? 1 : -1;
            if (is_cam[p][loc] == 1 && c->x[1] == 3) {
                const float *m = (const float *)(uintptr_t)c->x[2];
                extern uint64_t ykw_test_tick(void);
                static uint64_t frames_seen;
                LOG("[camup] tick %llu prog %u translation %.3f %.3f %.3f from %#llx (lr %#llx)\n",
                    (unsigned long long)ykw_test_tick(), p, m[3], m[7], m[11], (unsigned long long)c->x[2],
                    (unsigned long long)c->x[30]);
                (void)frames_seen;
            }
        }
    }
}

bool gl_hook(int id, Cpu *c) {
    if (g_replaying) return false;
    if (g_cam_probe_only) {
        if (!strcmp(gl_thunks[id].name, "glUseProgram")) g_prog = (uint32_t)c->x[0];
        cam_upload_probe(id, c);
        return false;
    }
    static int cam_log = -1;
    if (cam_log < 0) cam_log = getenv("YKW_CAM_UPLOADS") != NULL;
    if (cam_log) cam_upload_probe(id, c);
    record(id, gl_thunks[id].fn, c);
    return (g_frames[g_cur].recs[g_frames[g_cur].n - 1].flags & REC_DELETE) != 0;  // (deleted after the replays)
}

// (gl_fixups.c / fb0.c) a fixup's call, under the thunk's name
bool gl_hook_fixup(const char *name, HostFn fn, Cpu *c) {
    if (!g_gl_hook_on || g_replaying) return false;
    static struct { const char *name; int id; } cache[16];
    int id = -1;
    for (int i = 0; i < 16 && cache[i].name; i++) if (cache[i].name == name) id = cache[i].id;
    if (id < 0) {
        id = fixup_id(name);
        for (int i = 0; i < 16; i++) if (!cache[i].name) { cache[i].name = name, cache[i].id = id; break; }
    }
    if (id < 0) return false;
    record(id, fn, c);
    return false;
}

// ---- matching: each blendable call of a frame to the same value's call in the frame before, by (function, program,
// location, count) and which occurrence of that it is in the frame
static uint64_t blend_key(const Rec *r) {
    const char *name = gl_thunks[r->id].name;
    int lp;
    blend_kind(name, &gl_sigs[r->id], &lp);
    uint64_t loc = r->x[lp], count = (r->flags & REC_BLEND_P) && name[2] == 'U' ? r->x[lp + 1] : 0;
    if (!strncmp(name, "glProgramUniform", 16)) count = (r->flags & REC_BLEND_P) ? r->x[2] : 0;
    uint64_t h = 1469598103934665603ull;
    uint64_t parts[4] = {r->id, r->prog, loc, count};
    for (int i = 0; i < 4; i++) h = (h ^ parts[i]) * 1099511628211ull;
    return h | 1;
}

typedef struct { uint64_t key; uint32_t occ, rec; } KeyEnt;
static KeyEnt *g_tab;
static uint32_t g_tab_cap;

static uint32_t slot_of(uint64_t key, uint32_t occ, uint32_t mask) {
    return (uint32_t)((key ^ (occ * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull >> 32) & mask;
}

// (each frame once, at its end) which occurrence of its key each blendable call is: a table of counts per key
static void assign_occ(Frame *f) {
    uint32_t need = 1;
    while (need < f->n * 2 + 16) need <<= 1;
    if (need > g_tab_cap) g_tab = realloc(g_tab, sizeof *g_tab * need), g_tab_cap = need;
    memset(g_tab, 0, sizeof *g_tab * need);
    for (uint32_t k = 0; k < f->n; k++) {
        Rec *r = &f->recs[k];
        if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
        for (uint32_t i = slot_of(r->key, 0, need - 1);; i = (i + 1) & (need - 1)) {
            if (!g_tab[i].key) { g_tab[i] = (KeyEnt){r->key, 1, 0}; r->occ = 0; break; }
            if (g_tab[i].key == r->key) { r->occ = g_tab[i].occ++; break; }
        }
    }
}

static void match_by_order(Frame *cur, Frame *prev, int *matched, int *blendable) {
    *matched = *blendable = 0;
    uint32_t need = 1;
    while (need < prev->n * 2 + 16) need <<= 1;
    if (need > g_tab_cap) g_tab = realloc(g_tab, sizeof *g_tab * need), g_tab_cap = need;
    memset(g_tab, 0, sizeof *g_tab * need);
    for (uint32_t k = 0; k < prev->n; k++) {  // the previous frame's (key, occ) -> call
        Rec *r = &prev->recs[k];
        if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
        uint32_t i = slot_of(r->key, r->occ, need - 1);
        while (g_tab[i].key) i = (i + 1) & (need - 1);
        g_tab[i] = (KeyEnt){r->key, r->occ, k};
    }
    // how many of each key the frame before had (its highest occurrence + 1), and this frame: a key uploaded a
    // different number of times has an extra or a missing draw somewhere, and its order can't be trusted (the
    // minimap, redrawn about once a second, put a zoomed-in map in that second's in-betweens). Those calls count as
    // matched (no cut for it) but aren't blended, unless the draw pairing (match_frames) finds their own.
    static uint64_t *cnt_key;
    static uint32_t *cnt_prev, *cnt_cur, cnt_cap;
    uint32_t cneed = 1;
    while (cneed < (prev->n + cur->n) * 2 + 16) cneed <<= 1;
    if (cneed > cnt_cap) {
        cnt_cap = cneed;
        cnt_key = realloc(cnt_key, sizeof *cnt_key * cneed);
        cnt_prev = realloc(cnt_prev, sizeof *cnt_prev * cneed), cnt_cur = realloc(cnt_cur, sizeof *cnt_cur * cneed);
    }
    memset(cnt_key, 0, sizeof *cnt_key * cneed);
    for (int fr = 0; fr < 2; fr++) {
        Frame *f = fr ? cur : prev;
        for (uint32_t k = 0; k < f->n; k++) {
            const Rec *r = &f->recs[k];
            if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
            uint32_t i = slot_of(r->key, 0, cneed - 1);
            while (cnt_key[i] && cnt_key[i] != r->key) i = (i + 1) & (cneed - 1);
            if (!cnt_key[i]) cnt_key[i] = r->key, cnt_prev[i] = cnt_cur[i] = 0;
            uint32_t *c = fr ? &cnt_cur[i] : &cnt_prev[i];
            if (r->occ + 1 > *c) *c = r->occ + 1;
        }
    }
    for (uint32_t k = 0; k < cur->n; k++) {
        Rec *r = &cur->recs[k];
        r->match = -1;
        if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
        (*blendable)++;
        uint32_t ci = slot_of(r->key, 0, cneed - 1);
        while (cnt_key[ci] != r->key) ci = (ci + 1) & (cneed - 1);
        bool same_count = cnt_prev[ci] == cnt_cur[ci];
        for (uint32_t i = slot_of(r->key, r->occ, need - 1); g_tab[i].key; i = (i + 1) & (need - 1))
            if (g_tab[i].key == r->key && g_tab[i].occ == r->occ) {
                const Rec *p = &prev->recs[g_tab[i].rec];
                if ((r->flags & REC_BLEND_V) || p->psize[0] == r->psize[0]) {
                    (*matched)++;
                    if (same_count) r->match = (int32_t)g_tab[i].rec;
                }
                break;
            }
    }
}

// By order alone, the n-th upload of a key pairs with the n-th the frame before - but the game sorts its draws (by
// depth, as the camera moves), and objects drawn with the same program, such as a row of bushes, swapped places in
// that order from one frame to the next: in-betweens blended one bush's bones with another's and drew it out of
// place, a flicker in every in-between while the camera moved up or down (Uptown, by Triangle Park). So the frame is
// cut into draws (the calls up to and including a glDraw*), draws of the same call and arguments (the same mesh) are
// paired with the nearest by their uniform values (objects move little in a frame), and each upload blends with its
// own in the paired draw. Uploads outside such a pairing keep the order match. YKW_INTERP_ORDER_MATCH=1: no draw
// pairing (the order match, with its count check, only). Measured walking up and down by Triangle Park: before these
// two, 2-8 in-betweens a run stood out from both neighbours (whole image, by 10-25); after, none in 8 runs.
typedef struct { uint32_t first, end; uint64_t sig; int32_t match; float fp[16]; } Seg;
static Seg *g_segs[2];
static uint32_t g_seg_cap[2];
static const float *rec_floats(const Frame *f, const Rec *r, float tmp[4], size_t *n);

static uint32_t cut_segments(Frame *f, int which) {
    uint32_t n = 0, first = 0;
    for (uint32_t k = 0; k < f->n; k++) {
        const Rec *r = &f->recs[k];
        if (strncmp(gl_thunks[r->id].name, "glDraw", 6)) continue;
        if (n == g_seg_cap[which]) g_segs[which] = realloc(g_segs[which], sizeof(Seg) * (g_seg_cap[which] = g_seg_cap[which] * 2 + 64));
        uint64_t h = 1469598103934665603ull, parts[6] = {r->id, r->prog, r->x[0], r->x[1], r->x[2], r->x[3]};
        for (int i = 0; i < 6; i++) h = (h ^ parts[i]) * 1099511628211ull;
        Seg *s = &g_segs[which][n++];
        *s = (Seg){first, k + 1, h, -1, {0}};
        // the draw's fingerprint: its uniform values (the first 64 of each upload) folded into 16 sums, in order
        uint32_t j = 0;
        for (uint32_t q = first; q <= k; q++) {
            const Rec *u = &f->recs[q];
            if (!(u->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
            float tmp[4];
            size_t cnt;
            const float *v = rec_floats(f, u, tmp, &cnt);
            for (size_t e = 0; e < cnt && e < 64; e++, j++)
                if (!isnan(v[e])) s->fp[j & 15] += v[e];
        }
        first = k + 1;
    }
    return n;
}

// the call in segment s of frame f with this key, as the occ-th of its key in the segment; -1: none
static int32_t seg_find(const Frame *f, const Seg *s, uint64_t key, int occ) {
    for (uint32_t k = s->first; k < s->end; k++)
        if (f->recs[k].key == key && (f->recs[k].flags & (REC_BLEND_V | REC_BLEND_P)) && occ-- == 0) return (int32_t)k;
    return -1;
}

static const float *rec_floats(const Frame *f, const Rec *r, float tmp[4], size_t *n) {
    if (r->flags & REC_BLEND_P) { *n = r->payload[0] == ~0u ? 0 : r->psize[0] / 4; return (const float *)(f->arena + r->payload[0]); }
    for (int i = 0; i < 4; i++) { uint32_t u = (uint32_t)r->v[i]; memcpy(&tmp[i], &u, 4); }
    *n = 4;
    return tmp;
}

// how far apart two draws' uniform values are: their fingerprints (cut_segments; comparing every value of every
// pair cost ~10 fps outdoors)
static double seg_cost(const Seg *a, const Seg *b) {
    double cost = 0;
    for (int i = 0; i < 16; i++) cost += fabsf(a->fp[i] - b->fp[i]);
    return cost;
}

static void match_frames(Frame *cur, Frame *prev, int *matched, int *blendable) {
    match_by_order(cur, prev, matched, blendable);
    if (getenv("YKW_INTERP_ORDER_MATCH")) return;
    uint32_t nc = cut_segments(cur, 0), np = cut_segments(prev, 1);
    Seg *sc = g_segs[0], *sp = g_segs[1];
    static bool *taken;
    static uint32_t taken_cap;
    if (np > taken_cap) taken = realloc(taken, (taken_cap = np * 2));
    memset(taken, 0, np);
    // each group of draws with the same signature: greedy nearest pairs (groups past 48 draws keep the order match)
    for (uint32_t i = 0; i < nc; i++) {
        if (sc[i].match != -1 || sc[i].sig == 0) continue;
        uint64_t sig = sc[i].sig;
        uint32_t gc[48], gp[48], ncg = 0, npg = 0;
        bool big = false;
        for (uint32_t k = i; k < nc; k++) if (sc[k].sig == sig) { if (ncg == 48) { big = true; break; } gc[ncg++] = k; }
        for (uint32_t k = 0; k < np && !big; k++) if (sp[k].sig == sig) { if (npg == 48) { big = true; break; } gp[npg++] = k; }
        for (uint32_t k = i; k < nc; k++) if (sc[k].sig == sig) sc[k].match = -2;  // (this group is done)
        if (big || ncg < 2 || !npg) continue;
        static double cost[48][48];
        for (uint32_t a = 0; a < ncg; a++)
            for (uint32_t b = 0; b < npg; b++) cost[a][b] = seg_cost(&sc[gc[a]], &sp[gp[b]]);
        bool done_c[48] = {0}, done_p[48] = {0};
        for (uint32_t round = 0; round < ncg && round < npg; round++) {
            double best = 1e300;
            int ba = -1, bb = -1;
            for (uint32_t a = 0; a < ncg; a++)
                for (uint32_t b = 0; b < npg; b++)
                    if (!done_c[a] && !done_p[b] && cost[a][b] < best) best = cost[a][b], ba = (int)a, bb = (int)b;
            if (ba < 0 || best >= 1e6) break;
            done_c[ba] = done_p[bb] = true;
            sc[gc[ba]].match = (int32_t)gp[bb];
        }
    }
    // the uploads of paired draws: each to its own in the paired draw
    for (uint32_t i = 0; i < nc; i++) {
        if (sc[i].match < 0) continue;
        const Seg *b = &sp[sc[i].match];
        for (uint32_t k = sc[i].first; k < sc[i].end; k++) {
            Rec *r = &cur->recs[k];
            if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) continue;
            int occ = 0;
            for (uint32_t j = sc[i].first; j < k; j++) occ += cur->recs[j].key == r->key && (cur->recs[j].flags & (REC_BLEND_V | REC_BLEND_P));
            int32_t q = seg_find(prev, b, r->key, occ);
            if (q < 0 || ((r->flags & REC_BLEND_P) && prev->recs[q].psize[0] != r->psize[0])) continue;
            if (r->match < 0) (*matched)++;
            r->match = q;
        }
    }
}

// ---- replay, at t in (0, 1]: blendable values between the previous frame's (t = 0) and this frame's (t = 1)
static float g_cut = 4.0f;  // a change past this (+5% of the value) in one frame is a jump: the new value at once

static bool jumps(float a, float b) {
    float d = fabsf(b - a), m = fmaxf(fabsf(a), fabsf(b));
    return d > g_cut + 0.05f * m || isnan(d);
}

static float *g_tmp;
static size_t g_tmp_cap;

// (debug) YKW_INTERP_NOBLEND=<program>,<program>...: those programs' values aren't blended (the frame's own)
static bool noblend(uint32_t prog) {
    static uint32_t list[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("YKW_INTERP_NOBLEND");
        for (; e && *e && n < 32; e++) {
            list[n++] = (uint32_t)strtoul(e, (char **)&e, 10);
            if (!*e) break;
        }
    }
    for (int i = 0; i < n; i++)
        if (list[i] == prog) return true;
    return false;
}

static void replay_t(Frame *f, Frame *prev, float t) {
    uint64_t stack[16];
    Cpu c;
    bool blend_all = prev && t < 0.999f;
    g_replaying = 1;
    for (uint32_t k = 0; k < f->n; k++) {
        const Rec *r = &f->recs[k];
        if (r->flags & REC_SKIP) continue;
        const GlSig *s = &gl_sigs[r->id];
        bool blend = blend_all && !noblend(r->prog);
        memset(&c, 0, sizeof c);
        memcpy(c.x, r->x, sizeof r->x);
        for (int i = 0; i < 8; i++) c.v[i] = r->v[i];
        memcpy(stack, r->stack, sizeof r->stack);
        c.sp = (uint64_t)(uintptr_t)stack;
        int pk = 0;
        for (int i = 0; i < s->n && pk < 4; i++) {
            if (s->p[i].k != 'P') continue;
            uint32_t off = r->payload[pk++];
            if (off == ~0u) continue;
            uint64_t p = (uint64_t)(uintptr_t)(f->arena + off);
            if (blend && pk == 1 && (r->flags & REC_BLEND_P) && r->match >= 0) {  // a float array, blended
                const Rec *q = &prev->recs[r->match];
                const float *a = (const float *)(prev->arena + q->payload[0]), *b = (const float *)(f->arena + off);
                size_t n = r->psize[0] / 4;
                if (n > g_tmp_cap) g_tmp = realloc(g_tmp, (g_tmp_cap = n * 2) * 4);
                bool jump = false;
                for (size_t e = 0; e < n && !jump; e++) jump = jumps(a[e], b[e]);
                if (!jump) {
                    for (size_t e = 0; e < n; e++) g_tmp[e] = a[e] + (b[e] - a[e]) * t;
                    p = (uint64_t)(uintptr_t)g_tmp;
                }
            }
            if (s->p[i].loc < 8) c.x[s->p[i].loc] = p;
            else if (s->p[i].loc >= 16) stack[s->p[i].loc - 16] = p;
        }
        if (blend && (r->flags & REC_BLEND_V) && r->match >= 0) {  // floats in v0-v3, blended
            const Rec *q = &prev->recs[r->match];
            float a[4], b[4];
            bool jump = false;
            for (int i = 0; i < 4; i++) {
                uint32_t ua = (uint32_t)q->v[i], ub = (uint32_t)r->v[i];
                memcpy(&a[i], &ua, 4), memcpy(&b[i], &ub, 4);
                jump |= jumps(a[i], b[i]);
            }
            if (!jump)
                for (int i = 0; i < 4; i++) {
                    float m = a[i] + (b[i] - a[i]) * t;
                    uint32_t u;
                    memcpy(&u, &m, 4);
                    c.v[i] = u;
                }
        }
        r->fn(&c);
    }
    g_replaying = 0;
}

static void replay(Frame *f) { replay_t(f, NULL, 1.0f); }

// the frame's deletions, held back for its replays
static void flush_deletes(Frame *f) {
    for (uint32_t k = 0; k < f->n; k++) {
        const Rec *r = &f->recs[k];
        if (!(r->flags & REC_DELETE)) continue;
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[0] = r->x[0];
        c.x[1] = r->payload[0] != ~0u ? (uint64_t)(uintptr_t)(f->arena + r->payload[0]) : 0;
        g_replaying = 1;
        r->fn(&c);
        g_replaying = 0;
    }
}

static void frame_reset(Frame *f) {
    f->n = 0, f->used = 0, f->bad = false, f->bad_name[0] = 0;
}

// ---- verification: replay a frame unchanged and compare its image with the game's own
extern GLuint fb0_current(void);
extern bool fb0_read_bind(int *w, int *h);

static uint8_t *read_image(int *w, int *h) {
    if (!fb0_read_bind(w, h)) return NULL;
    uint8_t *px = malloc((size_t)*w * *h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px;
}

static void verify(Frame *f, uint64_t frame) {
    static PFNGLBINDFRAMEBUFFERPROC bind_fb;
    if (!bind_fb) bind_fb = (PFNGLBINDFRAMEBUFFERPROC)gl_proc("glBindFramebuffer");
    GLint draw_fb, read_fb;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    int w, h;
    uint8_t *a = read_image(&w, &h);
    bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    if (!a) return;
    // (the game's image wiped first: only a replay that draws all of it again can match)
    bind_fb(GL_DRAW_FRAMEBUFFER, fb0_current());
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLint scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    bind_fb(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    uint64_t t0 = plat_now_ns();
    replay(f);
    glFinish();
    uint64_t t1 = plat_now_ns();
    uint8_t *b = read_image(&w, &h);
    bind_fb(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    uint64_t diff = 0, maxd = 0;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        if (d) diff++;
        if ((uint64_t)d > maxd) maxd = d;
    }
    LOG("[interp] verify frame %llu: %u calls, %zu KB recorded, replay %.2f ms: %llu of %d bytes differ (max %llu)%s%s\n",
        (unsigned long long)frame, f->n, f->used >> 10, (t1 - t0) / 1e6, (unsigned long long)diff, w * h * 4,
        (unsigned long long)maxd, f->bad ? ", not replayable: " : "", f->bad ? f->bad_name : "");
    const char *dir = getenv("YKW_SHOT_DIR");
    if (dir && *dir) {  // (both images, to look at)
        for (int k = 0; k < 2; k++) {
            uint8_t *px = k ? b : a;
            SDL_Surface *s = SDL_CreateSurface(w / 2, h / 2, SDL_PIXELFORMAT_ABGR8888);
            for (int y = 0; y < h / 2; y++) {
                uint32_t *dst = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
                const uint32_t *src = (const uint32_t *)(px + (size_t)(h - 1 - 2 * y) * w * 4);
                for (int x = 0; x < w / 2; x++) dst[x] = src[2 * x] | 0xFF000000u;
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/verify_%llu_%s.bmp", dir, (unsigned long long)frame, k ? "replay" : "game");
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
        }
    }
    free(a), free(b);
}

// (debug) YKW_INTERP_DUMP=<frame>: that frame's blendable calls, with the uniform's name, its match in the frame before
// and both values, to YKW_SHOT_DIR/interp_dump_<frame>.txt
static const char *uniform_name(uint32_t prog, int loc) {
    static struct { uint32_t prog; int loc; char name[64]; } cache[4096];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].prog == prog && cache[i].loc == loc) return cache[i].name;
    typedef void (APIENTRY *GetActive)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
    typedef GLint (APIENTRY *GetLoc)(GLuint, const GLchar *);
    typedef void (APIENTRY *GetPiv)(GLuint, GLenum, GLint *);
    static GetActive active;
    static GetLoc getloc;
    static GetPiv getpiv;
    if (!active) {
        active = (GetActive)gl_proc("glGetActiveUniform");
        getloc = (GetLoc)gl_proc("glGetUniformLocation");
        getpiv = (GetPiv)gl_proc("glGetProgramiv");
    }
    GLint count = 0;
    getpiv(prog, GL_ACTIVE_UNIFORMS, &count);
    const char *found = "?";
    for (GLint i = 0; i < count && n < 4096; i++) {
        char nm[64];
        GLint size;
        GLenum type;
        active(prog, (GLuint)i, sizeof nm, NULL, &size, &type, nm);
        char base[64];
        snprintf(base, sizeof base, "%s", nm);
        char *br = strchr(base, '[');
        if (br) *br = 0;
        for (GLint e = 0; e < (size > 0 ? size : 1) && n < 4096; e++) {
            char q[80];
            if (size > 1) snprintf(q, sizeof q, "%s[%d]", base, e); else snprintf(q, sizeof q, "%s", base);
            GLint l = getloc(prog, q);
            if (l < 0) continue;
            cache[n].prog = prog, cache[n].loc = l;
            snprintf(cache[n].name, sizeof cache[n].name, "%s", q);
            if (l == loc) found = cache[n].name;
            n++;
        }
    }
    return found;
}

static void dump_frame(Frame *f, Frame *prev, uint64_t frame) {
    const char *dir = getenv("YKW_SHOT_DIR");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/interp_dump_%llu.txt", dir, (unsigned long long)frame);
    FILE *o = fopen(path, "w");
    if (!o) return;
    {   // every function the frame calls, with how often
        static uint32_t counts[4096];
        memset(counts, 0, sizeof counts);
        for (uint32_t k = 0; k < f->n; k++) if (f->recs[k].id < 4096) counts[f->recs[k].id]++;
        for (int i = 0; i < gl_thunk_count && i < 4096; i++)
            if (counts[i]) fprintf(o, "# %5u %s%s\n", counts[i], gl_thunks[i].name, (f->recs[0].flags, ""));
    }
    {   // every call that differs from the previous frame's call at the same position (registers or payload)
        fprintf(o, "## calls: %u this frame, %u the frame before\n", f->n, prev->n);
        uint32_t nd = 0;
        for (uint32_t k = 0; k < f->n && k < prev->n && nd < 400; k++) {
            const Rec *r = &f->recs[k], *q = &prev->recs[k];
            const char *nm = gl_thunks[r->id].name;
            if (r->id != q->id) { fprintf(o, "## %5u %s vs %s (sequence differs from here)\n", k, nm, gl_thunks[q->id].name); break; }
            const GlSig *s = &gl_sigs[r->id];
            char what[256] = "";
            int wl = 0;
            for (int i = 0; i < s->n; i++) {
                uint64_t a = s->p[i].loc < 8 ? r->x[s->p[i].loc] : s->p[i].loc < 16 ? r->v[s->p[i].loc - 8] : r->stack[s->p[i].loc - 16];
                uint64_t b = s->p[i].loc < 8 ? q->x[s->p[i].loc] : s->p[i].loc < 16 ? q->v[s->p[i].loc - 8] : q->stack[s->p[i].loc - 16];
                if (s->p[i].k == 'P' || s->p[i].k == 'p') continue;  // (pointers: the payloads are compared below)
                if (a != b && wl < 200) wl += snprintf(what + wl, sizeof what - wl, " arg%d %llx->%llx", i, (unsigned long long)b, (unsigned long long)a);
            }
            for (int p = 0; p < 4; p++) {
                if (r->payload[p] == ~0u || q->payload[p] == ~0u) continue;
                if (r->psize[p] != q->psize[p] || memcmp(f->arena + r->payload[p], prev->arena + q->payload[p], r->psize[p])) {
                    if (wl < 200) wl += snprintf(what + wl, sizeof what - wl, " payload%d (%u bytes)", p, r->psize[p]);
                }
            }
            if (wl) { fprintf(o, "## %5u %s prog %u:%s\n", k, nm, r->prog, what); nd++; }
        }
    }
    for (uint32_t k = 0; k < f->n; k++) {
        const Rec *r = &f->recs[k];
        const char *name = gl_thunks[r->id].name;
        if (!(r->flags & (REC_BLEND_V | REC_BLEND_P))) {
            if (strstr(name, "Draw") || !strcmp(name, "glUseProgram") || !strcmp(name, "glBindFramebuffer"))
                fprintf(o, "%5u %s %llx %llx %llx\n", k, name, (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                        (unsigned long long)r->x[2]);
            continue;
        }
        int lp;
        blend_kind(name, &gl_sigs[r->id], &lp);
        int loc = (int)r->x[lp];
        fprintf(o, "%5u %s prog %u loc %d (%s) occ %u match %d\n", k, name, r->prog, loc,
                name[2] == 'U' ? uniform_name(r->prog, loc) : "attrib", r->occ, r->match);
        const float *b = (r->flags & REC_BLEND_P) ? (const float *)(f->arena + r->payload[0]) : NULL;
        const float *a = (r->flags & REC_BLEND_P) && r->match >= 0
                             ? (const float *)(prev->arena + prev->recs[r->match].payload[0]) : NULL;
        int nv = (r->flags & REC_BLEND_P) ? (int)(r->psize[0] / 4) : 4;
        for (int e = 0; e < nv && e < 48; e++) {
            float cv, pv = NAN;
            if (b) cv = b[e]; else { uint32_t u = (uint32_t)r->v[e]; memcpy(&cv, &u, 4); }
            if (a) pv = a[e];
            else if (!b && r->match >= 0) { uint32_t u = (uint32_t)prev->recs[r->match].v[e]; memcpy(&pv, &u, 4); }
            fprintf(o, "      [%2d] %12.5f  prev %12.5f%s\n", e, cv, pv, !isnan(pv) && jumps(pv, cv) ? "  JUMP" : "");
        }
    }
    fclose(o);
    LOG("[interp] dumped frame %llu to %s\n", (unsigned long long)frame, path);
}

// (debug) the image in framebuffer 0 to YKW_SHOT_DIR/interp_<tag>.bmp, half size
static void save_image(const char *tag) {
    const char *dir = getenv("YKW_SHOT_DIR");
    if (!dir || !*dir) return;
    static PFNGLBINDFRAMEBUFFERPROC bind_fb;
    if (!bind_fb) bind_fb = (PFNGLBINDFRAMEBUFFERPROC)gl_proc("glBindFramebuffer");
    GLint read_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    int w, h;
    uint8_t *px = read_image(&w, &h);
    bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    if (!px) return;
    SDL_Surface *s = SDL_CreateSurface(w / 2, h / 2, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < h / 2; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
        const uint32_t *src = (const uint32_t *)(px + (size_t)(h - 1 - 2 * y) * w * 4);
        for (int x = 0; x < w / 2; x++) dst[x] = src[2 * x] | 0xFF000000u;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/interp_%s.bmp", dir, tag);
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    free(px);
}

// ---- per frame (eglSwapBuffers, before the present)
bool interp_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("YKW_INTERP");
        on = e && *e ? atoi(e) != 0 : g_settings.interp;
        if (on) LOG("[interp] on: recording every frame's GL calls\n");
    }
    return on;
}

// ---- presenting: the display's refreshes on the game's timeline. With R refreshes per logic step (75 Hz: 1.25),
// refresh i shows the game at step i / R; for the step that just finished (n - 1, n] that is t = i / R - (n - 1): an
// in-between for t < 1, the frame's own image at t = 1. (A fixed latency of up to one step: the in-betweens come from
// the step before and this one.)
extern double g_display_hz;
extern void fb0_present(void);
static double g_step;     // logic steps presented so far
static uint64_t g_shown;  // the display refresh shown last (its index)
static uint64_t g_images, g_mids, g_cuts, g_backoffs;

static long g_shot_at = -2;
static uint64_t g_frame_now, g_shot_frame;  // g_shot_frame: (YKW_TEST_CLOCK) the frame the shot tick fell on
static void save_image(const char *tag);

static bool shooting(void) {
    if (g_shot_at < 0) return false;
    uint64_t first = getenv("YKW_TEST_CLOCK") ? g_shot_frame : (uint64_t)g_shot_at;
    return first && g_frame_now >= first && g_frame_now < first + 3;
}

static bool g_locked_shown;  // (the last image was shown at its own refresh: hle/vi.c's present lock)
static uint64_t g_sim_missed;  // (YKW_SIM_HZ: refreshes an image came too late for)
static double g_replay_ms;     // (the last in-between's replay, for YKW_PRESENT_LOG)

// After a frame's last image: the vsync for the step the display's next refresh needs (hle/vi.c), unless the game
// has had it. Only here, once a frame: asked for after each image, a frame spanning two steps had its two vsyncs
// waiting when it finished and the game stayed at two steps (30 fps) a frame.
static void request_next(void) {
    if (!g_locked_shown) return;
    extern bool vi_request_vsyncs(uint64_t total, uint64_t *which);
    extern volatile uint64_t g_vsync_count;
    static int64_t off;
    static uint64_t last_shown;
    double r = g_display_hz / 60.0;
    int64_t need = (int64_t)ceil((double)(g_shown + 1) / r - 1e-6);  // (the step the next refresh shows)
    int64_t count = (int64_t)__atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);
    if (!last_shown || g_shown < last_shown || need + off > count + 8 || need + off < count - 8) off = count + 1 - need;
    last_shown = g_shown;
    uint64_t which = 0;
    vi_request_vsyncs((uint64_t)(need + off), &which);
    // (Open: every 1-2 s the game's frame loop still misses one of these - the game's vsync-counting thread (main
    // +0x2bf3b0) takes it, yet the frame loop sleeps on to the vi.c timer's next one, 1.5 refreshes after the last
    // image. Not a timing race at the swap: sending it later, or only once the frame loop waits, or holding the swap
    // until the counter has taken it, didn't help.)
    static long lo = -2, hi;
    if (lo == -2) { const char *e = getenv("YKW_PRESENT_LOG"); lo = -1; if (e && sscanf(e, "%ld-%ld", &lo, &hi) != 2) lo = -1; }
    if (lo >= 0 && (long)g_frame_now >= lo && (long)g_frame_now <= hi)
        LOG("[request] frame %llu: need %lld + %lld, count %lld\n", (unsigned long long)g_frame_now, (long long)need,
            (long long)off, (long long)count);
}

static void show(double game_t) {
    extern SDL_Window *g_window;
    static long all_lo = -2, all_hi;  // (debug) YKW_SAVE_ALL=<first tick>-<last tick>: every image in that test-tick range
    if (all_lo == -2) { const char *e = getenv("YKW_SAVE_ALL"); all_lo = -1; if (e && sscanf(e, "%ld-%ld", &all_lo, &all_hi) != 2) all_lo = -1; }
    extern uint64_t ykw_test_tick(void);
    long tick_now = all_lo >= 0 ? (long)ykw_test_tick() : 0;
    if (shooting() || (all_lo >= 0 && tick_now >= all_lo && tick_now <= all_hi)) {
        char tag[64];
        snprintf(tag, sizeof tag, "i%llu_f%llu_g%.3f", (unsigned long long)g_shown, (unsigned long long)g_frame_now, game_t);
        save_image(tag);
    }
    extern void flicker_probe(double game_t);
    flicker_probe(game_t);
    extern bool vi_present_locked(void);
    bool locked = vi_present_locked();
    static int sim = -1;
    if (sim < 0) sim = getenv("YKW_SIM_HZ") != NULL;
    const SDL_DisplayMode *fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) ? SDL_GetWindowFullscreenMode(g_window) : NULL;
    // (YKW_PRESENT_PREWAIT=1, experiment: wait before an image only until the compositor has taken the one before,
    // not after it. Measured outdoors walking: 42-51 fps against 53-57 waiting after each image, so not the default.)
    static int prewait = -1;
    if (prewait < 0) prewait = getenv("YKW_PRESENT_PREWAIT") != NULL;
    bool pre_wait = prewait && !locked && g_settings.vsync && !sim && !fs;
    static uint64_t last_present;
    if (pre_wait && last_present) {
        extern uint64_t plat_next_composition_ns(uint64_t t);
        uint64_t t = plat_next_composition_ns(last_present);
        if (t) {
            t += 500000;  // (just past the pass)
            if (plat_now_ns() < t && t - plat_now_ns() < 50000000) plat_sleep_until_ns(t);
        } else {
            extern bool plat_wait_composition(void);
            plat_wait_composition();
        }
    }
    fb0_present();
    uint64_t t0 = plat_now_ns();
    SDL_GL_SwapWindow(g_window);
    g_images++;
    if (pre_wait) {
        glFinish();  // (the image is done: the pass it was ready for is the next one)
        last_present = plat_now_ns();
        extern void vi_image_shown(void);
        vi_image_shown();
        g_locked_shown = false;
    } else {   // Locked to the display (hle/vi.c): each image waits for its refresh, and once a frame's last image is up the
        // display asks for the game frame its next refresh needs (request_next). Refresh i shows game time i / r, in
        // logic steps = vsyncs (present_images); the vsyncs asked for are counted against all the game has had (the
        // vi.c timer's in a stall included), so after a slow frame the game takes the steps it missed.
        last_present = 0;
        if (locked || g_settings.vsync) {
            // (exclusive fullscreen: the swap is done at the refresh it was shown at. In a window the driver queues
            // swaps and returns at once, several in a row and then a long wait: there the compositor's pass is the
            // refresh)
            extern bool plat_wait_composition(void);
            glFinish();
            if (sim) {  // (test, YKW_SIM_HZ: the refreshes of a display that fast, on a timer; a late image starts over)
                static uint64_t next;
                uint64_t now = plat_now_ns(), period = (uint64_t)(1e9 / g_display_hz);
                if (!next || now > next + period) {
                    if (next) g_sim_missed++;
                    next = now;
                } else plat_sleep_until_ns(next);
                next += period;
            } else if (!fs) plat_wait_composition();
            extern void vi_image_shown(void);
            vi_image_shown();
            g_locked_shown = locked;
        } else g_locked_shown = false;
    }
    // (debug) YKW_PRESENT_LOG=<first frame>-<last frame>: each image: ms since the last one returned, the game time
    // it shows (in 1/60 s steps) and its advance, how long the swap blocked
    static long lo = -2, hi;
    if (lo == -2) { const char *e = getenv("YKW_PRESENT_LOG"); lo = -1; if (e && sscanf(e, "%ld-%ld", &lo, &hi) != 2) lo = -1; }
    if (lo >= 0) {
        static uint64_t last;
        static double last_g;
        uint64_t now = plat_now_ns();
        if ((long)g_frame_now >= lo && (long)g_frame_now <= hi)
            LOG("[present] frame %llu: +%.2f ms, game %.3f (+%.3f), swap %.2f ms, replay %.2f ms, missed %llu\n",
                (unsigned long long)g_frame_now, last ? (now - last) / 1e6 : 0.0, game_t, game_t - last_g,
                (now - t0) / 1e6, g_replay_ms, (unsigned long long)g_sim_missed);
        last = now, last_g = game_t;
    }
}

// true: the frame and its in-betweens were presented here
// The timeline is in vsyncs: a frame that took two (a slow one: the game ran two logic steps) is two steps on, and
// its in-betweens spread over both. Refreshes already behind the previous frame (after a stall) are skipped.
static bool present_images(Frame *f, Frame *prev, double steps) {
    double r = g_display_hz / 60.0;
    g_step += steps;
    double from = g_step - steps;  // (where the previous frame is on the timeline)
    if (!g_shown || (double)g_shown / r < from - 1e-6) g_shown = (uint64_t)floor(from * r + 1e-6);
    bool mid_drawn = false, own_shown = false;
    // (on the game's own vsync timer: an in-between more than a vsync behind the vsyncs gone by since is skipped -
    // each image takes a refresh, and after a slow frame showing them all made the next frame slower still)
    extern volatile uint64_t g_vsync_count;
    double base = (double)__atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE) - g_step;
    while ((double)(g_shown + 1) / r <= g_step + 1e-6) {
        g_shown++;
        double t = ((double)g_shown / r - from) / steps;
        if (shooting())
            LOG("[interp] frame %llu: image %llu at t %.3f\n", (unsigned long long)g_frame_now, (unsigned long long)g_shown, t);
        if (t <= 0.999 && !g_locked_shown &&
            from + t * steps + base < (double)__atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE) - 1.0)
            continue;
        if (t > 0.999) {  // the frame's own image
            if (mid_drawn) replay_t(f, NULL, 1.0f);  // (an exact replay: the frame's own image and render targets)
            show(from + t * steps);
            own_shown = true;
            mid_drawn = false;
        } else {
            uint64_t r0 = plat_now_ns();
            replay_t(f, prev, (float)t);
            static int timing = -1;
            if (timing < 0) timing = getenv("YKW_PRESENT_LOG") != NULL;
            if (timing) {  // (YKW_PRESENT_LOG: the replay's own time, not the wait for its refresh - costs the overlap;
                           //  YKW_REPLAY_CPU=1: only the CPU's, the calls issued, no wait for the GPU)
                if (!getenv("YKW_REPLAY_CPU")) glFinish();
                g_replay_ms = (plat_now_ns() - r0) / 1e6;
            }
            show(from + t * steps);
            g_mids++;
            mid_drawn = true;
        }
    }
    if (mid_drawn && !own_shown) replay_t(f, NULL, 1.0f);  // (the targets back to the frame's own state for the next)
    request_next();
    return true;
}

bool interp_frame_end(uint64_t frame) {
    if (!interp_enabled()) {
        if (getenv("YKW_CAM_UPLOADS") && !g_gl_hook_on) g_cam_probe_only = 1, g_gl_hook_on = 1;  // (the probe alone)
        return false;
    }
    g_gl_hook_on = 1;
    Frame *f = &g_frames[g_cur], *prev = &g_frames[g_cur ^ 1];
    g_frame_now = frame;
    assign_occ(f);
    static long verify_at = -2;
    if (verify_at == -2) { const char *e = getenv("YKW_INTERP_VERIFY"); verify_at = e ? atol(e) : -1; }
    if (verify_at >= 0 && (long)frame == verify_at) verify(f, frame);
    bool presented = false;
    extern bool ykw_fps_unlocked(void);
    // the vsyncs this frame took (the game's logic steps: hle/framerate.c): 1, or more after a slow frame
    extern volatile uint64_t g_vsync_count;
    static uint64_t last_vsyncs;
    uint64_t vs = __atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE);
    double steps = last_vsyncs && vs > last_vsyncs && vs - last_vsyncs <= 8 ? (double)(vs - last_vsyncs) : 1.0;
    last_vsyncs = vs;
    // The game's own frame rate comes first. Each in-between is a whole replay of the frame on the GPU (outdoors 5-7
    // ms, up to 11, against ~2.4 indoors; the CPU's part 0.2 ms), and at 75 Hz nearly every frame has one: where the
    // GPU can't fit both in a frame, the game missed vsyncs (53-57 fps walking outdoors, a stutter; 60 steady without
    // in-betweens). So a frame that took more than one vsync stops the in-betweens for half a second - the game's own
    // frames only, as without interpolation - and then they come back. YKW_INTERP_NO_BACKOFF=1: never.
    static int backoff;
    static int no_backoff = -1;
    if (no_backoff < 0) no_backoff = getenv("YKW_INTERP_NO_BACKOFF") != NULL;
    if (steps >= 2 && !no_backoff && prev->n) backoff = 30, g_backoffs++;
    if (backoff > 0) backoff--;
    if (ykw_fps_unlocked() && g_display_hz > 60.5 && !getenv("YKW_INTERP_NO_PRESENT")) {
        int matched = 0, blendable = 0;
        bool cut = f->bad || prev->bad || !prev->n || backoff > 0;
        if (!cut) {
            match_frames(f, prev, &matched, &blendable);
            cut = !blendable || matched < blendable * 8 / 10;  // (a scene change: too little in common)
        }
        // (YKW_INTERP_DUMP / _SHOT count test ticks when YKW_TEST_CLOCK is set: the first frame at or past the tick)
        {   // (debug) YKW_INTERP_NAMES=<substring>: per frame, the uniform calls whose name contains it (one log line a
            // frame with the count and the frame's first such value, while YKW_CAM_LOG's tick range is on)
            static const char *want = (const char *)1;
            if (want == (const char *)1) want = getenv("YKW_INTERP_NAMES");
            if (want) {
                int cnt = 0;
                float first[4] = {0};
                for (uint32_t k = 0; k < f->n; k++) {
                    const Rec *r = &f->recs[k];
                    if (!(r->flags & (REC_BLEND_V | REC_BLEND_P)) || gl_thunks[r->id].name[2] != 'U') continue;
                    int lp;
                    blend_kind(gl_thunks[r->id].name, &gl_sigs[r->id], &lp);
                    if (!strstr(uniform_name(r->prog, (int)r->x[lp]), want)) continue;
                    if (!cnt++ && (r->flags & REC_BLEND_P)) memcpy(first, f->arena + r->payload[0], 16);
                }
                extern uint64_t ykw_test_tick(void);
                long tk = (long)ykw_test_tick();
                if (tk >= 1640 && tk <= 1760)
                    LOG("[names] frame %llu tick %ld: %d '%s' calls, first %.3f %.3f %.3f %.3f\n", (unsigned long long)frame,
                        tk, cnt, want, first[0], first[1], first[2], first[3]);
            }
        }
        static long dump_at = -2;
        extern uint64_t ykw_test_tick(void);
        long when = getenv("YKW_TEST_CLOCK") ? (long)ykw_test_tick() : (long)frame;
        if (dump_at == -2) { const char *e = getenv("YKW_INTERP_DUMP"); dump_at = e ? atol(e) : -1; }
        if (dump_at >= 0 && when >= dump_at) { dump_frame(f, prev, frame); dump_at = -1; }
        if (g_shot_at == -2) { const char *e = getenv("YKW_INTERP_SHOT"); g_shot_at = e ? atol(e) : -1; }
        if (g_shot_at >= 0 && getenv("YKW_TEST_CLOCK") && when >= g_shot_at && !g_shot_frame) g_shot_frame = frame;
        if (cut) {  // no in-betweens: the frame as it is, at its place on the timeline
            g_cuts++;
            g_step += steps;
            g_shown = (uint64_t)floor(g_step * g_display_hz / 60.0);
            show(g_step);
            request_next();
            presented = true;
        } else presented = present_images(f, prev, steps);
    }
    static uint64_t bad_frames, frames, calls, bytes, t_log;
    frames++, calls += f->n, bytes += f->used;
    if (f->bad) {
        bad_frames++;
        static char last_bad[64];
        if (strcmp(last_bad, f->bad_name)) {
            LOG("[interp] frame %llu can't be replayed: %s\n", (unsigned long long)frame, f->bad_name);
            snprintf(last_bad, sizeof last_bad, "%s", f->bad_name);
        }
    }
    uint64_t now = plat_now_ns();
    if (getenv("YKW_INTERP_LOG") && now - t_log > 5000000000ull) {
        if (t_log && frames)
            LOG("[interp] %llu frames: %llu calls, %llu KB a frame, %llu not replayable; %.1f images/s, %llu in-betweens, "
                "%llu cuts, %llu late (YKW_SIM_HZ), %llu back-offs\n", (unsigned long long)frames,
                (unsigned long long)(calls / frames), (unsigned long long)(bytes / frames >> 10),
                (unsigned long long)bad_frames, g_images * 1e9 / (double)(now - t_log), (unsigned long long)g_mids,
                (unsigned long long)g_cuts, (unsigned long long)g_sim_missed, (unsigned long long)g_backoffs);
        t_log = now, frames = calls = bytes = bad_frames = 0, g_images = g_mids = g_cuts = 0, g_sim_missed = 0, g_backoffs = 0;
    }
    flush_deletes(f);
    g_cur ^= 1;
    frame_reset(&g_frames[g_cur]);
    return presented;
}
