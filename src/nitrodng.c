// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrodng: see nitrodng.h
#include "nitrodng.h"
#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// ---------------------------------------------------------------------------
// TIFF

typedef struct {
    const uint8_t *d;
    size_t len;
    int le;
} Tiff;

static uint32_t t16(const Tiff *t, size_t o) {
    if (o + 2 > t->len) return 0;
    const uint8_t *p = t->d + o;
    return t->le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : (uint32_t)p[0] << 8 | p[1];
}

static uint32_t t32(const Tiff *t, size_t o) {
    if (o + 4 > t->len) return 0;
    return t->le ? t16(t, o) | t16(t, o + 2) << 16 : t16(t, o) << 16 | t16(t, o + 2);
}

typedef struct {
    uint32_t type, count, pos;   // pos: where the value(s) are
    int found;
} Entry;

static const int type_size[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};

static Entry tag(const Tiff *t, uint32_t ifd, unsigned want) {
    Entry e = {0, 0, 0, 0};
    if (!ifd || ifd + 2 > t->len) return e;
    uint32_t n = t16(t, ifd);
    if (n > (t->len - ifd - 2) / 12) return e;
    for (uint32_t i = 0; i < n; i++) {
        size_t p = ifd + 2 + 12 * (size_t)i;
        if (t16(t, p) != want) continue;
        e.type = t16(t, p + 2);
        e.count = t32(t, p + 4);
        uint64_t size = (uint64_t)(e.type < 13 ? type_size[e.type] : 1) * e.count;
        e.pos = size <= 4 ? (uint32_t)(p + 8) : t32(t, p + 8);
        e.found = e.pos < t->len && size <= t->len - e.pos;
        return e;
    }
    return e;
}

// the k-th value of an entry as a number (integers and rationals)
static double val(const Tiff *t, Entry e, uint32_t k) {
    if (!e.found || k >= e.count) return 0;
    switch (e.type) {
    case 1: case 7: return t->d[e.pos + k];
    case 6: return (int8_t)t->d[e.pos + k];
    case 3: return t16(t, e.pos + 2 * k);
    case 8: return (int16_t)t16(t, e.pos + 2 * k);
    case 4: return t32(t, e.pos + 4 * k);
    case 9: return (int32_t)t32(t, e.pos + 4 * k);
    case 5: {
        uint32_t den = t32(t, e.pos + 8 * k + 4);
        return den ? (double)t32(t, e.pos + 8 * k) / den : 0;
    }
    case 10: {
        int32_t den = (int32_t)t32(t, e.pos + 8 * k + 4);
        return den ? (double)(int32_t)t32(t, e.pos + 8 * k) / den : 0;
    }
    case 11: {
        uint32_t u = t32(t, e.pos + 4 * k);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    default: return 0;
    }
}

static int ival(const Tiff *t, Entry e, uint32_t k) {   // as an int (absurd values clamped)
    double v = val(t, e, k);
    return v < -1e9 ? -1000000000 : v > 1e9 ? 1000000000 : (int)v;
}

static int invert3(const double m[9], double r[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabs(det) < 1e-12) return -1;
    r[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    r[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    r[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    r[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    r[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    r[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    r[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    r[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    r[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return 0;
}

int ndng_read_info(const uint8_t *data, size_t len, ndng_info *info) {
    memset(info, 0, sizeof *info);
    if (len < 16 || !(data[0] == data[1] && (data[0] == 'I' || data[0] == 'M'))) return -1;
    Tiff t = {data, len, data[0] == 'I'};
    if (t16(&t, 2) != 42) return -1;
    uint32_t ifd0 = t32(&t, 4);
    if (!tag(&t, ifd0, 0xC612).found) return -1;   // DNGVersion: a DNG
    // the raw image: the directory (IFD0 or a SubIFD) with NewSubfileType 0
    uint32_t raw = 0;
    if (!tag(&t, ifd0, 0xFE).found || val(&t, tag(&t, ifd0, 0xFE), 0) == 0) raw = ifd0;
    Entry sub = tag(&t, ifd0, 0x14A);
    for (uint32_t i = 0; !raw && i < sub.count && i < 16; i++) {
        uint32_t s = sub.count == 1 ? (sub.type == 4 || sub.type == 13 ? t32(&t, sub.pos) : 0) : t32(&t, sub.pos + 4 * i);
        if (s && val(&t, tag(&t, s, 0xFE), 0) == 0) raw = s;
    }
    if (!raw) return -1;
    info->raw_w = ival(&t, tag(&t, raw, 0x100), 0);
    info->raw_h = ival(&t, tag(&t, raw, 0x101), 0);
    info->compression = ival(&t, tag(&t, raw, 0x103), 0);
    info->bps = ival(&t, tag(&t, raw, 0x102), 0);
    if (val(&t, tag(&t, raw, 0x106), 0) != 32803 || val(&t, tag(&t, raw, 0x115), 0) > 1) return -1;   // CFA, 1 sample
    if (tag(&t, raw, 0x153).found && val(&t, tag(&t, raw, 0x153), 0) != 1) return -1;              // unsigned ints
    if (info->compression != 1 && info->compression != 7) return -1;
    if (info->compression == 1 && info->bps != 8 && info->bps != 16) return -1;
    if (info->raw_w < 16 || info->raw_h < 16 || info->raw_w > 65535 || info->raw_h > 65535) return -1;
    // 2x2 CFA
    Entry dim = tag(&t, raw, 0x828D), pat = tag(&t, raw, 0x828E);
    if (!pat.found || pat.count != 4 || (dim.found && (val(&t, dim, 0) != 2 || val(&t, dim, 1) != 2))) return -1;
    int seen[3] = {0};
    for (int i = 0; i < 4; i++) {
        info->cfa[i] = ival(&t, pat, i);
        if (info->cfa[i] < 0 || info->cfa[i] > 2) return -1;
        seen[info->cfa[i]]++;
    }
    if (seen[0] != 1 || seen[1] != 2 || seen[2] != 1) return -1;   // a Bayer pattern
    // tiles or strips
    Entry to = tag(&t, raw, 0x144), tc = tag(&t, raw, 0x145);
    if (to.found) {
        info->tile_w = ival(&t, tag(&t, raw, 0x142), 0);
        info->tile_h = ival(&t, tag(&t, raw, 0x143), 0);
    } else {
        to = tag(&t, raw, 0x111);
        tc = tag(&t, raw, 0x117);
        info->tile_w = info->raw_w;
        info->tile_h = tag(&t, raw, 0x116).found ? ival(&t, tag(&t, raw, 0x116), 0) : info->raw_h;
        if (info->tile_h > info->raw_h || info->tile_h <= 0) info->tile_h = info->raw_h;
    }
    if (!to.found || !tc.found || to.count != tc.count || info->tile_w <= 0 || info->tile_h <= 0) return -1;
    int across = (info->raw_w + info->tile_w - 1) / info->tile_w, down = (info->raw_h + info->tile_h - 1) / info->tile_h;
    if ((uint64_t)across * down != to.count) return -1;
    info->ntiles = (int)to.count;
    info->tiles = data;
    info->tile_off_pos = to.pos;
    info->tile_off_type = to.type;
    info->tile_cnt_pos = tc.pos;
    info->tile_cnt_type = tc.type;
    info->big_endian = !t.le;
    // levels (in the raw IFD; per 2x2 position when BlackLevelRepeatDim is 2x2)
    Entry bl = tag(&t, raw, 0xC61A), blr = tag(&t, raw, 0xC619);
    int br = blr.found ? ival(&t, blr, 0) : 1, bc = blr.found ? ival(&t, blr, 1) : 1;
    for (int i = 0; i < 4; i++) {
        int k = br == 2 && bc == 2 ? i : br == 1 && bc == 2 ? (i & 1) : br == 2 && bc == 1 ? (i >> 1) : 0;
        info->black[i] = bl.found ? (float)val(&t, bl, (uint32_t)k) : 0;
    }
    Entry dh = tag(&t, raw, 0xC61B), dv = tag(&t, raw, 0xC61C);
    if (dh.found) { info->delta_h[0] = dh.type; info->delta_h[1] = dh.count; info->delta_h[2] = dh.pos; }
    if (dv.found) { info->delta_v[0] = dv.type; info->delta_v[1] = dv.count; info->delta_v[2] = dv.pos; }
    Entry wl = tag(&t, raw, 0xC61D);
    info->white = wl.found ? (float)val(&t, wl, 0) : (float)((1 << (info->bps > 16 ? 16 : info->bps)) - 1);
    Entry lt = tag(&t, raw, 0xC618);
    if (lt.found && lt.type == 3 && lt.count >= 2) {
        info->lin = data + lt.pos;
        info->lin_n = (int)lt.count;
    }
    // active area and default crop
    Entry aa = tag(&t, raw, 0xC68D);
    info->area[0] = aa.found ? ival(&t, aa, 0) : 0;
    info->area[1] = aa.found ? ival(&t, aa, 1) : 0;
    info->area[2] = aa.found ? ival(&t, aa, 2) : info->raw_h;
    info->area[3] = aa.found ? ival(&t, aa, 3) : info->raw_w;
    int aw = info->area[3] - info->area[1], ah = info->area[2] - info->area[0];
    if (info->area[0] < 0 || info->area[1] < 0 || info->area[2] > info->raw_h || info->area[3] > info->raw_w || aw < 8 || ah < 8)
        return -1;
    Entry co = tag(&t, raw, 0xC61F), cs = tag(&t, raw, 0xC620);
    info->crop[0] = co.found ? ival(&t, co, 0) : 0;
    info->crop[1] = co.found ? ival(&t, co, 1) : 0;
    info->crop[2] = cs.found ? ival(&t, cs, 0) : aw;
    info->crop[3] = cs.found ? ival(&t, cs, 1) : ah;
    if (info->crop[0] < 0 || info->crop[1] < 0 || info->crop[2] < 1 || info->crop[3] < 1 || info->crop[0] + info->crop[2] > aw ||
        info->crop[1] + info->crop[3] > ah)
        return -1;
    info->width = info->crop[2];
    info->height = info->crop[3];
    // colour: white balance, the colour matrix of D65 (else the first one), exposure, orientation
    Entry n = tag(&t, ifd0, 0xC628);
    for (int c = 0; c < 3; c++) info->neutral[c] = n.found && n.count == 3 && val(&t, n, (uint32_t)c) > 0 ? (float)val(&t, n, (uint32_t)c) : 1;
    Entry cm1 = tag(&t, ifd0, 0xC621), cm2 = tag(&t, ifd0, 0xC622);
    Entry cm = cm2.found && cm2.count == 9 && val(&t, tag(&t, ifd0, 0xC65B), 0) == 21 ? cm2 : cm1;
    if (!cm.found || cm.count != 9) return -1;
    for (int i = 0; i < 9; i++) info->cam_xyz[i] = (float)val(&t, cm, (uint32_t)i);
    info->exposure = (float)val(&t, tag(&t, ifd0, 0xC62A), 0);
    int o = ival(&t, tag(&t, ifd0, 0x112), 0);
    info->orientation = o >= 1 && o <= 8 ? o : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// lossless JPEG (T.81 process 14)

#define FAST_BITS 11
typedef struct {
    int32_t fast[1 << FAST_BITS];   // (difference << 8 | bits used) when the code and its extra bits fit, 0: not
    uint16_t code[1 << FAST_BITS];  // else (code length << 8 | value) when the code fits, 0: longer
    int maxcode[18], valptr[17], mincode[17];
    uint8_t vals[256];
} Huff;

typedef struct {
    const uint8_t *p, *end;
    uint64_t buf;
    int bits;
} BitIn;

static inline void bit_fill(BitIn *b) {
    if (b->end - b->p >= 8) {   // 8 bytes without an 0xFF among them: as many as fit at once
        uint64_t w;
        memcpy(&w, b->p, 8);
        w = ~__builtin_bswap64(w);   // (an 0xFF byte is a zero byte here)
        if (!((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull)) {
            int n = (63 - b->bits) >> 3;
            b->buf |= (~w >> (64 - 8 * n)) << (64 - 8 * n - b->bits);
            b->p += n;
            b->bits += 8 * n;
            return;
        }
    }
    while (b->bits <= 56) {
        uint32_t byte = 0;
        if (b->p < b->end) {
            byte = *b->p;
            if (byte == 0xFF) {
                if (b->p + 1 < b->end && b->p[1] == 0) b->p += 2;   // stuffed zero
                else byte = 0;                                       // a marker: zeros from here on
                if (byte) {
                    b->buf |= (uint64_t)byte << (56 - b->bits);
                    b->bits += 8;
                    continue;
                }
            } else {
                b->p++;
            }
        }
        b->buf |= (uint64_t)byte << (56 - b->bits);
        b->bits += 8;
    }
}

static inline uint32_t bit_peek(BitIn *b, int n) { return (uint32_t)(b->buf >> (64 - n)); }
static inline void bit_skip(BitIn *b, int n) {
    b->buf <<= n;
    b->bits -= n;
}

static int build_huff(Huff *h, const uint8_t *counts, const uint8_t *vals, int total) {
    memset(h, 0, sizeof *h);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        for (int i = 0; i < counts[len - 1]; i++) {
            if (k >= total) return -1;
            h->vals[k] = vals[k];
            int sz = vals[k] < 16 ? vals[k] : 0, used = len + sz;
            if (len <= FAST_BITS) {   // every continuation of the code: its difference decoded if it fits
                int shift = FAST_BITS - len;
                for (int f = 0; f < 1 << shift; f++) h->code[(code << shift) | f] = (uint16_t)(len << 8 | vals[k]);
            }
            if (used <= FAST_BITS) {
                int shift = FAST_BITS - len;
                for (int f = 0; f < 1 << shift; f++) {
                    int extra = sz ? f >> (shift - sz) : 0;
                    int diff = vals[k] == 0 ? 0 : vals[k] >= 16 ? 32768 : extra < (1 << (sz - 1)) ? extra - (1 << sz) + 1 : extra;
                    h->fast[(code << shift) | f] = (int32_t)((uint32_t)diff << 8 | (uint32_t)used);
                }
            }
            k++;
            code++;
        }
        h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    return 0;
}

static inline int huff_decode(BitIn *b, const Huff *h) {   // the difference of one sample
    if (b->bits < 32) bit_fill(b);
    int32_t f = h->fast[bit_peek(b, FAST_BITS)];
    if (f) {
        bit_skip(b, f & 255);
        return f >> 8;
    }
    int s, len;
    uint16_t g = h->code[bit_peek(b, FAST_BITS)];
    if (g) {
        len = g >> 8;
        s = g & 255;
    } else {
        len = FAST_BITS + 1;
        int code = (int)bit_peek(b, len);
        while (len < 16 && (h->maxcode[len] < 0 || code > h->maxcode[len])) {
            len++;
            code = (int)bit_peek(b, len);
        }
        if (h->maxcode[len] < 0 || code > h->maxcode[len]) return 0;
        s = h->vals[h->valptr[len] + code - h->mincode[len]];
    }
    bit_skip(b, len);
    if (s == 0) return 0;
    if (s >= 16) return 32768;
    int v = (int)bit_peek(b, s);
    bit_skip(b, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

// Decodes one lossless JPEG (a tile or strip) into out (tw x th samples, row stride ostride).
static int lossless_jpeg(const uint8_t *d, size_t n, uint16_t *out, int tw, int th, size_t ostride) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    Huff *huff = malloc(4 * sizeof(Huff));
    if (!huff) return -1;
    int have[4] = {0}, w = 0, h = 0, nc = 0, prec = 0, comp_id[4] = {0}, tbl[4] = {0}, pred = 1, pt = 0, ri = 0;
    size_t p = 2;
    int rc = -1;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) goto out;
        int m = d[p + 1];
        if (m == 0xFF) {
            p++;
            continue;
        }
        size_t l = (size_t)d[p + 2] << 8 | d[p + 3];
        if (l < 2 || p + 2 + l > n) goto out;
        const uint8_t *s = d + p + 4;
        if (m == 0xC3) {   // SOF3
            if (l < 8) goto out;
            prec = s[0];
            h = s[1] << 8 | s[2];
            w = s[3] << 8 | s[4];
            nc = s[5];
            if (nc < 1 || nc > 4 || l < 8 + 3 * (size_t)nc) goto out;
            for (int c = 0; c < nc; c++) {
                comp_id[c] = s[6 + 3 * c];
                if (s[7 + 3 * c] != 0x11) goto out;   // (no subsampling)
            }
        } else if (m == 0xC4) {   // DHT
            size_t q = 0;
            while (q + 17 <= l - 2) {
                int cls = s[q] >> 4, id = s[q] & 15;
                int total = 0;
                for (int i = 0; i < 16; i++) total += s[q + 1 + i];
                if (cls != 0 || id > 3 || total > 256 || q + 17 + (size_t)total > l - 2) goto out;
                if (build_huff(&huff[id], s + q + 1, s + q + 17, total)) goto out;
                have[id] = 1;
                q += 17 + (size_t)total;
            }
        } else if (m == 0xDD) {   // DRI
            if (l < 4) goto out;
            ri = s[0] << 8 | s[1];
        } else if (m == 0xDA) {   // SOS: the scan
            if (l < 3) goto out;
            int ns = s[0];
            if (ns != nc || l < 6 + 2 * (size_t)ns) goto out;
            for (int c = 0; c < ns; c++) {
                if (s[1 + 2 * c] != comp_id[c]) goto out;
                tbl[c] = s[2 + 2 * c] >> 4;
                if (tbl[c] > 3 || !have[tbl[c]]) goto out;
            }
            pred = s[1 + 2 * ns];
            pt = s[3 + 2 * ns] & 15;
            if (pred < 1 || pred > 7 || prec < 2 || prec > 16 || w < 1 || h < 1) goto out;
            // the samples fill the tile row by row (a JPEG row may hold one or two raw rows; the
            // last strip may be shorter)
            if ((uint64_t)w * nc * h > (uint64_t)tw * th) goto out;
            size_t rowlen = (size_t)w * nc;
            uint16_t *prev = calloc(rowlen, 2), *cur = calloc(rowlen, 2);
            if (!prev || !cur) {
                free(prev);
                free(cur);
                goto out;
            }
            BitIn b = {d + p + 2 + l, d + n, 0, 0};
            int initial = 1 << (prec - pt - 1);
            int mcus = 0, first_row = 1;
            size_t ox = 0, oy = 0;   // output position
            for (int y = 0; y < h; y++) {
                if (!ri) {   // (the usual case) without restarts: a loop per predictor
                    const Huff *hc[4] = {&huff[tbl[0]], &huff[tbl[1 % nc]], &huff[tbl[2 % nc]], &huff[tbl[3 % nc]]};
                    for (int c = 0; c < nc; c++) cur[c] = (uint16_t)((first_row ? initial : prev[c]) + huff_decode(&b, hc[c]));
                    #define ROW(PX) for (size_t x = 1; x < (size_t)w; x++) \
                        for (int c = 0; c < nc; c++) { \
                            size_t i = x * nc + c; \
                            int ra = cur[i - nc], rb = prev[i], rcc = prev[i - nc]; \
                            (void)ra; (void)rb; (void)rcc; \
                            cur[i] = (uint16_t)((PX) + huff_decode(&b, hc[c])); \
                        }
                    switch (first_row ? 1 : pred) {
                    case 1: ROW(ra) break;
                    case 2: ROW(rb) break;
                    case 3: ROW(rcc) break;
                    case 4: ROW(ra + rb - rcc) break;
                    case 5: ROW(ra + ((rb - rcc) >> 1)) break;
                    case 6: ROW(rb + ((ra - rcc) >> 1)) break;
                    default: ROW((ra + rb) >> 1) break;
                    }
                    #undef ROW
                } else for (int x = 0; x < w; x++) {
                    if (ri && mcus == ri) {   // restart: realign to the marker, predictors reset
                        b.bits = 0;
                        b.buf = 0;
                        while (b.p + 1 < b.end && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) b.p++;
                        if (b.p + 1 < b.end) b.p += 2;
                        mcus = 0;
                        first_row = 1;
                    }
                    for (int c = 0; c < nc; c++) {
                        int diff = huff_decode(&b, &huff[tbl[c]]);
                        size_t i = (size_t)x * nc + c;
                        int ra = x ? cur[i - nc] : 0, rb = prev[i], rcc = x ? prev[i - nc] : 0, px;
                        if (first_row) px = x ? ra : initial;
                        else if (x == 0) px = rb;
                        else switch (pred) {
                            case 1: px = ra; break;
                            case 2: px = rb; break;
                            case 3: px = rcc; break;
                            case 4: px = ra + rb - rcc; break;
                            case 5: px = ra + ((rb - rcc) >> 1); break;
                            case 6: px = rb + ((ra - rcc) >> 1); break;
                            default: px = (ra + rb) >> 1; break;
                            }
                        cur[i] = (uint16_t)(px + diff);
                    }
                    mcus++;
                    if (ri && mcus == ri && x + 1 == w) first_row = 1;   // (handled at the next sample)
                }
                for (size_t i = 0; i < rowlen; i++) {
                    out[oy * ostride + ox] = (uint16_t)(cur[i] << pt);
                    if (++ox == (size_t)tw) {
                        ox = 0;
                        oy++;
                    }
                }
                uint16_t *t2 = prev;
                prev = cur;
                cur = t2;
                first_row = 0;
            }
            free(prev);
            free(cur);
            rc = 0;
            goto out;
        } else if (m == 0xD9) {
            break;
        }
        p += 2 + l;
    }
out:
    free(huff);
    return rc;
}

// ---------------------------------------------------------------------------
// development

static void run(size_t n, int nthreads, nitro_body body) {   // nthreads 1: on the calling thread
    if (nthreads == 1)
        for (size_t i = 0; i < n; i++) body(i);
    else
        nitro_parallel(n, body);
}

static const float xyz_rgb[9] = {0.412453f, 0.357580f, 0.180423f, 0.212671f, 0.715160f,
                                 0.072169f, 0.019334f, 0.119193f, 0.950227f};   // linear sRGB -> XYZ (D65)

int ndng_decode(const uint8_t *data, size_t len, const ndng_info *in, uint8_t *bgrx, size_t pitch, int nthreads) {
    Tiff t = {data, len, !in->big_endian};
    int rw = in->raw_w, rh = in->raw_h;
    uint16_t *raw = malloc((size_t)rw * rh * 2);
    if (!raw) return -1;
    // 1. the raw samples
    _Atomic int failed = 0;
    _Atomic int *pf = &failed;
    int across = (rw + in->tile_w - 1) / in->tile_w;
    const ndng_info *pin = in;
    Tiff *pt = &t;
    uint16_t *praw = raw;
    run((size_t)in->ntiles, nthreads, ^(size_t i) {
        Entry eo = {pin->tile_off_type, (uint32_t)pin->ntiles, pin->tile_off_pos, 1};
        Entry ec = {pin->tile_cnt_type, (uint32_t)pin->ntiles, pin->tile_cnt_pos, 1};
        uint64_t off = (uint64_t)val(pt, eo, (uint32_t)i), cnt = (uint64_t)val(pt, ec, (uint32_t)i);
        if (off >= len || cnt > len - off) {
            atomic_store(pf, 1);
            return;
        }
        int tx = (int)(i % (size_t)across) * pin->tile_w, ty = (int)(i / (size_t)across) * pin->tile_h;
        int w = MIN(pin->tile_w, rw - tx), h = MIN(pin->tile_h, rh - ty);
        const uint8_t *src = data + off;
        if (pin->compression == 7) {
            // a whole tile is coded (also its part beyond the image): decode into a scratch buffer
            uint16_t *tile = malloc((size_t)pin->tile_w * pin->tile_h * 2);
            if (!tile || lossless_jpeg(src, (size_t)cnt, tile, pin->tile_w, pin->tile_h, (size_t)pin->tile_w)) {
                free(tile);
                atomic_store(pf, 1);
                return;
            }
            for (int y = 0; y < h; y++) memcpy(praw + (size_t)(ty + y) * rw + tx, tile + (size_t)y * pin->tile_w, (size_t)w * 2);
            free(tile);
        } else {
            int bytes = pin->bps / 8;
            if ((uint64_t)pin->tile_w * h * bytes > cnt) {
                atomic_store(pf, 1);
                return;
            }
            for (int y = 0; y < h; y++) {
                const uint8_t *s = src + (size_t)y * pin->tile_w * bytes;
                uint16_t *o = praw + (size_t)(ty + y) * rw + tx;
                if (bytes == 1)
                    for (int x = 0; x < w; x++) o[x] = s[x];
                else if (pt->le)
                    for (int x = 0; x < w; x++) o[x] = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                else
                    for (int x = 0; x < w; x++) o[x] = (uint16_t)(s[2 * x] << 8 | s[2 * x + 1]);
            }
        }
    });
    if (atomic_load(&failed)) {
        free(raw);
        return -1;
    }
    // 2. colour: camera -> linear sRGB (rows normalised so white stays white), white balance
    double cam_rgb[9], rgb_cam[9];
    for (int i = 0; i < 3; i++) {
        double sum = 0;
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int k = 0; k < 3; k++) v += in->cam_xyz[i * 3 + k] * xyz_rgb[k * 3 + j];
            cam_rgb[i * 3 + j] = v;
            sum += v;
        }
        for (int j = 0; j < 3; j++) cam_rgb[i * 3 + j] /= sum ? sum : 1;
    }
    if (invert3(cam_rgb, rgb_cam)) {
        free(raw);
        return -1;
    }
    double wb[3], wbmin = 1e9;
    for (int c = 0; c < 3; c++) {
        wb[c] = 1.0 / in->neutral[c];
        wbmin = MIN(wbmin, wb[c]);
    }
    float scale[4], black[4];   // per 2x2 position: (v - black) * scale, white balance included
    for (int i = 0; i < 4; i++) {
        black[i] = in->black[i];
        float range = in->white - in->black[i];
        scale[i] = range > 0 ? (float)(wb[in->cfa[i]] / wbmin / range) : 0;
    }
    float exposure = powf(2.0f, MIN(MAX(in->exposure, -8.0f), 8.0f));
    float m[9];
    for (int i = 0; i < 9; i++) m[i] = (float)rgb_cam[i] * exposure;
    // linear 0..2 -> sRGB 8-bit, through a simple tone curve: raw converters brighten the mid
    // tones (+0.6 EV here) and roll the highlights off softly instead of clipping them
    uint8_t lut[8193];
    for (int i = 0; i <= 8192; i++) {
        double x = i / 4096.0 * 1.5;
        double v = x <= 0.75 ? x : 0.75 + 0.25 * (1 - exp(-(x - 0.75) / 0.25));
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
        lut[i] = (uint8_t)lrint(MIN(MAX(v, 0), 1) * 255);
    }
    // 3. demosaic (Malvar-He-Cutler) and convert, row by row: the active area's samples are
    // normalised on the fly (mirrored at its edges)
    int ax = in->area[1], ay = in->area[0], aw = in->area[3] - ax, ah = in->area[2] - ay;
    int cx = in->crop[0], cy = in->crop[1], ow = in->crop[2], oh = in->crop[3];
    // per column / row black level deltas of the active area; the linearization table
    float *bh = calloc((size_t)aw + (size_t)ah + 32768, sizeof(float)), *bv = bh ? bh + aw : NULL;
    if (!bh) {
        free(raw);
        return -1;
    }
    const uint16_t *lin = NULL;
    int lin_n = 0;
    if (in->lin) {
        uint16_t *lintab = (uint16_t *)(bv + ah);
        lin_n = MIN(in->lin_n, 65536);
        for (int i = 0; i < lin_n; i++) lintab[i] = (uint16_t)val(&t, (Entry){3, (uint32_t)in->lin_n, (uint32_t)(in->lin - data), 1}, (uint32_t)i);
        lin = lintab;
    }
    if (in->delta_h[1]) {
        Entry e = {in->delta_h[0], in->delta_h[1], in->delta_h[2], 1};
        for (int i = 0; i < aw && (uint32_t)i < e.count; i++) bh[i] = (float)val(&t, e, (uint32_t)i);
    }
    if (in->delta_v[1]) {
        Entry e = {in->delta_v[0], in->delta_v[1], in->delta_v[2], 1};
        for (int i = 0; i < ah && (uint32_t)i < e.count; i++) bv[i] = (float)val(&t, e, (uint32_t)i);
    }
    const float *pbh = bh, *pbv = bv;
    const uint16_t *plin = lin;
    const float *pscale = scale, *pblack = black, *pm = m;
    const uint8_t *plut = lut;
    const int *pcfa = in->cfa;
    size_t bands = (size_t)(oh + 15) / 16;
    run(bands, nthreads, ^(size_t band) {
        int y0 = (int)band * 16, y1 = MIN(y0 + 16, oh);
        int pw = ow + 4;
        float *win = malloc(sizeof(float) * (size_t)pw * (y1 - y0 + 4));   // the band's normalised samples, +-2 rows / columns
        if (!win) return;
        // normalised sample at active-area coordinates (mirrored inside the area)
        #define SAMPLE(X, Y) ({ \
            int xx_ = (X), yy_ = (Y); \
            if (xx_ < 0) xx_ = -xx_; else if (xx_ >= aw) xx_ = 2 * aw - 2 - xx_; \
            if (yy_ < 0) yy_ = -yy_; else if (yy_ >= ah) yy_ = 2 * ah - 2 - yy_; \
            int k_ = (yy_ & 1) * 2 + (xx_ & 1); \
            unsigned v_ = praw[(size_t)(ay + yy_) * rw + ax + xx_]; \
            if (plin) v_ = plin[MIN(v_, (unsigned)lin_n - 1)]; \
            float f_ = ((float)v_ - pblack[k_] - pbh[xx_] - pbv[yy_]) * pscale[k_]; \
            f_ < 0 ? 0 : f_ > 1 ? 1 : f_; })
        for (int r = 0; r < y1 - y0 + 4; r++)
            for (int x = 0; x < pw; x++) win[r * pw + x] = SAMPLE(cx + x - 2, cy + y0 + r - 2);
        for (int y = y0; y < y1; y++) {
            int sy = cy + y;
            uint8_t *o = bgrx + (size_t)y * pitch;
            for (int x = 0; x < ow; x++) {
                int sx = cx + x;
                const float *c = win + (y - y0 + 2) * pw + x + 2;   // the current sample; rows +-pw
                #define P(dx, dy) c[(dy) * pw + (dx)]
                int col = pcfa[(sy & 1) * 2 + (sx & 1)];
                float r, g, b, v = P(0, 0);
                float cross = P(-1, 0) + P(1, 0) + P(0, -1) + P(0, 1), outer = P(-2, 0) + P(2, 0) + P(0, -2) + P(0, 2);
                float diag = P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1);
                if (col == 1) {   // green: red / blue horizontally or vertically
                    float h = (5 * v + 4 * (P(-1, 0) + P(1, 0)) - diag - (P(-2, 0) + P(2, 0)) + 0.5f * (P(0, -2) + P(0, 2))) / 8;
                    float vv = (5 * v + 4 * (P(0, -1) + P(0, 1)) - diag - (P(0, -2) + P(0, 2)) + 0.5f * (P(-2, 0) + P(2, 0))) / 8;
                    int next = pcfa[(sy & 1) * 2 + ((sx + 1) & 1)];   // the colour beside it in this row
                    g = v;
                    r = next == 0 ? h : vv;
                    b = next == 0 ? vv : h;
                } else {
                    float gg = (4 * v + 2 * cross - outer) / 8;
                    float other = (6 * v + 2 * diag - 1.5f * outer) / 8;
                    g = gg;
                    if (col == 0) { r = v; b = other; }
                    else { b = v; r = other; }
                }
                #undef P
                float R = pm[0] * r + pm[1] * g + pm[2] * b;
                float G = pm[3] * r + pm[4] * g + pm[5] * b;
                float B = pm[6] * r + pm[7] * g + pm[8] * b;
                #define Q(X) (!((X) > 0) ? 0 : (X) >= 2 ? 8192 : (int)((X) * 4096))   // (NaN: 0)
                o[4 * x + 0] = plut[Q(B)];
                o[4 * x + 1] = plut[Q(G)];
                o[4 * x + 2] = plut[Q(R)];
                #undef Q
                o[4 * x + 3] = 255;
            }
        }
        #undef SAMPLE
        free(win);
    });
    free(bh);
    free(raw);
    return 0;
}
