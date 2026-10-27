// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroview for Windows: the same viewer as nitroview.m (macOS), on Win32 + Direct3D 11.
//
//   nitroview [-f] [-s ms] [-j threads] file.jpg [more files, directories or wildcards...]
//   nitroview --bench files...     decode everything, print timings, no window
//   nitroview --selftest files...  compare the GPU output with Windows' own (WIC) decoding
//
// Keys, mouse and window behaviour as on macOS (see README): PgDn/Space next,
// PgUp/Backspace previous, Home/End, +/- zoom, 0 fit to screen, 1..8 zoom 100..800%,
// W window to image, arrows pan, F/Enter full screen, P pause slideshow, Esc/Q quit,
// [ / ] the pages of a multi-page CorelDRAW drawing.
//
// Pipeline: file -> nitrojpeg / nitropng / nitropsd / nitroheic / nitrowebp / nitrodng / nitrotiff / nitroavif
// (multi-threaded, into a buffer the decoder thread hands to Direct3D) -> compute shader (upsampling,
// YCbCr->RGB, PNG rows, PSD planes) -> mipmaps -> trilinear scaled draw.
// Anything else goes through WIC (damaged or CMYK JPEG, JPEG-compressed TIFF, GIF, BMP ...).
// SVG / CorelDRAW (optional, NV_SVG / NV_CDR): nitrosvg / nitrocdr render it once in full as
// BGRA, and the visible part again, sharp, when zoomed in (as nitroview.m does).
// Neighbouring images are decoded ahead in the background.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "nitrojpeg.h"
#include "nitropng.h"
#include "nitropsd.h"
#include "nitroheic.h"
#include "nitrodng.h"
#include "nitrowebp.h"
#include "nitrotiff.h"
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

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
#endif
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define MAX_TEX 16384
#define AHEAD 3     // images decoded ahead in the paging direction
#define BEHIND 2    // ... and kept / decoded behind
#define WM_APP_DECODED (WM_APP + 1)
#define WM_APP_DETAIL (WM_APP + 2)   // a vector image's sharp detail is rendered
#define WM_APP_PAGE (WM_APP + 3)     // another page of a multi-page document is drawn
#define WM_APP_DNG (WM_APP + 4)      // a DNG is developed again (another exposure correction)
#define WM_APP_OPEN (WM_APP + 5)     // started without files: the open dialog
#define TIMER_SLIDE 1
#define TIMER_DETAIL 2

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static char *utf8(const wchar_t *w) {   // for printing (rotating buffers, per thread: the decoder prints too)
    static __thread char buf[4][1024];
    static __thread int k;
    char *b = buf[k++ & 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof buf[0], NULL, NULL)) b[sizeof buf[0] - 1] = 0;   // (too long: cut)
    return b;
}

// _snwprintf that always ends the string (_snwprintf does not when it cuts). Returns -1 if cut.
static int swfmt(wchar_t *b, size_t n, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf(b, n, fmt, ap);
    va_end(ap);
    b[n - 1] = 0;
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *n = p;
    for (const wchar_t *c = p; *c; c++)
        if (*c == L'\\' || *c == L'/') n = c + 1;
    return n;
}

// ---------------------------------------------------------------------------
// decoded image

typedef enum { KIND_YUV = 0, KIND_BGRA = 1, KIND_PNG = 2, KIND_PLANAR = 3 } Kind;
enum { MODE_WIC = -3, MODE_NITROPNG = -4, MODE_NITROPSD = -5, MODE_SVG = -6, MODE_CDR = -7, MODE_NITROHEVC = -8, MODE_NITRODNG = -9, MODE_NITROWEBP = -10, MODE_NITROTIFF = -11, MODE_NITROAVIF = -12 };

#ifdef NV_VECTOR
// A loaded vector document (SVG, CorelDRAW), shared by the images of its pages and the background
// renders of their sharp detail: reference counted, freed by whichever drops it last.
typedef struct {
    void *doc;
    int (*render)(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0);
    void (*size)(void *doc, int page, double *w, double *h);
    void (*free)(void *doc);
    int pages, mode;
    volatile LONG refs;
} VectorDoc;
static VectorDoc *vec_ref(VectorDoc *v) {
    if (v) InterlockedIncrement(&v->refs);
    return v;
}
static void vec_unref(VectorDoc *v) {
    if (v && !InterlockedDecrement(&v->refs)) {
        v->free(v->doc);
        free(v);
    }
}
// Shown fitted to the screen (full screen or key 0), the main thread sets the screen's size in
// pixels here: vector images are then rendered at least that large (the prefetched ones too),
// sharp from the start, not first enlarged and sharpened a moment later by the detail render.
static volatile LONG g_vec_tw, g_vec_th;
#endif

typedef struct {
    Kind kind;
    int width, height, ncomp, orientation;
    int h1, v1, hmax, vmax, hasa;      // chroma sampling; PSD: transparency plane
    int pw[3], ph[3];
    size_t offset[3], pitch[3];
    size_t size;                       // bytes of image data
    ID3D11Buffer *raw;                 // the decoded data on the GPU (created by the decoder thread):
    ID3D11ShaderResourceView *rawsrv;  //   PNG / BGRA as a raw buffer,
    ID3D11Texture2D *pl[4];            //   JPEG / PSD planes as R8 textures (Y Cb Cr / R G B A)
    ID3D11ShaderResourceView *plsrv[4];
    ID3D11Texture2D *tex;              // converted RGBA + mips (main thread)
    ID3D11ShaderResourceView *texsrv;
    double read_ms, decode_ms, upload_ms, conv_ms;
    int mode, bands;
    unsigned serial;              // (unique: the image a page / DNG job's result replaces)
    int ev3;                           // a DNG: the exposure correction it was developed with (1/3 EV)
    int limited;                       // KIND_YUV: limited range (Y 16..235, as WebP), not full range
#ifdef NV_VECTOR
    VectorDoc *vec;                    // SVG / CDR: the document (this is its page `page`)
    int page;
    double vscale;                     // decoded pixels per document unit
    ID3D11Texture2D *dtex;             // sharp detail (main thread): the visible part rendered at screen
    ID3D11ShaderResourceView *dsrv;    // resolution, over the rectangle dx, dy, dw, dh of the image
    double dx, dy, dw, dh, dscale;     // (decoded pixels), at dscale device pixels per decoded pixel
#endif
} Decoded;

static volatile LONG g_serial;
static unsigned next_serial(void) { return (unsigned)InterlockedIncrement(&g_serial); }

static const char *mode_name(int m) {
    switch (m) {
    case 3: return "prog";
    case 2: return "split";
    case 1: return "RST";
    case 0: return "single";
    case MODE_WIC: return "wic";
    case MODE_NITROPNG: return "nitropng";
    case MODE_NITROPSD: return "nitropsd";
    case MODE_NITROHEVC: return "nitrohevc";
    case MODE_NITRODNG: return "nitrodng";
    case MODE_NITROWEBP: return "nitrowebp";
    case MODE_NITROTIFF: return "nitrotiff";
    case MODE_NITROAVIF: return "nitroavif";
    case MODE_SVG: return "svg";
    case MODE_CDR: return "cdr";
    default: return "?";
    }
}

// Host buffers the decoders write into: 50-100 MB each, reused (page faults are expensive).
static SRWLOCK g_pool_lock = SRWLOCK_INIT;
static struct { uint8_t *p; size_t cap; } g_free_bufs[4];
static int g_nfree;

static uint8_t *buf_get(size_t len, size_t *cap) {
    AcquireSRWLockExclusive(&g_pool_lock);
    int best = -1;
    for (int i = 0; i < g_nfree; i++)
        if (g_free_bufs[i].cap >= len && g_free_bufs[i].cap <= len * 2 && (best < 0 || g_free_bufs[i].cap < g_free_bufs[best].cap))
            best = i;
    if (best >= 0) {
        uint8_t *p = g_free_bufs[best].p;
        *cap = g_free_bufs[best].cap;
        g_free_bufs[best] = g_free_bufs[--g_nfree];
        ReleaseSRWLockExclusive(&g_pool_lock);
        return p;
    }
    ReleaseSRWLockExclusive(&g_pool_lock);
    *cap = align_up(len, 64);
    return _aligned_malloc(*cap + 64, 64);   // + slack: the GPU view is a multiple of 16 bytes
}

static void buf_put(uint8_t *p, size_t cap) {
    if (!p) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    if (g_nfree == 4) {   // drop the oldest
        _aligned_free(g_free_bufs[0].p);
        memmove(g_free_bufs, g_free_bufs + 1, 3 * sizeof *g_free_bufs);
        g_nfree--;
    }
    g_free_bufs[g_nfree].p = p;
    g_free_bufs[g_nfree].cap = cap;
    g_nfree++;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

// ---------------------------------------------------------------------------
// files

static uint8_t *read_file(const wchar_t *path, size_t *len) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *d = NULL;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart < ((uint64_t)1 << 40)) {
        size_t n = (size_t)sz.QuadPart, got = 0;
        d = malloc(n + 64);   // decoders may look a few bytes ahead
        while (d && got < n) {
            DWORD chunk = (DWORD)MIN(n - got, (size_t)1 << 30), r = 0;
            if (!ReadFile(f, d + got, chunk, &r, NULL) || !r) { free(d); d = NULL; break; }
            got += r;
        }
        if (d) { memset(d + n, 0, 64); *len = n; }
    }
    CloseHandle(f);
    return d;
}

static int pread_all(HANDLE f, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        OVERLAPPED o = {0};
        o.Offset = (DWORD)(off + got);
        o.OffsetHigh = (DWORD)((off + got) >> 32);
        DWORD r = 0, chunk = (DWORD)MIN(n - got, (size_t)1 << 30);
        if (!ReadFile(f, (uint8_t *)buf + got, chunk, &r, &o) || !r) return -1;
        got += r;
    }
    return 0;
}

// PSD/PSB: reads only what the merged image needs (see nitroview.m): header +
// colour mode data + image resources + an empty layer section that keeps the
// layer count's sign + the merged image. NULL if it is not a sane PSD.
static uint8_t *read_psd_merged(const wchar_t *path, size_t *len) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    uint8_t *out = NULL, h[26], b[8];
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || pread_all(f, h, 26, 0) || memcmp(h, "8BPS", 4)) goto done;
    {
        uint64_t size = (uint64_t)sz.QuadPart, pos = 26;
        int psb = h[5] == 2;
        size_t L = psb ? 8 : 4;
        for (int k = 0; k < 2; k++) {
            if (pread_all(f, b, 4, pos)) goto done;
            pos += 4 + ((uint64_t)b[0] << 24 | (uint64_t)b[1] << 16 | (uint64_t)b[2] << 8 | b[3]);
            if (pos > size) goto done;
        }
        uint64_t head = pos, lm = 0;
        if (pread_all(f, b, L, pos)) goto done;
        for (size_t i = 0; i < L; i++) lm = lm << 8 | b[i];
        uint64_t x = pos + L + lm;
        if (lm > size || x + 2 > size) goto done;
        int16_t count = 0;
        if (lm >= L + 2) {
            uint8_t c[10];
            if (pread_all(f, c, L + 2, pos + L)) goto done;
            uint64_t li = 0;
            for (size_t i = 0; i < L; i++) li = li << 8 | c[i];
            if (li >= 2) count = (int16_t)(c[L] << 8 | c[L + 1]);
        }
        uint64_t merged = size - x, total = head + L + L + 2 + merged;
        out = malloc((size_t)total + 64);
        if (!out) goto done;
        if (pread_all(f, out, (size_t)head, 0)) { free(out); out = NULL; goto done; }
        uint8_t *q = out + head;
        uint64_t slim = L + 2;
        for (size_t i = 0; i < L; i++) q[i] = (uint8_t)(slim >> (8 * (L - 1 - i)));
        for (size_t i = 0; i < L; i++) q[L + i] = (uint8_t)(2 >> (8 * (L - 1 - i)));
        q[2 * L] = (uint8_t)((uint16_t)count >> 8);
        q[2 * L + 1] = (uint8_t)count;
        if (pread_all(f, q + 2 * L + 2, (size_t)merged, x)) { free(out); out = NULL; goto done; }
        memset(out + total, 0, 64);
        *len = (size_t)total;
    }
done:
    CloseHandle(f);
    return out;
}

static int has_ext(const wchar_t *path, const wchar_t *const *exts) {
    const wchar_t *dot = wcsrchr(base_name(path), L'.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++)
        if (!_wcsicmp(dot + 1, exts[i])) return 1;
    return 0;
}
static const wchar_t *const g_exts[] = {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"heic", L"heif", L"avif", L"tif", L"tiff",
                                        L"webp", L"gif", L"bmp", L"psd", L"psb", L"cr2", L"nef", L"nrw", L"arw", L"raf", L"dng",
#ifdef NV_SVG
                                        L"svg",
#endif
#ifdef NV_CDR
                                        L"cdr", L"cmx",
#endif
                                        NULL};

typedef struct { wchar_t **v; int n, cap; } FileList;

static void fl_add(FileList *l, const wchar_t *p) {
    if (l->n == l->cap) l->v = realloc(l->v, (size_t)(l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->v);
    l->v[l->n++] = _wcsdup(p);
}
static void fl_free(FileList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}
static int cmp_logical(const void *a, const void *b) {   // Explorer order: "img2" before "img10"
    return StrCmpLogicalW(base_name(*(wchar_t *const *)a), base_name(*(wchar_t *const *)b));
}

// dir\pattern -> matching image files (or any file for an explicit wildcard), Explorer order
static void list_dir(FileList *out, const wchar_t *dir, const wchar_t *pattern, int any_ext) {
    wchar_t q[MAX_PATH * 2];
    size_t dl = wcslen(dir);
    int sep = dl && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/');
    if (swfmt(q, MAX_PATH * 2, L"%ls%ls%ls", dir, dl && !sep ? L"\\" : L"", pattern)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(q, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int first = out->n;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!any_ext && !has_ext(fd.cFileName, g_exts)) continue;
        wchar_t p[MAX_PATH * 2];
        if (!swfmt(p, MAX_PATH * 2, L"%ls%ls%ls", dir, dl && !sep ? L"\\" : L"", fd.cFileName)) fl_add(out, p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(out->v + first, (size_t)(out->n - first), sizeof *out->v, cmp_logical);
}

// Arguments -> files: directories are listed, wildcards expanded (cmd.exe doesn't).
static void collect_files(FileList *out, wchar_t **args, int n) {
    for (int i = 0; i < n; i++) {
        const wchar_t *a = args[i];
        if (wcspbrk(a, L"*?")) {
            const wchar_t *name = base_name(a);
            wchar_t dir[MAX_PATH * 2];
            size_t dl = (size_t)(name - a);
            if (dl >= MAX_PATH * 2) continue;
            wmemcpy(dir, a, dl);
            dir[dl] = 0;
            int before = out->n;
            list_dir(out, dl ? dir : L"", name, 1);
            if (out->n == before) fprintf(stderr, "no match: %s\n", utf8(a));
            continue;
        }
        DWORD at = GetFileAttributesW(a);
        if (at == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "no such file: %s\n", utf8(a)); continue; }
        if (at & FILE_ATTRIBUTE_DIRECTORY) list_dir(out, a, L"*", 0);
        else fl_add(out, a);
    }
}

// ---------------------------------------------------------------------------
// GPU

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11ComputeShader *g_cs, *g_cs_tex;
static ID3D11VertexShader *g_vs, *g_conv_vs;
static ID3D11PixelShader *g_ps, *g_conv_ps;
static int g_convert_draw;   // NV_CONVERT=draw: planes -> RGBA as a draw instead of a compute shader
static int g_bench_split;    // --bench: time the conversion and the mipmaps separately
static ID3D11Buffer *g_cb_conv, *g_cb_draw;
static ID3D11SamplerState *g_samp_lin, *g_samp_pt, *g_samp_bil;
static ID3D11RasterizerState *g_rast;

static const char g_hlsl[] =
    "cbuffer Conv : register(b0) {\n"
    "    uint4 A;    // width, height, kind, hasa\n"
    "    float4 CS;  // cscale.xy\n"
    "    int4 CM;    // chroma plane size - 1\n"
    "    uint4 OFF;  // plane offsets, alpha offset\n"
    "    uint4 PI;   // pitches\n"
    "};\n"
    "ByteAddressBuffer src : register(t0);\n"
    "RWTexture2D<unorm float4> outt : register(u0);\n"
    "uint B(uint a) { return (src.Load(a & ~3u) >> ((a & 3u) * 8u)) & 255u; }\n"
    "float chroma(uint base, uint pitch, float2 c) {\n"
    "    float2 f = c - 0.5;\n"
    "    int2 i0 = int2(floor(f));\n"
    "    float2 w = f - float2(i0);\n"
    "    int2 a = clamp(i0, int2(0, 0), CM.xy), b = clamp(i0 + 1, int2(0, 0), CM.xy);\n"
    "    float v00 = B(base + a.y * pitch + a.x), v01 = B(base + a.y * pitch + b.x);\n"
    "    float v10 = B(base + b.y * pitch + a.x), v11 = B(base + b.y * pitch + b.x);\n"
    "    return lerp(lerp(v00, v01, w.x), lerp(v10, v11, w.x), w.y) * (1.0 / 255.0);\n"
    "}\n"
    "[numthreads(16, 16, 1)]\n"
    "void convert(uint3 id : SV_DispatchThreadID) {\n"
    "    uint2 g = id.xy;\n"
    "    if (g.x >= A.x || g.y >= A.y) return;\n"
    "    float4 c;\n"
    "    if (A.z == 2) {            // BGRA (premultiplied)\n"
    "        uint q = g.y * PI.x + g.x * 4;\n"
    "        c = float4(B(q + 2), B(q + 1), B(q), 255) * (1.0 / 255.0);\n"
    "    } else if (A.z == 4) {     // PSD planes (gray: all offsets equal)\n"
    "        uint i = g.y * PI.x + g.x;\n"
    "        float3 v = float3(B(OFF.x + i), B(OFF.y + i), B(OFF.z + i));\n"
    "        if (A.w) v = max(v + float(B(OFF.w + i)) - 255.0, 0.0);   // matted with white -> over black\n"
    "        c = float4(v * (1.0 / 255.0), 1.0);\n"
    "    } else if (A.z == 3) {     // PNG rows after the filter byte; OFF.y = channels; over black\n"
    "        uint ch = OFF.y, q = OFF.x + g.y * PI.x + g.x * ch;\n"
    "        float4 v;\n"
    "        if (ch == 4) v = float4(B(q), B(q + 1), B(q + 2), B(q + 3));\n"
    "        else if (ch == 3) v = float4(B(q), B(q + 1), B(q + 2), 255);\n"
    "        else if (ch == 2) { float t = B(q); v = float4(t, t, t, B(q + 1)); }\n"
    "        else { float t = B(q); v = float4(t, t, t, 255); }\n"
    "        v *= 1.0 / 255.0;\n"
    "        c = float4(v.rgb * v.a, 1.0);\n"
    "    } else {\n"
    "        float y = B(OFF.x + g.y * PI.x + g.x) * (1.0 / 255.0);\n"
    "        if (A.z == 1) {\n"
    "            c = float4(y, y, y, 1.0);\n"
    "        } else {                // same \"fancy upsampling\" as libjpeg (linear, centred)\n"
    "            float2 cc = (float2(g) + 0.5) * CS.xy;\n"
    "            float cb = chroma(OFF.y, PI.y, cc) - 128.0 / 255.0;\n"
    "            float cr = chroma(OFF.z, PI.z, cc) - 128.0 / 255.0;\n"
    "            if (A.z == 5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); cb *= 255.0 / 224.0; cr *= 255.0 / 224.0; }\n"
    "            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);\n"
    "        }\n"
    "    }\n"
    "    outt[g] = saturate(c);\n"
    "}\n"
    "// JPEG / PSD planes as textures: Y and PSD channels are read, chroma comes through the\n"
    "// sampler's bilinear filter (the same centred linear upsampling as libjpeg's \"fancy\" one)\n"
    "Texture2D<float> P0 : register(t2);\n"
    "Texture2D<float> P1 : register(t3);\n"
    "Texture2D<float> P2 : register(t4);\n"
    "Texture2D<float> P3 : register(t5);\n"
    "SamplerState sbil : register(s2);\n"
    "float4 planes_rgb(uint2 g) {\n"
    "    float4 c;\n"
    "    int3 at = int3(g, 0);\n"
    "    if (A.z == 4) {             // PSD: R G B (gray: the same plane three times), transparency\n"
    "        float3 v = float3(P0.Load(at), P1.Load(at), P2.Load(at));\n"
    "        if (A.w) v = max(v + P3.Load(at) - 1.0, 0.0);   // matted with white -> over black\n"
    "        c = float4(v, 1.0);\n"
    "    } else {\n"
    "        float y = P0.Load(at);\n"
    "        if (A.z == 1) {\n"
    "            c = float4(y, y, y, 1.0);\n"
    "        } else {\n"
    "            float2 uv = (float2(g) + 0.5) * CS.xy / CS.zw;   // CS.zw: chroma plane size\n"
    "            float cb = P1.SampleLevel(sbil, uv, 0) - 128.0 / 255.0;\n"
    "            float cr = P2.SampleLevel(sbil, uv, 0) - 128.0 / 255.0;\n"
    "            if (A.z == 5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); cb *= 255.0 / 224.0; cr *= 255.0 / 224.0; }\n"
    "            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);\n"
    "        }\n"
    "    }\n"
    "    return saturate(c);\n"
    "}\n"
    "[numthreads(16, 16, 1)]\n"
    "void convert_tex(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < A.x && id.y < A.y) outt[id.xy] = planes_rgb(id.xy);\n"
    "}\n"
    "// the same as a draw into the texture (render target writes: faster on some GPUs)\n"
    "float4 conv_vs(uint vid : SV_VertexID) : SV_Position {   // one triangle over the whole target\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 conv_ps(float4 pos : SV_Position) : SV_Target { return planes_rgb(uint2(pos.xy)); }\n"
    "cbuffer Draw : register(b1) { float2 origin; float2 size; uint orientation; uint nearest; uint2 pad; };\n"
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VOut vmain(uint vid : SV_VertexID) {\n"
    "    float2 q = float2(vid & 1, vid >> 1);   // 0..1 display space, y down\n"
    "    VOut o;\n"
    "    o.pos = float4(origin.x + q.x * size.x, origin.y - q.y * size.y, 0, 1);\n"
    "    float u = q.x, v = q.y;\n"
    "    float2 t = float2(u, v);\n"
    "    if (orientation == 2) t = float2(1 - u, v);   // EXIF orientation\n"
    "    else if (orientation == 3) t = float2(1 - u, 1 - v);\n"
    "    else if (orientation == 4) t = float2(u, 1 - v);\n"
    "    else if (orientation == 5) t = float2(v, u);\n"
    "    else if (orientation == 6) t = float2(v, 1 - u);\n"
    "    else if (orientation == 7) t = float2(1 - v, 1 - u);\n"
    "    else if (orientation == 8) t = float2(1 - v, u);\n"
    "    o.uv = t;\n"
    "    return o;\n"
    "}\n"
    "Texture2D tex : register(t1);\n"
    "SamplerState slin : register(s0);\n"
    "SamplerState spt : register(s1);   // integer zoom: exact pixel blocks\n"
    "float4 pmain(VOut i) : SV_Target {\n"
    "    return nearest ? tex.SampleLevel(spt, i.uv, 0) : tex.Sample(slin, i.uv);\n"
    "}\n";

typedef struct {
    uint32_t a[4];     // width, height, kind, hasa
    float cs[4];
    int32_t cm[4];
    uint32_t off[4];
    uint32_t pitch[4];
} ConvParams;

typedef struct {
    float origin[2];   // NDC position of the image's top-left corner
    float size[2];     // NDC extent
    uint32_t orientation;
    uint32_t nearest;  // 1: nearest pixel (integer zoom >= 200%)
    uint32_t pad[2];
} DrawParams;

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3DCompile(g_hlsl, sizeof g_hlsl - 1, "nitroview", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "shader %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return blob;
}

static ID3D11Buffer *make_cb(UINT size) {
    D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &b);
    return b;
}

static int gpu_init(void) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))   // e.g. an old driver without 11.1
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                               D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {   // no usable GPU (remote session, VM): WARP, Direct3D in software
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                               D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
        if (SUCCEEDED(hr)) fprintf(stderr, "no Direct3D 11 GPU: using WARP (software, slow)\n");
    }
    if (FAILED(hr)) { fprintf(stderr, "Direct3D 11 device: error 0x%08lx\n", (unsigned long)hr); return -1; }
    ID3DBlob *cs = compile("convert", "cs_5_0"), *ct = compile("convert_tex", "cs_5_0"), *vs = compile("vmain", "vs_5_0"),
             *ps = compile("pmain", "ps_5_0"), *cv = compile("conv_vs", "vs_5_0"), *cp = compile("conv_ps", "ps_5_0");
    if (!cs || !ct || !vs || !ps || !cv || !cp) return -1;
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(cv), ID3D10Blob_GetBufferSize(cv), NULL, &g_conv_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(cp), ID3D10Blob_GetBufferSize(cp), NULL, &g_conv_ps);
    ID3D10Blob_Release(cv);
    ID3D10Blob_Release(cp);
    ID3D11Device_CreateComputeShader(g_dev, ID3D10Blob_GetBufferPointer(cs), ID3D10Blob_GetBufferSize(cs), NULL, &g_cs);
    ID3D11Device_CreateComputeShader(g_dev, ID3D10Blob_GetBufferPointer(ct), ID3D10Blob_GetBufferSize(ct), NULL, &g_cs_tex);
    ID3D10Blob_Release(ct);
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &g_ps);
    ID3D10Blob_Release(cs);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    g_cb_conv = make_cb(sizeof(ConvParams));
    g_cb_draw = make_cb(sizeof(DrawParams));
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             D3D11_TEXTURE_ADDRESS_CLAMP, 0, 4, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.MaxAnisotropy = 1;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_pt);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;   // chroma upsampling
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_bil);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rast);
    return g_cs && g_cs_tex && g_vs && g_ps && g_cb_conv && g_cb_draw && g_samp_bil ? 0 : -1;
}

static void release_planes(Decoded *d) {
    for (int c = 0; c < 4; c++) {
        if (d->plsrv[c]) ID3D11ShaderResourceView_Release(d->plsrv[c]);
        if (d->pl[c]) ID3D11Texture2D_Release(d->pl[c]);
        d->plsrv[c] = NULL;
        d->pl[c] = NULL;
    }
}

// JPEG / PSD: one R8 texture per plane, copied from the decoder's buffer.
static int upload_planes(Decoded *d, const uint8_t *data) {
    int n = d->kind == KIND_YUV ? d->ncomp : d->ncomp == 3 ? 3 + d->hasa : 1 + d->hasa;
    for (int c = 0; c < n; c++) {
        int w, h;
        size_t off, pitch;
        if (d->kind == KIND_YUV) { w = d->pw[c]; h = d->ph[c]; off = d->offset[c]; pitch = d->pitch[c]; }
        else {   // PSD: planes one after the other, width x height each (gray: plane 0 + alpha)
            w = d->width; h = d->height; pitch = (size_t)w;
            off = (size_t)c * w * h;
        }
        D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {data + off, (UINT)pitch, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &d->pl[c])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)d->pl[c], NULL, &d->plsrv[c]))) {
            release_planes(d);
            return -1;
        }
    }
    return 0;
}

// Hands the decoded data to Direct3D (decoder thread: device calls are thread-safe).
static int gpu_upload(Decoded *d, const uint8_t *data) {
    if ((d->kind == KIND_YUV || d->kind == KIND_PLANAR) && !upload_planes(d, data)) return 0;
    UINT size = (UINT)align_up(d->size, 16);
    D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                            D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &init, &d->raw))) return -1;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx.NumElements = size / 4;
    sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)d->raw, &sv, &d->rawsrv))) {
        ID3D11Buffer_Release(d->raw);
        d->raw = NULL;
        return -1;
    }
    return 0;
}

// Textures: reused by size (main thread only).
static struct { ID3D11Texture2D *t; ID3D11ShaderResourceView *s; int w, h; } g_free_tex[4];
static int g_nfree_tex;

static void tex_put(ID3D11Texture2D *t, ID3D11ShaderResourceView *s, int w, int h) {
    if (!t) return;
    if (g_nfree_tex == 4) {
        ID3D11ShaderResourceView_Release(g_free_tex[0].s);
        ID3D11Texture2D_Release(g_free_tex[0].t);
        memmove(g_free_tex, g_free_tex + 1, 3 * sizeof *g_free_tex);
        g_nfree_tex--;
    }
    g_free_tex[g_nfree_tex].t = t;
    g_free_tex[g_nfree_tex].s = s;
    g_free_tex[g_nfree_tex].w = w;
    g_free_tex[g_nfree_tex].h = h;
    g_nfree_tex++;
}

static int tex_get(int w, int h, ID3D11Texture2D **t, ID3D11ShaderResourceView **s) {
    for (int i = 0; i < g_nfree_tex; i++)
        if (g_free_tex[i].w == w && g_free_tex[i].h == h) {
            *t = g_free_tex[i].t;
            *s = g_free_tex[i].s;
            g_free_tex[i] = g_free_tex[--g_nfree_tex];
            return 0;
        }
    D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, 0,
                               D3D11_RESOURCE_MISC_GENERATE_MIPS};
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, t))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)*t, NULL, s))) {
        ID3D11Texture2D_Release(*t);
        return -1;
    }
    return 0;
}

static void gpu_finish(void);

// Convert + mipmaps on the GPU (asynchronous). Main thread.
// (a failure: the texture back to the pool, not left as if converted)
static ID3D11ShaderResourceView *texture_failed(Decoded *d) {
    tex_put(d->tex, d->texsrv, d->width, d->height);
    d->tex = NULL;
    d->texsrv = NULL;
    return NULL;
}

static ID3D11ShaderResourceView *texture_for(Decoded *d) {
    if (d->texsrv) return d->texsrv;
    if ((!d->rawsrv && !d->plsrv[0]) || tex_get(d->width, d->height, &d->tex, &d->texsrv)) return NULL;
    ConvParams p = {{(uint32_t)d->width, (uint32_t)d->height, 0, 0}, {1, 1, 0, 0}, {0}, {0}, {0}};
    for (int c = 0; c < 3; c++) { p.off[c] = (uint32_t)d->offset[c]; p.pitch[c] = (uint32_t)d->pitch[c]; }
    if (d->kind == KIND_BGRA) p.a[2] = 2;
    else if (d->kind == KIND_PLANAR) {
        p.a[2] = 4;
        p.a[3] = (uint32_t)d->hasa;
        p.off[3] = (uint32_t)((size_t)d->ncomp * d->width * d->height);
    } else if (d->kind == KIND_PNG) {
        p.a[2] = 3;
        p.off[0] = 1;
        p.off[1] = (uint32_t)d->ncomp;
    } else if (d->ncomp == 1) p.a[2] = 1;
    else {
        if (d->limited) p.a[2] = 5;   // (limited range YCbCr: WebP)
        p.cs[0] = (float)d->h1 / d->hmax;
        p.cs[1] = (float)d->v1 / d->vmax;
        p.cm[0] = d->pw[1] - 1;
        p.cm[1] = d->ph[1] - 1;
        p.cs[2] = (float)d->pw[1];
        p.cs[3] = (float)d->ph[1];
    }
    int tex_path = d->plsrv[0] != NULL;
    double t0 = now_ms();
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_cb_conv, 0, NULL, &p, 0, 0);
    ID3D11ShaderResourceView *p0 = d->plsrv[0], *v[4] = {p0, p0, p0, p0}, *ns[4] = {NULL, NULL, NULL, NULL};
    if (tex_path) {   // gray: plane 0 three times
        if (d->ncomp == 3) { v[1] = d->plsrv[1]; v[2] = d->plsrv[2]; }
        if (d->kind == KIND_PLANAR && d->hasa) v[3] = d->plsrv[d->ncomp];   // PSD transparency
    }
    if (tex_path && g_convert_draw) {   // a draw into the texture (render target writes)
        ID3D11RenderTargetView *rtv = NULL;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)d->tex, &rd, &rtv))) return texture_failed(d);
        D3D11_VIEWPORT vp = {0, 0, (float)d->width, (float)d->height, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_conv_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_conv_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb_conv);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 2, 4, v);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 2, 1, &g_samp_bil);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 2, 4, ns);
        ID3D11RenderTargetView_Release(rtv);
    } else {          // compute shader
        ID3D11UnorderedAccessView *uav = NULL, *nu = NULL;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(g_dev, (ID3D11Resource *)d->tex, &ud, &uav))) return texture_failed(d);
        ID3D11DeviceContext_CSSetShader(g_ctx, tex_path ? g_cs_tex : g_cs, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &g_cb_conv);
        if (tex_path) {
            ID3D11DeviceContext_CSSetShaderResources(g_ctx, 2, 4, v);
            ID3D11DeviceContext_CSSetSamplers(g_ctx, 2, 1, &g_samp_bil);
        } else {
            ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &d->rawsrv);
        }
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &uav, NULL);
        ID3D11DeviceContext_Dispatch(g_ctx, (UINT)(d->width + 15) / 16, (UINT)(d->height + 15) / 16, 1);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &nu, NULL);
        ID3D11DeviceContext_CSSetShaderResources(g_ctx, tex_path ? 2 : 0, tex_path ? 4 : 1, ns);
        ID3D11UnorderedAccessView_Release(uav);
    }
    if (g_bench_split) { gpu_finish(); d->conv_ms = now_ms() - t0; }
    ID3D11DeviceContext_GenerateMips(g_ctx, d->texsrv);
    // the decoded data is not needed any more
    if (d->rawsrv) ID3D11ShaderResourceView_Release(d->rawsrv);
    if (d->raw) ID3D11Buffer_Release(d->raw);
    d->rawsrv = NULL;
    d->raw = NULL;
    release_planes(d);
    return d->texsrv;
}

static void decoded_free(Decoded *d) {   // main thread
    if (!d) return;
    tex_put(d->tex, d->texsrv, d->width, d->height);
    if (d->rawsrv) ID3D11ShaderResourceView_Release(d->rawsrv);
    if (d->raw) ID3D11Buffer_Release(d->raw);
    release_planes(d);
#ifdef NV_VECTOR
    if (d->dsrv) ID3D11ShaderResourceView_Release(d->dsrv);
    if (d->dtex) ID3D11Texture2D_Release(d->dtex);
    vec_unref(d->vec);
#endif
    free(d);
}

// Waits until the GPU has finished everything submitted so far (bench / tests).
static void gpu_finish(void) {
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *q = NULL;
    if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &q))) return;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)q);
    BOOL done = FALSE;
    while (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) == S_FALSE) SwitchToThread();
    ID3D11Query_Release(q);
}

// ---------------------------------------------------------------------------
// decoding (decoder thread)

static int g_nthreads = 0;   // -j
static volatile LONG g_dng_ev3;   // DNG exposure correction in 1/3 EV (keys E / Shift+E)

static IWICImagingFactory *wic_factory(void) {   // one per thread (COM apartment)
    static __thread IWICImagingFactory *f;
    if (!f) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                    (void **)&f)))
            f = NULL;
    }
    return f;
}

static int wic_orientation(IWICBitmapFrameDecode *frame) {
    IWICMetadataQueryReader *r = NULL;
    int o = 1;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &r))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(r, L"System.Photo.Orientation", &v)) && v.vt == VT_UI2)
            o = v.uiVal;
        PropVariantClear(&v);
        IWICMetadataQueryReader_Release(r);
    }
    return o >= 1 && o <= 8 ? o : 1;
}

// Any format Windows has a codec for, as premultiplied BGRA (transparent areas black).
// Huge images are scaled down to MAX_TEX. Returns the pixels (pool buffer) or NULL.
static uint8_t *decode_wic(const uint8_t *bytes, size_t len, Decoded *d, size_t *cap) {
    IWICImagingFactory *f = wic_factory();
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *src = NULL, *conv = NULL;
    IWICBitmapScaler *sc = NULL;
    uint8_t *buf = NULL;
    UINT w = 0, h = 0;
    if (!f || len > 0xFFFFFFFF || FAILED(IWICImagingFactory_CreateStream(f, &st)) ||   // (a memory stream: DWORD size)
        FAILED(IWICStream_InitializeFromMemory(st, (BYTE *)bytes, (DWORD)len)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) || FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) || !w || !h)
        goto done;
    d->orientation = wic_orientation(frame);
    src = (IWICBitmapSource *)frame;
    IWICBitmapSource_AddRef(src);
    if (w > MAX_TEX || h > MAX_TEX) {
        double k = (double)MAX_TEX / MAX(w, h);
        UINT nw = MAX(1, (UINT)(w * k)), nh = MAX(1, (UINT)(h * k));
        if (FAILED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) ||
            FAILED(IWICBitmapScaler_Initialize(sc, src, nw, nh, WICBitmapInterpolationModeFant)))
            goto done;
        IWICBitmapSource_Release(src);
        src = (IWICBitmapSource *)sc;
        IWICBitmapSource_AddRef(src);
        w = nw;
        h = nh;
    }
    if (FAILED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppPBGRA, src, &conv))) goto done;
    size_t pitch = (size_t)w * 4;
    buf = buf_get(pitch * h, cap);
    if (!buf || FAILED(IWICBitmapSource_CopyPixels(conv, NULL, (UINT)pitch, (UINT)(pitch * h), buf))) {
        if (buf) buf_put(buf, *cap);
        buf = NULL;
        goto done;
    }
    d->kind = KIND_BGRA;
    d->width = (int)w;
    d->height = (int)h;
    d->pitch[0] = pitch;
    d->size = pitch * h;
    d->mode = MODE_WIC;
    d->bands = 1;
done:
    if (conv) IWICBitmapSource_Release(conv);
    if (src) IWICBitmapSource_Release(src);
    if (sc) IWICBitmapScaler_Release(sc);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return buf;
}

// Camera raw files (CR2, NEF ...): reads only the start (the TIFF directories) and the
// full-size JPEG nitrojpeg finds there, not the raw data (most of the 10-30 MB); with the zero
// padding read_file leaves. NULL if there is no such JPEG (the caller reads it all).
static uint8_t *read_raw_jpeg(const wchar_t *path, size_t *len, int *orientation) {
    static const wchar_t *const e[] = {L"cr2", L"nef", L"nrw", L"arw", L"raf", NULL};
    if (!has_ext(path, e)) return NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    uint8_t *out = NULL;
    LARGE_INTEGER sz;
    DWORD cap = 1 << 20, n = 0;   // (Nikon puts the SubIFD directories after the maker notes: ~270 KB in)
    uint8_t *hdr = malloc(cap);
    size_t off, size;
    if (hdr && GetFileSizeEx(f, &sz) && ReadFile(f, hdr, cap, &n, NULL) && n > 0 &&
        !nj_find_embedded(hdr, n, &off, &size, orientation) && off < (uint64_t)sz.QuadPart &&
        size <= (uint64_t)sz.QuadPart - off && size < ((size_t)1 << 31) && (out = malloc(size + 64))) {
        LARGE_INTEGER at = {.QuadPart = (LONGLONG)off};
        DWORD got = 0;
        if (SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, out, (DWORD)size, &got, NULL) && got == size) {
            memset(out + size, 0, 64);
            *len = size;
        } else {
            free(out);
            out = NULL;
        }
    }
    free(hdr);
    CloseHandle(f);
    return out;
}

static int is_psd_name(const wchar_t *p) {
    static const wchar_t *const e[] = {L"psd", L"psb", NULL};
    return has_ext(p, e);
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
static VectorDoc *load_vector(const wchar_t *path, const uint8_t *data, size_t len) {
    VectorDoc v = {0};
#ifdef NV_SVG
    static const wchar_t *const svg_exts[] = {L"svg", NULL};
    if (has_ext(path, svg_exts) && (v.doc = nitrosvg_load(data, len))) {
        v.render = svg_render;
        v.size = svg_size;
        v.free = svg_free;
        v.pages = 1;
        v.mode = MODE_SVG;
    }
#endif
#ifdef NV_CDR
    static const wchar_t *const cdr_exts[] = {L"cdr", L"cmx", NULL};
    if (!v.doc && has_ext(path, cdr_exts) && (v.doc = nitrocdr_load(data, len))) {
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
    p->refs = 1;
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
    LONG tw = g_vec_tw, th = g_vec_th;
    if (tw > 0 && th > 0) k = MAX(k, MIN(tw / w, th / h));
    k = MIN(k, MAX_TEX / l);
    int iw = MAX(1, MIN(MAX_TEX, (int)lround(w * k))), ih = MAX(1, MIN(MAX_TEX, (int)lround(h * k)));
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

// Decodes one file and hands it to the GPU. NULL: cannot be decoded.
static Decoded *decode_file(const wchar_t *path) {
    double t0 = now_ms();
    size_t len = 0;
    uint8_t *data = NULL;
    if (is_psd_name(path)) {
        data = read_psd_merged(path, &len);
        ps_info pi;
        if (data && (ps_read_info(data, len, &pi) || !pi.supported || pi.width > MAX_TEX || pi.height > MAX_TEX)) {
            free(data);   // WIC gets the whole file
            data = NULL;
        }
    }
    // a camera raw (CR2, NEF ...) shows its full-size JPEG (nitrojpeg finds it): only that is read
    int raw_orientation = 0, orient = 0, raw_read = 0;
    if (!data && (data = read_raw_jpeg(path, &len, &orient))) {
        raw_orientation = orient;   // (0: the JPEG's own, RAF)
        raw_read = 1;
    }
    if (!data) data = read_file(path, &len);
    if (!data) return NULL;
    double t1 = now_ms();
    Decoded *d = calloc(1, sizeof *d);
    if (d) d->serial = next_serial();
    d->read_ms = t1 - t0;
    d->orientation = 1;
    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t eoff, esize;
    ndng_info di;   // a DNG: its raw image developed by nitrodng (else its largest preview below)
    if (!raw_read && !ndng_read_info(data, len, &di) && di.width <= MAX_TEX && di.height <= MAX_TEX) {
        size_t pitch = (size_t)di.width * 4;
        buf = buf_get(pitch * di.height, &cap);
        d->ev3 = (int)g_dng_ev3;
        di.ev = d->ev3 / 3.0f;
        if (buf && !ndng_decode(data, len, &di, buf, pitch, g_nthreads)) {
            d->kind = KIND_BGRA;
            d->width = di.width;
            d->height = di.height;
            d->pitch[0] = pitch;
            d->size = pitch * di.height;
            d->bands = 1;
            d->orientation = di.orientation;
            d->mode = MODE_NITRODNG;
            goto decoded;
        }
        if (buf) buf_put(buf, cap);
        buf = NULL;
    }
    if (!raw_read && !nj_find_embedded(data, len, &eoff, &esize, &orient) && eoff < len && esize <= len - eoff) {
        memmove(data, data + eoff, esize);   // (a raw file with another extension: read whole)
        memset(data + esize, 0, 64);         // (the zero padding read_file leaves after the data)
        len = esize;
        raw_orientation = orient;
    }
#ifdef NV_VECTOR
    VectorDoc *vec = load_vector(path, data, len);
    if (vec) {
        buf = decode_vector_page(vec, 0, d, &cap);
        vec_unref(vec);   // (d keeps its own reference)
    } else
#endif
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        np_info pi;   // nitropng: 8-bit gray / gray+alpha / RGB / RGBA, non-interlaced: the rows to the GPU
        if (!np_read_info(data, len, &pi) && pi.supported && pi.width <= MAX_TEX && pi.height <= MAX_TEX) {
            buf = buf_get(pi.raw_size, &cap);
            np_stats ps;
            if (buf && !np_decode(data, len, &pi, buf, g_nthreads, &ps)) {
                d->kind = KIND_PNG;
                d->width = pi.width;
                d->height = pi.height;
                d->ncomp = pi.channels;
                d->pitch[0] = pi.stride + 1;   // rows keep their filter-type byte
                d->size = pi.raw_size;
                d->orientation = pi.orientation;   // (eXIf chunk)
                d->bands = ps.chunks;
                d->mode = MODE_NITROPNG;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        } else if (!np_read_info(data, len, &pi) && pi.convertible && pi.width <= MAX_TEX && pi.height <= MAX_TEX) {
            // palette, 1 / 2 / 4 / 16-bit, a transparency key: the rows decoded, then made BGRX (in parallel)
            size_t rcap, pitch = (size_t)pi.width * 4;
            uint8_t *raw = buf_get(pi.raw_size, &rcap);
            buf = raw ? buf_get(pitch * pi.height, &cap) : NULL;
            np_stats ps;
            if (buf && !np_decode(data, len, &pi, raw, g_nthreads, &ps) && !np_to_bgrx(data, len, &pi, raw, buf, pitch, g_nthreads)) {
                d->kind = KIND_BGRA;
                d->width = pi.width;
                d->height = pi.height;
                d->pitch[0] = pitch;
                d->size = pitch * pi.height;
                d->orientation = pi.orientation;   // (eXIf chunk)
                d->bands = ps.chunks;
                d->mode = MODE_NITROPNG;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
            buf_put(raw, rcap);
        }
    } else if (len >= 4 && !memcmp(data, "8BPS", 4)) {
        ps_info pi;   // nitropsd: merged image of 8-bit RGB / grayscale PSD and PSB
        if (!ps_read_info(data, len, &pi) && pi.supported && pi.width <= MAX_TEX && pi.height <= MAX_TEX) {
            size_t P = (size_t)(pi.ncolor + pi.alpha);
            buf = buf_get(pi.plane_size * P, &cap);
            ps_stats ps;
            if (buf && !ps_decode(data, len, &pi, buf, g_nthreads, &ps)) {
                d->kind = KIND_PLANAR;
                d->width = pi.width;
                d->height = pi.height;
                d->ncomp = pi.ncolor;
                d->hasa = pi.alpha;
                d->pitch[0] = (size_t)pi.width;
                for (int c = 0; c < 3; c++) d->offset[c] = pi.ncolor == 3 ? c * pi.plane_size : 0;
                d->size = pi.plane_size * P;
                d->bands = 1;
                d->mode = MODE_NITROPSD;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
    } else if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        nj_info fi;
        int ok = !nj_read_info(data, len, &fi);
        if (ok) d->orientation = raw_orientation ? raw_orientation : fi.orientation;
        if (ok && fi.supported && fi.width <= MAX_TEX && fi.height <= MAX_TEX) {
            size_t total = 0;
            for (int c = 0; c < fi.ncomp; c++) {
                d->pitch[c] = align_up((size_t)fi.plane_w[c], 64);
                d->offset[c] = total;
                total = align_up(total + d->pitch[c] * fi.plane_h[c], 64);
                d->pw[c] = fi.plane_w[c];
                d->ph[c] = fi.plane_h[c];
            }
            buf = buf_get(total, &cap);
            uint8_t *planes[3] = {buf + d->offset[0], buf + d->offset[1], buf + d->offset[2]};
            nj_stats st;
            if (buf && !nj_decode_planes(data, len, &fi, planes, d->pitch, 0, &st)) {
                d->kind = KIND_YUV;
                d->width = fi.width;
                d->height = fi.height;
                d->ncomp = fi.ncomp;
                d->h1 = fi.ncomp > 1 ? fi.h[1] : 1;
                d->v1 = fi.ncomp > 1 ? fi.v[1] : 1;
                d->hmax = fi.hmax;
                d->vmax = fi.vmax;
                d->size = total;
                d->mode = st.mode;
                d->bands = st.bands;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
    } else if (len >= 12 && !memcmp(data + 4, "ftyp", 4)) {
        nh_info hi;   // HEIC: its HEVC tiles decoded in parallel by our own decoder (nitroheic + nitrohevc)
        if (!nh_read_info(data, len, &hi) && hi.plane_w[0] <= MAX_TEX && hi.plane_h[0] <= MAX_TEX) {
            size_t total = 0;
            for (int c = 0; c < 3; c++) {
                d->pitch[c] = align_up((size_t)hi.plane_w[c], 64);
                d->offset[c] = total;
                total = align_up(total + d->pitch[c] * hi.plane_h[c], 64);
                d->pw[c] = hi.plane_w[c];
                d->ph[c] = hi.plane_h[c];
            }
            buf = buf_get(total, &cap);
            uint8_t *planes[3] = {buf + d->offset[0], buf + d->offset[1], buf + d->offset[2]};
            if (buf && !nh_decode_planes(data, len, &hi, planes, d->pitch, 0)) {
                d->kind = KIND_YUV;   // (full range BT.601 4:2:0, as a JPEG; the ICC profile is not used)
                d->width = hi.width;
                d->height = hi.height;
                d->ncomp = 3;
                d->h1 = d->v1 = 1;
                d->hmax = d->vmax = 2;
                d->orientation = hi.orientation;
                d->size = total;
                d->mode = MODE_NITROHEVC;
                d->bands = hi.ntiles;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
#ifdef NV_DAV1D
        na_info ai;   // AVIF: AV1 through dav1d, our colour conversion (in parallel)
        if (!buf && !na_read_info(data, len, &ai) && ai.width <= MAX_TEX && ai.height <= MAX_TEX) {
            size_t pitch = (size_t)ai.width * 4;
            buf = buf_get(pitch * ai.height, &cap);
            if (buf && !na_decode(data, len, &ai, buf, pitch, g_nthreads)) {
                d->kind = KIND_BGRA;
                d->width = ai.width;
                d->height = ai.height;
                d->pitch[0] = pitch;
                d->size = pitch * ai.height;
                d->orientation = ai.orientation;
                d->mode = MODE_NITROAVIF;
                d->bands = ai.ntiles;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
#endif
    } else if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        nw_info wi;   // lossy WebP: nitrowebp (the tokens parsed on one thread, the rows reconstructed on another)
        if (!nw_read_info(data, len, &wi) && wi.plane_w[0] <= MAX_TEX && wi.plane_h[0] <= MAX_TEX) {
            size_t total = 0;
            for (int c = 0; c < 3; c++) {
                d->pitch[c] = align_up((size_t)wi.plane_w[c], 64);
                d->offset[c] = total;
                total = align_up(total + d->pitch[c] * wi.plane_h[c], 64);
                d->pw[c] = wi.plane_w[c];
                d->ph[c] = wi.plane_h[c];
            }
            buf = buf_get(total, &cap);
            uint8_t *planes[3] = {buf + d->offset[0], buf + d->offset[1], buf + d->offset[2]};
            if (buf && !nw_decode_planes(data, len, &wi, planes, d->pitch, g_nthreads)) {
                d->kind = KIND_YUV;   // (limited range BT.601 4:2:0; the ICC profile is not used)
                d->limited = 1;
                d->width = wi.width;
                d->height = wi.height;
                d->ncomp = 3;
                d->h1 = d->v1 = 1;
                d->hmax = d->vmax = 2;
                d->orientation = wi.orientation;
                d->size = total;
                d->mode = MODE_NITROWEBP;
                d->bands = 1;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
    } else if (len >= 8 && data[0] == data[1] && (data[0] == 'I' || data[0] == 'M')) {
        nt_info ti;   // TIFF: nitrotiff (strips / tiles in parallel; uncompressed: straight from the file)
        if (!nt_read_info(data, len, &ti) && ti.width <= MAX_TEX && ti.height <= MAX_TEX) {
            size_t pitch = (size_t)ti.width * 4;
            buf = buf_get(pitch * ti.height, &cap);
            if (buf && !nt_decode(data, len, &ti, buf, pitch, g_nthreads)) {
                d->kind = KIND_BGRA;
                d->width = ti.width;
                d->height = ti.height;
                d->pitch[0] = pitch;
                d->size = pitch * ti.height;
                d->orientation = ti.orientation;
                d->mode = MODE_NITROTIFF;
                d->bands = 1;
            } else if (buf) {
                buf_put(buf, cap);
                buf = NULL;
            }
        }
    }
decoded:
    if (!buf) {   // everything else, and what the own decoders could not handle (damaged, progressive ...)
        int o = d->orientation;
        buf = decode_wic(data, len, d, &cap);
        if (d->orientation == 1) d->orientation = o;
    }
    free(data);
    if (!buf) { free(d); return NULL; }
    double t2 = now_ms();
    int up = gpu_upload(d, buf);
    buf_put(buf, cap);
    d->decode_ms = t2 - t1;
    d->upload_ms = now_ms() - t2;
    if (up) {
#ifdef NV_VECTOR
        vec_unref(d->vec);
#endif
        free(d);
        return NULL;
    }
    return d;
}

// ---------------------------------------------------------------------------
// image cache + background decoder

typedef struct {
    wchar_t **files;
    int nfiles;
    struct { int index; Decoded *d; } cache[16];
    int ncache;
    char *failed;                 // per file
    int cur, dir;
    unsigned gen;                 // bumped when the file list changes: in-flight decodes are dropped
    Decoded *grave[64];           // evicted, freed by the main thread (textures live there)
    int ngrave;
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    HWND notify;
} Loader;

static Loader L;

static Decoded *loader_get(int i) {
    AcquireSRWLockExclusive(&L.lock);
    Decoded *d = NULL;
    for (int k = 0; k < L.ncache; k++)
        if (L.cache[k].index == i) d = L.cache[k].d;
    ReleaseSRWLockExclusive(&L.lock);
    return d;
}

static int loader_failed(int i) {
    AcquireSRWLockExclusive(&L.lock);
    int f = i >= 0 && i < L.nfiles && L.failed[i];
    ReleaseSRWLockExclusive(&L.lock);
    return f;
}

static void bury_locked(Decoded *d) {
    if (!d) return;
    if (L.ngrave < 64) L.grave[L.ngrave++] = d;   // (never full: the main thread empties it on every message)
}

static void loader_reap(void) {   // main thread
    Decoded *g[64];
    AcquireSRWLockExclusive(&L.lock);
    int n = L.ngrave;
    memcpy(g, L.grave, (size_t)n * sizeof *g);
    L.ngrave = 0;
    ReleaseSRWLockExclusive(&L.lock);
    for (int i = 0; i < n; i++) decoded_free(g[i]);
}

static void loader_focus(int cur, int dir) {
    AcquireSRWLockExclusive(&L.lock);
    L.cur = cur;
    L.dir = dir;
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
}

// New file list (takes ownership); the decoded image of old_index (if any) is kept as new_index.
static void loader_replace(wchar_t **files, int n, int old_index, int new_index) {
    AcquireSRWLockExclusive(&L.lock);
    Decoded *keep = NULL;
    for (int k = 0; k < L.ncache; k++) {
        if (L.cache[k].index == old_index && old_index >= 0) keep = L.cache[k].d;
        else bury_locked(L.cache[k].d);
    }
    int keep_failed = old_index >= 0 && old_index < L.nfiles && L.failed[old_index];
    L.ncache = 0;
    if (keep) { L.cache[0].index = new_index; L.cache[0].d = keep; L.ncache = 1; }
    for (int i = 0; i < L.nfiles; i++) free(L.files[i]);
    free(L.files);
    free(L.failed);
    L.files = files;
    L.nfiles = n;
    L.failed = calloc((size_t)MAX(n, 1), 1);
    if (keep_failed) L.failed[new_index] = 1;
    L.cur = new_index;
    L.gen++;
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
}

// File i left the list (deleted): later images move up one place, decoded ones with them. (Main
// thread only: the viewer's list is this same array.)
static void loader_remove(int i) {
    AcquireSRWLockExclusive(&L.lock);
    for (int c = 0; c < L.ncache;) {
        if (L.cache[c].index == i) {
            bury_locked(L.cache[c].d);
            L.cache[c] = L.cache[--L.ncache];
            continue;
        }
        if (L.cache[c].index > i) L.cache[c].index--;
        c++;
    }
    free(L.files[i]);
    memmove(L.files + i, L.files + i + 1, (size_t)(L.nfiles - i - 1) * sizeof *L.files);
    memmove(L.failed + i, L.failed + i + 1, (size_t)(L.nfiles - i - 1));
    L.nfiles--;
    L.gen++;   // (a decode in flight would land at the old index)
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
}

// A file came back at i (undo; takes path). Returns -1 if out of memory.
static int loader_insert(int i, wchar_t *path) {
    AcquireSRWLockExclusive(&L.lock);
    wchar_t **f = realloc(L.files, (size_t)(L.nfiles + 1) * sizeof *f);
    if (f) L.files = f;
    char *fl = f ? realloc(L.failed, (size_t)L.nfiles + 1) : NULL;
    if (fl) L.failed = fl;
    if (!f || !fl) {
        ReleaseSRWLockExclusive(&L.lock);
        return -1;
    }
    for (int c = 0; c < L.ncache; c++)
        if (L.cache[c].index >= i) L.cache[c].index++;
    memmove(L.files + i + 1, L.files + i, (size_t)(L.nfiles - i) * sizeof *L.files);
    memmove(L.failed + i + 1, L.failed + i, (size_t)(L.nfiles - i));
    L.files[i] = path;
    L.failed[i] = 0;
    L.nfiles++;
    L.gen++;
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
    return 0;
}

#ifdef NV_VECTOR
// The size vector images are rendered for changed (g_vec_tw): prefetched ones too small for it
// are dropped and rendered again (the current one is sharpened by the detail render).
static void loader_vector_target_changed(void) {
    LONG tw = g_vec_tw, th = g_vec_th;
    AcquireSRWLockExclusive(&L.lock);
    for (int c = 0; c < L.ncache;) {
        Decoded *d = L.cache[c].d;
        if (d->vec && L.cache[c].index != L.cur && tw > 0 && th > 0 &&
            MIN((double)tw / d->width, (double)th / d->height) > 1.01 && MAX(d->width, d->height) < MAX_TEX) {
            bury_locked(d);
            L.cache[c] = L.cache[--L.ncache];
        } else {
            c++;
        }
    }
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
}

#endif

static int stale_dng(const Decoded *d) { return d && d->mode == MODE_NITRODNG && d->ev3 != (int)g_dng_ev3; }

// An image decoded again (another page, another DNG exposure): d replaces the one with that serial if
// it is still in the cache - wherever it is now (Del / U may have moved it) - and d is not outdated by
// a later key press. Returns its index, -1 if it did not.
static int loader_replace_image(unsigned serial, Decoded *d) {
    int at = -1;
    AcquireSRWLockExclusive(&L.lock);
    for (int c = 0; c < L.ncache; c++)
        if (L.cache[c].d && L.cache[c].d->serial == serial && !stale_dng(d)) {
            bury_locked(L.cache[c].d);
            L.cache[c].d = d;
            at = L.cache[c].index;
            break;
        }
    if (at < 0) bury_locked(d);
    ReleaseSRWLockExclusive(&L.lock);
    return at;
}

typedef struct {
    Decoded *old, *d;
    unsigned old_serial;          // (old may be freed meanwhile: matched by this)
    wchar_t *path;
    int index;
} DngJob;

static DWORD WINAPI dng_run(void *arg) {
    DngJob *j = arg;
    j->d = decode_file(j->path);
    PostMessageW(L.notify, WM_APP_DNG, (WPARAM)j, 0);
    return 0;
}

// The DNG exposure correction changed (g_dng_ev3): prefetched DNGs are dropped and developed again,
// the current one is replaced when its new development is ready (it stays on screen until then).
static void loader_dng_exposure_changed(void) {
    DngJob *j = NULL;
    AcquireSRWLockExclusive(&L.lock);
    for (int c = 0; c < L.ncache;) {
        Decoded *d = L.cache[c].d;
        if (L.cache[c].index == L.cur) {
            if (stale_dng(d) && (j = calloc(1, sizeof *j)) && (j->path = _wcsdup(L.files[L.cur]))) {
                j->old = d;
                j->old_serial = d->serial;
                j->index = L.cur;
            }
            c++;
        } else if (stale_dng(d)) {
            bury_locked(d);
            L.cache[c] = L.cache[--L.ncache];
        } else {
            c++;
        }
    }
    WakeConditionVariable(&L.cv);
    ReleaseSRWLockExclusive(&L.lock);
    if (j && j->path) {
        HANDLE th = CreateThread(NULL, 0, dng_run, j, 0, NULL);
        if (th) {
            CloseHandle(th);
            return;
        }
    }
    if (j) free(j->path);
    free(j);
}

// Priority order: current, then AHEAD in the paging direction, then BEHIND.
static int wanted_locked(int *w) {
    int n = 0;
#define ADD(i) do { int _i = (i); int dup = 0; for (int q = 0; q < n; q++) dup |= w[q] == _i; \
                    if (_i >= 0 && _i < L.nfiles && !dup) w[n++] = _i; } while (0)
    ADD(L.cur);
    for (int k = 1; k <= MAX(AHEAD, BEHIND); k++) {
        if (k <= AHEAD) ADD(L.cur + L.dir * k);
        if (k <= BEHIND) ADD(L.cur - L.dir * k);
    }
#undef ADD
    return n;
}

static DWORD WINAPI loader_run(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        AcquireSRWLockExclusive(&L.lock);
        int w[16], nw, next = -1;
        for (;;) {
            nw = wanted_locked(w);
            for (int k = 0; k < nw && next < 0; k++) {
                int have = 0;
                for (int c = 0; c < L.ncache; c++) have |= L.cache[c].index == w[k];
                if (!have && !L.failed[w[k]]) next = w[k];
            }
            if (next >= 0) break;
            SleepConditionVariableSRW(&L.cv, &L.lock, INFINITE, 0);
        }
        for (int c = 0; c < L.ncache;) {   // evict everything outside the window
            int in = 0;
            for (int k = 0; k < nw; k++) in |= L.cache[c].index == w[k];
            if (in) { c++; continue; }
            bury_locked(L.cache[c].d);
            L.cache[c] = L.cache[--L.ncache];
        }
        wchar_t *path = _wcsdup(L.files[next]);
        unsigned gen = L.gen;
        ReleaseSRWLockExclusive(&L.lock);

        Decoded *d = decode_file(path);

        AcquireSRWLockExclusive(&L.lock);
        int stale = gen != L.gen || stale_dng(d);   // the file list or the DNG exposure changed meanwhile
        if (stale) bury_locked(d);
        else if (d && L.ncache < 16) { L.cache[L.ncache].index = next; L.cache[L.ncache].d = d; L.ncache++; }
        else if (d) bury_locked(d);
        else L.failed[next] = 1;
        HWND hw = L.notify;
        ReleaseSRWLockExclusive(&L.lock);
        if (!stale && !d) fprintf(stderr, "cannot decode %s\n", utf8(path));
        free(path);
        if (hw) PostMessageW(hw, WM_APP_DECODED, (WPARAM)(stale ? -1 : next), 0);
    }
    return 0;
}

static void loader_start(wchar_t **files, int n) {
    InitializeSRWLock(&L.lock);
    InitializeConditionVariable(&L.cv);
    L.files = files;
    L.nfiles = n;
    L.failed = calloc((size_t)MAX(n, 1), 1);
    L.dir = 1;
    CloseHandle(CreateThread(NULL, 0, loader_run, NULL, 0, NULL));
}

// ---------------------------------------------------------------------------
// viewer

typedef struct {
    HWND hwnd;
    IDXGISwapChain1 *swap;
    ID3D11RenderTargetView *rtv;
    int vw, vh;                   // client size (device pixels)
    wchar_t **files;              // (owned by the loader; same list)
    int nfiles, index, started, lazy_dir;
    double request_time;
    int reported, was_ready;
    // slideshow
    double slide_ms;
    int paused;
    int shown_index;
    double shown_time;
    struct { wchar_t *path, *to; int index; } *deleted;   // Del: original path, path in .delete\, for undo
    int ndeleted;
    // zoom / pan: kept when paging, so a series can be compared at the same spot
    int zoomed, fit_screen;
    double scale, cx, cy;         // image pixels -> device pixels; image point at the view centre
    int dragging;
    double drag_x, drag_y;
    double page_accum;            // Ctrl + wheel (touchpad): accumulated distance
    // window sizing
    int user_sized, self_resizing, fullscreen;
    WINDOWPLACEMENT saved;
    LONG saved_style;
} Viewer;

static Viewer V;

static void display_size(const Decoded *d, double *iw, double *ih) {   // after EXIF rotation
    int swap = d->orientation >= 5;
    *iw = swap ? d->height : d->width;
    *ih = swap ? d->width : d->height;
}

static void redraw(void) { InvalidateRect(V.hwnd, NULL, FALSE); }

// Work area of the window's monitor, and the frame around the client area.
static RECT work_area(void) {
    MONITORINFO mi = {.cbSize = sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(V.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}
static SIZE frame_extra(void) {   // (per-monitor DPI: the frame at the window's own DPI)
    typedef BOOL(WINAPI * AdjForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    typedef UINT(WINAPI * DpiFor)(HWND);
    static AdjForDpi adj;
    static DpiFor dpi;
    if (!adj) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        adj = (AdjForDpi)(void *)GetProcAddress(u, "AdjustWindowRectExForDpi");
        dpi = (DpiFor)(void *)GetProcAddress(u, "GetDpiForWindow");
    }
    RECT r = {0, 0, 100, 100};
    DWORD style = (DWORD)GetWindowLongW(V.hwnd, GWL_STYLE);
    if (!adj || !dpi || !adj(&r, style, FALSE, 0, dpi(V.hwnd))) AdjustWindowRectEx(&r, style, FALSE, 0);
    SIZE s = {r.right - r.left - 100, r.bottom - r.top - 100};
    return s;
}
static SIZE max_client(void) {
    RECT wa = work_area();
    SIZE fe = frame_extra(), s = {wa.right - wa.left - fe.cx, wa.bottom - wa.top - fe.cy};
    return s;
}

// Client size for an image of w x h pixels: smaller than the screen: exactly 100%
// (unless upscale); larger: its aspect ratio, as large as fits the work area.
static SIZE window_content_size(double w, double h, int upscale) {
    SIZE mx = max_client();
    double k = MIN(mx.cx / w, mx.cy / h);
    if (!upscale) k = MIN(k, 1.0);
    double cw = k == 1 ? ceil(w) : floor(w * k), ch = k == 1 ? ceil(h) : floor(h * k);
    SIZE s = {(LONG)MAX(cw, 320), (LONG)MAX(ch, 200)};
    return s;
}

static double fit_scale(const Decoded *d) {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = MIN(V.vw / iw, V.vh / ih);
    if (s > 1 && !V.fullscreen && !V.fit_screen) s = 1;   // small images: 100% (unless key 0)
    return s;
}
static double current_scale(const Decoded *d) { return V.zoomed ? V.scale : fit_scale(d); }

// The "fit" zoom used for snapping: the scale of the normal image-sized window
// (the current one may have shrunk with a zoomed-out image).
static double fit_snap_scale(const Decoded *d) {
    if (V.user_sized || V.fullscreen) return fit_scale(d);
    double iw, ih;
    display_size(d, &iw, &ih);
    SIZE cs = window_content_size(iw, ih, V.fit_screen);
    double s = MIN(cs.cx / iw, cs.cy / ih);
    return V.fit_screen ? s : MIN(s, 1.0);
}

static void clamp_center(const Decoded *d, double s) {
    double iw, ih;
    display_size(d, &iw, &ih);
    double vw = V.vw / s, vh = V.vh / s;
    V.cx = iw <= vw ? iw / 2 : MAX(vw / 2, MIN(V.cx, iw - vw / 2));
    V.cy = ih <= vh ? ih / 2 : MAX(vh / 2, MIN(V.cy, ih - vh / 2));
}

static void update_title(void) {
    wchar_t t[1024];
    if (V.nfiles <= 0) {
        SetWindowTextW(V.hwnd, L"nitroview - drop images here, or O to open files, Shift+O a folder");
        return;
    }
    const wchar_t *name = base_name(V.files[V.index]);
    Decoded *d = loader_get(V.index);
    if (d)
        swfmt(t, 1024, L"%ls  %.0f%%%ls  (%d/%d)  %dx%d  decode %.1f ms", name, 100 * current_scale(d),
                   V.zoomed ? L"" : L" (fit)", V.index + 1, V.nfiles, d->width, d->height, d->decode_ms);
    else
        swfmt(t, 1024, L"%ls  (%d/%d)  %ls", name, V.index + 1, V.nfiles,
                   loader_failed(V.index) ? L"CANNOT DECODE (unsupported or damaged)" : L"loading...");
#ifdef NV_VECTOR
    if (d && d->vec && d->vec->pages > 1) {
        size_t n = wcslen(t);
        swfmt(t + n, 1024 - n, L"  [page %d/%d]", d->page + 1, d->vec->pages);
    }
#endif
    if (d && d->mode == MODE_NITRODNG && d->ev3) {
        size_t n = wcslen(t);
        swfmt(t + n, 1024 - n, L"  %+.1f EV", d->ev3 / 3.0);
    }
    if (V.slide_ms > 0) {
        size_t n = wcslen(t);
        if (V.paused) swfmt(t + n, 1024 - n, L"   [slideshow paused: P]");
        else swfmt(t + n, 1024 - n, L"   [slideshow %g ms]", V.slide_ms);
    }
    SetWindowTextW(V.hwnd, t);
}

// The window follows the image: in fit mode its size (window_content_size); zoomed
// in, as large as the image at the current zoom, up to the screen. Keeps its centre
// and stays on screen. Not after a manual resize (until W) and not in full screen.
static void size_window_for(const Decoded *d) {
    if (!d || V.user_sized || V.fullscreen || IsIconic(V.hwnd) || IsZoomed(V.hwnd)) return;
    double iw, ih;
    display_size(d, &iw, &ih);
    SIZE cs;
    if (V.zoomed) {
        SIZE mx = max_client();
        cs.cx = (LONG)MAX(320, MIN(mx.cx, floor(iw * V.scale)));
        cs.cy = (LONG)MAX(200, MIN(mx.cy, floor(ih * V.scale)));
    } else {
        cs = window_content_size(iw, ih, V.fit_screen);
    }
    if (cs.cx == V.vw && cs.cy == V.vh) return;
    RECT wr, wa = work_area();
    GetWindowRect(V.hwnd, &wr);
    SIZE fe = frame_extra();
    LONG w = cs.cx + fe.cx, h = cs.cy + fe.cy;
    LONG x = (wr.left + wr.right) / 2 - w / 2, y = (wr.top + wr.bottom) / 2 - h / 2;
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (y < wa.top) y = wa.top;
    V.self_resizing = 1;
    SetWindowPos(V.hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    V.self_resizing = 0;
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
    LONG tw = 0, th = 0;
    if (V.hwnd && (V.fullscreen || V.fit_screen)) {
        MONITORINFO mi = {.cbSize = sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(V.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        tw = mi.rcMonitor.right - mi.rcMonitor.left;
        th = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    if (tw == g_vec_tw && th == g_vec_th) return;
    InterlockedExchange(&g_vec_tw, tw);
    InterlockedExchange(&g_vec_th, th);
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
    double cur = current_scale(d), cx = iw / 2, cy = ih / 2;
    if (V.zoomed) {
        clamp_center(d, V.scale);
        cx = V.cx;
        cy = V.cy;
    }
    double px = cx + ax / cur, py = cy + ay / cur;   // image point under the anchor
    V.cx = px - ax / ns;
    V.cy = py - ay / ns;
    V.zoomed = 1;
    V.scale = ns;
    clamp_center(d, ns);
    size_window_for(d);
    update_title();
    redraw();
}

// continuous: touchpad, many small steps (no 2% snap, it would swallow them)
static void zoom_by(double f, const Decoded *d, double ax, double ay, int continuous) {
    double fit = fit_snap_scale(d), cur = current_scale(d), ns = cur * f;
    for (int k = 0; k < 2; k++) {   // stop exactly at 100% and at fit when a step crosses them
        double snap = k == 0 ? 1.0 : fit;
        if ((cur < snap - 1e-9 && ns > snap + 1e-9) || (cur > snap + 1e-9 && ns < snap - 1e-9)) {
            ns = snap;
            break;
        }
    }
    ns = MAX(MIN(fit, 1.0) / 4, MIN(ns, 32.0));
    double r = round(ns);   // e.g. sqrt(2)^2 -> exactly 200%
    if (r >= 1 && fabs(ns - r) < 1e-6 * r) ns = r;
    if (!continuous && fabs(ns - fit) < 0.02 * fit) ns = fit;
    set_scale(ns, d, ax, ay);
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
    if (V.nfiles <= 0) return;   // (no files yet: started without any)
    i = MAX(0, MIN(i, V.nfiles - 1));   // stop at the first/last image, no rollover
    if (i == V.index && V.started) return;
    V.started = 1;
    V.index = i;
    V.request_time = now_ms();
    V.reported = 0;
    Decoded *ready = loader_get(i);
    V.was_ready = ready != NULL;
    loader_focus(i, dir);
    if (ready) fit_window_to(ready);
    update_title();
    redraw();
}

// One file was given: when the user pages, list its folder; the current image
// keeps its decoded data and becomes its place in the folder.
static void expand_directory(void) {
    if (!V.lazy_dir) return;
    V.lazy_dir = 0;
    const wchar_t *path = V.files[V.index], *name = base_name(path);
    wchar_t dir[MAX_PATH * 2];
    size_t dl = (size_t)(name - path);
    if (dl >= MAX_PATH * 2) return;
    wmemcpy(dir, path, dl);
    dir[dl] = 0;
    FileList l = {0};
    list_dir(&l, dl ? dir : L".", L"*", 0);
    int k = -1;
    for (int i = 0; i < l.n; i++)
        if (!_wcsicmp(base_name(l.v[i]), name)) k = i;
    if (k < 0 || l.n < 2) { fl_free(&l); return; }
    free(l.v[k]);
    l.v[k] = _wcsdup(path);   // keep the path exactly as given
    if (V.shown_index == V.index) V.shown_index = k;
    loader_replace(l.v, l.n, V.index, k);
    V.files = l.v;
    V.nfiles = l.n;
    V.index = k;
    update_title();
}

static void page(int delta) { expand_directory(); go(V.index + delta, delta >= 0 ? 1 : -1); }

// Del: the current file goes to the (hidden) ".delete" folder next to it: nothing is lost (undo
// with U / Ctrl+Z, or move it back by hand). The next image follows; the last one is not deleted.
static void delete_current(void) {
    expand_directory();
    void *more = V.nfiles >= 2 ? realloc(V.deleted, (size_t)(V.ndeleted + 1) * sizeof *V.deleted) : NULL;
    if (!more) {
        MessageBeep(MB_OK);
        return;
    }
    V.deleted = more;
    const wchar_t *path = V.files[V.index], *name = base_name(path), *ext = wcsrchr(name, L'.');
    if (!ext) ext = name + wcslen(name);
    size_t dl = (size_t)(name - path), n = dl + wcslen(name) + 32;
    wchar_t *bin = malloc(n * sizeof *bin), *to = malloc(n * sizeof *to);
    if (!bin || !to) {
        free(bin);
        free(to);
        MessageBeep(MB_OK);
        return;
    }
    swfmt(bin, n, L"%.*ls.delete", (int)dl, path);
    if (CreateDirectoryW(bin, NULL)) SetFileAttributesW(bin, FILE_ATTRIBUTE_HIDDEN);
    swfmt(to, n, L"%ls\\%ls", bin, name);
    for (int k = 2; GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES; k++)   // (never over an earlier one)
        swfmt(to, n, L"%ls\\%.*ls (%d)%ls", bin, (int)(ext - name), name, k, ext);
    free(bin);
    if (!MoveFileW(path, to)) {
        fprintf(stderr, "cannot move %s: error %lu\n", utf8(path), GetLastError());
        free(to);
        MessageBeep(MB_OK);
        return;
    }
    int i = V.index;
    V.deleted[V.ndeleted].path = _wcsdup(path);
    V.deleted[V.ndeleted].to = to;
    V.deleted[V.ndeleted].index = i;
    V.ndeleted++;
    loader_remove(i);   // (frees path)
    V.nfiles--;
    V.shown_index = V.shown_index == i ? -1 : V.shown_index > i ? V.shown_index - 1 : V.shown_index;
    V.started = 0;
    go(MIN(i, V.nfiles - 1), 1);
}

// Undo the last Del: the file goes back to its place in the folder and in the list.
static void undo_delete(void) {
    if (!V.ndeleted) {
        MessageBeep(MB_OK);
        return;
    }
    wchar_t *path = V.deleted[V.ndeleted - 1].path, *to = V.deleted[V.ndeleted - 1].to;
    int i = MIN(V.deleted[V.ndeleted - 1].index, V.nfiles);
    if (!path || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES || !MoveFileW(to, path)) {
        MessageBeep(MB_OK);   // (something is there again, or it was moved away)
        return;
    }
    if (loader_insert(i, path)) {
        MoveFileW(path, to);
        MessageBeep(MB_OK);
        return;
    }
    free(to);
    V.ndeleted--;
    V.files = L.files;
    V.nfiles++;
    if (V.shown_index >= i) V.shown_index++;
    if (V.index >= i) V.index++;
    V.started = 0;
    go(i, 1);
}
static void page_first(void) { expand_directory(); go(0, 1); }
static void page_last(void) { expand_directory(); go(V.nfiles - 1, -1); }

static void open_files(FileList *l, int lazy) {   // drag & drop
    if (!l->n) return;
    for (int k = 0; k < V.ndeleted; k++) {   // (undo: positions in the old list; the files stay in .delete)
        free(V.deleted[k].path);
        free(V.deleted[k].to);
    }
    V.ndeleted = 0;
    V.lazy_dir = lazy;
    V.files = l->v;
    V.nfiles = l->n;
    V.started = 0;
    V.index = 0;
    V.shown_index = -1;
    loader_replace(l->v, l->n, -1, 0);
    l->v = NULL;
    l->n = l->cap = 0;
    go(0, 1);
}

// Slideshow: the next image comes slide_ms after the current one was drawn.
static void schedule_slide(void) {
    if (V.slide_ms > 0 && !V.paused) expand_directory();
    KillTimer(V.hwnd, TIMER_SLIDE);
    if (V.slide_ms <= 0 || V.paused || V.index >= V.nfiles - 1) return;   // stops at the last image
    double wait = V.slide_ms - (now_ms() - V.shown_time);
    SetTimer(V.hwnd, TIMER_SLIDE, (UINT)MAX(wait, USER_TIMER_MINIMUM), NULL);
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
    } else if (abs(i - V.index) <= 1 && !d->texsrv) {
        texture_for(d);   // convert neighbours ahead of time, so paging only has to draw
        ID3D11DeviceContext_Flush(g_ctx);
    }
}

static DrawParams draw_params(const Decoded *d) {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = current_scale(d);
    if (V.zoomed) clamp_center(d, s);
    double cx = V.zoomed ? V.cx : iw / 2, cy = V.zoomed ? V.cy : ih / 2;
    double x0 = round(V.vw / 2.0 - cx * s), y0 = round(V.vh / 2.0 - cy * s);
    DrawParams p = {{(float)(x0 / V.vw * 2 - 1), (float)(1 - y0 / V.vh * 2)},
                    {(float)(2 * iw * s / V.vw), (float)(2 * ih * s / V.vh)},
                    (uint32_t)d->orientation, (uint32_t)(s >= 2 && s == floor(s)), {0, 0}};
    return p;
}

static void make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(V.swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &V.rtv);
        ID3D11Texture2D_Release(bb);
    }
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
    return d->dsrv && d->dscale == r->scale && d->dx == r->dx && d->dy == r->dy && d->dw == r->dw && d->dh == r->dh;
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

static DWORD WINAPI detail_run(void *arg) {
    DetailJob *j = arg;
    double t = now_ms(), k = j->vscale * j->r.scale;
    j->px = malloc((size_t)j->r.w * j->r.h * 4);
    if (j->px && j->vec->render(j->vec->doc, j->page, j->px, j->r.w, j->r.h, (size_t)j->r.w * 4, k, j->r.dx / j->vscale,
                                j->r.dy / j->vscale)) {
        free(j->px);
        j->px = NULL;
    }
    j->ms = now_ms() - t;
    PostMessageW(V.hwnd, WM_APP_DETAIL, (WPARAM)j, 0);
    return 0;
}

static void start_detail(void) {   // TIMER_DETAIL
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
    HANDLE th = CreateThread(NULL, 0, detail_run, j, 0, NULL);
    if (!th) {
        vec_unref(j->vec);
        free(j);
        return;
    }
    CloseHandle(th);
    g_detail_busy = 1;
}

static void detail_done(DetailJob *j) {   // WM_APP_DETAIL
    g_detail_busy = 0;
    Decoded *d = loader_get(V.index);
    if (j->px && d == j->d && d->vec == j->vec && d->page == j->page) {
        D3D11_TEXTURE2D_DESC td = {(UINT)j->r.w, (UINT)j->r.h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                   D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {j->px, (UINT)j->r.w * 4, 0};
        ID3D11Texture2D *t = NULL;
        ID3D11ShaderResourceView *v = NULL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &t)) &&
            SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &v))) {
            if (d->dsrv) ID3D11ShaderResourceView_Release(d->dsrv);
            if (d->dtex) ID3D11Texture2D_Release(d->dtex);
            d->dtex = t;
            d->dsrv = v;
            d->dscale = j->r.scale;
            d->dx = j->r.dx;
            d->dy = j->r.dy;
            d->dw = j->r.dw;
            d->dh = j->r.dh;
            if (getenv("NV_DEBUG_SVG")) printf("vector detail %dx%d at %.0f%%: %.1f ms\n", j->r.w, j->r.h, 100 * j->r.scale, j->ms);
        } else if (t) {
            ID3D11Texture2D_Release(t);
        }
    }
    free(j->px);
    vec_unref(j->vec);
    free(j);
    redraw();   // (a view changed meanwhile asks for its own detail)
}

// Multi-page documents (CorelDRAW): [ and ] draw another page in the background; it replaces the
// shown image (the document is shared, only the page is drawn).
typedef struct {
    Decoded *old, *d;
    unsigned old_serial;          // (old may be freed meanwhile: matched by this)
    VectorDoc *vec;
    int page, index;
} PageJob;

static DWORD WINAPI page_run(void *arg) {
    PageJob *j = arg;
    Decoded *d = calloc(1, sizeof *d);
    if (d) d->serial = next_serial();
    size_t cap = 0;
    double t1 = now_ms();
    uint8_t *buf = d ? decode_vector_page(j->vec, j->page, d, &cap) : NULL;
    if (buf) {
        double t2 = now_ms();
        int up = gpu_upload(d, buf);
        buf_put(buf, cap);
        d->decode_ms = t2 - t1;
        d->upload_ms = now_ms() - t2;
        if (up) {
            vec_unref(d->vec);
            free(d);
            d = NULL;
        }
    } else {
        free(d);
        d = NULL;
    }
    j->d = d;
    PostMessageW(V.hwnd, WM_APP_PAGE, (WPARAM)j, 0);
    return 0;
}

static void show_page(int delta) {
    Decoded *d = loader_get(V.index);
    if (!d || !d->vec || d->page + delta < 0 || d->page + delta >= d->vec->pages) return;
    PageJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->old = d;
    j->old_serial = d->serial;
    j->vec = vec_ref(d->vec);
    j->page = d->page + delta;
    j->index = V.index;
    HANDLE th = CreateThread(NULL, 0, page_run, j, 0, NULL);
    if (!th) {
        vec_unref(j->vec);
        free(j);
        return;
    }
    CloseHandle(th);
}

static void page_done(PageJob *j) {   // WM_APP_PAGE
    if (j->d && loader_replace_image(j->old_serial, j->d) == V.index) {
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
    if (!V.swap || !V.rtv || V.vw <= 0 || V.vh <= 0) return;
    loader_reap();
    Decoded *d = loader_get(V.index);
    ID3D11ShaderResourceView *srv = d ? texture_for(d) : NULL;   // first: it may use the render target itself
    // not decoded yet (a slow HEIC, a slideshow faster than decoding): the previous image stays
    // on screen until it is, rather than a black window (a failed one is not waited for)
    int previous = 0;
    if (!srv && V.shown_index >= 0 && V.shown_index != V.index && !loader_failed(V.index)) {
        Decoded *o = loader_get(V.shown_index);
        if (o && (srv = texture_for(o))) {
            d = o;
            previous = 1;
        }
    }
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &V.rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, V.rtv, black);
    if (srv) {
        if (!previous && V.shown_index != V.index) {
            V.shown_index = V.index;
            V.shown_time = now_ms();
            schedule_slide();
        }
        DrawParams p = draw_params(d);
        D3D11_VIEWPORT vp = {0, 0, (float)V.vw, (float)V.vh, 0, 1};
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_cb_draw, 0, NULL, &p, 0, 0);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 1, 1, &g_cb_draw);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 1, 1, &g_cb_draw);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &srv);
        ID3D11SamplerState *ss[2] = {g_samp_lin, g_samp_pt};
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 2, ss);
        ID3D11DeviceContext_Draw(g_ctx, 4, 0);
#ifdef NV_VECTOR
        DetailRect r;
        if (!previous && detail_target(d, &r)) {
            if (d->dsrv) {   // the sharp detail over its part of the image, scaled to the current view
                double s = r.scale, l = r.ox + d->dx * s, t = r.oy + d->dy * s;
                DrawParams q = {{(float)(l / V.vw * 2 - 1), (float)(1 - t / V.vh * 2)},
                                {(float)(2 * d->dw * s / V.vw), (float)(2 * d->dh * s / V.vh)}, 1, 0, {0, 0}};
                ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_cb_draw, 0, NULL, &q, 0, 0);
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &d->dsrv);
                ID3D11DeviceContext_Draw(g_ctx, 4, 0);
            }
            if (!detail_current(d, &r)) SetTimer(V.hwnd, TIMER_DETAIL, 80, NULL);   // (each change postpones it)
        }
#endif
    }
    IDXGISwapChain1_Present(V.swap, 1, 0);
    if (srv && !previous && !V.reported) {
        V.reported = 1;
        printf("[%3d] %-40s %5dx%-5d read %5.1f ms  decode %6.1f ms (%-8s %3d bands)  key->on screen %6.1f ms%s\n",
               V.index + 1, utf8(base_name(V.files[V.index])), d->width, d->height, d->read_ms, d->decode_ms,
               mode_name(d->mode), d->bands, now_ms() - V.request_time, V.was_ready ? "  (prefetched)" : "");
        fflush(stdout);
    }
}

static void toggle_fullscreen(void) {
    if (!V.fullscreen) {
        V.saved.length = sizeof V.saved;
        GetWindowPlacement(V.hwnd, &V.saved);
        V.saved_style = GetWindowLongW(V.hwnd, GWL_STYLE);
        MONITORINFO mi = {.cbSize = sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(V.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        V.fullscreen = 1;
        V.self_resizing = 1;
        SetWindowLongW(V.hwnd, GWL_STYLE, V.saved_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(V.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        V.self_resizing = 0;
    } else {
        V.self_resizing = 1;
        SetWindowLongW(V.hwnd, GWL_STYLE, V.saved_style);
        SetWindowPlacement(V.hwnd, &V.saved);
        SetWindowPos(V.hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        V.self_resizing = 0;
        V.fullscreen = 0;
        Decoded *d = loader_get(V.index);
        if (d) size_window_for(d);
    }
    update_vector_target();
    update_title();
    redraw();
}

static void anchor_of(LPARAM lp, int screen, double *ax, double *ay) {   // device pixels from the view centre
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    if (screen) ScreenToClient(V.hwnd, &p);
    *ax = p.x - V.vw / 2.0;
    *ay = p.y - V.vh / 2.0;
}

static void on_char(wchar_t ch) {   // zoom keys by character: any keyboard layout, the numeric keypad too
    Decoded *cur = loader_get(V.index);
    switch (ch) {
    case L'+': case L'=': if (cur) zoom_by(M_SQRT2, cur, 0, 0, 0); break;
    case L'-': case L'_': if (cur) zoom_by(M_SQRT1_2, cur, 0, 0, 0); break;
    case L'0': fit_to_screen(); break;
    case L'1': case L'2': case L'3': case L'4': case L'5': case L'6': case L'7': case L'8':   // 100% .. 800%
        if (cur) set_scale(ch - L'0', cur, 0, 0);
        break;
    case L'w': case L'W': window_to_image(); break;
    case L'u': case L'U': case 26: undo_delete(); break;   // U, Ctrl+Z: undo the last Del
    case L'e': case L'E':   // DNG: brighter / (Shift) darker, 1/3 EV
        if (cur && cur->mode == MODE_NITRODNG) {
            int ev3 = MAX(-9, MIN(9, (int)g_dng_ev3 + (GetKeyState(VK_SHIFT) < 0 ? -1 : 1)));
            if (ev3 != (int)g_dng_ev3) {
                InterlockedExchange(&g_dng_ev3, ev3);
                loader_dng_exposure_changed();
            }
        }
        break;
#ifdef NV_VECTOR
    case L'[': show_page(-1); break;   // the pages of a multi-page CorelDRAW drawing
    case L']': show_page(1); break;
#endif
    default: break;
    }
}

static void open_dialog(int folder);

static void on_key(WPARAM vk) {
    Decoded *cur = loader_get(V.index);
    double step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.5 : 0.125;
    switch (vk) {
    case VK_LEFT: if (cur) pan(-step, 0, cur); break;
    case VK_RIGHT: if (cur) pan(step, 0, cur); break;
    case VK_UP: if (cur) pan(0, -step, cur); break;
    case VK_DOWN: if (cur) pan(0, step, cur); break;
    case VK_NEXT: case VK_SPACE: page(1); break;
    case VK_PRIOR: case VK_BACK: page(-1); break;
    case VK_DELETE: delete_current(); break;
    case VK_HOME: page_first(); break;
    case VK_END: page_last(); break;
    case 'F': case VK_RETURN: toggle_fullscreen(); break;
    case VK_ESCAPE:
        if (V.fullscreen) toggle_fullscreen();
        else DestroyWindow(V.hwnd);
        break;
    case 'Q': DestroyWindow(V.hwnd); break;
    case 'O': open_dialog(GetKeyState(VK_SHIFT) < 0); break;
    case 'P':   // pause slideshow
        if (V.slide_ms > 0) {
            V.paused = !V.paused;
            V.shown_time = now_ms();
            if (V.paused) KillTimer(V.hwnd, TIMER_SLIDE);
            else schedule_slide();
            update_title();
        }
        break;
    }
}

static void on_wheel(WPARAM wp, LPARAM lp) {
    int delta = (short)HIWORD(wp);   // 120 per notch; touchpads: smaller steps
    if (!delta) return;
    // Ctrl + wheel pages (forward: previous). A touchpad pinch arrives as Ctrl + wheel too,
    // but without the Ctrl key really down: that one zooms.
    if ((LOWORD(wp) & MK_CONTROL) && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (delta % 120 == 0) {
            if (delta < 0) page(1);
            else page(-1);
            return;
        }
        V.page_accum += delta;   // touchpad: one image per notch's worth
        while (V.page_accum <= -120) { V.page_accum += 120; page(1); }
        while (V.page_accum >= 120) { V.page_accum -= 120; page(-1); }
        return;
    }
    Decoded *d = loader_get(V.index);
    if (!d) return;
    double ax, ay;
    anchor_of(lp, 1, &ax, &ay);
    zoom_by(pow(M_SQRT2, delta / 120.0), d, ax, ay, delta % 120 != 0);   // one sqrt(2) step per notch
}

// Files dropped on the window or sent by a second nitroview (opened from Explorer).
static void open_paths(wchar_t **args, int n) {
    FileList l = {0};
    collect_files(&l, args, n);
    int lazy = n == 1 && !wcspbrk(args[0], L"*?") && !(GetFileAttributesW(args[0]) & FILE_ATTRIBUTE_DIRECTORY);
    if (l.n) {
        if (IsIconic(V.hwnd)) {
            V.self_resizing = 1;
            ShowWindow(V.hwnd, SW_RESTORE);
            V.self_resizing = 0;
        }
        open_files(&l, lazy);
        SetForegroundWindow(V.hwnd);
    }
    fl_free(&l);
}

static void on_drop(HDROP drop) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    wchar_t **args = calloc(n ? n : 1, sizeof *args);
    for (UINT i = 0; i < n; i++) {
        UINT l = DragQueryFileW(drop, i, NULL, 0);
        args[i] = calloc(l + 1, sizeof(wchar_t));
        DragQueryFileW(drop, i, args[i], l + 1);
    }
    DragFinish(drop);
    if (n) open_paths(args, (int)n);
    for (UINT i = 0; i < n; i++) free(args[i]);
    free(args);
}

// O: open images, Shift+O: a folder (Windows' own dialog); also when started without files.
static void open_dialog(int folder) {
    static wchar_t pattern[1024];
    if (!pattern[0])
        for (int i = 0; g_exts[i]; i++)
            swfmt(pattern + wcslen(pattern), 1024 - wcslen(pattern), L"%ls*.%ls", i ? L";" : L"", g_exts[i]);
    IFileOpenDialog *dlg;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return;
    FILEOPENDIALOGOPTIONS o = 0;
    IFileOpenDialog_GetOptions(dlg, &o);
    IFileOpenDialog_SetOptions(dlg, o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
    if (!folder) {
        COMDLG_FILTERSPEC f[2] = {{L"Images", pattern}, {L"All files", L"*.*"}};
        IFileOpenDialog_SetFileTypes(dlg, 2, f);
    }
    IShellItemArray *items;
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, V.hwnd)) && SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items))) {
        DWORD n = 0;
        IShellItemArray_GetCount(items, &n);
        wchar_t **args = calloc(n ? n : 1, sizeof *args);
        int k = 0;
        for (DWORD i = 0; args && i < n; i++) {
            IShellItem *it;
            wchar_t *path;
            if (FAILED(IShellItemArray_GetItemAt(items, i, &it))) continue;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
                if ((args[k] = _wcsdup(path))) k++;
                CoTaskMemFree(path);
            }
            IShellItem_Release(it);
        }
        IShellItemArray_Release(items);
        if (k) open_paths(args, k);
        for (int i = 0; i < k; i++) free(args[i]);
        free(args);
    }
    IFileOpenDialog_Release(dlg);
}

#define COPYDATA_OPEN 0x4E56   // "NV": full paths, each 0-terminated, then an empty one

static void on_copydata(const COPYDATASTRUCT *cd) {
    if (cd->dwData != COPYDATA_OPEN || !cd->lpData || cd->cbData < 2 * sizeof(wchar_t)) return;
    const wchar_t *p = cd->lpData, *end = p + cd->cbData / sizeof(wchar_t);
    wchar_t *args[256];
    int n = 0;
    while (p < end && *p && n < 256) {
        size_t l = wcsnlen(p, (size_t)(end - p));
        if (p + l >= end) break;
        args[n++] = (wchar_t *)p;
        p += l + 1;
    }
    if (n) open_paths(args, n);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Decoded *d;
    double ax, ay;
    switch (msg) {
    case WM_APP_OPEN: open_dialog(0); return 0;
    case WM_APP_DECODED:
        loader_reap();
        if ((int)wp >= 0) image_decoded((int)wp);
        return 0;
    case WM_APP_DNG: {
        DngJob *j = (DngJob *)wp;
        if (j->d && loader_replace_image(j->old_serial, j->d) == V.index) {
            update_title();
            redraw();
        }
        free(j->path);
        free(j);
        loader_reap();
        return 0;
    }
#ifdef NV_VECTOR
    case WM_APP_DETAIL: detail_done((DetailJob *)wp); return 0;
    case WM_APP_PAGE: page_done((PageJob *)wp); return 0;
#endif
    case WM_PAINT:
        render();
        ValidateRect(h, NULL);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        V.vw = LOWORD(lp);
        V.vh = HIWORD(lp);
        if (V.swap) {
            if (V.rtv) { ID3D11RenderTargetView_Release(V.rtv); V.rtv = NULL; }
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
            IDXGISwapChain1_ResizeBuffers(V.swap, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            make_rtv();
        }
        // any resize we didn't do ourselves (edge drag, maximize, Snap) is the user's choice
        if (!V.self_resizing && !V.fullscreen && V.started) V.user_sized = 1;
        update_vector_target();   // (full screen on / off, another screen)
        if (V.files) update_title();   // the fit percentage changes with the window size
        redraw();
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        V.self_resizing = 1;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        V.self_resizing = 0;
        return 0;
    }
    case WM_TIMER:
#ifdef NV_VECTOR
        if (wp == TIMER_DETAIL) {
            KillTimer(h, TIMER_DETAIL);
            start_detail();
            return 0;
        }
#endif
        if (wp == TIMER_SLIDE) {
            KillTimer(h, TIMER_SLIDE);
            if (!V.paused) page(1);
        }
        return 0;
    case WM_CHAR: on_char((wchar_t)wp); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp != VK_RETURN) break;   // Alt+F4 & co. stay with Windows
        on_key(wp);
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        V.dragging = 1;
        V.drag_x = (short)LOWORD(lp);
        V.drag_y = (short)HIWORD(lp);
        if (V.zoomed) SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        return 0;
    case WM_MOUSEMOVE:
        if (V.dragging && (wp & MK_LBUTTON)) {
            double x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if ((d = loader_get(V.index))) pan_pixels(x - V.drag_x, y - V.drag_y, d);
            V.drag_x = x;
            V.drag_y = y;
        }
        return 0;
    case WM_LBUTTONUP:
        V.dragging = 0;
        ReleaseCapture();
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return 0;
    case WM_LBUTTONDBLCLK:   // fit -> 100% at the clicked point, otherwise back to fit
        if ((d = loader_get(V.index))) {
            anchor_of(lp, 0, &ax, &ay);
            toggle_actual_size(d, ax, ay);
        }
        return 0;
    case WM_RBUTTONDOWN: page((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0;
    case WM_XBUTTONDOWN: page(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1); return TRUE;   // back / forward
    case WM_MOUSEWHEEL: on_wheel(wp, lp); return 0;
    case WM_DROPFILES: on_drop((HDROP)wp); return 0;
    case WM_COPYDATA: on_copydata((const COPYDATASTRUCT *)lp); return TRUE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Image size from the file header (first MB), for the first window size.
static int header_size(const wchar_t *path, double *w, double *h) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static uint8_t b[(1 << 20) + 64];   // (camera raw: Nikon's SubIFD directories are ~270 KB in)
    DWORD n = 0;
    ReadFile(f, b, 1 << 20, &n, NULL);
    CloseHandle(f);
    memset(b + n, 0, 64);
    ndng_info di;   // a DNG: its developed raw image
    if (!ndng_read_info(b, n, &di)) {
        int swap = di.orientation >= 5;
        *w = swap ? di.height : di.width;
        *h = swap ? di.width : di.height;
        return 1;
    }
    nw_info wi;   // WebP (its VP8 chunk may go on beyond what was read)
    if (!nw_read_info(b, n, &wi)) {
        int swap = wi.orientation >= 5;
        *w = swap ? wi.height : wi.width;
        *h = swap ? wi.width : wi.height;
        return 1;
    }
    nj_info ji;
    np_info pi;
    ps_info si;
    size_t eoff, esize;
    int raw_orientation;   // camera raw (CR2, NEF ...): the header of its JPEG
    if (!nj_find_embedded(b, n, &eoff, &esize, &raw_orientation)) {
        static uint8_t jb[65536 + 64];
        const uint8_t *jp = b + eoff;
        size_t jn = eoff < n ? n - eoff : 0;
        if (jn < 65536) {   // the JPEG's header lies beyond what was read: read it
            HANDLE g = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            DWORD got = 0;
            LARGE_INTEGER at = {.QuadPart = (LONGLONG)eoff};
            if (g != INVALID_HANDLE_VALUE) {
                if (SetFilePointerEx(g, at, NULL, FILE_BEGIN)) ReadFile(g, jb, 65536, &got, NULL);
                CloseHandle(g);
            }
            memset(jb + got, 0, 64);
            jp = jb;
            jn = got;
        }
        if (!nj_read_info(jp, jn, &ji)) {
            int swap = (raw_orientation ? raw_orientation : ji.orientation) >= 5;   // (RAF: the JPEG's own)
            *w = swap ? ji.height : ji.width;
            *h = swap ? ji.width : ji.height;
            return 1;
        }
    }
    if (n >= 8 && b[0] == b[1] && (b[0] == 'I' || b[0] == 'M')) {   // TIFF: from its directory (Photoshop's is at the end)
        int le = b[0] == 'I', tw, th, to;
        uint32_t dir = le ? (uint32_t)b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24
                          : (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | b[6] << 8 | b[7];
        static uint8_t db[65536];
        DWORD got = 0;
        if (dir + 2 <= n) {
            got = (DWORD)MIN(n - dir, sizeof db);
            memcpy(db, b + dir, got);
        } else {
            HANDLE g = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            LARGE_INTEGER at = {.QuadPart = (LONGLONG)dir};
            if (g != INVALID_HANDLE_VALUE) {
                if (SetFilePointerEx(g, at, NULL, FILE_BEGIN)) ReadFile(g, db, sizeof db, &got, NULL);
                CloseHandle(g);
            }
        }
        if (!nt_dir_size(db, got, le, &tw, &th, &to)) {
            *w = to >= 5 ? th : tw;
            *h = to >= 5 ? tw : th;
            return 1;
        }
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && !nj_read_info(b, n, &ji)) {
        int swap = ji.orientation >= 5;
        *w = swap ? ji.height : ji.width;
        *h = swap ? ji.width : ji.height;
        return 1;
    }
    if (!np_read_info(b, n, &pi)) {   // (an eXIf after the first MB: not seen here)
        int swap = pi.orientation >= 5;
        *w = swap ? pi.height : pi.width;
        *h = swap ? pi.width : pi.height;
        return 1;
    }
    if (n >= 26 && !memcmp(b, "8BPS", 4)) {   // (ps_read_info wants the whole file)
        *h = (double)((uint32_t)b[14] << 24 | (uint32_t)b[15] << 16 | (uint32_t)b[16] << 8 | b[17]);
        *w = (double)((uint32_t)b[18] << 24 | (uint32_t)b[19] << 16 | (uint32_t)b[20] << 8 | b[21]);
        (void)si;
        return *w > 0 && *h > 0;
    }
    return 0;
}

static int create_window(const wchar_t *first) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {.cbSize = sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"nitroview";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW;
    V.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"nitroview", L"nitroview", style, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                             NULL, NULL, hi, NULL);
    if (!V.hwnd) return -1;
    // files dropped from Explorer, and the files of a second start, even when running as
    // administrator (Windows blocks these messages from a normal process to an elevated one)
    ChangeWindowMessageFilterEx(V.hwnd, WM_DROPFILES, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(V.hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(V.hwnd, 0x0049, MSGFLT_ALLOW, NULL);   // (WM_COPYGLOBALDATA: the drop's data)
    // first size: from the header, so the window doesn't jump when the image arrives
    double w, h;
    SIZE cs;
    if (first && header_size(first, &w, &h)) cs = window_content_size(w, h, 0);
    else { SIZE mx = max_client(); cs.cx = mx.cx * 84 / 100; cs.cy = mx.cy * 84 / 100; }
    RECT wa = work_area();
    SIZE fe = frame_extra();
    LONG ww = cs.cx + fe.cx, wh = cs.cy + fe.cy;
    SetWindowPos(V.hwnd, NULL, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    RECT cr;
    GetClientRect(V.hwnd, &cr);
    V.vw = cr.right;
    V.vh = cr.bottom;
    // swap chain
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&xd);
    if (xd) IDXGIDevice_GetAdapter(xd, &ad);
    if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    if (!fac) { fprintf(stderr, "DXGI 1.2 not available\n"); return -1; }
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g_dev, V.hwnd, &sd, NULL, NULL, &V.swap);
    if (FAILED(hr)) {   // older Windows: no FLIP_DISCARD
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g_dev, V.hwnd, &sd, NULL, NULL, &V.swap);
    }
    if (FAILED(hr)) { fprintf(stderr, "swap chain: error 0x%08lx\n", (unsigned long)hr); return -1; }
    IDXGIFactory2_MakeWindowAssociation(fac, V.hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(fac);
    IDXGIAdapter_Release(ad);
    IDXGIDevice_Release(xd);
    make_rtv();
    return 0;
}

// ---------------------------------------------------------------------------
// file association (--register / --unregister): for the current user, no admin rights

static const wchar_t *const g_assoc_exts[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".psd", L".psb", L".heic",
                                              L".heif", L".avif", L".tif", L".tiff", L".webp", L".gif", L".bmp", L".cr2", L".nef", L".nrw", L".arw", L".raf", L".dng",
#ifdef NV_SVG
                                              L".svg",
#endif
#ifdef NV_CDR
                                              L".cdr", L".cmx",
#endif
                                              NULL};
#define PROGID L"NitroView.Image"

static int reg_str(const wchar_t *key, const wchar_t *name, const wchar_t *val) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS;
}

static int do_register(void) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16], icon[MAX_PATH + 8], k[256];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swfmt(cmd, MAX_PATH + 16, L"\"%ls\" \"%%1\"", exe);
    swfmt(icon, MAX_PATH + 8, L"\"%ls\",0", exe);
    int bad = 0;
    bad |= reg_str(L"Software\\Classes\\" PROGID, NULL, L"NitroView image");
    bad |= reg_str(L"Software\\Classes\\" PROGID L"\\DefaultIcon", NULL, icon);
    bad |= reg_str(L"Software\\Classes\\" PROGID L"\\shell\\open\\command", NULL, cmd);
    bad |= reg_str(L"Software\\Classes\\Applications\\nitroview.exe", L"FriendlyAppName", L"NitroView");
    bad |= reg_str(L"Software\\Classes\\Applications\\nitroview.exe\\shell\\open\\command", NULL, cmd);
    bad |= reg_str(L"Software\\NitroView\\Capabilities", L"ApplicationName", L"NitroView");
    bad |= reg_str(L"Software\\NitroView\\Capabilities", L"ApplicationDescription", L"Very fast image viewer (JPEG, PNG, PSD)");
    bad |= reg_str(L"Software\\NitroView\\Capabilities", L"ApplicationIcon", icon);
    for (int i = 0; g_assoc_exts[i]; i++) {
        const wchar_t *e = g_assoc_exts[i];
        swfmt(k, 256, L"Software\\Classes\\%ls\\OpenWithProgids", e);   // "Open with" list
        bad |= RegSetKeyValueW(HKEY_CURRENT_USER, k, PROGID, REG_NONE, NULL, 0) != ERROR_SUCCESS;
        bad |= reg_str(L"Software\\Classes\\Applications\\nitroview.exe\\SupportedTypes", e, L"");
        bad |= reg_str(L"Software\\NitroView\\Capabilities\\FileAssociations", e, PROGID);
    }
    bad |= reg_str(L"Software\\RegisteredApplications", L"NitroView", L"Software\\NitroView\\Capabilities");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    if (bad) { fprintf(stderr, "could not write the registry\n"); return 1; }
    printf("NitroView is registered for JPEG, PNG, PSD, HEIC, AVIF, TIFF, WebP, GIF and BMP (%s).\n"
           "Windows lets only you choose the default app: in the Settings page that opens now, pick NitroView\n"
           "(or right-click an image > Open with > Choose another app > NitroView, \"Always\").\n"
           "After moving nitroview.exe, run --register again; --unregister removes all of it.\n", utf8(exe));
    ShellExecuteW(NULL, L"open", L"ms-settings:defaultapps?registeredAppUser=NitroView", NULL, NULL, SW_SHOWNORMAL);
    return 0;
}

static int do_unregister(void) {
    wchar_t k[256];
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\" PROGID);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\nitroview.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\NitroView");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"NitroView");
    for (int i = 0; g_assoc_exts[i]; i++) {
        swfmt(k, 256, L"Software\\Classes\\%ls\\OpenWithProgids", g_assoc_exts[i]);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, k, PROGID);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    printf("NitroView is unregistered.\n");
    return 0;
}

// Opened from Explorer while a viewer is running: the running one shows the file.
static int send_to_running(wchar_t **args, int n) {
    HWND w = FindWindowW(L"nitroview", NULL);
    if (!w || !n) return 0;
    size_t cap = 1, len = 0;
    for (int i = 0; i < n; i++) cap += MAX_PATH * 2 + 1;
    wchar_t *buf = calloc(cap, sizeof(wchar_t));
    for (int i = 0; i < n; i++) {   // full paths: the running viewer has another current directory
        DWORD l = GetFullPathNameW(args[i], MAX_PATH * 2, buf + len, NULL);
        if (!l || l >= MAX_PATH * 2) continue;
        len += l + 1;
    }
    buf[len++] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cd = {COPYDATA_OPEN, (DWORD)(len * sizeof(wchar_t)), buf};
    DWORD_PTR res = 0;
    int ok = SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) != 0;
    free(buf);
    return ok;
}

// ---------------------------------------------------------------------------
// test modes

static int run_bench(FileList *fl) {
    double tr = 0, td = 0, tu = 0, tg = 0, tc = 0, t0 = now_ms();
    g_bench_split = 1;
    for (int i = 0; i < fl->n; i++) {
        Decoded *d = decode_file(fl->v[i]);
        if (!d) { printf("FAILED %s\n", utf8(fl->v[i])); continue; }
        double g0 = now_ms();
        texture_for(d);
        gpu_finish();
        double g = now_ms() - g0;
        printf("%-36.36s %5dx%-5d read %5.1f  decode %6.1f  upload %5.1f  convert %5.1f  mips %5.1f ms  (%s)\n",
               utf8(base_name(fl->v[i])), d->width, d->height, d->read_ms, d->decode_ms, d->upload_ms, d->conv_ms,
               g - d->conv_ms, mode_name(d->mode));
        tc += d->conv_ms;
        tr += d->read_ms;
        td += d->decode_ms;
        tu += d->upload_ms;
        tg += g;
        decoded_free(d);
    }
    double n = fl->n;
    printf("\n%d files, average ms/image: read %.1f, decode %.1f, upload to the GPU %.1f, convert %.1f (%s), mipmaps %.1f\n"
           "total %.1f ms, avg %.1f ms/image\n",
           fl->n, tr / n, td / n, tu / n, tc / n, g_convert_draw ? "draw" : "compute", (tg - tc) / n, now_ms() - t0,
           (now_ms() - t0) / n);
    return 0;
}

// Compares the GPU conversion with WIC's own decoding of the same file (premultiplied,
// like the viewer). JPEG: WIC's decoder rounds differently (small differences are normal).
static int run_selftest(FileList *fl) {
    int bad = 0;
    for (int i = 0; i < fl->n; i++) {
        const char *name = utf8(base_name(fl->v[i]));
        Decoded *d = decode_file(fl->v[i]);
        if (!d) { printf("%-30.30s FAILED to decode\n", name); bad++; continue; }
        ID3D11ShaderResourceView *srv = texture_for(d);
        D3D11_TEXTURE2D_DESC td = {(UINT)d->width, (UINT)d->height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                   D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        ID3D11Texture2D *stg = NULL;
        if (!srv || FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &stg))) {
            printf("%-30.30s GPU error\n", name);
            bad++;
            decoded_free(d);
            continue;
        }
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)d->tex, 0, NULL);
        D3D11_MAPPED_SUBRESOURCE m;
        size_t len;
        uint8_t *file = read_file(fl->v[i], &len);
        Decoded r = {0};
        size_t cap = 0;
        uint8_t *ref = file ? decode_wic(file, len, &r, &cap) : NULL;
        free(file);
        if (!ref || r.width != d->width || r.height != d->height) {
            printf("%-30.30s %-8s no WIC reference (%s)\n", name, mode_name(d->mode), ref ? "other size" : "WIC can't decode it");
        } else if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            long hist[256] = {0};
            int maxd = 0;
            double sum = 0;
            size_t cnt = 0;
            for (int y = 0; y < d->height; y++) {
                const uint8_t *g = (const uint8_t *)m.pData + (size_t)y * m.RowPitch, *q = ref + (size_t)y * r.pitch[0];
                for (int x = 0; x < d->width; x++)
                    for (int c = 0; c < 3; c++) {
                        int e = abs((int)g[x * 4 + c] - q[x * 4 + 2 - c]);   // RGBA vs BGRA
                        hist[e]++;
                        sum += e;
                        cnt++;
                        if (e > maxd) maxd = e;
                    }
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
            long le1 = hist[0] + hist[1], le3 = le1 + hist[2] + hist[3];
            double mean = sum / cnt, p3 = 100.0 * le3 / cnt;
            int ok = d->kind == KIND_YUV ? mean <= 1.0 && p3 >= 99.0 : maxd <= 1;
            printf("%-30.30s %-8s max diff %3d  mean %.3f  <=1: %8.4f%%  <=3: %8.4f%%  %s\n", name, mode_name(d->mode), maxd,
                   mean, 100.0 * le1 / cnt, p3, ok ? "OK" : "DIFFERENT");
            if (!ok) bad++;
        }
        if (ref) buf_put(ref, cap);
        ID3D11Texture2D_Release(stg);
        decoded_free(d);
    }
    printf(bad ? "SELFTEST: %d problem(s)\n" : "SELFTEST OK\n", bad);
    return bad != 0;
}

// ---------------------------------------------------------------------------

static void usage(void) {
    fprintf(stderr,
        "usage: nitroview [options] file.jpg|directory|*.png ...\n"
        "\n"
        "options:\n"
        "  -f, --fullscreen      start in full screen\n"
        "  -s, --slideshow MS    slideshow: next image after MS milliseconds on screen\n"
        "                        (waits for slow images, stops at the last one; P pauses)\n"
        "  -j N                  use N decoder threads (default: all logical CPUs)\n"
        "  --bench               load all files without a window, print timings\n"
        "  --selftest            compare the GPU output with Windows' own decoding (WIC)\n"
        "  -n, --new-window      don't pass the files to a running nitroview\n"
        "  --register            offer nitroview for JPEG, PNG, PSD ... in Explorer (current user)\n"
        "  --unregister          remove that again\n"
        "  -h, --help            this help\n"
        "\n"
        "keys:\n"
        "  PgDn Space              next image         PgUp Backspace           previous\n"
        "  Home / End              first / last       F / Enter                full screen\n"
        "  + / -                   zoom in / out      0 fit to screen          1 actual size (1:1)\n"
        "  2 .. 8                  200%% .. 800%%, pixel-exact (1 image pixel = NxN screen pixels)\n"
        "  arrows                  move a zoomed image (Shift: bigger steps)\n"
        "  W                       back to the image's own size (small images 100%%, window follows)\n"
        "  E / Shift+E             DNG raw image: brighter / darker (1/3 EV steps, kept while paging)\n"
        "  Del                     move the file to the .delete folder next to it   U / Ctrl+Z  undo\n"
        "  O / Shift+O             open images / a folder\n"
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

int wmain(int argc, wchar_t **argv) {
    int full = 0, bench = 0, selftest = 0, help = 0, newwin = 0;
    double slide_ms = 0;
    wchar_t **args = calloc((size_t)argc + 1, sizeof *args);
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"-f") || !wcscmp(a, L"--fullscreen")) full = 1;
        else if (!wcscmp(a, L"--bench")) bench = 1;
        else if (!wcscmp(a, L"--selftest")) selftest = 1;
        else if (!wcscmp(a, L"-j") && i + 1 < argc) { g_nthreads = _wtoi(argv[++i]); nj_set_max_workers(g_nthreads); }
        else if ((!wcscmp(a, L"-s") || !wcscmp(a, L"--slideshow")) && i + 1 < argc) slide_ms = _wtof(argv[++i]);
        else if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help") || !wcscmp(a, L"/?")) help = 1;
        else if (!wcscmp(a, L"-n") || !wcscmp(a, L"--new-window")) newwin = 1;
        else if (!wcscmp(a, L"--register")) { SetConsoleOutputCP(CP_UTF8); return do_register(); }
        else if (!wcscmp(a, L"--unregister")) { SetConsoleOutputCP(CP_UTF8); return do_unregister(); }
        else args[nargs++] = argv[i];
    }
    SetConsoleOutputCP(CP_UTF8);
    if (help) { usage(); return 1; }
    if (nargs && !newwin && !bench && !selftest && !full && slide_ms <= 0 && send_to_running(args, nargs)) return 0;
    FileList fl = {0};
    collect_files(&fl, args, nargs);
    if ((bench || selftest) && !fl.n) { usage(); return 1; }
    // started from Explorer (a console of its own): no console window
    DWORD pids[2];
    if (!bench && !selftest && GetConsoleProcessList(pids, 2) == 1) FreeConsole();
    {   // per-monitor DPI (Windows 10 1703+): 100% = one image pixel per screen pixel on every monitor.
        // First: the awareness can be set only once (after SetProcessDPIAware this one fails).
        typedef BOOL(WINAPI * SetCtx)(HANDLE);
        SetCtx f = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (!f || !f((HANDLE)-4)) SetProcessDPIAware();   // (-4: DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    }
    const char *cv = getenv("NV_CONVERT");
    g_convert_draw = cv && !strcmp(cv, "draw");
    if (gpu_init()) return 1;
    if (bench) return run_bench(&fl);
    if (selftest) return run_selftest(&fl);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   // (the open dialog; the decoder threads have their own)
    timeBeginPeriod(1);   // slideshow timers with 1 ms resolution

    loader_start(fl.v, fl.n);   // (the loader owns the list from now on)
    V.files = fl.v;
    V.nfiles = fl.n;
    V.shown_index = -1;
    if (create_window(fl.n ? fl.v[0] : NULL)) return 1;
    L.notify = V.hwnd;
    if (fl.n) {
        V.lazy_dir = nargs == 1 && !wcspbrk(args[0], L"*?") && !(GetFileAttributesW(args[0]) & FILE_ATTRIBUTE_DIRECTORY);
        go(0, 1);
    } else {
        update_title();
        PostMessageW(V.hwnd, WM_APP_OPEN, 0, 0);   // (once the window is on screen)
    }
    V.self_resizing = 1;   // (the first show sends WM_SIZE: not a resize by the user)
    ShowWindow(V.hwnd, SW_SHOW);
    V.self_resizing = 0;
    if (full) toggle_fullscreen();
    if (slide_ms > 0) { V.slide_ms = slide_ms; update_title(); }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);   // (the decoder thread may be busy)
}
