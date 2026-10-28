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
    int located;           // (one extent: off, len)
    int nextents;          // an item in several pieces (in the file): up to 8 of them
    size_t xoff[8], xlen[8];
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

// The pieces of an item in several pieces (in the file): their count (0: not such an item / damaged).
static inline int item_extents(Heif *h, const Item *it, size_t *off, size_t *len) {
    for (int k = 0; k < it->nextents; k++) {
        if (it->xoff[k] > h->len || it->xlen[k] > h->len - it->xoff[k]) return 0;
        off[k] = it->xoff[k];
        len[k] = it->xlen[k];
    }
    return it->nextents;
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
    if (v >= 2 && e - o < 10) return -1;   // (version 2: a 32-bit item count)
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
            if (it && ext == 1) {
                it->method = method;
                it->off = (size_t)(base + eo);
                it->len = (size_t)el;
                it->located = 1;
            } else if (it && ext <= 8 && method == 0) {   // (several pieces: only item_extents returns them)
                it->xoff[k] = (size_t)(base + eo);
                it->xlen[k] = (size_t)el;
                it->nextents = ext;
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

// ---------------------------------------------------------------------------
// what nitroheic and nitroavif do alike

// Opens a HEIF file: one of the brands (NULL-terminated) in ftyp, the meta box's items, locations
// and properties into h; *iref the item references box (end 0: none), *primary the primary item's
// id (an item of the file). -1 if it is not such a file or is damaged.
static inline int heif_open(Heif *h, const uint8_t *d, size_t len, const char *const *brands, Box *iref, uint32_t *primary) {
    h->d = d;
    h->len = len;
    Box ftyp, meta, hdlr, pitm, iinf, iloc, iprp;
    size_t o = 0;
    memset(iref, 0, sizeof *iref);
    if (!next_box(d, &o, len, &ftyp) || ftyp.type != FOURCC("ftyp")) return -1;
    int brand = 0;   // the major brand, or (after the version) a compatible one
    for (size_t p = ftyp.start; p + 4 <= ftyp.end; p += 4)
        for (int k = 0; p != ftyp.start + 4 && brands[k]; k++)
            if (be32(d + p) == FOURCC(brands[k])) brand = 1;
    if (!brand || !find_box(d, 0, len, "meta", &meta) || meta.end - meta.start < 4) return -1;
    size_t ms = meta.start + 4;
    if (!find_box(d, ms, meta.end, "hdlr", &hdlr) || hdlr.end - hdlr.start < 12 ||
        be32(d + hdlr.start + 8) != FOURCC("pict"))
        return -1;
    if (!find_box(d, ms, meta.end, "pitm", &pitm) || !find_box(d, ms, meta.end, "iinf", &iinf) ||
        !find_box(d, ms, meta.end, "iloc", &iloc) || !find_box(d, ms, meta.end, "iprp", &iprp))
        return -1;
    if (find_box(d, ms, meta.end, "iref", iref) && iref->end - iref->start < 4) iref->start = iref->end = 0;   // (empty: none)
    find_box(d, ms, meta.end, "idat", &h->idat);
    if (pitm.end - pitm.start < 6 || (d[pitm.start] != 0 && pitm.end - pitm.start < 8)) return -1;   // (v1: a 32-bit id)
    *primary = d[pitm.start] == 0 ? be16(d + pitm.start + 4) : be32(d + pitm.start + 4);
    if (parse_iinf(h, &iinf) || parse_iloc(h, &iloc) || parse_iprp(h, &iprp)) return -1;
    return item(h, *primary) ? 0 : -1;
}

// the dimg references of item `from` (a grid's tiles); returns their count, -1 if damaged
static inline int dimg_refs(const uint8_t *d, const Box *iref, uint32_t from, uint32_t *ids, int max) {
    if (!iref->end) return 0;
    int v = d[iref->start], idsz = v == 0 ? 2 : 4, n = 0;
    size_t ro = iref->start + 4;
    Box b;
    while (next_box(d, &ro, iref->end, &b)) {
        if (b.end - b.start < (size_t)idsz + 2) continue;
        uint32_t f = idsz == 2 ? be16(d + b.start) : be32(d + b.start), k = be16(d + b.start + idsz);
        size_t p = b.start + idsz + 2;
        if (p + (size_t)k * idsz > b.end) return -1;
        if (b.type != FOURCC("dimg") || f != from) continue;
        if (k > (uint32_t)max) return -1;
        for (uint32_t i = 0; i < k; i++) ids[i] = idsz == 2 ? be16(d + p + 2 * i) : be32(d + p + 4 * i);
        n = (int)k;
    }
    return n;
}

// Is there a reference of this type from item `from` to item `to`?
static inline int has_ref(const uint8_t *d, const Box *iref, const char *type, uint32_t from, uint32_t to) {
    if (!iref->end) return 0;
    int v = d[iref->start], idsz = v == 0 ? 2 : 4;
    size_t ro = iref->start + 4;
    Box b;
    while (next_box(d, &ro, iref->end, &b)) {
        if (b.type != FOURCC(type) || b.end - b.start < (size_t)idsz + 2) continue;
        uint32_t f = idsz == 2 ? be16(d + b.start) : be32(d + b.start), k = be16(d + b.start + idsz);
        size_t p = b.start + idsz + 2;
        if (f != from || p + (size_t)k * idsz > b.end) continue;
        for (uint32_t i = 0; i < k; i++)
            if ((idsz == 2 ? be16(d + p + 2 * i) : be32(d + p + 4 * i)) == to) return 1;
    }
    return 0;
}

// the alpha plane of the primary image: the item whose auxl reference points to it (0: none)
static inline uint32_t alpha_item(Heif *h, const Box *iref, uint32_t primary) {
    const uint8_t *d = h->d;
    if (!iref->end) return 0;
    int v = d[iref->start], idsz = v == 0 ? 2 : 4;
    size_t ro = iref->start + 4;
    Box b;
    while (next_box(d, &ro, iref->end, &b)) {
        if (b.type != FOURCC("auxl") || b.end - b.start < (size_t)idsz + 2) continue;
        uint32_t from = idsz == 2 ? be16(d + b.start) : be32(d + b.start), k = be16(d + b.start + idsz);
        size_t p = b.start + idsz + 2;
        if (p + (size_t)k * idsz > b.end) return 0;
        for (uint32_t i = 0; i < k; i++)
            if ((idsz == 2 ? be16(d + p + 2 * i) : be32(d + p + 4 * i)) == primary) {
                Item *ax = item(h, from);
                const Box *c = ax ? prop(h, ax, "auxC") : NULL;
                if (c && c->end - c->start > 4 && (contains(d + c->start + 4, c->end - c->start - 4, "alpha") ||
                                                   contains(d + c->start + 4, c->end - c->start - 4, "auxid:1")))
                    return from;
            }
    }
    return 0;
}

// The tiles of an image item `it` (a grid of them, or a single one, of type `tile`: "hvc1" /
// "av01"): their data (off / len; a single image in several pieces - a layered one - in poff /
// plen instead, *npieces of them, else 0), the grid (rows x cols of tw x th; the image gw x gh:
// the grid's output size, a single tile's size). The tiles must be of one size and have no
// transformations of their own; with cfg (a decoder configuration property, "hvcC"), they must
// share one, *cfg_index its property index. -1 if not so.
static inline int heif_tiles(Heif *h, const Box *iref, Item *it, const char *tile, size_t *off, size_t *len, int *ntiles,
                             int *rows, int *cols, int *tw, int *th, int *gw, int *gh, size_t *poff, size_t *plen,
                             int *npieces, const char *cfg, int *cfg_index) {
    *npieces = 0;
    const uint8_t *d = h->d;
    uint32_t ids[256];
    int n;
    if (it->type == FOURCC("grid")) {
        size_t gl;
        const uint8_t *g = item_data(h, it, &gl);
        n = dimg_refs(d, iref, it->id, ids, 256);
        if (!g || gl < 8 || n < 1) return -1;
        int wide = g[1] & 1;
        if (gl < (size_t)(wide ? 12 : 8)) return -1;
        *rows = g[2] + 1;
        *cols = g[3] + 1;
        *gw = (int)(wide ? be32(g + 4) : be16(g + 4));
        *gh = (int)(wide ? be32(g + 8) : be16(g + 6));
        if (*rows * *cols != n) return -1;
    } else if (it->type == FOURCC(tile)) {
        n = 1;
        ids[0] = it->id;
        *rows = *cols = 1;
        *gw = *gh = 0;
    } else {
        return -1;
    }
    for (int t = 0; t < n; t++) {
        Item *ti = item(h, ids[t]);
        if (!ti || ti->type != FOURCC(tile)) return -1;
        if (cfg) {
            int x = prop_index(h, ti, cfg);
            if (!x || (t > 0 && x != *cfg_index)) return -1;
            *cfg_index = x;
        }
        const Box *sz = prop(h, ti, "ispe");
        if (!sz || sz->end - sz->start < 12) return -1;
        int w = (int)be32(d + sz->start + 4), hh = (int)be32(d + sz->start + 8);
        if (t == 0) {
            *tw = w;
            *th = hh;
        } else if (w != *tw || hh != *th) {
            return -1;
        }
        if (ti != it && (prop(h, ti, "clap") || prop(h, ti, "irot") || prop(h, ti, "imir"))) return -1;
        size_t k;
        const uint8_t *td = item_data(h, ti, &k);
        if (!td && n == 1 && (*npieces = item_extents(h, ti, poff, plen)) > 0) continue;
        if (!td) return -1;
        off[t] = (size_t)(td - d);
        len[t] = k;
    }
    if (*tw < 1 || *th < 1 || *tw > 16384 || *th > 16384) return -1;
    if (n == 1) {
        *gw = *tw;
        *gh = *th;
    }
    *ntiles = n;
    return 0;
}

// The colr properties of an item: an ICC profile (*icc, *icc_len; NULL: none) and / or nclx
// (*nclx 1: *primaries, *matrix, *full; 0: none; -1: a damaged one).
static inline void heif_colr(Heif *h, const Item *pi, const uint8_t **icc, size_t *icc_len, int *nclx, int *primaries,
                             int *matrix, int *full) {
    const uint8_t *d = h->d;
    *icc = NULL;
    *icc_len = 0;
    *nclx = *primaries = *matrix = *full = 0;
    for (int i = 0; i < pi->nprops; i++) {
        int x = pi->props[i];
        if (x < 1 || x > h->nprops || h->props[x - 1].type != FOURCC("colr")) continue;
        const Box *col = &h->props[x - 1];
        if (col->end - col->start <= 4) continue;
        uint32_t kind = be32(d + col->start);
        if (kind == FOURCC("prof") || kind == FOURCC("rICC")) {
            *icc = d + col->start + 4;
            *icc_len = col->end - col->start - 4;
        } else if (kind == FOURCC("nclx")) {
            if (col->end - col->start < 11) {
                *nclx = -1;
                continue;
            }
            *nclx = 1;
            *primaries = (int)be16(d + col->start + 4);
            *matrix = (int)be16(d + col->start + 8);
            *full = d[col->start + 10] >> 7;
        }
    }
}

// A clean aperture box, for an image of vw x vh: the crop's size and its top left corner (both
// may be fractional, the corner negative). -1 if damaged.
static inline int heif_clap(const uint8_t *d, const Box *b, int vw, int vh, double *cw, double *ch, double *left, double *top) {
    if (b->end - b->start < 32) return -1;
    const uint8_t *c = d + b->start;
    if (!be32(c + 4) || !be32(c + 12) || !be32(c + 20) || !be32(c + 28)) return -1;
    *cw = (double)be32(c) / be32(c + 4);
    *ch = (double)be32(c + 8) / be32(c + 12);
    double ox = (double)(int32_t)be32(c + 16) / be32(c + 20), oy = (double)(int32_t)be32(c + 24) / be32(c + 28);
    *left = ox + (vw - 1) / 2.0 - (*cw - 1) / 2.0;
    *top = oy + (vh - 1) / 2.0 - (*ch - 1) / 2.0;
    return 0;
}

typedef struct { int kind, arg; } Xf;   // 0: irot (arg x 90 degrees anticlockwise), 1: imir (arg 0: top-bottom, 1: left-right)

// An item's irot / imir properties in their order (HEIF applies them so): up to max of them, their count.
static inline int heif_transforms(Heif *h, const Item *pi, Xf *xf, int max) {
    int n = 0;
    for (int i = 0; i < pi->nprops && n < max; i++) {
        int x = pi->props[i];
        if (x < 1 || x > h->nprops) continue;
        const Box *b = &h->props[x - 1];
        if (b->end <= b->start) continue;
        if (b->type == FOURCC("irot")) xf[n++] = (Xf){0, h->d[b->start] & 3};
        else if (b->type == FOURCC("imir")) xf[n++] = (Xf){1, h->d[b->start] & 1};
    }
    return n;
}

// The EXIF orientation of rotations and mirrorings in this order.
static inline int heif_orientation(const Xf *xf, int n) {
    // 2x2 matrices (x right, y down) of the 8 EXIF orientations, index 1..8
    static const int ex[9][4] = {{0}, {1, 0, 0, 1}, {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                                 {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    int m[4] = {1, 0, 0, 1};
    for (int k = 0; k < n; k++) {
        if (xf[k].kind == 0) {
            for (int r = 0; r < (xf[k].arg & 3); r++) {   // 90 degrees anticlockwise: (x, y) -> (y, -x)
                int t[4] = {m[2], m[3], -m[0], -m[1]};
                memcpy(m, t, sizeof m);
            }
        } else if (xf[k].arg == 1) {   // left and right exchanged
            m[0] = -m[0];
            m[1] = -m[1];
        } else {   // top and bottom (as libavif / libheif read imir 0)
            m[2] = -m[2];
            m[3] = -m[3];
        }
    }
    for (int o = 1; o <= 8; o++)
        if (!memcmp(m, ex[o], sizeof m)) return o;
    return 1;
}
