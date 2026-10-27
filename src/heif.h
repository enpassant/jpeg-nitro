// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// heif.h (internal): reading the HEIF structure - boxes, items, their locations and properties -
// shared by nitroheic (HEIC: HEVC) and nitroavif (AVIF: AV1). Header-only, static functions.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int contains(const uint8_t *d, size_t n, const char *word) {   // (memmem: not on Windows)
    size_t k = strlen(word);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(d + i, word, k)) return 1;
    return 0;
}

#define FOURCC(s) ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])

static inline uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t ben(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

typedef struct {
    uint32_t type;
    size_t start, end;   // contents (after the header)
} Box;

// The next box in [*pos, end); 0 at the end or on a damaged box.
static inline int next_box(const uint8_t *d, size_t *pos, size_t end, Box *b) {
    size_t o = *pos;
    if (o + 8 > end) return 0;
    uint64_t size = be32(d + o);
    size_t hdr = 8;
    if (size == 1) {
        if (o + 16 > end) return 0;
        size = ben(d + o + 8, 8);
        hdr = 16;
    } else if (size == 0)
        size = end - o;
    if (size < hdr || size > end - o) return 0;
    b->type = be32(d + o + 4);
    b->start = o + hdr;
    b->end = o + size;
    *pos = b->end;
    return 1;
}

static inline int find_box(const uint8_t *d, size_t start, size_t end, const char *type, Box *out) {
    Box b;
    size_t o = start;
    while (next_box(d, &o, end, &b))
        if (b.type == FOURCC(type)) {
            *out = b;
            return 1;
        }
    return 0;
}

#define MAX_ITEMS 512
#define MAX_PROPS 256

typedef struct {
    uint32_t id, type;
    int method;            // iloc construction method (0: file, 1: idat)
    size_t off, len;
    int located;
    uint16_t props[16];    // ipma: property indices (1-based)
    int nprops;
} Item;

typedef struct {
    const uint8_t *d;
    size_t len;
    Item items[MAX_ITEMS];
    int nitems;
    Box props[MAX_PROPS];
    int nprops;
    Box idat;
} Heif;

static inline Item *item(Heif *h, uint32_t id) {
    for (int i = 0; i < h->nitems; i++)
        if (h->items[i].id == id) return &h->items[i];
    return NULL;
}

// A property of an item by type (NULL if it has none).
static inline const Box *prop(Heif *h, const Item *it, const char *type) {
    for (int i = 0; i < it->nprops; i++) {
        int x = it->props[i];
        if (x >= 1 && x <= h->nprops && h->props[x - 1].type == FOURCC(type)) return &h->props[x - 1];
    }
    return NULL;
}

static inline int prop_index(Heif *h, const Item *it, const char *type) {
    for (int i = 0; i < it->nprops; i++) {
        int x = it->props[i];
        if (x >= 1 && x <= h->nprops && h->props[x - 1].type == FOURCC(type)) return x;
    }
    return 0;
}

// The bytes of an item (one extent, in the file or in idat).
static inline const uint8_t *item_data(Heif *h, const Item *it, size_t *n) {
    if (!it->located) return NULL;
    size_t base = 0, limit = h->len;
    if (it->method == 1) {
        if (!h->idat.end) return NULL;
        base = h->idat.start;
        limit = h->idat.end;
    } else if (it->method != 0)
        return NULL;
    if (it->off > limit - base || it->len > limit - base - it->off) return NULL;
    *n = it->len;
    return h->d + base + it->off;
}

static inline int parse_iinf(Heif *h, const Box *b) {
    const uint8_t *d = h->d;
    if (b->end - b->start < 6) return -1;
    int v = d[b->start];
    size_t o = b->start + 4 + (v == 0 ? 2 : 4);
    Box e;
    while (next_box(d, &o, b->end, &e)) {
        if (e.type != FOURCC("infe") || e.end - e.start < 12 || h->nitems >= MAX_ITEMS) continue;
        int iv = d[e.start];
        if (iv < 2) continue;
        Item *it = &h->items[h->nitems++];
        memset(it, 0, sizeof *it);
        if (iv == 2) {
            it->id = be16(d + e.start + 4);
            it->type = be32(d + e.start + 8);
        } else {
            if (e.end - e.start < 14) return -1;
            it->id = be32(d + e.start + 4);
            it->type = be32(d + e.start + 10);
        }
    }
    return 0;
}

static inline int parse_iloc(Heif *h, const Box *b) {
    const uint8_t *d = h->d;
    size_t o = b->start, e = b->end;
    if (e - o < 8) return -1;
    int v = d[o];
    o += 4;
    int osz = d[o] >> 4, lsz = d[o] & 15, bsz = d[o + 1] >> 4, isz = v >= 1 ? d[o + 1] & 15 : 0;
    o += 2;
    if ((osz != 0 && osz != 4 && osz != 8) || (lsz != 0 && lsz != 4 && lsz != 8) || (bsz != 0 && bsz != 4 && bsz != 8) ||
        (isz != 0 && isz != 4 && isz != 8))
        return -1;
    uint32_t count;
    if (v < 2) {
        count = be16(d + o);
        o += 2;
    } else {
        count = be32(d + o);
        o += 4;
    }
    for (uint32_t i = 0; i < count; i++) {
        size_t need = (v < 2 ? 2 : 4) + (v >= 1 ? 2 : 0) + 2 + (size_t)bsz + 2;
        if (o + need > e) return -1;
        uint32_t id = v < 2 ? be16(d + o) : be32(d + o);
        o += v < 2 ? 2 : 4;
        int method = 0;
        if (v >= 1) {
            method = d[o + 1] & 15;
            o += 2;
        }
        o += 2;   // data reference index
        uint64_t base = ben(d + o, bsz);
        o += bsz;
        int ext = (int)be16(d + o);
        o += 2;
        if (o + (size_t)ext * (isz + osz + lsz) > e) return -1;
        Item *it = item(h, id);
        for (int k = 0; k < ext; k++) {
            o += isz;
            uint64_t eo = ben(d + o, osz);
            o += osz;
            uint64_t el = ben(d + o, lsz);
            o += lsz;
            if (it && ext == 1) {   // (items in several pieces: not handled)
                it->method = method;
                it->off = (size_t)(base + eo);
                it->len = (size_t)el;
                it->located = 1;
            }
        }
    }
    return 0;
}

static inline int parse_iprp(Heif *h, const Box *b) {
    const uint8_t *d = h->d;
    Box ipco, ipma;
    if (!find_box(d, b->start, b->end, "ipco", &ipco) || !find_box(d, b->start, b->end, "ipma", &ipma)) return -1;
    size_t o = ipco.start;
    while (h->nprops < MAX_PROPS && next_box(d, &o, ipco.end, &h->props[h->nprops])) h->nprops++;
    o = ipma.start;
    if (ipma.end - o < 8) return -1;
    int v = d[o], wide = d[o + 3] & 1;
    uint32_t count = be32(d + o + 4);
    o += 8;
    for (uint32_t i = 0; i < count; i++) {
        if (o + (v < 1 ? 3 : 5) > ipma.end) return -1;
        uint32_t id = v < 1 ? be16(d + o) : be32(d + o);
        o += v < 1 ? 2 : 4;
        int n = d[o++];
        if (o + (size_t)n * (wide ? 2 : 1) > ipma.end) return -1;
        Item *it = item(h, id);
        for (int k = 0; k < n; k++) {
            int x = wide ? (int)(be16(d + o) & 0x7fff) : d[o] & 0x7f;
            o += wide ? 2 : 1;
            if (it && it->nprops < 16) it->props[it->nprops++] = (uint16_t)x;
        }
    }
    return 0;
}

// EXIF orientation of: rotation by irot * 90 degrees anticlockwise, then mirroring (imir).
static inline int orientation(int irot, int imir) {
    // 2x2 matrices (x right, y down) of the 8 EXIF orientations, index 1..8
    static const int ex[9][4] = {{0}, {1, 0, 0, 1}, {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                                 {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    int m[4] = {1, 0, 0, 1};
    for (int k = 0; k < (irot & 3); k++) {   // 90 degrees anticlockwise: (x, y) -> (y, -x)
        int n[4] = {m[2], m[3], -m[0], -m[1]};
        memcpy(m, n, sizeof m);
    }
    if (imir == 0) { m[0] = -m[0]; m[1] = -m[1]; }   // mirror about the vertical axis
    if (imir == 1) { m[2] = -m[2]; m[3] = -m[3]; }   // about the horizontal axis
    for (int o = 1; o <= 8; o++)
        if (!memcmp(m, ex[o], sizeof m)) return o;
    return 1;
}
