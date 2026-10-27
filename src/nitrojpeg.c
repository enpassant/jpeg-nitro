// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrojpeg: multi-threaded single-image baseline and progressive JPEG decoding.
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
    int progressive;              // SOF2: decoded by nj_decode_prog
    size_t sos_pos;               // the first SOS marker
    uint8_t comp_id[3];
#ifdef NJ_REFERENCE
    // header template for band JPEGs (the old engine): SOI DQT* DHT* [DRI] SOF SOS
    uint8_t hdr[4096];
    size_t hdr_len;
    size_t hdr_height_off;        // offset of the SOF height field in hdr
    int hdr_overflow;             // template did not fit: old engine can't split this file
#endif
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

#ifdef NJ_REFERENCE
static int hdr_append(Jpeg *j, const uint8_t *p, size_t n) {   // never fails: only the old engine needs it
    if (j->hdr_len + n > sizeof j->hdr) { j->hdr_overflow = 1; return 0; }
    memcpy(j->hdr + j->hdr_len, p, n);
    j->hdr_len += n;
    return 0;
}
#else
static inline int hdr_append(Jpeg *j, const uint8_t *p, size_t n) { (void)j; (void)p; (void)n; return 0; }
#endif

static int parse(const uint8_t *d, size_t len, Jpeg *j) {
    memset(j, 0, sizeof *j);
    j->data = d;
    j->len = len;
    nj_info *fi = &j->info;
    fi->orientation = 1;
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
#ifdef NJ_REFERENCE
    j->hdr[0] = 0xFF; j->hdr[1] = 0xD8; j->hdr_len = 2;
#endif

    uint8_t sof[64]; size_t sof_len = 0;
    uint8_t comp_id[3] = {0};
    int have_sof = 0, baseline = 0, progressive = 0, adobe = -1, jfif = 0;
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
            progressive = m == 0xC2 && seg[0] == 8;
            fi->height = (int)be16(seg + 1);
            fi->width = (int)be16(seg + 3);
            fi->ncomp = seg[5];
            if (fi->ncomp != 1 && fi->ncomp != 3) { fi->ncomp = fi->ncomp > 3 ? 3 : fi->ncomp; baseline = progressive = 0; }
            if (sl < 6 + 3 * (size_t)seg[5]) return -1;
            for (int c = 0; c < fi->ncomp; c++) {
                comp_id[c] = seg[6 + 3 * c];
                j->cq[c] = seg[8 + 3 * c] & 3;
                fi->h[c] = seg[7 + 3 * c] >> 4;
                fi->v[c] = seg[7 + 3 * c] & 15;
                if (fi->h[c] < 1 || fi->h[c] > 4 || fi->v[c] < 1 || fi->v[c] > 4) baseline = progressive = 0;
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
            j->sos_pos = pos - 2;
            memcpy(j->comp_id, comp_id, 3);
            // Is it one interleaved baseline scan with all components? (progressive: the
            // scans are checked by nj_decode_prog, which fails on anything it can't do)
            int ok = baseline && ns == fi->ncomp;
            int ss = seg[1 + 2 * ns], se = seg[2 + 2 * ns], a = seg[3 + 2 * ns];
            if (ss != 0 || se != 63 || a != 0) ok = 0;
            if (progressive) ok = j->progressive = 1;
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
            for (int c = 0; ok && !progressive && c < fi->ncomp; c++)
                if (!j->dc[j->cdc[c]].present || !j->ac[j->cac[c]].present || !j->qpresent[j->cq[c]]) ok = 0;
            // band header template: ... [DRI] SOF SOS
            if (fi->restart_interval) {
                uint8_t dri[6] = {0xFF, 0xDD, 0, 4, (uint8_t)(fi->restart_interval >> 8), (uint8_t)fi->restart_interval};
                if (hdr_append(j, dri, 6)) return -1;
            }
#ifdef NJ_REFERENCE
            j->hdr_height_off = j->hdr_len + 5;
#endif
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
    if (len >= 92 && !memcmp(data, "FUJIFILMCCD-RAW ", 16)) {
        // Fujifilm RAF: the JPEG's offset and length at 84 (big-endian); it has its own EXIF
        uint32_t off = (uint32_t)data[84] << 24 | (uint32_t)data[85] << 16 | (uint32_t)data[86] << 8 | data[87];
        uint32_t cnt = (uint32_t)data[88] << 24 | (uint32_t)data[89] << 16 | (uint32_t)data[90] << 8 | data[91];
        if (off < 92 || cnt < 4 || (off < len - 2 && !(data[off] == 0xFF && data[off + 1] == 0xD8))) return -1;
        *offset = off;
        *size = cnt;
        *orientation = 0;   // (the JPEG's own)
        return 0;
    }
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
    if (!j) return -1;
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
#ifdef NJ_REFERENCE
    size_t *rst;          // raw positions of RST markers (0xFF byte) owned by this chunk (old engine)
    size_t nrst, caprst;
#endif
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
#ifdef NJ_REFERENCE
                if (c->nrst == c->caprst) {
                    c->caprst = c->caprst ? c->caprst * 2 : 256;
                    c->rst = realloc(c->rst, c->caprst * sizeof *c->rst);
                }
                c->rst[c->nrst++] = i;
#endif
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
    if (!j || parse(data, len, j) || !j->info.supported || j->progressive || j->hdr_overflow) { free(j); return -1; }
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

// Zigzag-ordered blocks (progressive decoding): byte j of natural row r is byte
// g_zzidx[r][j] of the zigzag block; g_zzmask[r][s]: the same as a pshufb mask on
// zigzag bytes 16s..16s+15 (0x80: from another 16 bytes).
static uint8_t g_zzidx[8][16];
#ifdef __AVX2__
static uint8_t g_zzmask[8][8][16] __attribute__((aligned(16)));
#endif
static void zz_tables_init(void) {
    for (int k = 0; k < 64; k++) {
        int n = kNatural[k];
        g_zzidx[n / 8][2 * (n % 8)] = (uint8_t)(2 * k);
        g_zzidx[n / 8][2 * (n % 8) + 1] = (uint8_t)(2 * k + 1);
    }
#ifdef __AVX2__
    for (int r = 0; r < 8; r++)
        for (int src = 0; src < 8; src++)
            for (int j = 0; j < 16; j++)
                g_zzmask[r][src][j] = g_zzidx[r][j] / 16 == src ? g_zzidx[r][j] % 16 : 0x80;
#endif
}

#ifdef __AVX2__
#include <immintrin.h>

#define ADD _mm256_add_epi32
#define SUB _mm256_sub_epi32

// ISLOW in 16 bits (as libjpeg-turbo's SIMD code): dequantisation by pmullw, the products by
// pmaddwd (two 16 x 16-bit products summed into 32 bits). The odd part of ISLOW folded into
// one 4 x 4 integer matrix: v7, v5, v3, v1 -> tmp0..tmp3 (the same sums, only reordered:
// e.g. tmp0 = v7 (F_0_298 - F_0_899 - F_1_961 + F_1_175) + v5 F_1_175 + ...), every entry
// fits in 16 bits. One pass on 8 vectors (lanes = the other dimension), descaled by 'shift'
// and saturated to 16 bits.
static inline __m256i pair16(__m128i a, __m128i b) {   // 32-bit lanes (a, b) of columns 0..7
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_unpacklo_epi16(a, b)), _mm_unpackhi_epi16(a, b), 1);
}
#define KP(a, b) _mm256_set1_epi32((int)((uint32_t)(uint16_t)(a) | (uint32_t)(uint16_t)(b) << 16))
#define MADD _mm256_madd_epi16
static inline __attribute__((always_inline)) void islow16_pass(__m128i *v, int shift) {
    __m256i p04 = pair16(v[0], v[4]), p26 = pair16(v[2], v[6]), p75 = pair16(v[7], v[5]), p31 = pair16(v[3], v[1]);
    __m256i t0 = MADD(p04, KP(8192, 8192)), t1 = MADD(p04, KP(8192, -8192));   // << CONST_BITS
    __m256i t3 = MADD(p26, KP(FIX_0_541196100 + FIX_0_765366865, FIX_0_541196100));
    __m256i t2 = MADD(p26, KP(FIX_0_541196100, FIX_0_541196100 - FIX_1_847759065));
    __m256i e10 = ADD(t0, t3), e13 = SUB(t0, t3), e11 = ADD(t1, t2), e12 = SUB(t1, t2);
    __m256i o0 = ADD(MADD(p75, KP(FIX_0_298631336 - FIX_0_899976223 - FIX_1_961570560 + FIX_1_175875602, FIX_1_175875602)),
                     MADD(p31, KP(FIX_1_175875602 - FIX_1_961570560, FIX_1_175875602 - FIX_0_899976223)));
    __m256i o1 = ADD(MADD(p75, KP(FIX_1_175875602, FIX_2_053119869 - FIX_2_562915447 - FIX_0_390180644 + FIX_1_175875602)),
                     MADD(p31, KP(FIX_1_175875602 - FIX_2_562915447, FIX_1_175875602 - FIX_0_390180644)));
    __m256i o2 = ADD(MADD(p75, KP(FIX_1_175875602 - FIX_1_961570560, FIX_1_175875602 - FIX_2_562915447)),
                     MADD(p31, KP(FIX_3_072711026 - FIX_2_562915447 - FIX_1_961570560 + FIX_1_175875602, FIX_1_175875602)));
    __m256i o3 = ADD(MADD(p75, KP(FIX_1_175875602 - FIX_0_899976223, FIX_1_175875602 - FIX_0_390180644)),
                     MADD(p31, KP(FIX_1_175875602, FIX_1_501321110 - FIX_0_899976223 - FIX_0_390180644 + FIX_1_175875602)));
    const __m256i rnd = _mm256_set1_epi32(1 << (shift - 1));
    __m256i w[8] = {ADD(e10, o3), ADD(e11, o2), ADD(e12, o1), ADD(e13, o0), SUB(e13, o0), SUB(e12, o1), SUB(e11, o2), SUB(e10, o3)};
    for (int y = 0; y < 8; y++) {
        __m256i d = _mm256_srai_epi32(ADD(w[y], rnd), shift);
        v[y] = _mm_packs_epi32(_mm256_castsi256_si128(d), _mm256_extracti128_si256(d, 1));
    }
}
#undef MADD
#undef KP

static inline __attribute__((always_inline)) void transpose8x16(__m128i *v) {
    __m128i a0 = _mm_unpacklo_epi16(v[0], v[1]), a1 = _mm_unpackhi_epi16(v[0], v[1]);
    __m128i a2 = _mm_unpacklo_epi16(v[2], v[3]), a3 = _mm_unpackhi_epi16(v[2], v[3]);
    __m128i a4 = _mm_unpacklo_epi16(v[4], v[5]), a5 = _mm_unpackhi_epi16(v[4], v[5]);
    __m128i a6 = _mm_unpacklo_epi16(v[6], v[7]), a7 = _mm_unpackhi_epi16(v[6], v[7]);
    __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    v[0] = _mm_unpacklo_epi64(b0, b4); v[1] = _mm_unpackhi_epi64(b0, b4);
    v[2] = _mm_unpacklo_epi64(b1, b5); v[3] = _mm_unpackhi_epi64(b1, b5);
    v[4] = _mm_unpacklo_epi64(b2, b6); v[5] = _mm_unpackhi_epi64(b2, b6);
    v[6] = _mm_unpacklo_epi64(b3, b7); v[7] = _mm_unpackhi_epi64(b3, b7);
}

// rows: the 8 coefficient rows (natural order, quantised); dc replaces rows[0][0].
static inline __attribute__((always_inline)) void idct_rows_avx2(const __m128i *rows, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    __m128i v[8];
    for (int r = 0; r < 8; r++) v[r] = _mm_mullo_epi16(r ? rows[r] : _mm_insert_epi16(rows[0], dc, 0), _mm_loadu_si128((const __m128i *)(q + 8 * r)));
    islow16_pass(v, 11);        // columns (lanes = columns): v[y] = row y
    transpose8x16(v);
    islow16_pass(v, 18);        // rows (lanes = rows): v[x] = column x
    // to bytes (+128, saturated), then an 8 x 8 byte transpose: column pairs -> rows
    const __m128i bias = _mm_set1_epi8((char)0x80);
    __m128i c[4];
    for (int i = 0; i < 4; i++) {
        __m128i b = _mm_xor_si128(_mm_packs_epi16(v[2 * i], v[2 * i + 1]), bias);   // columns 2i, 2i+1
        c[i] = _mm_unpacklo_epi8(b, _mm_srli_si128(b, 8));                          // per row: (2i, 2i+1)
    }
    __m128i u0 = _mm_unpacklo_epi16(c[0], c[1]), u1 = _mm_unpackhi_epi16(c[0], c[1]);   // columns 0-3
    __m128i u2 = _mm_unpacklo_epi16(c[2], c[3]), u3 = _mm_unpackhi_epi16(c[2], c[3]);   // columns 4-7
    __m128i r01 = _mm_unpacklo_epi32(u0, u2), r23 = _mm_unpackhi_epi32(u0, u2);
    __m128i r45 = _mm_unpacklo_epi32(u1, u3), r67 = _mm_unpackhi_epi32(u1, u3);
    _mm_storel_epi64((__m128i *)out, r01);
    _mm_storeh_pd((double *)(out + pitch), _mm_castsi128_pd(r01));
    _mm_storel_epi64((__m128i *)(out + 2 * pitch), r23);
    _mm_storeh_pd((double *)(out + 3 * pitch), _mm_castsi128_pd(r23));
    _mm_storel_epi64((__m128i *)(out + 4 * pitch), r45);
    _mm_storeh_pd((double *)(out + 5 * pitch), _mm_castsi128_pd(r45));
    _mm_storel_epi64((__m128i *)(out + 6 * pitch), r67);
    _mm_storeh_pd((double *)(out + 7 * pitch), _mm_castsi128_pd(r67));
}

static void idct_islow_avx2(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    __m128i rows[8];
    for (int r = 0; r < 8; r++) rows[r] = _mm_loadu_si128((const __m128i *)(in + 8 * r));
    idct_rows_avx2(rows, q, dc, out, pitch);
}

// The same on a zigzag-ordered block: the natural rows are put together in registers
// (each from the 16-byte pieces of the zigzag order it has coefficients in).
static void idct_islow_avx2_zz(const int16_t *zz, const uint16_t *q, uint8_t *out, size_t pitch) {
    __m128i S[8], rows[8];
    for (int i = 0; i < 8; i++) S[i] = _mm_loadu_si128((const __m128i *)(zz + 8 * i));
#define Z(r, i) _mm_shuffle_epi8(S[i], _mm_load_si128((const __m128i *)g_zzmask[r][i]))
    rows[0] = _mm_or_si128(_mm_or_si128(Z(0, 0), Z(0, 1)), Z(0, 3));
    rows[1] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(1, 0), Z(1, 1)), _mm_or_si128(Z(1, 2), Z(1, 3))), Z(1, 5));
    rows[2] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(2, 0), Z(2, 1)), _mm_or_si128(Z(2, 2), Z(2, 3))), Z(2, 5));
    rows[3] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(3, 1), Z(3, 2)), _mm_or_si128(Z(3, 3), Z(3, 5))), Z(3, 6));
    rows[4] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(4, 1), Z(4, 2)), _mm_or_si128(Z(4, 4), Z(4, 5))), Z(4, 6));
    rows[5] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(5, 2), Z(5, 4)), _mm_or_si128(Z(5, 5), Z(5, 6))), Z(5, 7));
    rows[6] = _mm_or_si128(_mm_or_si128(_mm_or_si128(Z(6, 2), Z(6, 4)), _mm_or_si128(Z(6, 5), Z(6, 6))), Z(6, 7));
    rows[7] = _mm_or_si128(_mm_or_si128(Z(7, 4), Z(7, 6)), Z(7, 7));
#undef Z
    idct_rows_avx2(rows, q, zz[0], out, pitch);
}
#undef ADD
#undef SUB
#endif

#if defined(__ARM_NEON) && !defined(__AVX2__)
#include <arm_neon.h>

// Same as the AVX2 version: 16-bit dequantisation (vmulq_s16), 16 x 16 -> 32-bit
// multiply-accumulates (vmull / vmlal with constants), the same 4 x 4 matrix for the odd part.
// 32-bit values as two halves: lo = lanes 0-3, hi = lanes 4-7.
typedef struct { int32x4_t l, h; } I32x8;
static inline I32x8 add8(I32x8 a, I32x8 b) { return (I32x8){vaddq_s32(a.l, b.l), vaddq_s32(a.h, b.h)}; }
static inline I32x8 sub8(I32x8 a, I32x8 b) { return (I32x8){vsubq_s32(a.l, b.l), vsubq_s32(a.h, b.h)}; }
// a * ka + b * kb
static inline I32x8 madd2(int16x8_t a, int16_t ka, int16x8_t b, int16_t kb) {
    return (I32x8){vmlal_n_s16(vmull_n_s16(vget_low_s16(a), ka), vget_low_s16(b), kb),
                   vmlal_high_n_s16(vmull_high_n_s16(a, ka), b, kb)};
}
// a * ka + b * kb + c * kc + d * kd
static inline I32x8 madd4(int16x8_t a, int16_t ka, int16x8_t b, int16_t kb, int16x8_t c, int16_t kc, int16x8_t d, int16_t kd) {
    I32x8 r = madd2(a, ka, b, kb);
    r.l = vmlal_n_s16(vmlal_n_s16(r.l, vget_low_s16(c), kc), vget_low_s16(d), kd);
    r.h = vmlal_high_n_s16(vmlal_high_n_s16(r.h, c, kc), d, kd);
    return r;
}

// One pass on 8 vectors (lanes = the other dimension): pass 1 descales by 11, pass 2 by 18,
// saturated to 16 bits.
static inline __attribute__((always_inline)) void islow16_pass_neon(int16x8_t *v, int pass2) {
    I32x8 t0 = {vshlq_n_s32(vaddl_s16(vget_low_s16(v[0]), vget_low_s16(v[4])), 13), vshlq_n_s32(vaddl_high_s16(v[0], v[4]), 13)};
    I32x8 t1 = {vshlq_n_s32(vsubl_s16(vget_low_s16(v[0]), vget_low_s16(v[4])), 13), vshlq_n_s32(vsubl_high_s16(v[0], v[4]), 13)};
    I32x8 t3 = madd2(v[2], FIX_0_541196100 + FIX_0_765366865, v[6], FIX_0_541196100);
    I32x8 t2 = madd2(v[2], FIX_0_541196100, v[6], FIX_0_541196100 - FIX_1_847759065);
    I32x8 e10 = add8(t0, t3), e13 = sub8(t0, t3), e11 = add8(t1, t2), e12 = sub8(t1, t2);
    I32x8 o0 = madd4(v[7], FIX_0_298631336 - FIX_0_899976223 - FIX_1_961570560 + FIX_1_175875602, v[5], FIX_1_175875602,
                     v[3], FIX_1_175875602 - FIX_1_961570560, v[1], FIX_1_175875602 - FIX_0_899976223);
    I32x8 o1 = madd4(v[7], FIX_1_175875602, v[5], FIX_2_053119869 - FIX_2_562915447 - FIX_0_390180644 + FIX_1_175875602,
                     v[3], FIX_1_175875602 - FIX_2_562915447, v[1], FIX_1_175875602 - FIX_0_390180644);
    I32x8 o2 = madd4(v[7], FIX_1_175875602 - FIX_1_961570560, v[5], FIX_1_175875602 - FIX_2_562915447,
                     v[3], FIX_3_072711026 - FIX_2_562915447 - FIX_1_961570560 + FIX_1_175875602, v[1], FIX_1_175875602);
    I32x8 o3 = madd4(v[7], FIX_1_175875602 - FIX_0_899976223, v[5], FIX_1_175875602 - FIX_0_390180644,
                     v[3], FIX_1_175875602, v[1], FIX_1_501321110 - FIX_0_899976223 - FIX_0_390180644 + FIX_1_175875602);
    I32x8 w[8] = {add8(e10, o3), add8(e11, o2), add8(e12, o1), add8(e13, o0),
                  sub8(e13, o0), sub8(e12, o1), sub8(e11, o2), sub8(e10, o3)};
    for (int y = 0; y < 8; y++)
        v[y] = pass2 ? vcombine_s16(vqmovn_s32(vrshrq_n_s32(w[y].l, 18)), vqmovn_s32(vrshrq_n_s32(w[y].h, 18)))
                     : vcombine_s16(vqrshrn_n_s32(w[y].l, 11), vqrshrn_n_s32(w[y].h, 11));
}

static inline __attribute__((always_inline)) void transpose8x16_neon(int16x8_t *v) {
    int16x8_t a[8];
    for (int i = 0; i < 8; i += 2) { a[i] = vtrn1q_s16(v[i], v[i + 1]); a[i + 1] = vtrn2q_s16(v[i], v[i + 1]); }
    int32x4_t b[8];
    for (int i = 0; i < 8; i += 4)
        for (int k = 0; k < 2; k++) {
            b[i + 2 * k] = vtrn1q_s32(vreinterpretq_s32_s16(a[i + k]), vreinterpretq_s32_s16(a[i + k + 2]));
            b[i + 2 * k + 1] = vtrn2q_s32(vreinterpretq_s32_s16(a[i + k]), vreinterpretq_s32_s16(a[i + k + 2]));
        }
    // b[0]: columns 0 / 4 of rows 0-3, b[1]: 2 / 6, b[2]: 1 / 5, b[3]: 3 / 7; b[4..7]: rows 4-7
    static const int col[4] = {0, 2, 1, 3};
    for (int k = 0; k < 4; k++) {
        int64x2_t x = vreinterpretq_s64_s32(b[k]), y = vreinterpretq_s64_s32(b[k + 4]);
        v[col[k]] = vreinterpretq_s16_s64(vtrn1q_s64(x, y));
        v[col[k] + 4] = vreinterpretq_s16_s64(vtrn2q_s64(x, y));
    }
}

// rows: the 8 coefficient rows (natural order, quantised); dc replaces rows[0][0].
static inline __attribute__((always_inline)) void idct_rows_neon(const int16x8_t *rows, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    int16x8_t v[8];
    for (int r = 0; r < 8; r++)
        v[r] = vmulq_s16(r ? rows[r] : vsetq_lane_s16((int16_t)dc, rows[0], 0), vreinterpretq_s16_u16(vld1q_u16(q + 8 * r)));
    islow16_pass_neon(v, 0);    // columns (lanes = columns): v[y] = row y
    transpose8x16_neon(v);
    islow16_pass_neon(v, 1);    // rows (lanes = rows): v[x] = column x
    transpose8x16_neon(v);
    for (int r = 0; r < 8; r++)   // +128, saturated
        vst1_u8(out + r * pitch, veor_u8(vreinterpret_u8_s8(vqmovn_s16(v[r])), vdup_n_u8(0x80)));
}

static void idct_islow_neon(const int16_t *in, const uint16_t *q, int dc, uint8_t *out, size_t pitch) {
    int16x8_t rows[8];
    for (int r = 0; r < 8; r++) rows[r] = vld1q_s16(in + 8 * r);
    idct_rows_neon(rows, q, dc, out, pitch);
}

// The same on a zigzag-ordered block: table lookups put the natural rows together, from
// the first 64 bytes, then (indices - 64, the others left as they are) the last 64.
static void idct_islow_neon_zz(const int16_t *zz, const uint16_t *q, uint8_t *out, size_t pitch) {
    uint8x16x4_t lo = vld1q_u8_x4((const uint8_t *)zz), hi = vld1q_u8_x4((const uint8_t *)zz + 64);
    int16x8_t rows[8];
    for (int r = 0; r < 8; r++) {
        uint8x16_t idx = vld1q_u8(g_zzidx[r]);
        rows[r] = vreinterpretq_s16_u8(vqtbx4q_u8(vqtbl4q_u8(lo, idx), hi, vsubq_u8(idx, vdupq_n_u8(64))));
    }
    idct_rows_neon(rows, q, zz[0], out, pitch);
}
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

// IDCT of a zigzag-ordered block.
static inline void idct_block_zz(const int16_t *zz, const uint16_t *q, uint8_t *out, size_t pitch) {
#ifdef __AVX2__
    if (!g_scalar_idct) { idct_islow_avx2_zz(zz, q, out, pitch); return; }
#elif defined(__ARM_NEON)
    if (!g_scalar_idct) { idct_islow_neon_zz(zz, q, out, pitch); return; }
#endif
    int16_t tmp[64];
    for (int k = 0; k < 64; k++) tmp[kNatural[k]] = zz[k];
    idct_islow_scalar(tmp, q, tmp[0], out, pitch);
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
    void *p = malloc(need);
    *cap = p ? need : 0;
    return p;
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

// Frees the buffers kept for reuse between images (they are taken again by the next decode).
void nj_free_buffers(void) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_n; i++) free(g_pool[i].p);
    g_pool_n = 0;
    pthread_mutex_unlock(&g_pool_mu);
}

static int pool_grow(void **p, size_t *cap, size_t need) {   // like realloc, keeps contents; -1: *p unchanged
    if (need <= *cap) return 0;
    size_t ncap = need + need / 2;
    void *np = realloc(*p, ncap);
    if (!np) return -1;
    *p = np;
    *cap = ncap;
    return 0;
}

// --- pipelined chunk processing (own engine, no restart markers) ---
typedef struct {
    uint64_t start, end;              // bit range of the clean stream
    Rec *rec; size_t rec_cap;         // MCU starts (bytes allocated)
    int16_t *coef; size_t coef_cap;   // coefficients per record
    size_t n;
    int nerr;
    uint32_t err[32], err_last;       // the first 32 records that failed, and the last one
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

// Returns -1 if out of memory.
static int chunk_decode(const Jpeg *j, const uint8_t *buf, Chunk *s, size_t max_mcus) {
    const size_t msz = (size_t)j->nblocks * 64;
    BR br;
    br_init(&br, buf, s->start);
    uint64_t pos = s->start;
    int32_t dc[3] = {0, 0, 0};
    size_t n = 0;
    while (pos < s->end && n < max_mcus) {
        if (pool_grow((void **)&s->rec, &s->rec_cap, (n + 2) * sizeof(Rec)) ||
            pool_grow((void **)&s->coef, &s->coef_cap, (n + 1) * msz * 2))
            return -1;
        Rec *r = &s->rec[n];
        r->pos = pos;
        memcpy(r->dc, dc, sizeof dc);
        if (decode_mcu_coef(j, &br, dc, s->coef + n * msz)) {
            if (s->nerr < 32) s->err[s->nerr] = (uint32_t)n;
            s->err_last = (uint32_t)n;
            s->nerr++;
        }
        n++;
        pos = br_pos(&br, buf);
    }
    if (pool_grow((void **)&s->rec, &s->rec_cap, (n + 2) * sizeof(Rec))) return -1;
    s->rec[n].pos = pos;
    memcpy(s->rec[n].dc, dc, sizeof dc);
    s->n = n;
    return 0;
}

// Did a record in [lo, hi) fail? (The garbage before a chunk synchronises may fail many times.)
static int chunk_err_in(const Chunk *s, size_t lo, size_t hi) {
    if (!s->nerr) return 0;
    if (s->err_last >= lo && s->err_last < hi) return 1;
    for (int i = 0; i < s->nerr && i < 32; i++)
        if (s->err[i] >= lo && s->err[i] < hi) return 1;
    if (s->nerr <= 33) return 0;
    // the ones not kept are somewhere between err[31] and err_last
    size_t a = (size_t)s->err[31] + 1 > lo ? (size_t)s->err[31] + 1 : lo, b = s->err_last < hi ? s->err_last : hi;
    return a < b;
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

static int nj_decode_prog(const Jpeg *j, const uint8_t *data, size_t len, const Output *o, nj_stats *st);

static int nj_decode_own(const uint8_t *data, size_t len, const Output *o, int nthreads, nj_stats *st) {
    Jpeg *j = malloc(sizeof *j);
    if (!j || parse(data, len, j) || !j->info.supported) { free(j); return -1; }
    if (j->progressive) {
        int r = nj_decode_prog(j, data, len, o, st);
        free(j);
        return r;
    }
    const nj_info *fi = &j->info;
    if (nthreads <= 0) nthreads = 4 * ncpu();
    const int R = fi->mcu_rows, mpr = fi->mcus_per_row;
    const size_t total = (size_t)R * mpr;
    const size_t msz = (size_t)j->nblocks * 64;   // coefficients per MCU
    int rc = -1, fail = 0, *const pfail = &fail;
    double t0 = now_ms();
    uint8_t *clean = NULL;
    size_t clean_cap = 0;

    // --- 1. scan (parallel): end of scan, stuffing, restart markers ---
    const size_t lo = j->scan_start;
    const int T = nthreads;
    ScanChunk *sc = calloc(T, sizeof *sc);
    if (!sc) { free(j); return -1; }
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
    if (!coff) goto out;
    size_t acc = 0;
    for (int t = 0; t < T; t++) {
        coff[t] = (sc[t].a < scan_end ? sc[t].a : scan_end) - lo - acc;
        if (t <= tend) acc += sc[t].removed;
    }
    const size_t clean_len = (scan_end - lo) - acc;
    clean = pool_take(clean_len + PAD_BYTES, &clean_cap);
    if (!clean) { free(coff); goto out; }
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
                    size_t *more = realloc(c->rstc, (cap ? cap * 2 : 64) * sizeof *c->rstc);
                    if (!more) { *pfail = 1; return; }
                    c->rstc = more;
                    cap = cap ? cap * 2 : 64;
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
    if (fail) goto out;
    st->scan_ms = now_ms() - t0;
    const uint64_t Lbits = (uint64_t)clean_len * 8;

    if (fi->restart_interval) {
        // --- restart intervals: decode + IDCT fused, in parallel, no big buffers ---
        const size_t Ri = (size_t)fi->restart_interval;
        const size_t I = (total + Ri - 1) / Ri;
        size_t nr = 0;
        for (int t = 0; t <= tend; t++) nr += sc[t].nrstc;
        size_t *istart = malloc((nr + 2) * sizeof *istart);
        if (!istart) goto out;
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
        if (!ch) goto out;
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
                if (!__atomic_load_n(&Sp->fail, __ATOMIC_RELAXED) &&
                    (!c->rec || !c->coef || chunk_decode(j, clean, c, total + 1)))   // (out of memory: fails)
                    __atomic_store_n(&Sp->fail, 1, __ATOMIC_RELAXED);
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
    for (int t = 0; t < T; t++) free(sc[t].rstc);
    free(sc);
    pool_give(clean, clean_cap);
    free(j);
    return rc;
}

// ===========================================================================
// Progressive JPEG (SOF2). The image comes in many scans, each its own Huffman-coded
// segment: DC first / refinement scans, and AC bands of one component, first and
// refinement. A scan is sequential, but its MCU row only needs the same rows of the
// earlier scans that touch the same coefficients (same component, overlapping band).
// So the scans run as a pipeline (a wavefront): every scan advances by a few MCU rows
// as soon as the scans it follows are far enough ahead, and the IDCT follows the last
// ones. The time is bounded by the longest scan instead of the sum of all scans.
// The coefficients of the whole image stay in memory (zigzag order, like the scans).

#define PMAX_SCANS 64

typedef struct {
    int ncomp, comp[3], Ss, Se, Ah, Al, ri;
    const HuffTable *dct[3], *act;
    size_t raw0, raw1;                  // entropy-coded bytes in the file
    uint64_t deps;                      // earlier scans it has to follow
    uint8_t *clean;                     // unstuffed, RST markers dropped
    size_t clean_len, clean_cap;
    size_t *rst, nrst;                  // clean offsets where restart intervals 1.. start
    // decoding state between the chunks
    uint64_t pos;
    unsigned eobrun;
    int32_t dc[3];
    size_t mcu;                         // MCUs (one-component scan: blocks) done
    int done, busy;                     // MCU rows finished; a thread works on it
    int ready;                          // 0: not unstuffed yet, 1: being unstuffed, 2: ready
} PScan;

typedef struct {
    const Jpeg *j;
    const Output *o;
    const uint8_t *data;
    size_t len;
    int nscan, ntabs;
    PScan s[PMAX_SCANS];
    HuffTable *tabs[4 * PMAX_SCANS];    // tables defined between the scans
    int16_t *coef[3];                   // coefficient blocks per component, MCU-padded:
    size_t bw[3], bh[3];                //   bw x bh blocks of 64
    size_t cbw[3], cbh[3];              // blocks a one-component scan covers
    void *cbuf;
    size_t cbuf_cap;
    int R, K;                           // MCU rows, rows per chunk
    int clear_next, cleared;            // rows given out / cleared (all before it)
    uint8_t *cleared_chunk;             // chunks of K rows cleared
    int idct_next, idct_done, fail;
    int nwait;                          // workers sleeping on cv
    pthread_mutex_t mu;
    pthread_cond_t cv;
} Prog;

// All scans and the tables in between. Fails on anything unusual (the caller falls back).
static int prog_layout(Prog *P) {
    const Jpeg *j = P->j;
    const nj_info *fi = &j->info;
    const uint8_t *d = P->data;
    size_t len = P->len, pos = j->sos_pos;
    const HuffTable *dc[4], *ac[4];
    for (int i = 0; i < 4; i++) { dc[i] = j->dc[i].present ? &j->dc[i] : NULL; ac[i] = j->ac[i].present ? &j->ac[i] : NULL; }
    int ri = fi->restart_interval;
    for (;;) {
        while (pos < len && d[pos] == 0xFF) pos++;
        if (pos + 2 >= len) break;      // no EOI
        unsigned m = d[pos++];
        if (m == 0xD9) break;
        if (m >= 0xD0 && m <= 0xD7) continue;
        unsigned L = be16(d + pos);
        if (L < 2 || pos + L > len) return -1;
        const uint8_t *seg = d + pos + 2;
        size_t sl = L - 2;
        if (m == 0xC4) {                // DHT
            size_t q = 0;
            while (q + 17 <= sl) {
                int tc = seg[q] >> 4, th = seg[q] & 15, n = 0;
                uint8_t b[17] = {0};
                for (int l = 1; l <= 16; l++) { b[l] = seg[q + l]; n += b[l]; }
                if (th > 3 || tc > 1 || q + 17 + n > sl) return -1;
                if (P->ntabs == (int)(sizeof P->tabs / sizeof *P->tabs)) return -1;
                HuffTable *t = malloc(sizeof *t);
                P->tabs[P->ntabs++] = t;
                if (!t || build_huff(t, b, seg + q + 17, n)) return -1;
                if (tc) ac[th] = t; else dc[th] = t;
                q += 17 + n;
            }
        } else if (m == 0xDD) {         // DRI
            if (sl < 2) return -1;
            ri = (int)be16(seg);
        } else if (m == 0xDA) {         // SOS
            if (P->nscan == PMAX_SCANS || sl < 1) return -1;
            PScan *s = &P->s[P->nscan];
            int ns = seg[0];
            if (ns < 1 || ns > fi->ncomp || sl < 1 + 2 * (size_t)ns + 3) return -1;
            s->ncomp = ns;
            s->Ss = seg[1 + 2 * ns]; s->Se = seg[2 + 2 * ns];
            s->Ah = seg[3 + 2 * ns] >> 4; s->Al = seg[3 + 2 * ns] & 15;
            s->ri = ri;
            if (s->Ss == 0 ? s->Se != 0 : (s->Se < s->Ss || s->Se > 63 || ns != 1)) return -1;
            if (s->Al > 13 || (s->Ah && s->Al != s->Ah - 1)) return -1;
            int nb = 0;
            for (int i = 0; i < ns; i++) {
                int c;
                for (c = 0; c < fi->ncomp && j->comp_id[c] != seg[1 + 2 * i]; c++) {}
                if (c == fi->ncomp) return -1;
                for (int k = 0; k < i; k++) if (s->comp[k] == c) return -1;
                s->comp[i] = c;
                s->dct[i] = dc[seg[2 + 2 * i] >> 4 & 3];
                if (s->Ss == 0 && !s->Ah && (!s->dct[i] || seg[2 + 2 * i] >> 4 > 3)) return -1;
                if (s->Ss) { s->act = ac[seg[2 + 2 * i] & 3]; if (!s->act || (seg[2 + 2 * i] & 15) > 3) return -1; }
                nb += ns > 1 ? fi->h[c] * fi->v[c] : 1;
                if (!j->qpresent[j->cq[c]]) return -1;
            }
            if (nb > MAX_BLOCKS) return -1;
            // earlier scans of the same coefficients; an AC refinement reads the whole block
            // (the nonzero mask): it follows / is followed by any scan of its component
            for (int t = 0; t < P->nscan; t++) {
                const PScan *e = &P->s[t];
                int share = 0;
                for (int a = 0; a < e->ncomp; a++) for (int b = 0; b < ns; b++) share |= e->comp[a] == s->comp[b];
                int refine = (s->Ss && s->Ah) || (e->Ss && e->Ah);
                if (share && (refine || (e->Ss <= s->Se && s->Ss <= e->Se))) s->deps |= 1ULL << t;
            }
            // the entropy-coded data: up to the first marker other than RST
            size_t p = pos + L, end = len;
            while (p + 1 < len) {
                const uint8_t *f = memchr(d + p, 0xFF, len - 1 - p);
                if (!f) break;
                size_t i = (size_t)(f - d);
                uint8_t nb2 = d[i + 1];
                if (nb2 == 0 || (nb2 >= 0xD0 && nb2 <= 0xD7)) p = i + 2;
                else if (nb2 == 0xFF) p = i + 1;
                else { end = i; break; }
            }
            s->raw0 = pos + L;
            s->raw1 = end;
            P->nscan++;
            pos = end;
            continue;
        } else if (m == 0xDB || (m >= 0xC0 && m <= 0xCF)) {
            return -1;                  // new quantisation tables or frame: not here
        }
        pos += L;
    }
    // Files without the last scans (low-quality previews) are decoded as they are: libjpeg
    // would smooth those blocks (an estimate of the missing low AC coefficients from the
    // neighbours' DC), we don't
    return P->nscan ? 0 : -1;
}

// Unstuffs a scan's data (0xFF 0x00 -> 0xFF, fill bytes and RST markers dropped). -1: out of memory.
static int prog_unstuff(PScan *s, const uint8_t *d) {
    size_t n = s->raw1 - s->raw0, i = s->raw0, cap = 0;
    s->clean = pool_take(n + PAD_BYTES, &s->clean_cap);
    if (!s->clean) return -1;
    uint8_t *out = s->clean;
    while (i < s->raw1) {
        const uint8_t *f = memchr(d + i, 0xFF, s->raw1 - i);
        size_t k = f ? (size_t)(f - d) - i : s->raw1 - i;
        memcpy(out, d + i, k);
        out += k;
        i += k;
        if (!f) break;
        while (i < s->raw1 && d[i] == 0xFF) i++;
        if (i >= s->raw1) break;
        uint8_t nb = d[i++];
        if (nb == 0) {
            *out++ = 0xFF;
        } else {                        // RST (the scan ends at any other marker)
            if (s->nrst == cap) {
                size_t *more = realloc(s->rst, (cap ? 2 * cap : 64) * sizeof *s->rst);
                if (!more) return -1;
                s->rst = more;
                cap = cap ? 2 * cap : 64;
            }
            s->rst[s->nrst++] = (size_t)(out - s->clean);
        }
    }
    s->clean_len = (size_t)(out - s->clean);
    memset(out, 0, PAD_BYTES);
    return 0;
}

static inline unsigned br_bits(BR *br, int n) {   // n <= 16 bits, n may be 0
    if (br->cnt < 16) br_refill(br);
    unsigned v = n ? (unsigned)(br->buf >> (64 - n)) : 0;
    br_skip(br, n);
    return v;
}
static inline unsigned br_bit(BR *br) {
    if (__builtin_expect(br->cnt == 0, 0)) br_refill(br);
    unsigned v = (unsigned)(br->buf >> 63);
    br_skip(br, 1);
    return v;
}
static inline int prog_huff(const HuffTable *t, BR *br, int *err) {
    br_refill(br);
    uint64_t w = br->buf;
    unsigned e = t->look[w >> (64 - LOOK)];
    int l, sym;
    if (__builtin_expect(e != 0, 1)) { l = e >> 8; sym = e & 0xFF; }
    else if ((l = huff_slow_chk(t, w, &sym)) < 0) { *err = 1; l = 16; }
    br_skip(br, l);
    return sym;
}
static inline int16_t shl16(int v, int n) { return (int16_t)(uint16_t)((unsigned)v << n); }

// One block of a one-component AC scan (zigzag coefficients), as libjpeg's jdphuff.c.
static inline int prog_ac_first(PScan *s, BR *br, int16_t *blk) {
    if (s->eobrun) { s->eobrun--; return 0; }
    const HuffTable *t = s->act;
    const int Se = s->Se, Al = s->Al;
    for (int k = s->Ss; k <= Se; k++) {
        br_refill(br);
        uint64_t w = br->buf;
        int32_t f = t->acfast[w >> (64 - LOOK)];
        int kinc = (f >> 8) & 0xFF;
        if (__builtin_expect(f != 0 && kinc != 0, 1)) {   // code + value in one lookup
            br_skip(br, f & 0xFF);
            if (kinc == 0x40) { k += 15; continue; }
            k += kinc - 1;
            if (k > Se) return 1;
            blk[k] = shl16(f >> 16, Al);
            continue;
        }
        unsigned e = t->look[w >> (64 - LOOK)];
        int l, sym;
        if (e) { l = e >> 8; sym = e & 0xFF; }
        else if ((l = huff_slow_chk(t, w, &sym)) < 0) return 1;
        int r = sym >> 4, n = sym & 15;
        if (n) {
            k += r;
            if (k > Se) return 1;
            blk[k] = shl16(extend((unsigned)((w << l) >> (64 - n)), n), Al);
            br_skip(br, l + n);
        } else if (r == 15) {
            br_skip(br, l);
            k += 15;
        } else {                        // EOBn: this block and 2^r + bits - 1 more end here
            s->eobrun = (1u << r) - 1 + (r ? (unsigned)((w << l) >> (64 - r)) : 0);
            br_skip(br, l + r);
            break;
        }
    }
    return 0;
}

// Bit k set: zigzag coefficient k of the block is nonzero.
static inline uint64_t nz_mask(const int16_t *blk) {
#ifdef __AVX2__
    const __m256i z = _mm256_setzero_si256();
    __m256i ab = _mm256_packs_epi16(_mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)blk), z),
                                    _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(blk + 16)), z));
    __m256i cd = _mm256_packs_epi16(_mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(blk + 32)), z),
                                    _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(blk + 48)), z));
    uint32_t lo = (uint32_t)_mm256_movemask_epi8(_mm256_permute4x64_epi64(ab, 0xD8));
    uint32_t hi = (uint32_t)_mm256_movemask_epi8(_mm256_permute4x64_epi64(cd, 0xD8));
    return ~((uint64_t)hi << 32 | lo);
#elif defined(__ARM_NEON)
    static const uint8_t wts[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t w = vld1q_u8(wts);
    uint8x16_t t[4];
    for (int i = 0; i < 4; i++) {
        int16x8_t a = vld1q_s16(blk + 16 * i), b = vld1q_s16(blk + 16 * i + 8);
        t[i] = vandq_u8(vcombine_u8(vmovn_u16(vtstq_s16(a, a)), vmovn_u16(vtstq_s16(b, b))), w);
    }
    uint8x16_t r = vpaddq_u8(vpaddq_u8(t[0], t[1]), vpaddq_u8(t[2], t[3]));
    return vgetq_lane_u64(vreinterpretq_u64_u8(vpaddq_u8(r, r)), 0);
#else
    uint64_t m = 0;
    for (int k = 0; k < 64; k++) m |= (uint64_t)(blk[k] != 0) << k;
    return m;
#endif
}

// Set bit positions of every byte value (8 bytes each, the unused ones 0).
static uint64_t g_bitpos[256];
static pthread_once_t g_bitpos_once = PTHREAD_ONCE_INIT;
static void bitpos_init(void) {
    zz_tables_init();
    for (int x = 0; x < 256; x++)
        for (int b = 0, n = 0; b < 8; b++)
            if (x >> b & 1) g_bitpos[x] |= (uint64_t)b << (8 * n++);
}

// The positions of the set bits of x, in order, to out[0..]; returns their number.
// Writes up to 8 bytes past the end.
static inline int bit_list(uint64_t x, uint8_t *out) {
    int n = 0;
    for (int b = 0; b < 8; b++, x >>= 8) {
        uint64_t p = g_bitpos[x & 0xFF] + 0x0808080808080808ULL * b;
        memcpy(out + n, &p, 8);
        n += __builtin_popcount((unsigned)(x & 0xFF));
    }
    return n;
}

// Correction bits for the cnt nonzero coefficients in 'm' (in order): a 1 adds p1 to the
// magnitude (if that bit is not set yet).
static inline void prog_correct(BR *br, int16_t *blk, uint64_t m, int cnt, int p1) {
    if (__builtin_expect(cnt <= (int)br->cnt, 1)) {   // the usual: the bits are in the buffer
        uint64_t b = br->buf;
        br_skip(br, cnt);
        for (; m; m &= m - 1, b <<= 1) {
            int k = __builtin_ctzll(m), c = blk[k], add = (int)(b >> 63) & !(c & p1);
            blk[k] = (int16_t)(c + ((c >> 31 | 1) * p1 & -add));
        }
        return;
    }
    while (m) {
        br_refill(br);
        int n = __builtin_popcountll(m);
        if (n > 48) n = 48;
        uint64_t b = br->buf;
        br_skip(br, n);
        for (int i = 0; i < n; i++, b <<= 1) {
            int k = __builtin_ctzll(m);
            m &= m - 1;
            int c = blk[k], add = (int)(b >> 63) & !(c & p1);
            blk[k] = (int16_t)(c + ((c >> 31 | 1) * p1 & -add));
        }
    }
}

// One block of an AC refinement scan. libjpeg walks the band one coefficient at a time,
// reading a correction bit for every nonzero one it passes and counting the zero ones for
// the run. Here the zero coefficients of the block are listed first: a run of r ends at
// the (r+1)-th next zero, and the nonzero ones before it get the correction bits.
static inline int prog_ac_refine(PScan *s, BR *br, int16_t *blk) {
    const int Se = s->Se, p1 = 1 << s->Al;
    const uint64_t band = (~0ULL >> (63 - Se)) & (~0ULL << s->Ss);
    const uint64_t nz = nz_mask(blk) & band;
    int k = s->Ss;
    if (!s->eobrun) {
        uint8_t zpos[64 + 8 + 16];
        int nzero = bit_list(~nz & band, zpos), zi = 0;
        memset(zpos + nzero, Se + 1, 16);   // a run past the band's last zero ends the band
        const HuffTable *t = s->act;
        while (k <= Se) {
            br_refill(br);
            uint64_t w = br->buf;
            unsigned e = t->look[w >> (64 - LOOK)];
            int l, sym;
            if (__builtin_expect(e != 0, 1)) { l = e >> 8; sym = e & 0xFF; }
            else if ((l = huff_slow_chk(t, w, &sym)) < 0) return 1;
            int r = sym >> 4, n = sym & 15;
            if (!n && r != 15) {            // EOBn: this block and 2^r + bits - 1 more
                s->eobrun = (1u << r) + (r ? (unsigned)((w << l) >> (64 - r)) : 0);
                br_skip(br, l + r);
                break;
            }
            if (n > 1) return 1;
            int v = n ? ((w << l) >> 63 ? p1 : -p1) : 0;
            br_skip(br, l + n);
            // the (r+1)-th zero: the nonzero ones before it are all positions minus r zeros
            // (past the last zero: minus the zeros that were left)
            int z = zpos[zi + r], left = nzero - zi;
            prog_correct(br, blk, nz & (~0ULL << k) & (z > Se ? ~0ULL : (1ULL << z) - 1), z - k - (r < left ? r : left), p1);
            if (v) {
                if (z > Se) return 1;
                blk[z] = (int16_t)v;
            }
            zi += r + 1;
            k = z + 1;
        }
    }
    if (s->eobrun) {                    // in an EOB run: only the corrections
        if (k <= Se) {
            uint64_t m = nz & (~0ULL << k);
            prog_correct(br, blk, m, __builtin_popcountll(m), p1);
        }
        s->eobrun--;
    }
    return 0;
}

// Restart before MCU s->mcu if one is due. Returns nonzero on damaged data.
static inline int prog_restart(PScan *s, BR *br) {
    if (!s->ri || !s->mcu || s->mcu % (size_t)s->ri) return 0;
    size_t k = s->mcu / (size_t)s->ri;
    if (k > s->nrst || br_pos(br, s->clean) > (uint64_t)s->rst[k - 1] * 8) return 1;
    br_init(br, s->clean, (uint64_t)s->rst[k - 1] * 8);
    s->eobrun = 0;
    s->dc[0] = s->dc[1] = s->dc[2] = 0;
    return 0;
}

// Decodes MCU rows [r0, r1) of a scan.
static int prog_scan_rows(Prog *P, PScan *s, int r0, int r1) {
    const nj_info *fi = &P->j->info;
    BR br;
    br_init(&br, s->clean, s->pos);
    const uint8_t *lim = s->clean + s->clean_len + PAD_BYTES / 2;   // far past the data: damaged
    const int Al = s->Al;
    int err = 0;
    if (s->ncomp > 1) {                 // interleaved: DC only
        for (int my = r0; my < r1 && !err; my++)
            for (int mx = 0; mx < fi->mcus_per_row && !err; mx++) {
                err |= prog_restart(s, &br);
                for (int i = 0; i < s->ncomp; i++) {
                    int c = s->comp[i], h = fi->h[c], v = fi->v[c];
                    for (int by = 0; by < v; by++)
                        for (int bx = 0; bx < h; bx++) {
                            int16_t *blk = P->coef[c] + (((size_t)my * v + by) * P->bw[c] + (size_t)mx * h + bx) * 64;
                            if (!s->Ah) {
                                int n = prog_huff(s->dct[i], &br, &err) & 15;
                                s->dc[i] += n ? extend(br_bits(&br, n), n) : 0;
                                blk[0] = shl16(s->dc[i], Al);
                            } else if (br_bit(&br)) {
                                blk[0] |= (int16_t)(1 << Al);
                            }
                        }
                }
                s->mcu++;
                if (br.p > lim) err = 1;
            }
    } else {
        const int c = s->comp[0], v = fi->v[c];
        const size_t y1 = (size_t)r1 * v < P->cbh[c] ? (size_t)r1 * v : P->cbh[c];
        const int kind = s->Ss == 0 ? (s->Ah ? 1 : 0) : (s->Ah ? 3 : 2);
        for (size_t by = (size_t)r0 * v; by < y1 && !err; by++) {
            int16_t *row = P->coef[c] + by * P->bw[c] * 64;
            for (size_t bx = 0; bx < P->cbw[c]; bx++) {
                err |= prog_restart(s, &br);
                int16_t *blk = row + bx * 64;
                switch (kind) {
                case 0: {
                    int n = prog_huff(s->dct[0], &br, &err) & 15;
                    s->dc[0] += n ? extend(br_bits(&br, n), n) : 0;
                    blk[0] = shl16(s->dc[0], Al);
                    break;
                }
                case 1: if (br_bit(&br)) blk[0] |= (int16_t)(1 << Al); break;
                case 2: err |= prog_ac_first(s, &br, blk); break;
                default: err |= prog_ac_refine(s, &br, blk); break;
                }
                s->mcu++;
                if (br.p > lim) { err = 1; break; }
            }
        }
    }
    s->pos = br_pos(&br, s->clean);
    if (r1 == P->R && s->pos > (uint64_t)s->clean_len * 8) err = 1;   // ran out of data
    return err;
}

// IDCT of MCU rows [r0, r1).
static void prog_idct_rows(Prog *P, int r0, int r1) {
    const Jpeg *j = P->j;
    const nj_info *fi = &j->info;
    for (int c = 0; c < fi->ncomp; c++) {
        const uint16_t *q = j->qt[j->cq[c]];
        uint8_t *plane = P->o->planes[c];
        size_t pitch = P->o->pitch[c];
        for (size_t by = (size_t)r0 * fi->v[c]; by < (size_t)r1 * fi->v[c]; by++) {
            const int16_t *blk = P->coef[c] + by * P->bw[c] * 64;
            for (size_t bx = 0; bx < P->bw[c]; bx++, blk += 64)
                idct_block_zz(blk, q, plane + by * 8 * pitch + bx * 8, pitch);
        }
    }
}

// Clears the coefficients of MCU rows [r0, r1).
static void prog_clear_rows(Prog *P, int r0, int r1) {
    const nj_info *fi = &P->j->info;
    for (int c = 0; c < fi->ncomp; c++) {
        size_t row = P->bw[c] * 64 * fi->v[c];
        memset(P->coef[c] + (size_t)r0 * row, 0, (size_t)(r1 - r0) * row * sizeof(int16_t));
    }
}

// A worker: takes the next piece of work until everything is done. In this order: a chunk
// of a scan whose predecessors are far enough (the most lagging scan first), unstuffing a
// scan, clearing rows, IDCT of rows that all scans have finished. Idle workers sleep; each
// one that takes or finishes work wakes one more (waking all of them each time made 16
// threads slower than 4: they all queued for the mutex).
static void prog_worker(Prog *P) {
    pthread_mutex_lock(&P->mu);
    while (!P->fail && P->idct_done < P->R) {
        int all = P->R, best = -1, unstuff = -1, r0 = 0, r1 = 0;
        for (int i = 0; i < P->nscan; i++) {
            PScan *s = &P->s[i];
            if (s->done < all) all = s->done;
            if (s->ready == 0 && unstuff < 0) unstuff = i;
            if (s->busy || s->ready != 2 || s->done == P->R) continue;
            int avail = P->cleared;
            for (uint64_t m = s->deps; m; m &= m - 1) {
                int d = P->s[__builtin_ctzll(m)].done;
                if (d < avail) avail = d;
            }
            if (avail > s->done && (best < 0 || s->done <= P->s[best].done)) {
                best = i;
                r0 = s->done;
                r1 = avail < r0 + P->K ? avail : r0 + P->K;
            }
        }
        if (best >= 0) {
            PScan *s = &P->s[best];
            s->busy = 1;
            if (P->nwait) pthread_cond_signal(&P->cv);   // more may be there: pass it on
            pthread_mutex_unlock(&P->mu);
            int err = prog_scan_rows(P, s, r0, r1);
            pthread_mutex_lock(&P->mu);
            s->busy = 0;
            s->done = r1;
            if (err) P->fail = 1;
        } else if (unstuff >= 0) {
            PScan *s = &P->s[unstuff];
            s->ready = 1;
            if (P->nwait) pthread_cond_signal(&P->cv);   // more may be there: pass it on
            pthread_mutex_unlock(&P->mu);
            int err = prog_unstuff(s, P->data);
            pthread_mutex_lock(&P->mu);
            s->ready = 2;
            if (err) P->fail = 1;
        } else if (P->clear_next < P->R) {
            int c = P->clear_next / P->K;
            r0 = P->clear_next;
            r1 = r0 + P->K < P->R ? r0 + P->K : P->R;
            P->clear_next = r1;
            if (P->nwait) pthread_cond_signal(&P->cv);   // more may be there: pass it on
            pthread_mutex_unlock(&P->mu);
            prog_clear_rows(P, r0, r1);
            pthread_mutex_lock(&P->mu);
            P->cleared_chunk[c] = 1;
            while (P->cleared < P->R && P->cleared_chunk[P->cleared / P->K]) P->cleared += P->K;
            if (P->cleared > P->R) P->cleared = P->R;
        } else if (P->idct_next < all) {
            r0 = P->idct_next;
            r1 = all < r0 + P->K ? all : r0 + P->K;
            P->idct_next = r1;
            if (P->nwait) pthread_cond_signal(&P->cv);   // more may be there: pass it on
            pthread_mutex_unlock(&P->mu);
            prog_idct_rows(P, r0, r1);
            pthread_mutex_lock(&P->mu);
            P->idct_done += r1 - r0;
        } else {
            P->nwait++;
            pthread_cond_wait(&P->cv, &P->mu);
            P->nwait--;
            continue;
        }
        if (P->nwait) pthread_cond_signal(&P->cv);   // new work may have become free
    }
    pthread_cond_broadcast(&P->cv);
    pthread_mutex_unlock(&P->mu);
}

static int nj_decode_prog(const Jpeg *j, const uint8_t *data, size_t len, const Output *o, nj_stats *st) {
    const nj_info *fi = &j->info;
    double t0 = now_ms();
    Prog *P = calloc(1, sizeof *P);
    if (!P) return -1;
    P->j = j; P->o = o; P->data = data; P->len = len;
    P->R = fi->mcu_rows;
    pthread_once(&g_bitpos_once, bitpos_init);
    int rc = -1;
    if (prog_layout(P)) goto out;
    size_t total = 0;
    for (int c = 0; c < fi->ncomp; c++) {
        P->bw[c] = (size_t)fi->mcus_per_row * fi->h[c];
        P->bh[c] = (size_t)P->R * fi->v[c];
        size_t cw = ((size_t)fi->width * fi->h[c] + fi->hmax - 1) / fi->hmax;
        size_t chh = ((size_t)fi->height * fi->v[c] + fi->vmax - 1) / fi->vmax;
        P->cbw[c] = (cw + 7) / 8;
        P->cbh[c] = (chh + 7) / 8;
        total += P->bw[c] * P->bh[c] * 64;
    }
    P->cbuf = pool_take(total * sizeof(int16_t), &P->cbuf_cap);
    P->K = (P->R + 47) / 48;
    P->cleared_chunk = calloc((size_t)(P->R + P->K - 1) / P->K, 1);
    if (!P->cbuf || !P->cleared_chunk) goto out;
    for (int c = 0; c < fi->ncomp; c++) P->coef[c] = c ? P->coef[c - 1] + P->bw[c - 1] * P->bh[c - 1] * 64 : (int16_t *)P->cbuf;
    st->scan_ms = now_ms() - t0;
    double t1 = now_ms();
    pthread_mutex_init(&P->mu, NULL);
    pthread_cond_init(&P->cv, NULL);
    // 4 workers unless set (nj_set_max_workers): the longest scan is about a quarter to a
    // third of the work, so more can't be kept busy; they only cost (i9 and M1: 7% faster
    // than all cores, small web images 30%; 24 MP the same from 4 to 16)
    const int W = t_workers > 0 ? t_workers : g_max_workers > 0 ? g_max_workers : ncpu() < 4 ? ncpu() : 4;
    Prog *const p = P;
    parallel_for((size_t)W, ^(size_t w) { (void)w; prog_worker(p); });
    pthread_cond_destroy(&P->cv);
    pthread_mutex_destroy(&P->mu);
    st->decode_ms = now_ms() - t1;
    st->bands = P->nscan;
    st->mode = 3;
    rc = P->fail ? -1 : 0;
out:
    for (int i = 0; i < P->nscan; i++) { pool_give(P->s[i].clean, P->s[i].clean_cap); free(P->s[i].rst); }
    for (int i = 0; i < P->ntabs; i++) free(P->tabs[i]);
    pool_give(P->cbuf, P->cbuf_cap);
    free(P->cleared_chunk);
    free(P);
    return rc;
}

int nj_decode_planes(const uint8_t *data, size_t len, const nj_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads, nj_stats *stats) {
    Output o = {planes, pitch, NULL, 0};
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
    Output o = {NULL, NULL, dst, pitch};
    return nj_decode(data, len, &o, nthreads, stats);
}
#endif
