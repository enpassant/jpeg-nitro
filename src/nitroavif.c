// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroavif: see nitroavif.h
#include "nitroavif.h"
#include "heif.h"       // the HEIF structure (shared with nitroheic)
#include "nitro_os.h"   // parallel loop (GCD on macOS, a thread pool elsewhere)

#include <dav1d/dav1d.h>
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

// the AV1 tiles of an image item (a grid or one av01): their data; size of the first one
static int tiles_of(Heif *h, const Box *iref, Item *it, size_t *off, size_t *len, int *ntiles, int *rows, int *cols,
                    int *tw, int *th, int *gw, int *gh) {
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
                 &info->tile_h, &gw, &gh))
        goto out;
    info->width = gw;
    info->height = gh;
    uint32_t aid = alpha_item(h, &iref, primary);
    Item *ai = aid ? item(h, aid) : NULL;
    if (ai) {   // the alpha plane: the same tile layout
        int an, ar, ac, atw, ath, agw, agh;
        if (tiles_of(h, &iref, ai, info->alpha_off, info->alpha_len, &an, &ar, &ac, &atw, &ath, &agw, &agh) ||
            an != info->ntiles || ar != info->rows || ac != info->cols || atw != info->tile_w || ath != info->tile_h)
            goto out;
        info->alpha = 1;
    }
    // clean aperture: only a crop from the top left corner (it then trims the right and bottom edges)
    const Box *cl = prop(h, pi, "clap");
    if (cl) {
        if (cl->end - cl->start < 32) goto out;
        const uint8_t *c = d + cl->start;
        if (!be32(c + 4) || !be32(c + 12) || !be32(c + 20) || !be32(c + 28)) goto out;
        double cw = (double)be32(c) / be32(c + 4), ch = (double)be32(c + 8) / be32(c + 12);
        double ox = (double)(int32_t)be32(c + 16) / be32(c + 20), oy = (double)(int32_t)be32(c + 24) / be32(c + 28);
        double left = ox + (info->width - 1) / 2.0 - (cw - 1) / 2.0, top = oy + (info->height - 1) / 2.0 - (ch - 1) / 2.0;
        if (left != 0 || top != 0 || cw != (int)cw || ch != (int)ch || cw < 1 || ch < 1 || cw > info->width ||
            ch > info->height)
            goto out;
        info->width = (int)cw;
        info->height = (int)ch;
    }
    if (info->width < 1 || info->height < 1 || info->width > info->cols * info->tile_w ||
        info->height > info->rows * info->tile_h || (int64_t)info->width * info->height > (1 << 28))
        goto out;
    const Box *r = prop(h, pi, "irot"), *m = prop(h, pi, "imir");
    info->orientation = orientation(r && r->end > r->start ? d[r->start] & 3 : 0, m && m->end > m->start ? d[m->start] & 1 : -1);
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
static int av1_picture(const uint8_t *p, size_t n, int threads, Dav1dPicture *pic) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = threads;
    s.max_frame_delay = 1;
    s.all_layers = 0;   // (a layered image: its best layer)
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
    int mtx = in->nclx ? in->matrix : (int)p->seq_hdr->mtrx, full = in->nclx ? in->full : p->seq_hdr->color_range;
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

static inline float sample(const Dav1dPicture *p, int plane, int x, int y) {
    const uint8_t *row = (const uint8_t *)p->data[plane] + (ptrdiff_t)y * p->stride[plane > 0];
    return p->p.bpc > 8 ? (float)((const uint16_t *)row)[x] : (float)row[x];
}

static inline uint8_t to8(float v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255 + 0.5f)); }

// Rows [y0, y1) of a decoded tile (and its alpha) -> B G R X at out (its top left pixel), vw wide.
static void convert(const Dav1dPicture *p, const Dav1dPicture *a, const Csp *c, int y0, int y1, int vw, uint8_t *out,
                    size_t pitch) {
    int lay = p->p.layout, ssx = lay == DAV1D_PIXEL_LAYOUT_I420 || lay == DAV1D_PIXEL_LAYOUT_I422,
        ssy = lay == DAV1D_PIXEL_LAYOUT_I420, mono = lay == DAV1D_PIXEL_LAYOUT_I400;
    int cw = (p->p.w + ssx) >> ssx, ch = (p->p.h + ssy) >> ssy;
    float g1 = 1 - c->kr - c->kb;
    for (int y = y0; y < y1; y++) {
        // chroma rows around this one (linear, centred: as libjpeg's "fancy" upsampling)
        int r0 = y, r1 = y;
        float wy = 0;
        if (ssy) {
            r0 = (y - 1) >> 1;
            r1 = r0 + 1;
            wy = y & 1 ? 0.25f : 0.75f;
            r0 = MAX(r0, 0);
            r1 = MIN(r1, ch - 1);
        }
        uint8_t *o = out + pitch * (size_t)(y - y0);
        for (int x = 0; x < vw; x++, o += 4) {
            float Y = (sample(p, 0, x, y) - c->yoff) * c->yscale, cb = 0, cr = 0;
            if (!mono) {
                int q0 = x, q1 = x;
                float wx = 0;
                if (ssx) {
                    q0 = (x - 1) >> 1;
                    q1 = q0 + 1;
                    wx = x & 1 ? 0.25f : 0.75f;
                    q0 = MAX(q0, 0);
                    q1 = MIN(q1, cw - 1);
                }
                float u = (1 - wy) * ((1 - wx) * sample(p, 1, q0, r0) + wx * sample(p, 1, q1, r0)) +
                          wy * ((1 - wx) * sample(p, 1, q0, r1) + wx * sample(p, 1, q1, r1));
                float v = (1 - wy) * ((1 - wx) * sample(p, 2, q0, r0) + wx * sample(p, 2, q1, r0)) +
                          wy * ((1 - wx) * sample(p, 2, q0, r1) + wx * sample(p, 2, q1, r1));
                if (c->identity) {
                    cb = (u - c->yoff) * c->yscale;
                    cr = (v - c->yoff) * c->yscale;
                } else {
                    cb = (u - c->coff) * c->cscale;
                    cr = (v - c->coff) * c->cscale;
                }
            }
            float r, g, b;
            if (mono) {
                r = g = b = Y;
            } else if (c->identity) {
                g = Y;
                b = cb;
                r = cr;
            } else {
                r = Y + (2 - 2 * c->kr) * cr;
                b = Y + (2 - 2 * c->kb) * cb;
                g = (Y - c->kr * r - c->kb * b) / g1;
            }
            if (a) {
                float al = (sample(a, 0, x, y) - c->aoff) * c->ascale;
                al = al < 0 ? 0 : al > 1 ? 1 : al;
                r *= al;
                g *= al;
                b *= al;
            }
            o[0] = to8(b);
            o[1] = to8(g);
            o[2] = to8(r);
            o[3] = 255;
        }
    }
}

int na_decode(const uint8_t *data, size_t len, const na_info *in, uint8_t *bgrx, size_t pitch, int nthreads) {
    (void)len;
    _Atomic int failed = 0;
    _Atomic int *pfailed = &failed;
    if (in->ntiles == 1) {   // one picture: dav1d's threads, then the rows in parallel
        Dav1dPicture p, a;
        int dt = nthreads <= 0 ? 0 : nthreads;
        if (av1_picture(data + in->tile_off[0], in->tile_len[0], dt, &p)) return -1;
        int ok = p.p.w >= in->width && p.p.h >= in->height;
        if (ok && in->alpha) {
            ok = !av1_picture(data + in->alpha_off[0], in->alpha_len[0], dt, &a) && a.p.w >= in->width && a.p.h >= in->height;
            if (!ok && a.data[0]) dav1d_picture_unref(&a);
        }
        Csp c;
        if (ok && !csp(in, &p, in->alpha ? &a : NULL, &c)) {
            const int band = 16, H = in->height, W = in->width;
            const Dav1dPicture *pp = &p, *ap = in->alpha ? &a : NULL;
            const Csp *cp = &c;
            size_t nb = (size_t)(H + band - 1) / band;
            nitro_body body = ^(size_t b) {
                int y0 = (int)b * band, y1 = MIN(H, y0 + band);
                convert(pp, ap, cp, y0, y1, W, bgrx + pitch * y0, pitch);
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
        return failed ? -1 : 0;
    }
    // a grid: the tiles in parallel, each decoded and converted on one thread
    nitro_body body = ^(size_t t) {
        int col = (int)t % in->cols, row = (int)t / in->cols, x0 = col * in->tile_w, y0 = row * in->tile_h;
        int vw = MIN(in->tile_w, in->width - x0), vh = MIN(in->tile_h, in->height - y0);
        if (vw <= 0 || vh <= 0 || atomic_load(pfailed)) return;
        Dav1dPicture p, a;
        if (av1_picture(data + in->tile_off[t], in->tile_len[t], 1, &p)) { atomic_store(pfailed, 1); return; }
        int ok = p.p.w >= vw && p.p.h >= vh, ha = 0;
        if (ok && in->alpha) ok = ha = !av1_picture(data + in->alpha_off[t], in->alpha_len[t], 1, &a) && a.p.w >= vw && a.p.h >= vh;
        Csp c;
        if (ok && !csp(in, &p, in->alpha ? &a : NULL, &c)) convert(&p, in->alpha ? &a : NULL, &c, 0, vh, vw, bgrx + pitch * y0 + (size_t)x0 * 4, pitch);
        else atomic_store(pfailed, 1);
        dav1d_picture_unref(&p);
        if (ha) dav1d_picture_unref(&a);
    };
    if (nthreads == 1)
        for (size_t t = 0; t < (size_t)in->ntiles; t++) body(t);
    else
        nitro_parallel((size_t)in->ntiles, body);
    return atomic_load(&failed) ? -1 : 0;
}
