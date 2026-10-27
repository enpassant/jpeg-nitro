// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro-convert: decoded planes -> RGB, multi-threaded. See nitro-convert.h.
#include "nitro-convert.h"
#include "../nitro_os.h"

#include <stdlib.h>
#include <string.h>

// libjpeg's YCbCr -> RGB (jdcolor.c): 16-bit fixed point, the same rounding.
#define SCALEBITS 16
#define ONE_HALF ((int32_t)1 << (SCALEBITS - 1))
#define FIX(x) ((int32_t)((x) * (1L << SCALEBITS) + 0.5))

typedef struct {
    int y[256], cr_r[256], cb_b[256];
    int32_t cr_g[256], cb_g[256];
} Tables;

// full range (JPEG, HEIC); limited range (WebP / VP8: Y 16..235, Cb Cr 16..240): the same, scaled
static void build_tables(Tables *t, int limited) {
    double ky = limited ? 255.0 / 219 : 1, kc = limited ? 255.0 / 224 : 1;
    for (int i = 0; i < 256; i++) {
        int32_t x = i - 128;
        t->y[i] = limited ? (int)((FIX(ky) * (i - 16) + ONE_HALF) >> SCALEBITS) : i;
        t->cr_r[i] = (int)((FIX(1.40200 * kc) * x + ONE_HALF) >> SCALEBITS);
        t->cb_b[i] = (int)((FIX(1.77200 * kc) * x + ONE_HALF) >> SCALEBITS);
        t->cr_g[i] = -FIX(0.71414 * kc) * x;
        t->cb_g[i] = -FIX(0.34414 * kc) * x + ONE_HALF;
    }
}

static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

typedef struct {
    const uint8_t *p;
    size_t pitch;
    int cw, ch;   // valid (downsampled) size, as libjpeg computes it
    int hr, vr;   // upsampling factors: hmax / h, vmax / v (0: not a whole number)
    int h, hmax, v, vmax;   // the sampling factors themselves (replication: also for 3 / 2 ...)
} Comp;

// One component's samples for output row y, upsampled to full width, into out
// (at least 2 * cw bytes). cs: scratch of cw ints.
static void upsample_row(const Comp *c, int y, int width, uint8_t *out, int *cs) {
    const int n = c->cw;
    if (c->hr == 1 && c->vr == 1) {
        memcpy(out, c->p + (size_t)y * c->pitch, (size_t)width);
    } else if (c->hr == 2 && c->vr == 1 && n > 2) {   // h2v1 "fancy": 3/4 near + 1/4 far
        const uint8_t *in = c->p + (size_t)y * c->pitch;
        out[0] = in[0];
        out[1] = (uint8_t)((in[0] * 3 + in[1] + 2) >> 2);
        for (int i = 1; i < n - 1; i++) {
            int v = in[i] * 3;
            out[2 * i] = (uint8_t)((v + in[i - 1] + 1) >> 2);
            out[2 * i + 1] = (uint8_t)((v + in[i + 1] + 2) >> 2);
        }
        out[2 * n - 2] = (uint8_t)((in[n - 1] * 3 + in[n - 2] + 1) >> 2);
        out[2 * n - 1] = in[n - 1];
    } else if (c->hr == 2 && c->vr == 2 && n > 2) {   // h2v2 "fancy": the row above / below, then across
        int r = y >> 1, nb = (y & 1) ? (r + 1 < c->ch ? r + 1 : c->ch - 1) : (r > 0 ? r - 1 : 0);
        const uint8_t *in0 = c->p + (size_t)r * c->pitch, *in1 = c->p + (size_t)nb * c->pitch;
        for (int i = 0; i < n; i++) cs[i] = in0[i] * 3 + in1[i];
        out[0] = (uint8_t)((cs[0] * 4 + 8) >> 4);
        out[1] = (uint8_t)((cs[0] * 3 + cs[1] + 7) >> 4);
        for (int i = 1; i < n - 1; i++) {
            out[2 * i] = (uint8_t)((cs[i] * 3 + cs[i - 1] + 8) >> 4);
            out[2 * i + 1] = (uint8_t)((cs[i] * 3 + cs[i + 1] + 7) >> 4);
        }
        out[2 * n - 2] = (uint8_t)((cs[n - 1] * 3 + cs[n - 2] + 8) >> 4);
        out[2 * n - 1] = (uint8_t)((cs[n - 1] * 4 + 7) >> 4);
    } else if (c->hr == 1 && c->vr == 2) {             // h1v2 "fancy"
        int r = y >> 1, nb = (y & 1) ? (r + 1 < c->ch ? r + 1 : c->ch - 1) : (r > 0 ? r - 1 : 0);
        int bias = (y & 1) ? 2 : 1;
        const uint8_t *in0 = c->p + (size_t)r * c->pitch, *in1 = c->p + (size_t)nb * c->pitch;
        for (int x = 0; x < width; x++) out[x] = (uint8_t)((in0[x] * 3 + in1[x] + bias) >> 2);
    } else {                                            // other factors (and tiny images): replication
        const uint8_t *in = c->p + (size_t)y * c->v / c->vmax * c->pitch;
        for (int x = 0; x < width; x++) out[x] = in[(size_t)x * c->h / c->hmax];
    }
}

static void convert(const Comp *comps, int nc, int W, int H, int limited, uint8_t *dst, size_t stride) {
    int maxcw = W;
    for (int k = 0; k < nc; k++)
        if (2 * comps[k].cw > maxcw) maxcw = 2 * comps[k].cw;
    Tables tab;
    build_tables(&tab, limited);
    const Comp *cp = comps;
    const Tables *t = &tab;
    size_t bands = (size_t)nitro_ncpu() * 4;
    if (bands > (size_t)H) bands = (size_t)H;
    const size_t rowlen = (size_t)maxcw + 16;
    nitro_parallel(bands, ^(size_t b) {
        int y0 = (int)((size_t)H * b / bands), y1 = (int)((size_t)H * (b + 1) / bands);
        uint8_t *rows = malloc(3 * rowlen);
        int *cs = malloc(sizeof(int) * rowlen);
        if (!rows || !cs) { free(rows); free(cs); return; }
        uint8_t *yr = rows, *cbr = rows + rowlen, *crr = rows + 2 * rowlen;
        for (int y = y0; y < y1; y++) {
            uint8_t *o = dst + (size_t)y * stride;
            upsample_row(&cp[0], y, W, yr, cs);
            if (nc == 1) {
                for (int x = 0; x < W; x++) o[3 * x] = o[3 * x + 1] = o[3 * x + 2] = yr[x];
                continue;
            }
            upsample_row(&cp[1], y, W, cbr, cs);
            upsample_row(&cp[2], y, W, crr, cs);
            for (int x = 0; x < W; x++) {
                int Y = t->y[yr[x]], cb = cbr[x], cr = crr[x];
                o[3 * x] = clamp255(Y + t->cr_r[cr]);
                o[3 * x + 1] = clamp255(Y + (int)((t->cb_g[cb] + t->cr_g[cr]) >> SCALEBITS));
                o[3 * x + 2] = clamp255(Y + t->cb_b[cb]);
            }
        }
        free(rows);
        free(cs);
    });
}

void nc_jpeg_to_rgb(const nj_info *fi, uint8_t *const planes[3], const size_t pitch[3], uint8_t *dst, size_t stride) {
    const int W = fi->width, H = fi->height;
    Comp comps[3];
    for (int k = 0; k < fi->ncomp; k++) {
        Comp *c = &comps[k];
        c->p = planes[k];
        c->pitch = pitch[k];
        c->cw = (W * fi->h[k] + fi->hmax - 1) / fi->hmax;
        c->ch = (H * fi->v[k] + fi->vmax - 1) / fi->vmax;
        c->h = fi->h[k];
        c->hmax = fi->hmax;
        c->v = fi->v[k];
        c->vmax = fi->vmax;
        c->hr = fi->hmax % fi->h[k] ? 0 : fi->hmax / fi->h[k];   // (h 2 of hmax 3: replication)
        c->vr = fi->vmax % fi->v[k] ? 0 : fi->vmax / fi->v[k];
    }
    convert(comps, fi->ncomp, W, H, 0, dst, stride);
}

void nc_yuv420_to_rgb(int width, int height, uint8_t *const planes[3], const size_t pitch[3], int limited, uint8_t *dst,
                      size_t stride) {
    Comp comps[3];
    for (int k = 0; k < 3; k++) {
        Comp *c = &comps[k];
        c->p = planes[k];
        c->pitch = pitch[k];
        c->cw = k ? (width + 1) / 2 : width;
        c->ch = k ? (height + 1) / 2 : height;
        c->hr = c->vr = k ? 2 : 1;
        c->h = c->v = 1;
        c->hmax = c->vmax = c->hr;
    }
    convert(comps, 3, width, height, limited, dst, stride);
}

void nc_psd_to_rgb(const ps_info *si, const uint8_t *planes, uint8_t *dst, size_t stride) {
    const int W = si->width, H = si->height;
    const size_t ps = si->plane_size;
    const uint8_t *p0 = planes, *p1 = si->ncolor == 3 ? planes + ps : planes, *p2 = si->ncolor == 3 ? planes + 2 * ps : planes;
    const uint8_t *pa = si->alpha ? planes + ps * (size_t)si->ncolor : NULL;
    size_t bands = (size_t)nitro_ncpu() * 4;
    if (bands > (size_t)H) bands = (size_t)H;
    nitro_parallel(bands, ^(size_t b) {
        int y0 = (int)((size_t)H * b / bands), y1 = (int)((size_t)H * (b + 1) / bands);
        for (int y = y0; y < y1; y++) {
            size_t i = (size_t)y * W;
            uint8_t *o = dst + (size_t)y * stride;
            for (int x = 0; x < W; x++, i++) {
                int r = p0[i], g = p1[i], bl = p2[i];
                if (pa) {   // matted with white -> over black
                    int a = pa[i] - 255;
                    r = r + a < 0 ? 0 : r + a;
                    g = g + a < 0 ? 0 : g + a;
                    bl = bl + a < 0 ? 0 : bl + a;
                }
                o[3 * x] = (uint8_t)r;
                o[3 * x + 1] = (uint8_t)g;
                o[3 * x + 2] = (uint8_t)bl;
            }
        }
    });
}
