// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_load: see nitro_load.h
#include "nitro_load.h"
#include "nitrodng.h"
#include "nitroheic.h"
#include "nitrojpeg.h"
#include "nitropng.h"
#include "nitropsd.h"
#include "nitrotiff.h"
#include "nitrowebp.h"
#ifdef NV_DAV1D
#include "nitroavif.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define PAD 64   // zero bytes after the data read (the decoders may look a few bytes ahead)

// ---------------------------------------------------------------------------
// file names

const char *const nv_image_exts[] = {"jpg", "jpeg", "jpe", "jfif", "png", "heic", "heif", "avif", "webp", "tif", "tiff",
                                     "gif", "bmp", "psd", "psb", "cr2", "nef", "nrw", "arw", "raf", "dng", NULL};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// the extension (after the last dot of the last path component), NULL if there is none
static const char *extension(const char *path) {
    const char *name = path, *dot = NULL;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') name = c + 1;
    }
    for (const char *c = name; *c; c++)
        if (*c == '.') dot = c;
    return dot ? dot + 1 : NULL;
}

int nv_has_ext(const char *path, const char *const *exts) {
    const char *e = extension(path);
    if (!e) return 0;
    for (int i = 0; exts[i]; i++) {
        const char *a = e, *b = exts[i];
        while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
        if (!*a && !*b) return 1;
    }
    return 0;
}

int nv_is_image_name(const char *path) { return nv_has_ext(path, nv_image_exts); }

#ifdef _WIN32
int nv_has_ext_w(const wchar_t *path, const char *const *exts) {
    const wchar_t *name = path, *dot = NULL;
    for (const wchar_t *c = path; *c; c++)
        if (*c == L'/' || *c == L'\\') name = c + 1;
    for (const wchar_t *c = name; *c; c++)
        if (*c == L'.') dot = c;
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++) {
        const wchar_t *a = dot + 1;
        const char *b = exts[i];
        while (*a && *b && *a < 128 && lower((int)*a) == (int)*b) a++, b++;
        if (!*a && !*b) return 1;
    }
    return 0;
}

int nv_is_image_name_w(const wchar_t *path) { return nv_has_ext_w(path, nv_image_exts); }
#endif

char *nv_ext_list(char *buf, size_t n, const char *prefix, const char *sep) {
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; nv_image_exts[i] && o < n; i++)
        o += (size_t)snprintf(buf + o, n - o, "%s%s%s", i ? sep : "", prefix, nv_image_exts[i]);
    buf[n - 1] = 0;
    return buf;
}

// ---------------------------------------------------------------------------
// reading

static FILE *open_rb(const char *path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    FILE *f = _wfopen(w, L"rb");
    free(w);
    return f;
#else
    return fopen(path, "rb");
#endif
}

static int seek_to(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int64_t file_size(FILE *f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return -1;
    int64_t n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END)) return -1;
    int64_t n = (int64_t)ftello(f);
#endif
    return seek_to(f, 0) ? -1 : n;
}

static int read_all(FILE *f, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread((uint8_t *)buf + got, 1, n - got, f);
        if (!r) return -1;
        got += r;
    }
    return 0;
}

static int pread_all(FILE *f, void *buf, size_t n, uint64_t off) {
    if (seek_to(f, off)) return -1;
    return read_all(f, buf, n);
}

uint8_t *nv_read_file(const char *path, size_t *len) {
    FILE *f = open_rb(path);
    if (!f) return NULL;
    int64_t sz = file_size(f);
    uint8_t *d = NULL;
    if (sz > 0 && (uint64_t)sz < ((uint64_t)1 << 40) && (d = malloc((size_t)sz + PAD))) {
        if (read_all(f, d, (size_t)sz)) {
            free(d);
            d = NULL;
        } else {
            memset(d + sz, 0, PAD);
            *len = (size_t)sz;
        }
    }
    fclose(f);
    return d;
}

uint8_t *nv_read_psd_merged(const char *path, size_t *len) {
    FILE *f = open_rb(path);
    if (!f) return NULL;
    uint8_t *out = NULL, h[26], b[8];
    int64_t sz = file_size(f);
    if (sz <= 0 || pread_all(f, h, 26, 0) || memcmp(h, "8BPS", 4)) goto done;
    {
        uint64_t size = (uint64_t)sz, pos = 26;
        int psb = h[5] == 2;
        size_t L = psb ? 8 : 4;
        for (int k = 0; k < 2; k++) {   // colour mode data, image resources
            if (pread_all(f, b, 4, pos)) goto done;
            pos += 4 + ((uint64_t)b[0] << 24 | (uint64_t)b[1] << 16 | (uint64_t)b[2] << 8 | b[3]);
            if (pos > size) goto done;
        }
        uint64_t head = pos, lm = 0;   // bytes kept as they are
        if (pread_all(f, b, L, pos)) goto done;
        for (size_t i = 0; i < L; i++) lm = lm << 8 | b[i];
        uint64_t x = pos + L + lm;   // merged image data (compression field first)
        if (lm > size || x + 2 > size) goto done;
        int16_t count = 0;
        if (lm >= L + 2) {
            uint8_t c[10];
            if (pread_all(f, c, L + 2, pos + L)) goto done;
            uint64_t li = 0;
            for (size_t i = 0; i < L; i++) li = li << 8 | c[i];
            if (li >= 2) count = (int16_t)(c[L] << 8 | c[L + 1]);
        }
        uint64_t merged = size - x, total = head + L + L + 2 + merged;
        out = malloc((size_t)total + PAD);
        if (!out) goto done;
        if (pread_all(f, out, (size_t)head, 0)) { free(out); out = NULL; goto done; }
        uint8_t *q = out + head;
        uint64_t slim = L + 2;   // layer and mask section: layer info (length 2) + layer count
        for (size_t i = 0; i < L; i++) q[i] = (uint8_t)(slim >> (8 * (L - 1 - i)));
        for (size_t i = 0; i < L; i++) q[L + i] = (uint8_t)(2 >> (8 * (L - 1 - i)));
        q[2 * L] = (uint8_t)((uint16_t)count >> 8);
        q[2 * L + 1] = (uint8_t)count;
        if (pread_all(f, q + 2 * L + 2, (size_t)merged, x)) { free(out); out = NULL; goto done; }
        memset(out + total, 0, PAD);
        *len = (size_t)total;
    }
done:
    fclose(f);
    return out;
}

uint8_t *nv_read_raw_jpeg(const char *path, size_t *len, int *orientation) {
    static const char *const e[] = {"cr2", "nef", "nrw", "arw", "raf", NULL};
    if (!nv_has_ext(path, e)) return NULL;
    FILE *f = open_rb(path);
    if (!f) return NULL;
    uint8_t *out = NULL;
    int64_t sz = file_size(f);
    size_t cap = 1 << 20;   // (Nikon puts the SubIFD directories after the maker notes: ~270 KB in)
    uint8_t *hdr = malloc(cap);
    size_t n = hdr ? fread(hdr, 1, cap, f) : 0, off, size;
    if (n > 0 && sz > 0 && !nj_find_embedded(hdr, n, &off, &size, orientation) && off < (uint64_t)sz &&
        size <= (uint64_t)sz - off && size < ((size_t)1 << 31) && (out = malloc(size + PAD))) {
        if (!pread_all(f, out, size, off)) {
            memset(out + size, 0, PAD);
            *len = size;
        } else {
            free(out);
            out = NULL;
        }
    }
    free(hdr);
    fclose(f);
    return out;
}

uint8_t *nv_read_image(const char *path, size_t *len, int max_tex, int *raw_orientation, int *raw_read) {
    static const char *const psd[] = {"psd", "psb", NULL};
    uint8_t *data = NULL;
    *raw_orientation = 0;
    *raw_read = 0;
    if (nv_has_ext(path, psd)) {
        data = nv_read_psd_merged(path, len);
        ps_info pi;   // only if nitropsd can decode it: the platform's decoder gets the whole file
        if (data && (ps_read_info(data, *len, &pi) || !pi.supported || pi.width > max_tex || pi.height > max_tex)) {
            free(data);
            data = NULL;
        }
    }
    int orient = 0;   // a camera raw (CR2, NEF ...) shows its full-size JPEG (nitrojpeg finds it): only that is read
    if (!data && (data = nv_read_raw_jpeg(path, len, &orient))) {
        *raw_orientation = orient;   // (0: the JPEG's own, RAF)
        *raw_read = 1;
    }
    if (!data) data = nv_read_file(path, len);
    return data;
}

// ---------------------------------------------------------------------------
// decoding

const char *nv_mode_name(int mode) {
    switch (mode) {
    case 3: return "prog";
    case 2: return "split";
    case 1: return "RST";
    case 0: return "single";
    case NV_MODE_NITROPNG: return "nitropng";
    case NV_MODE_NITROPSD: return "nitropsd";
    case NV_MODE_SVG: return "svg";
    case NV_MODE_CDR: return "cdr";
    case NV_MODE_NITROHEIC: return "nitroheic";
    case NV_MODE_NITRODNG: return "nitrodng";
    case NV_MODE_NITROWEBP: return "nitrowebp";
    case NV_MODE_NITROTIFF: return "nitrotiff";
    case NV_MODE_NITROAVIF: return "nitroavif";
    default: return NULL;
    }
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

// planes of pw x ph at aligned pitches, one after the other: the offsets; returns the total size
static size_t plane_layout(NvImage *img, int n, const int *pw, const int *ph, size_t align) {
    size_t total = 0;
    for (int c = 0; c < n; c++) {
        img->pitch[c] = align_up((size_t)pw[c], align);
        img->offset[c] = total;
        total = align_up(total + img->pitch[c] * (size_t)ph[c], align);
        img->pw[c] = pw[c];
        img->ph[c] = ph[c];
    }
    return total;
}

// a buffer for a BGRA image of w x h (img describes it)
static uint8_t *bgra_buffer(const NvDecoder *dec, NvImage *img, int w, int h, size_t *cap) {
    size_t pitch = align_up((size_t)w * 4, dec->align);
    uint8_t *buf = dec->get(pitch * (size_t)h, cap, dec->ctx);
    if (!buf) return NULL;
    img->kind = NV_BGRA;
    img->width = w;
    img->height = h;
    img->pitch[0] = pitch;
    img->size = pitch * (size_t)h;
    img->bands = 1;
    return buf;
}

// 4:2:0 planes (HEIC, WebP) of w x h in a buffer of plane_layout's layout
static void yuv420(NvImage *img, int w, int h, size_t total) {
    img->kind = NV_YUV;
    img->width = w;
    img->height = h;
    img->ncomp = 3;
    img->h[0] = img->v[0] = 2;
    img->h[1] = img->h[2] = img->v[1] = img->v[2] = 1;
    img->hmax = img->vmax = 2;
    img->size = total;
}

#define GIVE_BACK(b) do { if (b) dec->put(b, *cap, dec->ctx); else oom = 1; b = NULL; } while (0)

int nv_decode(uint8_t *data, size_t *len, int raw_orientation, int raw_read, const NvDecoder *dec, NvImage *img,
              uint8_t **buf, size_t *cap) {
    size_t n = *len, total;
    int oom = 0, mt = dec->max_tex, orient = 0;
    uint8_t *b = NULL;
    memset(img, 0, sizeof *img);
    img->orientation = 1;
    *buf = NULL;
    *cap = 0;
    ndng_info di;   // a DNG: its raw image developed by nitrodng (else its largest preview below)
    if (!raw_read && !ndng_read_info(data, n, &di)) {
        if (di.width <= mt && di.height <= mt) {
            b = bgra_buffer(dec, img, di.width, di.height, cap);
            img->ev3 = dec->dng_ev3;
            di.ev = img->ev3 / 3.0f;
            if (b && !ndng_decode(data, n, &di, b, img->pitch[0], dec->threads)) {
                img->orientation = di.orientation;
                img->mode = NV_MODE_NITRODNG;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
        }
        img->error = "DNG decode failed";
    }
    size_t eoff, esize;
    if (!raw_read && !nj_find_embedded(data, n, &eoff, &esize, &orient) && eoff < n && esize <= n - eoff) {
        memmove(data, data + eoff, esize);   // (a raw file with another extension: read whole)
        memset(data + esize, 0, PAD);        // (the zero padding nv_read_file leaves after the data)
        n = *len = esize;
        raw_orientation = orient;
    }
    if (n >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        np_info pi;   // nitropng: 8-bit gray / gray+alpha / RGB / RGBA, non-interlaced: the rows as they are
        if (np_read_info(data, n, &pi) || !pi.convertible) {
            img->error = "Unsupported PNG (interlaced)";
        } else if (pi.width > mt || pi.height > mt) {
            img->orientation = pi.orientation;
            img->error = "Unsupported PNG (too large)";
        } else if (pi.supported) {
            img->orientation = pi.orientation;   // (eXIf chunk)
            b = dec->get(pi.raw_size, cap, dec->ctx);
            np_stats ps;
            if (b && !np_decode(data, n, &pi, b, dec->threads, &ps)) {
                img->kind = NV_PNG;
                img->width = pi.width;
                img->height = pi.height;
                img->ncomp = pi.channels;
                img->pitch[0] = pi.stride + 1;   // rows keep their filter-type byte
                img->size = pi.raw_size;
                img->bands = ps.chunks;
                img->mode = NV_MODE_NITROPNG;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
            img->error = "PNG decode failed";
        } else {   // palette, 1 / 2 / 4 / 16-bit, a transparency key: the rows decoded, then made BGRX (in parallel)
            img->orientation = pi.orientation;
            size_t rcap = 0;
            uint8_t *raw = dec->get(pi.raw_size, &rcap, dec->ctx);
            b = raw ? bgra_buffer(dec, img, pi.width, pi.height, cap) : NULL;
            np_stats ps;
            if (b && !np_decode(data, n, &pi, raw, dec->threads, &ps) &&
                !np_to_bgrx(data, n, &pi, raw, b, img->pitch[0], dec->threads)) {
                dec->put(raw, rcap, dec->ctx);
                img->bands = ps.chunks;
                img->mode = NV_MODE_NITROPNG;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
            if (raw) dec->put(raw, rcap, dec->ctx);
            else oom = 1;
            img->error = "PNG decode failed";
        }
    } else if (n >= 4 && !memcmp(data, "8BPS", 4)) {
        ps_info pi;   // nitropsd: merged image of 8-bit RGB / grayscale PSD and PSB
        if (ps_read_info(data, n, &pi) || !pi.supported) {
            img->error = "Unsupported PSD/PSB";
        } else if (pi.width > mt || pi.height > mt) {
            img->error = "Unsupported PSD/PSB (too large)";
        } else {
            size_t P = (size_t)(pi.ncolor + pi.alpha);
            b = dec->get(pi.plane_size * P, cap, dec->ctx);
            ps_stats ps;
            if (b && !ps_decode(data, n, &pi, b, dec->threads, &ps)) {
                img->kind = NV_PLANAR;
                img->width = pi.width;
                img->height = pi.height;
                img->ncomp = pi.ncolor;
                img->hasa = pi.alpha;
                img->pitch[0] = (size_t)pi.width;
                for (int c = 0; c < 3; c++) img->offset[c] = pi.ncolor == 3 ? c * pi.plane_size : 0;
                img->size = pi.plane_size * P;
                img->bands = 1;
                img->mode = NV_MODE_NITROPSD;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
            img->error = "PSD decode failed";
        }
    } else if (n >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        nj_info fi;
        int ok = !nj_read_info(data, n, &fi);
        img->orientation = raw_orientation ? raw_orientation : ok ? fi.orientation : 1;   // (for the fallback too)
        if (!ok || !fi.supported) {
            img->error = "Unsupported JPEG";
        } else if (fi.width > mt || fi.height > mt) {
            img->error = "Unsupported JPEG (too large)";
        } else {
            total = plane_layout(img, fi.ncomp, fi.plane_w, fi.plane_h, dec->align);
            b = dec->get(total, cap, dec->ctx);
            nj_stats st;
            if (b) {
                uint8_t *planes[3] = {b + img->offset[0], b + img->offset[1], b + img->offset[2]};
                if (!nj_decode_planes(data, n, &fi, planes, img->pitch, 0, &st)) {
                    img->kind = NV_YUV;
                    img->width = fi.width;
                    img->height = fi.height;
                    img->ncomp = fi.ncomp;
                    for (int c = 0; c < 3; c++) {
                        img->h[c] = c < fi.ncomp ? fi.h[c] : 1;
                        img->v[c] = c < fi.ncomp ? fi.v[c] : 1;
                    }
                    img->hmax = fi.hmax;
                    img->vmax = fi.vmax;
                    img->size = total;
                    img->mode = st.mode;
                    img->bands = st.bands;
                    *buf = b;
                    return 0;
                }
            }
            GIVE_BACK(b);
            img->error = "JPEG decode failed";
        }
    } else if (n >= 12 && !memcmp(data + 4, "ftyp", 4)) {
        nh_info hi;   // HEIC: its HEVC tiles decoded in parallel (nitroheic: VideoToolbox on macOS, else nitrohevc)
        if (nh_read_info(data, n, &hi)) {
            img->error = "Unsupported HEIC";
        } else if (hi.plane_w[0] > mt || hi.plane_h[0] > mt) {
            img->error = "Unsupported HEIC (too large)";
        } else {
            total = plane_layout(img, 3, hi.plane_w, hi.plane_h, dec->align);
            b = dec->get(total, cap, dec->ctx);
            if (b) {
                uint8_t *planes[3] = {b + img->offset[0], b + img->offset[1], b + img->offset[2]};
                if (!nh_decode_planes(data, n, &hi, planes, img->pitch, dec->threads)) {
                    yuv420(img, hi.width, hi.height, total);   // (full range BT.601 4:2:0, as a JPEG)
                    img->orientation = hi.orientation;
                    img->icc = hi.icc;
                    img->icc_len = hi.icc_len;
                    img->mode = NV_MODE_NITROHEIC;
                    img->bands = hi.ntiles;
                    *buf = b;
                    return 0;
                }
            }
            GIVE_BACK(b);
            img->error = "HEIC decode failed";
        }
#ifdef NV_DAV1D
        na_info ai;   // AVIF: AV1 through dav1d, our colour conversion (in parallel)
        if (na_read_info(data, n, &ai)) {
            if (!strcmp(img->error, "Unsupported HEIC")) img->error = "Unsupported AVIF";
        } else if (ai.width > mt || ai.height > mt) {
            img->error = "Unsupported AVIF (too large)";
        } else {
            b = bgra_buffer(dec, img, ai.width, ai.height, cap);
            if (b && !na_decode(data, n, &ai, b, img->pitch[0], dec->threads)) {
                img->orientation = ai.orientation;
                img->icc = ai.icc;
                img->icc_len = ai.icc_len;
                img->mode = NV_MODE_NITROAVIF;
                img->bands = ai.ntiles;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
            img->error = "AVIF decode failed";
        }
#endif
    } else if (n >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        nw_info wi;   // lossy WebP: nitrowebp (the tokens parsed on one thread, the rows reconstructed on another)
        if (nw_read_info(data, n, &wi)) {
            img->error = "Unsupported WebP (lossless, alpha, animation)";
        } else if (wi.plane_w[0] > mt || wi.plane_h[0] > mt) {
            img->error = "Unsupported WebP (too large)";
        } else {
            total = plane_layout(img, 3, wi.plane_w, wi.plane_h, dec->align);
            b = dec->get(total, cap, dec->ctx);
            if (b) {
                uint8_t *planes[3] = {b + img->offset[0], b + img->offset[1], b + img->offset[2]};
                if (!nw_decode_planes(data, n, &wi, planes, img->pitch, dec->threads)) {
                    yuv420(img, wi.width, wi.height, total);   // (limited range BT.601 4:2:0)
                    img->limited = 1;
                    img->orientation = wi.orientation;
                    img->icc = wi.icc;
                    img->icc_len = wi.icc_len;
                    img->mode = NV_MODE_NITROWEBP;
                    img->bands = 1;
                    *buf = b;
                    return 0;
                }
            }
            GIVE_BACK(b);
            img->error = "WebP decode failed";
        }
    } else if (n >= 8 && data[0] == data[1] && (data[0] == 'I' || data[0] == 'M')) {
        nt_info ti;   // TIFF: nitrotiff (strips / tiles in parallel; uncompressed: straight from the file)
        if (nt_read_info(data, n, &ti)) {
            img->error = "Unsupported TIFF (JPEG, YCbCr, floating point ...)";
        } else if (ti.width > mt || ti.height > mt) {
            img->error = "Unsupported TIFF (too large)";
        } else {
            b = bgra_buffer(dec, img, ti.width, ti.height, cap);
            if (b && !nt_decode(data, n, &ti, b, img->pitch[0], dec->threads)) {
                img->orientation = ti.orientation;
                img->icc = ti.photometric == 2 ? ti.icc : NULL;   // (scanners: Adobe RGB ...)
                img->icc_len = ti.icc_len;
                img->mode = NV_MODE_NITROTIFF;
                *buf = b;
                return 0;
            }
            GIVE_BACK(b);
            img->error = "TIFF decode failed";
        }
    }
    return oom ? -1 : 1;
}

// ---------------------------------------------------------------------------
// the size from the header

int nv_header_size(const char *path, double *w, double *h) {
    FILE *f = open_rb(path);
    if (!f) return 0;
    uint8_t *b = malloc((1 << 20) + PAD);   // (camera raw: Nikon's SubIFD directories are ~270 KB in)
    if (!b) {
        fclose(f);
        return 0;
    }
    size_t n = fread(b, 1, 1 << 20, f);
    memset(b + n, 0, PAD);
    ndng_info di;   // a DNG: its developed raw image
    nw_info wi;     // WebP (its VP8 chunk may go on beyond what was read)
    nj_info ji;
    np_info pi;
    size_t eoff, esize;
    int raw_orientation, ok = 0, o = 1;
    if (!ndng_read_info(b, n, &di)) {
        *w = di.width; *h = di.height; o = di.orientation; ok = 1;
    } else if (!nw_read_info(b, n, &wi)) {
        *w = wi.width; *h = wi.height; o = wi.orientation; ok = 1;
    } else if (!nj_find_embedded(b, n, &eoff, &esize, &raw_orientation)) {   // camera raw (CR2, NEF ...): its JPEG's header
        uint8_t jb[65536 + PAD];
        const uint8_t *jp = b + eoff;
        size_t jn = eoff < n ? n - eoff : 0;
        if (jn < 65536) {   // the JPEG's header lies beyond what was read: read it
            jn = pread_all(f, jb, 65536, eoff) ? 0 : 65536;
            memset(jb + jn, 0, PAD);
            jp = jb;
        }
        if (!nj_read_info(jp, jn, &ji)) {
            *w = ji.width; *h = ji.height; o = raw_orientation ? raw_orientation : ji.orientation; ok = 1;   // (RAF: the JPEG's own)
        }
    }
    if (!ok && n >= 8 && b[0] == b[1] && (b[0] == 'I' || b[0] == 'M')) {   // TIFF: from its directory (Photoshop's is at the end)
        int le = b[0] == 'I', tw, th, to;
        uint32_t dir = le ? (uint32_t)b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24
                          : (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | b[6] << 8 | b[7];
        uint8_t db[65536];
        size_t got = 0;
        if (dir + 2 <= n) {
            got = n - dir < sizeof db ? n - dir : sizeof db;
            memcpy(db, b + dir, got);
        } else if (!seek_to(f, dir)) {
            got = fread(db, 1, sizeof db, f);
        }
        if (!nt_dir_size(db, got, le, &tw, &th, &to)) {
            *w = tw; *h = th; o = to; ok = 1;
        }
    }
    if (!ok && n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && !nj_read_info(b, n, &ji)) {
        *w = ji.width; *h = ji.height; o = ji.orientation; ok = 1;
    }
    if (!ok && !np_read_info(b, n, &pi)) {   // (an eXIf after the first MB: not seen here)
        *w = pi.width; *h = pi.height; o = pi.orientation; ok = 1;
    }
    if (!ok && n >= 26 && !memcmp(b, "8BPS", 4)) {   // (ps_read_info wants the whole file)
        *h = (double)((uint32_t)b[14] << 24 | (uint32_t)b[15] << 16 | (uint32_t)b[16] << 8 | b[17]);
        *w = (double)((uint32_t)b[18] << 24 | (uint32_t)b[19] << 16 | (uint32_t)b[20] << 8 | b[21]);
        ok = *w > 0 && *h > 0;
    }
    free(b);
    fclose(f);
    if (ok && o >= 5) { double t = *w; *w = *h; *h = t; }
    return ok;
}
