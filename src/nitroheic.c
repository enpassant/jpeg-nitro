// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroheic: see nitroheic.h
#include "nitroheic.h"
#include "nitrohevc.h"

#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int contains(const uint8_t *d, size_t n, const char *word) {   // (memmem: not on Windows)
    size_t k = strlen(word);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(d + i, word, k)) return 1;
    return 0;
}

#define FOURCC(s) ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t ben(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

typedef struct {
    uint32_t type;
    size_t start, end;   // contents (after the header)
} Box;

// The next box in [*pos, end); 0 at the end or on a damaged box.
static int next_box(const uint8_t *d, size_t *pos, size_t end, Box *b) {
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

static int find_box(const uint8_t *d, size_t start, size_t end, const char *type, Box *out) {
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

static Item *item(Heif *h, uint32_t id) {
    for (int i = 0; i < h->nitems; i++)
        if (h->items[i].id == id) return &h->items[i];
    return NULL;
}

// A property of an item by type (NULL if it has none).
static const Box *prop(Heif *h, const Item *it, const char *type) {
    for (int i = 0; i < it->nprops; i++) {
        int x = it->props[i];
        if (x >= 1 && x <= h->nprops && h->props[x - 1].type == FOURCC(type)) return &h->props[x - 1];
    }
    return NULL;
}

static int prop_index(Heif *h, const Item *it, const char *type) {
    for (int i = 0; i < it->nprops; i++) {
        int x = it->props[i];
        if (x >= 1 && x <= h->nprops && h->props[x - 1].type == FOURCC(type)) return x;
    }
    return 0;
}

// The bytes of an item (one extent, in the file or in idat).
static const uint8_t *item_data(Heif *h, const Item *it, size_t *n) {
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

static int parse_iinf(Heif *h, const Box *b) {
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

static int parse_iloc(Heif *h, const Box *b) {
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

static int parse_iprp(Heif *h, const Box *b) {
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
static int orientation(int irot, int imir) {
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

int nh_read_info(const uint8_t *d, size_t len, nh_info *info) {
    Heif *h = calloc(1, sizeof *h);   // (big: not on the stack)
    if (!h) return -1;
    int rc = -1, wide_primaries = 0;
    h->d = d;
    h->len = len;
    Box ftyp, meta, b;
    size_t o = 0;
    if (!next_box(d, &o, len, &ftyp) || ftyp.type != FOURCC("ftyp")) goto out;
    int brand = 0;
    for (size_t p = ftyp.start; p + 4 <= ftyp.end; p += 4)
        if (p != ftyp.start + 4 && (be32(d + p) == FOURCC("heic") || be32(d + p) == FOURCC("heix") ||
                                    be32(d + p) == FOURCC("mif1")))
            brand = 1;
    if (!brand || !find_box(d, 0, len, "meta", &meta) || meta.end - meta.start < 4) goto out;
    size_t ms = meta.start + 4;
    Box hdlr, pitm, iinf, iloc, iprp, iref = {0};
    if (!find_box(d, ms, meta.end, "hdlr", &hdlr) || hdlr.end - hdlr.start < 12 ||
        be32(d + hdlr.start + 8) != FOURCC("pict"))
        goto out;
    if (!find_box(d, ms, meta.end, "pitm", &pitm) || !find_box(d, ms, meta.end, "iinf", &iinf) ||
        !find_box(d, ms, meta.end, "iloc", &iloc) || !find_box(d, ms, meta.end, "iprp", &iprp))
        goto out;
    find_box(d, ms, meta.end, "iref", &iref);
    find_box(d, ms, meta.end, "idat", &h->idat);
    if (pitm.end - pitm.start < 6) goto out;
    uint32_t primary = d[pitm.start] == 0 ? be16(d + pitm.start + 4) : be32(d + pitm.start + 4);
    if (parse_iinf(h, &iinf) || parse_iloc(h, &iloc) || parse_iprp(h, &iprp)) goto out;
    Item *pi = item(h, primary);
    if (!pi) goto out;

    // tiles: dimg references of a grid; alpha planes (auxl to the primary): not handled
    memset(info, 0, sizeof *info);
    if (iref.end) {
        int v = d[iref.start];
        size_t ro = iref.start + 4;
        int idsz = v == 0 ? 2 : 4;
        while (next_box(d, &ro, iref.end, &b)) {
            if (b.end - b.start < (size_t)idsz + 2) continue;
            uint32_t from = idsz == 2 ? be16(d + b.start) : be32(d + b.start);
            uint32_t n = be16(d + b.start + idsz);
            size_t p = b.start + idsz + 2;
            if (p + (size_t)n * idsz > b.end) goto out;
            if (b.type == FOURCC("dimg") && from == primary) {
                if (n > 256) goto out;
                for (uint32_t k = 0; k < n; k++) info->tile_id[k] = idsz == 2 ? be16(d + p + 2 * k) : be32(d + p + 4 * k);
                info->ntiles = (int)n;
            }
            if (b.type == FOURCC("auxl")) {
                for (uint32_t k = 0; k < n; k++)
                    if ((idsz == 2 ? be16(d + p + 2 * k) : be32(d + p + 4 * k)) == primary) {
                        Item *ax = item(h, from);
                        const Box *c = ax ? prop(h, ax, "auxC") : NULL;
                        if (c && c->end - c->start > 4 &&
                            contains(d + c->start + 4, c->end - c->start - 4, "alpha"))
                            goto out;
                        if (c && c->end - c->start > 4 &&
                            contains(d + c->start + 4, c->end - c->start - 4, "auxid:1"))
                            goto out;
                    }
            }
        }
    }
    if (pi->type == FOURCC("grid")) {
        size_t n;
        const uint8_t *g = item_data(h, pi, &n);
        if (!g || n < 8 || info->ntiles < 1) goto out;
        int wide = g[1] & 1;
        info->rows = g[2] + 1;
        info->cols = g[3] + 1;
        if (n < (size_t)(wide ? 12 : 8)) goto out;
        info->width = (int)(wide ? be32(g + 4) : be16(g + 4));
        info->height = (int)(wide ? be32(g + 8) : be16(g + 6));
        if (info->rows * info->cols != info->ntiles) goto out;
    } else if (pi->type == FOURCC("hvc1")) {
        info->ntiles = 1;
        info->tile_id[0] = primary;
        info->rows = info->cols = 1;
    } else
        goto out;

    // the tiles: HEVC, one decoder configuration, one size, 8-bit 4:2:0
    int hvcc_index = 0;
    for (int t = 0; t < info->ntiles; t++) {
        Item *ti = item(h, info->tile_id[t]);
        if (!ti || ti->type != FOURCC("hvc1")) goto out;
        int x = prop_index(h, ti, "hvcC");
        const Box *sz = prop(h, ti, "ispe");
        if (!x || !sz || sz->end - sz->start < 12) goto out;
        int w = (int)be32(d + sz->start + 4), hh = (int)be32(d + sz->start + 8);
        if (t == 0) {
            hvcc_index = x;
            info->tile_w = w;
            info->tile_h = hh;
        } else if (x != hvcc_index || w != info->tile_w || hh != info->tile_h)
            goto out;
        if (ti != pi && (prop(h, ti, "clap") || prop(h, ti, "irot") || prop(h, ti, "imir"))) goto out;
        size_t n;
        const uint8_t *td = item_data(h, ti, &n);
        if (!td || ti->method != 0) goto out;
        info->tile_off[t] = (size_t)(td - d);
        info->tile_len[t] = n;
    }
    const Box *hc = &h->props[hvcc_index - 1];
    if (hc->end - hc->start < 23) goto out;
    const uint8_t *c = d + hc->start;
    if ((c[16] & 3) != 1 || (c[17] & 7) != 0 || (c[18] & 7) != 0) goto out;   // 4:2:0, 8-bit luma and chroma
    info->hvcc = c;
    info->hvcc_len = hc->end - hc->start;
    if (info->tile_w < 16 || info->tile_h < 16 || (info->tile_w | info->tile_h) & 1 || info->tile_w > 8192 ||
        info->tile_h > 8192)
        goto out;
    if (pi->type == FOURCC("hvc1")) {
        info->width = info->tile_w;
        info->height = info->tile_h;
    }
    // clean aperture: only a crop from the top left corner (it then trims the right and bottom
    // edges, like the grid's output size); other crops go to ImageIO
    const Box *cl = prop(h, pi, "clap");
    if (cl) {
        if (cl->end - cl->start < 32) goto out;
        const uint8_t *c = d + cl->start;
        double cw = (double)be32(c) / be32(c + 4), ch = (double)be32(c + 8) / be32(c + 12);
        double ox = (double)(int32_t)be32(c + 16) / be32(c + 20), oy = (double)(int32_t)be32(c + 24) / be32(c + 28);
        if (!be32(c + 4) || !be32(c + 12) || !be32(c + 20) || !be32(c + 28)) goto out;
        double left = ox + (info->width - 1) / 2.0 - (cw - 1) / 2.0, top = oy + (info->height - 1) / 2.0 - (ch - 1) / 2.0;
        if (left != 0 || top != 0 || cw != (int)cw || ch != (int)ch || cw < 1 || ch < 1 || cw > info->width ||
            ch > info->height)
            goto out;
        info->width = (int)cw;
        info->height = (int)ch;
    }
    int gw = info->cols * info->tile_w, gh = info->rows * info->tile_h;
    if (info->width < 1 || info->height < 1 || info->width > gw || info->height > gh) goto out;
    info->plane_w[0] = gw;
    info->plane_h[0] = gh;
    info->plane_w[1] = info->plane_w[2] = gw / 2;
    info->plane_h[1] = info->plane_h[2] = gh / 2;

    // rotation, mirroring, colour profile (of the primary image)
    const Box *r = prop(h, pi, "irot"), *m = prop(h, pi, "imir");
    int irot = r && r->end > r->start ? d[r->start] & 3 : 0;
    int imir = m && m->end > m->start ? d[m->start] & 1 : -1;
    info->orientation = orientation(irot, imir);
    for (int i = 0; i < pi->nprops; i++) {   // colr: an ICC profile and / or nclx
        int x = pi->props[i];
        if (x < 1 || x > h->nprops || h->props[x - 1].type != FOURCC("colr")) continue;
        const Box *col = &h->props[x - 1];
        if (col->end - col->start <= 4) continue;
        uint32_t kind = be32(d + col->start);
        if (kind == FOURCC("prof") || kind == FOURCC("rICC")) {
            info->icc = d + col->start + 4;
            info->icc_len = col->end - col->start - 4;
        } else if (kind == FOURCC("nclx")) {
            // the planes go to the GPU as full range BT.601 (as JPEG); colours: sRGB without a profile
            if (col->end - col->start < 11) goto out;
            uint32_t prim = be16(d + col->start + 4), matrix = be16(d + col->start + 8);
            int full = d[col->start + 10] >> 7;
            if (!full || (matrix != 2 && matrix != 5 && matrix != 6)) goto out;   // other YCbCr: ImageIO
            if (prim != 1 && prim != 2) wide_primaries = 1;   // (P3, BT.2020 ...: fine with an ICC profile)
        }
    }
    if (wide_primaries && !info->icc) goto out;
    rc = 0;
out:
    free(h);
    return rc;
}

// ---- decoding with nitrohevc, our own decoder (other systems; macOS: NITROHEVC=1 in the environment)
static int decode_own(const uint8_t *data, const nh_info *info, uint8_t *const planes[3], const size_t pitch[3]) {
    _Atomic int failed = 0;
    _Atomic int *pfailed = &failed;
    nitro_parallel((size_t)info->ntiles, ^(size_t t) {
        int col = (int)t % info->cols, row = (int)t / info->cols;
        uint8_t *p[3];
        for (int c = 0; c < 3; c++) {
            int sh = c ? 1 : 0;
            p[c] = planes[c] + (size_t)(row * (info->tile_h >> sh)) * pitch[c] + (size_t)col * (info->tile_w >> sh);
        }
        // (a grid: the tiles in parallel, each on one thread; a single picture: its CTB rows in parallel)
        if (nhevc_decode(info->hvcc, info->hvcc_len, data + info->tile_off[t], info->tile_len[t], info->tile_w,
                         info->tile_h, p, pitch, info->ntiles > 1 ? 1 : 0))
            atomic_store(pfailed, 1);
    });
    return atomic_load(&failed) ? -1 : 0;
}

#ifdef __APPLE__
// ---- decoding: one VideoToolbox session per worker, tiles handed out in order

typedef struct {
    const nh_info *info;
    uint8_t *const *planes;
    const size_t *pitch;
    int tile;
    _Atomic int *failed;
} Job;

static void tile_done(void *ref, void *src, OSStatus st, VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts,
                      CMTime dur) {
    Job *j = src;
    const nh_info *in = j->info;
    if (st || !img || CVPixelBufferGetPixelFormatType(img) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        (int)CVPixelBufferGetWidth(img) != in->tile_w || (int)CVPixelBufferGetHeight(img) != in->tile_h) {
        atomic_store(j->failed, 1);
        return;
    }
    // BT.601 coefficients: matrix_coeffs 6 (ITU_R_601_4) or 5 (BT.470BG, as Samsung phones write it, which
    // VideoToolbox names "YCbCrMatrix#5"); BT.709 ...: ImageIO's job
    CFTypeRef matrix = CVBufferCopyAttachment(img, kCVImageBufferYCbCrMatrixKey, NULL);
    int other = matrix && !CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4) &&
                !CFEqual(matrix, CFSTR("YCbCrMatrix#5"));
    if (matrix) CFRelease(matrix);
    if (other) {
        atomic_store(j->failed, 1);
        return;
    }
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    int col = j->tile % in->cols, row = j->tile / in->cols;
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
    for (int r = 0; r < in->tile_h; r++)
        memcpy(j->planes[0] + (size_t)(row * in->tile_h + r) * j->pitch[0] + (size_t)col * in->tile_w, y + r * ys,
               (size_t)in->tile_w);
    int cw = in->tile_w / 2, ch = in->tile_h / 2;
    for (int r = 0; r < ch; r++) {   // CbCr interleaved -> two planes
        const uint8_t *s = uv + r * uvs;
        uint8_t *cb = j->planes[1] + (size_t)(row * ch + r) * j->pitch[1] + (size_t)col * cw;
        uint8_t *cr = j->planes[2] + (size_t)(row * ch + r) * j->pitch[2] + (size_t)col * cw;
        for (int x = 0; x < cw; x++) {
            cb[x] = s[2 * x];
            cr[x] = s[2 * x + 1];
        }
    }
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}

static int decode_vt(const uint8_t *data, const nh_info *info, uint8_t *const planes[3], const size_t pitch[3],
                     int nthreads) {
    CFDataRef hvcc = CFDataCreate(NULL, info->hvcc, (CFIndex)info->hvcc_len);
    const void *ak[] = {CFSTR("hvcC")}, *av[] = {hvcc};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef fmt = NULL;
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_HEVC, info->tile_w, info->tile_h, ext, &fmt);
    CFRelease(ext);
    CFRelease(atoms);
    CFRelease(hvcc);
    if (st || !fmt) return -1;

    // Intel: the software decoder; in parallel it is several times faster than a hardware one
    // (a Radeon's serves one tile at a time). Apple Silicon: its media engine, with 4 sessions
    // (M1, 12 MP: ~26 ms; software ~65 ms, ImageIO ~59 ms)
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *pk[] = {kCVPixelBufferPixelFormatTypeKey}, *pv[] = {pfn};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, pk, pv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(pfn);
#if defined(__arm64__) || defined(__aarch64__)
    const CFBooleanRef hardware = kCFBooleanTrue;
    const int max_workers = 4;
#else
    const CFBooleanRef hardware = kCFBooleanFalse;
    const int max_workers = 8;
#endif
    const void *sk[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder}, *sv[] = {hardware};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    if (nthreads <= 0) {   // all CPUs, but at most max_workers: more decoders do not make it faster
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = n < 1 ? 1 : n > max_workers ? max_workers : (int)n;
    }
    if (nthreads > info->ntiles) nthreads = info->ntiles;
    _Atomic int next = 0, failed = 0;
    _Atomic int *pnext = &next, *pfailed = &failed;
    dispatch_apply((size_t)nthreads, DISPATCH_APPLY_AUTO, ^(size_t w) {
        (void)w;
        VTDecompressionOutputCallbackRecord cb = {tile_done, NULL};
        VTDecompressionSessionRef s = NULL;
        if (VTDecompressionSessionCreate(NULL, fmt, spec, attrs, &cb, &s) || !s) {
            atomic_store(pfailed, 1);
            return;
        }
        int t;
        while (!atomic_load(pfailed) && (t = atomic_fetch_add(pnext, 1)) < info->ntiles) {
            CMBlockBufferRef bb = NULL;
            CMSampleBufferRef sb = NULL;
            size_t n = info->tile_len[t];
            Job job = {info, planes, pitch, t, pfailed};
            if (CMBlockBufferCreateWithMemoryBlock(NULL, (void *)(data + info->tile_off[t]), n, kCFAllocatorNull, NULL, 0,
                                                   n, 0, &bb) ||
                CMSampleBufferCreateReady(NULL, bb, fmt, 1, 0, NULL, 1, &n, &sb) ||
                VTDecompressionSessionDecodeFrame(s, sb, 0, &job, NULL))
                atomic_store(pfailed, 1);
            VTDecompressionSessionWaitForAsynchronousFrames(s);   // (job lives on this stack)
            if (sb) CFRelease(sb);
            if (bb) CFRelease(bb);
        }
        VTDecompressionSessionInvalidate(s);
        CFRelease(s);
    });
    CFRelease(spec);
    CFRelease(attrs);
    CFRelease(fmt);
    return atomic_load(&failed) ? -1 : 0;
}
#endif

int nh_decode_planes(const uint8_t *data, size_t len, const nh_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads) {
    (void)len;
#ifdef __APPLE__
    if (!getenv("NITROHEVC")) return decode_vt(data, info, planes, pitch, nthreads);
#else
    (void)nthreads;
#endif
    return decode_own(data, info, planes, pitch);
}
