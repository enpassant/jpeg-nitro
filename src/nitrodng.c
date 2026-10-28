// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrodng: see nitrodng.h
#include "nitrodng.h"
#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)
#include "tiff.h"       // the TIFF directory (shared with nitrotiff, nitrojpeg ...)

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// ---------------------------------------------------------------------------
// TIFF (tiff.h)

static int ival(const Tiff *t, Entry e, uint32_t k) {   // as an int (absurd values clamped)
    double v = tiff_val(t, e, k);
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
    if (!tag(&t, ifd0, 0xFE).found || tiff_val(&t, tag(&t, ifd0, 0xFE), 0) == 0) raw = ifd0;
    Entry sub = tag(&t, ifd0, 0x14A);
    for (uint32_t i = 0; !raw && i < sub.count && i < 16; i++) {
        uint32_t s = sub.count == 1 ? (sub.type == 4 || sub.type == 13 ? t32(&t, sub.pos) : 0) : t32(&t, sub.pos + 4 * i);
        if (s && tiff_val(&t, tag(&t, s, 0xFE), 0) == 0) raw = s;
    }
    if (!raw) return -1;
    info->raw_w = ival(&t, tag(&t, raw, 0x100), 0);
    info->raw_h = ival(&t, tag(&t, raw, 0x101), 0);
    info->compression = ival(&t, tag(&t, raw, 0x103), 0);
    info->bps = ival(&t, tag(&t, raw, 0x102), 0);
    if (tiff_val(&t, tag(&t, raw, 0x106), 0) != 32803 || tiff_val(&t, tag(&t, raw, 0x115), 0) > 1) return -1;   // CFA, 1 sample
    if (tag(&t, raw, 0x153).found && tiff_val(&t, tag(&t, raw, 0x153), 0) != 1) return -1;              // unsigned ints
    if (info->compression != 1 && info->compression != 7) return -1;
    if (info->compression == 1 && info->bps != 8 && info->bps != 16) return -1;
    if (info->raw_w < 16 || info->raw_h < 16 || info->raw_w > 65535 || info->raw_h > 65535) return -1;
    // 2x2 CFA
    Entry dim = tag(&t, raw, 0x828D), pat = tag(&t, raw, 0x828E);
    if (!pat.found || pat.count != 4 || (dim.found && (tiff_val(&t, dim, 0) != 2 || tiff_val(&t, dim, 1) != 2))) return -1;
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
        info->black[i] = bl.found ? (float)tiff_val(&t, bl, (uint32_t)k) : 0;
    }
    Entry dh = tag(&t, raw, 0xC61B), dv = tag(&t, raw, 0xC61C);
    if (dh.found) { info->delta_h[0] = dh.type; info->delta_h[1] = dh.count; info->delta_h[2] = dh.pos; }
    if (dv.found) { info->delta_v[0] = dv.type; info->delta_v[1] = dv.count; info->delta_v[2] = dv.pos; }
    Entry wl = tag(&t, raw, 0xC61D);
    info->white = wl.found ? (float)tiff_val(&t, wl, 0) : (float)((1 << (info->bps > 16 ? 16 : info->bps)) - 1);
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
    for (int c = 0; c < 3; c++) info->neutral[c] = n.found && n.count == 3 && tiff_val(&t, n, (uint32_t)c) > 0 ? (float)tiff_val(&t, n, (uint32_t)c) : 1;
    Entry cm1 = tag(&t, ifd0, 0xC621), cm2 = tag(&t, ifd0, 0xC622);
    Entry cm = cm2.found && cm2.count == 9 && tiff_val(&t, tag(&t, ifd0, 0xC65B), 0) == 21 ? cm2 : cm1;
    if (!cm.found || cm.count != 9) return -1;
    for (int i = 0; i < 9; i++) info->cam_xyz[i] = (float)tiff_val(&t, cm, (uint32_t)i);
    info->exposure = (float)tiff_val(&t, tag(&t, ifd0, 0xC62A), 0);
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
            if (k >= total || code >= 1 << len) return -1;   // (more codes than fit in len bits: damaged)
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

// ---------------------------------------------------------------------------
// a long lossless JPEG scan decoded in parallel: Huffman codes resynchronise by themselves, so the
// scan is cut into parts decoded at once from a guessed start; where the code boundaries of a part
// meet the real ones of the part before it, the rest of it is right. (One Huffman table, no restarts.)

static inline uint64_t load_be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

// The difference coded at bit *pos of u (ulen bytes without stuffing, then zeros: reads beyond give
// zeros, as at the end of a scan). An invalid code skips one bit (only a wrong guess meets one).
static inline int udiff(const uint8_t *u, size_t ulen, uint64_t *pos, const Huff *h) {
    size_t q = (size_t)(*pos >> 3);
    uint64_t w = load_be64(u + (q < ulen ? q : ulen)) << (*pos & 7);   // (at least 57 bits)
    int32_t f = h->fast[w >> (64 - FAST_BITS)];
    if (f) {
        *pos += f & 255;
        return f >> 8;
    }
    int s, len;
    uint16_t g = h->code[w >> (64 - FAST_BITS)];
    if (g) {
        len = g >> 8;
        s = g & 255;
    } else {
        len = FAST_BITS + 1;
        int code = (int)(w >> (64 - len));
        while (len < 16 && (h->maxcode[len] < 0 || code > h->maxcode[len])) {
            len++;
            code = (int)(w >> (64 - len));
        }
        if (h->maxcode[len] < 0 || code > h->maxcode[len]) {
            *pos += 1;
            return 0;
        }
        s = h->vals[h->valptr[len] + code - h->mincode[len]];
    }
    if (s >= 16) {
        *pos += len;
        return 32768;
    }
    *pos += len + s;
    if (!s) return 0;
    int v = (int)((w << len) >> (64 - s));
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

// The entropy-coded data at d (up to its first marker) without the stuffed zero bytes, in a new
// buffer followed by 16 zero bytes; *ulen: its length. In parallel, by 1 MB pieces.
static uint8_t *unstuff(const uint8_t *d, size_t n, size_t *ulen) {
    const size_t PS = 1 << 20;
    size_t np = (n + PS - 1) / PS;
    size_t *drops = calloc(np + 1, sizeof(size_t)), *marker = malloc(np * sizeof(size_t));
    uint8_t *u = NULL;
    if (!drops || !marker) goto done;
    size_t *pd = drops, *pm = marker;
    // per piece: the FF bytes whose next byte lies in it: stuffing (FF 00) or the marker that ends the data
    nitro_parallel(np, ^(size_t k) {
        size_t a = k * PS, b = MIN(n, a + PS), i = a ? a - 1 : 0, lim = b == n ? n : b - 1, cnt = 0, mk = SIZE_MAX;
        while (i < lim) {
            const uint8_t *f = memchr(d + i, 0xFF, lim - i);
            if (!f) break;
            i = (size_t)(f - d);
            if (i + 1 < n && d[i + 1] == 0) {
                cnt++;
                i += 2;
            } else {
                mk = i;
                break;
            }
        }
        pd[k + 1] = cnt;
        pm[k] = mk;
    });
    size_t end = n;
    for (size_t k = 0; k < np; k++) {
        drops[k + 1] += drops[k];   // dropped bytes before piece k + 1
        if (marker[k] != SIZE_MAX) {
            end = marker[k];
            break;
        }
    }
    size_t kend = MIN(np, end / PS + 1);
    *ulen = end - drops[kend];
    u = malloc(*ulen + 16);
    if (!u) goto done;
    memset(u + *ulen, 0, 16);
    uint8_t *pu = u;
    nitro_parallel(kend, ^(size_t k) {
        size_t a = k * PS, lim = MIN(a + PS, end), i = a;
        if (a >= end) return;
        uint8_t *o = pu + a - pd[k];
        if (a > 0 && d[a - 1] == 0xFF && d[a] == 0) i++;   // (its FF ended the piece before)
        while (i < lim) {
            const uint8_t *f = memchr(d + i, 0xFF, lim - i);
            size_t j = f ? (size_t)(f - d) + 1 : lim;
            memcpy(o, d + i, j - i);
            o += j - i;
            i = j;
            if (f && i < lim && d[i] == 0) i++;
        }
    });
done:
    free(drops);
    free(marker);
    return u;
}

#define SYNC_N 256
typedef struct {
    uint64_t pos[SYNC_N];   // where its first (guessed) codes start
    uint64_t end;           // where the first code after it starts
    size_t count, npos;     // codes decoded in it, positions kept
    size_t sync, bridge;    // its first right code; codes from the end of the part before up to it
    uint64_t start;         // the real code run of the part: from here (the end of the part before) ...
    size_t off, n;          // ... n codes, the samples at off
    uint16_t *diff;         // the differences decoded in step 1
    size_t cap;
} Part;

// From p, the real start of part c, up to its first right code: *j its index, *br the codes up
// to it. Returns -1 if none of its first codes is met.
static int resync(const Part *c, uint64_t p, const uint8_t *u, size_t ulen, const Huff *hf, size_t *j, size_t *br) {
    size_t k = 0, b = 0;
    for (;;) {
        while (k < c->npos && c->pos[k] < p) k++;
        if (k == c->npos) return -1;
        if (c->pos[k] == p) break;
        udiff(u, ulen, &p, hf);
        b++;
    }
    *j = k;
    *br = b;
    return 0;
}

// Decodes the scan at d (n bytes) into out (w x nc samples per row, h rows; the predictor's
// initial value, point transform pt) in parallel. Returns -1 if it cannot (the caller decodes it
// serially).
static int scan_parallel(const uint8_t *d, size_t n, const Huff *hf, uint16_t *out, int w, int h, int nc, int pred,
                         int initial, int pt) {
    size_t total = (size_t)w * nc * h, L = (size_t)w * nc, ulen = 0;
    uint8_t *u = unstuff(d, n, &ulen);
    if (!u) return -1;
    uint64_t nbits = (uint64_t)ulen * 8;
    size_t K = MAX(1, MIN(512, ulen / (128 << 10)));
    Part *parts = calloc(K, sizeof(Part));
    if (!parts) {
        free(u);
        return -1;
    }
    Part *pp = parts;
    // 1. each part from a guessed start: its differences, where its first codes start
    _Atomic int failed = 0, *pfailed = &failed;
    nitro_parallel(K, ^(size_t k) {
        Part *c = &pp[k];
        uint64_t p = nbits * k / K, end = nbits * (k + 1) / K;
        size_t cnt = 0, cap = (size_t)(end - p) / 8 + 64;   // (grows when needed)
        uint16_t *df = malloc(cap * 2);
        while (df && p < end) {
            if (cnt < SYNC_N) c->pos[cnt] = p;
            if (cnt == cap) {
                uint16_t *more = realloc(df, cap * 4);
                if (!more) {
                    free(df);
                    df = NULL;
                    break;
                }
                df = more;
                cap *= 2;
            }
            df[cnt++] = (uint16_t)udiff(u, ulen, &p, hf);
        }
        if (!df) atomic_store(pfailed, 1);
        c->diff = df;
        c->npos = MIN(cnt, SYNC_N);
        c->count = cnt;
        c->end = p;
    });
    if (atomic_load(&failed)) {
        for (size_t k = 0; k < K; k++) free(parts[k].diff);
        free(parts);
        free(u);
        return -1;
    }
    // 2. from the end of the part before (right, once that part met the real codes) up to one of
    // its first codes
    if (K > 1) nitro_parallel(K - 1, ^(size_t i) {
        Part *c = &pp[i + 1];
        if (resync(c, pp[i].end, u, ulen, hf, &c->sync, &c->bridge)) c->sync = SIZE_MAX;
    });
    // the real code runs in order; a part that did not meet the real codes (rare: long runs of the
    // same short codes) is decoded once more from its real start, the next one then resynchronised
    uint64_t start = 0, guessed_end = 0;   // (step 2 started part k at the guessed end of part k - 1)
    size_t off = 0;
    for (size_t k = 0; k < K; k++) {
        Part *c = &parts[k];
        uint64_t end = c->end;
        if (k == 0) {
            c->n = c->count;
        } else if (start != guessed_end && resync(c, start, u, ulen, hf, &c->sync, &c->bridge)) {
            c->sync = SIZE_MAX;
        }
        guessed_end = c->end;
        if (k > 0 && c->sync != SIZE_MAX) {
            c->n = c->bridge + c->count - c->sync;
        } else if (k > 0) {
            uint64_t p = start, lim = nbits * (k + 1) / K;
            size_t cnt = 0;
            while (p < lim) {
                udiff(u, ulen, &p, hf);
                cnt++;
            }
            c->n = cnt;
            end = p;
        }
        c->start = start;
        c->off = off;
        off += c->n;
        start = end;
    }
    if (off < total) parts[K - 1].n += total - off;   // (a short scan: the zeros after it, as when serial)
    // 3. the differences in place: the codes up to the first right one decoded again, the rest
    // copied (a part decoded again: all of it)
    nitro_parallel(K, ^(size_t k) {
        Part *c = &pp[k];
        if (c->off >= total) return;
        size_t m = MIN(c->n, total - c->off), i = 0;
        uint64_t p = c->start;
        uint16_t *o = out + c->off;
        if (k == 0 || c->sync != SIZE_MAX) {
            size_t br = k ? c->bridge : 0;
            for (; i < MIN(br, m); i++) o[i] = (uint16_t)udiff(u, ulen, &p, hf);
            size_t cp = MIN(c->count - (k ? c->sync : 0), m - i);
            memcpy(o + i, c->diff + (k ? c->sync : 0), cp * 2);
            i += cp;
            p = c->end;
        }
        for (; i < m; i++) o[i] = (uint16_t)udiff(u, ulen, &p, hf);   // (a short scan: the zeros after it)
    });
    for (size_t k = 0; k < K; k++) free(parts[k].diff);
    free(parts);
    free(u);
    // 4. the samples: predictor 1 (left neighbour) row by row in parallel once the first column is
    // known; the others need the row above: a wavefront (a block of a row once the row above has
    // finished it)
    int wave = pred != 1 && h > 1;
    _Atomic int *progress = wave ? calloc((size_t)h, sizeof(_Atomic int)) : NULL;
    if (wave && !progress) return -1;   // (the caller decodes it serially)
    if (!wave) {
        for (int y = 0; y < h; y++)
            for (int c = 0; c < nc; c++) out[y * L + c] = (uint16_t)(out[y * L + c] + (y ? out[(y - 1) * L + c] : initial));
        size_t groups = ((size_t)h + 15) / 16;
        nitro_parallel(groups, ^(size_t g) {
            for (size_t y = g * 16; y < MIN((size_t)h, g * 16 + 16); y++) {
                uint16_t *r = out + y * L;
                for (size_t i = (size_t)nc; i < L; i++) r[i] = (uint16_t)(r[i] + r[i - nc]);
            }
        });
    } else {
        for (int c = 0; c < nc; c++) out[c] = (uint16_t)(out[c] + initial);   // the first row: predictor 1
        for (size_t i = (size_t)nc; i < L; i++) out[i] = (uint16_t)(out[i] + out[i - nc]);
        const size_t BS = 512;
        int nb = (int)((L + BS - 1) / BS);
        progress[0] = nb;
        _Atomic int next = 1, *pnext = &next, *pprog = progress;
        nitro_parallel((size_t)MIN(nitro_ncpu(), h - 1), ^(size_t worker) {
            (void)worker;
            for (;;) {
                int y = atomic_fetch_add(pnext, 1);
                if (y >= h) break;
                uint16_t *cur = out + y * L;
                const uint16_t *prev = cur - L;
                for (int b = 0; b < nb; b++) {
                    while (atomic_load_explicit(&pprog[y - 1], memory_order_acquire) <= b) nitro_yield();
                    size_t i0 = b ? b * BS : (size_t)nc, i1 = MIN(L, (size_t)(b + 1) * BS);
                    if (!b)
                        for (int c = 0; c < nc; c++) cur[c] = (uint16_t)(cur[c] + prev[c]);
                    #define ROW(PX) for (size_t i = i0; i < i1; i++) { \
                            int ra = cur[i - nc], rb = prev[i], rcc = prev[i - nc]; \
                            (void)ra; (void)rb; (void)rcc; \
                            cur[i] = (uint16_t)((PX) + cur[i]); \
                        }
                    switch (pred) {
                    case 2: ROW(rb) break;
                    case 3: ROW(rcc) break;
                    case 4: ROW(ra + rb - rcc) break;
                    case 5: ROW(ra + ((rb - rcc) >> 1)) break;
                    case 6: ROW(rb + ((ra - rcc) >> 1)) break;
                    default: ROW((ra + rb) >> 1) break;
                    }
                    #undef ROW
                    atomic_store_explicit(&pprog[y], b + 1, memory_order_release);
                }
            }
        });
        free((void *)progress);
    }
    if (pt)
        for (size_t i = 0; i < total; i++) out[i] = (uint16_t)(out[i] << pt);
    return 0;
}

// Decodes one lossless JPEG (a tile or strip) into out (tw x th samples, row stride ostride).
// par: a long scan is decoded in parallel.
static int lossless_jpeg(const uint8_t *d, size_t n, uint16_t *out, int tw, int th, size_t ostride, int par) {
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
            int initial = 1 << (prec - pt - 1);
            int one_table = 1;
            for (int c = 1; c < nc; c++) one_table &= tbl[c] == tbl[0];
            if (par && !ri && one_table && ostride == (size_t)tw && n - (p + 2 + l) >= (1 << 20) &&
                !scan_parallel(d + p + 2 + l, n - (p + 2 + l), &huff[tbl[0]], out, w, h, nc, pred, initial, pt)) {
                rc = 0;
                goto out;
            }
            size_t rowlen = (size_t)w * nc;
            uint16_t *prev = calloc(rowlen, 2), *cur = calloc(rowlen, 2);
            if (!prev || !cur) {
                free(prev);
                free(cur);
                goto out;
            }
            BitIn b = {d + p + 2 + l, d + n, 0, 0};
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

static void run(size_t n, int nthreads, nitro_body body) {   // nthreads 1: on the calling thread
    if (nthreads == 1)
        for (size_t i = 0; i < n; i++) body(i);
    else
        nitro_parallel(n, body);
}

// ---------------------------------------------------------------------------
// development, row by row: the samples of five rows around the current one normalised (black /
// white levels, white balance) and split into even and odd columns, so that the loops over a
// row run without branches (the compiler vectorizes them)

typedef struct {
    const uint16_t *raw;          // the raw image, its row length
    size_t rw;
    int ax, ay, aw, ah;           // the active area
    const uint16_t *lin;          // linearization table or NULL
    int lin_n;
    const float *bh, *bv;         // black level deltas per column / row of the active area
    float black[4], scale[4];     // per 2x2 position: (v - black) * scale
} Dev;

static float edge_sample(const Dev *g, const uint16_t *src, int xx, float bv, int p) {   // mirrored inside the area
    if (xx < 0) xx = -xx;
    else if (xx >= g->aw) xx = 2 * g->aw - 2 - xx;
    int k = p + (xx & 1);
    unsigned v = src[xx];
    if (g->lin) v = g->lin[MIN(v, (unsigned)g->lin_n - 1)];
    float f = ((float)v - g->black[k] - g->bh[xx] - bv) * g->scale[k];
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

// Row yy of the active area (mirrored at its edges), columns x0, x0 + 1 ... normalised: x0 + 2i into
// e[i], x0 + 2i + 1 into o[i] (i < n). (Mirroring keeps the colour of a sample: an even distance.)
static void norm_row(const Dev *g, int yy, int x0, int n, float *e, float *o) {
    if (yy < 0) yy = -yy;
    else if (yy >= g->ah) yy = 2 * g->ah - 2 - yy;
    const uint16_t *src = g->raw + (size_t)(g->ay + yy) * g->rw + g->ax;
    const float *bh = g->bh;
    float bv = g->bv[yy];
    int p = (yy & 1) * 2;
    int lo = x0 >= 0 ? 0 : 1, hi = MAX(lo, MIN(n, (g->aw - 2 - x0) / 2 + 1));   // [lo, hi): inside the area
    for (int i = 0; i < n; i++) {   // the edges
        if (i == lo) i = hi;
        if (i >= n) break;
        e[i] = edge_sample(g, src, x0 + 2 * i, bv, p);
        o[i] = edge_sample(g, src, x0 + 2 * i + 1, bv, p);
    }
    int ke = p + (x0 & 1), ko = p + ((x0 + 1) & 1);
    float be = g->black[ke], bo = g->black[ko], se = g->scale[ke], so = g->scale[ko];
    const uint16_t *lin = g->lin;
    #define NORM(VE, VO) \
        for (int i = lo; i < hi; i++) { \
            int xe = x0 + 2 * i; \
            float fe = ((float)(VE) - be - bh[xe] - bv) * se, fo = ((float)(VO) - bo - bh[xe + 1] - bv) * so; \
            e[i] = fe < 0 ? 0 : fe > 1 ? 1 : fe; \
            o[i] = fo < 0 ? 0 : fo > 1 ? 1 : fo; \
        }
    if (!lin) {
        NORM(src[xe], src[xe + 1])
    } else {
        unsigned last = (unsigned)g->lin_n - 1;
        NORM(lin[MIN((unsigned)src[xe], last)], lin[MIN((unsigned)src[xe + 1], last)])
    }
    #undef NORM
}

// Malvar-He-Cutler at one pixel: x is the colour beside it in its row, y the one above / below
// (at green), or x its own colour and y the other one (at red / blue).
typedef struct {
    float x, g, y;
} Px;

static inline Px px_green(float v, float L, float R, float L2, float R2, float U, float D, float U2, float D2, float diag) {
    Px p = {(5 * v + 4 * (L + R) - diag - (L2 + R2) + 0.5f * (U2 + D2)) / 8, v,
            (5 * v + 4 * (U + D) - diag - (U2 + D2) + 0.5f * (L2 + R2)) / 8};
    return p;
}

static inline Px px_other(float v, float L, float R, float L2, float R2, float U, float D, float U2, float D2, float diag) {
    float cross = L + R + U + D, outer = L2 + R2 + U2 + D2;
    Px p = {v, (4 * v + 2 * cross - outer) / 8, (6 * v + 2 * diag - 1.5f * outer) / 8};
    return p;
}

// One row of n pairs of pixels: demosaiced, converted (m: camera to linear sRGB) and quantised to
// indices of the output curve: pixel 2k into ie[k], ie[n + k], ie[2n + k] (B, G, R), pixel 2k + 1
// into io. e / o [0..4] are the five rows around it (even / odd columns); pixel 2k is e[2][k + 1],
// pixel 2k + 1 is o[2][k + 1]. green_even: the even pixels are green; red_row: red is in this row.
static void develop_row(float *const e[5], float *const o[5], int n, int green_even, int red_row, const float m[9],
                        uint16_t *restrict ie, uint16_t *restrict io) {
    const float *e0 = e[0], *e1 = e[1], *e2 = e[2], *e3 = e[3], *e4 = e[4];
    const float *o0 = o[0], *o1 = o[1], *o2 = o[2], *o3 = o[3], *o4 = o[4];
    float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7], m8 = m[8];
    #define EVEN(K) K(e2[k + 1], o2[k], o2[k + 1], e2[k], e2[k + 2], e1[k + 1], e3[k + 1], e0[k + 1], e4[k + 1], \
                      o1[k] + o1[k + 1] + o3[k] + o3[k + 1])
    #define ODD(K) K(o2[k + 1], e2[k + 1], e2[k + 2], o2[k], o2[k + 2], o1[k + 1], o3[k + 1], o0[k + 1], o4[k + 1], \
                     e1[k + 1] + e1[k + 2] + e3[k + 1] + e3[k + 2])
    #define Q(X) (!((X) > 0) ? 0 : (X) >= 2 ? 8192 : (int)((X) * 4096))   // (NaN: 0)
    #define STORE_PX(P, I) { \
        float r = red_row ? P.x : P.y, g = P.g, b = red_row ? P.y : P.x; \
        float R = m0 * r + m1 * g + m2 * b; \
        float G = m3 * r + m4 * g + m5 * b; \
        float B = m6 * r + m7 * g + m8 * b; \
        I[k] = (uint16_t)Q(B); \
        I[n + k] = (uint16_t)Q(G); \
        I[2 * n + k] = (uint16_t)Q(R); }
    if (green_even)
        for (int k = 0; k < n; k++) {
            Px pe = EVEN(px_green), po = ODD(px_other);
            STORE_PX(pe, ie) STORE_PX(po, io)
        }
    else
        for (int k = 0; k < n; k++) {
            Px pe = EVEN(px_other), po = ODD(px_green);
            STORE_PX(pe, ie) STORE_PX(po, io)
        }
    #undef EVEN
    #undef ODD
    #undef Q
    #undef STORE_PX
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
    // few long lossless JPEG scans (one strip ...): one after the other, each decoded in parallel
    int par = in->compression == 7 && in->ntiles < 4 && nthreads != 1;
    run((size_t)in->ntiles, par ? 1 : nthreads, ^(size_t i) {
        Entry eo = {pin->tile_off_type, (uint32_t)pin->ntiles, pin->tile_off_pos, 1};
        Entry ec = {pin->tile_cnt_type, (uint32_t)pin->ntiles, pin->tile_cnt_pos, 1};
        uint64_t off = (uint64_t)tiff_val(pt, eo, (uint32_t)i), cnt = (uint64_t)tiff_val(pt, ec, (uint32_t)i);
        if (off >= len || cnt > len - off) {
            atomic_store(pf, 1);
            return;
        }
        int tx = (int)(i % (size_t)across) * pin->tile_w, ty = (int)(i / (size_t)across) * pin->tile_h;
        int w = MIN(pin->tile_w, rw - tx), h = MIN(pin->tile_h, rh - ty);
        const uint8_t *src = data + off;
        if (pin->compression == 7 && pin->tile_w == rw && ty + pin->tile_h <= rh) {   // whole rows: in place
            if (lossless_jpeg(src, (size_t)cnt, praw + (size_t)ty * rw, rw, pin->tile_h, (size_t)rw, par)) atomic_store(pf, 1);
        } else if (pin->compression == 7) {
            // a whole tile is coded (also its part beyond the image): decode into a scratch buffer
            uint16_t *tile = malloc((size_t)pin->tile_w * pin->tile_h * 2);
            if (!tile || lossless_jpeg(src, (size_t)cnt, tile, pin->tile_w, pin->tile_h, (size_t)pin->tile_w, par)) {
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
    float exposure = powf(2.0f, MIN(MAX(in->exposure + in->ev, -8.0f), 8.0f));
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
    // 3. demosaic and convert, in bands of rows
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
        for (int i = 0; i < lin_n; i++) lintab[i] = (uint16_t)tiff_val(&t, (Entry){3, (uint32_t)in->lin_n, (uint32_t)(in->lin - data), 1}, (uint32_t)i);
        lin = lintab;
    }
    if (in->delta_h[1]) {
        Entry e = {in->delta_h[0], in->delta_h[1], in->delta_h[2], 1};
        for (int i = 0; i < aw && (uint32_t)i < e.count; i++) bh[i] = (float)tiff_val(&t, e, (uint32_t)i);
    }
    if (in->delta_v[1]) {
        Entry e = {in->delta_v[0], in->delta_v[1], in->delta_v[2], 1};
        for (int i = 0; i < ah && (uint32_t)i < e.count; i++) bv[i] = (float)tiff_val(&t, e, (uint32_t)i);
    }
    Dev dev = {raw, (size_t)rw, ax, ay, aw, ah, lin, lin_n, bh, bv, {0}, {0}};
    memcpy(dev.black, black, sizeof black);
    memcpy(dev.scale, scale, sizeof scale);
    const Dev *pdev = &dev;
    const float *pm = m;
    const uint8_t *plut = lut;
    const int *pcfa = in->cfa;
    int npairs = (ow + 1) / 2, n = npairs + 2;   // n: samples per row and plane (window: +-2 columns)
    size_t bands = (size_t)(oh + 31) / 32;
    run(bands, nthreads, ^(size_t band) {
        int y0 = (int)band * 32, y1 = MIN(y0 + 32, oh);
        float *mem = malloc(sizeof(float) * (size_t)n * 10 + sizeof(uint16_t) * (size_t)npairs * 6);
        if (!mem) return;
        float *ring[5][2];   // window rows (by row index mod 5): even / odd columns
        for (int k = 0; k < 5; k++) {
            ring[k][0] = mem + (size_t)n * (2 * k);
            ring[k][1] = mem + (size_t)n * (2 * k + 1);
        }
        uint16_t *ie = (uint16_t *)(mem + (size_t)n * 10), *io = ie + 3 * npairs;
        for (int r = 0; r < 4; r++) norm_row(pdev, cy + y0 + r - 2, cx - 2, n, ring[r % 5][0], ring[r % 5][1]);
        for (int y = y0; y < y1; y++) {
            int r = y - y0 + 4;   // the window's newest row
            norm_row(pdev, cy + y + 2, cx - 2, n, ring[r % 5][0], ring[r % 5][1]);
            float *e[5], *o[5];
            for (int k = 0; k < 5; k++) {
                e[k] = ring[(r - 4 + k) % 5][0];
                o[k] = ring[(r - 4 + k) % 5][1];
            }
            int sy = cy + y, ce = pcfa[(sy & 1) * 2 + (cx & 1)], co = pcfa[(sy & 1) * 2 + ((cx + 1) & 1)];
            develop_row(e, o, npairs, ce == 1, ce == 0 || co == 0, pm, ie, io);
            uint32_t *out = (uint32_t *)(bgrx + (size_t)y * pitch);   // B G R X
            #define PACK(I, k) (plut[I[k]] | plut[I[npairs + (k)]] << 8 | plut[I[2 * npairs + (k)]] << 16 | 0xFF000000u)
            for (int k = 0; k < ow / 2; k++) {
                out[2 * k] = PACK(ie, k);
                out[2 * k + 1] = PACK(io, k);
            }
            if (ow & 1) out[ow - 1] = PACK(ie, ow / 2);
            #undef PACK
        }
        free(mem);
    });
    free(bh);
    free(raw);
    return 0;
}
