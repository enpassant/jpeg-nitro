// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroview for Linux: the same viewer as nitroview.m (macOS) and nitroview_win.c (Windows), on
// SDL3 + OpenGL 3.3. It builds on macOS too (for testing; the native viewers stay the real ones).
//
//   nitroview-sdl [-f] [-s ms] [-j threads] file.jpg [more files, directories or wildcards...]
//   nitroview-sdl --bench files...     decode everything, print timings (a hidden window)
//   nitroview-sdl --selftest files...  compare the GPU output with stb_image's decoding
//   nitroview-sdl --register           add it to the desktop's applications ("Open with")
//
// Keys, mouse and window behaviour as in the other viewers (see README), + O: open files,
// Shift+O: a folder.
//
// Pipeline: file -> nitrojpeg / nitropng / nitropsd / nitroheic / nitrowebp / nitrodng / nitrotiff / nitroavif
// (multi-threaded, into a host buffer, on the decoder thread) -> OpenGL: the planes as textures
// -> a fragment shader into the RGBA texture (upsampling, YCbCr->RGB, PNG rows, PSD planes) ->
// mipmaps -> trilinear scaled draw. Anything else goes through stb_image (16-bit / palette /
// interlaced PNG, GIF, BMP, TGA, damaged or CMYK JPEG). SVG / CorelDRAW (optional, NV_SVG /
// NV_CDR) as on Windows. Neighbouring images are decoded ahead in the background.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#ifdef __linux__
#include <unistd.h>   // readlink
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nitrojpeg.h"
#include "nitropng.h"
#include "nitropsd.h"
#include "nitroheic.h"
#include "nitrodng.h"
#include "nitrowebp.h"
#include "nitrotiff.h"
#include "nitro_load.h"       // reading and decoding image files (shared by the viewers)
#include "nitro_prefetch.h"   // the image cache and background decoder (shared too)
#include "nitro_view.h"       // the zoom / fit / pan arithmetic (shared too)
#ifdef NV_DAV1D
#include "nitroavif.h"   // AVIF: AV1 through dav1d
#endif
#ifdef NV_SVG
#include "nitrosvg.h"   // optional SVG rendering (lunasvg)
#endif
#ifdef NV_CDR
#include "nitrocdr.h"   // optional CorelDRAW rendering (libcdr + plutovg)
#endif
#if defined(NV_SVG) || defined(NV_CDR)
#define NV_VECTOR 1     // vector images: rendered again, sharp, when zoomed in
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "stb_image.h"
#pragma clang diagnostic pop

#include "icon.inc"   // packaging/icon.png (xxd -i): the window icon

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
#endif
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


static int g_max_tex = 16384;   // (the GPU's limit, if lower)

static double now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static const char *base_name(const char *p) {
    const char *n = p;
    for (const char *c = p; *c; c++)
        if (*c == '/' || *c == '\\') n = c + 1;
    return n;
}

// ---------------------------------------------------------------------------
// OpenGL 3.3 core: the functions are loaded through SDL (no GL headers, no link-time libGL)

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#ifdef _WIN32
#define NVGL __stdcall
#else
#define NVGL
#endif

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_STATIC_DRAW 0x88E4
#define GL_ARRAY_BUFFER 0x8892
#define GL_FLOAT 0x1406
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_TEXTURE_BUFFER_SIZE 0x8C2B
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#define GL_FUNCS(X) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(void, glGetFloatv, (GLenum pname, GLfloat *data)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glGenTextures, (GLsizei n, GLuint *t)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t)) \
    X(void, glBindTexture, (GLenum target, GLuint t)) \
    X(void, glActiveTexture, (GLenum unit)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *p)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *p)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint v)) \
    X(void, glPixelStorei, (GLenum pname, GLint v)) \
    X(void, glGenerateMipmap, (GLenum target)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *f)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint f)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len)) \
    X(void, glCompileShader, (GLuint s)) \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint *v)) \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void, glDeleteShader, (GLuint s)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint p, GLuint s)) \
    X(void, glLinkProgram, (GLuint p)) \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint *v)) \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void, glUseProgram, (GLuint p)) \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name)) \
    X(void, glUniform1i, (GLint loc, GLint v)) \
    X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b)) \
    X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *a)) \
    X(void, glBindVertexArray, (GLuint a)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *b)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *b)) \
    X(void, glBindBuffer, (GLenum target, GLuint b)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(void, glTexBuffer, (GLenum target, GLenum ifmt, GLuint b)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *p)) \
    X(void, glGenSamplers, (GLsizei n, GLuint *s)) \
    X(void, glSamplerParameteri, (GLuint s, GLenum pname, GLint v)) \
    X(void, glSamplerParameterf, (GLuint s, GLenum pname, GLfloat v)) \
    X(void, glBindSampler, (GLuint unit, GLuint s)) \
    X(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLubyte norm, GLsizei stride, const void *p)) \
    X(void, glEnableVertexAttribArray, (GLuint i))

#define GL_DECL(ret, name, args) typedef ret(NVGL *name##_fn) args; static name##_fn name;
GL_FUNCS(GL_DECL)

static int gl_load(void) {
    int missing = 0;
#define GL_LOAD(ret, name, args) \
    if (!(name = (name##_fn)SDL_GL_GetProcAddress(#name))) { fprintf(stderr, "OpenGL: no %s\n", #name); missing = 1; }
    GL_FUNCS(GL_LOAD)
    return missing ? -1 : 0;
}

// ---------------------------------------------------------------------------
// decoded image

typedef enum { KIND_YUV = 0, KIND_BGRA = 1, KIND_PNG = 2, KIND_PLANAR = 3 } Kind;
enum { MODE_STB = -3, MODE_SVG = NV_MODE_SVG, MODE_CDR = NV_MODE_CDR, MODE_NITRODNG = NV_MODE_NITRODNG };   // (the rest: nitro_load.h)

#ifdef NV_VECTOR
// A loaded vector document (SVG, CorelDRAW), shared by the images of its pages and the background
// renders of their sharp detail: reference counted, freed by whichever drops it last.
typedef struct {
    void *doc;
    int (*render)(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0);
    void (*size)(void *doc, int page, double *w, double *h);
    void (*free)(void *doc);
    int pages, mode;
    SDL_AtomicInt refs;
} VectorDoc;
static VectorDoc *vec_ref(VectorDoc *v) {
    if (v) SDL_AddAtomicInt(&v->refs, 1);
    return v;
}
static void vec_unref(VectorDoc *v) {
    if (v && SDL_AddAtomicInt(&v->refs, -1) == 1) {
        v->free(v->doc);
        free(v);
    }
}
// Shown fitted to the screen (full screen or key 0), the main thread sets the screen's size in
// pixels here: vector images are then rendered at least that large (the prefetched ones too).
static SDL_AtomicInt g_vec_tw, g_vec_th;
#endif

typedef struct {
    Kind kind;
    int width, height, ncomp, orientation;
    int h1, v1, hmax, vmax, hasa;      // chroma sampling; PSD: transparency plane
    int pw[3], ph[3];
    size_t offset[3], pitch[3];
    size_t size;                       // bytes of image data
    uint8_t *buf;                      // the decoded data (host memory, pool buffer) until converted
    size_t cap;
    GLuint tex;                        // converted RGBA + mips (main thread)
    double read_ms, decode_ms, upload_ms, conv_ms;
    int mode, bands;
    int ev3;                           // a DNG: the exposure correction it was developed with (1/3 EV)
    int limited;                       // KIND_YUV: limited range (Y 16..235, as WebP), not full range
#ifdef NV_VECTOR
    VectorDoc *vec;                    // SVG / CDR: the document (this is its page `page`)
    int page;
    double vscale;                     // decoded pixels per document unit
    GLuint dtex;                       // sharp detail (main thread): the visible part rendered at screen
    double dx, dy, dw, dh, dscale;     // resolution, over the rectangle dx, dy, dw, dh of the image
#endif
} Decoded;


static const char *mode_name(int m) {
    const char *n = nv_mode_name(m);
    return n ? n : m == MODE_STB ? "stb" : "?";
}

// Host buffers the decoders write into: 50-100 MB each, reused (page faults are expensive).
static SDL_Mutex *g_pool_lock;
static struct { uint8_t *p; size_t cap; } g_free_bufs[4];
static int g_nfree;

static uint8_t *buf_get(size_t len, size_t *cap) {
    SDL_LockMutex(g_pool_lock);
    int best = -1;
    for (int i = 0; i < g_nfree; i++)
        if (g_free_bufs[i].cap >= len && g_free_bufs[i].cap <= len * 2 && (best < 0 || g_free_bufs[i].cap < g_free_bufs[best].cap))
            best = i;
    if (best >= 0) {
        uint8_t *p = g_free_bufs[best].p;
        *cap = g_free_bufs[best].cap;
        g_free_bufs[best] = g_free_bufs[--g_nfree];
        SDL_UnlockMutex(g_pool_lock);
        return p;
    }
    SDL_UnlockMutex(g_pool_lock);
    *cap = align_up(len, 64);
    return SDL_aligned_alloc(64, *cap + 64);
}

static void buf_put(uint8_t *p, size_t cap) {
    if (!p) return;
    SDL_LockMutex(g_pool_lock);
    if (g_nfree == 4) {   // drop the oldest
        SDL_aligned_free(g_free_bufs[0].p);
        memmove(g_free_bufs, g_free_bufs + 1, 3 * sizeof *g_free_bufs);
        g_nfree--;
    }
    g_free_bufs[g_nfree].p = p;
    g_free_bufs[g_nfree].cap = cap;
    g_nfree++;
    SDL_UnlockMutex(g_pool_lock);
}

// ---------------------------------------------------------------------------
// files


typedef struct { char **v; int n, cap; } FileList;

static void fl_add(FileList *l, const char *p) {
    if (l->n == l->cap) l->v = realloc(l->v, (size_t)(l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->v);
    l->v[l->n++] = SDL_strdup(p);
}
static void fl_free(FileList *l) {
    for (int i = 0; i < l->n; i++) SDL_free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

// "img2" before "img10", case-insensitive (as file managers sort)
static int natcmp(const char *a, const char *b) {
    const char *a0 = a, *b0 = b;
    while (*a && *b) {
        if (SDL_isdigit((unsigned char)*a) && SDL_isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *sa = a, *sb = b;
            while (SDL_isdigit((unsigned char)*a)) a++;
            while (SDL_isdigit((unsigned char)*b)) b++;
            if (a - sa != b - sb) return a - sa < b - sb ? -1 : 1;
            int c = memcmp(sa, sb, (size_t)(a - sa));
            if (c) return c;
            continue;
        }
        int ca = SDL_tolower((unsigned char)*a), cb = SDL_tolower((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++;
        b++;
    }
    if (*a || *b) return *a ? 1 : -1;
    return strcmp(a0, b0);
}
static int cmp_names(const void *a, const void *b) {
    return natcmp(base_name(*(char *const *)a), base_name(*(char *const *)b));
}

static const char *const g_more_exts[] = {"tga",   // (stb_image reads these too)
#ifdef NV_SVG
                                          "svg",
#endif
#ifdef NV_CDR
                                          "cdr", "cmx",
#endif
                                          NULL};

static int is_dir(const char *p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    int sep = !dl || dir[dl - 1] == '/' || dir[dl - 1] == '\\';
    char *p = NULL;
    SDL_asprintf(&p, "%s%s%s", dir, sep ? "" : "/", name);
    return p;
}

// dir + pattern -> matching image files (or any file for an explicit wildcard), sorted
static void list_dir(FileList *out, const char *dir, const char *pattern, int any_ext) {
    int n = 0;
    char **names = SDL_GlobDirectory(*dir ? dir : ".", pattern, SDL_GLOB_CASEINSENSITIVE, &n);
    if (!names) return;
    int first = out->n;
    for (int i = 0; i < n; i++) {
        if (!any_ext && !nv_is_image_name(names[i]) && !nv_has_ext(names[i], g_more_exts)) continue;
        char *p = *dir ? join_path(dir, names[i]) : SDL_strdup(names[i]);
        SDL_PathInfo info;
        if (p && SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE) fl_add(out, p);
        SDL_free(p);
    }
    SDL_free(names);
    qsort(out->v + first, (size_t)(out->n - first), sizeof *out->v, cmp_names);
}

// Arguments -> files: directories are listed, wildcards expanded (if the shell didn't).
static void collect_files(FileList *out, char **args, int n) {
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        if (strpbrk(a, "*?")) {
            const char *name = base_name(a);
            char *dir = SDL_strndup(a, (size_t)(name - a));
            int before = out->n;
            list_dir(out, dir, name, 1);
            if (out->n == before) fprintf(stderr, "no match: %s\n", a);
            SDL_free(dir);
            continue;
        }
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(a, &info)) { fprintf(stderr, "no such file: %s\n", a); continue; }
        if (info.type == SDL_PATHTYPE_DIRECTORY) list_dir(out, a, NULL, 0);
        else fl_add(out, a);
    }
}

// ---------------------------------------------------------------------------
// GPU

static GLuint g_conv_prog, g_png_prog, g_draw_prog, g_vao, g_fbo;
static GLuint g_samp_lin, g_samp_pt, g_samp_bil;
static GLint g_max_tbo;      // PNG rows in a buffer texture up to this size (0: converted on the CPU)
static int g_bench_split;    // --bench: time the conversion and the mipmaps separately
static struct { GLint kind, hasa, cs; } U;                          // conversion uniforms
static struct { GLint ch, pitch; } UP;                              // PNG
static struct { GLint origin, size, orientation; } UD;              // draw uniforms

static const char g_conv_vs[] =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;   // one triangle over the whole target\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

// The decoded data -> RGBA: JPEG / PSD planes as R8 textures (Y and PSD channels are read, chroma
// comes through the bilinear filter: the same centred linear upsampling as libjpeg's "fancy" one).
static const char g_conv_fs[] =
    "#version 330 core\n"
    "uniform int kind;      // 0 YCbCr, 1 gray, 4 PSD planes, 5 limited range YCbCr\n"
    "uniform int hasa;      // PSD transparency plane\n"
    "uniform vec4 cs;       // chroma scale, chroma plane size\n"
    "uniform sampler2D P0, P1, P2, P3;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    ivec2 g = ivec2(gl_FragCoord.xy);\n"
    "    vec4 c;\n"
    "    if (kind == 4) {   // PSD: R G B (gray: the same plane three times), transparency\n"
    "        vec3 v = vec3(texelFetch(P0, g, 0).r, texelFetch(P1, g, 0).r, texelFetch(P2, g, 0).r);\n"
    "        if (hasa != 0) v = max(v + texelFetch(P3, g, 0).r - 1.0, 0.0);   // matted with white -> over black\n"
    "        c = vec4(v, 1.0);\n"
    "    } else {\n"
    "        float y = texelFetch(P0, g, 0).r;\n"
    "        if (kind == 1) {\n"
    "            c = vec4(y, y, y, 1.0);\n"
    "        } else {\n"
    "            vec2 uv = (vec2(g) + 0.5) * cs.xy / cs.zw;\n"
    "            float cb = texture(P1, uv).r - 128.0 / 255.0;\n"
    "            float cr = texture(P2, uv).r - 128.0 / 255.0;\n"
    "            if (kind == 5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); cb *= 255.0 / 224.0; cr *= 255.0 / 224.0; }\n"
    "            c = vec4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);\n"
    "        }\n"
    "    }\n"
    "    o = clamp(c, 0.0, 1.0);\n"
    "}\n";

// PNG rows (after their filter byte) from a buffer texture, over black. A program of its own:
// virgl on a macOS host draws nothing with a samplerBuffer in the program.
static const char g_png_fs[] =
    "#version 330 core\n"
    "uniform int ch, pitch;   // channels, row pitch\n"
    "uniform samplerBuffer RAW;\n"
    "out vec4 o;\n"
    "float B(int a) { return texelFetch(RAW, a).r; }\n"
    "void main() {\n"
    "    ivec2 g = ivec2(gl_FragCoord.xy);\n"
    "    int q = 1 + g.y * pitch + g.x * ch;\n"
    "    vec4 v;\n"
    "    if (ch == 4) v = vec4(B(q), B(q + 1), B(q + 2), B(q + 3));\n"
    "    else if (ch == 3) v = vec4(B(q), B(q + 1), B(q + 2), 1.0);\n"
    "    else if (ch == 2) { float t = B(q); v = vec4(t, t, t, B(q + 1)); }\n"
    "    else { float t = B(q); v = vec4(t, t, t, 1.0); }\n"
    "    o = vec4(v.rgb * v.a, 1.0);\n"
    "}\n";

static const char g_draw_vs[] =
    "#version 330 core\n"
    "uniform vec2 origin, size;   // NDC: the image's top-left corner, its extent\n"
    "uniform int orientation;     // EXIF\n"
    "layout(location = 0) in vec2 q;   // the corner: 0..1 display space, y down\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(origin.x + q.x * size.x, origin.y - q.y * size.y, 0.0, 1.0);\n"
    "    float u = q.x, v = q.y;\n"
    "    vec2 t = vec2(u, v);\n"
    "    if (orientation == 2) t = vec2(1.0 - u, v);\n"
    "    else if (orientation == 3) t = vec2(1.0 - u, 1.0 - v);\n"
    "    else if (orientation == 4) t = vec2(u, 1.0 - v);\n"
    "    else if (orientation == 5) t = vec2(v, u);\n"
    "    else if (orientation == 6) t = vec2(v, 1.0 - u);\n"
    "    else if (orientation == 7) t = vec2(1.0 - v, 1.0 - u);\n"
    "    else if (orientation == 8) t = vec2(1.0 - v, u);\n"
    "    uv = t;\n"
    "}\n";

static const char g_draw_fs[] =
    "#version 330 core\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texture(tex, uv).rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "shader program: %s\n", log);
        return 0;
    }
    return p;
}

// Whether the PNG program works here: a 1x1 RGB pixel converted and read back. (0: it does)
static int png_gpu_check(void) {
    static const uint8_t row[4] = {0, 10, 128, 250};   // filter byte + RGB
    GLuint tbo = 0, tbt = 0, t = 0;
    uint8_t px[4] = {0, 0, 0, 0};
    glGenBuffers(1, &tbo);
    glBindBuffer(GL_TEXTURE_BUFFER, tbo);
    glBufferData(GL_TEXTURE_BUFFER, sizeof row, row, GL_STATIC_DRAW);
    glGenTextures(1, &tbt);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_BUFFER, tbt);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R8, tbo);
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, 1, 1);
    glUseProgram(g_png_prog);
    glUniform1i(UP.ch, 3);
    glUniform1i(UP.pitch, 4);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &t);
    glDeleteTextures(1, &tbt);
    glDeleteBuffers(1, &tbo);
    return !(px[0] == 10 && px[1] == 128 && px[2] == 250 && px[3] == 255);
}

static int gpu_init(void) {
    if (gl_load()) return -1;
    g_conv_prog = link_program(g_conv_vs, g_conv_fs);
    g_png_prog = link_program(g_conv_vs, g_png_fs);
    g_draw_prog = link_program(g_draw_vs, g_draw_fs);
    if (!g_conv_prog || !g_draw_prog) return -1;
    glUseProgram(g_conv_prog);
    U.kind = glGetUniformLocation(g_conv_prog, "kind");
    U.hasa = glGetUniformLocation(g_conv_prog, "hasa");
    U.cs = glGetUniformLocation(g_conv_prog, "cs");
    static const char *const units[] = {"P0", "P1", "P2", "P3"};   // texture units 1..4
    for (int i = 0; i < 4; i++) glUniform1i(glGetUniformLocation(g_conv_prog, units[i]), i + 1);
    if (g_png_prog) {
        glUseProgram(g_png_prog);
        UP.ch = glGetUniformLocation(g_png_prog, "ch");
        UP.pitch = glGetUniformLocation(g_png_prog, "pitch");
        glUniform1i(glGetUniformLocation(g_png_prog, "RAW"), 5);   // texture unit 5
    }
    glUseProgram(g_draw_prog);
    UD.origin = glGetUniformLocation(g_draw_prog, "origin");
    UD.size = glGetUniformLocation(g_draw_prog, "size");
    UD.orientation = glGetUniformLocation(g_draw_prog, "orientation");
    glUniform1i(glGetUniformLocation(g_draw_prog, "tex"), 0);
    // the corners: a triangle over the whole target (conversion), a quad 0..1 (drawing). (Not made
    // from gl_VertexID alone: virgl on a macOS host doesn't draw without vertex data.)
    static const float corners[] = {-1, -1, 3, -1, -1, 3, 0, 0, 1, 0, 0, 1, 1, 1};
    GLuint vbo;
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
    glEnableVertexAttribArray(0);
    glGenFramebuffers(1, &g_fbo);
    GLuint s[3];
    glGenSamplers(3, s);
    g_samp_lin = s[0];   // trilinear (+ anisotropic where there is)
    g_samp_pt = s[1];    // integer zoom: exact pixel blocks
    g_samp_bil = s[2];   // a vector image's sharp detail (no mipmaps)
    for (int i = 0; i < 3; i++) {
        glSamplerParameteri(s[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glSamplerParameteri(g_samp_lin, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(g_samp_lin, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") || SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic")) {
        GLfloat mx = 1;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &mx);
        glSamplerParameterf(g_samp_lin, GL_TEXTURE_MAX_ANISOTROPY, MIN(mx, 4.0f));
    }
    glSamplerParameteri(g_samp_pt, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samp_pt, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samp_bil, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_samp_bil, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint mt = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
    if (mt > 0 && mt < g_max_tex) g_max_tex = mt;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &g_max_tbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (!g_png_prog || png_gpu_check()) g_max_tbo = 0;   // PNG rows: converted on the CPU
    return 0;
}

// Textures: reused by size (main thread only).
static struct { GLuint t; int w, h; } g_free_tex[4];
static int g_nfree_tex;

static void tex_put(GLuint t, int w, int h) {
    if (!t) return;
    if (g_nfree_tex == 4) {
        glDeleteTextures(1, &g_free_tex[0].t);
        memmove(g_free_tex, g_free_tex + 1, 3 * sizeof *g_free_tex);
        g_nfree_tex--;
    }
    g_free_tex[g_nfree_tex].t = t;
    g_free_tex[g_nfree_tex].w = w;
    g_free_tex[g_nfree_tex].h = h;
    g_nfree_tex++;
}

static GLuint tex_get(int w, int h) {
    for (int i = 0; i < g_nfree_tex; i++)
        if (g_free_tex[i].w == w && g_free_tex[i].h == h) {
            GLuint t = g_free_tex[i].t;
            g_free_tex[i] = g_free_tex[--g_nfree_tex];
            return t;
        }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);   // (the mipmaps: glGenerateMipmap)
    return t;
}

// An R8 texture of one plane (texture unit `unit`), read with texelFetch or bilinear.
static GLuint plane_texture(int unit, const uint8_t *p, int w, int h, size_t pitch) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pitch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, p);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return t;
}

// PNG rows -> premultiplied BGRA on the CPU (only when they don't fit the GPU's buffer texture).
static int png_to_bgra(Decoded *d) {
    size_t cap, pitch = (size_t)d->width * 4;
    uint8_t *out = buf_get(pitch * d->height, &cap);
    if (!out) return -1;
    int ch = d->ncomp;
    for (int y = 0; y < d->height; y++) {
        const uint8_t *s = d->buf + 1 + (size_t)y * d->pitch[0];
        uint8_t *o = out + (size_t)y * pitch;
        for (int x = 0; x < d->width; x++, s += ch, o += 4) {
            int r = s[0], g = ch >= 3 ? s[1] : r, b = ch >= 3 ? s[2] : r, a = ch == 4 ? s[3] : ch == 2 ? s[1] : 255;
            o[0] = (uint8_t)((b * a + 127) / 255);
            o[1] = (uint8_t)((g * a + 127) / 255);
            o[2] = (uint8_t)((r * a + 127) / 255);
            o[3] = 255;
        }
    }
    buf_put(d->buf, d->cap);
    d->buf = out;
    d->cap = cap;
    d->kind = KIND_BGRA;
    d->pitch[0] = pitch;
    d->size = pitch * d->height;
    return 0;
}

// Upload + convert + mipmaps on the GPU. Main thread.
static GLuint texture_for(Decoded *d) {
    if (d->tex) return d->tex;
    if (!d->buf) return 0;
    if (d->kind == KIND_PNG && d->size > (size_t)MAX(g_max_tbo, 0) && png_to_bgra(d)) return 0;
    d->tex = tex_get(d->width, d->height);
    if (!d->tex) return 0;
    double t0 = now_ms();
    if (d->kind == KIND_BGRA) {   // already the pixels: straight into the texture
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(d->pitch[0] / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, d->width, d->height, GL_BGRA, GL_UNSIGNED_BYTE, d->buf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (g_bench_split) glFinish();
        d->upload_ms = now_ms() - t0;
    } else {
        GLuint pl[4] = {0, 0, 0, 0}, tbo = 0, tbt = 0;
        int kind = 0, n = 0;
        if (d->kind == KIND_PNG) {
            kind = 3;
            glGenBuffers(1, &tbo);
            glBindBuffer(GL_TEXTURE_BUFFER, tbo);
            glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)d->size, d->buf, GL_STATIC_DRAW);
            glGenTextures(1, &tbt);
            glActiveTexture(GL_TEXTURE0 + 5);
            glBindTexture(GL_TEXTURE_BUFFER, tbt);
            glTexBuffer(GL_TEXTURE_BUFFER, GL_R8, tbo);
        } else if (d->kind == KIND_PLANAR) {   // PSD: planes one after the other, width x height each
            kind = 4;
            n = d->ncomp + d->hasa;
            for (int c = 0; c < n; c++)
                pl[c] = plane_texture(1 + c, d->buf + (size_t)c * d->width * d->height, d->width, d->height, (size_t)d->width);
        } else {
            kind = d->ncomp == 1 ? 1 : d->limited ? 5 : 0;
            n = d->ncomp;
            for (int c = 0; c < n; c++) pl[c] = plane_texture(1 + c, d->buf + d->offset[c], d->pw[c], d->ph[c], d->pitch[c]);
        }
        if (kind == 4 || kind == 1) {   // gray: plane 0 three times; PSD transparency in unit 4
            GLuint t1 = d->ncomp == 3 ? pl[1] : pl[0], t2 = d->ncomp == 3 ? pl[2] : pl[0];
            if (kind == 4 && d->hasa && d->ncomp == 1) {
                glActiveTexture(GL_TEXTURE0 + 4);
                glBindTexture(GL_TEXTURE_2D, pl[1]);
            }
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, t1);
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, t2);
        }
        if (g_bench_split) glFinish();
        double t1 = now_ms();
        d->upload_ms = t1 - t0;
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, d->width, d->height);
            if (kind == 3) {
                glUseProgram(g_png_prog);
                glUniform1i(UP.ch, d->ncomp);
                glUniform1i(UP.pitch, (GLint)d->pitch[0]);
            } else {
                glUseProgram(g_conv_prog);
                glUniform1i(U.kind, kind);
                glUniform1i(U.hasa, d->hasa);
                if (kind == 0 || kind == 5)
                    glUniform4f(U.cs, (float)d->h1 / d->hmax, (float)d->v1 / d->vmax, (float)d->pw[1], (float)d->ph[1]);
            }
            glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (g_bench_split) glFinish();
        d->conv_ms = now_ms() - t1;
        glDeleteTextures(4, pl);
        if (tbt) glDeleteTextures(1, &tbt);
        if (tbo) glDeleteBuffers(1, &tbo);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    buf_put(d->buf, d->cap);   // the decoded data is not needed any more
    d->buf = NULL;
    return d->tex;
}

static void decoded_free(Decoded *d) {   // main thread
    if (!d) return;
    tex_put(d->tex, d->width, d->height);
    buf_put(d->buf, d->cap);
#ifdef NV_VECTOR
    if (d->dtex) glDeleteTextures(1, &d->dtex);
    vec_unref(d->vec);
#endif
    free(d);
}

// ---------------------------------------------------------------------------
// decoding (decoder thread)

static int g_nthreads = 0;   // -j
static SDL_AtomicInt g_dng_ev3;   // DNG exposure correction in 1/3 EV (keys E / Shift+E)

// Anything stb_image reads, as premultiplied BGRA (transparent areas black). Huge images are
// scaled down to the GPU's limit. Returns the pixels (pool buffer) or NULL.
static uint8_t *decode_stb(const uint8_t *bytes, size_t len, Decoded *d, size_t *cap) {
    int w = 0, h = 0, n = 0;
    uint8_t *px = len < INT32_MAX ? stbi_load_from_memory(bytes, (int)len, &w, &h, &n, 4) : NULL;
    if (!px) return NULL;
    int k = 1;   // box filter k x k
    while ((w + k - 1) / k > g_max_tex || (h + k - 1) / k > g_max_tex) k++;
    int ow = (w + k - 1) / k, oh = (h + k - 1) / k;
    size_t pitch = (size_t)ow * 4;
    uint8_t *buf = buf_get(pitch * oh, cap);
    if (buf) {
        for (int y = 0; y < oh; y++) {
            uint8_t *o = buf + (size_t)y * pitch;
            for (int x = 0; x < ow; x++, o += 4) {
                unsigned r = 0, g = 0, b = 0, cnt = 0;
                for (int yy = y * k; yy < MIN(h, y * k + k); yy++)
                    for (int xx = x * k; xx < MIN(w, x * k + k); xx++) {
                        const uint8_t *s = px + ((size_t)yy * w + xx) * 4;
                        r += s[0] * s[3];
                        g += s[1] * s[3];
                        b += s[2] * s[3];
                        cnt++;
                    }
                cnt *= 255;
                o[0] = (uint8_t)((b + cnt / 2) / cnt);
                o[1] = (uint8_t)((g + cnt / 2) / cnt);
                o[2] = (uint8_t)((r + cnt / 2) / cnt);
                o[3] = 255;
            }
        }
        d->kind = KIND_BGRA;
        d->width = ow;
        d->height = oh;
        d->pitch[0] = pitch;
        d->size = pitch * oh;
        d->mode = MODE_STB;
        d->bands = 1;
    }
    stbi_image_free(px);
    return buf;
}


#ifdef NV_VECTOR
#ifdef NV_SVG
static int svg_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    (void)page;
    return nitrosvg_render(doc, bgra, w, h, pitch, scale, x0, y0);
}
static void svg_size(void *doc, int page, double *w, double *h) {
    (void)page;
    nitrosvg_size(doc, w, h);
}
static void svg_free(void *doc) { nitrosvg_free(doc); }
#endif
#ifdef NV_CDR
static int cdr_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    return nitrocdr_render(doc, page, bgra, w, h, pitch, scale, x0, y0);
}
static void cdr_size(void *doc, int page, double *w, double *h) { nitrocdr_size(doc, page, w, h); }
static void cdr_free(void *doc) { nitrocdr_free(doc); }
#endif

// An SVG / CDR / CMX file by its name: the document (one reference), else NULL.
static VectorDoc *load_vector(const char *path, const uint8_t *data, size_t len) {
    VectorDoc v = {0};
#ifdef NV_SVG
    static const char *const svg_exts[] = {"svg", NULL};
    if (nv_has_ext(path, svg_exts) && (v.doc = nitrosvg_load(data, len))) {
        v.render = svg_render;
        v.size = svg_size;
        v.free = svg_free;
        v.pages = 1;
        v.mode = MODE_SVG;
    }
#endif
#ifdef NV_CDR
    static const char *const cdr_exts[] = {"cdr", "cmx", NULL};
    if (!v.doc && nv_has_ext(path, cdr_exts) && (v.doc = nitrocdr_load(data, len))) {
        v.render = cdr_render;
        v.size = cdr_size;
        v.free = cdr_free;
        v.pages = nitrocdr_pages(v.doc);
        v.mode = MODE_CDR;
    }
#endif
    if (!v.doc) return NULL;
    VectorDoc *p = malloc(sizeof *p);
    if (!p) { v.free(v.doc); return NULL; }
    *p = v;
    SDL_SetAtomicInt(&p->refs, 1);
    return p;
}

// Renders a page of v as the image of d, in full: the long side 1024..4096 pixels (small icons
// enlarged, huge drawings reduced), at least the fit-to-screen target, over white. Returns the
// pixels (pool buffer) or NULL; d takes a reference to v.
static uint8_t *decode_vector_page(VectorDoc *v, int page, Decoded *d, size_t *cap) {
    double w = 0, h = 0;
    v->size(v->doc, page, &w, &h);
    if (!(w > 0 && h > 0)) return NULL;
    double l = MAX(w, h), k = l < 1024 ? 1024 / l : l > 4096 ? 4096 / l : 1;
    int tw = SDL_GetAtomicInt(&g_vec_tw), th = SDL_GetAtomicInt(&g_vec_th);
    if (tw > 0 && th > 0) k = MAX(k, MIN(tw / w, th / h));
    k = MIN(k, g_max_tex / l);
    int iw = MAX(1, MIN(g_max_tex, (int)lround(w * k))), ih = MAX(1, MIN(g_max_tex, (int)lround(h * k)));
    size_t pitch = (size_t)iw * 4;
    uint8_t *buf = buf_get(pitch * ih, cap);
    if (!buf) return NULL;
    if (v->render(v->doc, page, buf, iw, ih, pitch, k, 0, 0)) {
        buf_put(buf, *cap);
        return NULL;
    }
    d->kind = KIND_BGRA;
    d->width = iw;
    d->height = ih;
    d->pitch[0] = pitch;
    d->size = pitch * ih;
    d->mode = v->mode;
    d->bands = 1;
    d->orientation = 1;
    d->vec = vec_ref(v);
    d->page = page;
    d->vscale = k;
    return buf;
}
#endif


static uint8_t *pool_get(size_t len, size_t *cap, void *ctx) { return buf_get(len, cap); }
static void pool_put(uint8_t *p, size_t cap, void *ctx) { buf_put(p, cap); }

// Decodes one file into host memory (nitro_load's decoders, else stb_image). NULL: cannot be decoded.
static Decoded *decode_file(const char *path) {
    double t0 = now_ms();
    size_t len = 0, cap = 0;
    int raw_orientation, raw_read;
    uint8_t *data = nv_read_image(path, &len, g_max_tex, &raw_orientation, &raw_read), *buf = NULL;
    if (!data) return NULL;
    double t1 = now_ms();
    Decoded *d = calloc(1, sizeof *d);
    if (!d) {
        free(data);
        return NULL;
    }
    d->read_ms = t1 - t0;
    d->orientation = 1;
#ifdef NV_VECTOR
    VectorDoc *vec = load_vector(path, data, len);
    if (vec) {
        buf = decode_vector_page(vec, 0, d, &cap);
        vec_unref(vec);   // (d keeps its own reference)
    } else
#endif
    {
        NvDecoder dec = {pool_get, pool_put, NULL, 64, g_max_tex, g_nthreads, SDL_GetAtomicInt(&g_dng_ev3)};
        NvImage img;
        if (!nv_decode(data, &len, raw_orientation, raw_read, &dec, &img, &buf, &cap)) {
            d->kind = (Kind)img.kind;
            d->width = img.width;
            d->height = img.height;
            d->ncomp = img.ncomp;
            d->orientation = img.orientation;
            d->h1 = img.h[1];
            d->v1 = img.v[1];
            d->hmax = img.hmax;
            d->vmax = img.vmax;
            d->hasa = img.hasa;
            d->limited = img.limited;
            memcpy(d->pw, img.pw, sizeof d->pw);
            memcpy(d->ph, img.ph, sizeof d->ph);
            memcpy(d->offset, img.offset, sizeof d->offset);
            memcpy(d->pitch, img.pitch, sizeof d->pitch);
            d->size = img.size;
            d->mode = img.mode;
            d->bands = img.bands;
            d->ev3 = img.ev3;
        } else {
            d->orientation = img.orientation;   // (a JPEG's, a camera raw's: known from the header)
        }
    }
    if (!buf) buf = decode_stb(data, len, d, &cap);   // everything else, and what the own decoders could not handle
    free(data);
    if (!buf) { free(d); return NULL; }
    d->buf = buf;
    d->cap = cap;
    d->decode_ms = now_ms() - t1;
    return d;
}

// ---------------------------------------------------------------------------
// events from other threads

enum { EV_DECODED, EV_DNG, EV_DETAIL, EV_PAGE, EV_SLIDE, EV_DETAIL_TIMER, EV_OPEN, EV_COUNT };
static Uint32 g_ev_base;

static void post(int ev, int code, void *data) {
    SDL_Event e;
    SDL_zero(e);
    e.type = g_ev_base + (Uint32)ev;
    e.user.code = code;
    e.user.data1 = data;
    SDL_PushEvent(&e);
}

// ---------------------------------------------------------------------------

// image cache + background decoder: nitro_prefetch, with these callbacks

static NvLoader *L;

static int stale_dng(const Decoded *d) { return d && d->mode == MODE_NITRODNG && d->ev3 != SDL_GetAtomicInt(&g_dng_ev3); }

static void *ld_decode(void *path, char **error, void *ctx) { return decode_file(path); }
static void ld_free(void *image, void *ctx) { decoded_free(image); }
static int ld_stale(const void *image, void *ctx) { return stale_dng(image); }
static void *ld_path_dup(void *path, void *ctx) { return SDL_strdup(path); }
static void ld_path_free(void *path, void *ctx) { SDL_free(path); }
static const char *ld_path_name(void *path, void *ctx) { return path; }
static void ld_notify(int index, void *ctx) { post(EV_DECODED, index, NULL); }
static void ld_thread_started(void *ctx) { SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH); }

static int loader_start(char **files, int n) {   // (the loader owns the list from now on)
    static const NvLoaderOps ops = {ld_decode, ld_free, ld_stale, ld_path_dup, ld_path_free, ld_path_name, ld_notify,
                                    ld_thread_started, NULL};
    L = nv_loader_new(&ops, (void **)files, n);
    return L ? 0 : -1;
}

static Decoded *loader_get(int i) { return nv_loader_get(L, i, NULL); }
static int loader_failed(int i) { return nv_loader_failed(L, i, NULL); }

static void loader_reap(void) {   // main thread (textures live here)
    void *dead[64];
    int n;
    while ((n = nv_loader_reap(L, dead, 64)) > 0)
        for (int i = 0; i < n; i++) decoded_free(dead[i]);
}

static int start_thread(SDL_ThreadFunction fn, const char *name, void *arg) {
    SDL_Thread *t = SDL_CreateThread(fn, name, arg);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}

#ifdef NV_VECTOR
// The size vector images are rendered for changed (g_vec_tw): prefetched ones too small for it
// are dropped and rendered again (the current one is sharpened by the detail render).
static int vec_too_small(const void *image, void *ctx) {
    const Decoded *d = image;
    int tw = SDL_GetAtomicInt(&g_vec_tw), th = SDL_GetAtomicInt(&g_vec_th);
    return d->vec && tw > 0 && th > 0 && MIN((double)tw / d->width, (double)th / d->height) > 1.01 &&
           MAX(d->width, d->height) < g_max_tex;
}
static void loader_vector_target_changed(void) { nv_loader_drop(L, vec_too_small, NULL, 1, NULL, NULL); }
#endif

typedef struct {
    Decoded *d;
    unsigned old_serial;          // the image it replaces (matched by serial: it may be freed meanwhile)
    char *path;
} DngJob;

static int SDLCALL dng_run(void *arg) {
    DngJob *j = arg;
    j->d = decode_file(j->path);
    post(EV_DNG, 0, j);
    return 0;
}

// The DNG exposure correction changed (g_dng_ev3): prefetched DNGs are dropped and developed again,
// the current one is replaced when its new development is ready (it stays on screen until then).
static void loader_dng_exposure_changed(void) {
    DngJob *j = calloc(1, sizeof *j);
    void *path = NULL;
    unsigned serial = 0;
    if (nv_loader_drop(L, ld_stale, NULL, j != NULL, &serial, &path) && path) {   // (no job: dropped, decoded again)
        j->old_serial = serial;
        j->path = path;
        if (!start_thread(dng_run, "dng", j)) return;
        j->path = NULL;
    }
    SDL_free(path);
    free(j);
}

// ---------------------------------------------------------------------------
// viewer

typedef struct {
    SDL_Window *win;
    SDL_GLContext gl;
    int vw, vh;                   // drawable size (device pixels)
    char **files;                 // (owned by the loader; same list)
    int nfiles, index, started, lazy_dir;
    double request_time;
    int reported, was_ready, dirty, quit;
    // slideshow
    double slide_ms;
    int paused;
    int shown_index;
    double shown_time;
    SDL_TimerID slide_timer, detail_timer;
    struct { char *path, *to; int index; } *deleted;   // Del: original path, path in .delete/, for undo
    int ndeleted;
    // zoom / pan: kept when paging, so a series can be compared at the same spot
    int zoomed, fit_screen;
    double scale, cx, cy;         // image pixels -> device pixels; image point at the view centre
    int dragging;
    double drag_x, drag_y;
    double page_accum;            // Ctrl + wheel (touchpad): accumulated distance
    // window sizing
    int user_sized, fullscreen;
    double self_resize_time;      // our own resizes arrive later as events: ignored for a while
    FileList dropped;             // drag & drop, until it is complete
} Viewer;

static Viewer V;
static float g_density = 1;       // device pixels per window coordinate, before the window exists

static float density(void) {
    float k = V.win ? SDL_GetWindowPixelDensity(V.win) : g_density;
    return k > 0 ? k : 1;
}

static void display_size(const Decoded *d, double *iw, double *ih) {   // after EXIF rotation
    nv_display_size(d->width, d->height, d->orientation, iw, ih);
}

static void redraw(void) { V.dirty = 1; }

// Work area of the window's display (window coordinates). Wayland doesn't tell it: there a part
// of the screen is left for panels.
static SDL_Rect work_area(void) {
    SDL_DisplayID dpy = V.win ? SDL_GetDisplayForWindow(V.win) : SDL_GetPrimaryDisplay();
    SDL_Rect r = {0, 0, 1280, 800}, full;
    if (!SDL_GetDisplayUsableBounds(dpy, &r)) SDL_GetDisplayBounds(dpy, &r);
    if (SDL_GetDisplayBounds(dpy, &full) && r.w == full.w && r.h == full.h && !strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        r.y += r.h / 24;
        r.h -= r.h / 24;
    }
    return r;
}
static void frame_extra(int *fw, int *fh) {   // the frame around the content (window coordinates)
    int t = 0, l = 0, b = 0, r = 0;
    if (V.win && SDL_GetWindowBordersSize(V.win, &t, &l, &b, &r) && t + b > 0) {
        *fw = l + r;
        *fh = t + b;
    } else {   // (not known yet or not told: a title bar)
        *fw = 0;
        *fh = 40;
    }
}
static void max_client(double *w, double *h) {   // device pixels
    SDL_Rect wa = work_area();
    int fw, fh;
    frame_extra(&fw, &fh);
    float k = density();
    *w = floor((wa.w - fw) * k);
    *h = floor((wa.h - fh) * k);
}

// Content size for an image of w x h pixels: smaller than the screen: exactly 100%
// (unless upscale); larger: its aspect ratio, as large as fits the work area.
static void window_content_size(double w, double h, int upscale, double *cw, double *ch) {
    double mw, mh;
    max_client(&mw, &mh);
    nv_content_size(w, h, mw, mh, upscale, cw, ch);
}

static double fit_scale(const Decoded *d) {
    double iw, ih;
    display_size(d, &iw, &ih);
    return nv_fit_scale(iw, ih, V.vw, V.vh, V.fullscreen || V.fit_screen);   // small images: 100% (unless key 0)
}
static double current_scale(const Decoded *d) { return V.zoomed ? V.scale : fit_scale(d); }

// The "fit" zoom used for snapping: the scale of the normal image-sized window
// (the current one may have shrunk with a zoomed-out image).
static double fit_snap_scale(const Decoded *d) {
    if (V.user_sized || V.fullscreen) return fit_scale(d);
    double iw, ih, cw, ch;
    display_size(d, &iw, &ih);
    window_content_size(iw, ih, V.fit_screen, &cw, &ch);
    return nv_fit_snap_scale(iw, ih, cw, ch, V.fit_screen);
}

static void clamp_center(const Decoded *d, double s) {
    double iw, ih;
    display_size(d, &iw, &ih);
    nv_clamp_center(iw, ih, V.vw, V.vh, s, &V.cx, &V.cy);
}

static void update_title(void) {
    char t[1024];
    if (!V.nfiles) {
        SDL_SetWindowTitle(V.win, "nitroview - drop images here, or O to open files, Shift+O a folder");
        return;
    }
    const char *name = base_name(V.files[V.index]);
    Decoded *d = loader_get(V.index);
    if (d)
        snprintf(t, sizeof t, "%s  %.0f%%%s  (%d/%d)  %dx%d  decode %.1f ms", name, 100 * current_scale(d),
                 V.zoomed ? "" : " (fit)", V.index + 1, V.nfiles, d->width, d->height, d->decode_ms);
    else
        snprintf(t, sizeof t, "%s  (%d/%d)  %s", name, V.index + 1, V.nfiles,
                 loader_failed(V.index) ? "CANNOT DECODE (unsupported or damaged)" : "loading...");
    size_t n = strlen(t);
#ifdef NV_VECTOR
    if (d && d->vec && d->vec->pages > 1) n += (size_t)snprintf(t + n, sizeof t - n, "  [page %d/%d]", d->page + 1, d->vec->pages);
#endif
    if (d && d->mode == MODE_NITRODNG && d->ev3) n += (size_t)snprintf(t + n, sizeof t - n, "  %+.1f EV", d->ev3 / 3.0);
    if (V.slide_ms > 0) {
        if (V.paused) snprintf(t + n, sizeof t - n, "   [slideshow paused: P]");
        else snprintf(t + n, sizeof t - n, "   [slideshow %g ms]", V.slide_ms);
    }
    SDL_SetWindowTitle(V.win, t);
}

// The window follows the image: in fit mode its size (window_content_size); zoomed
// in, as large as the image at the current zoom, up to the screen. Keeps its centre
// and stays on screen. Not after a manual resize (until W) and not in full screen.
static void size_window_for(const Decoded *d) {
    if (!d || V.user_sized || V.fullscreen || (SDL_GetWindowFlags(V.win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED))) return;
    double iw, ih, cw, ch;
    display_size(d, &iw, &ih);
    if (V.zoomed) {
        double mw, mh;
        max_client(&mw, &mh);
        cw = MAX(320, MIN(mw, floor(iw * V.scale)));
        ch = MAX(200, MIN(mh, floor(ih * V.scale)));
    } else {
        window_content_size(iw, ih, V.fit_screen, &cw, &ch);
    }
    if ((int)cw == V.vw && (int)ch == V.vh) return;
    float k = density();
    int w = (int)lround(cw / k), h = (int)lround(ch / k), x, y, ow, oh, fw, fh;
    SDL_GetWindowPosition(V.win, &x, &y);
    SDL_GetWindowSize(V.win, &ow, &oh);
    SDL_Rect wa = work_area();
    frame_extra(&fw, &fh);
    x += ow / 2 - w / 2;
    y += oh / 2 - h / 2;
    if (x + w > wa.x + wa.w) x = wa.x + wa.w - w;
    if (x < wa.x) x = wa.x;
    if (y + h + fh > wa.y + wa.h) y = wa.y + wa.h - h - fh;
    if (y - fh < wa.y) y = wa.y + fh;   // (the position is the content's: the title bar is above it)
    V.self_resize_time = now_ms();
    SDL_SetWindowSize(V.win, w, h);
    SDL_SetWindowPosition(V.win, x, y);   // (Wayland: the compositor places windows)
}

static void fit_window_to(const Decoded *d) {   // paging: only in fit mode
    if (!V.zoomed) size_window_for(d);
}

static void zoom_fit(void) {
    V.zoomed = 0;
    Decoded *d = loader_get(V.index);
    if (d) size_window_for(d);
    update_title();
    redraw();
}

// Fitted to the screen (full screen or key 0): vector images are rendered for its size.
static void update_vector_target(void) {
#ifdef NV_VECTOR
    int tw = 0, th = 0;
    if (V.win && (V.fullscreen || V.fit_screen)) {
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(V.win));
        if (m) {
            tw = (int)lround(m->w * m->pixel_density);
            th = (int)lround(m->h * m->pixel_density);
        }
    }
    if (tw == SDL_GetAtomicInt(&g_vec_tw) && th == SDL_GetAtomicInt(&g_vec_th)) return;
    SDL_SetAtomicInt(&g_vec_tw, tw);
    SDL_SetAtomicInt(&g_vec_th, th);
    loader_vector_target_changed();
#endif
}

static void fit_to_screen(void) {
    V.fit_screen = 1;
    update_vector_target();
    zoom_fit();
}

static void window_to_image(void) {
    V.user_sized = 0;
    V.fit_screen = 0;
    update_vector_target();
    zoom_fit();
    Decoded *d = loader_get(V.index);
    if (d) fit_window_to(d);
}

// Sets the zoom so that the image point under (ax, ay) (device pixels from the
// view centre, y down) stays under it.
static void set_scale(double ns, const Decoded *d, double ax, double ay) {
    double fit = fit_snap_scale(d);
    if (fabs(ns - fit) < 1e-9 && fabs(ns - 1.0) > 1e-9) { zoom_fit(); return; }
    double iw, ih;
    display_size(d, &iw, &ih);
    double cur = current_scale(d);
    if (V.zoomed) {
        clamp_center(d, V.scale);
    } else {
        V.cx = iw / 2;
        V.cy = ih / 2;
    }
    nv_zoom_anchor(cur, ns, ax, ay, &V.cx, &V.cy);
    V.zoomed = 1;
    V.scale = ns;
    clamp_center(d, ns);
    size_window_for(d);
    update_title();
    redraw();
}

// continuous: touchpad, many small steps (no 2% snap, it would swallow them)
static void zoom_by(double f, const Decoded *d, double ax, double ay, int continuous) {
    double fit = fit_snap_scale(d);
    set_scale(nv_zoom_step(current_scale(d), fit, f, continuous), d, ax, ay);
}

static void pan_pixels(double dx, double dy, const Decoded *d) {
    if (!V.zoomed) return;
    clamp_center(d, V.scale);
    V.cx -= dx / V.scale;
    V.cy -= dy / V.scale;
    clamp_center(d, V.scale);
    redraw();
}
static void pan(double fx, double fy, const Decoded *d) { pan_pixels(-fx * V.vw, -fy * V.vh, d); }

static void toggle_actual_size(const Decoded *d, double ax, double ay) {
    if (V.zoomed) zoom_fit();
    else set_scale(1.0, d, ax, ay);
}

static void go(int i, int dir) {
    if (!V.nfiles) return;
    i = MAX(0, MIN(i, V.nfiles - 1));   // stop at the first/last image, no rollover
    if (i == V.index && V.started) return;
    V.started = 1;
    V.index = i;
    V.request_time = now_ms();
    V.reported = 0;
    Decoded *ready = loader_get(i);
    V.was_ready = ready != NULL;
    nv_loader_focus(L, i, dir);
    if (ready) fit_window_to(ready);
    update_title();
    redraw();
}

// One file was given: when the user pages, list its folder; the current image
// keeps its decoded data and becomes its place in the folder.
static void expand_directory(void) {
    if (!V.lazy_dir) return;
    V.lazy_dir = 0;
    const char *path = V.files[V.index], *name = base_name(path);
    char *dir = SDL_strndup(path, (size_t)(name - path));
    FileList l = {0};
    list_dir(&l, dir, NULL, 0);
    SDL_free(dir);
    int k = -1;
    for (int i = 0; i < l.n; i++)
        if (!strcmp(base_name(l.v[i]), name)) k = i;
    if (k < 0 || l.n < 2) { fl_free(&l); return; }
    SDL_free(l.v[k]);
    l.v[k] = SDL_strdup(path);   // keep the path exactly as given
    if (V.shown_index == V.index) V.shown_index = k;
    nv_loader_replace(L, (void **)l.v, l.n, V.index, k);
    V.files = l.v;
    V.nfiles = l.n;
    V.index = k;
    update_title();
}

static void page(int delta) {
    if (!V.nfiles) return;
    expand_directory();
    go(V.index + delta, delta >= 0 ? 1 : -1);
}

// Del: the current file goes to the (hidden) ".delete" folder next to it: nothing is lost (undo
// with U / Ctrl+Z, or move it back by hand). The next image follows; the last one is not deleted.
static void delete_current(void) {
    if (!V.nfiles) return;
    expand_directory();
    void *more = V.nfiles >= 2 ? realloc(V.deleted, (size_t)(V.ndeleted + 1) * sizeof *V.deleted) : NULL;
    if (!more) return;
    V.deleted = more;
    const char *path = V.files[V.index], *name = base_name(path), *ext = strrchr(name, '.');
    if (!ext) ext = name + strlen(name);
    char *bin = NULL, *to = NULL;
    SDL_asprintf(&bin, "%.*s.delete", (int)(name - path), path);
    if (!bin) return;
    SDL_CreateDirectory(bin);
    SDL_asprintf(&to, "%s/%s", bin, name);
    SDL_PathInfo info;
    for (int k = 2; to && SDL_GetPathInfo(to, &info); k++) {   // (never over an earlier one)
        SDL_free(to);
        to = NULL;
        SDL_asprintf(&to, "%s/%.*s (%d)%s", bin, (int)(ext - name), name, k, ext);
    }
    SDL_free(bin);
    if (!to || !SDL_RenamePath(path, to)) {
        fprintf(stderr, "cannot move %s: %s\n", path, SDL_GetError());
        SDL_free(to);
        return;
    }
    int i = V.index;
    V.deleted[V.ndeleted].path = SDL_strdup(path);
    V.deleted[V.ndeleted].to = to;
    V.deleted[V.ndeleted].index = i;
    V.ndeleted++;
    nv_loader_remove(L, i);   // (frees path)
    V.nfiles--;
    V.shown_index = V.shown_index == i ? -1 : V.shown_index > i ? V.shown_index - 1 : V.shown_index;
    V.started = 0;
    go(MIN(i, V.nfiles - 1), 1);
}

// Undo the last Del: the file goes back to its place in the folder and in the list.
static void undo_delete(void) {
    if (!V.ndeleted) return;
    char *path = V.deleted[V.ndeleted - 1].path, *to = V.deleted[V.ndeleted - 1].to;
    int i = MIN(V.deleted[V.ndeleted - 1].index, V.nfiles);
    SDL_PathInfo info;
    if (!path || SDL_GetPathInfo(path, &info) || !SDL_RenamePath(to, path)) return;   // (something is there again, or it was moved away)
    if (nv_loader_insert(L, i, path)) {
        SDL_RenamePath(path, to);
        return;
    }
    SDL_free(to);
    V.ndeleted--;
    V.files = (char **)nv_loader_files(L, NULL);
    V.nfiles++;
    if (V.shown_index >= i) V.shown_index++;
    if (V.index >= i) V.index++;
    V.started = 0;
    go(i, 1);
}
static void page_first(void) { if (V.nfiles) { expand_directory(); go(0, 1); } }
static void page_last(void) { if (V.nfiles) { expand_directory(); go(V.nfiles - 1, -1); } }

static void open_files(FileList *l, int lazy) {   // drag & drop, the Open dialog
    if (!l->n) return;
    for (int k = 0; k < V.ndeleted; k++) {   // (undo: positions in the old list; the files stay in .delete)
        SDL_free(V.deleted[k].path);
        SDL_free(V.deleted[k].to);
    }
    V.ndeleted = 0;
    V.lazy_dir = lazy;
    V.files = l->v;
    V.nfiles = l->n;
    V.started = 0;
    V.index = 0;
    V.shown_index = -1;
    nv_loader_replace(L, (void **)l->v, l->n, -1, 0);
    l->v = NULL;
    l->n = l->cap = 0;
    go(0, 1);
}

static void open_paths(char **args, int n) {
    FileList l = {0};
    collect_files(&l, args, n);
    int lazy = n == 1 && !strpbrk(args[0], "*?") && !is_dir(args[0]);
    if (l.n) {
        SDL_RestoreWindow(V.win);
        open_files(&l, lazy);
        SDL_RaiseWindow(V.win);
    }
    fl_free(&l);
}

static void SDLCALL dialog_done(void *userdata, const char *const *files, int filter) {   // (any thread)
    (void)userdata;
    (void)filter;
    if (!files || !files[0]) return;   // error / cancelled
    FileList *l = calloc(1, sizeof *l);
    for (int i = 0; l && files[i]; i++) fl_add(l, files[i]);
    if (l) post(EV_OPEN, 0, l);
}

static void open_dialog(int folder) {   // (SDL has no dialog for files and folders at once)
    static char pattern[512];
    if (!pattern[0]) {
        nv_ext_list(pattern, sizeof pattern - 32, "", ";");
        for (int i = 0; g_more_exts[i]; i++) {
            strcat(pattern, ";");
            strcat(pattern, g_more_exts[i]);
        }
    }
    const SDL_DialogFileFilter f[] = {{"Images", pattern}, {"All files", "*"}};
    if (folder) SDL_ShowOpenFolderDialog(dialog_done, NULL, V.win, NULL, false);
    else SDL_ShowOpenFileDialog(dialog_done, NULL, V.win, f, 2, NULL, true);
}

// Slideshow: the next image comes slide_ms after the current one was drawn.
static Uint32 SDLCALL timer_post(void *userdata, SDL_TimerID id, Uint32 interval) {
    (void)id;
    (void)interval;
    post((int)(intptr_t)userdata, 0, NULL);
    return 0;
}
static void schedule_slide(void) {
    if (V.slide_ms > 0 && !V.paused) expand_directory();
    if (V.slide_timer) SDL_RemoveTimer(V.slide_timer);
    V.slide_timer = 0;
    if (V.slide_ms <= 0 || V.paused || V.index >= V.nfiles - 1) return;   // stops at the last image
    double wait = V.slide_ms - (now_ms() - V.shown_time);
    V.slide_timer = SDL_AddTimer((Uint32)MAX(wait, 1), timer_post, (void *)(intptr_t)EV_SLIDE);
}

static void image_decoded(int i) {
    Decoded *d = loader_get(i);
    if (!d) {
        if (i == V.index) {   // show the error, clear the old image; slideshow moves on
            update_title();
            redraw();
            if (V.shown_index != V.index) { V.shown_index = V.index; V.shown_time = now_ms(); schedule_slide(); }
        }
        return;
    }
    if (i == V.index) {
        fit_window_to(d);
        update_title();
        redraw();
    } else if (abs(i - V.index) <= 1 && !d->tex) {
        texture_for(d);   // convert neighbours ahead of time, so paging only has to draw
        glFlush();
    }
}

typedef struct { float origin[2], size[2]; int orientation, nearest; } DrawParams;

static DrawParams draw_params(const Decoded *d) {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = current_scale(d);
    if (V.zoomed) clamp_center(d, s);
    double cx = V.zoomed ? V.cx : iw / 2, cy = V.zoomed ? V.cy : ih / 2;
    double x0 = round(V.vw / 2.0 - cx * s), y0 = round(V.vh / 2.0 - cy * s);
    DrawParams p = {{(float)(x0 / V.vw * 2 - 1), (float)(1 - y0 / V.vh * 2)},
                    {(float)(2 * iw * s / V.vw), (float)(2 * ih * s / V.vh)},
                    d->orientation, s >= 2 && s == floor(s)};
    return p;
}

static void draw_quad(GLuint tex, const DrawParams *p, GLuint sampler) {
    glUseProgram(g_draw_prog);
    glUniform2f(UD.origin, p->origin[0], p->origin[1]);
    glUniform2f(UD.size, p->size[0], p->size[1]);
    glUniform1i(UD.orientation, p->orientation);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, sampler);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 3, 4);
    glBindSampler(0, 0);
}

#ifdef NV_VECTOR
// A vector image zoomed in beyond its decoded size (more than one device pixel per decoded pixel):
// the visible part is rendered again at screen resolution ~80 ms after the zoom or position last
// changed (a background thread), and drawn over the image; until then the previous one is scaled.
typedef struct {
    double ox, oy;               // the image's top left corner on the screen (device pixels)
    double dx, dy, dw, dh;       // the visible part of the image (decoded pixels)
    double scale;                // device pixels per decoded pixel
    int w, h;                    // its size on the screen
} DetailRect;

static int detail_target(const Decoded *d, DetailRect *r) {   // (after draw_params: same centre)
    if (!d->vec) return 0;
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = current_scale(d);
    if (s <= 1.01) return 0;
    double cx = V.zoomed ? V.cx : iw / 2, cy = V.zoomed ? V.cy : ih / 2;
    r->ox = round(V.vw / 2.0 - cx * s);
    r->oy = round(V.vh / 2.0 - cy * s);
    double l = MAX(0, r->ox), t = MAX(0, r->oy), rr = MIN(V.vw, r->ox + iw * s), b = MIN(V.vh, r->oy + ih * s);
    r->w = (int)ceil(rr - l);
    r->h = (int)ceil(b - t);
    if (r->w <= 0 || r->h <= 0) return 0;
    r->scale = s;
    r->dx = (l - r->ox) / s;
    r->dy = (t - r->oy) / s;
    r->dw = r->w / s;
    r->dh = r->h / s;
    return 1;
}
static int detail_current(const Decoded *d, const DetailRect *r) {
    return d->dtex && d->dscale == r->scale && d->dx == r->dx && d->dy == r->dy && d->dw == r->dw && d->dh == r->dh;
}

typedef struct {
    Decoded *d;                  // (identified with vec and page: d itself may be gone by then)
    VectorDoc *vec;              // (a reference: the document stays while it renders)
    int page;
    double vscale;
    DetailRect r;
    uint8_t *px;
    double ms;
} DetailJob;
static int g_detail_busy;   // (main thread)

static int SDLCALL detail_run(void *arg) {
    DetailJob *j = arg;
    double t = now_ms(), k = j->vscale * j->r.scale;
    j->px = malloc((size_t)j->r.w * j->r.h * 4);
    if (j->px && j->vec->render(j->vec->doc, j->page, j->px, j->r.w, j->r.h, (size_t)j->r.w * 4, k, j->r.dx / j->vscale,
                                j->r.dy / j->vscale)) {
        free(j->px);
        j->px = NULL;
    }
    j->ms = now_ms() - t;
    post(EV_DETAIL, 0, j);
    return 0;
}

static void start_detail(void) {   // the detail timer
    Decoded *d = loader_get(V.index);
    DetailRect r;
    if (g_detail_busy || !d || !detail_target(d, &r) || detail_current(d, &r)) return;
    DetailJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->d = d;
    j->vec = vec_ref(d->vec);
    j->page = d->page;
    j->vscale = d->vscale;
    j->r = r;
    if (start_thread(detail_run, "detail", j)) {
        vec_unref(j->vec);
        free(j);
        return;
    }
    g_detail_busy = 1;
}

static void detail_done(DetailJob *j) {
    g_detail_busy = 0;
    Decoded *d = loader_get(V.index);
    if (j->px && d == j->d && d->vec == j->vec && d->page == j->page) {
        if (!d->dtex) glGenTextures(1, &d->dtex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->dtex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, j->r.w, j->r.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, j->px);
        d->dscale = j->r.scale;
        d->dx = j->r.dx;
        d->dy = j->r.dy;
        d->dw = j->r.dw;
        d->dh = j->r.dh;
        if (getenv("NV_DEBUG_SVG")) printf("vector detail %dx%d at %.0f%%: %.1f ms\n", j->r.w, j->r.h, 100 * j->r.scale, j->ms);
    }
    free(j->px);
    vec_unref(j->vec);
    free(j);
    redraw();   // (a view changed meanwhile asks for its own detail)
}

// Multi-page documents (CorelDRAW): [ and ] draw another page in the background; it replaces the
// shown image (the document is shared, only the page is drawn).
typedef struct {
    Decoded *d;
    unsigned old_serial;          // the image it replaces (matched by serial: it may be freed meanwhile)
    VectorDoc *vec;
    int page, index;
} PageJob;

static int SDLCALL page_run(void *arg) {
    PageJob *j = arg;
    Decoded *d = calloc(1, sizeof *d);
    size_t cap = 0;
    double t1 = now_ms();
    uint8_t *buf = d ? decode_vector_page(j->vec, j->page, d, &cap) : NULL;
    if (buf) {
        d->buf = buf;
        d->cap = cap;
        d->decode_ms = now_ms() - t1;
    } else {
        free(d);
        d = NULL;
    }
    j->d = d;
    post(EV_PAGE, 0, j);
    return 0;
}

static void show_page(int delta) {
    unsigned serial;
    Decoded *d = nv_loader_get(L, V.index, &serial);
    if (!d || !d->vec || d->page + delta < 0 || d->page + delta >= d->vec->pages) return;
    PageJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->old_serial = serial;
    j->vec = vec_ref(d->vec);
    j->page = d->page + delta;
    j->index = V.index;
    if (start_thread(page_run, "page", j)) {
        vec_unref(j->vec);
        free(j);
    }
}

static void page_done(PageJob *j) {
    if (j->d && nv_loader_replace_image(L, j->old_serial, j->d) == V.index) {
        fit_window_to(j->d);   // (zoomed in: zoom and position stay, as when paging images)
        update_title();
        redraw();
    }
    vec_unref(j->vec);
    free(j);
    loader_reap();
}
#endif

static void render(void) {
    if (V.vw <= 0 || V.vh <= 0) return;
    loader_reap();
    Decoded *d = V.nfiles ? loader_get(V.index) : NULL;
    GLuint tex = d ? texture_for(d) : 0;
    // not decoded yet (a slow HEIC, a slideshow faster than decoding): the previous image stays
    // on screen until it is, rather than a black window (a failed one is not waited for)
    int previous = 0;
    if (!tex && V.shown_index >= 0 && V.shown_index != V.index && !loader_failed(V.index)) {
        Decoded *o = loader_get(V.shown_index);
        if (o && (tex = texture_for(o))) {
            d = o;
            previous = 1;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, V.vw, V.vh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex) {
        if (!previous && V.shown_index != V.index) {
            V.shown_index = V.index;
            V.shown_time = now_ms();
            schedule_slide();
        }
        DrawParams p = draw_params(d);
        draw_quad(tex, &p, p.nearest ? g_samp_pt : g_samp_lin);
#ifdef NV_VECTOR
        DetailRect r;
        if (!previous && detail_target(d, &r)) {
            if (d->dtex) {   // the sharp detail over its part of the image, scaled to the current view
                double s = r.scale, l = r.ox + d->dx * s, t = r.oy + d->dy * s;
                DrawParams q = {{(float)(l / V.vw * 2 - 1), (float)(1 - t / V.vh * 2)},
                                {(float)(2 * d->dw * s / V.vw), (float)(2 * d->dh * s / V.vh)}, 1, 0};
                draw_quad(d->dtex, &q, g_samp_bil);
            }
            if (!detail_current(d, &r)) {   // (each change postpones it)
                if (V.detail_timer) SDL_RemoveTimer(V.detail_timer);
                V.detail_timer = SDL_AddTimer(80, timer_post, (void *)(intptr_t)EV_DETAIL_TIMER);
            }
        }
#endif
    }
    SDL_GL_SwapWindow(V.win);
    if (tex && !previous && !V.reported) {
        V.reported = 1;
        printf("[%3d] %-40s %5dx%-5d read %5.1f ms  decode %6.1f ms (%-8s %3d bands)  key->on screen %6.1f ms%s\n",
               V.index + 1, base_name(V.files[V.index]), d->width, d->height, d->read_ms, d->decode_ms,
               mode_name(d->mode), d->bands, now_ms() - V.request_time, V.was_ready ? "  (prefetched)" : "");
        fflush(stdout);
    }
}

static void toggle_fullscreen(void) {
    V.self_resize_time = now_ms();
    SDL_SetWindowFullscreen(V.win, !V.fullscreen);   // (the window events follow)
}

static void fullscreen_changed(int on) {
    V.fullscreen = on;
    V.self_resize_time = now_ms();
    if (!on) {
        Decoded *d = loader_get(V.index);
        if (d) size_window_for(d);
    }
    if (on || V.slide_ms > 0) SDL_DisableScreenSaver();
    else SDL_EnableScreenSaver();
    update_vector_target();
    update_title();
    redraw();
}

static void anchor_of(float x, float y, double *ax, double *ay) {   // window coordinates -> device pixels from the view centre
    float k = density();
    *ax = x * k - V.vw / 2.0;
    *ay = y * k - V.vh / 2.0;
}

static void on_char(char ch) {   // zoom keys by character: any keyboard layout, the numeric keypad too
    Decoded *cur = V.nfiles ? loader_get(V.index) : NULL;
    switch (ch) {
    case '+': case '=': if (cur) zoom_by(M_SQRT2, cur, 0, 0, 0); break;
    case '-': case '_': if (cur) zoom_by(M_SQRT1_2, cur, 0, 0, 0); break;
    case '0': fit_to_screen(); break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':   // 100% .. 800%
        if (cur) set_scale(ch - '0', cur, 0, 0);
        break;
    case 'w': case 'W': window_to_image(); break;
    case 'u': case 'U': undo_delete(); break;
    case 'e': case 'E':   // DNG: brighter / (E) darker, 1/3 EV
        if (cur && cur->mode == MODE_NITRODNG) {
            int ev3 = MAX(-9, MIN(9, SDL_GetAtomicInt(&g_dng_ev3) + (ch == 'E' ? -1 : 1)));
            if (ev3 != SDL_GetAtomicInt(&g_dng_ev3)) {
                SDL_SetAtomicInt(&g_dng_ev3, ev3);
                loader_dng_exposure_changed();
            }
        }
        break;
#ifdef NV_VECTOR
    case '[': show_page(-1); break;   // the pages of a multi-page CorelDRAW drawing
    case ']': show_page(1); break;
#endif
    default: break;
    }
}

static void on_key(SDL_Keycode k, SDL_Keymod mod) {
    Decoded *cur = V.nfiles ? loader_get(V.index) : NULL;
    double step = (mod & SDL_KMOD_SHIFT) ? 0.5 : 0.125;
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
        if (k == SDLK_Z) undo_delete();
        else if (k == SDLK_O) open_dialog((mod & SDL_KMOD_SHIFT) != 0);
        else if (k == SDLK_Q || k == SDLK_W) V.quit = 1;
        return;
    }
    switch (k) {
    case SDLK_LEFT: if (cur) pan(-step, 0, cur); break;
    case SDLK_RIGHT: if (cur) pan(step, 0, cur); break;
    case SDLK_UP: if (cur) pan(0, -step, cur); break;
    case SDLK_DOWN: if (cur) pan(0, step, cur); break;
    case SDLK_PAGEDOWN: case SDLK_SPACE: page(1); break;
    case SDLK_PAGEUP: case SDLK_BACKSPACE: page(-1); break;
    case SDLK_DELETE: delete_current(); break;
    case SDLK_HOME: page_first(); break;
    case SDLK_END: page_last(); break;
    case SDLK_F: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_F11: toggle_fullscreen(); break;
    case SDLK_O: open_dialog((mod & SDL_KMOD_SHIFT) != 0); break;   // Shift: a folder
    case SDLK_ESCAPE:
        if (V.fullscreen) toggle_fullscreen();
        else V.quit = 1;
        break;
    case SDLK_Q: V.quit = 1; break;
    case SDLK_P:   // pause slideshow
        if (V.slide_ms > 0) {
            V.paused = !V.paused;
            V.shown_time = now_ms();
            if (V.paused && V.slide_timer) {
                SDL_RemoveTimer(V.slide_timer);
                V.slide_timer = 0;
            } else if (!V.paused) {
                schedule_slide();
            }
            update_title();
        }
        break;
    }
}

static void on_wheel(const SDL_MouseWheelEvent *e) {
    float y = e->direction == SDL_MOUSEWHEEL_FLIPPED ? -e->y : e->y;   // > 0: away from the user
    if (y == 0) return;
    int notch = e->integer_y != 0 && y == (float)(int)y;
    // Ctrl + wheel pages (forward: previous)
    if (SDL_GetModState() & SDL_KMOD_CTRL) {
        if (notch) {
            page(y < 0 ? 1 : -1);
            return;
        }
        V.page_accum += y;   // touchpad: one image per notch's worth
        while (V.page_accum <= -1) { V.page_accum += 1; page(1); }
        while (V.page_accum >= 1) { V.page_accum -= 1; page(-1); }
        return;
    }
    Decoded *d = V.nfiles ? loader_get(V.index) : NULL;
    if (!d) return;
    double ax, ay;
    anchor_of(e->mouse_x, e->mouse_y, &ax, &ay);
    zoom_by(pow(M_SQRT2, y), d, ax, ay, !notch);   // one sqrt(2) step per notch
}

static void window_resized(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(V.win, &w, &h);
    if (w != V.vw || h != V.vh) {
        V.vw = w;
        V.vh = h;
        if (V.nfiles) update_title();   // the fit percentage changes with the window size
    }
    redraw();
}

static void handle(const SDL_Event *e) {
    Decoded *d;
    double ax, ay;
    if (e->type >= g_ev_base && e->type < g_ev_base + EV_COUNT) {
        switch ((int)(e->type - g_ev_base)) {
        case EV_DECODED:
            loader_reap();
            if (e->user.code >= 0 && V.nfiles) image_decoded(e->user.code);
            break;
        case EV_DNG: {
            DngJob *j = e->user.data1;
            if (j->d && nv_loader_replace_image(L, j->old_serial, j->d) == V.index) {
                update_title();
                redraw();
            }
            SDL_free(j->path);
            free(j);
            loader_reap();
            break;
        }
#ifdef NV_VECTOR
        case EV_DETAIL: detail_done(e->user.data1); break;
        case EV_PAGE: page_done(e->user.data1); break;
        case EV_DETAIL_TIMER: V.detail_timer = 0; start_detail(); break;
#endif
        case EV_SLIDE:
            V.slide_timer = 0;
            if (!V.paused) page(1);
            break;
        case EV_OPEN: {
            FileList *l = e->user.data1;
            open_paths(l->v, l->n);
            fl_free(l);
            free(l);
            break;
        }
        }
        return;
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: V.quit = 1; break;
    case SDL_EVENT_WINDOW_EXPOSED: redraw(); break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: window_resized(); break;
    case SDL_EVENT_WINDOW_RESIZED:
        // a resize we didn't ask for (edge drag, tiling) is the user's choice
        if (!V.fullscreen && V.started && now_ms() - V.self_resize_time > 1000) V.user_sized = 1;
        window_resized();
        break;
    case SDL_EVENT_WINDOW_MAXIMIZED: if (V.started) V.user_sized = 1; break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: fullscreen_changed(1); break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: fullscreen_changed(0); break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: update_vector_target(); break;
    case SDL_EVENT_TEXT_INPUT:
        for (const char *c = e->text.text; *c; c++)
            if (!(*c & 0x80)) on_char(*c);
        break;
    case SDL_EVENT_KEY_DOWN: on_key(e->key.key, e->key.mod); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (e->button.clicks == 2) {   // fit -> 100% at the clicked point, otherwise back to fit
                if (V.nfiles && (d = loader_get(V.index))) {
                    anchor_of(e->button.x, e->button.y, &ax, &ay);
                    toggle_actual_size(d, ax, ay);
                }
                break;
            }
            V.dragging = 1;
            V.drag_x = e->button.x;
            V.drag_y = e->button.y;
            static SDL_Cursor *move;   // (made once: SDL keeps cursors until destroyed)
            if (V.zoomed && (move || (move = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE)))) SDL_SetCursor(move);
        } else if (e->button.button == SDL_BUTTON_RIGHT) {
            page((SDL_GetModState() & SDL_KMOD_SHIFT) ? -1 : 1);
        } else if (e->button.button == SDL_BUTTON_X1) {
            page(-1);   // back / forward
        } else if (e->button.button == SDL_BUTTON_X2) {
            page(1);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (V.dragging && (e->motion.state & SDL_BUTTON_LMASK)) {
            float k = density();
            if (V.nfiles && (d = loader_get(V.index))) pan_pixels((e->motion.x - V.drag_x) * k, (e->motion.y - V.drag_y) * k, d);
            V.drag_x = e->motion.x;
            V.drag_y = e->motion.y;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            V.dragging = 0;
            SDL_SetCursor(SDL_GetDefaultCursor());
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: on_wheel(&e->wheel); break;
    case SDL_EVENT_PINCH_UPDATE:   // touchpad pinch: zoom around the cursor
        if (V.nfiles && (d = loader_get(V.index)) && e->pinch.scale > 0) {
            float x, y;
            SDL_GetMouseState(&x, &y);
            anchor_of(x, y, &ax, &ay);
            zoom_by(e->pinch.scale, d, ax, ay, 1);
        }
        break;
    case SDL_EVENT_DROP_BEGIN: fl_free(&V.dropped); break;
    case SDL_EVENT_DROP_FILE: if (e->drop.data) fl_add(&V.dropped, e->drop.data); break;
    case SDL_EVENT_DROP_COMPLETE:
        if (V.dropped.n) open_paths(V.dropped.v, V.dropped.n);
        fl_free(&V.dropped);
        break;
    }
}

static void set_icon(void) {
    int w, h, n;
    uint8_t *px = stbi_load_from_memory(icon_png, (int)icon_png_len, &w, &h, &n, 4);
    if (!px) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
    if (s) {
        SDL_SetWindowIcon(V.win, s);
        SDL_DestroySurface(s);
    }
    stbi_image_free(px);
}

// The window (hidden for the test modes) and its OpenGL 3.3 context. first: the first image, for
// the window's size.
static int create_window(const char *first, int hidden) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);   // (macOS)
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    // first size: from the header, so the window doesn't jump when the image arrives
    float k = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const char *drv = SDL_GetCurrentVideoDriver();   // (Retina, a scaled Wayland output: device pixels per window
    if (k > 0 && drv && (!strcmp(drv, "cocoa") || !strcmp(drv, "wayland"))) g_density = k;   // unit; X11: pixels)
    double w, h, cw, ch;
    if (first && nv_header_size(first, &w, &h)) {   // (the image's size from the file header: the window's size)
        window_content_size(w, h, 0, &cw, &ch);
    } else {
        max_client(&cw, &ch);
        cw = floor(cw * 0.84);
        ch = floor(ch * 0.84);
    }
    V.win = SDL_CreateWindow("nitroview", (int)lround(cw / g_density), (int)lround(ch / g_density),
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!V.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return -1; }
    V.gl = SDL_GL_CreateContext(V.win);
    if (!V.gl) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return -1; }
    SDL_GL_MakeCurrent(V.win, V.gl);
    SDL_GL_SetSwapInterval(1);
    if (gpu_init()) return -1;
    if (getenv("NV_DEBUG_GL")) printf("OpenGL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    if (hidden) return 0;
    set_icon();
    V.self_resize_time = now_ms();
    SDL_ShowWindow(V.win);
    SDL_GetWindowSizeInPixels(V.win, &V.vw, &V.vh);
    SDL_StartTextInput(V.win);
    return 0;
}

// ---------------------------------------------------------------------------
// test modes

static int run_bench(FileList *fl) {
    double tr = 0, td = 0, tu = 0, tg = 0, tc = 0, t0 = now_ms();
    g_bench_split = 1;
    for (int i = 0; i < fl->n; i++) {
        Decoded *d = decode_file(fl->v[i]);
        if (!d) { printf("FAILED %s\n", fl->v[i]); continue; }
        double g0 = now_ms();
        texture_for(d);
        glFinish();
        double g = now_ms() - g0;
        printf("%-36.36s %5dx%-5d read %5.1f  decode %6.1f  upload %5.1f  convert %5.1f  mips %5.1f ms  (%s)\n",
               base_name(fl->v[i]), d->width, d->height, d->read_ms, d->decode_ms, d->upload_ms, d->conv_ms,
               g - d->upload_ms - d->conv_ms, mode_name(d->mode));
        tc += d->conv_ms;
        tr += d->read_ms;
        td += d->decode_ms;
        tu += d->upload_ms;
        tg += g;
        decoded_free(d);
    }
    double n = fl->n;
    printf("\n%d files, average ms/image: read %.1f, decode %.1f, upload to the GPU %.1f, convert %.1f, mipmaps %.1f\n"
           "total %.1f ms, avg %.1f ms/image\n",
           fl->n, tr / n, td / n, tu / n, tc / n, (tg - tu - tc) / n, now_ms() - t0, (now_ms() - t0) / n);
    return 0;
}

// Compares the GPU conversion with stb_image's decoding of the same file (premultiplied, like the
// viewer). JPEG: stb_image rounds and upsamples a little differently (small differences are normal).
static int run_selftest(FileList *fl) {
    int bad = 0;
    for (int i = 0; i < fl->n; i++) {
        const char *name = base_name(fl->v[i]);
        Decoded *d = decode_file(fl->v[i]);
        if (!d) { printf("%-30.30s FAILED to decode\n", name); bad++; continue; }
        int kind = d->kind, mode = d->mode;   // (texture_for may make PNG rows BGRA)
        int alpha = kind == KIND_PNG ? d->ncomp == 2 || d->ncomp == 4 : kind == KIND_PLANAR ? d->hasa : 1;   // transparency?
        GLuint tex = texture_for(d);
        uint8_t *g = tex ? malloc((size_t)d->width * d->height * 4) : NULL;
        if (!g) {
            printf("%-30.30s GPU error\n", name);
            bad++;
            decoded_free(d);
            continue;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glReadPixels(0, 0, d->width, d->height, GL_RGBA, GL_UNSIGNED_BYTE, g);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        size_t len;
        uint8_t *file = nv_read_file(fl->v[i], &len);
        int rw = 0, rh = 0, rn = 0;
        uint8_t *ref = file && len < INT32_MAX ? stbi_load_from_memory(file, (int)len, &rw, &rh, &rn, 4) : NULL;
        int png16 = file && len > 24 && !memcmp(file, "\x89PNG", 4) && file[24] == 16;   // (stb_image truncates to 8 bits, we round)
        free(file);
        if (!ref || rw != d->width || rh != d->height) {
            printf("%-30.30s %-8s no stb_image reference (%s)\n", name, mode_name(mode), ref ? "other size" : "stb_image can't decode it");
        } else {
            long hist[256] = {0};
            int maxd = 0;
            double sum = 0;
            size_t cnt = 0, np = (size_t)d->width * d->height;
            for (size_t p = 0; p < np; p++) {
                const uint8_t *q = ref + p * 4;
                for (int c = 0; c < 3; c++) {
                    int e = abs((int)g[p * 4 + c] - (alpha ? (q[c] * q[3] + 127) / 255 : q[c]));
                    hist[e]++;
                    sum += e;
                    cnt++;
                    if (e > maxd) maxd = e;
                }
            }
            long le1 = hist[0] + hist[1], le3 = le1 + hist[2] + hist[3];
            double mean = sum / cnt, p3 = 100.0 * le3 / cnt;
            // (a PSD's extra channel that isn't transparency: stb_image takes it for alpha and removes the
            // white matte where it isn't 255, so those pixels differ)
            int unmatted = kind == KIND_PLANAR && !alpha && rn == 4;
            int ok = kind == KIND_YUV || unmatted || png16 ? mean <= 1.0 && p3 >= 99.0 : maxd <= 1;
            printf("%-30.30s %-8s max diff %3d  mean %.3f  <=1: %8.4f%%  <=3: %8.4f%%  %s\n", name, mode_name(mode), maxd,
                   mean, 100.0 * le1 / cnt, p3, ok ? "OK" : "DIFFERENT");
            if (!ok) bad++;
        }
        stbi_image_free(ref);
        free(g);
        decoded_free(d);
    }
    printf(bad ? "SELFTEST: %d problem(s)\n" : "SELFTEST OK\n", bad);
    return bad != 0;
}

// ---------------------------------------------------------------------------
// desktop integration (--register / --unregister): a desktop file and the icon for the current
// user, so the file manager offers it ("Open with") and the window gets its icon (Linux)

#define APP_ID "com.github.gereoffy.nitroview.sdl"

#ifdef __linux__
static char *data_home(const char *sub) {   // $XDG_DATA_HOME/sub, by default ~/.local/share/sub
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    char *p = NULL;
    if (x && *x) SDL_asprintf(&p, "%s/%s", x, sub);
    else if (h) SDL_asprintf(&p, "%s/.local/share/%s", h, sub);
    return p;
}
#endif

static int do_register(int on) {
#ifdef __linux__
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *apps = data_home("applications"), *icons = data_home("icons/hicolor/512x512/apps");
    if (n <= 0 || !apps || !icons) { fprintf(stderr, "cannot find the program or the home directory\n"); return 1; }
    exe[n] = 0;
    char *desktop = NULL, *icon = NULL;
    SDL_asprintf(&desktop, "%s/" APP_ID ".desktop", apps);
    SDL_asprintf(&icon, "%s/" APP_ID ".png", icons);
    int bad = 0;
    if (on) {
        char q[8192], *o = q;   // the path as a desktop file wants it: quoted, \ " ` $ escaped
        *o++ = '"';
        for (const char *c = exe; *c && o < q + sizeof q - 4; c++) {
            if (strchr("\\\"`$", *c)) *o++ = '\\';
            *o++ = *c;
        }
        *o++ = '"';
        *o = 0;
        char *text = NULL;
        SDL_asprintf(&text,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=NitroView\n"
            "Comment=Very fast image viewer (SDL)\n"
            "Exec=%s %%F\n"
            "Icon=" APP_ID "\n"
            "Terminal=false\n"
            "Categories=Graphics;Viewer;\n"
            "MimeType=image/jpeg;image/png;image/tiff;image/gif;image/bmp;image/webp;image/heic;image/heif;image/avif;image/x-tga;"
            "image/vnd.adobe.photoshop;image/x-adobe-dng;image/x-canon-cr2;image/x-nikon-nef;image/x-sony-arw;image/x-fuji-raf;"
#ifdef NV_SVG
            "image/svg+xml;"
#endif
#ifdef NV_CDR
            "image/x-coreldraw;"
#endif
            "\n"
            "StartupNotify=true\n", q);
        bad = !text || !SDL_CreateDirectory(apps) || !SDL_CreateDirectory(icons) || !SDL_SaveFile(desktop, text, strlen(text)) ||
              !SDL_SaveFile(icon, icon_png, icon_png_len);
        SDL_free(text);
    } else {
        SDL_RemovePath(desktop);
        SDL_RemovePath(icon);
    }
    char *cmd = NULL;   // (the desktop's caches; not every system has these tools)
    SDL_asprintf(&cmd, "update-desktop-database '%s' >/dev/null 2>&1; gtk-update-icon-cache -q -t '%s/../..' >/dev/null 2>&1", apps, icons);
    if (cmd && !strchr(apps, '\'') && !strchr(icons, '\'')) {
        int r = system(cmd);
        (void)r;
    }
    SDL_free(cmd);
    if (bad) fprintf(stderr, "could not write %s: %s\n", desktop, SDL_GetError());
    else if (on) printf("NitroView is registered: %s\n(it runs %s; after moving it, run --register again; --unregister removes it)\n", desktop, exe);
    else printf("NitroView is unregistered.\n");
    SDL_free(desktop);
    SDL_free(icon);
    SDL_free(apps);
    SDL_free(icons);
    return bad;
#else
    (void)on;
    fprintf(stderr, "--register: only on Linux\n");
    return 1;
#endif
}

// ---------------------------------------------------------------------------

static void usage(void) {
    fprintf(stderr,
        "usage: nitroview-sdl [options] file.jpg|directory|*.png ...\n"
        "\n"
        "options:\n"
        "  -f, --fullscreen      start in full screen\n"
        "  -s, --slideshow MS    slideshow: next image after MS milliseconds on screen\n"
        "                        (waits for slow images, stops at the last one; P pauses)\n"
        "  -j N                  use N decoder threads (default: all logical CPUs; progressive JPEG: 4)\n"
        "  --bench               load all files (a hidden window), print timings\n"
        "  --selftest            compare the GPU output with stb_image's decoding\n"
        "  --register            add nitroview to the desktop's applications (\"Open with\"), current user\n"
        "  --unregister          remove that again\n"
        "  -h, --help            this help\n"
        "\n"
        "keys:\n"
        "  PgDn Space              next image         PgUp Backspace           previous\n"
        "  Home / End              first / last       F / Enter / F11          full screen\n"
        "  + / -                   zoom in / out      0 fit to screen          1 actual size (1:1)\n"
        "  2 .. 8                  200%% .. 800%%, pixel-exact (1 image pixel = NxN screen pixels)\n"
        "  arrows                  move a zoomed image (Shift: bigger steps)\n"
        "  W                       back to the image's own size (small images 100%%, window follows)\n"
        "  E / Shift+E             DNG raw image: brighter / darker (1/3 EV steps, kept while paging)\n"
        "  Del                     move the file to the .delete folder next to it   U / Ctrl+Z  undo\n"
        "  O / Ctrl+O              open files         Shift+O                  open a folder\n"
        "mouse:\n"
        "  drag                    move a zoomed image\n"
        "  wheel                   zoom around the cursor (touchpad: smooth, pinch too)\n"
        "  double click            fit <-> 100%% at the clicked point\n"
        "  right click             next image (Shift: previous)\n"
        "  Ctrl + wheel            previous / next image\n"
        "  side buttons            previous / next image\n"
        "  P                       pause/resume slideshow\n"
        "  Esc                     leave full screen / quit                Q   quit\n");
}

int main(int argc, char **argv) {
    int full = 0, bench = 0, selftest = 0, help = 0;
    double slide_ms = 0;
    char **args = calloc((size_t)argc + 1, sizeof *args);
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) full = 1;
        else if (!strcmp(a, "--bench")) bench = 1;
        else if (!strcmp(a, "--selftest")) selftest = 1;
        else if (!strcmp(a, "-j") && i + 1 < argc) { g_nthreads = atoi(argv[++i]); nj_set_max_workers(g_nthreads); }
        else if ((!strcmp(a, "-s") || !strcmp(a, "--slideshow")) && i + 1 < argc) slide_ms = atof(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) help = 1;
        else if (!strcmp(a, "-n") || !strcmp(a, "--new-window")) {}   // (the other viewers' option: always a new window here)
        else if (!strcmp(a, "--register")) return do_register(1);
        else if (!strcmp(a, "--unregister")) return do_register(0);
        else args[nargs++] = argv[i];
    }
    if (help) { usage(); return 1; }
    g_pool_lock = SDL_CreateMutex();
    SDL_SetAppMetadata("NitroView", "1.0", APP_ID);   // (Wayland: the desktop file's icon, see --register)
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    FileList fl = {0};
    collect_files(&fl, args, nargs);
    if ((bench || selftest) && !fl.n) { usage(); return 1; }
    g_ev_base = SDL_RegisterEvents(EV_COUNT);
    if (create_window(fl.n ? fl.v[0] : NULL, bench || selftest)) return 1;
    if (bench) return run_bench(&fl);
    if (selftest) return run_selftest(&fl);

    if (loader_start(fl.v, fl.n)) { fprintf(stderr, "cannot start the decoder thread\n"); return 1; }
    V.files = fl.v;
    V.nfiles = fl.n;
    V.shown_index = -1;
    if (fl.n) {
        V.lazy_dir = nargs == 1 && !strpbrk(args[0], "*?") && !is_dir(args[0]);
        go(0, 1);
    } else {
        update_title();
        open_dialog(0);
    }
    if (full) toggle_fullscreen();
    if (slide_ms > 0) {
        V.slide_ms = slide_ms;
        SDL_DisableScreenSaver();
        update_title();
    }
    while (!V.quit) {
        SDL_Event e;
        if (SDL_WaitEvent(&e)) handle(&e);
        while (!V.quit && SDL_PollEvent(&e)) handle(&e);
        if (V.dirty && !V.quit) {
            V.dirty = 0;
            render();
        }
    }
    fflush(stdout);
    SDL_DestroyWindow(V.win);
    _Exit(0);   // (the decoder thread may be busy)
}
