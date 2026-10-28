// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitropsd: multi-threaded PSD/PSB merged-image decoding. See nitropsd.h.
#include "nitropsd.h"
#include "nitropng.h"   // np_zlib_decompress

#include "nitro_os.h"   // parallel loop, clock, CPU count
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static inline uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

int ps_read_info(const uint8_t *d, size_t len, ps_info *fi) {
    memset(fi, 0, sizeof *fi);
    if (len < 26 + 4 || memcmp(d, "8BPS", 4)) return -1;
    fi->version = (int)be16(d + 4);
    if (fi->version != 1 && fi->version != 2) return -1;
    fi->channels = (int)be16(d + 12);
    fi->height = (int)be32(d + 14);
    fi->width = (int)be32(d + 18);
    fi->depth = (int)be16(d + 22);
    fi->mode = (int)be16(d + 24);
    int psb = fi->version == 2;
    int maxdim = psb ? 300000 : 30000;
    if (fi->channels < 1 || fi->channels > 56 || fi->width < 1 || fi->height < 1 || fi->width > maxdim || fi->height > maxdim)
        return -1;
    size_t pos = 26;
    // colour mode data, image resources
    for (int k = 0; k < 2; k++) {
        if (pos + 4 > len) return -1;
        uint32_t l = be32(d + pos);
        if (l > len - pos - 4) return -1;
        pos += 4 + (size_t)l;
    }
    // layer and mask information: only the layer count's sign matters (negative:
    // the first alpha channel is the transparency of the merged image)
    size_t ls = psb ? 8 : 4;
    if (pos + ls > len) return -1;
    uint64_t lm = psb ? be64(d + pos) : be32(d + pos);
    if (lm > len - pos - ls) return -1;
    int layer_count = 0;
    if (lm >= ls + 2) {
        uint64_t li = psb ? be64(d + pos + ls) : be32(d + pos + ls);
        if (li >= 2) layer_count = (int16_t)be16(d + pos + 2 * ls);
    }
    pos += ls + (size_t)lm;
    if (pos + 2 > len) return -1;
    fi->compression = (int)be16(d + pos);
    fi->data_off = pos + 2;
    fi->ncolor = fi->mode == 3 ? 3 : fi->mode == 1 ? 1 : 0;
    fi->alpha = layer_count < 0 && fi->channels > fi->ncolor;
    fi->plane_size = (size_t)fi->width * fi->height;
    fi->supported = fi->depth == 8 && fi->ncolor && fi->channels >= fi->ncolor && fi->compression <= 3;
    return 0;
}

// PackBits row: decodes exactly n bytes from [src, end); 0 if the row is valid
// (fully used, exact length).
static inline int unpackbits(const uint8_t *src, const uint8_t *end, uint8_t *dst, size_t n) {
    size_t o = 0;
    while (src < end) {
        int c = *src++;
        if (c < 128) {                       // c + 1 literal bytes
            size_t k = (size_t)c + 1;
            if (o + k > n || src + k > end) return -1;
            memcpy(dst + o, src, k);
            src += k;
            o += k;
        } else if (c > 128) {                // 257 - c repeats
            size_t k = 257 - (size_t)c;
            if (o + k > n || src >= end) return -1;
            memset(dst + o, *src++, k);
            o += k;
        }                                    // 128: no-op
    }
    return o == n ? 0 : -1;
}

int ps_decode(const uint8_t *d, size_t len, const ps_info *fi, uint8_t *out, int nthreads, ps_stats *st) {
    ps_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    if (!fi->supported) return -1;
    double t0 = nitro_now_ms();
    const int H = fi->height, P = fi->ncolor + fi->alpha;   // planes we need
    const size_t W = (size_t)fi->width, rows = (size_t)P * H;
    const uint8_t *data = d + fi->data_off;
    const size_t avail = len - fi->data_off;
    const size_t tasks = (size_t)(nthreads > 0 ? nthreads : nitro_ncpu()) * 8;   // row ranges, for balance
    const size_t nt = tasks < rows ? tasks : rows;
    volatile int bad = 0, *const pbad = &bad;
    st->mode = fi->compression;
    if (fi->compression == 0) {
        if (fi->plane_size * P > avail) return -1;
        nitro_parallel(nt, ^(size_t t) {   // planes are contiguous in the file
            size_t a = fi->plane_size * P * t / nt, b = fi->plane_size * P * (t + 1) / nt;
            memcpy(out + a, data + a, b - a);
        });
    } else if (fi->compression == 1) {
        // row length table: channels * height entries (2 bytes, 4 in PSB); prefix sums give offsets
        const size_t cs = fi->version == 2 ? 4 : 2, all = (size_t)fi->channels * H;
        if (all * cs > avail) return -1;
        size_t *off = malloc((rows + 1) * sizeof *off);
        if (!off) return -1;
        size_t o = all * cs;
        for (size_t r = 0; r < rows; r++) {
            off[r] = o;
            o += cs == 4 ? be32(data + r * 4) : be16(data + r * 2);
        }
        off[rows] = o;
        if (o > avail) { free(off); return -1; }
        nitro_parallel(nt, ^(size_t t) {
            size_t r0 = rows * t / nt, r1 = rows * (t + 1) / nt;
            for (size_t r = r0; r < r1 && !*pbad; r++)
                if (unpackbits(data + off[r], data + off[r + 1], out + r * W, W)) *pbad = 1;
        });
        free(off);
    } else {
        // ZIP: one zlib stream of all channels (we need the first P planes of it)
        size_t total = (size_t)fi->channels * fi->plane_size;
        uint8_t *buf = P == fi->channels ? out : malloc(total);
        if (!buf) return -1;
        if (np_zlib_decompress(data, avail, buf, total, nthreads)) bad = 1;
        if (!bad && fi->compression == 3)   // prediction: each row stores differences of neighbours
            nitro_parallel(nt, ^(size_t t) {
                size_t r0 = rows * t / nt, r1 = rows * (t + 1) / nt;
                for (size_t r = r0; r < r1; r++) {
                    uint8_t *row = buf + r * W, acc = 0;
                    for (size_t x = 0; x < W; x++) { acc += row[x]; row[x] = acc; }
                }
            });
        if (buf != out) {
            if (!bad) memcpy(out, buf, fi->plane_size * P);
            free(buf);
        }
    }
    st->decode_ms = nitro_now_ms() - t0;
    return bad ? -1 : 0;
}
