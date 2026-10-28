// SPDX-License-Identifier: MIT
#include "nitro-image.h"

#include <gtk/gtk.h>   // GTK_CHECK_VERSION

#include "../nitro_load.h"      // reading and decoding with our decoders (the library)
#include "../nitro_convert.h"   // their planes -> RGB (the library too)
#include "../nitrojpeg.h"    // nj_set_max_workers
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
    gboolean found = name && nv_has_ext(name, exts);
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

static uint8_t *image_get(size_t len, size_t *cap, void *ctx) {
    (void)ctx;
    *cap = len;
    return g_try_malloc(len);
}
static void image_put(uint8_t *p, size_t cap, void *ctx) {
    (void)cap, (void)ctx;
    g_free(p);
}

// The decoded pixels (nitro_load's NvImage in buf, which this takes) as a texture: in a format
// GdkMemoryTexture takes as it is where there is one (BGRX, a PNG's rows), else converted to RGB.
static GdkTexture *texture_of(const NvImage *img, uint8_t *buf, GError **error) {
    gsize stride = (gsize)img->width * 3, size = stride * (gsize)img->height;
    GBytes *bytes;
    GdkMemoryFormat format;
    switch (img->kind) {
    case NV_BGRA:
        bytes = g_bytes_new_take(buf, img->size);
        format = GDK_MEMORY_B8G8R8A8;   // (X: 255)
        stride = img->pitch[0];
        break;
    case NV_PNG: {
        // The unfiltered rows are already in a texture format: row y at buf + y * (stride + 1) + 1
        // (after its filter-type byte), so GTK gets them as they are, without a copy.
        switch (img->ncomp) {
        case 4: format = GDK_MEMORY_R8G8B8A8; break;
        case 3: format = GDK_MEMORY_R8G8B8; break;
#if GTK_CHECK_VERSION(4, 12, 0)
        case 2: format = GDK_MEMORY_G8A8; break;
        default: format = GDK_MEMORY_G8; break;
#else
        default: {   // gray (+ alpha) without GTK 4.12's gray formats: expand to RGBA
            gsize rs = (gsize)img->width * 4;
            guint8 *rgba = g_try_malloc(rs * (gsize)img->height);
            if (!rgba) {
                g_free(buf);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
                return NULL;
            }
            for (int y = 0; y < img->height; y++) {
                const guint8 *src = buf + (gsize)y * img->pitch[0] + 1;
                guint8 *dst = rgba + (gsize)y * rs;
                for (int x = 0; x < img->width; x++) {
                    guint8 v = src[x * img->ncomp];
                    dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = v;
                    dst[4 * x + 3] = img->ncomp == 2 ? src[x * 2 + 1] : 255;
                }
            }
            g_free(buf);
            GBytes *b = g_bytes_new_take(rgba, rs * (gsize)img->height);
            GdkTexture *t = gdk_memory_texture_new(img->width, img->height, GDK_MEMORY_R8G8B8A8, b, rs);
            g_bytes_unref(b);
            return t;
        }
#endif
        }
        GBytes *all = g_bytes_new_take(buf, img->size);
        bytes = g_bytes_new_from_bytes(all, 1, img->size - 1);   // skips row 0's filter byte
        g_bytes_unref(all);
        stride = img->pitch[0];
        break;
    }
    default: {   // NV_YUV (JPEG, HEIC, WebP), NV_PLANAR (PSD): RGB (multi-threaded)
        guint8 *rgb = g_try_malloc(size);
        if (!rgb) {
            g_free(buf);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
            return NULL;
        }
        if (img->kind == NV_YUV) nc_yuv_to_rgb(img, buf, rgb, stride);   // identical to libjpeg-turbo
        else nc_psd_to_rgb(img, buf, rgb, stride);
        g_free(buf);
        bytes = g_bytes_new_take(rgb, size);
        format = GDK_MEMORY_R8G8B8;
    }
    }
    GdkTexture *t = gdk_memory_texture_new(img->width, img->height, format, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}

NitroImage *nitro_image_load(GFile *file, GError **error) {
    g_return_val_if_fail(G_IS_FILE(file), NULL);

    gint64 start = g_get_monotonic_time();
    // a local file: nitro_load reads it (a PSD's merged image only, a camera raw's JPEG only);
    // else (a GVfs location) GIO, whose error says why if it cannot
    char *path = g_file_get_path(file);
    size_t len = 0;
    int raw_orientation = 0, raw_read = 0;
    uint8_t *data = path ? nv_read_image(path, &len, G_MAXINT, &raw_orientation, &raw_read) : NULL;
    g_free(path);
    if (!data) {
        gchar *contents = NULL;
        gsize n = 0;
        if (!g_file_load_contents(file, NULL, &contents, &n, NULL, error)) return NULL;
        if (n > G_MAXSIZE - NITRO_READ_PADDING || !(data = malloc(n + NITRO_READ_PADDING))) {
            g_free(contents);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image file is too large");
            return NULL;
        }
        memcpy(data, contents, n);
        memset(data + n, 0, NITRO_READ_PADDING);
        g_free(contents);
        len = n;
    }

    NitroImage *vector_image = load_vector(file, data, len);
    if (vector_image) {
        free(data);
        vector_image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
        return vector_image;
    }

    NvDecoder dec = {image_get, image_put, NULL, 1, G_MAXINT, decode_threads, g_atomic_int_get(&dng_ev3)};
    NvImage img;
    uint8_t *buf = NULL;
    size_t cap = 0;
    int rc = nv_decode(data, &len, raw_orientation, raw_read, &dec, &img, &buf, &cap);
    if (rc < 0) {
        free(data);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
        return NULL;
    }
    GdkTexture *texture = NULL;
    int width, height, orientation = img.orientation;
    if (rc == 0) {
        free(data);
        texture = texture_of(&img, buf, error);
        if (!texture) return NULL;
        width = img.width;
        height = img.height;
    } else {
        // Fallback: GTK's own loaders (CMYK / cut-off JPEG, interlaced PNG, JPEG / YCbCr TIFF, and
        // what the installed gdk-pixbuf loaders read: GIF, BMP, WebP ...). The EXIF orientation
        // still comes from the file header where we can read it (img.orientation).
        GBytes *file_bytes = g_bytes_new_with_free_func(data, len, free, data);   // (the padding is not part of it)
        GError *gdk_error = NULL;
        texture = gdk_texture_new_from_bytes(file_bytes, &gdk_error);
        g_bytes_unref(file_bytes);
        if (!texture) {   // report why our decoder refused it if it is one of ours, else GTK's reason
            if (img.error) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, img.error);
                g_clear_error(&gdk_error);
            } else {
                g_propagate_error(error, gdk_error);
            }
            return NULL;
        }
        width = gdk_texture_get_width(texture);
        height = gdk_texture_get_height(texture);
    }

#if !GTK_CHECK_VERSION(4, 10, 0)
    attach_mips(texture);
#endif
    orientation = orientation >= 1 && orientation <= 8 ? orientation : 1;
    int swap = orientation >= 5;   // EXIF 5..8: rotated by 90 degrees
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = texture;
    image->orientation = orientation;
    image->width = swap ? height : width;
    image->height = swap ? width : height;
    image->source_width = width;
    image->source_height = height;
    image->dng = rc == 0 && img.mode == NV_MODE_NITRODNG;
    image->ev3 = image->dng ? img.ev3 : 0;
    image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

void nitro_image_free(NitroImage *image) {
    if (!image) return;
    g_clear_object(&image->texture);
    g_clear_pointer(&image->vector, nitro_vector_unref);
    g_free(image);
}
