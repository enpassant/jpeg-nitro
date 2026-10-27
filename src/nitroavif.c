// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroavif: see nitroavif.h
#include "nitroavif.h"
#include "heif.h"       // the HEIF structure (shared with nitroheic)
#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)

#include <dav1d/dav1d.h>
#include <math.h>
#include <stdatomic.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// the dimg references of item `from` (a grid's tiles); returns their count, -1 if damaged
static int dimg_refs(const uint8_t *d, const Box *iref, uint32_t from, uint32_t *ids, int max) {
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
static int has_ref(const uint8_t *d, const Box *iref, const char *type, uint32_t from, uint32_t to) {
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
static uint32_t alpha_item(Heif *h, const Box *iref, uint32_t primary) {
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

// the AV1 tiles of an image item (a grid or one av01): their data; size of the first one. A single
// image in several pieces (a layered one): them in poff / plen (*npieces), else *npieces = 0.
static int tiles_of(Heif *h, const Box *iref, Item *it, size_t *off, size_t *len, int *ntiles, int *rows, int *cols,
                    int *tw, int *th, int *gw, int *gh, size_t *poff, size_t *plen, int *npieces) {
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
    } else if (it->type == FOURCC("av01")) {
        n = 1;
        ids[0] = it->id;
        *rows = *cols = 1;
        *gw = *gh = 0;
    } else {
        return -1;
    }
    for (int t = 0; t < n; t++) {
        Item *ti = item(h, ids[t]);
        if (!ti || ti->type != FOURCC("av01")) return -1;
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

typedef struct { int kind, arg; } Xf;   // 0: irot (arg x 90 degrees anticlockwise), 1: imir (arg 0: top-bottom, 1: left-right)

// A pixel of the image after transformations xf[0..n) back to before them (ws / hs: the sizes before each).
static void unmap(const Xf *xf, int n, const int *ws, const int *hs, int *x, int *y) {
    for (int k = n - 1; k >= 0; k--) {
        int W = ws[k], H = hs[k], u = *x, v = *y;
        if (xf[k].kind == 1) {
            if (xf[k].arg == 1) *x = W - 1 - u;
            else *y = H - 1 - v;
        } else if (xf[k].arg == 1) {   // (forward: (x, y) -> (y, W - 1 - x))
            *x = W - 1 - v;
            *y = u;
        } else if (xf[k].arg == 2) {
            *x = W - 1 - u;
            *y = H - 1 - v;
        } else if (xf[k].arg == 3) {   // (forward: (x, y) -> (H - 1 - y, x))
            *x = v;
            *y = H - 1 - u;
        }
    }
}

// The EXIF orientation of rotations and mirrorings in this order.
static int orient(const Xf *xf, int n) {
    static const int ex[9][4] = {{0}, {1, 0, 0, 1}, {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                                 {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    int m[4] = {1, 0, 0, 1};   // (as heif.h's orientation(), one step at a time)
    for (int k = 0; k < n; k++) {
        if (xf[k].kind == 0) {
            for (int r = 0; r < (xf[k].arg & 3); r++) {
                int t[4] = {m[2], m[3], -m[0], -m[1]};
                memcpy(m, t, sizeof m);
            }
        } else if (xf[k].arg == 1) {   // left and right exchanged
            m[0] = -m[0];
            m[1] = -m[1];
        } else {   // top and bottom
            m[2] = -m[2];
            m[3] = -m[3];
        }
    }
    for (int o = 1; o <= 8; o++)
        if (!memcmp(m, ex[o], sizeof m)) return o;
    return 1;
}

int na_read_info(const uint8_t *d, size_t len, na_info *info) {
    Heif *h = calloc(1, sizeof *h);   // (big: not on the stack)
    if (!h) return -1;
    int rc = -1;
    h->d = d;
    h->len = len;
    Box ftyp, meta;
    size_t o = 0;
    if (!next_box(d, &o, len, &ftyp) || ftyp.type != FOURCC("ftyp")) goto out;
    int brand = 0;
    for (size_t p = ftyp.start; p + 4 <= ftyp.end; p += 4)
        if (p != ftyp.start + 4 && (be32(d + p) == FOURCC("avif") || be32(d + p) == FOURCC("avis")))
            brand = 1;
    if (!brand || !find_box(d, 0, len, "meta", &meta) || meta.end - meta.start < 4) goto out;
    size_t ms = meta.start + 4;
    Box hdlr, pitm, iinf, iloc, iprp, iref = {0, 0, 0};
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
    memset(info, 0, sizeof *info);
    int gw, gh;
    if (tiles_of(h, &iref, pi, info->tile_off, info->tile_len, &info->ntiles, &info->rows, &info->cols, &info->tile_w,
                 &info->tile_h, &gw, &gh, info->cpiece_off, info->cpiece_len, &info->cpieces))
        goto out;
    info->width = gw;
    info->height = gh;
    uint32_t aid = alpha_item(h, &iref, primary);
    Item *ai = aid ? item(h, aid) : NULL;
    if (ai) {   // the alpha plane: the same tile layout
        int an, ar, ac, atw, ath, agw, agh;
        if (tiles_of(h, &iref, ai, info->alpha_off, info->alpha_len, &an, &ar, &ac, &atw, &ath, &agw, &agh, info->apiece_off,
                     info->apiece_len, &info->apieces) ||
            an != info->ntiles || ar != info->rows || ac != info->cols || atw != info->tile_w || ath != info->tile_h)
            goto out;
        info->alpha = 1;
        // prem: the colours are already multiplied by alpha, i.e. over black - as they are drawn
        if (has_ref(d, &iref, "prem", primary, aid)) info->alpha = 0;
    }
    // the transformations, in the order of the item's properties (HEIF applies them so): rotation,
    // mirroring and a crop (clap, by whole pixels) - the crop is mapped back onto the decoded image,
    // the rotation and mirroring become the EXIF orientation it is drawn with
    Xf xf[8];
    int nx = 0, gwid = info->width, ghei = info->height;
    int ws[9] = {gwid}, hs[9] = {ghei};   // the image's size after the first k transformations
    for (int i = 0; i < pi->nprops && nx < 8; i++) {
        int x = pi->props[i];
        if (x < 1 || x > h->nprops) continue;
        const Box *b = &h->props[x - 1];
        if (b->end <= b->start) continue;
        if (b->type == FOURCC("irot")) xf[nx++] = (Xf){0, d[b->start] & 3};
        else if (b->type == FOURCC("imir")) xf[nx++] = (Xf){1, d[b->start] & 1};
        else if (b->type != FOURCC("clap")) continue;
        else {   // the crop, in the image as the transformations before it left it
            if (b->end - b->start < 32 || info->crop_x || info->crop_y || info->width != gwid || info->height != ghei) goto out;
            const uint8_t *c = d + b->start;
            if (!be32(c + 4) || !be32(c + 12) || !be32(c + 20) || !be32(c + 28)) goto out;
            int vw = ws[nx], vh = hs[nx];
            double cw = (double)be32(c) / be32(c + 4), ch = (double)be32(c + 8) / be32(c + 12);
            double ox = (double)(int32_t)be32(c + 16) / be32(c + 20), oy = (double)(int32_t)be32(c + 24) / be32(c + 28);
            double left = ox + (vw - 1) / 2.0 - (cw - 1) / 2.0, top = oy + (vh - 1) / 2.0 - (ch - 1) / 2.0;
            left = floor(left + 0.5);   // (half a pixel: rounded up, as Link-U's PNG reference of it)
            top = floor(top + 0.5);
            if (cw != (int)cw || ch != (int)ch || left < 0 || top < 0 || cw < 1 || ch < 1 || left + cw > vw || top + ch > vh)
                goto out;
            int x0 = (int)left, y0 = (int)top, x1 = x0 + (int)cw - 1, y1 = y0 + (int)ch - 1;
            unmap(xf, nx, ws, hs, &x0, &y0);   // its corners on the decoded image
            unmap(xf, nx, ws, hs, &x1, &y1);
            info->crop_x = MIN(x0, x1);
            info->crop_y = MIN(y0, y1);
            info->width = abs(x1 - x0) + 1;
            info->height = abs(y1 - y0) + 1;
            continue;
        }
        ws[nx] = xf[nx - 1].kind == 0 && (xf[nx - 1].arg & 1) ? hs[nx - 1] : ws[nx - 1];
        hs[nx] = xf[nx - 1].kind == 0 && (xf[nx - 1].arg & 1) ? ws[nx - 1] : hs[nx - 1];
    }
    info->orientation = orient(xf, nx);
    const Box *op = prop(h, pi, "a1op");   // a layered image: which operating point
    if (op && op->end > op->start) info->op = d[op->start];
    if (info->width < 1 || info->height < 1 || info->crop_x + info->width > info->cols * info->tile_w ||
        info->crop_y + info->height > info->rows * info->tile_h || (int64_t)info->width * info->height > (1 << 28))
        goto out;
    for (int i = 0; i < pi->nprops; i++) {   // colr: an ICC profile and / or nclx
        int x = pi->props[i];
        if (x < 1 || x > h->nprops || h->props[x - 1].type != FOURCC("colr")) continue;
        const Box *col = &h->props[x - 1];
        if (col->end - col->start <= 4) continue;
        uint32_t kind = be32(d + col->start);
        if (kind == FOURCC("prof") || kind == FOURCC("rICC")) {
            info->icc = d + col->start + 4;
            info->icc_len = col->end - col->start - 4;
        } else if (kind == FOURCC("nclx") && col->end - col->start >= 11) {
            info->nclx = 1;
            info->matrix = (int)be16(d + col->start + 8);
            info->full = d[col->start + 10] >> 7;
        }
    }
    if (info->nclx && (info->matrix == 3 || info->matrix == 8 || info->matrix > 10)) goto out;   // (YCgCo ...)
    rc = 0;
out:
    free(h);
    return rc;
}

// ---------------------------------------------------------------------------
// decoding

static void keep(const uint8_t *buf, void *cookie) {   // (the data is the caller's file)
    (void)buf;
    (void)cookie;
}

// One AV1 still picture. threads: dav1d's (0: all CPUs). Returns 0 on success.
static int av1_picture(const uint8_t *p, size_t n, int threads, int op, Dav1dPicture *pic) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = threads;
    s.max_frame_delay = 1;
    s.all_layers = 0;   // (a layered image: the best layer of its operating point)
    s.operating_point = op >= 0 && op < 32 ? op : 0;
    s.logger.callback = NULL;   // (no messages about damaged files on stderr)
    Dav1dContext *c = NULL;
    if (dav1d_open(&c, &s)) return -1;
    Dav1dData data;
    memset(&data, 0, sizeof data);
    memset(pic, 0, sizeof *pic);
    int rc = -1;
    if (!dav1d_data_wrap(&data, p, n, keep, NULL)) {
        for (int k = 0; k < 1000; k++) {   // feed the data, take the picture; then drain
            int sr = data.sz ? dav1d_send_data(c, &data) : 0;
            if (sr < 0 && sr != DAV1D_ERR(EAGAIN)) break;
            int gr = dav1d_get_picture(c, pic);
            if (!gr) { rc = 0; break; }
            if (gr != DAV1D_ERR(EAGAIN)) break;
        }
        if (data.sz) dav1d_data_unref(&data);
    }
    dav1d_close(&c);
    return rc;
}

typedef struct {
    float kr, kb;          // luma weights of R and B (identity: RGB in the planes)
    int identity;
    float yoff, yscale, coff, cscale;   // sample -> 0..1 / -0.5..0.5
    float aoff, ascale;    // alpha
} Csp;

static int csp(const na_info *in, const Dav1dPicture *p, const Dav1dPicture *a, Csp *c) {
    // (nclx first; its "unspecified" matrix: the AV1 sequence header's, if that says one)
    int mtx = in->nclx && in->matrix != 2 ? in->matrix : (int)p->seq_hdr->mtrx, full = in->nclx ? in->full : p->seq_hdr->color_range;
    memset(c, 0, sizeof *c);
    switch (mtx) {
    case 0: c->identity = 1; break;
    case 1: c->kr = 0.2126f; c->kb = 0.0722f; break;    // BT.709
    case 4: c->kr = 0.30f; c->kb = 0.11f; break;        // FCC
    case 7: c->kr = 0.212f; c->kb = 0.087f; break;      // SMPTE 240M
    case 9: case 10: c->kr = 0.2627f; c->kb = 0.0593f; break;   // BT.2020
    case 2: case 5: case 6: c->kr = 0.299f; c->kb = 0.114f; break;   // BT.601 (2: unspecified)
    default: return -1;
    }
    int bd = p->p.bpc, sh = bd - 8;
    float max = (float)((1 << bd) - 1);
    if (full || c->identity) {
        c->yscale = c->cscale = 1 / max;
        c->coff = (float)(1 << (bd - 1));
    } else {
        c->yoff = (float)(16 << sh);
        c->yscale = 1.0f / (float)(219 << sh);
        c->coff = (float)(128 << sh);
        c->cscale = 1.0f / (float)(224 << sh);
    }
    if (a) {
        int abd = a->p.bpc, ash = abd - 8;
        if (a->seq_hdr->color_range) {
            c->ascale = 1.0f / (float)((1 << abd) - 1);
        } else {
            c->aoff = (float)(16 << ash);
            c->ascale = 1.0f / (float)(219 << ash);
        }
    }
    return 0;
}

// n samples of row y from x0 into dst (outside 0..lim-1: the edge sample)
static void load(const Dav1dPicture *p, int plane, int y, int x0, int n, int lim, float *dst) {
    const uint8_t *row = (const uint8_t *)p->data[plane] + (ptrdiff_t)y * p->stride[plane > 0];
    int a = MAX(0, -x0), b = MIN(n, lim - x0);
    if (p->p.bpc > 8)
        for (int i = a; i < b; i++) dst[i] = ((const uint16_t *)row)[x0 + i];
    else
        for (int i = a; i < b; i++) dst[i] = row[x0 + i];
    for (int i = 0; i < a; i++) dst[i] = dst[a];
    for (int i = b; i < n; i++) dst[i] = dst[b - 1];
}

static inline uint8_t to8(float v) {
    v = v < 0 ? 0 : v;
    v = v > 1 ? 1 : v;
    return (uint8_t)(int)(v * 255 + 0.5f);
}

// The w x h pixels of a decoded tile (and its alpha) from (sx, sy) -> B G R X at out, a row at a time:
// the samples (chroma upsampled) into float rows, then a loop the compiler vectorises. 0: done.
static int convert(const Dav1dPicture *p, const Dav1dPicture *a, const Csp *c, int sx, int sy, int w, int h, uint8_t *out,
                   size_t pitch) {
    int lay = p->p.layout, ssx = lay == DAV1D_PIXEL_LAYOUT_I420 || lay == DAV1D_PIXEL_LAYOUT_I422,
        ssy = lay == DAV1D_PIXEL_LAYOUT_I420, mono = lay == DAV1D_PIXEL_LAYOUT_I400;
    int cw = (p->p.w + ssx) >> ssx, ch = (p->p.h + ssy) >> ssy;
    // R, G, B from (Y, Cb, Cr) (0..1, -0.5..0.5); identity: G, B, R in the planes
    float g1 = 1 - c->kr - c->kb, m[9];
    if (mono) {
        const float k[9] = {1, 0, 0, 1, 0, 0, 1, 0, 0};
        memcpy(m, k, sizeof m);
    } else if (c->identity) {
        const float k[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
        memcpy(m, k, sizeof m);
    } else {
        const float k[9] = {1, 0, 2 - 2 * c->kr, 1, -c->kb * (2 - 2 * c->kb) / g1, -c->kr * (2 - 2 * c->kr) / g1, 1, 2 - 2 * c->kb, 0};
        memcpy(m, k, sizeof m);
    }
    float yo = c->yoff, ys = c->yscale, co = c->identity ? c->yoff : c->coff, cs = c->identity ? c->yscale : c->cscale;
    float ao = a ? c->aoff : 0, as = a ? c->ascale : 1;
    // the chroma samples a row needs: qa .. qa + qn - 1
    int qa = ssx ? (sx - 1) >> 1 : sx, qn = ssx ? ((sx + w - 2) >> 1) + 2 - qa : w;
    float *Yr = malloc(sizeof(float) * ((size_t)w * 4 + (size_t)qn * 2));
    if (!Yr) return -1;
    float *U = Yr + w, *V = U + w, *A = V + w, *t0 = A + w, *t1 = t0 + qn;
    if (mono)
        for (int i = 0; i < w; i++) U[i] = V[i] = co;
    if (!a)
        for (int i = 0; i < w; i++) A[i] = 1;
    for (int j = 0; j < h; j++) {
        int y = sy + j;
        load(p, 0, y, sx, w, p->p.w, Yr);
        for (int pl = 1; pl <= 2 && !mono; pl++) {
            float *d = pl == 1 ? U : V;
            if (ssy) {   // chroma rows around this one (linear, centred: as libjpeg's "fancy" upsampling)
                int r0 = (y - 1) >> 1, r1 = r0 + 1;
                float wy = y & 1 ? 0.25f : 0.75f;
                load(p, pl, MAX(r0, 0), qa, qn, cw, t0);
                load(p, pl, MIN(r1, ch - 1), qa, qn, cw, t1);
                for (int k = 0; k < qn; k++) t0[k] += wy * (t1[k] - t0[k]);
            } else {
                load(p, pl, y, qa, qn, cw, t0);
            }
            if (ssx)
                for (int i = 0; i < w; i++) {
                    int x = sx + i, k = ((x - 1) >> 1) - qa;
                    float f = x & 1 ? 0.25f : 0.75f;
                    d[i] = t0[k] + f * (t0[k + 1] - t0[k]);
                }
            else
                memcpy(d, t0, sizeof(float) * (size_t)w);
        }
        if (a) load(a, 0, y, sx, w, a->p.w, A);
        uint8_t *o = out + pitch * (size_t)j;
        for (int i = 0; i < w; i++) {
            float Y = (Yr[i] - yo) * ys, cb = (U[i] - co) * cs, cr = (V[i] - co) * cs, al = (A[i] - ao) * as;
            al = al < 0 ? 0 : al > 1 ? 1 : al;
            o[4 * i + 0] = to8((m[6] * Y + m[7] * cb + m[8] * cr) * al);
            o[4 * i + 1] = to8((m[3] * Y + m[4] * cb + m[5] * cr) * al);
            o[4 * i + 2] = to8((m[0] * Y + m[1] * cb + m[2] * cr) * al);
            o[4 * i + 3] = 255;
        }
    }
    free(Yr);
    return 0;
}

// The AV1 data of a single image in several pieces, joined (malloc'ed); NULL: out of memory.
static uint8_t *join(const uint8_t *data, const size_t *off, const size_t *len, int n, size_t *total) {
    size_t t = 0;
    for (int k = 0; k < n; k++) t += len[k];
    uint8_t *b = malloc(t + 1);
    if (!b) return NULL;
    for (int k = 0, at = 0; k < n; at += (int)len[k], k++) memcpy(b + at, data + off[k], len[k]);
    *total = t;
    return b;
}

int na_decode(const uint8_t *data, size_t len, const na_info *in, uint8_t *bgrx, size_t pitch, int nthreads) {
    (void)len;
    _Atomic int failed = 0;
    _Atomic int *pfailed = &failed;
    const int W = in->width, H = in->height, cx = in->crop_x, cy = in->crop_y;
    if (in->ntiles == 1) {   // one picture: dav1d's threads, then the rows in parallel
        Dav1dPicture p, a;
        int dt = nthreads <= 0 ? 0 : nthreads, ok = 0;
        const uint8_t *cd = data + in->tile_off[0], *ad = data + in->alpha_off[0];
        size_t cn = in->tile_len[0], an = in->alpha_len[0];
        uint8_t *cj = in->cpieces ? join(data, in->cpiece_off, in->cpiece_len, in->cpieces, &cn) : NULL;
        uint8_t *aj = in->alpha && in->apieces ? join(data, in->apiece_off, in->apiece_len, in->apieces, &an) : NULL;
        if (cj) cd = cj;
        if (aj) ad = aj;
        if ((in->cpieces && !cj) || (in->alpha && in->apieces && !aj) || av1_picture(cd, cn, dt, in->op, &p)) {
            free(cj);
            free(aj);
            return -1;
        }
        ok = p.p.w >= cx + W && p.p.h >= cy + H;
        if (ok && in->alpha) {
            ok = !av1_picture(ad, an, dt, in->op, &a) && a.p.w >= cx + W && a.p.h >= cy + H;
            if (!ok && a.data[0]) dav1d_picture_unref(&a);
        }
        Csp c;
        if (ok && !csp(in, &p, in->alpha ? &a : NULL, &c)) {
            const int band = 16;
            const Dav1dPicture *pp = &p, *ap = in->alpha ? &a : NULL;
            const Csp *cp = &c;
            size_t nb = (size_t)(H + band - 1) / band;
            nitro_body body = ^(size_t b) {
                int y0 = (int)b * band, y1 = MIN(H, y0 + band);
                if (convert(pp, ap, cp, cx, cy + y0, W, y1 - y0, bgrx + pitch * y0, pitch)) atomic_store(pfailed, 1);
            };
            if (nthreads == 1)
                for (size_t b = 0; b < nb; b++) body(b);
            else
                nitro_parallel(nb, body);
        } else {
            failed = 1;
        }
        dav1d_picture_unref(&p);
        if (ok && in->alpha) dav1d_picture_unref(&a);
        free(cj);
        free(aj);
        return failed ? -1 : 0;
    }
    // a grid: the tiles in parallel, each decoded and converted on one thread (its part inside the crop)
    nitro_body body = ^(size_t t) {
        int gx = (int)t % in->cols * in->tile_w, gy = (int)t / in->cols * in->tile_h;
        int x0 = MAX(gx, cx), x1 = MIN(gx + in->tile_w, cx + W), y0 = MAX(gy, cy), y1 = MIN(gy + in->tile_h, cy + H);
        if (x0 >= x1 || y0 >= y1 || atomic_load(pfailed)) return;
        Dav1dPicture p, a;
        if (av1_picture(data + in->tile_off[t], in->tile_len[t], 1, in->op, &p)) { atomic_store(pfailed, 1); return; }
        int ok = p.p.w >= x1 - gx && p.p.h >= y1 - gy, ha = 0;
        if (ok && in->alpha)
            ok = ha = !av1_picture(data + in->alpha_off[t], in->alpha_len[t], 1, in->op, &a) && a.p.w >= x1 - gx && a.p.h >= y1 - gy;
        Csp c;
        if (!ok || csp(in, &p, in->alpha ? &a : NULL, &c) ||
            convert(&p, in->alpha ? &a : NULL, &c, x0 - gx, y0 - gy, x1 - x0, y1 - y0, bgrx + pitch * (size_t)(y0 - cy) + (size_t)(x0 - cx) * 4,
                    pitch))
            atomic_store(pfailed, 1);
        dav1d_picture_unref(&p);
        if (ha) dav1d_picture_unref(&a);
    };
    if (nthreads == 1)
        for (size_t t = 0; t < (size_t)in->ntiles; t++) body(t);
    else
        nitro_parallel((size_t)in->ntiles, body);
    return atomic_load(&failed) ? -1 : 0;
}
