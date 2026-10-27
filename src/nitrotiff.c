// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrotiff: see nitrotiff.h
#include "nitrotiff.h"
#include "nitropng.h"   // np_zlib_decompress: Deflate
#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// ---------------------------------------------------------------------------
// TIFF structure

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

static const int type_size[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};   // (13: IFD, an offset)

static Entry tag(const Tiff *t, uint32_t ifd, unsigned want) {
    Entry e = {0, 0, 0, 0};
    uint32_t n = t16(t, ifd);
    for (uint32_t i = 0; i < n; i++) {
        size_t p = ifd + 2 + 12 * (size_t)i;
        if (p + 12 > t->len) break;
        if (t16(t, p) != want) continue;
        e.type = t16(t, p + 2);
        e.count = t32(t, p + 4);
        uint64_t sz = e.type < 14 ? (uint64_t)type_size[e.type] * e.count : 0;
        if (!sz || e.count > (1u << 28)) return e;
        e.pos = sz <= 4 ? (uint32_t)(p + 8) : t32(t, p + 8);
        e.found = e.pos + sz <= t->len;
        return e;
    }
    return e;
}

static uint32_t val(const Tiff *t, Entry e, uint32_t k) {   // BYTE / SHORT / LONG
    if (!e.found || k >= e.count) return 0;
    switch (e.type) {
    case 1: case 7: return t->d[e.pos + k];
    case 3: return t16(t, e.pos + 2 * (size_t)k);
    case 4: case 13: return t32(t, e.pos + 4 * (size_t)k);
    default: return 0;
    }
}

static uint32_t get(const Tiff *t, uint32_t ifd, unsigned want, uint32_t def) {
    Entry e = tag(t, ifd, want);
    return e.found && e.count ? val(t, e, 0) : def;
}

static uint32_t next_ifd(const Tiff *t, uint32_t ifd) { return t32(t, ifd + 2 + 12 * (size_t)t16(t, ifd)); }

int nt_read_info(const uint8_t *data, size_t len, nt_info *in) {
    memset(in, 0, sizeof *in);
    if (len < 16 || !(data[0] == data[1] && (data[0] == 'I' || data[0] == 'M'))) return -1;
    Tiff t = {data, len, data[0] == 'I'};
    if (t16(&t, 2) != 42) return -1;
    uint32_t ifd = t32(&t, 4);
    if (!ifd || ifd + 2 > len || tag(&t, ifd, 50706).found) return -1;   // (a DNG: nitrodng's)
    for (int k = 0; k < 8 && ifd && ifd + 2 <= len && (get(&t, ifd, 254, 0) & 1); k++) ifd = next_ifd(&t, ifd);   // (a preview first)
    if (!ifd || ifd + 2 > len) return -1;
    int w = (int)get(&t, ifd, 256, 0), h = (int)get(&t, ifd, 257, 0);
    if (w <= 0 || h <= 0 || w > 1 << 20 || h > 1 << 20) return -1;
    in->width = w;
    in->height = h;
    in->le = t.le;
    uint32_t o = get(&t, ifd, 274, 1);
    in->orientation = o >= 1 && o <= 8 ? (int)o : 1;
    Entry icc = tag(&t, ifd, 34675);
    if (icc.found && (icc.type == 7 || icc.type == 1) && icc.count >= 128) {
        in->icc = data + icc.pos;
        in->icc_len = icc.count;
    }
    int spp = (int)get(&t, ifd, 277, 1), comp = (int)get(&t, ifd, 259, 1);
    Entry bs = tag(&t, ifd, 258);
    int bps = bs.found ? (int)val(&t, bs, 0) : 1;
    for (uint32_t k = 1; bs.found && k < bs.count && k < (uint32_t)spp; k++)
        if ((int)val(&t, bs, k) != bps) return -1;   // (mixed sample sizes)
    int phot = (int)get(&t, ifd, 262, spp >= 3 ? 2 : 1);
    int color = phot == 2 ? 3 : phot == 5 ? 4 : 1;
    if (spp < color || spp > 8) return -1;
    in->bps = bps;
    in->spp = spp;
    in->color = color;
    in->photometric = phot;
    in->compression = comp;
    in->predictor = (int)get(&t, ifd, 317, 1);
    in->planar = (int)get(&t, ifd, 284, 1);
    in->fillorder = (int)get(&t, ifd, 266, 1);
    in->t4options = (int)get(&t, ifd, comp == 3 ? 292 : 293, 0);
    if (spp > color) {
        uint32_t es = get(&t, ifd, 338, 0);
        in->alpha = es == 1 || es == 2 ? (int)es : 0;   // (0: an extra channel, not transparency)
    }
    // what is decoded
    int ccitt = comp == 2 || comp == 3 || comp == 4;
    if (!(comp == 1 || comp == 5 || comp == 8 || comp == 32946 || comp == 32773 || ccitt)) return -1;
    if (ccitt && !(bps == 1 && spp == 1 && phot <= 1)) return -1;
    if (comp == 3 && (in->t4options & ~5)) return -1;   // (uncompressed mode: not handled)
    if (comp == 4 && (in->t4options & 2)) return -1;
    if (phot == 0 || phot == 1) {
        if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16) return -1;
        if (bps < 8 && spp != 1) return -1;
    } else if (phot == 3) {
        if (bps != 1 && bps != 2 && bps != 4 && bps != 8) return -1;
        if (spp != 1) return -1;
        Entry cm = tag(&t, ifd, 320);
        if (!cm.found || cm.type != 3 || cm.count != 3u << bps) return -1;
        in->cmap_pos = cm.pos;
    } else if (phot == 2 || phot == 5) {
        if (bps != 8 && bps != 16) return -1;
        if (phot == 5 && get(&t, ifd, 332, 1) != 1) return -1;   // (InkSet: CMYK)
    } else {
        return -1;   // YCbCr, CIELab ...
    }
    if (in->planar != 1 && in->planar != 2) return -1;
    if (in->predictor != 1 && !(in->predictor == 2 && (bps == 8 || bps == 16) && !ccitt)) return -1;
    if (in->fillorder != 1 && !(in->fillorder == 2 && ccitt)) return -1;
    // strips or tiles
    Entry offs, cnts;
    if (tag(&t, ifd, 322).found) {
        int tw = (int)get(&t, ifd, 322, 0), th = (int)get(&t, ifd, 323, 0);
        if (tw <= 0 || th <= 0 || tw > 1 << 16 || th > 1 << 16) return -1;
        in->tiled = 1;
        in->cw = tw;
        in->ch = th;
        in->across = (w + tw - 1) / tw;
        in->down = (h + th - 1) / th;
        offs = tag(&t, ifd, 324);
        cnts = tag(&t, ifd, 325);
    } else {
        uint32_t rps = get(&t, ifd, 278, (uint32_t)h);
        in->cw = w;
        in->ch = rps && rps < (uint32_t)h ? (int)rps : h;
        in->across = 1;
        in->down = (h + in->ch - 1) / in->ch;
        offs = tag(&t, ifd, 273);
        cnts = tag(&t, ifd, 279);
    }
    in->nchunks = in->across * in->down * (in->planar == 2 ? spp : 1);
    if (!offs.found || !cnts.found || offs.count < (uint32_t)in->nchunks || cnts.count < (uint32_t)in->nchunks ||
        (offs.type != 3 && offs.type != 4) || (cnts.type != 3 && cnts.type != 4))
        return -1;
    in->off_pos = offs.pos;
    in->off_type = offs.type;
    in->cnt_pos = cnts.pos;
    in->cnt_type = cnts.type;
    return 0;
}

int nt_dir_size(const uint8_t *dir, size_t n, int le, int *width, int *height, int *orientation) {
    Tiff t = {dir, n, le};
    uint32_t count = t16(&t, 0);
    int w = 0, h = 0, o = 1;
    for (uint32_t i = 0; i < count && 2 + 12 * (size_t)i + 12 <= n; i++) {
        size_t p = 2 + 12 * (size_t)i;
        uint32_t id = t16(&t, p), type = t16(&t, p + 2), v = type == 3 ? t16(&t, p + 8) : type == 4 ? t32(&t, p + 8) : 0;
        if (id == 256) w = (int)v;
        else if (id == 257) h = (int)v;
        else if (id == 274 && v >= 1 && v <= 8) o = (int)v;
    }
    if (w <= 0 || h <= 0) return -1;
    *width = w;
    *height = h;
    *orientation = o;
    return 0;
}

// ---------------------------------------------------------------------------
// decompression of one strip / tile

static void packbits(const uint8_t *s, size_t sl, uint8_t *d, size_t dl) {
    size_t i = 0, o = 0;
    while (i < sl && o < dl) {
        int n = (int8_t)s[i++];
        if (n >= 0) {   // n + 1 literal bytes
            size_t c = MIN((size_t)n + 1, MIN(dl - o, sl - i));
            memcpy(d + o, s + i, c);
            o += c;
            i += (size_t)n + 1;
        } else if (n != -128) {   // the next byte 1 - n times
            if (i >= sl) break;
            size_t c = MIN((size_t)(1 - n), dl - o);
            memset(d + o, s[i++], c);
            o += c;
        }
    }
    if (o < dl) memset(d + o, 0, dl - o);   // (short data: the rest stays black)
}

// TIFF LZW: codes of 9..12 bits MSB first, "early change". A code's string is kept as where it was
// first written in the output and its length: decoding a code is one copy from earlier output.
static int lzw(const uint8_t *s, size_t sl, uint8_t *d, size_t dl) {
    if (sl >= 2 && s[0] == 0 && (s[1] & 1)) return -1;   // (old-style LZW, LSB first: not handled)
    uint32_t *pos = malloc(4096 * sizeof *pos);
    uint16_t *len = malloc(4096 * sizeof *len);
    if (!pos || !len) {
        free(pos);
        free(len);
        return -1;
    }
    uint64_t buf = 0;
    int nbits = 0, width = 9, next = 258, prev = -1, rc = 0;
    size_t i = 0, o = 0, prev_pos = 0;
    uint32_t prev_len = 0;
    while (o < dl) {
        while (nbits <= 56 && i < sl) {
            buf |= (uint64_t)s[i++] << (56 - nbits);
            nbits += 8;
        }
        if (nbits < width) break;   // (out of data)
        int code = (int)(buf >> (64 - width));
        buf <<= width;
        nbits -= width;
        if (code == 257) break;   // EOI
        if (code == 256) {        // clear
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        size_t at = o;
        uint32_t n;
        if (code < 256) {
            d[o++] = (uint8_t)code;
            n = 1;
        } else if (code < next && prev >= 0) {
            n = len[code];
            if (n > dl - o) n = (uint32_t)(dl - o);
            memcpy(d + o, d + pos[code], n);
            o += n;
        } else if (code == next && prev >= 0) {   // KwKwK: the previous string + its first byte
            n = prev_len + 1;
            if (n > dl - o) n = (uint32_t)(dl - o);
            memcpy(d + o, d + prev_pos, MIN(n, prev_len));
            if (n > prev_len) d[o + prev_len] = d[prev_pos];
            o += n;
        } else {
            rc = -1;   // (a code not defined yet)
            break;
        }
        if (prev >= 0 && next < 4096) {   // new string: the previous one + the first byte of this one
            pos[next] = (uint32_t)prev_pos;
            len[next] = (uint16_t)MIN(prev_len + 1, 65535);
            next++;
            if (next == (1 << width) - 1 && width < 12) width++;
        }
        prev = code;
        prev_pos = at;
        prev_len = n;
    }
    if (o < dl) memset(d + o, 0, dl - o);
    free(pos);
    free(len);
    return rc;
}

// ---------------------------------------------------------------------------
// CCITT G3 (MH, T.4) and G4 (T.6): bilevel, runs of white and black

typedef struct { int16_t run; uint8_t len; } Code;
static Code g_white[1 << 13], g_black[1 << 13];   // the next 13 bits -> run length, code length
static uint8_t g_lut16[65536];                    // 16-bit sample -> 8 bits, rounded
static uint8_t g_rev[256];                        // bit order reversed (FillOrder 2)

static const char *const white_codes[] = {   // run lengths 0..63, then 64..1728 in steps of 64
    "00110101", "000111", "0111", "1000", "1011", "1100", "1110", "1111", "10011", "10100", "00111", "01000",
    "001000", "000011", "110100", "110101", "101010", "101011", "0100111", "0001100", "0001000", "0010111",
    "0000011", "0000100", "0101000", "0101011", "0010011", "0100100", "0011000", "00000010", "00000011",
    "00011010", "00011011", "00010010", "00010011", "00010100", "00010101", "00010110", "00010111", "00101000",
    "00101001", "00101010", "00101011", "00101100", "00101101", "00000100", "00000101", "00001010", "00001011",
    "01010010", "01010011", "01010100", "01010101", "00100100", "00100101", "01011000", "01011001", "01011010",
    "01011011", "01001010", "01001011", "00110010", "00110011", "00110100",
    "11011", "10010", "010111", "0110111", "00110110", "00110111", "01100100", "01100101", "01101000",
    "01100111", "011001100", "011001101", "011010010", "011010011", "011010100", "011010101", "011010110",
    "011010111", "011011000", "011011001", "011011010", "011011011", "010011000", "010011001", "010011010",
    "011000", "010011011"};
static const char *const black_codes[] = {
    "0000110111", "010", "11", "10", "011", "0011", "0010", "00011", "000101", "000100", "0000100", "0000101",
    "0000111", "00000100", "00000111", "000011000", "0000010111", "0000011000", "0000001000", "00001100111",
    "00001101000", "00001101100", "00000110111", "00000101000", "00000010111", "00000011000", "000011001010",
    "000011001011", "000011001100", "000011001101", "000001101000", "000001101001", "000001101010",
    "000001101011", "000011010010", "000011010011", "000011010100", "000011010101", "000011010110",
    "000011010111", "000001101100", "000001101101", "000011011010", "000011011011", "000001010100",
    "000001010101", "000001010110", "000001010111", "000001100100", "000001100101", "000001010010",
    "000001010011", "000000100100", "000000110111", "000000111000", "000000100111", "000000101000",
    "000001011000", "000001011001", "000000101011", "000000101100", "000001011010", "000001100110",
    "000001100111",
    "0000001111", "000011001000", "000011001001", "000001011011", "000000110011", "000000110100",
    "000000110101", "0000001101100", "0000001101101", "0000001001010", "0000001001011", "0000001001100",
    "0000001001101", "0000001110010", "0000001110011", "0000001110100", "0000001110101", "0000001110110",
    "0000001110111", "0000001010010", "0000001010011", "0000001010100", "0000001010101", "0000001011010",
    "0000001011011", "0000001100100", "0000001100101"};
static const char *const ext_codes[] = {   // both colours: 1792..2560 in steps of 64
    "00000001000", "00000001100", "00000001101", "000000010010", "000000010011", "000000010100",
    "000000010101", "000000010110", "000000010111", "000000011100", "000000011101", "000000011110",
    "000000011111"};

static void add_code(Code *t, const char *bits, int run) {
    int n = (int)strlen(bits), v = 0;
    for (int i = 0; i < n; i++) v = v << 1 | (bits[i] == '1');
    for (int k = 0; k < 1 << (13 - n); k++) {
        t[v << (13 - n) | k].run = (int16_t)run;
        t[v << (13 - n) | k].len = (uint8_t)n;
    }
}

static void init_tables(void) {
    for (int i = 0; i < 91; i++) {
        int run = i < 64 ? i : (i - 63) * 64;
        add_code(g_white, white_codes[i], run);
        add_code(g_black, black_codes[i], run);
    }
    for (int i = 0; i < 13; i++) {
        add_code(g_white, ext_codes[i], 1792 + 64 * i);
        add_code(g_black, ext_codes[i], 1792 + 64 * i);
    }
    for (int v = 0; v < 65536; v++) g_lut16[v] = (uint8_t)(((uint32_t)v * 255 + 32767) / 65535);
    for (int v = 0; v < 256; v++) {
        int r = 0;
        for (int b = 0; b < 8; b++) r |= (v >> b & 1) << (7 - b);
        g_rev[v] = (uint8_t)r;
    }
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

typedef struct {
    const uint8_t *p;
    size_t n;     // bytes
    size_t pos;   // bit position
} Bits;

static inline uint32_t peek(const Bits *b, int k) {   // the next k (<= 24) bits; zeros after the end
    size_t i = b->pos >> 3;
    uint32_t v = 0;
    for (int j = 0; j < 4; j++) v = v << 8 | (i + j < b->n ? b->p[i + j] : 0);
    return (v << (b->pos & 7)) >> (32 - k);
}

static int run_length(Bits *b, const Code *t) {   // one run: make-up codes + a terminating code
    int total = 0;
    for (;;) {
        Code c = t[peek(b, 13)];
        if (!c.len || b->pos > b->n * 8) return -1;
        b->pos += c.len;
        total += c.run;
        if (c.run < 64) return total;
    }
}

static int skip_eol(Bits *b) {   // an EOL (11+ zeros, a one; fill bits before it): 1 if there was one
    size_t at = b->pos;
    int z = 0;
    while (z < 4096 && b->pos < b->n * 8 && !peek(b, 1)) {
        b->pos++;
        z++;
    }
    if (z >= 11 && peek(b, 1)) {
        b->pos++;
        return 1;
    }
    b->pos = at;
    return 0;
}

static void black_run(uint8_t *row, int a, int e, int black_one) {   // pixels [a, e) black
    for (int x = a; x < e; x++) {
        if (black_one) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        else row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
    }
}

// A row has at most w + 1 changing elements (every pixel a new colour, starting with black); more:
// damaged (zero-length runs or vertical modes that don't move). cur / ref hold w + 8 (+ 2 at once, + 3 ends).
#define MAX_CHANGES(w) ((w) + 1)

// One row of runs (MH / T.4 1D): the changing elements into cur. Returns their count or -1.
static int row_1d(Bits *b, int w, int *cur) {
    int a = 0, color = 0, n = 0;
    while (a < w) {
        if (n > MAX_CHANGES(w)) return -1;
        int r = run_length(b, color ? g_black : g_white);
        if (r < 0) return -1;
        a = MIN(a + r, w);
        cur[n++] = a;
        color ^= 1;
    }
    return n;
}

// One row coded against the reference row ref (nr changing elements, then w, w sentinels).
static int row_2d(Bits *b, int w, const int *ref, int nr, int *cur) {
    int a0 = -1, color = 0, n = 0, bi = 0;
    while (a0 < w) {
        if (n > MAX_CHANGES(w)) return -1;
        bi = MAX(bi - 2, 0);   // (a vertical mode to the left may need an earlier one)
        while (bi < nr && (ref[bi] <= a0 || (bi & 1) != color)) bi++;
        int b1 = bi < nr ? ref[bi] : w, b2 = bi + 1 < nr ? ref[bi + 1] : w;
        uint32_t v = peek(b, 12);
        if (b->pos > b->n * 8) return -1;
        int d;
        if (v >> 11) { b->pos += 1; d = 0; }                        // V0      1
        else if (v >> 9 == 3) { b->pos += 3; d = 1; }               // VR1     011
        else if (v >> 9 == 2) { b->pos += 3; d = -1; }              // VL1     010
        else if (v >> 9 == 1) {                                      // H       001
            b->pos += 3;
            int s = MAX(a0, 0), r1 = run_length(b, color ? g_black : g_white), r2 = r1 < 0 ? -1 : run_length(b, color ? g_white : g_black);
            if (r2 < 0) return -1;
            int a1 = MIN(s + r1, w), a2 = MIN(a1 + r2, w);
            if (n && a1 < cur[n - 1]) return -1;
            cur[n++] = a1;
            cur[n++] = a2;
            a0 = a2;
            continue;
        } else if (v >> 8 == 1) {                                    // pass    0001
            b->pos += 4;
            a0 = b2;
            continue;
        }
        else if (v >> 6 == 3) { b->pos += 6; d = 2; }               // VR2     000011
        else if (v >> 6 == 2) { b->pos += 6; d = -2; }              // VL2     000010
        else if (v >> 5 == 3) { b->pos += 7; d = 3; }               // VR3     0000011
        else if (v >> 5 == 2) { b->pos += 7; d = -3; }              // VL3     0000010
        else return -1;                                              // extensions, EOL: not here
        int a1 = b1 + d;
        if (a1 < 0 || a1 > w || (n && a1 < cur[n - 1])) return -1;
        cur[n++] = a1;
        a0 = a1;
        color ^= 1;
    }
    return n;
}

// rows of one strip / tile: packed bits, prow bytes per row
static int ccitt(const nt_info *in, const uint8_t *s, size_t sl, uint8_t *d, int w, int rows, size_t prow) {
    uint8_t *rev = NULL;
    if (in->fillorder == 2) {
        if (!(rev = malloc(sl))) return -1;
        for (size_t i = 0; i < sl; i++) rev[i] = g_rev[s[i]];
        s = rev;
    }
    int *ref = malloc((size_t)(w + 8) * 2 * sizeof *ref), *cur = ref + w + 8, nr = 0, rc = 0;
    if (!ref) {
        free(rev);
        return -1;
    }
    Bits b = {s, sl, 0};
    ref[0] = ref[1] = ref[2] = w;   // (the row above the first one: all white)
    int black_one = in->photometric == 0;   // WhiteIsZero: a black pixel is 1
    for (int y = 0; y < rows; y++) {
        int n;
        if (in->compression == 4) {
            n = row_2d(&b, w, ref, nr, cur);
        } else if (in->compression == 3) {
            skip_eol(&b);
            int two_d = (in->t4options & 1) && !(peek(&b, 1) >> 0 & 1);
            if (in->t4options & 1) b.pos++;   // (the tag bit: 1 one-dimensional, 0 two-dimensional)
            n = two_d ? row_2d(&b, w, ref, nr, cur) : row_1d(&b, w, cur);
        } else {   // MH: rows start on a byte
            n = row_1d(&b, w, cur);
            b.pos = (b.pos + 7) & ~(size_t)7;
        }
        uint8_t *row = d + (size_t)y * prow;
        if (n < 0) {   // (damaged: the rest stays white)
            memset(row, black_one ? 0 : 0xFF, prow * (size_t)(rows - y));
            rc = y ? 0 : -1;
            break;
        }
        memset(row, black_one ? 0 : 0xFF, prow);
        for (int k = 0; k < n; k += 2) black_run(row, cur[k], k + 1 < n ? cur[k + 1] : w, black_one);
        int *t = ref;   // this row is the next one's reference
        ref = cur;
        cur = t;
        nr = n;
        ref[nr] = ref[nr + 1] = ref[nr + 2] = w;
    }
    free(MIN(ref, cur));
    free(rev);
    return rc;
}

// ---------------------------------------------------------------------------
// decoding

static void run(size_t n, int nthreads, nitro_body body) {   // nthreads 1: on the calling thread
    if (nthreads == 1)
        for (size_t i = 0; i < n; i++) body(i);
    else
        nitro_parallel(n, body);
}

static int chunk(const Tiff *t, const nt_info *in, int i, const uint8_t **p, size_t *n) {
    uint32_t o = in->off_type == 3 ? t16(t, in->off_pos + 2 * (size_t)i) : t32(t, in->off_pos + 4 * (size_t)i);
    uint32_t c = in->cnt_type == 3 ? t16(t, in->cnt_pos + 2 * (size_t)i) : t32(t, in->cnt_pos + 4 * (size_t)i);
    if (o >= t->len) return -1;
    *p = t->d + o;
    *n = MIN((size_t)c, t->len - o);
    return 0;
}

// horizontal predictor: each sample was stored as the difference from the one before it
static void unpredict(const nt_info *in, uint8_t *b, int rows, size_t rowbytes, int samples, int sp) {
    for (int y = 0; y < rows; y++) {
        uint8_t *r = b + (size_t)y * rowbytes;
        if (in->bps == 8) {
            for (int i = sp; i < samples; i++) r[i] = (uint8_t)(r[i] + r[i - sp]);
        } else if (in->le) {
            for (int i = sp; i < samples; i++) {
                uint16_t v = (uint16_t)((r[2 * i] | r[2 * i + 1] << 8) + (r[2 * (i - sp)] | r[2 * (i - sp) + 1] << 8));
                r[2 * i] = (uint8_t)v;
                r[2 * i + 1] = (uint8_t)(v >> 8);
            }
        } else {
            for (int i = sp; i < samples; i++) {
                uint16_t v = (uint16_t)((r[2 * i] << 8 | r[2 * i + 1]) + (r[2 * (i - sp)] << 8 | r[2 * (i - sp) + 1]));
                r[2 * i] = (uint8_t)(v >> 8);
                r[2 * i + 1] = (uint8_t)v;
            }
        }
    }
}

// one strip / tile: w x rows pixels into d (rowbytes per row)
static int decompress(const nt_info *in, const uint8_t *s, size_t sl, uint8_t *d, int w, int rows, size_t rowbytes, int nthreads) {
    size_t dl = rowbytes * (size_t)rows;
    switch (in->compression) {
    case 1:
        memcpy(d, s, MIN(sl, dl));
        if (sl < dl) memset(d + sl, 0, dl - sl);
        break;
    case 32773: packbits(s, sl, d, dl); break;
    case 5: if (lzw(s, sl, d, dl)) return -1; break;
    case 8: case 32946: if (np_zlib_decompress(s, sl, d, dl, nthreads)) return -1; break;
    default: return ccitt(in, s, sl, d, w, rows, rowbytes);
    }
    if (in->predictor == 2) {
        int sp = in->planar == 2 ? 1 : in->spp;
        unpredict(in, d, rows, rowbytes, w * sp, sp);
    }
    return 0;
}

// One row of samples (P planes) -> B G R X. s8: room for width x spp bytes.
static void convert_row(const nt_info *in, const uint8_t *const *src, int P, const uint8_t (*cmap)[3], uint8_t *s8, uint8_t *o) {
    int w = in->width, spp = in->spp, sp = P > 1 ? 1 : spp, bps = in->bps;
    size_t n = (size_t)w * sp;
    for (int p = 0; p < P; p++) {   // the samples, 8 bits each
        const uint8_t *r = src[p];
        if (bps == 8) {
            if (P == 1) memcpy(s8, r, n);
            else for (int x = 0; x < w; x++) s8[(size_t)x * spp + p] = r[x];
        } else if (bps == 16) {
            size_t step = P == 1 ? 1 : (size_t)spp, at = (size_t)p;
            if (in->le)
                for (size_t i = 0; i < n; i++, at += step) s8[at] = g_lut16[r[2 * i] | r[2 * i + 1] << 8];
            else
                for (size_t i = 0; i < n; i++, at += step) s8[at] = g_lut16[r[2 * i] << 8 | r[2 * i + 1]];
        } else {   // 1 / 2 / 4 bits, one sample per pixel
            int mask = (1 << bps) - 1, scale = in->photometric == 3 ? 1 : 255 / mask;
            for (int x = 0; x < w; x++) {
                int bit = x * bps;
                s8[x] = (uint8_t)((r[bit >> 3] >> (8 - bps - (bit & 7)) & mask) * scale);
            }
        }
    }
    const uint8_t *s = s8;
    int a = in->color;   // the alpha sample, if any
    switch (in->photometric) {
    case 0: case 1: {
        int inv = in->photometric == 0 ? 255 : 0;
        for (int x = 0; x < w; x++, s += spp, o += 4) {
            int v = s[0] ^ inv;
            if (in->alpha == 2) v = (v * s[a] + 127) / 255;
            o[0] = o[1] = o[2] = (uint8_t)v;
            o[3] = 255;
        }
        break;
    }
    case 3:
        for (int x = 0; x < w; x++, s++, o += 4) {
            o[0] = cmap[s[0]][2];
            o[1] = cmap[s[0]][1];
            o[2] = cmap[s[0]][0];
            o[3] = 255;
        }
        break;
    case 2:
        if (in->alpha == 2) {
            for (int x = 0; x < w; x++, s += spp, o += 4) {
                int al = s[a];
                o[0] = (uint8_t)((s[2] * al + 127) / 255);
                o[1] = (uint8_t)((s[1] * al + 127) / 255);
                o[2] = (uint8_t)((s[0] * al + 127) / 255);
                o[3] = 255;
            }
        } else {
            for (int x = 0; x < w; x++, s += spp, o += 4) {
                o[0] = s[2];
                o[1] = s[1];
                o[2] = s[0];
                o[3] = 255;
            }
        }
        break;
    case 5:   // CMYK: the naive conversion (no profile)
        for (int x = 0; x < w; x++, s += spp, o += 4) {
            int k = 255 - s[3], al = in->alpha == 2 ? s[a] : 255;
            int r = ((255 - s[0]) * k + 127) / 255, g = ((255 - s[1]) * k + 127) / 255, b = ((255 - s[2]) * k + 127) / 255;
            o[0] = (uint8_t)((b * al + 127) / 255);
            o[1] = (uint8_t)((g * al + 127) / 255);
            o[2] = (uint8_t)((r * al + 127) / 255);
            o[3] = 255;
        }
        break;
    }
}

int nt_decode(const uint8_t *data, size_t len, const nt_info *in, uint8_t *bgrx, size_t pitch, int nthreads) {
    pthread_once(&g_once, init_tables);
    Tiff t = {data, len, in->le};
    int w = in->width, h = in->height, P = in->planar == 2 ? in->spp : 1, sp = P > 1 ? 1 : in->spp;
    size_t prow = ((size_t)w * sp * in->bps + 7) / 8;   // bytes per row of a plane
    uint8_t cmap[256][3];
    if (in->photometric == 3) {   // 16-bit colour map (some writers put 8-bit values in it)
        int n = 1 << in->bps, eight = 1;
        for (int i = 0; i < 3 * n; i++) eight &= t16(&t, in->cmap_pos + 2 * (size_t)i) < 256;
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < n; i++) {
                uint32_t v = t16(&t, in->cmap_pos + 2 * (size_t)(c * n + i));
                cmap[i][c] = eight ? (uint8_t)v : g_lut16[v];
            }
    }
    const uint8_t **rows = malloc((size_t)h * P * sizeof *rows);   // row y of plane p: rows[p * h + y]
    uint8_t *raw = NULL;
    volatile int err = 0;
    volatile int *errp = &err;
    if (!rows) return -1;
    if (in->compression == 1 && !in->tiled && in->predictor == 1) {   // uncompressed strips: the rows straight from the file
        for (int p = 0; p < P && !err; p++)
            for (int s = 0; s < in->down && !err; s++) {
                const uint8_t *c;
                size_t n;
                int y0 = s * in->ch, nrows = MIN(in->ch, h - y0);
                if (chunk(&t, in, p * in->down + s, &c, &n) || (size_t)(c - data) + prow * nrows > len) err = 1;
                for (int y = 0; y < nrows && !err; y++) rows[(size_t)p * h + y0 + y] = c + (size_t)y * prow;
            }
    } else {   // the strips / tiles decompressed into one raw image, in parallel
        size_t plane = prow * h;
        raw = malloc(plane * P + 16);
        if (!raw) err = 1;
        int one = in->nchunks == 1 ? nthreads : 1;   // (a single chunk: Deflate can still go parallel)
        if (!err) run((size_t)in->nchunks, nthreads, ^(size_t i) {
            const uint8_t *c;
            size_t n;
            int k = (int)i, per = in->across * in->down, p = k / per, ty = k % per / in->across, tx = k % in->across;
            if (*errp || chunk(&t, in, k, &c, &n)) { *errp = 1; return; }
            if (!in->tiled) {
                int y0 = ty * in->ch, nrows = MIN(in->ch, h - y0);
                if (decompress(in, c, n, raw + plane * p + prow * y0, w, nrows, prow, one)) *errp = 1;
                return;
            }
            size_t trow = ((size_t)in->cw * sp * in->bps + 7) / 8;   // a tile, then its visible part copied
            uint8_t *tb = malloc(trow * in->ch);
            if (!tb || decompress(in, c, n, tb, in->cw, in->ch, trow, one)) {
                free(tb);
                *errp = 1;
                return;
            }
            int x0 = tx * in->cw, y0 = ty * in->ch, vw = MIN(in->cw, w - x0), vh = MIN(in->ch, h - y0);
            size_t xb = (size_t)x0 * sp * in->bps / 8, vb = ((size_t)vw * sp * in->bps + 7) / 8;
            for (int y = 0; y < vh; y++) memcpy(raw + plane * p + prow * (y0 + y) + xb, tb + trow * y, vb);
            free(tb);
        });
        for (int p = 0; p < P; p++)
            for (int y = 0; y < h; y++) rows[(size_t)p * h + y] = raw + plane * p + prow * y;
    }
    if (!err) {   // the conversion, in bands of rows
        const int band = 16;
        const uint8_t(*cm)[3] = (const uint8_t(*)[3])cmap;
        run((size_t)(h + band - 1) / band, nthreads, ^(size_t bi) {
            uint8_t *s8 = malloc((size_t)w * in->spp + 16);
            if (!s8) { *errp = 1; return; }
            for (int y = (int)bi * band; y < MIN(h, (int)bi * band + band); y++) {
                const uint8_t *src[8];
                for (int p = 0; p < P; p++) src[p] = rows[(size_t)p * h + y];
                convert_row(in, src, P, cm, s8, bgrx + pitch * y);
            }
            free(s8);
        });
    }
    free(raw);
    free(rows);
    return err ? -1 : 0;
}
