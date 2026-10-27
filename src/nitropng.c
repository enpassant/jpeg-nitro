// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitropng: multi-threaded single-image PNG decoding. See nitropng.h.
#include "nitropng.h"

#include "nitro_os.h"   // parallel loop, clock, CPU count
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static double now_ms(void) { return nitro_now_ms(); }

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

#define IN_PAD 64   // zero bytes after the compressed data (branch-free bit reader)
#define OUT_PAD 64  // slack after output buffers (8-byte-at-a-time match copies)

// ---------------------------------------------------------------------------
// PNG container

// The Orientation tag (0x112) of an Exif block (TIFF structure, as in eXIf): 1..8, 1 if none.
static int exif_orientation(const uint8_t *p, size_t n) {
    if (n < 8 || p[0] != p[1] || (p[0] != 'I' && p[0] != 'M')) return 1;
    int le = p[0] == 'I';
#define G16(q) (le ? (uint32_t)(q)[0] | (uint32_t)(q)[1] << 8 : (uint32_t)(q)[0] << 8 | (uint32_t)(q)[1])
    uint32_t ifd = le ? (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24 : be32(p + 4);
    if (G16(p + 2) != 42 || ifd > n - 2) return 1;
    uint32_t count = G16(p + ifd);
    for (uint32_t i = 0; i < count && ifd + 2 + 12 * (size_t)(i + 1) <= n; i++) {
        const uint8_t *e = p + ifd + 2 + 12 * (size_t)i;
        if (G16(e) != 0x112) continue;
        uint32_t o = G16(e + 8);
        return G16(e + 2) == 3 && o >= 1 && o <= 8 ? (int)o : 1;
    }
#undef G16
    return 1;
}

int np_read_info(const uint8_t *d, size_t len, np_info *fi) {
    memset(fi, 0, sizeof *fi);
    if (len < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) || memcmp(d + 12, "IHDR", 4) || be32(d + 8) != 13) return -1;
    fi->width = (int)be32(d + 16);
    fi->height = (int)be32(d + 20);
    fi->bit_depth = d[24];
    fi->color_type = d[25];
    fi->interlace = d[28];
    if (fi->width <= 0 || fi->height <= 0) return -1;
    static const int ch[7] = {1, 0, 3, 1, 2, 0, 4};
    fi->channels = fi->color_type <= 6 ? ch[fi->color_type] : 0;
    int bits = fi->channels * fi->bit_depth;
    fi->stride = ((size_t)fi->width * bits + 7) / 8;
    fi->raw_size = (size_t)fi->height * (fi->stride + 1);
    fi->bpp = bits >= 8 ? bits / 8 : 1;
    fi->orientation = 1;
    int image = 0;   // (past the first IDAT: only eXIf, which some writers put after the image data)
    for (size_t i = 8; i + 12 <= len;) {
        uint32_t n = be32(d + i);
        if (n > len - i - 12) break;
        if (!memcmp(d + i + 4, "IEND", 4)) break;
        if (!memcmp(d + i + 4, "IDAT", 4)) image = 1;
        if (!memcmp(d + i + 4, "eXIf", 4)) fi->orientation = exif_orientation(d + i + 8, n);
        if (!image && !memcmp(d + i + 4, "iCCP", 4)) fi->has_icc = 1;
        if (!image && !memcmp(d + i + 4, "PLTE", 4)) { fi->plte_pos = (uint32_t)(i + 8); fi->plte_n = n / 3; }
        if (!image && !memcmp(d + i + 4, "tRNS", 4)) { fi->trns_pos = (uint32_t)(i + 8); fi->trns_n = n; }
        i += 12 + (size_t)n;
    }
    int bd = fi->bit_depth, ct = fi->color_type, sane = fi->channels > 0 && fi->interlace == 0 &&
                                                       fi->width < (1 << 24) && fi->height < (1 << 24);
    fi->supported = sane && bd == 8 && ct != 3 && !(fi->trns_pos && (ct == 0 || ct == 2));   // (a transparency key: converted)
    fi->convertible = sane && (ct == 0 ? bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16
                               : ct == 3 ? (bd == 1 || bd == 2 || bd == 4 || bd == 8) && fi->plte_n > 0
                                         : bd == 8 || bd == 16);
    return 0;
}

// Concatenates the IDAT chunks (the zlib stream), with IN_PAD zero bytes after it.
static uint8_t *gather_idat(const uint8_t *d, size_t len, size_t *zlen) {
    size_t total = 0;
    for (size_t i = 8; i + 12 <= len;) {
        uint32_t n = be32(d + i);
        if (n > len - i - 12) return NULL;
        if (!memcmp(d + i + 4, "IDAT", 4)) total += n;
        if (!memcmp(d + i + 4, "IEND", 4)) break;
        i += 12 + (size_t)n;
    }
    uint8_t *z = malloc(total + IN_PAD);
    if (!z) return NULL;
    size_t k = 0;
    for (size_t i = 8; i + 12 <= len;) {
        uint32_t n = be32(d + i);
        if (!memcmp(d + i + 4, "IDAT", 4)) { memcpy(z + k, d + i + 8, n); k += n; }
        if (!memcmp(d + i + 4, "IEND", 4)) break;
        i += 12 + (size_t)n;
    }
    memset(z + k, 0, IN_PAD);
    *zlen = k;
    return z;
}

// ---------------------------------------------------------------------------
// Huffman tables (deflate: LSB-first bit order, codes bit-reversed)

enum { K_LIT = 0, K_LEN = 1, K_EOB = 2, K_SUB = 3, K_BAD = 4 };
// entry: value << 16 | extra << 8 | kind << 5 | code length
#define ENT(len, kind, extra, value) ((uint32_t)(value) << 16 | (uint32_t)(extra) << 8 | (uint32_t)(kind) << 5 | (uint32_t)(len))
#define E_LEN(e) ((e) & 31)
#define E_KIND(e) (((e) >> 5) & 7)
#define E_EXTRA(e) (((e) >> 8) & 15)
#define E_VAL(e) ((e) >> 16)

#define LBITS 11                     // litlen primary table bits
#define DBITS 8                      // distance primary table bits
#define LSIZE ((1 << LBITS) + 288 * (1 << (15 - LBITS)))
#define DSIZE ((1 << DBITS) + 32 * (1 << (15 - DBITS)))

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                       769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static inline uint32_t sym_info(int sym, int is_dist) {
    if (is_dist) return sym < 30 ? ENT(0, K_LEN, dist_extra[sym], dist_base[sym]) : ENT(0, K_BAD, 0, 0);
    if (sym < 256) return ENT(0, K_LIT, 0, sym);
    if (sym == 256) return ENT(0, K_EOB, 0, 0);
    if (sym < 286) return ENT(0, K_LEN, len_extra[sym - 257], len_base[sym - 257]);
    return ENT(0, K_BAD, 0, 0);
}

static inline unsigned bitrev(unsigned code, int len) {
    unsigned r = 0;
    for (int i = 0; i < len; i++) { r = r << 1 | (code & 1); code >>= 1; }
    return r;
}

// Builds a decode table. Code-set rules as in zlib: over-subscribed is an
// error; incomplete only for a single code of length 1 (or no distance codes).
// Returns 0 on success.
static int build_table(const uint8_t *lens, int n, uint32_t *table, int tb, int is_dist) {
    int count[16] = {0};
    for (int s = 0; s < n; s++) count[lens[s]]++;
    count[0] = 0;
    int left = 1, max = 0, total = 0;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= count[l];
        if (left < 0) return -1;
        if (count[l]) max = l;
        total += count[l];
    }
    if (left > 0 && !(max == 1 && total == 1) && !(is_dist && total == 0)) return -1;
    int size = 1 << tb, sub_size = 1 << (15 - tb), used = size;
    for (int i = 0; i < size; i++) table[i] = ENT(0, K_BAD, 0, 0);
    unsigned next[16];
    unsigned code = 0;
    for (int l = 1; l <= 15; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (int s = 0; s < n; s++) {
        int l = lens[s];
        if (!l) continue;
        unsigned rev = bitrev(next[l]++, l);
        uint32_t info = sym_info(s, is_dist) | (uint32_t)l;
        if (l <= tb) {
            for (unsigned i = rev; i < (unsigned)size; i += 1u << l) table[i] = info;
        } else {
            unsigned prefix = rev & (size - 1);
            if (E_KIND(table[prefix]) != K_SUB) {
                table[prefix] = ENT(0, K_SUB, 0, used);
                for (int i = 0; i < sub_size; i++) table[used + i] = ENT(0, K_BAD, 0, 0);
                used += sub_size;
            }
            uint32_t *sub = table + E_VAL(table[prefix]);
            for (unsigned i = rev >> tb; i < (unsigned)sub_size; i += 1u << (l - tb)) sub[i] = info;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// bit reader: LSB first, branch-free refill (bits past 'cnt' are zero or correct)

typedef struct {
    const uint8_t *p, *base, *end;   // end: first padding byte
    uint64_t buf;
    unsigned cnt;
} BR;

static inline void br_refill(BR *r) {
    uint64_t v;
    memcpy(&v, r->p, 8);
    r->buf |= v << r->cnt;
    r->p += (63 - r->cnt) >> 3;
    r->cnt |= 56;
}
static inline void br_init(BR *r, const uint8_t *base, size_t len, uint64_t bitpos) {
    r->base = base;
    r->end = base + len;
    r->p = base + (bitpos >> 3);
    r->buf = 0;
    r->cnt = 0;
    br_refill(r);
    r->buf >>= bitpos & 7;
    r->cnt -= bitpos & 7;
}
static inline uint64_t br_pos(const BR *r) { return (uint64_t)(r->p - r->base) * 8 - r->cnt; }
static inline unsigned br_peek(const BR *r, unsigned n) { return (unsigned)(r->buf & ((1ull << n) - 1)); }
static inline void br_skip(BR *r, unsigned n) { r->buf >>= n; r->cnt -= n; }
static inline int br_over(const BR *r) { return r->p > r->end + 16; }   // ran into the padding

// ---------------------------------------------------------------------------
// block headers

typedef struct {
    uint32_t lt[LSIZE];
    uint32_t dt[DSIZE];
} Tables;

static Tables g_fixed;
static int g_fixed_ready;

static void init_fixed(void) {
    if (g_fixed_ready) return;
    uint8_t l[320];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    build_table(l, 288, g_fixed.lt, LBITS, 0);
    // 32 codes of 5 bits (a complete code; 30 and 31 are invalid symbols): with only 30 the
    // code is incomplete, build_table refused it and every fixed block with a match failed
    for (int i = 0; i < 32; i++) l[i] = 5;
    build_table(l, 32, g_fixed.dt, DBITS, 1);
    __sync_synchronize();
    g_fixed_ready = 1;
}

// Reads a dynamic block header (after BFINAL/BTYPE) and builds its tables.
static int read_dynamic(BR *r, Tables *t) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    br_refill(r);
    unsigned hlit = br_peek(r, 5) + 257; br_skip(r, 5);
    unsigned hdist = br_peek(r, 5) + 1; br_skip(r, 5);
    unsigned hclen = br_peek(r, 4) + 4; br_skip(r, 4);
    if (hlit > 286 || hdist > 30) return -1;
    uint8_t cl[19] = {0};
    for (unsigned i = 0; i < hclen; i++) {
        if (i == 10) br_refill(r);
        cl[order[i]] = (uint8_t)br_peek(r, 3);
        br_skip(r, 3);
    }
    uint32_t clt[1 << 7];
    // code-length code: 7-bit table, all codes <= 7 bits
    {
        int count[8] = {0}, left = 1, total = 0, max = 0;
        for (int s = 0; s < 19; s++) count[cl[s]]++;
        count[0] = 0;
        for (int l = 1; l <= 7; l++) { left <<= 1; left -= count[l]; if (left < 0) return -1; total += count[l]; if (count[l]) max = l; }
        if (left > 0 && !(max == 1 && total == 1)) return -1;
        for (int i = 0; i < 128; i++) clt[i] = ENT(0, K_BAD, 0, 0);
        unsigned next[8], code = 0;
        for (int l = 1; l <= 7; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
        for (int s = 0; s < 19; s++) {
            int l = cl[s];
            if (!l) continue;
            unsigned rev = bitrev(next[l]++, l);
            for (unsigned i = rev; i < 128; i += 1u << l) clt[i] = ENT(l, K_LIT, 0, s);
        }
    }
    uint8_t lens[286 + 30];
    unsigned n = 0, total = hlit + hdist;
    while (n < total) {
        if (br_over(r)) return -1;   // (the data ended in the header: the padding decodes as codes too)
        br_refill(r);
        uint32_t e = clt[br_peek(r, 7)];
        if (E_KIND(e) == K_BAD) return -1;
        br_skip(r, E_LEN(e));
        unsigned sym = E_VAL(e);
        if (sym < 16) { lens[n++] = (uint8_t)sym; continue; }
        unsigned rep, val = 0;
        if (sym == 16) {
            if (!n) return -1;
            val = lens[n - 1];
            rep = 3 + br_peek(r, 2); br_skip(r, 2);
        } else if (sym == 17) {
            rep = 3 + br_peek(r, 3); br_skip(r, 3);
        } else {
            rep = 11 + br_peek(r, 7); br_skip(r, 7);
        }
        if (n + rep > total) return -1;
        while (rep--) lens[n++] = (uint8_t)val;
    }
    if (!lens[256]) return -1;   // no end-of-block code
    if (build_table(lens, (int)hlit, t->lt, LBITS, 0)) return -1;
    if (build_table(lens + hlit, (int)hdist, t->dt, DBITS, 1)) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// block decoding: 8-bit output (sequential / first chunk)

// Decodes one block's symbols (tables ready) into out8 at *pos.
// Returns 0 at end of block, -1 on error.
static int decode_symbols8(BR *r, const Tables *t, uint8_t *out, size_t *ppos, size_t cap) {
    size_t pos = *ppos;
    for (;;) {
        br_refill(r);
        uint32_t e = t->lt[br_peek(r, LBITS)];
        if (E_KIND(e) == K_SUB) e = t->lt[E_VAL(e) + ((r->buf >> LBITS) & ((1 << (15 - LBITS)) - 1))];
        br_skip(r, E_LEN(e));
        unsigned kind = E_KIND(e);
        if (kind == K_LIT) {
            if (pos >= cap || br_over(r)) return -1;   // (garbage past the end: literals only)
            out[pos++] = (uint8_t)E_VAL(e);
            continue;
        }
        if (kind != K_LEN) {
            if (kind == K_EOB) break;
            return -1;
        }
        unsigned len = E_VAL(e) + br_peek(r, E_EXTRA(e));
        br_skip(r, E_EXTRA(e));
        uint32_t de = t->dt[br_peek(r, DBITS)];
        if (E_KIND(de) == K_SUB) de = t->dt[E_VAL(de) + ((r->buf >> DBITS) & ((1 << (15 - DBITS)) - 1))];
        if (E_KIND(de) != K_LEN) return -1;
        br_skip(r, E_LEN(de));
        br_refill(r);
        size_t dist = E_VAL(de) + br_peek(r, E_EXTRA(de));
        br_skip(r, E_EXTRA(de));
        if (dist > pos || pos + len > cap) return -1;
        uint8_t *dst = out + pos;
        const uint8_t *src = dst - dist;
        if (dist >= 8 && pos + len + 8 <= cap) {   // 8 bytes at a time (may write up to 7 past len)
            for (unsigned k = 0; k < len; k += 8) { uint64_t v; memcpy(&v, src + k, 8); memcpy(dst + k, &v, 8); }
        } else if (dist == 1) {
            memset(dst, src[0], len);
        } else {
            for (unsigned k = 0; k < len; k++) dst[k] = src[k];
        }
        pos += len;
        if (br_over(r)) return -1;
    }
    *ppos = pos;
    return br_over(r) ? -1 : 0;
}

// Decodes one deflate block (header at the reader's position) into out.
static int inflate8_one(BR *r, uint8_t *out, size_t *ppos, size_t cap, Tables *t, int *final) {
    {
        br_refill(r);
        unsigned bfinal = br_peek(r, 1);
        unsigned btype = (unsigned)(r->buf >> 1) & 3;
        br_skip(r, 3);
        if (btype == 0) {   // stored
            unsigned drop = r->cnt & 7;
            br_skip(r, drop);
            br_refill(r);
            unsigned slen = br_peek(r, 16), nlen = (unsigned)(r->buf >> 16) & 0xFFFF;
            br_skip(r, 32);
            if ((slen ^ 0xFFFF) != nlen || *ppos + slen > cap) return -1;
            // the remaining buffered bits are whole bytes: read them, then the rest directly
            for (; slen && r->cnt >= 8; slen--) { out[(*ppos)++] = (uint8_t)r->buf; br_skip(r, 8); }
            const uint8_t *src = r->p - r->cnt / 8;   // cnt is 0 here, or we are still inside the buffer
            if (slen) {
                if (src + slen > r->end) return -1;
                memcpy(out + *ppos, src, slen);
                *ppos += slen;
                uint64_t np = (uint64_t)(src + slen - r->base) * 8;
                br_init(r, r->base, (size_t)(r->end - r->base), np);
            }
        } else if (btype == 1) {
            if (decode_symbols8(r, &g_fixed, out, ppos, cap)) return -1;
        } else if (btype == 2) {
            if (read_dynamic(r, t) || decode_symbols8(r, t, out, ppos, cap)) return -1;
        } else {
            return -1;
        }
        *final = (int)bfinal;
        return br_over(r) ? -1 : 0;
    }
}

// Decodes deflate blocks until BFINAL. Returns 0 on success.
static int inflate8(BR *r, uint8_t *out, size_t *ppos, size_t cap, Tables *t, int *final) {
    *final = 0;
    while (!*final)
        if (inflate8_one(r, out, ppos, cap, t, final)) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Adler-32

#ifdef __AVX2__
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static uint32_t adler32(uint32_t adler, const uint8_t *p, size_t n) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
#ifdef __AVX2__
    // 32 bytes per step: s1 += sum(bytes); s2 += 32 * s1_before + sum((32 - i) * byte_i)
    const __m256i w = _mm256_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                       16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m256i ones = _mm256_set1_epi16(1), zero = _mm256_setzero_si256();
    while (n >= 32) {
        size_t blocks = n / 32 < 173 ? n / 32 : 173;   // keeps every lane sum far below overflow
        __m256i vs1 = zero, vs1acc = zero, vs2 = zero;
        for (size_t k = 0; k < blocks; k++) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(p + 32 * k));
            vs1acc = _mm256_add_epi64(vs1acc, vs1);
            vs1 = _mm256_add_epi64(vs1, _mm256_sad_epu8(v, zero));
            vs2 = _mm256_add_epi32(vs2, _mm256_madd_epi16(_mm256_maddubs_epi16(v, w), ones));
        }
        uint64_t t1[4], t2[4];
        uint32_t t3[8];
        _mm256_storeu_si256((__m256i *)t1, vs1);
        _mm256_storeu_si256((__m256i *)t2, vs1acc);
        _mm256_storeu_si256((__m256i *)t3, vs2);
        uint64_t s1 = t1[0] + t1[1] + t1[2] + t1[3], acc = t2[0] + t2[1] + t2[2] + t2[3], s2w = 0;
        for (int i = 0; i < 8; i++) s2w += t3[i];
        uint64_t bb = (uint64_t)b + (uint64_t)a * 32 * blocks + 32 * acc + s2w;
        a = (uint32_t)(((uint64_t)a + s1) % 65521);
        b = (uint32_t)(bb % 65521);
        p += 32 * blocks;
        n -= 32 * blocks;
    }
#elif defined(__ARM_NEON)
    // same scheme as AVX2: 32 bytes per step, weights 32..1
    static const uint8_t wt[32] = {32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                   16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    const uint8x8_t w0 = vld1_u8(wt), w1 = vld1_u8(wt + 8), w2 = vld1_u8(wt + 16), w3 = vld1_u8(wt + 24);
    while (n >= 32) {
        size_t blocks = n / 32 < 173 ? n / 32 : 173;   // keeps every lane sum far below overflow
        uint32x4_t vs1 = vdupq_n_u32(0), vs1acc = vs1, vs2 = vs1;
        for (size_t k = 0; k < blocks; k++) {
            uint8x16_t v0 = vld1q_u8(p + 32 * k), v1 = vld1q_u8(p + 32 * k + 16);
            vs1acc = vaddq_u32(vs1acc, vs1);
            vs1 = vpadalq_u16(vs1, vaddq_u16(vpaddlq_u8(v0), vpaddlq_u8(v1)));
            uint16x8_t m = vmull_u8(vget_low_u8(v0), w0);
            m = vmlal_u8(m, vget_high_u8(v0), w1);
            m = vmlal_u8(m, vget_low_u8(v1), w2);
            m = vmlal_u8(m, vget_high_u8(v1), w3);   // <= 8160 * 4 lanes-worth: fits 16 bits
            vs2 = vpadalq_u16(vs2, m);
        }
        uint64_t s1 = vaddvq_u32(vs1), acc = vaddvq_u32(vs1acc), s2w = vaddvq_u32(vs2);
        uint64_t bb = (uint64_t)b + (uint64_t)a * 32 * blocks + 32 * acc + s2w;
        a = (uint32_t)(((uint64_t)a + s1) % 65521);
        b = (uint32_t)(bb % 65521);
        p += 32 * blocks;
        n -= 32 * blocks;
    }
#endif
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

// ---------------------------------------------------------------------------
// filters (sequential reference implementation; parallel version below)

static int ncpu(void);

static inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static int unfilter_row(uint8_t *row, const uint8_t *prev, size_t stride, int bpp) {
    uint8_t t = row[-1];
    switch (t) {
    case 0: break;
    case 1: for (size_t x = bpp; x < stride; x++) row[x] += row[x - bpp]; break;
    case 2: if (prev) for (size_t x = 0; x < stride; x++) row[x] += prev[x]; break;
    case 3:
        if (prev) {
            for (int x = 0; x < bpp; x++) row[x] += prev[x] >> 1;
            for (size_t x = bpp; x < stride; x++) row[x] += (row[x - bpp] + prev[x]) >> 1;
        } else {
            for (size_t x = bpp; x < stride; x++) row[x] += row[x - bpp] >> 1;
        }
        break;
    case 4:
        if (prev) {
            for (int x = 0; x < bpp; x++) row[x] += prev[x];   // paeth(0, b, 0) = b
            for (size_t x = bpp; x < stride; x++) row[x] += paeth(row[x - bpp], prev[x], prev[x - bpp]);
        } else {
            for (size_t x = bpp; x < stride; x++) row[x] += row[x - bpp];   // paeth(a, 0, 0) = a
        }
        break;
    default: return -1;
    }
    return 0;
}

static int unfilter_seq(uint8_t *out, const np_info *fi) {
    int bpp = fi->bpp;
    size_t rs = fi->stride + 1;
    for (int y = 0; y < fi->height; y++)
        if (unfilter_row(out + (size_t)y * rs + 1, y ? out + (size_t)(y - 1) * rs + 1 : NULL, fi->stride, bpp)) return -1;
    return 0;
}



// Parallel filter reversal ("wavefront"). Up / Average / Paeth rows need the
// previous row, but only the bytes at or left of the current position, so row
// y can proceed segment by segment right behind row y-1. None / Sub rows don't
// wait at all. Workers take rows in order; progress[y] = bytes done in row y.
#define SEG 1024

static inline void cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause");
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

// Reverses bytes [a, b) of a row (a, b multiples of bpp, except b = stride);
// [0, a) is already done. Specialised per bpp so the left pixel stays in
// registers: going through memory would add store-forwarding latency to the
// (inherently serial) left-to-right dependency chain.
static inline __attribute__((always_inline)) void unfilter_seg_bpp(uint8_t *row, const uint8_t *prev, size_t a,
                                                                   size_t b, const int bpp, int t) {
    int L[8] = {0}, UL[8] = {0};
    size_t x = a;
    // UL only for Paeth: None / Sub rows don't wait for the row above, so must not read it
    if (a) for (int k = 0; k < bpp; k++) { L[k] = row[a - bpp + k]; if (prev && t == 4) UL[k] = prev[a - bpp + k]; }
    switch (t) {
    case 1:   // Sub
        for (; x < b; x += bpp)
            for (int k = 0; k < bpp; k++) { L[k] = (uint8_t)(row[x + k] + L[k]); row[x + k] = (uint8_t)L[k]; }
        break;
    case 2:   // Up (vectorised by the compiler)
        if (prev) for (; x < b; x++) row[x] += prev[x];
        break;
    case 3:   // Average
        for (; x < b; x += bpp)
            for (int k = 0; k < bpp; k++) {
                int up = prev ? prev[x + k] : 0;
                L[k] = (uint8_t)(row[x + k] + ((L[k] + up) >> 1));
                row[x + k] = (uint8_t)L[k];
            }
        break;
    case 4:   // Paeth
        for (; x < b; x += bpp)
            for (int k = 0; k < bpp; k++) {
                int up = prev ? prev[x + k] : 0, l = L[k], ul = UL[k];
                int pa = abs(up - ul), pb = abs(l - ul), pc = abs(l + up - 2 * ul);
                int m1 = -((pa <= pb) & (pa <= pc)), m2 = -(pb <= pc);   // branch-free select
                int pr = (l & m1) | (~m1 & ((up & m2) | (ul & ~m2)));
                L[k] = (uint8_t)(row[x + k] + pr);
                row[x + k] = (uint8_t)L[k];
                UL[k] = up;
            }
        break;
    }
}

static void unfilter_seg(uint8_t *row, const uint8_t *prev, size_t a, size_t b, int bpp, int t) {
    switch (bpp) {
    case 1: unfilter_seg_bpp(row, prev, a, b, 1, t); break;
    case 2: unfilter_seg_bpp(row, prev, a, b, 2, t); break;
    case 3: unfilter_seg_bpp(row, prev, a, b, 3, t); break;
    case 4: unfilter_seg_bpp(row, prev, a, b, 4, t); break;
    case 6: unfilter_seg_bpp(row, prev, a, b, 6, t); break;   // (16-bit RGB)
    default: unfilter_seg_bpp(row, prev, a, b, 8, t); break;  // (16-bit RGBA)
    }
}

static int unfilter_parallel(uint8_t *out, const np_info *fi, int nthreads) {
    const int H = fi->height, bpp = fi->bpp;
    const size_t stride = fi->stride, rs = stride + 1;
    for (int y = 0; y < H; y++)
        if (out[(size_t)y * rs] > 4) return -1;
    uint32_t *progress = calloc((size_t)H, sizeof *progress);
    if (!progress) return -1;
    volatile long next = 0, *const pnext = &next;
    // not all hyper-threads: waiting ones would slow down their working siblings (measured: 12 of 16 best)
    int W = nthreads > 0 ? nthreads : (ncpu() * 3 + 3) / 4;
    if (getenv("NP_UW")) W = atoi(getenv("NP_UW"));
    const size_t seg = (SEG / bpp) * bpp;
    nitro_parallel((size_t)W, ^(size_t w) {
        for (;;) {
            long y = __sync_fetch_and_add(pnext, 1);
            if (y >= H) break;
            uint8_t *row = out + (size_t)y * rs + 1;
            const uint8_t *prev = y ? row - rs : NULL;
            int t = row[-1];
            int needs_prev = y > 0 && t >= 2;
            for (size_t a = 0; a < stride; a += seg) {
                size_t b = a + seg < stride ? a + seg : stride;
                if (needs_prev) {
                    unsigned spins = 0;
                    while (__atomic_load_n(&progress[y - 1], __ATOMIC_ACQUIRE) < b) {
                        cpu_relax();
                        if (++spins > 4096) { sched_yield(); spins = 0; }
                    }
                }
                unfilter_seg(row, prev, a, b, bpp, t);
                __atomic_store_n(&progress[y], (uint32_t)b, __ATOMIC_RELEASE);
            }
        }
    });
    free(progress);
    return 0;
}

// ---------------------------------------------------------------------------
// speculative 16-bit decoding (all chunks but the first)
//
// Output symbols: v < 256 is a byte; v >= 256 is a placeholder for the byte
// at (chunk start + v - 33024), i.e. inside the 32 KB before the chunk, still
// unknown while decoding. In-chunk back-references copy symbols as they are,
// so every symbol later resolves on its own once that window is known.

#define PH_BASE 33024   // 256 + 32768

static int grow16(uint16_t **o, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t nc = need + need / 2 + 65536;
    uint16_t *n = realloc(*o, (nc + OUT_PAD) * sizeof **o);
    if (!n) return -1;
    *o = n;
    *cap = nc;
    return 0;
}

static int decode_symbols16(BR *r, const Tables *t, uint16_t **pout, size_t *ppos, size_t *pcap, size_t limit) {
    size_t pos = *ppos, cap = *pcap;
    uint16_t *out = *pout;
    for (;;) {
        if (pos + 258 > cap) {
            if (pos > limit) return -1;   // runaway (garbage) decoding
            if (grow16(pout, pcap, pos + 258)) return -1;
            out = *pout;
            cap = *pcap;
        }
        br_refill(r);
        uint32_t e = t->lt[br_peek(r, LBITS)];
        if (E_KIND(e) == K_SUB) e = t->lt[E_VAL(e) + ((r->buf >> LBITS) & ((1 << (15 - LBITS)) - 1))];
        br_skip(r, E_LEN(e));
        unsigned kind = E_KIND(e);
        if (kind == K_LIT) {
            if (br_over(r)) return -1;   // (garbage past the end: literals only)
            out[pos++] = (uint16_t)E_VAL(e);
            continue;
        }
        if (kind != K_LEN) {
            if (kind == K_EOB) break;
            return -1;
        }
        unsigned len = E_VAL(e) + br_peek(r, E_EXTRA(e));
        br_skip(r, E_EXTRA(e));
        uint32_t de = t->dt[br_peek(r, DBITS)];
        if (E_KIND(de) == K_SUB) de = t->dt[E_VAL(de) + ((r->buf >> DBITS) & ((1 << (15 - DBITS)) - 1))];
        if (E_KIND(de) != K_LEN) return -1;
        br_skip(r, E_LEN(de));
        br_refill(r);
        int64_t dist = (int64_t)(E_VAL(de) + br_peek(r, E_EXTRA(de)));   // (int64_t: long is 32-bit on Win64)
        br_skip(r, E_EXTRA(de));
        int64_t s0 = (int64_t)pos - dist;
        uint16_t *dst = out + pos;
        if (s0 >= 0) {
            const uint16_t *src = out + s0;
            if (dist >= 4) {
                for (unsigned k = 0; k < len; k += 4) { uint64_t v; memcpy(&v, src + k, 8); memcpy(dst + k, &v, 8); }
            } else {
                for (unsigned k = 0; k < len; k++) dst[k] = src[k];
            }
        } else {
            if (s0 < -32768) return -1;
            for (unsigned k = 0; k < len; k++) {
                int64_t sk = s0 + (int64_t)k;
                dst[k] = sk < 0 ? (uint16_t)(PH_BASE + sk) : out[sk];
            }
        }
        pos += len;
        if (br_over(r)) return -1;
    }
    *ppos = pos;
    return br_over(r) ? -1 : 0;
}

// One block (header at the reader's position) into 16-bit output.
// *boundary receives the bit position right after the block.
static int decode_block16(BR *r, Tables *t, uint16_t **o, size_t *pos, size_t *cap, size_t limit, int *final) {
    br_refill(r);
    *final = (int)br_peek(r, 1);
    unsigned btype = (unsigned)(r->buf >> 1) & 3;
    br_skip(r, 3);
    if (btype == 0) {
        br_skip(r, r->cnt & 7);
        br_refill(r);
        unsigned slen = br_peek(r, 16), nlen = (unsigned)(r->buf >> 16) & 0xFFFF;
        br_skip(r, 32);
        if ((slen ^ 0xFFFF) != nlen) return -1;
        if (grow16(o, cap, *pos + slen)) return -1;
        uint16_t *out = *o;
        for (; slen && r->cnt >= 8; slen--) { out[(*pos)++] = (uint8_t)r->buf; br_skip(r, 8); }
        const uint8_t *src = r->p;
        if (slen) {
            if (src + slen > r->end) return -1;
            for (unsigned k = 0; k < slen; k++) out[(*pos)++] = src[k];
            br_init(r, r->base, (size_t)(r->end - r->base), (uint64_t)(src + slen - r->base) * 8);
        }
        return 0;
    }
    if (btype == 1) return decode_symbols16(r, &g_fixed, o, pos, cap, limit);
    if (btype == 2) return read_dynamic(r, t) ? -1 : decode_symbols16(r, t, o, pos, cap, limit);
    return -1;
}

// Cheap test whether a dynamic block header could start at bit b: BTYPE, HLIT,
// HDIST and a valid (complete) code-length code. Most positions fail here.
static inline int maybe_dynamic(const uint8_t *z, uint64_t b) {
    uint64_t v;
    memcpy(&v, z + (b >> 3), 8);
    v >>= b & 7;   // >= 56 valid bits
    if (((v >> 1) & 3) != 2) return 0;
    if (((v >> 3) & 31) > 29 || ((v >> 8) & 31) > 29) return 0;
    unsigned hclen = (unsigned)((v >> 13) & 15) + 4;
    uint64_t w;
    memcpy(&w, z + ((b + 17) >> 3), 8);
    w >>= (b + 17) & 7;
    int kraft = 0, n = 0;   // sum of 2^(7 - len), complete code = 128
    for (unsigned i = 0; i < hclen; i++) {
        unsigned l = (unsigned)(w >> (3 * i)) & 7;
        if (l) { kraft += 128 >> l; n++; }
    }
    return kraft == 128 || (n == 1 && kraft == 64);
}

typedef struct {
    uint64_t lo, hi;        // nominal bit range to search for the first block
    uint64_t start;         // found block start (UINT64_MAX: none)
    uint16_t *o;            // decoded symbols
    size_t n, cap;
    int stop;               // chunk index whose start we reached (-1: ended with BFINAL)
    int final, err;
    uint64_t endpos;        // bit position after the last decoded block
    Tables *t;
} PChunk;

// Phase 1: find the first plausible block start in [lo, hi), confirmed by
// decoding that block and finding a sane header after it.
static void pc_find(const uint8_t *z, size_t zl, PChunk *c, size_t limit) {
    c->start = UINT64_MAX;
    for (uint64_t b = c->lo; b < c->hi; b++) {
        if (!maybe_dynamic(z, b)) continue;
        BR r;
        br_init(&r, z, zl, b);
        c->n = 0;
        int final;
        if (decode_block16(&r, c->t, &c->o, &c->n, &c->cap, limit, &final)) continue;
        if (!final) {   // the next header must at least have a valid block type
            br_refill(&r);
            if (((r.buf >> 1) & 3) == 3) continue;
        }
        c->start = b;
        c->endpos = br_pos(&r);
        c->final = final;
        return;
    }
    c->n = 0;
}

// Chunk 0: no window needed, decodes straight into the final 8-bit output.
static void pc_decode0(const uint8_t *z, size_t zl, PChunk *ch, int N, uint8_t *out, size_t outlen) {
    PChunk *c = &ch[0];
    c->stop = -1;
    BR r;
    br_init(&r, z, zl, 0);
    size_t pos = 0;
    int j = 1;
    for (;;) {
        uint64_t bp = br_pos(&r);
        while (j < N && (ch[j].start == UINT64_MAX || ch[j].start < bp)) j++;
        if (j < N && ch[j].start == bp && bp) { c->stop = j; break; }
        int final;
        if (inflate8_one(&r, out, &pos, outlen, c->t, &final)) { c->err = 1; return; }
        if (final) { c->final = 1; break; }
    }
    c->n = pos;
    c->endpos = br_pos(&r);
}

// Phase 2: keep decoding until a block boundary equals another chunk's start.
static void pc_decode(const uint8_t *z, size_t zl, PChunk *ch, int N, int i, size_t limit) {
    PChunk *c = &ch[i];
    c->stop = -1;
    if (c->start == UINT64_MAX) return;
    BR r;
    br_init(&r, z, zl, c->endpos);
    int j = i + 1;
    while (!c->final) {
        uint64_t pos = br_pos(&r);
        while (j < N && (ch[j].start == UINT64_MAX || ch[j].start < pos)) j++;
        if (j < N && ch[j].start == pos) { c->stop = j; c->endpos = pos; return; }
        if (decode_block16(&r, c->t, &c->o, &c->n, &c->cap, limit, &c->final)) { c->err = 1; return; }
    }
    c->endpos = br_pos(&r);
}

static uint32_t adler32_combine(uint32_t a1, uint32_t a2, size_t len2) {
    const uint64_t BASE = 65521;
    uint64_t rem = len2 % BASE, sum1 = a1 & 0xFFFF, sum2 = (rem * sum1) % BASE;
    sum1 += (a2 & 0xFFFF) + BASE - 1;
    sum2 += ((a1 >> 16) & 0xFFFF) + ((a2 >> 16) & 0xFFFF) + BASE - rem;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum2 >= BASE << 1) sum2 -= BASE << 1;
    if (sum2 >= BASE) sum2 -= BASE;
    return (uint32_t)(sum1 | sum2 << 16);
}

static int g_chunks = 0;
void np_set_chunks(int n) { g_chunks = n; }

// Writes symbols [i0, i1) of a chunk (at output offset o) as bytes, looking up
// placeholders in the window before the chunk. Returns -1 on an impossible reference.
static int resolve(uint8_t *out, size_t o, const uint16_t *sym, size_t i0, size_t i1) {
    size_t i = i0;
#ifdef __AVX2__
    const __m256i lim = _mm256_set1_epi16(255);
    for (; i + 16 <= i1; i += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(sym + i));
        if (!_mm256_testz_si256(v, _mm256_andnot_si256(lim, _mm256_set1_epi16(-1)))) {   // any >= 256
            for (size_t k = i; k < i + 16; k++) {
                uint16_t s = sym[k];
                if (s < 256) { out[o + k] = (uint8_t)s; continue; }
                int64_t w = (int64_t)o + ((int64_t)s - PH_BASE);
                if (w < 0) return -1;
                out[o + k] = out[w];
            }
            continue;
        }
        __m128i lo = _mm256_castsi256_si128(v), hi = _mm256_extracti128_si256(v, 1);
        _mm_storeu_si128((__m128i *)(out + o + i), _mm_packus_epi16(lo, hi));
    }
#elif defined(__ARM_NEON)
    for (; i + 16 <= i1; i += 16) {
        uint16x8_t lo = vld1q_u16(sym + i), hi = vld1q_u16(sym + i + 8);
        if (vmaxvq_u16(vorrq_u16(lo, hi)) > 255) {   // a placeholder among them
            for (size_t k = i; k < i + 16; k++) {
                uint16_t s = sym[k];
                if (s < 256) { out[o + k] = (uint8_t)s; continue; }
                int64_t w = (int64_t)o + ((int64_t)s - PH_BASE);
                if (w < 0) return -1;
                out[o + k] = out[w];
            }
            continue;
        }
        vst1q_u8(out + o + i, vcombine_u8(vmovn_u16(lo), vmovn_u16(hi)));
    }
#endif
    for (; i < i1; i++) {
        uint16_t s = sym[i];
        if (s < 256) { out[o + i] = (uint8_t)s; continue; }
        int64_t w = (int64_t)o + ((int64_t)s - PH_BASE);
        if (w < 0) return -1;
        out[o + i] = out[w];
    }
    return 0;
}

// --- buffer pool: symbol buffers and tables are reused between images ---
typedef struct { void *p; size_t cap; } PoolBuf;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static PoolBuf g_pool[1024];
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
    if (g_pool_n == (int)(sizeof g_pool / sizeof *g_pool)) {
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

static int ncpu(void) { return nitro_ncpu(); }

// Parallel inflate of the raw deflate stream d (dl bytes, padded) into out.
// Returns 0 if the result is complete and its Adler-32 matches.
static int inflate_parallel(const uint8_t *d, size_t dl, const uint8_t *ztrail_base, size_t zlen, uint8_t *out,
                            size_t outlen, int nthreads, np_stats *st) {
    int N = g_chunks > 0 ? g_chunks : (nthreads > 0 ? nthreads : ncpu());
    uint64_t bits = (uint64_t)dl * 8;
    if ((uint64_t)N * 8 * 65536 > bits) N = (int)(bits / (8 * 65536)) + 1;   // >= 64 KB per chunk
    PChunk *ch = calloc(N, sizeof *ch);
    size_t *tcap = calloc(N, sizeof *tcap);
    int *chain = NULL, nc = 0, cur = 0, rc = -1;
    size_t *off = NULL, total = 0;
    if (!ch || !tcap) goto fail;
    size_t limit = outlen + 65536;
    size_t per = outlen / N * 2 + 65536;   // symbols; grows if a chunk expands more than average
    for (int i = 0; i < N; i++) {
        ch[i].lo = bits * i / N;
        ch[i].hi = bits * (i + 1) / N;
        ch[i].t = pool_take(sizeof(Tables), &tcap[i]);
        if (!ch[i].t) goto fail;
        if (i) {
            size_t bytes;
            ch[i].o = pool_take((per + OUT_PAD) * sizeof(uint16_t), &bytes);
            if (!ch[i].o) goto fail;
            ch[i].cap = bytes / sizeof(uint16_t) - OUT_PAD;
        }
    }
    double tp0 = now_ms();
    // phase 1: block starts (chunk 0 starts at bit 0 by definition)
    ch[0].start = 0;
    nitro_parallel((size_t)(N - 1), ^(size_t k) { pc_find(d, dl, &ch[k + 1], limit); });
    double tp1 = now_ms();
    // phase 2: decode until reaching the next chunk's start (chunk 0: straight into out)
    nitro_parallel((size_t)N, ^(size_t i) {
        if (i == 0) pc_decode0(d, dl, ch, N, out, outlen);
        else if (!ch[i].err) pc_decode(d, dl, ch, N, (int)i, limit);
    });
    double tp2 = now_ms();
    // stitch: follow the chain of chunks from the first one
    chain = malloc(N * sizeof *chain);
    off = calloc(N, sizeof *off);
    if (!chain || !off) goto fail;
    for (;;) {
        if (ch[cur].err) goto fail;
        chain[nc++] = cur;
        off[cur] = total;
        total += ch[cur].n;
        if (total > outlen) goto fail;
        if (ch[cur].final) break;
        if (ch[cur].stop < 0) goto fail;
        cur = ch[cur].stop;
    }
    if (total != outlen) goto fail;
    st->chunks = nc;
    // the last 32 KB of every chunk, in order (each needs the previous ones' tails)
    for (int k = 1; k < nc; k++) {
        PChunk *c = &ch[chain[k]];
        size_t t0 = c->n > 32768 ? c->n - 32768 : 0;
        if (resolve(out, off[chain[k]], c->o, t0, c->n)) goto fail;
    }
    {
        // the rest in parallel, plus the Adler-32 of every chunk
        uint32_t *ad = calloc(nc, sizeof *ad);
        if (!ad) goto fail;
        int bad = 0, *const pbad = &bad;
        nitro_parallel((size_t)nc, ^(size_t k) {
            PChunk *c = &ch[chain[k]];
            size_t o = off[chain[k]], t0 = c->n > 32768 ? c->n - 32768 : 0;
            if (k && resolve(out, o, c->o, 0, t0)) *pbad = 1;
            ad[k] = adler32(1, out + o, c->n);
        });
        double tp3 = now_ms();
        if (getenv("NP_DEBUG")) {
            size_t ph = 0;
            for (int k = 1; k < nc; k++) {
                PChunk *c = &ch[chain[k]];
                for (size_t i = 0; i < c->n; i++) ph += c->o[i] >= 256;
            }
            fprintf(stderr, "  find %.1f  decode %.1f  resolve+adler %.1f ms | placeholders %zu\n",
                    tp1 - tp0, tp2 - tp1, tp3 - tp2, ph);
        }
        uint32_t a = ad[0];
        for (int k = 1; k < nc; k++) a = adler32_combine(a, ad[k], ch[chain[k]].n);
        free(ad);
        // trailer after the final block (byte aligned)
        size_t ai = (size_t)((ch[chain[nc - 1]].endpos + 7) / 8) + 2;
        if (!bad && ai + 4 <= zlen && a == be32(ztrail_base + ai)) rc = 0;
    }
fail:
    for (int i = 0; ch && tcap && i < N; i++) {
        pool_give(ch[i].t, tcap[i]);
        if (ch[i].o) pool_give(ch[i].o, (ch[i].cap + OUT_PAD) * sizeof(uint16_t));
    }
    free(ch);
    free(tcap);
    free(chain);
    free(off);
    return rc;
}

// ---------------------------------------------------------------------------
// plain zlib streams (used by nitropsd): parallel inflate + Adler-32 check

int np_zlib_decompress(const uint8_t *zin, size_t zlen, uint8_t *out, size_t outlen, int nthreads) {
    init_fixed();
    if (zlen < 6 || (zin[0] & 15) != 8 || (zin[0] >> 4) > 7 || ((zin[0] << 8) | zin[1]) % 31 || (zin[1] & 0x20))
        return -1;
    uint8_t *z = malloc(zlen + IN_PAD);   // the bit reader wants zero padding after the data
    if (!z) return -1;
    memcpy(z, zin, zlen);
    memset(z + zlen, 0, IN_PAD);
    np_stats st;
    memset(&st, 0, sizeof st);
    int rc = -1;
    if (nthreads != 1) {
        rc = inflate_parallel(z + 2, zlen - 2, z, zlen, out, outlen, nthreads, &st);
    } else {
        Tables *t = malloc(sizeof *t);
        BR r;
        br_init(&r, z + 2, zlen - 2, 0);
        size_t pos = 0;
        int final;
        if (t && !inflate8(&r, out, &pos, outlen, t, &final) && final && pos == outlen) {
            size_t ai = (size_t)((br_pos(&r) + 7) / 8) + 2;
            if (ai + 4 <= zlen && adler32(1, out, pos) == be32(z + ai)) rc = 0;
        }
        free(t);
    }
    free(z);
    return rc;
}

// ---------------------------------------------------------------------------
// driver

int np_decode(const uint8_t *data, size_t len, const np_info *fi, uint8_t *out, int nthreads, np_stats *st) {
    np_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    if (!fi->convertible) return -1;
    init_fixed();
    double t0 = now_ms();
    size_t zlen;
    uint8_t *z = gather_idat(data, len, &zlen);
    if (!z) return -1;
    int rc = -1;
    Tables *t = malloc(sizeof *t);
    if (zlen < 6 || (z[0] & 15) != 8 || (z[0] >> 4) > 7 || ((z[0] << 8) | z[1]) % 31 || (z[1] & 0x20)) goto done;
    st->parse_ms = now_ms() - t0;
    if (nthreads != 1) {
        double t1 = now_ms();
        if (inflate_parallel(z + 2, zlen - 2, z, zlen, out, fi->raw_size, nthreads, st)) goto done;
        st->inflate_ms = now_ms() - t1;
        st->mode = 1;
        double t2 = now_ms();
        if (unfilter_parallel(out, fi, nthreads)) goto done;
        st->unfilter_ms = now_ms() - t2;
        rc = 0;
        goto done;
    }
    {
        double t1 = now_ms();
        BR r;
        br_init(&r, z + 2, zlen - 2, 0);
        size_t pos = 0;
        int final;
        if (inflate8(&r, out, &pos, fi->raw_size, t, &final) || !final || pos != fi->raw_size) goto done;
        // Adler-32 trailer: byte-aligned after the last block
        uint64_t bp = (br_pos(&r) + 7) & ~7ull;
        size_t ai = (size_t)(bp / 8) + 2;
        if (ai + 4 > zlen || adler32(1, out, pos) != be32(z + ai)) goto done;
        st->inflate_ms = now_ms() - t1;
        st->chunks = 1;
        double t2 = now_ms();
        if (unfilter_seq(out, fi)) goto done;
        st->unfilter_ms = now_ms() - t2;
        rc = 0;
    }
done:
    free(t);
    free(z);
    return rc;
}

// ---------------------------------------------------------------------------
// any decoded PNG -> 8-bit B G R X (palette, 1 / 2 / 4 / 16-bit, transparency over black)

static inline int to8(int v16) { return (v16 * 255 + 32767) / 65535; }
static inline int mul(int c, int a) { return (c * a + 127) / 255; }

static void row_bgrx(const np_info *fi, const uint8_t *r, const uint8_t (*pal)[4], int key, const int *rgbkey, uint8_t *o) {
    int w = fi->width, bd = fi->bit_depth;
    switch (fi->color_type) {
    case 3: case 0: {   // palette / gray: 1 / 2 / 4 / 8 / 16 bits per sample
        int mask = (1 << (bd > 8 ? 8 : bd)) - 1, scale = 255 / mask;
        for (int x = 0; x < w; x++, o += 4) {
            int v;
            if (bd == 16) v = r[2 * x] << 8 | r[2 * x + 1];
            else if (bd == 8) v = r[x];
            else v = r[x * bd >> 3] >> (8 - bd - (x * bd & 7)) & mask;
            if (fi->color_type == 3) {
                o[0] = mul(pal[v][2], pal[v][3]);
                o[1] = mul(pal[v][1], pal[v][3]);
                o[2] = mul(pal[v][0], pal[v][3]);
            } else {
                int g = v == key ? 0 : bd == 16 ? to8(v) : v * scale;
                o[0] = o[1] = o[2] = (uint8_t)g;
            }
            o[3] = 255;
        }
        break;
    }
    case 4:   // gray + alpha
        for (int x = 0; x < w; x++, o += 4) {
            int g = bd == 16 ? to8(r[4 * x] << 8 | r[4 * x + 1]) : r[2 * x];
            int a = bd == 16 ? to8(r[4 * x + 2] << 8 | r[4 * x + 3]) : r[2 * x + 1];
            o[0] = o[1] = o[2] = (uint8_t)mul(g, a);
            o[3] = 255;
        }
        break;
    case 2: case 6: {   // RGB (+ alpha)
        int n = fi->color_type == 6 ? 4 : 3;
        for (int x = 0; x < w; x++, o += 4) {
            int c[4];
            if (bd == 16)
                for (int k = 0; k < n; k++) c[k] = r[2 * (n * x + k)] << 8 | r[2 * (n * x + k) + 1];
            else
                for (int k = 0; k < n; k++) c[k] = r[n * x + k];
            int a = n == 4 ? (bd == 16 ? to8(c[3]) : c[3]) : (c[0] == rgbkey[0] && c[1] == rgbkey[1] && c[2] == rgbkey[2] ? 0 : 255);
            if (bd == 16) for (int k = 0; k < 3; k++) c[k] = to8(c[k]);
            o[0] = (uint8_t)mul(c[2], a);
            o[1] = (uint8_t)mul(c[1], a);
            o[2] = (uint8_t)mul(c[0], a);
            o[3] = 255;
        }
        break;
    }
    }
}

int np_to_bgrx(const uint8_t *data, size_t len, const np_info *fi, const uint8_t *raw, uint8_t *bgrx, size_t pitch,
               int nthreads) {
    if (!fi->convertible) return -1;
    uint8_t pal[256][4];   // palette with its alpha (tRNS); missing entries black
    memset(pal, 0, sizeof pal);
    for (uint32_t i = 0; i < fi->plte_n && i < 256 && fi->plte_pos + 3 * i + 3 <= len; i++) {
        memcpy(pal[i], data + fi->plte_pos + 3 * i, 3);
        pal[i][3] = 255;
    }
    int key = -1, rgbkey[3] = {-1, -1, -1};   // transparency key of gray / RGB images
    if (fi->trns_pos && fi->trns_pos + fi->trns_n <= len) {
        const uint8_t *t = data + fi->trns_pos;
        if (fi->color_type == 3)
            for (uint32_t i = 0; i < fi->trns_n && i < 256; i++) pal[i][3] = t[i];
        else if (fi->color_type == 0 && fi->trns_n >= 2)
            key = t[0] << 8 | t[1];
        else if (fi->color_type == 2 && fi->trns_n >= 6)
            for (int k = 0; k < 3; k++) rgbkey[k] = t[2 * k] << 8 | t[2 * k + 1];
    }
    const uint8_t(*P)[4] = (const uint8_t(*)[4])pal;
    const int *K = rgbkey;
    const int band = 16, H = fi->height;
    const size_t rs = fi->stride + 1;
    size_t nb = (size_t)(H + band - 1) / band;
    nitro_body body = ^(size_t b) {
        for (int y = (int)b * band; y < H && y < (int)b * band + band; y++)
            row_bgrx(fi, raw + (size_t)y * rs + 1, P, key, K, bgrx + pitch * y);
    };
    if (nthreads == 1)
        for (size_t b = 0; b < nb; b++) body(b);
    else
        nitro_parallel(nb, body);
    return 0;
}
