// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitroconvert: an example of using libnitro - any image the library decodes (JPEG, PNG, PSD,
// HEIC, AVIF, WebP, TIFF, DNG, a camera raw's embedded JPEG) -> an uncompressed 24-bit BMP.
//
//   nitroconvert photo.jpg photo.bmp
//
// nv_read_image reads the file (only what is needed of a PSD or a raw), nv_decode decodes it
// with the decoder of its format into one buffer, which holds either YUV planes (JPEG, HEIC,
// AVIF, WebP), a PSD's planes, 8-bit BGRX pixels (TIFF, DNG, palette / 16-bit PNG) or a PNG's
// unfiltered rows; nitro_convert turns the first two into RGB, the others are a byte shuffle.
// The EXIF orientation is applied while the BMP is written. Build (see the Makefile's
// examples/nitroconvert): clang -O3 -fblocks -Isrc examples/nitroconvert.c build/libnitro.a
// (-lpthread -lm; libdav1d.a when the library was built with it; on macOS -framework CoreFoundation
// -framework VideoToolbox -framework CoreMedia -framework CoreVideo instead, for HEIC).
#include "nitro_convert.h"
#include "nitro_load.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

// nv_decode asks for its buffer through these (a viewer recycles buffers; this just mallocs)
static uint8_t *get_buffer(size_t len, size_t *cap, void *ctx) { *cap = len; return malloc(len); }
static void put_buffer(uint8_t *p, size_t cap, void *ctx) { free(p); }

// EXIF orientation o (1..8): the source pixel of the output's (x, y)
// (2 mirror, 3 180, 4 flip, 5 transpose, 6 90 cw, 7 transverse, 8 90 ccw)
static void source_xy(int o, int w, int h, int x, int y, int *sx, int *sy) {
    if (o >= 5) { int t = x; x = y; y = t; }                      // 5..8: transposed
    *sx = o == 2 || o == 3 || o == 7 || o == 8 ? w - 1 - x : x;   // mirrored
    *sy = o == 3 || o == 4 || o == 6 || o == 7 ? h - 1 - y : y;   // flipped
}

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: nitroconvert <image> <out.bmp>\n"); return 2; }

    size_t len = 0;
    int raw_orientation = 0, raw_read = 0;
    uint8_t *data = nv_read_image(argv[1], &len, INT_MAX, &raw_orientation, &raw_read);
    if (!data) { fprintf(stderr, "%s: cannot read\n", argv[1]); return 1; }

    NvDecoder dec = {get_buffer, put_buffer, NULL, 1 /* align */, INT_MAX /* max size */, 0 /* threads: all CPUs */, 0 /* DNG: 0 EV */};
    NvImage img;
    uint8_t *buf = NULL;
    size_t cap = 0;
    int rc = nv_decode(data, &len, raw_orientation, raw_read, &dec, &img, &buf, &cap);
    if (rc) {
        fprintf(stderr, "%s: %s\n", argv[1], rc < 0 ? "out of memory" : img.error ? img.error : "not an image the library decodes");
        return 1;
    }
    int w = img.width, h = img.height;

    // what the decoder says about it
    static const char *const kinds[] = {"YUV planes", "BGRX pixels", "PNG rows", "PSD planes"};
    static const char *const orientations[] = {"?", "normal", "mirrored", "rotated 180", "flipped", "transposed",
                                               "rotated 90 cw", "transversed", "rotated 90 ccw"};
    printf("%s: %s", argv[1], img.mode >= 0 ? "nitrojpeg" : nv_mode_name(img.mode));
    if (img.mode >= 0) printf(" (%s)", nv_mode_name(img.mode));   // (a JPEG: how nitrojpeg parallelised it)
    printf(", %dx%d, orientation %d (%s), %s", w, h, img.orientation, orientations[img.orientation], kinds[img.kind]);
    if (img.kind == NV_YUV) {
        printf(", %d component%s", img.ncomp, img.ncomp > 1 ? "s" : "");
        for (int c = 0; c < img.ncomp; c++) printf("%s%dx%d", c ? " " : " sampled ", img.h[c], img.v[c]);
        if (img.limited) printf(", limited range");
    } else if (img.kind == NV_PNG) printf(", %d channel%s", img.ncomp, img.ncomp > 1 ? "s" : "");
    else if (img.kind == NV_PLANAR) printf(", %d colour plane%s%s", img.ncomp, img.ncomp > 1 ? "s" : "", img.hasa ? " + alpha" : "");
    if (img.icc) printf(", ICC profile %zu bytes", img.icc_len);
    printf("\n");

    // the pixels as RGB, 3 bytes each (transparency over black, as the library's own conversions do)
    size_t stride = (size_t)w * 3;
    uint8_t *rgb = malloc(stride * (size_t)h);
    if (!rgb) { fprintf(stderr, "out of memory\n"); return 1; }
    switch (img.kind) {
    case NV_YUV: nc_yuv_to_rgb(&img, buf, rgb, stride); break;     // JPEG (any sampling), HEIC, AVIF, WebP
    case NV_PLANAR: nc_psd_to_rgb(&img, buf, rgb, stride); break;  // PSD
    case NV_BGRA:                                                   // TIFF, DNG, PNG that is not 8-bit gray / RGB(A)
        for (int y = 0; y < h; y++) {
            const uint8_t *s = buf + (size_t)y * img.pitch[0];
            uint8_t *d = rgb + (size_t)y * stride;
            for (int x = 0; x < w; x++, s += 4, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
        break;
    case NV_PNG:   // 8-bit rows of ncomp channels (gray, gray+alpha, RGB, RGBA), each after its filter-type byte
        for (int y = 0; y < h; y++) {
            const uint8_t *s = buf + (size_t)y * img.pitch[0] + 1;
            uint8_t *d = rgb + (size_t)y * stride;
            for (int x = 0; x < w; x++, s += img.ncomp, d += 3) {
                int a = img.ncomp == 2 || img.ncomp == 4 ? s[img.ncomp - 1] : 255;
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)(s[img.ncomp >= 3 ? c : 0] * a / 255);
            }
        }
        break;
    }
    free(buf);
    free(data);

    // the BMP: 54-byte header, rows bottom-up, B G R, each padded to 4 bytes; the orientation applied here
    int ow = img.orientation >= 5 ? h : w, oh = img.orientation >= 5 ? w : h;
    size_t row = ((size_t)ow * 3 + 3) & ~(size_t)3;
    uint8_t hdr[54] = {'B', 'M'}, *line = calloc(row, 1);
    le32(hdr + 2, (uint32_t)(54 + row * (size_t)oh));   // file size
    le32(hdr + 10, 54);                                  // pixel data offset
    le32(hdr + 14, 40);                                  // BITMAPINFOHEADER
    le32(hdr + 18, (uint32_t)ow);
    le32(hdr + 22, (uint32_t)oh);
    hdr[26] = 1;                                         // planes
    hdr[28] = 24;                                        // bits per pixel
    le32(hdr + 34, (uint32_t)(row * (size_t)oh));        // image size
    FILE *f = line ? fopen(argv[2], "wb") : NULL;
    if (!f) { fprintf(stderr, "%s: cannot write\n", argv[2]); return 1; }
    fwrite(hdr, 1, 54, f);
    for (int y = oh - 1; y >= 0; y--) {
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            source_xy(img.orientation, w, h, x, y, &sx, &sy);
            const uint8_t *s = rgb + (size_t)sy * stride + (size_t)sx * 3;
            line[3 * x] = s[2]; line[3 * x + 1] = s[1]; line[3 * x + 2] = s[0];
        }
        fwrite(line, 1, row, f);
    }
    if (fclose(f)) { fprintf(stderr, "%s: write error\n", argv[2]); return 1; }
    printf("-> %s: %dx%d, 24-bit BMP\n", argv[2], ow, oh);
    return 0;
}
