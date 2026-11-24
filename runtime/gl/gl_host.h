// Shared by the generated GL thunks (gl_thunks.c) and the EGL layer (egl.c).
#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#include <string.h>

#include "../hle.h"

#ifndef APIENTRY
#define APIENTRY
#endif
// (registry types some glext.h versions leave out)
typedef void *GLeglClientBufferEXT_;
#ifndef GL_EXT_external_buffer
typedef void *GLeglClientBufferEXT;
#endif
#ifndef GL_NV_vdpau_interop
typedef GLintptr GLvdpauSurfaceNV;
#endif

typedef GLint GLclampx;  // (OpenGL ES 1 fixed point)

typedef struct { const char *name; HostFn fn; } GlThunk;
extern const GlThunk gl_thunks[];
extern const int gl_thunk_count;

void *gl_proc(const char *name);         // the host driver's entry point (NULL: not there)
void gl_missing(const char *name);       // logged once per name
void gl_unsupported(const char *name);   // a command the thunks can't forward (callback arguments)

// YKW_GL_TRACE=<frame>: every GL call of that frame (counted in eglSwapBuffers) goes to the log
extern volatile int g_gl_trace_on;
void gl_trace(const char *name, Cpu *c);
#define GL_TRACE(name, c) do { if (__builtin_expect(g_gl_trace_on, 0)) gl_trace(name, c); } while (0)

// AAPCS64 argument and result access
static inline float gl_vf(__uint128_t v) { uint32_t b = (uint32_t)v; float f; memcpy(&f, &b, 4); return f; }
static inline double gl_vd(__uint128_t v) { uint64_t b = (uint64_t)v; double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t gl_si(Cpu *c, int slot) { return *(uint64_t *)(uintptr_t)(c->sp + 8ull * slot); }
static inline float gl_sf(Cpu *c, int slot) { float f; memcpy(&f, (void *)(uintptr_t)(c->sp + 8ull * slot), 4); return f; }
static inline double gl_sd(Cpu *c, int slot) { double d; memcpy(&d, (void *)(uintptr_t)(c->sp + 8ull * slot), 8); return d; }
static inline void gl_retf(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[0] = b; }
static inline void gl_retd(Cpu *c, double d) { uint64_t b; memcpy(&b, &d, 8); c->v[0] = b; }
