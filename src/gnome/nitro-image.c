// SPDX-License-Identifier: MIT
#include "nitro-image.h"
#include "nitro-convert.h"

#include <gtk/gtk.h>   // GTK_CHECK_VERSION

#include "../nitrodng.h"
#include "../nitroheic.h"
#include "../nitrojpeg.h"
#include "../nitropng.h"
#include "../nitropsd.h"
#include "../nitrowebp.h"
#ifdef NV_SVG
#include "../nitrosvg.h"   // optional SVG rendering (lunasvg)
#endif
#ifdef NV_CDR
#include "../nitrocdr.h"   // optional CorelDRAW rendering (libcdr)
#endif

#include <math.h>
#include <string.h>

#define NITRO_READ_PADDING 64

static int decode_threads;
static gint dng_ev3;   // DNG exposure correction in 1/3 EV (key E / Shift+E)

// ---------------------------------------------------------------------------
// Vector drawings, SVG (nitrosvg) and CorelDRAW CDR / CMX (nitrocdr): rendered at a fixed size
// here, the visible part again at screen resolution when zoomed in (nitro-view.c). Without
// nitrosvg, SVG goes through GTK's loaders.

struct _NitroVector {
    gint refs;
    int pages;
#ifdef NV_SVG
    nitrosvg *svg;
#endif
#ifdef NV_CDR
    nitrocdr *cdr;
#endif
};

NitroVector *nitro_vector_ref(NitroVector *vector) {
    if (vector) g_atomic_int_inc(&vector->refs);
    return vector;
}

void nitro_vector_unref(NitroVector *vector) {
    if (!vector || !g_atomic_int_dec_and_test(&vector->refs)) return;
#ifdef NV_SVG
    if (vector->svg) nitrosvg_free(vector->svg);
#endif
#ifdef NV_CDR
    if (vector->cdr) nitrocdr_free(vector->cdr);
#endif
    g_free(vector);
}

int nitro_vector_pages(const NitroVector *vector) { return vector ? vector->pages : 0; }

static void vector_size(const NitroVector *vector, int page, double *w, double *h) {
    *w = *h = 0;
#ifdef NV_SVG
    if (vector->svg) nitrosvg_size(vector->svg, w, h);
#endif
#ifdef NV_CDR
    if (vector->cdr) nitrocdr_size(vector->cdr, page, w, h);
#endif
    (void)page;
}

GdkTexture *nitro_vector_render(NitroVector *vector, int page, double vector_scale, int w, int h, double scale, double x0,
                                double y0) {
    if (!vector || w <= 0 || h <= 0 || (gsize)w > G_MAXSIZE / 4 / (gsize)h) return NULL;
    gsize stride = (gsize)w * 4;
    guint8 *px = g_try_malloc(stride * (gsize)h);
    if (!px) return NULL;
    double k = scale * vector_scale;   // device pixels per document unit
    int rc = -1;
#ifdef NV_SVG
    if (vector->svg) rc = nitrosvg_render(vector->svg, px, w, h, stride, k, x0 / k, y0 / k);
#endif
#ifdef NV_CDR
    if (vector->cdr) rc = nitrocdr_render(vector->cdr, page, px, w, h, stride, k, x0 / k, y0 / k);
#endif
    (void)page;
    if (rc) {
        g_free(px);
        return NULL;
    }
    GBytes *bytes = g_bytes_new_take(px, stride * (gsize)h);
    GdkTexture *t = gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}

// A page rendered with its long side 1024..4096 pixels (small icons enlarged, huge drawings
// reduced), as the macOS / Windows viewers do (takes a reference to vector).
static NitroImage *vector_image(NitroVector *vector, int page) {
    double w = 0, h = 0;
    vector_size(vector, page, &w, &h);
    double l = MAX(w, h), k = l < 1024 ? 1024 / l : l > 4096 ? 4096 / l : 1;
    int iw = MAX(1, MIN(16384, (int)lround(w * k))), ih = MAX(1, MIN(16384, (int)lround(h * k)));
    GdkTexture *t = w > 0 && h > 0 ? nitro_vector_render(vector, page, k, iw, ih, 1.0, 0, 0) : NULL;
    if (!t) return NULL;
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = t;
    image->vector = nitro_vector_ref(vector);
    image->page = page;
    image->vector_scale = k;
    image->orientation = 1;
    image->width = image->source_width = iw;
    image->height = image->source_height = ih;
    return image;
}

NitroImage *nitro_image_load_page(NitroVector *vector, int page) {
    if (!vector || page < 0 || page >= vector->pages) return NULL;
    gint64 start = g_get_monotonic_time();
    NitroImage *image = vector_image(vector, page);
    if (image) image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

static gboolean has_extension(GFile *file, const char *const *exts) {
    char *name = g_file_get_basename(file);
    const char *dot = name ? strrchr(name, '.') : NULL;
    gboolean found = FALSE;
    for (int i = 0; dot && exts[i] && !found; i++) found = g_ascii_strcasecmp(dot + 1, exts[i]) == 0;
    g_free(name);
    return found;
}

// An SVG (.svg, or text starting with '<') or a CorelDRAW file (.cdr, .cmx); NULL if it is not one.
static NitroImage *load_vector(GFile *file, const guint8 *data, gsize len) {
    NitroVector *vector = NULL;
#ifdef NV_CDR
    static const char *const cdr_exts[] = {"cdr", "cmx", NULL};
    nitrocdr *cdr = has_extension(file, cdr_exts) ? nitrocdr_load(data, len) : NULL;
    if (cdr) {
        vector = g_new0(NitroVector, 1);
        vector->cdr = cdr;
        vector->pages = nitrocdr_pages(cdr);
    }
#endif
#ifdef NV_SVG
    static const char *const svg_exts[] = {"svg", NULL};
    gsize i = len >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? 3 : 0;   // (UTF-8 byte order mark)
    while (i < len && g_ascii_isspace(data[i])) i++;
    nitrosvg *svg = !vector && (has_extension(file, svg_exts) || (i < len && data[i] == '<')) ? nitrosvg_load(data, len) : NULL;
    if (svg) {
        vector = g_new0(NitroVector, 1);
        vector->svg = svg;
        vector->pages = 1;
    }
#endif
    (void)file, (void)data, (void)len, (void)has_extension;
    if (!vector) return NULL;
    vector->refs = 1;
    NitroImage *image = vector_image(vector, 0);
    nitro_vector_unref(vector);   // (the image keeps it)
    return image;
}

// Decoded pixels in a format GdkMemoryTexture takes as it is (no RGBA copy); the EXIF
// orientation is applied when drawing (nitro-view.c), not by moving pixels.
typedef struct {
    GBytes *bytes;
    GdkMemoryFormat format;
    gsize stride;
    int width;
    int height;
    int orientation;
} DecodedPixels;

static guint16 read_be16(const guint8 *p) {
    return ((guint16)p[0] << 8) | p[1];
}

static guint32 read_be32(const guint8 *p) {
    return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

static guint16 read_tiff16(const guint8 *p, gboolean little_endian) {
    return little_endian ? ((guint16)p[0] | ((guint16)p[1] << 8)) : read_be16(p);
}

static guint32 read_tiff32(const guint8 *p, gboolean little_endian) {
    if (little_endian)
        return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
    return read_be32(p);
}

static int parse_tiff_orientation(const guint8 *data, gsize len) {
    if (len < 8) return 1;

    gboolean little_endian;
    if (data[0] == 'I' && data[1] == 'I') little_endian = TRUE;
    else if (data[0] == 'M' && data[1] == 'M') little_endian = FALSE;
    else return 1;

    if (read_tiff16(data + 2, little_endian) != 42) return 1;
    guint32 ifd = read_tiff32(data + 4, little_endian);
    if ((gsize)ifd + 2 > len) return 1;

    guint16 count = read_tiff16(data + ifd, little_endian);
    for (guint16 i = 0; i < count; i++) {
        gsize entry = (gsize)ifd + 2 + (gsize)i * 12;
        if (entry + 12 > len) break;
        if (read_tiff16(data + entry, little_endian) != 0x0112) continue;

        guint16 type = read_tiff16(data + entry + 2, little_endian);
        guint32 values = read_tiff32(data + entry + 4, little_endian);
        if (type != 3 || values < 1) return 1;

        guint16 orientation = read_tiff16(data + entry + 8, little_endian);
        return orientation >= 1 && orientation <= 8 ? orientation : 1;
    }

    return 1;
}

static int png_orientation(const guint8 *data, gsize len) {
    if (len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return 1;

    gsize offset = 8;
    while (offset + 12 <= len) {
        guint32 chunk_len = read_be32(data + offset);
        if ((gsize)chunk_len > len - offset - 12) break;
        const guint8 *type = data + offset + 4;
        const guint8 *chunk = data + offset + 8;

        if (memcmp(type, "eXIf", 4) == 0) return parse_tiff_orientation(chunk, chunk_len);
        if (memcmp(type, "IEND", 4) == 0) break;
        offset += 12 + (gsize)chunk_len;
    }

    return 1;
}

static gboolean checked_size(int width, int height, gsize bpp, gsize *stride, gsize *size, GError **error) {
    if (width <= 0 || height <= 0 || (gsize)width > G_MAXSIZE / bpp ||
        (gsize)height > G_MAXSIZE / ((gsize)width * bpp)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid image dimensions");
        return FALSE;
    }
    *stride = (gsize)width * bpp;
    *size = *stride * (gsize)height;
    return TRUE;
}

static gboolean decode_jpeg(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    nj_info info;
    if (nj_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported JPEG");
        return FALSE;
    }

    guint8 *planes[3] = {NULL, NULL, NULL};
    gsize pitch[3] = {0, 0, 0};
    gboolean ok = FALSE;

    for (int c = 0; c < info.ncomp; c++) {
        if (info.plane_w[c] <= 0 || info.plane_h[c] <= 0 ||
            (gsize)info.plane_h[c] > G_MAXSIZE / (gsize)info.plane_w[c]) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid JPEG plane dimensions");
            goto done;
        }
        pitch[c] = (gsize)info.plane_w[c];
        planes[c] = g_malloc(pitch[c] * (gsize)info.plane_h[c]);
    }

    // nthreads 0: nitrojpeg picks its piece count itself; -j limits the workers
    // (nj_set_max_workers in nitro_image_set_threads)
    nj_stats stats;
    if (nj_decode_planes(data, len, &info, planes, pitch, 0, &stats) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JPEG decode failed");
        goto done;
    }

    gsize stride, size;
    if (!checked_size(info.width, info.height, 3, &stride, &size, error)) goto done;
    guint8 *rgb = g_malloc(size);
    nc_jpeg_to_rgb(&info, planes, pitch, rgb, stride);   // multi-threaded, identical to libjpeg-turbo

    out->bytes = g_bytes_new_take(rgb, size);
    out->format = GDK_MEMORY_R8G8B8;
    out->stride = stride;
    out->width = info.width;
    out->height = info.height;
    out->orientation = info.orientation;
    ok = TRUE;

done:
    for (int c = 0; c < 3; c++) g_free(planes[c]);
    return ok;
}

static gboolean decode_png(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    np_info info;
    if (np_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported PNG");
        return FALSE;
    }

    guint8 *raw = g_malloc(info.raw_size);
    np_stats stats;
    if (np_decode(data, len, &info, raw, decode_threads, &stats) != 0) {
        g_free(raw);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "PNG decode failed");
        return FALSE;
    }

    // The unfiltered rows are already in a texture format: row y at raw + y * (stride + 1) + 1
    // (after its filter-type byte), so GTK gets them as they are, without a copy.
    GdkMemoryFormat format;
    switch (info.channels) {
    case 4: format = GDK_MEMORY_R8G8B8A8; break;
    case 3: format = GDK_MEMORY_R8G8B8; break;
#if GTK_CHECK_VERSION(4, 12, 0)
    case 2: format = GDK_MEMORY_G8A8; break;
    default: format = GDK_MEMORY_G8; break;
#else
    default: {   // gray (+ alpha) without GTK 4.12's gray formats: expand to RGBA
        gsize stride, size;
        if (!checked_size(info.width, info.height, 4, &stride, &size, error)) { g_free(raw); return FALSE; }
        guint8 *rgba = g_malloc(size);
        for (int y = 0; y < info.height; y++) {
            const guint8 *src = raw + (gsize)y * (info.stride + 1) + 1;
            guint8 *dst = rgba + (gsize)y * stride;
            for (int x = 0; x < info.width; x++) {
                guint8 v = src[x * info.channels];
                dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = v;
                dst[4 * x + 3] = info.channels == 2 ? src[x * 2 + 1] : 255;
            }
        }
        g_free(raw);
        out->bytes = g_bytes_new_take(rgba, size);
        out->format = GDK_MEMORY_R8G8B8A8;
        out->stride = stride;
        out->width = info.width;
        out->height = info.height;
        out->orientation = png_orientation(data, len);
        return TRUE;
    }
#endif
    }
    GBytes *all = g_bytes_new_take(raw, info.raw_size);
    out->bytes = g_bytes_new_from_bytes(all, 1, info.raw_size - 1);   // skips row 0's filter byte
    g_bytes_unref(all);
    out->format = format;
    out->stride = info.stride + 1;
    out->width = info.width;
    out->height = info.height;
    out->orientation = png_orientation(data, len);
    return TRUE;
}

static gboolean decode_psd(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    ps_info info;
    if (ps_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported PSD/PSB");
        return FALSE;
    }

    gsize plane_count = (gsize)(info.ncolor + info.alpha);
    if (plane_count == 0 || info.plane_size > G_MAXSIZE / plane_count) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid PSD plane dimensions");
        return FALSE;
    }

    guint8 *raw = g_malloc(info.plane_size * plane_count);
    ps_stats stats;
    if (ps_decode(data, len, &info, raw, decode_threads, &stats) != 0) {
        g_free(raw);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "PSD decode failed");
        return FALSE;
    }

    gsize stride, size;
    if (!checked_size(info.width, info.height, 3, &stride, &size, error)) {
        g_free(raw);
        return FALSE;
    }
    guint8 *rgb = g_malloc(size);
    nc_psd_to_rgb(&info, raw, rgb, stride);   // multi-threaded; transparency over black
    g_free(raw);

    out->bytes = g_bytes_new_take(rgb, size);
    out->format = GDK_MEMORY_R8G8B8;
    out->stride = stride;
    out->width = info.width;
    out->height = info.height;
    out->orientation = 1;
    return TRUE;
}

// 4:2:0 planes (HEIC, WebP) -> RGB
static gboolean yuv420_pixels(int width, int height, int plane_w0, int plane_h0, int limited, int orientation,
                              gboolean (*decode)(const guint8 *, gsize, const void *, guint8 *const[3], const gsize[3]),
                              const guint8 *data, gsize len, const void *info, DecodedPixels *out, GError **error) {
    guint8 *planes[3] = {NULL, NULL, NULL};
    gsize pitch[3] = {(gsize)plane_w0, (gsize)(plane_w0 + 1) / 2, (gsize)(plane_w0 + 1) / 2};
    gsize rows[3] = {(gsize)plane_h0, (gsize)(plane_h0 + 1) / 2, (gsize)(plane_h0 + 1) / 2};
    gsize stride, size;
    if (!checked_size(width, height, 3, &stride, &size, error) || !checked_size(plane_w0, plane_h0, 1, &pitch[0], &rows[0], error))
        return FALSE;
    rows[0] = (gsize)plane_h0;
    gboolean ok = FALSE;
    for (int c = 0; c < 3; c++) planes[c] = g_malloc(pitch[c] * rows[c]);
    if (!decode(data, len, info, planes, pitch)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "decode failed");
    } else {
        guint8 *rgb = g_malloc(size);
        nc_yuv420_to_rgb(width, height, planes, pitch, limited, rgb, stride);
        out->bytes = g_bytes_new_take(rgb, size);
        out->format = GDK_MEMORY_R8G8B8;
        out->stride = stride;
        out->width = width;
        out->height = height;
        out->orientation = orientation;
        ok = TRUE;
    }
    for (int c = 0; c < 3; c++) g_free(planes[c]);
    return ok;
}

static gboolean heic_planes(const guint8 *data, gsize len, const void *info, guint8 *const planes[3], const gsize pitch[3]) {
    return !nh_decode_planes(data, len, info, planes, pitch, decode_threads);
}

static gboolean webp_planes(const guint8 *data, gsize len, const void *info, guint8 *const planes[3], const gsize pitch[3]) {
    return !nw_decode_planes(data, len, info, planes, pitch, decode_threads);
}

// HEIC (nitroheic: its HEVC tiles in parallel through nitrohevc; full range BT.601)
static gboolean decode_heic(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    nh_info info;
    if (nh_read_info(data, len, &info)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported HEIC");
        return FALSE;
    }
    return yuv420_pixels(info.width, info.height, info.plane_w[0], info.plane_h[0], 0, info.orientation, heic_planes, data,
                         len, &info, out, error);
}

// lossy WebP (nitrowebp; limited range)
static gboolean decode_webp(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    nw_info info;
    if (nw_read_info(data, len, &info)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported WebP (lossless, alpha, animation)");
        return FALSE;
    }
    return yuv420_pixels(info.width, info.height, info.plane_w[0], info.plane_h[0], 1, info.orientation, webp_planes, data,
                         len, &info, out, error);
}

// DNG: its raw image developed by nitrodng (with the exposure correction of keys E / Shift+E)
static gboolean decode_dng(const guint8 *data, gsize len, DecodedPixels *out, int *ev3, GError **error) {
    ndng_info info;
    if (ndng_read_info(data, len, &info)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported DNG");
        return FALSE;
    }
    gsize stride, size;
    if (!checked_size(info.width, info.height, 4, &stride, &size, error)) return FALSE;
    *ev3 = g_atomic_int_get(&dng_ev3);
    info.ev = *ev3 / 3.0f;
    guint8 *bgrx = g_malloc(size);
    if (ndng_decode(data, len, &info, bgrx, stride, decode_threads)) {
        g_free(bgrx);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "DNG decode failed");
        return FALSE;
    }
    out->bytes = g_bytes_new_take(bgrx, size);
    out->format = GDK_MEMORY_B8G8R8A8;   // (X: 255)
    out->stride = stride;
    out->width = info.width;
    out->height = info.height;
    out->orientation = info.orientation;
    return TRUE;
}

// A camera raw file (Canon CR2, Nikon NEF, Sony ARW, Fujifilm RAF ...): the full-size JPEG the camera
// stored in it, with the raw file's orientation (0: the JPEG's own)
static gboolean decode_raw_jpeg(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    size_t off, size;
    int orientation = 0;
    if (nj_find_embedded(data, len, &off, &size, &orientation) || off >= len || size > len - off) return FALSE;
    guint8 *jpeg = g_malloc(size + NITRO_READ_PADDING);   // (zero padding after it, as for a file)
    memcpy(jpeg, data + off, size);
    memset(jpeg + size, 0, NITRO_READ_PADDING);
    gboolean ok = decode_jpeg(jpeg, size, out, error);
    g_free(jpeg);
    if (ok && orientation) out->orientation = orientation;
    return ok;
}

#if !GTK_CHECK_VERSION(4, 10, 0)
static void mips_free(gpointer data) {   // (see NitroMips)
    NitroMips *m = data;
    for (int i = 0; i < m->n; i++) g_object_unref(m->t[i]);
    g_free(m);
}

static void attach_mips(GdkTexture *texture) {
    int w = gdk_texture_get_width(texture), h = gdk_texture_get_height(texture);
    if (MAX(w, h) <= 1024 || (gsize)w > G_MAXSIZE / 4 / (gsize)h) return;
    gsize stride = (gsize)w * 4;
    guint8 *first = g_try_malloc(stride * (gsize)h);
    if (!first) return;
    gdk_texture_download(texture, first, stride);   // (B G R A premultiplied: GDK_MEMORY_DEFAULT)
    const guint8 *src = first;
    NitroMips *m = g_new0(NitroMips, 1);
    while (MAX(w, h) > 512 && m->n < 16) {
        int nw = (w + 1) / 2, nh = (h + 1) / 2;
        gsize ns = (gsize)nw * 4;
        guint8 *dst = g_try_malloc(ns * (gsize)nh);
        if (!dst) break;
        for (int y = 0; y < nh; y++) {
            const guint8 *r0 = src + (gsize)(2 * y) * stride, *r1 = src + (gsize)MIN(2 * y + 1, h - 1) * stride;
            guint8 *o = dst + (gsize)y * ns;
            for (int x = 0; x < nw; x++) {
                int a = 8 * x, b = 4 * MIN(2 * x + 1, w - 1);
                for (int c = 0; c < 4; c++) o[4 * x + c] = (guint8)((r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c] + 2) >> 2);
            }
        }
        GBytes *bytes = g_bytes_new_take(dst, ns * (gsize)nh);
        m->t[m->n++] = gdk_memory_texture_new(nw, nh, GDK_MEMORY_DEFAULT, bytes, ns);
        g_bytes_unref(bytes);   // (the texture keeps it: the next level reads it)
        src = dst;
        w = nw;
        h = nh;
        stride = ns;
    }
    g_free(first);
    if (m->n) g_object_set_data_full(G_OBJECT(texture), NITRO_MIPS, m, mips_free);
    else g_free(m);
}
#endif

void nitro_image_set_dng_ev3(int ev3) { g_atomic_int_set(&dng_ev3, ev3); }
int nitro_image_get_dng_ev3(void) { return g_atomic_int_get(&dng_ev3); }

void nitro_image_set_threads(int threads) {
    decode_threads = MAX(threads, 0);
    nj_set_max_workers(decode_threads);
}

NitroImage *nitro_image_load(GFile *file, GError **error) {
    g_return_val_if_fail(G_IS_FILE(file), NULL);

    gint64 start = g_get_monotonic_time();
    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_load_contents(file, NULL, &contents, &len, NULL, error)) return NULL;

    if (len > G_MAXSIZE - NITRO_READ_PADDING) {
        g_free(contents);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image file is too large");
        return NULL;
    }

    guint8 *data = g_malloc(len + NITRO_READ_PADDING);
    memcpy(data, contents, len);
    memset(data + len, 0, NITRO_READ_PADDING);
    g_free(contents);

    NitroImage *vector_image = load_vector(file, data, len);
    if (vector_image) {
        g_free(data);
        vector_image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
        return vector_image;
    }

    DecodedPixels pixels = {0};
    GError *nitro_error = NULL;
    gboolean ok = FALSE;
    int ev3 = 0, dng = 0;
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8)
        ok = decode_jpeg(data, len, &pixels, &nitro_error);
    else if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        ok = decode_png(data, len, &pixels, &nitro_error);
    else if (len >= 4 && memcmp(data, "8BPS", 4) == 0)
        ok = decode_psd(data, len, &pixels, &nitro_error);
    else if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
        ok = decode_webp(data, len, &pixels, &nitro_error);
    else if (len >= 12 && memcmp(data + 4, "ftyp", 4) == 0)
        ok = decode_heic(data, len, &pixels, &nitro_error);
    else if (len >= 4 && (memcmp(data, "II*", 4) == 0 || memcmp(data, "MM\0*", 4) == 0) &&
             (ok = decode_dng(data, len, &pixels, &ev3, &nitro_error)))
        dng = 1;
    if (!ok && !dng) {   // (a raw file: its JPEG; the error of the decoder before stays if it is not one)
        GError *raw_error = NULL;
        if ((ok = decode_raw_jpeg(data, len, &pixels, &raw_error))) g_clear_error(&nitro_error);
        g_clear_error(&raw_error);
    }

    GdkTexture *texture = NULL;
    if (ok) {
        texture = gdk_memory_texture_new(pixels.width, pixels.height, pixels.format, pixels.bytes, pixels.stride);
        g_bytes_unref(pixels.bytes);
        g_clear_error(&nitro_error);
        g_free(data);
    } else {
        // Fallback: GTK's own loaders (progressive / CMYK JPEG, palette / 16-bit / interlaced
        // PNG, TIFF, and what the installed gdk-pixbuf loaders read: GIF, BMP, WebP ...).
        // The EXIF orientation still comes from the file header where we can read it.
        pixels.orientation = 1;
        if (len >= 3 && data[0] == 0xff && data[1] == 0xd8) {
            nj_info ji;
            if (nj_read_info(data, len, &ji) == 0) pixels.orientation = ji.orientation;
        } else if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
            pixels.orientation = png_orientation(data, len);
        }
        GBytes *file_bytes = g_bytes_new_take(data, len);   // (the padding is not part of it)
        GError *gdk_error = NULL;
        texture = gdk_texture_new_from_bytes(file_bytes, &gdk_error);
        g_bytes_unref(file_bytes);
        if (!texture) {
            // report why our decoder refused it if it is one of ours, else GTK's reason
            if (nitro_error) {
                g_propagate_error(error, nitro_error);
                g_clear_error(&gdk_error);
            } else {
                g_propagate_error(error, gdk_error);
            }
            return NULL;
        }
        g_clear_error(&nitro_error);
        pixels.width = gdk_texture_get_width(texture);
        pixels.height = gdk_texture_get_height(texture);
    }

#if !GTK_CHECK_VERSION(4, 10, 0)
    attach_mips(texture);
#endif
    int orientation = pixels.orientation >= 1 && pixels.orientation <= 8 ? pixels.orientation : 1;
    int swap = orientation >= 5;   // EXIF 5..8: rotated by 90 degrees
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = texture;
    image->orientation = orientation;
    image->width = swap ? pixels.height : pixels.width;
    image->height = swap ? pixels.width : pixels.height;
    image->source_width = pixels.width;
    image->source_height = pixels.height;
    image->dng = dng;
    image->ev3 = ev3;
    image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

void nitro_image_free(NitroImage *image) {
    if (!image) return;
    g_clear_object(&image->texture);
    g_clear_pointer(&image->vector, nitro_vector_unref);
    g_free(image);
}
