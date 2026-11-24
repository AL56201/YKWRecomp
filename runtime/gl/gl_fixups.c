// GL calls that need more than a straight pass-through (looked up before the generated thunks: egl.c).
//   glShaderSource: YKW_SHADER_DUMP=<dir> writes each shader's source there (shader_<name>.glsl)
//   glCompileShader / glLinkProgram: failures are logged with the driver's message and the shader's source name
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "gl_host.h"

typedef void (APIENTRY *ShaderSourceFn)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *ShaderFn)(GLuint);
typedef void (APIENTRY *GetivFn)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *InfoLogFn)(GLuint, GLsizei, GLsizei *, GLchar *);

static void fix_shader_source(Cpu *c) {
    GL_TRACE("glShaderSource", c);
    static ShaderSourceFn f;
    if (!f) f = (ShaderSourceFn)gl_proc("glShaderSource");
    GLuint sh = (GLuint)c->x[0];
    GLsizei n = (GLsizei)c->x[1];
    const GLchar *const *str = (const GLchar *const *)(uintptr_t)c->x[2];
    const GLint *len = (const GLint *)(uintptr_t)c->x[3];
    // YKW_NO_DOF=1 (test): the post-effect shader's depth of field off
    static char *patched;
    // YKW_DOF_DEBUG=1 (test): the post effect draws its depth-of-field inputs instead of the picture: red = the
    // pixel's linear depth, green = focus depth, blue = full-blur depth (all through the shader's linearDepth)
    if (getenv("YKW_DOF_DEBUG") && n == 1 && str[0] && strstr(str[0], "tex_color = dof(")) {
        size_t l = len && len[0] >= 0 ? (size_t)len[0] : strlen(str[0]);
        // (the vertical pass's final write; left half: linear depth, focus, full blur; right half: dofRate,
        // near / far, the raw depth-buffer value)
        static const char from[] = "outColor.rgb = tex_color.rgb;";
        static const char to[] = "outColor.rgb = uv.x < 0.5 ? vec3( linearZ, linearDepth(unf_Posteffect.dofFocusZ), "
                                 "linearDepth(unf_Posteffect.dofMaxBokehZ) ) : vec3( unf_Posteffect.dofRate, "
                                 "unf_camera_near_far.x / unf_camera_near_far.y, depthValue );";
        free(patched);
        patched = malloc(l + sizeof to);
        memcpy(patched, str[0], l);
        patched[l] = 0;
        char *p = strstr(patched, from), *q;
        while (p && (q = strstr(p + 1, from))) p = q;  // (the last one: the vertical pass)
        if (p) {
            memmove(p + strlen(to), p + strlen(from), strlen(p + strlen(from)) + 1);
            memcpy(p, to, strlen(to));
            LOG("[gl] shader %u: depth of field debug colours (YKW_DOF_DEBUG)\n", sh);
            static const GLchar *one[1];
            one[0] = patched;
            str = one, len = NULL;
        } else LOG("[gl] shader %u: YKW_DOF_DEBUG pattern not found\n", sh);
    }
    if (getenv("YKW_NO_DOF") && n == 1 && str[0] && strstr(str[0], "tex_color = dof(")) {
        size_t l = len && len[0] >= 0 ? (size_t)len[0] : strlen(str[0]);
        free(patched);
        patched = malloc(l + 1);
        memcpy(patched, str[0], l);
        patched[l] = 0;
        char *p = strstr(patched, "if ( unf_Posteffect.dofUse )");
        if (p) memcpy(p + 5, "false                ", 21), LOG("[gl] shader %u: depth of field off (YKW_NO_DOF)\n", sh);
        static const GLchar *one[1];
        one[0] = patched;
        str = one, len = NULL;
    }
    // smoothstep(a, b, x) with a > b is undefined in GLSL. The game's depth of field calls
    // smoothstep(focus, full_blur, depth) with the focus depth beyond the full-blur depth (near objects blur, the rest
    // stays sharp). NVIDIA (the Switch's compiler) evaluates the textbook formula; AMD returned full blur for the whole
    // 3D scene (measured: sharpness 254 vs 623 with the formula written out). Two earlier replacements only held until
    // the driver's optimized recompile, which it caches for later launches: the textbook formula, and
    // "a <= b ? smoothstep(a, b, x) : 1.0 - smoothstep(b, a, x)" - each was turned back into smoothstep(a, b, x) on the
    // reversed edges. This one computes the curve on min / max of the edges (ordered by construction, so any
    // smoothstep the compiler makes of it is defined) and flips it afterwards for reversed edges. Inserted after every
    // shader's #version line. YKW_GL_NATIVE_SMOOTHSTEP=1: the driver's own.
#define YKW_SS(T, B) \
    T " ykw_ss(" T " a, " T " b, " T " x) { " T " lo = min(a, b), hi = max(a, b); " \
    T " t = clamp((x - lo) / (hi - lo), 0.0, 1.0); " T " h = t * t * (3.0 - 2.0 * t); return " B "; }\n"
    static const char ss[] =
        "\n" YKW_SS("float", "a > b ? 1.0 - h : h")
        YKW_SS("vec2", "mix(h, 1.0 - h, greaterThan(a, b))")
        YKW_SS("vec3", "mix(h, 1.0 - h, greaterThan(a, b))")
        YKW_SS("vec4", "mix(h, 1.0 - h, greaterThan(a, b))")
        "vec2 ykw_ss(float a, float b, vec2 x) { return ykw_ss(vec2(a), vec2(b), x); }\n"
        "vec3 ykw_ss(float a, float b, vec3 x) { return ykw_ss(vec3(a), vec3(b), x); }\n"
        "vec4 ykw_ss(float a, float b, vec4 x) { return ykw_ss(vec4(a), vec4(b), x); }\n"
        "#define smoothstep ykw_ss\n";
    if (!getenv("YKW_GL_NATIVE_SMOOTHSTEP") && n > 0 && str[0] && !strncmp(str[0], "#version", 8)) {
        size_t total = 0;
        for (GLsizei i = 0; i < n; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        char *joined = malloc(total + sizeof ss + 1), *o = joined;
        for (GLsizei i = 0; i < n; i++) {
            size_t li = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
            memcpy(o, str[i], li);
            o += li;
        }
        *o = 0;
        char *eol = strchr(joined, '\n');
        if (eol) {  // (right after #version, before anything that could use it)
            memmove(eol + sizeof ss - 1, eol, strlen(eol) + 1);
            memcpy(eol, ss, sizeof ss - 1);
        }
        free(patched);
        patched = joined;
        static const GLchar *one[1];
        one[0] = patched;
        str = one, len = NULL, n = 1;
    }
    // YKW_SHADER_SUB="from=>to;;from2=>to2" (experiments): text replacements (every occurrence) in shaders that contain
    // YKW_SHADER_SUB_IN (default "tex_color = dof(": the post effect), applied after the fixes above
    const char *subs = getenv("YKW_SHADER_SUB");
    if (subs && *subs && n == 1 && str[0]) {
        const char *in = getenv("YKW_SHADER_SUB_IN");
        if (!in || !*in) in = "tex_color = dof(";
        if (strstr(str[0], in)) {
            size_t cap = strlen(str[0]) * 2 + 4096;
            char *cur = malloc(cap);
            strcpy(cur, str[0]);
            char *list = strdup(subs);
            int applied = 0;
            for (char *item = list, *next; item && *item; item = next) {
                next = strstr(item, ";;");
                if (next) *next = 0, next += 2;
                char *arrow = strstr(item, "=>");
                if (!arrow) continue;
                *arrow = 0;
                const char *from = item, *to = arrow + 2;
                size_t fl = strlen(from), tl = strlen(to);
                if (!fl) continue;
                for (char *p = strstr(cur, from); p; p = strstr(p + tl, from)) {
                    if (strlen(cur) + tl + 1 > cap) break;
                    memmove(p + tl, p + fl, strlen(p + fl) + 1);
                    memcpy(p, to, tl);
                    applied++;
                }
            }
            free(list);
            LOG("[gl] shader %u: YKW_SHADER_SUB made %d replacement(s)\n", sh, applied);
            free(patched);
            patched = cur;
            static const GLchar *one2[1];
            one2[0] = patched;
            str = one2, len = NULL;
        }
    }
    // A tag on top of every shader we changed: the hash of its final text, plus a value unique to this launch. AMD's
    // driver keeps compiled shaders in an on-disk cache, and what it loads from there on later launches drew the depth
    // of field fully blurred even with the smoothstep fix in place (measured: every fresh compile sharp, 623; every
    // later launch of the same text blurred, 254, whichever way the fix was written). With the launch's own value the
    // driver never finds a cached copy and compiles these shaders fresh at every boot, as the game asks for them.
    // YKW_SHADER_CACHE=1: the hash alone, so the driver's cache is used (blurred depth of field on AMD).
    if (patched && n == 1 && str[0] == patched && !strncmp(patched, "#version", 8)) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const unsigned char *p = (const unsigned char *)patched; *p; p++) h = (h ^ *p) * 0x100000001b3ull;
        static uint64_t launch;
        if (!launch) launch = plat_now_ns() | 1;
        if (!getenv("YKW_SHADER_CACHE")) h ^= launch;
        char tag[48];
        int tl = snprintf(tag, sizeof tag, "\n// ykw %016llx", (unsigned long long)h);
        char *eol = strchr(patched, '\n');
        size_t pl = strlen(patched);
        char *grown = eol ? realloc(patched, pl + tl + 1) : NULL;
        if (grown) {
            eol = grown + (eol - patched);
            memmove(eol + tl, eol, strlen(eol) + 1);
            memcpy(eol, tag, tl);
            patched = grown;
            static const GLchar *one3[1];
            one3[0] = patched;
            str = one3;
        }
    }
    const char *dir = getenv("YKW_SHADER_DUMP");
    if (dir && *dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/shader_%u.glsl", dir, sh);
        FILE *o = fopen(path, "wb");
        if (o) {
            for (GLsizei i = 0; i < n; i++)
                fwrite(str[i], 1, len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]), o);
            fclose(o);
        }
    }
    f(sh, n, str, len);
}

static void fix_compile_shader(Cpu *c) {
    GL_TRACE("glCompileShader", c);
    static ShaderFn f;
    static GetivFn getiv;
    static InfoLogFn info;
    if (!f) {
        f = (ShaderFn)gl_proc("glCompileShader");
        getiv = (GetivFn)gl_proc("glGetShaderiv");
        info = (InfoLogFn)gl_proc("glGetShaderInfoLog");
    }
    GLuint sh = (GLuint)c->x[0];
    f(sh);
    GLint ok = 0;
    getiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        info(sh, sizeof log, NULL, log);
        LOG("[gl] shader %u failed to compile:\n%s\n", sh, log);
    }
}

static void fix_link_program(Cpu *c) {
    GL_TRACE("glLinkProgram", c);
    static ShaderFn f;
    static GetivFn getiv;
    static InfoLogFn info;
    if (!f) {
        f = (ShaderFn)gl_proc("glLinkProgram");
        getiv = (GetivFn)gl_proc("glGetProgramiv");
        info = (InfoLogFn)gl_proc("glGetProgramInfoLog");
    }
    GLuint p = (GLuint)c->x[0];
    f(p);
    GLint ok = 0;
    getiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        info(p, sizeof log, NULL, log);
        LOG("[gl] program %u failed to link:\n%s\n", p, log);
    }
}

void fb0_fix_bind_framebuffer(Cpu *c);  // (fb0.c: the game's framebuffer 0 is offscreen)
void fb0_fix_draw_buffers(Cpu *c);
void fb0_fix_draw_buffer(Cpu *c);
void fb0_fix_read_buffer(Cpu *c);
void fb0_fix_get_integerv(Cpu *c);

static const GlThunk g_fixups[] = {
    {"glBindFramebuffer", fb0_fix_bind_framebuffer},
    {"glDrawBuffers", fb0_fix_draw_buffers},
    {"glDrawBuffer", fb0_fix_draw_buffer},
    {"glReadBuffer", fb0_fix_read_buffer},
    {"glGetIntegerv", fb0_fix_get_integerv},
    {"glCompileShader", fix_compile_shader},
    {"glLinkProgram", fix_link_program},
    {"glShaderSource", fix_shader_source},
};

HostFn gl_fixup(const char *name) {
    for (size_t i = 0; i < sizeof g_fixups / sizeof *g_fixups; i++)
        if (!strcmp(g_fixups[i].name, name)) return g_fixups[i].fn;
    return NULL;
}
