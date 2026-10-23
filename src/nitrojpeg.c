// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrojpeg: multi-threaded single-image baseline JPEG decoding.
// See nitrojpeg.h for the idea. Self-contained; the parts that need
// libjpeg-turbo (reference engine, BGRX output) are under NJ_REFERENCE.
//
// The inverse DCT (idct_islow_*) implements the "ISLOW" algorithm of the
// Independent JPEG Group's jidctint.c (same constants and rounding), so the
// output is bit-identical to libjpeg / libjpeg-turbo. This software is based
// in part on the work of the Independent JPEG Group.
#include "nitrojpeg.h"

#include "nitro_os.h"   // parallel loop, clock, CPU count
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef NJ_REFERENCE
// Reference / comparison code only (old band engine, BGRX output): needs libjpeg-turbo.
#include <jpeglib.h>
#include <setjmp.h>
#endif

#ifndef LOOK
#define LOOK 11                 // Huffman lookahead bits (-DLOOK=8..16, see README)
#endif
#if LOOK < 8 || LOOK > 16
#error "LOOK must be 8..16"
#endif
#define MAX_BLOCKS 10           // blocks per MCU (JPEG limit)
#define PAD_BYTES 8192          // zero padding after the unstuffed stream

// ---------------------------------------------------------------------------
// utilities

static double now_ms(void) { return nitro_now_ms(); }

static int ncpu(void) { return nitro_ncpu(); }

static inline unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

#ifdef NJ_REFERENCE
// 64 bits of the stream starting at bit 'pos' (MSB first). The buffer must be
// padded by at least 8 bytes.
static inline uint64_t peek64(const uint8_t *buf, uint64_t pos) {
    uint64_t v;
    memcpy(&v, buf + (pos >> 3), 8);
    return __builtin_bswap64(v) << (pos & 7);
}
#endif

// Runs body(0..n-1) on at most 'workers' threads (0 = all CPUs). Work items
// are handed out dynamically, so uneven items still balance well.
static int g_max_workers = 0;
void nj_set_max_workers(int n) { g_max_workers = n; }
static int g_bands = 0;   // 0: 2 x number of pieces
void nj_set_bands(int n) { g_bands = n; }
#ifdef NJ_REFERENCE
static int g_sequential = 0;   // experiment: one sequential Huffman pass instead of speculation
void nj_set_sequential_scan(int on) { g_sequential = on; }
#endif

static __thread int t_workers = 0;   // per-call override (nj_decode_own with nthreads == 1)

static void parallel_for(size_t n, void (^body)(size_t)) {
    int w = t_workers > 0 ? t_workers : g_max_workers > 0 ? g_max_workers : ncpu();
    if ((size_t)w >= n) {
        nitro_parallel(n, body);
        return;
    }
    volatile long next = 0, *const pnext = &next;
    // body via a pointer: a block capturing a block would need the blocks runtime (see nitro_os.h)
    struct { void (^fn)(size_t); } b = {body}, *const pb = &b;
    nitro_parallel((size_t)w, ^(size_t t) {
        for (;;) {
            long i = __sync_fetch_and_add(pnext, 1);
            if (i >= (long)n) break;
            pb->fn((size_t)i);
        }
    });
}

// ---------------------------------------------------------------------------
// Huffman tables

typedef struct {
    int present;
    uint16_t look[1 << LOOK];   // (codelen << 8) | symbol, 0 = not in table
    uint16_t aclook[1 << LOOK]; // AC skip: (codelen + extra bits) << 8 | k increment
    int32_t acfast[1 << LOOK];  // AC full decode when code + value fit in LOOK bits:
                                // value << 16 | kinc << 8 | total bits; kinc 0 = EOB, 0x40 = ZRL
    int32_t maxcode[18];
    int32_t valoff[18];
    uint8_t huffval[256];
    uint16_t ehufco[256];       // encoder tables (DC re-encoding)
    uint8_t ehufsi[256];
} HuffTable;

static inline int extend(unsigned v, int s) {
    return v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v;
}

static int build_huff(HuffTable *t, const uint8_t bits[17], const uint8_t *vals, int nvals) {
    memset(t, 0, sizeof *t);
    uint16_t huffcode[256];
    uint8_t huffsize[256];
    int k = 0;
    unsigned code = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++) {
            if (k >= 256) return -1;
            huffcode[k] = (uint16_t)code;
            huffsize[k] = (uint8_t)l;
            k++;
            code++;
        }
        if (code > (1u << l)) return -1;
        code <<= 1;
    }
    if (k != nvals) return -1;
    memcpy(t->huffval, vals, nvals);
    int p = 0;
    for (int l = 1; l <= 16; l++) {
        if (bits[l]) {
            t->valoff[l] = p - huffcode[p];
            p += bits[l];
            t->maxcode[l] = huffcode[p - 1];
        } else {
            t->maxcode[l] = -1;
        }
    }
    t->maxcode[17] = 0x7FFFFFFF;
    for (int i = 0; i < k; i++) {
        uint8_t sym = vals[i];
        t->ehufco[sym] = huffcode[i];
        t->ehufsi[sym] = huffsize[i];
        int l = huffsize[i];
        if (l > LOOK) continue;
        int r = sym >> 4, s = sym & 15;
        int kinc = s ? r + 1 : (r == 15 ? 16 : 64);
        int first = huffcode[i] << (LOOK - l), cnt = 1 << (LOOK - l);
        for (int j = 0; j < cnt; j++) {
            t->look[first + j] = (uint16_t)(l << 8 | sym);
            t->aclook[first + j] = (uint16_t)((l + s) << 8 | kinc);
            if (l + s <= LOOK) {
                int32_t e;
                if (s) {
                    unsigned v = (unsigned)((first + j) >> (LOOK - l - s)) & ((1u << s) - 1);
                    e = (int32_t)((uint32_t)(uint16_t)(int16_t)extend(v, s) << 16 | (uint32_t)(r + 1) << 8 | (uint32_t)(l + s));
                } else {
                    e = (r == 15 ? 0x40 : 0) << 8 | l;   // ZRL or EOB (r 1..14 treated as EOB, like libjpeg)
                }
                t->acfast[first + j] = e;
            }
        }
    }
    t->present = 1;
    return 0;
}

#ifdef NJ_REFERENCE
// Slow path for codes longer than LOOK bits. 'w' holds the next bits MSB-first.
static inline int huff_slow(const HuffTable *t, uint64_t w, int *sym) {
    unsigned c16 = (unsigned)(w >> 48);
    for (int l = LOOK + 1; l <= 16; l++) {
        int32_t c = (int32_t)(c16 >> (16 - l));
        if (c <= t->maxcode[l]) {
            *sym = t->huffval[(c + t->valoff[l]) & 0xFF];
            return l;
        }
    }
    *sym = 0;   // invalid code: happens only on not-yet-synchronised paths
    return 16;
}
#endif


// ---------------------------------------------------------------------------
// parsed file

typedef struct {
    nj_info info;
    const uint8_t *data;
    size_t len;
    HuffTable dc[4], ac[4];
    int nblocks;                  // blocks per MCU
    uint8_t bcomp[MAX_BLOCKS];    // frame component of each block in the MCU
    uint8_t bfirst[MAX_BLOCKS];   // block is the first of its component
    uint8_t bx[MAX_BLOCKS], by[MAX_BLOCKS];  // block position inside its component's MCU part
    uint16_t qt[4][64];           // quantisation tables, natural order
    uint8_t qpresent[4];
    uint8_t cq[3];                // quantisation table per frame component
    uint8_t cdc[3], cac[3];       // table selectors per frame component
    size_t scan_start;            // first entropy-coded byte
    // header template for band JPEGs: SOI DQT* DHT* [DRI] SOF SOS
    uint8_t hdr[4096];
    size_t hdr_len;
    size_t hdr_height_off;        // offset of the SOF height field in hdr
    int hdr_overflow;             // template did not fit: old engine can't split this file
} Jpeg;

// zigzag index -> natural (row-major) index; 16 extra entries guard
// against corrupt run lengths, like libjpeg's jpeg_natural_order.
static const uint8_t kNatural[80] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

static int parse_exif_orientation(const uint8_t *p, size_t n) {
    if (n < 14 || memcmp(p, "Exif\0\0", 6)) return 1;
    p += 6; n -= 6;
    int le = p[0] == 'I';
#define RD16(q) (le ? (unsigned)(q)[0] | (unsigned)(q)[1] << 8 : be16(q))
#define RD32(q) (le ? (uint32_t)RD16(q) | (uint32_t)RD16((q) + 2) << 16 : (uint32_t)RD16(q) << 16 | RD16((q) + 2))
    uint32_t ifd = RD32(p + 4);
    if (ifd + 2 > n) return 1;
    unsigned cnt = RD16(p + ifd);
    for (unsigned i = 0; i < cnt; i++) {
        size_t e = ifd + 2 + 12 * i;
        if (e + 12 > n) break;
        if (RD16(p + e) == 0x0112) {
            unsigned o = RD16(p + e + 8);
            return o >= 1 && o <= 8 ? (int)o : 1;
        }
    }
#undef RD16
#undef RD32
    return 1;
}

static int hdr_append(Jpeg *j, const uint8_t *p, size_t n) {   // never fails: only the old engine needs it
    if (j->hdr_len + n > sizeof j->hdr) { j->hdr_overflow = 1; return 0; }
    memcpy(j->hdr + j->hdr_len, p, n);
    j->hdr_len += n;
    return 0;
}

static int parse(const uint8_t *d, size_t len, Jpeg *j) {
    memset(j, 0, sizeof *j);
    j->data = d;
    j->len = len;
    nj_info *fi = &j->info;
    fi->orientation = 1;
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    j->hdr[0] = 0xFF; j->hdr[1] = 0xD8; j->hdr_len = 2;

    uint8_t sof[64]; size_t sof_len = 0;
    uint8_t comp_id[3] = {0};
    int have_sof = 0, baseline = 0, adobe = -1, jfif = 0;
    size_t pos = 2;
    for (;;) {
        while (pos < len && d[pos] != 0xFF) pos++;    // tolerate garbage
        while (pos < len && d[pos] == 0xFF) pos++;
        if (pos + 2 >= len) return -1;
        unsigned m = d[pos++];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) return -1;
        unsigned L = be16(d + pos);
        if (L < 2 || pos + L > len) return -1;
        const uint8_t *seg = d + pos + 2;
        size_t sl = L - 2;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            if (sl < 6) return -1;
            baseline = (m == 0xC0 || m == 0xC1) && seg[0] == 8;
            fi->height = (int)be16(seg + 1);
            fi->width = (int)be16(seg + 3);
            fi->ncomp = seg[5];
            if (fi->ncomp != 1 && fi->ncomp != 3) { fi->ncomp = fi->ncomp > 3 ? 3 : fi->ncomp; baseline = 0; }
            if (sl < 6 + 3 * (size_t)seg[5]) return -1;
            for (int c = 0; c < fi->ncomp; c++) {
                comp_id[c] = seg[6 + 3 * c];
                j->cq[c] = seg[8 + 3 * c] & 3;
                fi->h[c] = seg[7 + 3 * c] >> 4;
                fi->v[c] = seg[7 + 3 * c] & 15;
                if (fi->h[c] < 1 || fi->h[c] > 4 || fi->v[c] < 1 || fi->v[c] > 4) baseline = 0;
            }
            if (L + 2 > sizeof sof) return -1;
            memcpy(sof, d + pos - 2, L + 2);
            sof_len = L + 2;
            have_sof = 1;
            break;
        case 0xC4: {   // DHT
            size_t q = 0;
            while (q + 17 <= sl) {
                int tc = seg[q] >> 4, th = seg[q] & 15;
                uint8_t bits[17] = {0};
                int n = 0;
                for (int l = 1; l <= 16; l++) { bits[l] = seg[q + l]; n += bits[l]; }
                if (th > 3 || tc > 1 || q + 17 + n > sl) return -1;
                if (build_huff(tc ? &j->ac[th] : &j->dc[th], bits, seg + q + 17, n)) return -1;
                q += 17 + n;
            }
            if (hdr_append(j, d + pos - 2, L + 2)) return -1;
            break;
        }
        case 0xDB: {   // DQT
            size_t q = 0;
            while (q < sl) {
                int pq = seg[q] >> 4, tq = seg[q] & 15;
                size_t need = 1 + 64 * (pq ? 2 : 1);
                if (tq > 3 || q + need > sl) return -1;
                for (int i = 0; i < 64; i++)
                    j->qt[tq][kNatural[i]] = pq ? (uint16_t)be16(seg + q + 1 + 2 * i) : seg[q + 1 + i];
                j->qpresent[tq] = 1;
                q += need;
            }
            if (hdr_append(j, d + pos - 2, L + 2)) return -1;
            break;
        }
        case 0xDD:     // DRI
            if (sl < 2) return -1;
            fi->restart_interval = (int)be16(seg);
            break;
        case 0xE0:
            if (sl >= 5 && !memcmp(seg, "JFIF\0", 5)) jfif = 1;
            break;
        case 0xE1:
            if (fi->orientation == 1) fi->orientation = parse_exif_orientation(seg, sl);
            break;
        case 0xEE:
            if (sl >= 12 && !memcmp(seg, "Adobe", 5)) adobe = seg[11];
            break;
        case 0xDA: {   // SOS
            if (!have_sof) return -1;
            int ns = seg[0];
            if (sl < 1 + 2 * (size_t)ns + 3) return -1;
            j->scan_start = pos + L;
            // Is it one interleaved baseline scan with all components?
            int ok = baseline && ns == fi->ncomp;
            int ss = seg[1 + 2 * ns], se = seg[2 + 2 * ns], a = seg[3 + 2 * ns];
            if (ss != 0 || se != 63 || a != 0) ok = 0;
            // colour space: only YCbCr (or gray) goes to the planar path
            if (fi->ncomp == 3) {
                if (adobe == 0) ok = 0;
                if (!jfif && adobe < 0 && comp_id[0] == 'R' && comp_id[1] == 'G' && comp_id[2] == 'B') ok = 0;
            } else if (fi->ncomp == 1 && (fi->h[0] != 1 || fi->v[0] != 1)) {
                ok = 0;
            }
            int hmax = 1, vmax = 1;
            for (int c = 0; c < fi->ncomp; c++) {
                if (fi->h[c] > hmax) hmax = fi->h[c];
                if (fi->v[c] > vmax) vmax = fi->v[c];
            }
            if (fi->ncomp == 1) hmax = vmax = 1, fi->h[0] = fi->v[0] = 1;
            fi->hmax = hmax; fi->vmax = vmax;
            if (fi->width <= 0 || fi->height <= 0) return -1;
            fi->mcus_per_row = (fi->width + hmax * 8 - 1) / (hmax * 8);
            fi->mcu_rows = (fi->height + vmax * 8 - 1) / (vmax * 8);
            for (int c = 0; c < fi->ncomp; c++) {
                fi->plane_w[c] = fi->mcus_per_row * fi->h[c] * 8;
                fi->plane_h[c] = fi->mcu_rows * fi->v[c] * 8;
            }
            // MCU block layout (scan order) and table selectors
            int nb = 0;
            for (int i = 0; ok && i < ns; i++) {
                int cs = seg[1 + 2 * i], c;
                for (c = 0; c < fi->ncomp && comp_id[c] != cs; c++) {}
                if (c == fi->ncomp) { ok = 0; break; }
                j->cdc[c] = seg[2 + 2 * i] >> 4;
                j->cac[c] = seg[2 + 2 * i] & 15;
                if (j->cdc[c] > 3 || j->cac[c] > 3) { ok = 0; break; }
                int cnt = fi->h[c] * fi->v[c];
                for (int b = 0; b < cnt; b++) {
                    if (nb >= MAX_BLOCKS) { ok = 0; break; }
                    j->bcomp[nb] = (uint8_t)c;
                    j->bfirst[nb] = b == 0;
                    j->bx[nb] = (uint8_t)(b % fi->h[c]);
                    j->by[nb] = (uint8_t)(b / fi->h[c]);
                    nb++;
                }
            }
            j->nblocks = nb;
            for (int c = 0; ok && c < fi->ncomp; c++)
                if (!j->dc[j->cdc[c]].present || !j->ac[j->cac[c]].present || !j->qpresent[j->cq[c]]) ok = 0;
            // band header template: ... [DRI] SOF SOS
            if (fi->restart_interval) {
                uint8_t dri[6] = {0xFF, 0xDD, 0, 4, (uint8_t)(fi->restart_interval >> 8), (uint8_t)fi->restart_interval};
                if (hdr_append(j, dri, 6)) return -1;
            }
            j->hdr_height_off = j->hdr_len + 5;
            if (hdr_append(j, sof, sof_len)) return -1;
            if (hdr_append(j, d + pos - 2, L + 2)) return -1;
            fi->supported = ok;
            return 0;
        }
        default:
            break;
        }
        pos += L;
    }
}

typedef struct {
    const uint8_t *d;
    size_t len;
    int le;
} Tiff;

static uint32_t tiff16(const Tiff *t, size_t o) {
    if (o > t->len - 2) return 0;
    const uint8_t *p = t->d + o;
    return t->le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : (uint32_t)p[0] << 8 | p[1];
}

static uint32_t tiff32(const Tiff *t, size_t o) {
    if (o > t->len - 4) return 0;
    return t->le ? tiff16(t, o) | tiff16(t, o + 2) << 16 : tiff16(t, o) << 16 | tiff16(t, o + 2);
}

// The first value (SHORT or LONG) of a tag in a TIFF directory, 0 if the tag is missing; *count
// (if wanted) its number of values, *where the offset of its value(s).
static uint32_t tiff_tag(const Tiff *t, uint32_t ifd, unsigned want, uint32_t *count, uint32_t *where) {
    if (ifd < 8 || ifd > t->len - 2) return 0;
    uint32_t n = tiff16(t, ifd);
    if (n > (t->len - ifd - 2) / 12) return 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t e = ifd + 2 + 12 * (size_t)i;
        if (tiff16(t, e) != want) continue;
        uint32_t type = tiff16(t, e + 2), c = tiff32(t, e + 4);
        size_t vo = (type == 3 ? 2u : 4u) * (size_t)c <= 4 ? e + 8 : tiff32(t, e + 8);   // (inline or an offset)
        if (count) *count = c;
        if (where) *where = (uint32_t)vo;
        return type == 3 ? tiff16(t, vo) : tiff32(t, vo);
    }
    return 0;
}

// The JPEG (JPEGInterchangeFormat / Length) of a directory: the bigger one wins
static void tiff_jpeg(const Tiff *t, uint32_t ifd, uint32_t *best_off, uint32_t *best_len) {
    uint32_t off = tiff_tag(t, ifd, 0x201, NULL, NULL), cnt = tiff_tag(t, ifd, 0x202, NULL, NULL);
    if (!off || cnt < 4 || cnt <= *best_len) return;
    if (off < t->len - 2 && !(t->d[off] == 0xFF && t->d[off + 1] == 0xD8)) return;   // (beyond a header: unchecked)
    *best_off = off;
    *best_len = cnt;
}

int nj_find_embedded(const uint8_t *data, size_t len, size_t *offset, size_t *size, int *orientation) {
    if (len < 16 || !(data[0] == data[1] && (data[0] == 'I' || data[0] == 'M'))) return -1;
    Tiff t = {data, len, data[0] == 'I'};
    if (tiff16(&t, 2) != 42) return -1;
    uint32_t ifd0 = tiff32(&t, 4);
    uint32_t off = 0, cnt = 0;
    if (data[8] == 'C' && data[9] == 'R') {
        // Canon CR2: the JPEG is IFD0's strip (StripOffsets / StripByteCounts)
        off = tiff_tag(&t, ifd0, 0x111, NULL, NULL);
        cnt = tiff_tag(&t, ifd0, 0x117, NULL, NULL);
        if (!off || cnt < 4 || off > len - 2 || data[off] != 0xFF || data[off + 1] != 0xD8) return -1;
    } else {
        // other raw files (Nikon NEF ...): IFD0 is a reduced image (NewSubfileType 1), the camera's
        // JPEGs are in it, the next directories or the SubIFDs; the biggest is the full-size one
        if (tiff_tag(&t, ifd0, 0xFE, NULL, NULL) != 1) return -1;
        uint32_t ifd = ifd0;
        for (int k = 0; k < 4 && ifd; k++) {
            tiff_jpeg(&t, ifd, &off, &cnt);
            uint32_t n = 0, where = 0;
            if (tiff_tag(&t, ifd, 0x14A, &n, &where))   // SubIFDs
                for (uint32_t i = 0; i < n && i < 8; i++) tiff_jpeg(&t, tiff32(&t, where + 4 * (size_t)i), &off, &cnt);
            uint32_t entries = tiff16(&t, ifd);
            ifd = ifd < len - 2 ? tiff32(&t, ifd + 2 + 12 * (size_t)entries) : 0;   // the next directory
        }
        if (!off || cnt < 4) return -1;   // (off may lie beyond a header-only len)
    }
    // (it is sRGB even when a Canon is set to Adobe RGB: compared with the raw data, the
    // previews of such files look like those of sRGB ones only when shown as sRGB)
    uint32_t orient = tiff_tag(&t, ifd0, 0x112, NULL, NULL);
    *offset = off;
    *size = cnt;
    *orientation = orient >= 1 && orient <= 8 ? (int)orient : 1;
    return 0;
}

int nj_read_info(const uint8_t *data, size_t len, nj_info *info) {
    Jpeg *j = malloc(sizeof *j);
    int rc = parse(data, len, j);
    *info = j->info;
    free(j);
    return rc;
}

// ---------------------------------------------------------------------------
// entropy segment scan: find its end, restart markers and byte stuffing

typedef struct {
    size_t a, b;          // chunk [a, b) of raw bytes
    size_t removed;       // bytes in [a, b) dropped by unstuffing: stuffed 0x00s, RST markers
    size_t end;           // first non-RST marker found (SIZE_MAX if none)
    size_t *rst;          // raw positions of RST markers (0xFF byte) owned by this chunk
    size_t nrst, caprst;
    size_t *rstc;         // for the own engine: clean offsets where restart intervals start
    size_t nrstc;
} ScanChunk;

// Bytes are counted by position: a stuffed 0x00 or the 2nd byte of an RST
// marker belongs to the chunk it lies in, the 0xFF of an RST likewise.
static void scan_chunk(const uint8_t *d, size_t len, size_t lo, ScanChunk *c) {
    size_t i = c->a > lo ? c->a - 1 : lo;
    size_t stop = c->b < len - 1 ? c->b : len - 1;   // 0xFF positions [a-1, b) that have a next byte
    c->end = SIZE_MAX;
    while (i < stop) {
        const uint8_t *f = memchr(d + i, 0xFF, stop - i);
        if (!f) break;
        i = (size_t)(f - d);
        uint8_t nb = d[i + 1];
        int partner_here = i + 1 >= c->a && i + 1 < c->b;
        if (nb == 0x00) {
            c->removed += partner_here;
            i += 2;
        } else if (nb >= 0xD0 && nb <= 0xD7) {
            if (i >= c->a) {
                if (c->nrst == c->caprst) {
                    c->caprst = c->caprst ? c->caprst * 2 : 256;
                    c->rst = realloc(c->rst, c->caprst * sizeof *c->rst);
                }
                c->rst[c->nrst++] = i;
                c->removed++;
            }
            c->removed += partner_here;
            i += 2;
        } else if (nb == 0xFF) {
            i += 1;         // fill byte
        } else {
            c->end = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// speculative MCU decoding on the unstuffed stream

typedef struct {
    uint64_t pos;         // bit position of the MCU start
    int32_t dc[3];        // DC predictors before the MCU
    int32_t _pad;
} Rec;

// Bit reader over the unstuffed stream: 'buf' holds the next bits
// left-aligned, 'cnt' of them are valid. Branchless refill (F. Giesen's
// "variant 4"): bits past 'cnt' are always either zero or already correct.
typedef struct {
    const uint8_t *p;
    uint64_t buf;
    unsigned cnt;
} BR;

static inline void br_refill(BR *r) {
    uint64_t v;
    memcpy(&v, r->p, 8);
    r->buf |= __builtin_bswap64(v) >> r->cnt;
    r->p += (63 - r->cnt) >> 3;
    r->cnt |= 56;
}
static inline void br_init(BR *r, const uint8_t *base, uint64_t pos) {
    r->p = base + (pos >> 3);
    r->buf = 0;
    r->cnt = 0;
    br_refill(r);
    r->buf <<= pos & 7;
    r->cnt -= pos & 7;
}
static inline uint64_t br_pos(const BR *r, const uint8_t *base) {
    return (uint64_t)(r->p - base) * 8 - r->cnt;
}
static inline void br_skip(BR *r, unsigned n) { r->buf <<= n; r->cnt -= n; }

// Output target of a decode.
typedef struct {
    // output
    uint8_t *const *planes;   // raw mode
    const size_t *pitch;
    uint8_t *bgrx;            // bgrx mode
    size_t bgrx_pitch;
    int width, height;
} Output;

#ifdef NJ_REFERENCE
// ===========================================================================
// Reference engine (NJ_REFERENCE only): speculative *skip* pass finds the MCU
// rows, then every band is re-emitted as a small JPEG and decoded by
// libjpeg-turbo. Kept for comparison and for the BGRX output.

// Decode (skip) one MCU, updating the DC predictors.
static inline void skip_mcu_br(const Jpeg *j, BR *br, int32_t *dc) {
    for (int b = 0; b < j->nblocks; b++) {
        int c = j->bcomp[b];
        const HuffTable *dt = &j->dc[j->cdc[c]], *at = &j->ac[j->cac[c]];
        br_refill(br);
        uint64_t w = br->buf;
        unsigned e = dt->look[w >> (64 - LOOK)];
        int l, s;
        if (__builtin_expect(e != 0, 1)) { l = e >> 8; s = e & 15; } else { l = huff_slow(dt, w, &s); s &= 15; }
        if (s) dc[c] += extend((unsigned)((w << l) >> (64 - s)), s);
        br_skip(br, l + s);
        int k = 1;
        while (k < 64) {
            br_refill(br);
            w = br->buf;
            e = at->aclook[w >> (64 - LOOK)];
            if (__builtin_expect(e != 0, 1)) {
                br_skip(br, e >> 8);
                k += e & 0xFF;
            } else {
                int sym;
                l = huff_slow(at, w, &sym);
                int r = sym >> 4;
                s = sym & 15;
                br_skip(br, l + s);
                k += s ? r + 1 : (r == 15 ? 16 : 64);
            }
        }
    }
}

static inline void skip_mcu(const Jpeg *j, const uint8_t *buf, uint64_t *ppos, int32_t *dc) {
    BR br;
    br_init(&br, buf, *ppos);
    skip_mcu_br(j, &br, dc);
    *ppos = br_pos(&br, buf);
}

typedef struct {
    uint64_t start, end;  // bit range this thread is responsible for
    Rec *rec;             // MCU starts, rec[n] is the end state
    size_t n, cap;
} Spec;

static void spec_run(const Jpeg *j, const uint8_t *buf, Spec *s, size_t max_mcus) {
    BR br;
    br_init(&br, buf, s->start);
    uint64_t pos = s->start;
    int32_t dc[3] = {0, 0, 0};
    size_t n = 0;
    while (pos < s->end && n < max_mcus) {
        if (n + 1 >= s->cap) {
            s->cap = s->cap * 2 + 1024;
            s->rec = realloc(s->rec, s->cap * sizeof(Rec));
        }
        Rec *r = &s->rec[n++];
        r->pos = pos;
        memcpy(r->dc, dc, sizeof dc);
        skip_mcu_br(j, &br, dc);
        pos = br_pos(&br, buf);
    }
    if (n + 1 > s->cap) s->rec = realloc(s->rec, (s->cap = n + 1) * sizeof(Rec));
    s->rec[n].pos = pos;
    memcpy(s->rec[n].dc, dc, sizeof dc);
    s->n = n;
}

// ---------------------------------------------------------------------------
// bit writer with 0xFF byte stuffing

typedef struct {
    uint8_t *p;
    uint64_t acc;         // right-aligned pending bits
    int n;                // number of pending bits (< 32 between calls)
} BW;

static inline void bw_put(BW *w, uint32_t bits, int k) {   // k <= 32
    if (!k) return;
    w->acc = (w->acc << k) | (k == 32 ? bits : (bits & ((1u << k) - 1)));
    w->n += k;
    if (w->n >= 32) {
        uint32_t x = (uint32_t)(w->acc >> (w->n - 32));
        uint32_t nx = ~x;
        if (!((nx - 0x01010101u) & ~nx & 0x80808080u)) {  // no 0xFF byte
            uint32_t be = __builtin_bswap32(x);
            memcpy(w->p, &be, 4);
            w->p += 4;
        } else {
            for (int i = 3; i >= 0; i--) {
                uint8_t byte = (uint8_t)(x >> (8 * i));
                *w->p++ = byte;
                if (byte == 0xFF) *w->p++ = 0;
            }
        }
        w->n -= 32;
    }
}

static void bw_copy(BW *w, const uint8_t *buf, uint64_t a, uint64_t b) {
    while (b - a >= 32) {
        bw_put(w, (uint32_t)(peek64(buf, a) >> 32), 32);
        a += 32;
    }
    int rem = (int)(b - a);
    if (rem) bw_put(w, (uint32_t)(peek64(buf, a) >> (64 - rem)), rem);
}

static void bw_finish(BW *w) {
    if (w->n & 7) bw_put(w, 0xFF, 8 - (w->n & 7));   // pad with 1 bits
    while (w->n >= 8) {
        uint8_t byte = (uint8_t)(w->acc >> (w->n - 8));
        *w->p++ = byte;
        if (byte == 0xFF) *w->p++ = 0;
        w->n -= 8;
    }
}

// ---------------------------------------------------------------------------
// band decoding with libjpeg-turbo

typedef struct {
    struct jpeg_source_mgr pub;
    const uint8_t *seg[3];
    size_t len[3];
    int n, i;
} SegSrc;

static void src_init(j_decompress_ptr ci) { (void)ci; }
static boolean src_fill(j_decompress_ptr ci) {
    static const uint8_t eoi[2] = {0xFF, 0xD9};
    SegSrc *s = (SegSrc *)ci->src;
    while (s->i < s->n && !s->len[s->i]) s->i++;
    if (s->i < s->n) {
        s->pub.next_input_byte = s->seg[s->i];
        s->pub.bytes_in_buffer = s->len[s->i];
        s->i++;
    } else {
        s->pub.next_input_byte = eoi;
        s->pub.bytes_in_buffer = 2;
    }
    return TRUE;
}
static void src_skip(j_decompress_ptr ci, long num) {
    while (num > (long)ci->src->bytes_in_buffer) {
        num -= (long)ci->src->bytes_in_buffer;
        src_fill(ci);
    }
    ci->src->next_input_byte += num;
    ci->src->bytes_in_buffer -= num;
}
static void src_term(j_decompress_ptr ci) { (void)ci; }

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    int warnings;
} ErrMgr;

static void err_exit(j_common_ptr ci) { longjmp(((ErrMgr *)ci->err)->jb, 1); }
static void err_emit(j_common_ptr ci, int level) { if (level < 0) ((ErrMgr *)ci->err)->warnings++; }
static void err_out(j_common_ptr ci) { (void)ci; }


// Decode JPEG given as segments. The stream covers MCU rows [d0, d1) of the
// full image; rows [o0, o1) of those are written to the output.
static int decode_band(const Jpeg *j, const Output *o, const uint8_t *const seg[3], const size_t seglen[3],
                       int d0, int o0, int o1) {
    const nj_info *fi = &j->info;
    struct jpeg_decompress_struct ci;
    ErrMgr em;
    SegSrc src;
    uint8_t *scratch = NULL;
    ci.err = jpeg_std_error(&em.pub);
    em.pub.error_exit = err_exit;
    em.pub.emit_message = err_emit;
    em.pub.output_message = err_out;
    em.warnings = 0;
    if (setjmp(em.jb)) {
        jpeg_destroy_decompress(&ci);
        free(scratch);
        return -1;
    }
    jpeg_create_decompress(&ci);
    memset(&src, 0, sizeof src);
    src.pub.init_source = src_init;
    src.pub.fill_input_buffer = src_fill;
    src.pub.skip_input_data = src_skip;
    src.pub.resync_to_restart = jpeg_resync_to_restart;
    src.pub.term_source = src_term;
    for (int i = 0; i < 3; i++) { src.seg[i] = seg[i]; src.len[i] = seglen[i]; }
    src.n = 3;
    ci.src = &src.pub;
    jpeg_read_header(&ci, TRUE);
    ci.dct_method = JDCT_ISLOW;
    int mcu_h = fi->vmax * 8;
    if (o->planes) {
        ci.raw_data_out = TRUE;
        jpeg_start_decompress(&ci);
        JSAMPROW rows[3][32];
        JSAMPARRAY arr[3] = {rows[0], rows[1], rows[2]};
        size_t spitch = (size_t)fi->plane_w[0] + 64;
        for (int r = d0; r < o1; r++) {
            int out = r >= o0;
            if (!out && !scratch) scratch = malloc(spitch * 32);
            for (int c = 0; c < fi->ncomp; c++) {
                int vr = fi->v[c] * 8;
                for (int y = 0; y < vr; y++)
                    rows[c][y] = out ? o->planes[c] + ((size_t)r * vr + y) * o->pitch[c] : scratch + y * spitch;
            }
            if (jpeg_read_raw_data(&ci, arr, (JDIMENSION)mcu_h) != (JDIMENSION)mcu_h) em.warnings++;
        }
    } else {
        ci.out_color_space = JCS_EXT_BGRX;   // gray -> BGRX is supported too
        jpeg_start_decompress(&ci);
        int y0 = d0 * mcu_h;
        int ystart = o0 * mcu_h, yend = o1 * mcu_h;
        if (yend > fi->height) yend = fi->height;
        int bh = (int)ci.output_height;
        if (y0 + bh < yend) yend = y0 + bh;
        while ((int)ci.output_scanline + y0 < yend) {
            int y = (int)ci.output_scanline + y0;
            JSAMPROW row;
            if (y < ystart) {
                if (!scratch) scratch = malloc((size_t)fi->width * 4 + 64);
                row = scratch;
            } else {
                row = o->bgrx + (size_t)y * o->bgrx_pitch;
            }
            jpeg_read_scanlines(&ci, &row, 1);
        }
    }
    int warn = em.warnings;
    jpeg_abort_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(scratch);
    return warn ? -1 : 0;
}

// Band JPEG header: template with the SOF height patched.
static void band_header(const Jpeg *j, uint8_t *h, int rows_from, int rows_to) {
    const nj_info *fi = &j->info;
    int mcu_h = fi->vmax * 8;
    int y1 = rows_to * mcu_h;
    if (y1 > fi->height) y1 = fi->height;
    int bh = y1 - rows_from * mcu_h;
    memcpy(h, j->hdr, j->hdr_len);
    h[j->hdr_height_off] = (uint8_t)(bh >> 8);
    h[j->hdr_height_off + 1] = (uint8_t)bh;
}

// ---------------------------------------------------------------------------
// driver

typedef struct {
    uint64_t pos;
    int32_t dc[3];
} RowStart;

static int decode_single(const Jpeg *j, const Output *o) {
    const uint8_t *seg[3] = {j->data, NULL, NULL};
    size_t len[3] = {j->len, 0, 0};
    return decode_band(j, o, seg, len, 0, 0, j->info.mcu_rows);
}

static int nj_decode(const uint8_t *data, size_t len, const Output *o, int nthreads, nj_stats *st) {
    nj_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    Jpeg *j = malloc(sizeof *j);
    if (parse(data, len, j) || !j->info.supported || j->hdr_overflow) { free(j); return -1; }
    const nj_info *fi = &j->info;
    if (nthreads <= 0) nthreads = 4 * ncpu();   // many more pieces than cores: dynamic load balance (see README)
    const int R = fi->mcu_rows, mpr = fi->mcus_per_row;
    const size_t total_mcus = (size_t)R * mpr;
    int rc = -1;
    double t0 = now_ms();

    if (nthreads == 1 || R < 4) {
        rc = decode_single(j, o);
        st->decode_ms = now_ms() - t0;
        st->bands = 1;
        free(j);
        return rc;
    }

    // --- 1. scan entropy segment (parallel) ---
    const size_t lo = j->scan_start;
    const int T = nthreads;
    ScanChunk *sc = calloc(T, sizeof *sc);
    for (int t = 0; t < T; t++) {
        sc[t].a = lo + (len - lo) * t / T;
        sc[t].b = lo + (len - lo) * (t + 1) / T;
    }
    parallel_for(T, ^(size_t t) { scan_chunk(data, len, lo, &sc[t]); });
    size_t scan_end = len;
    int tend = T - 1;
    for (int t = 0; t < T; t++)
        if (sc[t].end != SIZE_MAX) { scan_end = sc[t].end; tend = t; break; }

    uint8_t *clean = NULL;
    RowStart *rows = NULL;
    Spec *sp = NULL;
    int nbands = 0;
    int *bstart = NULL;   // band b covers MCU rows [bstart[b], bstart[b+1])

    if (fi->restart_interval) {
        // --- restart marker path ---
        const int Ri = fi->restart_interval;
        size_t nrst = 0;
        for (int t = 0; t <= tend; t++) nrst += sc[t].nrst;
        size_t *rst = malloc((nrst + 1) * sizeof *rst);
        size_t k = 0;
        for (int t = 0; t <= tend; t++)
            for (size_t i = 0; i < sc[t].nrst; i++)
                if (sc[t].rst[i] < scan_end) rst[k++] = sc[t].rst[i];
        nrst = k;
        size_t intervals = (total_mcus + Ri - 1) / Ri;
        st->scan_ms = now_ms() - t0;
        if (nrst + 1 != intervals) {   // damaged or unusual: let libjpeg deal with it
            free(rst);
            goto failed;
        }
        // candidate band starts: rows beginning a restart interval whose
        // index is a multiple of 8 (so the band's first marker is RST0)
        int target = g_bands > 0 ? g_bands : T * 2;
        bstart = malloc((R + 2) * sizeof *bstart);
        bstart[nbands++] = 0;
        for (int r = 1; r < R; r++) {
            size_t m = (size_t)r * mpr;
            if (m % Ri || (m / Ri) % 8) continue;
            if ((long)r * target >= (long)nbands * R) bstart[nbands++] = r;
        }
        bstart[nbands] = R;
        if (nbands < 2) { free(rst); goto single; }
        st->mode = 1;
        int fail = 0, *const pfail = &fail;
        double t1 = now_ms();
        parallel_for(nbands, ^(size_t b) {
            int r0 = bstart[b], r1 = bstart[b + 1];
            size_t k0 = (size_t)r0 * mpr / Ri, k1 = (size_t)r1 * mpr / Ri;
            size_t a = k0 ? rst[k0 - 1] + 2 : lo;
            size_t e = r1 == R ? scan_end : rst[k1 - 1];
            uint8_t hdr[4096];
            band_header(j, hdr, r0, r1);
            const uint8_t *seg[3] = {hdr, data + a, NULL};
            size_t sl[3] = {j->hdr_len, e - a, 0};
            if (decode_band(j, o, seg, sl, r0, r0, r1)) __atomic_store_n(pfail, 1, __ATOMIC_RELAXED);
        });
        st->decode_ms = now_ms() - t1;
        st->bands = nbands;
        free(rst);
        rc = fail ? -1 : 0;
        if (fail) goto failed;
        goto done;
    }

    {
        // --- speculative path: 2. unstuff into a clean bit stream (parallel) ---
        size_t *coff = malloc((T + 1) * sizeof *coff);
        size_t acc = 0;
        for (int t = 0; t < T; t++) {
            coff[t] = (sc[t].a < scan_end ? sc[t].a : scan_end) - lo - acc;
            if (t <= tend) acc += sc[t].removed;
        }
        size_t clean_len = (scan_end - lo) - acc;
        clean = malloc(clean_len + PAD_BYTES);
        memset(clean + clean_len, 0, PAD_BYTES);
        parallel_for(T, ^(size_t t) {
            size_t a = sc[t].a, b = sc[t].b < scan_end ? sc[t].b : scan_end;
            if (a >= b) return;
            uint8_t *out = clean + coff[t];
            size_t i = a;
            if (i > lo && data[i - 1] == 0xFF && data[i] == 0) i++;
            while (i < b) {
                const uint8_t *f = memchr(data + i, 0xFF, b - i);
                size_t n = f ? (size_t)(f - data) + 1 - i : b - i;
                memcpy(out, data + i, n);
                out += n;
                i += n;
                if (f && i < b && data[i] == 0) i++;
            }
        });
        free(coff);
        st->scan_ms = now_ms() - t0;

        double t1 = now_ms();
        const uint64_t Lbits = (uint64_t)clean_len * 8;
        if (g_sequential) {
            // --- 3'. one sequential pass from the start: just record MCU row starts ---
            rows = malloc(R * sizeof *rows);
            for (int r = 0; r < R; r++) rows[r].pos = UINT64_MAX;
            BR br;
            br_init(&br, clean, 0);
            int32_t dc[3] = {0, 0, 0};
            for (int r = 0; r < R; r++) {
                uint64_t pos = br_pos(&br, clean);
                if (pos >= Lbits) break;                 // damaged / truncated
                rows[r].pos = pos;
                memcpy(rows[r].dc, dc, sizeof dc);
                for (int m = 0; m < mpr; m++) skip_mcu_br(j, &br, dc);
            }
            goto validate;
        }
        // --- 3. speculative decode (parallel) ---
        sp = calloc(T, sizeof *sp);
        for (int t = 0; t < T; t++) {
            sp[t].start = (uint64_t)(clean_len * t / T) * 8;
            sp[t].end = t == T - 1 ? Lbits : (uint64_t)(clean_len * (t + 1) / T) * 8;
            sp[t].cap = total_mcus / T * 2 + 1024;
            sp[t].rec = malloc(sp[t].cap * sizeof(Rec));
        }
        parallel_for(T, ^(size_t t) { spec_run(j, clean, &sp[t], total_mcus + 1); });

        // --- 4. stitch: follow the true path, sync with each thread ---
        rows = malloc(R * sizeof *rows);
        for (int r = 0; r < R; r++) rows[r].pos = UINT64_MAX;
        for (size_t a = 0; a < sp[0].n && a < total_mcus; a += mpr) {
            rows[a / mpr].pos = sp[0].rec[a].pos;
            memcpy(rows[a / mpr].dc, sp[0].rec[a].dc, sizeof rows[0].dc);
        }
        uint64_t cpos = sp[0].rec[sp[0].n].pos;
        size_t cidx = sp[0].n;
        int32_t cdc[3];
        memcpy(cdc, sp[0].rec[sp[0].n].dc, sizeof cdc);
        for (int t = 1; t < T && cidx < total_mcus; t++) {
            Spec *s = &sp[t];
            size_t jx = 0;
            for (;;) {
                while (jx <= s->n && s->rec[jx].pos < cpos) jx++;
                if (jx <= s->n && s->rec[jx].pos == cpos) {        // synchronised
                    if (st->nsync < 256) {
                        st->sync_mcus[st->nsync] = (int32_t)jx;
                        st->sync_bits[st->nsync] = (uint32_t)(cpos - s->start);
                        st->nsync++;
                    }
                    long off = (long)cidx - (long)jx;
                    int32_t dco[3];
                    for (int c = 0; c < 3; c++) dco[c] = cdc[c] - s->rec[jx].dc[c];
                    size_t a = (cidx + mpr - 1) / mpr * mpr;
                    for (; a < (size_t)(off + (long)s->n) && a < total_mcus; a += mpr) {
                        const Rec *rr = &s->rec[a - off];
                        rows[a / mpr].pos = rr->pos;
                        for (int c = 0; c < 3; c++) rows[a / mpr].dc[c] = rr->dc[c] + dco[c];
                    }
                    cpos = s->rec[s->n].pos;
                    cidx = off + s->n;
                    for (int c = 0; c < 3; c++) cdc[c] = s->rec[s->n].dc[c] + dco[c];
                    break;
                }
                if (jx > s->n || cidx >= total_mcus || cpos >= Lbits) {   // thread t never synced
                    if (jx > s->n && st->nsync < 256) {
                        st->sync_mcus[st->nsync] = -1;
                        st->sync_bits[st->nsync] = 0;
                        st->nsync++;
                    }
                    break;
                }
                if (cidx % mpr == 0) {
                    rows[cidx / mpr].pos = cpos;
                    memcpy(rows[cidx / mpr].dc, cdc, sizeof cdc);
                }
                skip_mcu(j, clean, &cpos, cdc);
                cidx++;
            }
        }
        // the true path may have to run past the last thread's records
        while (cidx < total_mcus && cpos < Lbits) {
            if (cidx % mpr == 0) {
                rows[cidx / mpr].pos = cpos;
                memcpy(rows[cidx / mpr].dc, cdc, sizeof cdc);
            }
            skip_mcu(j, clean, &cpos, cdc);
            cidx++;
        }
    validate:
        for (int r = 0; r < R; r++)
            if (rows[r].pos == UINT64_MAX || rows[r].pos >= Lbits) {
                st->sync_ms = now_ms() - t1;
                goto failed;
            }
        st->sync_ms = now_ms() - t1;

        // --- 5. build band JPEGs and decode them (parallel) ---
        double t2 = now_ms();
        int target = g_bands > 0 ? g_bands : T * 2;
        if (target > R) target = R;
        nbands = target;
        bstart = malloc((nbands + 1) * sizeof *bstart);
        for (int b = 0; b <= nbands; b++) bstart[b] = (int)((long)R * b / nbands);
        // context rows needed by fancy upsampling across band edges (BGRX, v>1)
        const int ctx = (!o->planes && fi->vmax > 1) ? 1 : 0;
        int fail = 0, *const pfail = &fail;
        int noenc = 0, *const pnoenc = &noenc;   // a band's first DC can't be re-encoded (see below)
        st->mode = 2;
        parallel_for(nbands, ^(size_t b) {
            int o0 = bstart[b], o1 = bstart[b + 1];
            int d0 = o0 - ctx < 0 ? 0 : o0 - ctx;
            int d1 = o1 + ctx > R ? R : o1 + ctx;
            uint64_t a = rows[d0].pos, e = d1 == R ? Lbits : rows[d1].pos;
            if (e <= a) { __atomic_store_n(pfail, 1, __ATOMIC_RELAXED); return; }   // damaged stream
            size_t cap = (size_t)((e - a) / 8) * 2 + 1024;
            uint8_t *buf = malloc(cap);
            BW w = {buf, 0, 0};
            // first MCU: re-encode each component's first DC with the true predictor
            uint64_t pos = a;
            for (int bl = 0; bl < j->nblocks; bl++) {
                int c = j->bcomp[bl];
                const HuffTable *dt = &j->dc[j->cdc[c]];
                uint64_t bstartpos = pos;
                uint64_t wv = peek64(clean, pos);
                unsigned en = dt->look[wv >> (64 - LOOK)];
                int l, s;
                if (en) { l = en >> 8; s = en & 15; } else { l = huff_slow(dt, wv, &s); s &= 15; }
                int diff = s ? extend((unsigned)((wv << l) >> (64 - s)), s) : 0;
                pos += l + s;
                uint64_t dcend = pos;
                int32_t dummy[3] = {0, 0, 0};
                // skip AC of this block by skipping a one-block "MCU"
                {
                    const HuffTable *at = &j->ac[j->cac[c]];
                    int k = 1;
                    while (k < 64) {
                        uint64_t x = peek64(clean, pos);
                        unsigned ea = at->aclook[x >> (64 - LOOK)];
                        if (ea) { pos += ea >> 8; k += ea & 0xFF; }
                        else {
                            int sym;
                            int ll = huff_slow(at, x, &sym);
                            int r = sym >> 4, ss = sym & 15;
                            pos += ll + ss;
                            k += ss ? r + 1 : (r == 15 ? 16 : 64);
                        }
                    }
                }
                (void)dummy;
                if (j->bfirst[bl]) {
                    int nd = diff + rows[d0].dc[c];
                    int ns = 0;
                    for (unsigned m = (unsigned)(nd < 0 ? -nd : nd); m; m >>= 1) ns++;
                    // optimised Huffman tables only have codes for the sizes the image used:
                    // this band can't be built (the stream is fine) -> single-threaded decode
                    if (ns > 15 || !dt->ehufsi[ns]) { __atomic_store_n(pnoenc, 1, __ATOMIC_RELAXED); free(buf); return; }
                    bw_put(&w, dt->ehufco[ns], dt->ehufsi[ns]);
                    if (ns) bw_put(&w, (uint32_t)(nd < 0 ? nd - 1 : nd), ns);
                    bw_copy(&w, clean, dcend, pos);
                } else {
                    bw_copy(&w, clean, bstartpos, pos);
                }
            }
            if (pos > e) { __atomic_store_n(pfail, 1, __ATOMIC_RELAXED); free(buf); return; }   // first MCU overran the band: damaged
            bw_copy(&w, clean, pos, e);
            bw_finish(&w);
            uint8_t hdr[4096];
            band_header(j, hdr, d0, d1);
            const uint8_t *seg[3] = {hdr, buf, NULL};
            size_t sl[3] = {j->hdr_len, (size_t)(w.p - buf), 0};
            if (decode_band(j, o, seg, sl, d0, o0, o1)) __atomic_store_n(pfail, 1, __ATOMIC_RELAXED);
            free(buf);
        });
        st->decode_ms = now_ms() - t2;
        st->bands = nbands;
        if (fail) goto failed;
        if (noenc) goto single;
        rc = 0;
        goto done;
    }

failed:
    // Damaged (or unexpected) stream: don't retry here, the caller's
    // fallback decoder (TurboJPEG) shows whatever is recoverable.
    rc = -1;
    goto done;
single:
    {
        double t3 = now_ms();
        st->mode = 0;
        st->bands = 1;
        rc = decode_single(j, o);
        st->decode_ms = now_ms() - t3;
    }
done:
    for (int t = 0; t < T; t++) { free(sc[t].rst); free(sc[t].rstc); }
    free(sc);
    if (sp) {
        for (int t = 0; t < T; t++) free(sp[t].rec);
        free(sp);
    }
    free(rows);
    free(clean);
    free(bstart);
    free(j);
    return rc;
}

#endif  // NJ_REFERENCE

// ===========================================================================
// Own engine: the Huffman stream is decoded exactly once (speculatively, in
// parallel), the coefficients stay in memory, then dequantisation + IDCT run
// in parallel. No libjpeg-turbo involved. The IDCT is the same algorithm as
// libjpeg's "ISLOW" (jidctint.c), so the output is bit-identical to it.

#ifdef NJ_REFERENCE
static int g_engine = 0;   // 0: own engine, 1: libjpeg-turbo bands
void nj_set_engine(int e) { g_engine = e; }
#endif
static int g_scalar_idct = 0;
void nj_set_scalar_idct(int on) { g_scalar_idct = on; }

#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

static inline uint8_t clamp_u8(int64_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

// Reference implementation (port of jpeg_idct_islow). 'dc' replaces in[0].
static void idct_islow_scalar(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    int32_t ws[64];
    for (int col = 0; col < 8; col++) {
        const int16_t *ip = in + col;
        const uint16_t *qp = q + col;
        int32_t *wp = ws + col;
        int64_t d0 = (int64_t)(col ? ip[0] : dc) * qp[0];
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            for (int r = 0; r < 8; r++) wp[8 * r] = (int32_t)(d0 * 4);
            continue;
        }
        int64_t z2 = (int64_t)ip[16] * qp[16], z3 = (int64_t)ip[48] * qp[48];
        int64_t z1 = (z2 + z3) * FIX_0_541196100;
        int64_t tmp2 = z1 + z3 * -FIX_1_847759065, tmp3 = z1 + z2 * FIX_0_765366865;
        z2 = d0;
        z3 = (int64_t)ip[32] * qp[32];
        int64_t tmp0 = (z2 + z3) * 8192, tmp1 = (z2 - z3) * 8192;
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = (int64_t)ip[56] * qp[56]; tmp1 = (int64_t)ip[40] * qp[40];
        tmp2 = (int64_t)ip[24] * qp[24]; tmp3 = (int64_t)ip[8] * qp[8];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3, z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336; tmp1 *= FIX_2_053119869; tmp2 *= FIX_3_072711026; tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447; z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        wp[0] = (int32_t)((tmp10 + tmp3 + 1024) >> 11);  wp[56] = (int32_t)((tmp10 - tmp3 + 1024) >> 11);
        wp[8] = (int32_t)((tmp11 + tmp2 + 1024) >> 11);  wp[48] = (int32_t)((tmp11 - tmp2 + 1024) >> 11);
        wp[16] = (int32_t)((tmp12 + tmp1 + 1024) >> 11); wp[40] = (int32_t)((tmp12 - tmp1 + 1024) >> 11);
        wp[24] = (int32_t)((tmp13 + tmp0 + 1024) >> 11); wp[32] = (int32_t)((tmp13 - tmp0 + 1024) >> 11);
    }
    for (int row = 0; row < 8; row++) {
        const int32_t *wp = ws + row * 8;
        uint8_t *op = out + row * pitch;
        int64_t z2 = wp[2], z3 = wp[6];
        int64_t z1 = (z2 + z3) * FIX_0_541196100;
        int64_t tmp2 = z1 + z3 * -FIX_1_847759065, tmp3 = z1 + z2 * FIX_0_765366865;
        int64_t tmp0 = ((int64_t)wp[0] + wp[4]) * 8192, tmp1 = ((int64_t)wp[0] - wp[4]) * 8192;
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = wp[7]; tmp1 = wp[5]; tmp2 = wp[3]; tmp3 = wp[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3, z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336; tmp1 *= FIX_2_053119869; tmp2 *= FIX_3_072711026; tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447; z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
#define IDCT_OUT(x) clamp_u8((((x) + (1 << 17)) >> 18) + 128)
        op[0] = IDCT_OUT(tmp10 + tmp3); op[7] = IDCT_OUT(tmp10 - tmp3);
        op[1] = IDCT_OUT(tmp11 + tmp2); op[6] = IDCT_OUT(tmp11 - tmp2);
        op[2] = IDCT_OUT(tmp12 + tmp1); op[5] = IDCT_OUT(tmp12 - tmp1);
        op[3] = IDCT_OUT(tmp13 + tmp0); op[4] = IDCT_OUT(tmp13 - tmp0);
#undef IDCT_OUT
    }
}

#ifdef __AVX2__
#include <immintrin.h>

#define ADD _mm256_add_epi32
#define SUB _mm256_sub_epi32
#define MULK(a, k) _mm256_mullo_epi32(a, _mm256_set1_epi32(k))

// One 1-D ISLOW pass on 8 vectors (x[k] = frequency k, lanes independent).
static inline __attribute__((always_inline)) void idct8_avx2(__m256i *x, int shift) {
    __m256i z2 = x[2], z3 = x[6];
    __m256i z1 = MULK(ADD(z2, z3), FIX_0_541196100);
    __m256i tmp2 = ADD(z1, MULK(z3, -FIX_1_847759065));
    __m256i tmp3 = ADD(z1, MULK(z2, FIX_0_765366865));
    __m256i tmp0 = _mm256_slli_epi32(ADD(x[0], x[4]), 13);
    __m256i tmp1 = _mm256_slli_epi32(SUB(x[0], x[4]), 13);
    __m256i tmp10 = ADD(tmp0, tmp3), tmp13 = SUB(tmp0, tmp3), tmp11 = ADD(tmp1, tmp2), tmp12 = SUB(tmp1, tmp2);
    __m256i t0 = x[7], t1 = x[5], t2 = x[3], t3 = x[1];
    z1 = ADD(t0, t3); z2 = ADD(t1, t2); z3 = ADD(t0, t2);
    __m256i z4 = ADD(t1, t3);
    __m256i z5 = MULK(ADD(z3, z4), FIX_1_175875602);
    t0 = MULK(t0, FIX_0_298631336); t1 = MULK(t1, FIX_2_053119869);
    t2 = MULK(t2, FIX_3_072711026); t3 = MULK(t3, FIX_1_501321110);
    z1 = MULK(z1, -FIX_0_899976223); z2 = MULK(z2, -FIX_2_562915447);
    z3 = ADD(MULK(z3, -FIX_1_961570560), z5); z4 = ADD(MULK(z4, -FIX_0_390180644), z5);
    t0 = ADD(t0, ADD(z1, z3)); t1 = ADD(t1, ADD(z2, z4));
    t2 = ADD(t2, ADD(z2, z3)); t3 = ADD(t3, ADD(z1, z4));
    __m256i rnd = _mm256_set1_epi32(1 << (shift - 1));
    __m128i sh = _mm_cvtsi32_si128(shift);
    tmp10 = ADD(tmp10, rnd); tmp11 = ADD(tmp11, rnd); tmp12 = ADD(tmp12, rnd); tmp13 = ADD(tmp13, rnd);
    x[0] = _mm256_sra_epi32(ADD(tmp10, t3), sh); x[7] = _mm256_sra_epi32(SUB(tmp10, t3), sh);
    x[1] = _mm256_sra_epi32(ADD(tmp11, t2), sh); x[6] = _mm256_sra_epi32(SUB(tmp11, t2), sh);
    x[2] = _mm256_sra_epi32(ADD(tmp12, t1), sh); x[5] = _mm256_sra_epi32(SUB(tmp12, t1), sh);
    x[3] = _mm256_sra_epi32(ADD(tmp13, t0), sh); x[4] = _mm256_sra_epi32(SUB(tmp13, t0), sh);
}

static inline __attribute__((always_inline)) void transpose8_avx2(__m256i *x) {
    __m256i t0 = _mm256_unpacklo_epi32(x[0], x[1]), t1 = _mm256_unpackhi_epi32(x[0], x[1]);
    __m256i t2 = _mm256_unpacklo_epi32(x[2], x[3]), t3 = _mm256_unpackhi_epi32(x[2], x[3]);
    __m256i t4 = _mm256_unpacklo_epi32(x[4], x[5]), t5 = _mm256_unpackhi_epi32(x[4], x[5]);
    __m256i t6 = _mm256_unpacklo_epi32(x[6], x[7]), t7 = _mm256_unpackhi_epi32(x[6], x[7]);
    __m256i u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);
    __m256i u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);
    __m256i u4 = _mm256_unpacklo_epi64(t4, t6), u5 = _mm256_unpackhi_epi64(t4, t6);
    __m256i u6 = _mm256_unpacklo_epi64(t5, t7), u7 = _mm256_unpackhi_epi64(t5, t7);
    x[0] = _mm256_permute2x128_si256(u0, u4, 0x20); x[4] = _mm256_permute2x128_si256(u0, u4, 0x31);
    x[1] = _mm256_permute2x128_si256(u1, u5, 0x20); x[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
    x[2] = _mm256_permute2x128_si256(u2, u6, 0x20); x[6] = _mm256_permute2x128_si256(u2, u6, 0x31);
    x[3] = _mm256_permute2x128_si256(u3, u7, 0x20); x[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}

static void idct_islow_avx2(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    __m256i x[8];
    for (int r = 0; r < 8; r++)
        x[r] = _mm256_mullo_epi32(_mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i *)(in + 8 * r))),
                                  _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(q + 8 * r))));
    x[0] = _mm256_blend_epi32(x[0], _mm256_set1_epi32(dc * q[0]), 1);
    idct8_avx2(x, 11);          // columns (lanes = columns)
    transpose8_avx2(x);
    idct8_avx2(x, 18);          // rows (lanes = rows)
    transpose8_avx2(x);
    __m256i c128 = _mm256_set1_epi32(128);
    __m256i p[4];
    for (int i = 0; i < 4; i++)
        p[i] = _mm256_permute4x64_epi64(_mm256_packs_epi32(ADD(x[2 * i], c128), ADD(x[2 * i + 1], c128)), 0xD8);
    __m256i b0 = _mm256_packus_epi16(p[0], p[1]);   // lane0: rows 0,2  lane1: rows 1,3
    __m256i b1 = _mm256_packus_epi16(p[2], p[3]);   // lane0: rows 4,6  lane1: rows 5,7
    __m128i l0 = _mm256_castsi256_si128(b0), h0 = _mm256_extracti128_si256(b0, 1);
    __m128i l1 = _mm256_castsi256_si128(b1), h1 = _mm256_extracti128_si256(b1, 1);
    _mm_storel_epi64((__m128i *)(out), l0);
    _mm_storel_epi64((__m128i *)(out + pitch), h0);
    _mm_storeh_pd((double *)(out + 2 * pitch), _mm_castsi128_pd(l0));
    _mm_storeh_pd((double *)(out + 3 * pitch), _mm_castsi128_pd(h0));
    _mm_storel_epi64((__m128i *)(out + 4 * pitch), l1);
    _mm_storel_epi64((__m128i *)(out + 5 * pitch), h1);
    _mm_storeh_pd((double *)(out + 6 * pitch), _mm_castsi128_pd(l1));
    _mm_storeh_pd((double *)(out + 7 * pitch), _mm_castsi128_pd(h1));
}
#undef ADD
#undef SUB
#undef MULK
#endif

#if defined(__ARM_NEON) && !defined(__AVX2__)
#include <arm_neon.h>

// Same as the AVX2 version with 4-wide vectors: every row is two halves,
// lo = columns 0-3, hi = columns 4-7.

#define ADD vaddq_s32
#define SUB vsubq_s32
#define MULK(a, k) vmulq_n_s32(a, k)

// One 1-D ISLOW pass on 8 vectors (x[k] = frequency k, lanes independent).
static inline __attribute__((always_inline)) void idct4_neon(int32x4_t *x, int shift) {
    int32x4_t z2 = x[2], z3 = x[6];
    int32x4_t z1 = MULK(ADD(z2, z3), FIX_0_541196100);
    int32x4_t tmp2 = ADD(z1, MULK(z3, -FIX_1_847759065));
    int32x4_t tmp3 = ADD(z1, MULK(z2, FIX_0_765366865));
    int32x4_t tmp0 = vshlq_n_s32(ADD(x[0], x[4]), 13);
    int32x4_t tmp1 = vshlq_n_s32(SUB(x[0], x[4]), 13);
    int32x4_t tmp10 = ADD(tmp0, tmp3), tmp13 = SUB(tmp0, tmp3), tmp11 = ADD(tmp1, tmp2), tmp12 = SUB(tmp1, tmp2);
    int32x4_t t0 = x[7], t1 = x[5], t2 = x[3], t3 = x[1];
    z1 = ADD(t0, t3); z2 = ADD(t1, t2); z3 = ADD(t0, t2);
    int32x4_t z4 = ADD(t1, t3);
    int32x4_t z5 = MULK(ADD(z3, z4), FIX_1_175875602);
    t0 = MULK(t0, FIX_0_298631336); t1 = MULK(t1, FIX_2_053119869);
    t2 = MULK(t2, FIX_3_072711026); t3 = MULK(t3, FIX_1_501321110);
    z1 = MULK(z1, -FIX_0_899976223); z2 = MULK(z2, -FIX_2_562915447);
    z3 = ADD(MULK(z3, -FIX_1_961570560), z5); z4 = ADD(MULK(z4, -FIX_0_390180644), z5);
    t0 = ADD(t0, ADD(z1, z3)); t1 = ADD(t1, ADD(z2, z4));
    t2 = ADD(t2, ADD(z2, z3)); t3 = ADD(t3, ADD(z1, z4));
    int32x4_t rnd = vdupq_n_s32(1 << (shift - 1)), sh = vdupq_n_s32(-shift);
    tmp10 = ADD(tmp10, rnd); tmp11 = ADD(tmp11, rnd); tmp12 = ADD(tmp12, rnd); tmp13 = ADD(tmp13, rnd);
    x[0] = vshlq_s32(ADD(tmp10, t3), sh); x[7] = vshlq_s32(SUB(tmp10, t3), sh);
    x[1] = vshlq_s32(ADD(tmp11, t2), sh); x[6] = vshlq_s32(SUB(tmp11, t2), sh);
    x[2] = vshlq_s32(ADD(tmp12, t1), sh); x[5] = vshlq_s32(SUB(tmp12, t1), sh);
    x[3] = vshlq_s32(ADD(tmp13, t0), sh); x[4] = vshlq_s32(SUB(tmp13, t0), sh);
}

static inline __attribute__((always_inline)) void transpose4_neon(int32x4_t *a, int32x4_t *b, int32x4_t *c, int32x4_t *d) {
    int32x4_t t0 = vtrn1q_s32(*a, *b), t1 = vtrn2q_s32(*a, *b);
    int32x4_t t2 = vtrn1q_s32(*c, *d), t3 = vtrn2q_s32(*c, *d);
    *a = vreinterpretq_s32_s64(vtrn1q_s64(vreinterpretq_s64_s32(t0), vreinterpretq_s64_s32(t2)));
    *b = vreinterpretq_s32_s64(vtrn1q_s64(vreinterpretq_s64_s32(t1), vreinterpretq_s64_s32(t3)));
    *c = vreinterpretq_s32_s64(vtrn2q_s64(vreinterpretq_s64_s32(t0), vreinterpretq_s64_s32(t2)));
    *d = vreinterpretq_s32_s64(vtrn2q_s64(vreinterpretq_s64_s32(t1), vreinterpretq_s64_s32(t3)));
}

// lo[r] / hi[r]: columns 0-3 / 4-7 of row r. Transposes the 8x8 matrix.
static inline __attribute__((always_inline)) void transpose8_neon(int32x4_t *lo, int32x4_t *hi) {
    transpose4_neon(&lo[0], &lo[1], &lo[2], &lo[3]);
    transpose4_neon(&hi[0], &hi[1], &hi[2], &hi[3]);
    transpose4_neon(&lo[4], &lo[5], &lo[6], &lo[7]);
    transpose4_neon(&hi[4], &hi[5], &hi[6], &hi[7]);
    for (int i = 0; i < 4; i++) { int32x4_t t = hi[i]; hi[i] = lo[i + 4]; lo[i + 4] = t; }
}

static void idct_islow_neon(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    int32x4_t lo[8], hi[8];
    for (int r = 0; r < 8; r++) {
        int16x8_t c = vld1q_s16(in + 8 * r);
        uint16x8_t k = vld1q_u16(q + 8 * r);
        lo[r] = vmulq_s32(vmovl_s16(vget_low_s16(c)), vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(k))));
        hi[r] = vmulq_s32(vmovl_s16(vget_high_s16(c)), vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(k))));
    }
    lo[0] = vsetq_lane_s32(dc * q[0], lo[0], 0);
    idct4_neon(lo, 11);          // columns (lanes = columns)
    idct4_neon(hi, 11);
    transpose8_neon(lo, hi);
    idct4_neon(lo, 18);          // rows (lanes = rows)
    idct4_neon(hi, 18);
    transpose8_neon(lo, hi);
    int32x4_t c128 = vdupq_n_s32(128);
    for (int r = 0; r < 8; r++) {
        int16x8_t s = vcombine_s16(vqmovn_s32(ADD(lo[r], c128)), vqmovn_s32(ADD(hi[r], c128)));
        vst1_u8(out + r * pitch, vqmovun_s16(s));
    }
}
#undef ADD
#undef SUB
#undef MULK
#endif

static inline void idct_block(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
#ifdef __AVX2__
    if (!g_scalar_idct) { idct_islow_avx2(in, q, dc, out, pitch); return; }
#elif defined(__ARM_NEON)
    if (!g_scalar_idct) { idct_islow_neon(in, q, dc, out, pitch); return; }
#else
    (void)g_scalar_idct;   // no SIMD IDCT in this build: always the plain C one
#endif
    idct_islow_scalar(in, q, dc, out, pitch);
}

// IDCT all blocks of MCU 'idx'. dcoff: DC correction per component (modulo 2^16).
static inline void idct_mcu(const Jpeg *j, const Output *o, size_t idx, const int16_t *coef, const int32_t *dcoff) {
    const nj_info *fi = &j->info;
    size_t mx = idx % fi->mcus_per_row, my = idx / fi->mcus_per_row;
    for (int b = 0; b < j->nblocks; b++) {
        int c = j->bcomp[b];
        const int16_t *blk = coef + 64 * b;
        int dc = (int16_t)(uint16_t)((uint16_t)blk[0] + (uint16_t)dcoff[c]);
        size_t px = (mx * fi->h[c] + j->bx[b]) * 8, py = (my * fi->v[c] + j->by[b]) * 8;
        idct_block(blk, j->qt[j->cq[c]], dc, o->planes[c] + py * o->pitch[c] + px, o->pitch[c]);
    }
}

// Huffman decode with validity check: returns length, or -1 for an invalid code.
static inline int huff_slow_chk(const HuffTable *t, uint64_t w, int *sym) {
    unsigned c16 = (unsigned)(w >> 48);
    for (int l = LOOK + 1; l <= 16; l++) {
        int32_t c = (int32_t)(c16 >> (16 - l));
        if (c <= t->maxcode[l]) {
            *sym = t->huffval[(c + t->valoff[l]) & 0xFF];
            return l;
        }
    }
    *sym = 0;
    return -1;
}

// Full decode of one MCU into nblocks x 64 coefficients (natural order,
// quantised). DC is stored as the running predictor value (int16, modulo
// 2^16: a later constant offset fixes speculative starts). Returns nonzero
// if the data was invalid (bad code, coefficient index overrun).
static inline int decode_mcu_coef(const Jpeg *j, BR *br, int32_t *dc, int16_t *out) {
    int err = 0;
    for (int b = 0; b < j->nblocks; b++) {
        int c = j->bcomp[b];
        const HuffTable *dt = &j->dc[j->cdc[c]], *at = &j->ac[j->cac[c]];
        int16_t *blk = out + 64 * b;
        memset(blk, 0, 64 * sizeof *blk);
        br_refill(br);
        uint64_t w = br->buf;
        unsigned e = dt->look[w >> (64 - LOOK)];
        int l, s;
        if (__builtin_expect(e != 0, 1)) { l = e >> 8; s = e & 0xFF; }
        else if ((l = huff_slow_chk(dt, w, &s)) < 0) { err = 1; l = 16; }
        if (s > 15) { err = 1; s &= 15; }
        if (s) dc[c] += extend((unsigned)((w << l) >> (64 - s)), s);
        br_skip(br, l + s);
        blk[0] = (int16_t)dc[c];
        for (int k = 1; k < 64;) {
            br_refill(br);
            w = br->buf;
            int32_t f = at->acfast[w >> (64 - LOOK)];
            if (__builtin_expect(f != 0, 1)) {       // code + value in one lookup
                br_skip(br, f & 0xFF);
                int kinc = (f >> 8) & 0xFF;
                if (kinc == 0) break;                 // EOB
                if (kinc == 0x40) { k += 16; continue; }
                k += kinc - 1;
                if (k > 63) err = 1;
                blk[kNatural[k]] = (int16_t)(f >> 16);
                k++;
                continue;
            }
            e = at->look[w >> (64 - LOOK)];
            int sym;
            if (__builtin_expect(e != 0, 1)) { l = e >> 8; sym = e & 0xFF; }
            else if ((l = huff_slow_chk(at, w, &sym)) < 0) { err = 1; l = 16; }
            int r = sym >> 4;
            s = sym & 15;
            if (s) {
                k += r;
                if (k > 63) err = 1;
                blk[kNatural[k]] = (int16_t)extend((unsigned)((w << l) >> (64 - s)), s);
                br_skip(br, l + s);
                k++;
            } else {
                br_skip(br, l);
                if (r != 15) break;
                k += 16;
            }
        }
    }
    return err;
}

// --- buffer pool: big buffers are reused between images (no page faults) ---
typedef struct { void *p; size_t cap; } PoolBuf;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static PoolBuf g_pool[4096];
static int g_pool_n;

static void *pool_take(size_t need, size_t *cap) {
    pthread_mutex_lock(&g_pool_mu);
    int best = -1;
    for (int i = 0; i < g_pool_n; i++)
        if (g_pool[i].cap >= need && (best < 0 || g_pool[i].cap < g_pool[best].cap)) best = i;
    if (best >= 0) {
        void *p = g_pool[best].p;
        *cap = g_pool[best].cap;
        g_pool[best] = g_pool[--g_pool_n];
        pthread_mutex_unlock(&g_pool_mu);
        return p;
    }
    pthread_mutex_unlock(&g_pool_mu);
    *cap = need;
    return malloc(need);
}

static void pool_give(void *p, size_t cap) {
    if (!p) return;
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_n == (int)(sizeof g_pool / sizeof *g_pool)) {   // full: drop the smallest
        int s = 0;
        for (int i = 1; i < g_pool_n; i++) if (g_pool[i].cap < g_pool[s].cap) s = i;
        free(g_pool[s].p);
        g_pool[s] = g_pool[--g_pool_n];
    }
    g_pool[g_pool_n].p = p;
    g_pool[g_pool_n].cap = cap;
    g_pool_n++;
    pthread_mutex_unlock(&g_pool_mu);
}

static void *pool_grow(void *p, size_t *cap, size_t need) {   // like realloc, keeps contents
    if (need <= *cap) return p;
    size_t ncap = need + need / 2;
    void *np = realloc(p, ncap);
    *cap = ncap;
    return np;
}

// --- pipelined chunk processing (own engine, no restart markers) ---
typedef struct {
    uint64_t start, end;              // bit range of the clean stream
    Rec *rec; size_t rec_cap;         // MCU starts (bytes allocated)
    int16_t *coef; size_t coef_cap;   // coefficients per record
    size_t n;
    int nerr;
    uint32_t err[32];
    long off;                         // absolute MCU index = off + record index
    int32_t dcoff[3];
    size_t lo, hi;                    // valid records after stitching
    int state;                        // 0 new, 1 decoded, 2 stitched, 3 IDCT claimed
} Chunk;

typedef struct {
    pthread_mutex_t mu;
    int frontier, finalized, fail;
    uint64_t cpos, endpos;
    size_t cidx;
    int32_t cdc[3];
    int16_t tmp[MAX_BLOCKS * 64];
} Stitch;

static int g_chunks = 0;
void nj_set_chunks(int n) { g_chunks = n; }

static void chunk_decode(const Jpeg *j, const uint8_t *buf, Chunk *s, size_t max_mcus) {
    const size_t msz = (size_t)j->nblocks * 64;
    BR br;
    br_init(&br, buf, s->start);
    uint64_t pos = s->start;
    int32_t dc[3] = {0, 0, 0};
    size_t n = 0;
    while (pos < s->end && n < max_mcus) {
        if ((n + 2) * sizeof(Rec) > s->rec_cap) s->rec = pool_grow(s->rec, &s->rec_cap, (n + 2) * sizeof(Rec));
        if ((n + 1) * msz * 2 > s->coef_cap) s->coef = pool_grow(s->coef, &s->coef_cap, (n + 1) * msz * 2);
        Rec *r = &s->rec[n];
        r->pos = pos;
        memcpy(r->dc, dc, sizeof dc);
        if (decode_mcu_coef(j, &br, dc, s->coef + n * msz)) {
            if (s->nerr < 32) s->err[s->nerr] = (uint32_t)n;
            s->nerr++;
        }
        n++;
        pos = br_pos(&br, buf);
    }
    if ((n + 2) * sizeof(Rec) > s->rec_cap) s->rec = pool_grow(s->rec, &s->rec_cap, (n + 2) * sizeof(Rec));
    s->rec[n].pos = pos;
    memcpy(s->rec[n].dc, dc, sizeof dc);
    s->n = n;
}

static int chunk_err_in(const Chunk *s, size_t lo, size_t hi) {
    if (s->nerr > 32) return 1;
    for (int i = 0; i < s->nerr; i++)
        if (s->err[i] >= lo && s->err[i] < hi) return 1;
    return 0;
}

// Claims a stitched chunk and runs its IDCT. Returns 1 if this call did it.
static int chunk_claim_idct(const Jpeg *j, const Output *o, Chunk *c, size_t msz) {
    int expect = 2;
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != 2 ||
        !__atomic_compare_exchange_n(&c->state, &expect, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0;
    for (size_t i = c->lo; i < c->hi; i++) idct_mcu(j, o, (size_t)(c->off + (long)i), c->coef + i * msz, c->dcoff);
    return 1;
}

// Decode + IDCT one MCU of the true path directly (DC already correct).
static int stitch_step(const Jpeg *j, const Output *o, const uint8_t *clean, Stitch *S, size_t total) {
    static const int32_t zero[3] = {0, 0, 0};
    BR br;
    br_init(&br, clean, S->cpos);
    if (decode_mcu_coef(j, &br, S->cdc, S->tmp)) return -1;
    idct_mcu(j, o, S->cidx, S->tmp, zero);
    S->cpos = br_pos(&br, clean);
    S->cidx++;
    if (S->cidx == total) S->endpos = S->cpos;
    return 0;
}

// Advances the stitching frontier over all consecutive decoded chunks.
static void stitch_advance(const Jpeg *j, const Output *o, const uint8_t *clean, uint64_t Lbits, size_t total,
                           Chunk *ch, int C, Stitch *S, nj_stats *st) {
    if (pthread_mutex_trylock(&S->mu)) return;   // someone else is stitching
    while (S->frontier < C && __atomic_load_n(&ch[S->frontier].state, __ATOMIC_ACQUIRE) >= 1) {
        Chunk *s = &ch[S->frontier];
        s->lo = s->hi = 0;
        if (S->fail) {
            // nothing valid
        } else if (S->frontier == 0) {
            s->off = 0;
            s->hi = s->n < total ? s->n : total;
            if (chunk_err_in(s, 0, s->hi)) __atomic_store_n(&S->fail, 1, __ATOMIC_RELAXED);
            if (s->n >= total) S->endpos = s->rec[total].pos;
            S->cpos = s->rec[s->n].pos;
            S->cidx = s->n;
            memcpy(S->cdc, s->rec[s->n].dc, sizeof S->cdc);
        } else if (S->cidx < total) {
            size_t jx = 0;
            for (;;) {
                while (jx <= s->n && s->rec[jx].pos < S->cpos) jx++;
                if (jx <= s->n && s->rec[jx].pos == S->cpos) {       // synchronised
                    long off = (long)S->cidx - (long)jx;
                    for (int c = 0; c < 3; c++) s->dcoff[c] = S->cdc[c] - s->rec[jx].dc[c];
                    size_t hi = (size_t)((long)total - off) < s->n ? (size_t)((long)total - off) : s->n;
                    if (chunk_err_in(s, jx, hi)) { __atomic_store_n(&S->fail, 1, __ATOMIC_RELAXED); break; }
                    s->off = off;
                    s->lo = jx;
                    s->hi = hi;
                    if (off + (long)s->n >= (long)total) S->endpos = s->rec[total - off].pos;
                    if (st->nsync < 256) {
                        st->sync_mcus[st->nsync] = (int32_t)jx;
                        st->sync_bits[st->nsync] = (uint32_t)(S->cpos - s->start);
                        st->nsync++;
                    }
                    S->cpos = s->rec[s->n].pos;
                    S->cidx = off + s->n;
                    for (int c = 0; c < 3; c++) S->cdc[c] = s->rec[s->n].dc[c] + s->dcoff[c];
                    break;
                }
                if (jx > s->n || S->cidx >= total || S->cpos >= Lbits) {
                    if (jx > s->n && st->nsync < 256) { st->sync_mcus[st->nsync] = -1; st->sync_bits[st->nsync] = 0; st->nsync++; }
                    break;
                }
                if (stitch_step(j, o, clean, S, total)) { __atomic_store_n(&S->fail, 1, __ATOMIC_RELAXED); break; }
            }
        }
        __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
        S->frontier++;
    }
    if (S->frontier == C && !S->finalized) {
        while (!S->fail && S->cidx < total && S->cpos < Lbits)   // past the last chunk
            if (stitch_step(j, o, clean, S, total)) __atomic_store_n(&S->fail, 1, __ATOMIC_RELAXED);
        if (S->cidx < total || S->endpos > Lbits) __atomic_store_n(&S->fail, 1, __ATOMIC_RELAXED);   // truncated
        S->finalized = 1;
    }
    pthread_mutex_unlock(&S->mu);
}

static int nj_decode_own(const uint8_t *data, size_t len, const Output *o, int nthreads, nj_stats *st) {
    Jpeg *j = malloc(sizeof *j);
    if (parse(data, len, j) || !j->info.supported) { free(j); return -1; }
    const nj_info *fi = &j->info;
    if (nthreads <= 0) nthreads = 4 * ncpu();
    const int R = fi->mcu_rows, mpr = fi->mcus_per_row;
    const size_t total = (size_t)R * mpr;
    const size_t msz = (size_t)j->nblocks * 64;   // coefficients per MCU
    int rc = -1;
    double t0 = now_ms();

    // --- 1. scan (parallel): end of scan, stuffing, restart markers ---
    const size_t lo = j->scan_start;
    const int T = nthreads;
    ScanChunk *sc = calloc(T, sizeof *sc);
    for (int t = 0; t < T; t++) {
        sc[t].a = lo + (len - lo) * t / T;
        sc[t].b = lo + (len - lo) * (t + 1) / T;
    }
    parallel_for(T, ^(size_t t) { scan_chunk(data, len, lo, &sc[t]); });
    size_t scan_end = len;
    int tend = T - 1;
    for (int t = 0; t < T; t++)
        if (sc[t].end != SIZE_MAX) { scan_end = sc[t].end; tend = t; break; }

    // --- 2. unstuff into a clean stream, dropping RST markers (parallel) ---
    size_t *coff = malloc((T + 1) * sizeof *coff);
    size_t acc = 0;
    for (int t = 0; t < T; t++) {
        coff[t] = (sc[t].a < scan_end ? sc[t].a : scan_end) - lo - acc;
        if (t <= tend) acc += sc[t].removed;
    }
    const size_t clean_len = (scan_end - lo) - acc;
    size_t clean_cap;
    uint8_t *clean = pool_take(clean_len + PAD_BYTES, &clean_cap);
    memset(clean + clean_len, 0, PAD_BYTES);
    parallel_for(T, ^(size_t t) {
        ScanChunk *c = &sc[t];
        size_t a = c->a, b = c->b < scan_end ? c->b : scan_end;
        if (a >= b) return;
        uint8_t *out = clean + coff[t];
        size_t i = a;
        if (i > lo && data[i - 1] == 0xFF && (data[i] == 0 || (data[i] >= 0xD0 && data[i] <= 0xD7))) i++;
        size_t cap = 0;
        while (i < b) {
            const uint8_t *f = memchr(data + i, 0xFF, b - i);
            size_t n = f ? (size_t)(f - data) - i : b - i;
            memcpy(out, data + i, n);
            out += n;
            i += n;
            if (!f) break;
            uint8_t nb = i + 1 < len ? data[i + 1] : 0;
            if (nb >= 0xD0 && nb <= 0xD7) {          // RST: dropped, next interval starts here
                if (c->nrstc == cap) {
                    cap = cap ? cap * 2 : 64;
                    c->rstc = realloc(c->rstc, cap * sizeof *c->rstc);
                }
                c->rstc[c->nrstc++] = (size_t)(out - clean);
                i += 2;
            } else if (nb == 0x00) {
                *out++ = 0xFF;
                i += 2;
            } else {
                *out++ = 0xFF;
                i += 1;
            }
        }
    });
    free(coff);
    st->scan_ms = now_ms() - t0;
    const uint64_t Lbits = (uint64_t)clean_len * 8;
    int fail = 0, *const pfail = &fail;

    if (fi->restart_interval) {
        // --- restart intervals: decode + IDCT fused, in parallel, no big buffers ---
        const size_t Ri = (size_t)fi->restart_interval;
        const size_t I = (total + Ri - 1) / Ri;
        size_t nr = 0;
        for (int t = 0; t <= tend; t++) nr += sc[t].nrstc;
        size_t *istart = malloc((nr + 2) * sizeof *istart);
        istart[0] = 0;
        size_t k = 1;
        for (int t = 0; t <= tend; t++)
            for (size_t i = 0; i < sc[t].nrstc; i++) istart[k++] = sc[t].rstc[i];
        if (nr + 1 != I) { free(istart); goto out; }   // damaged / unusual
        istart[I] = clean_len;
        int tasks = g_bands > 0 ? g_bands : T * 2;
        if ((size_t)tasks > I) tasks = (int)I;
        double t1 = now_ms();
        parallel_for((size_t)tasks, ^(size_t ti) {
            size_t k0 = I * ti / tasks, k1 = I * (ti + 1) / tasks;
            int16_t coef[MAX_BLOCKS * 64];
            const int32_t zero[3] = {0, 0, 0};
            for (size_t kk = k0; kk < k1 && !__atomic_load_n(pfail, __ATOMIC_RELAXED); kk++) {
                BR br;
                br_init(&br, clean, (uint64_t)istart[kk] * 8);
                int32_t dc[3] = {0, 0, 0};
                size_t m1 = (kk + 1) * Ri < total ? (kk + 1) * Ri : total;
                for (size_t m = kk * Ri; m < m1; m++) {
                    if (decode_mcu_coef(j, &br, dc, coef)) { __atomic_store_n(pfail, 1, __ATOMIC_RELAXED); break; }
                    idct_mcu(j, o, m, coef, zero);
                }
                if (br_pos(&br, clean) > (uint64_t)istart[kk + 1] * 8) __atomic_store_n(pfail, 1, __ATOMIC_RELAXED);   // overran the interval
            }
        });
        st->decode_ms = now_ms() - t1;
        st->bands = tasks;
        st->mode = 1;
        free(istart);
        rc = fail ? -1 : 0;
        goto out;
    }

    {
        // --- 3-5. pipelined: speculative full decode of small chunks, stitching
        // in order, IDCT of stitched chunks while their coefficients are hot ---
        double t1 = now_ms();
        const int C = g_chunks > 0 ? g_chunks : 32 * ncpu();   // ~200 KB of coefficients each
        const int W = t_workers > 0 ? t_workers : g_max_workers > 0 ? g_max_workers : ncpu();
        Chunk *ch = calloc(C, sizeof *ch);
        for (int i = 0; i < C; i++) {
            ch[i].start = (uint64_t)(clean_len * i / C) * 8;
            ch[i].end = i == C - 1 ? Lbits : (uint64_t)(clean_len * (i + 1) / C) * 8;
        }
        Stitch S = {.frontier = 0, .endpos = UINT64_MAX};
        pthread_mutex_init(&S.mu, NULL);
        volatile long next = 0, finished = 0, *const pnext = &next, *const pfinished = &finished;
        Stitch *Sp = &S;
        const size_t est = total / C * 2 + 256;
        parallel_for((size_t)W, ^(size_t w) {
            int own[4096], nown = 0;
            for (;;) {
                long i = __sync_fetch_and_add(pnext, 1);
                if (i >= C) break;
                Chunk *c = &ch[i];
                c->rec = pool_take(est * sizeof(Rec), &c->rec_cap);
                c->coef = pool_take(est * msz * 2, &c->coef_cap);
                if (!__atomic_load_n(&Sp->fail, __ATOMIC_RELAXED)) chunk_decode(j, clean, c, total + 1);
                __atomic_store_n(&c->state, 1, __ATOMIC_RELEASE);
                if (nown < 4096) own[nown++] = (int)i;
                stitch_advance(j, o, clean, Lbits, total, ch, C, Sp, st);
                for (int k = 0; k < nown; k++)       // own stitched chunks: still in cache
                    if (chunk_claim_idct(j, o, &ch[own[k]], msz)) __sync_fetch_and_add(pfinished, 1);
            }
            while (__atomic_load_n(pfinished, __ATOMIC_ACQUIRE) < C) {   // drain: help with anything
                stitch_advance(j, o, clean, Lbits, total, ch, C, Sp, st);
                int did = 0;
                for (int k = 0; k < C; k++)
                    if (chunk_claim_idct(j, o, &ch[k], msz)) { __sync_fetch_and_add(pfinished, 1); did = 1; }
                if (!did) sched_yield();
            }
        });
        fail |= S.fail;
        st->sync_ms = now_ms() - t1;   // whole pipeline (decode + stitch + IDCT)
        st->decode_ms = 0;
        st->bands = C;
        st->mode = 2;
        rc = fail ? -1 : 0;
        for (int i = 0; i < C; i++) { pool_give(ch[i].rec, ch[i].rec_cap); pool_give(ch[i].coef, ch[i].coef_cap); }
        pthread_mutex_destroy(&S.mu);
        free(ch);
    }
out:
    for (int t = 0; t < T; t++) { free(sc[t].rst); free(sc[t].rstc); }
    free(sc);
    pool_give(clean, clean_cap);
    free(j);
    return rc;
}

int nj_decode_planes(const uint8_t *data, size_t len, const nj_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads, nj_stats *stats) {
    Output o = {planes, pitch, NULL, 0, info->width, info->height};
#ifdef NJ_REFERENCE
    if (g_engine == 1) return nj_decode(data, len, &o, nthreads, stats);
#endif
    nj_stats dummy;
    if (!stats) stats = &dummy;
    memset(stats, 0, sizeof *stats);
    t_workers = nthreads == 1 ? 1 : 0;   // nthreads == 1: really single-threaded
    int rc = nj_decode_own(data, len, &o, nthreads, stats);
    t_workers = 0;
    return rc;
}

#ifdef NJ_REFERENCE
int nj_decode_bgrx(const uint8_t *data, size_t len, const nj_info *info, uint8_t *dst, size_t pitch,
                   int nthreads, nj_stats *stats) {
    Output o = {NULL, NULL, dst, pitch, info->width, info->height};
    return nj_decode(data, len, &o, nthreads, stats);
}
#endif
