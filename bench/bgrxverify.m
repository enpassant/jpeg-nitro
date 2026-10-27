// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// bgrxverify: the decoders that write 8-bit BGRX - nitrotiff, nitroavif, and nitropng for the PNGs it
// doesn't leave to the GPU (palette, 1 / 2 / 4 / 16-bit, transparency key) - against Apple ImageIO
// (macOS): the pixels and the time.
//
//   bench/bgrxverify [-j threads] [-a] file.tif|file.png|file.avif|directory ...    (-a: also 8-bit PNGs)
//
// Ours: the best of 3 decodes into the same buffer; ImageIO: one, decoded at once.
// ImageIO's image is drawn over black into a bitmap of its own colour space (no colour conversion:
// RGB, gray, the base space of a palette); a CMYK image is converted with its profile there, so
// its differences are only reported. 16-bit samples: ImageIO rounds to 8 bits its own way.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/nitrotiff.h"
#include "../src/nitropng.h"
#include "../src/nitroavif.h"
#include "../src/nitro_os.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 64);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) { memset(d + n, 0, 64); *len = (size_t)n; }
    return d;
}

static int g_threads = 0, g_all, g_bad, g_ok, g_unsup;
static double g_tn, g_ti;

static void one(const char *path) {
    size_t len;
    uint8_t *data = read_file(path, &len);
    if (!data) return;
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    nt_info in;
    np_info pn;
    na_info av;
    int png = len >= 8 && !memcmp(data, "\x89PNG", 4), avif = len >= 12 && !memcmp(data + 4, "ftyp", 4), w, h, k1, k2, k3;
    if (avif) {
        if (na_read_info(data, len, &av)) {
            printf("%-34.34s not handled (AVIF)\n", name);
            g_unsup++;
            free(data);
            return;
        }
        png = 2;
    } else if (png ? np_read_info(data, len, &pn) || !pn.convertible || (pn.supported && !g_all) : nt_read_info(data, len, &in)) {
        if (png) printf("%-34.34s %s (colour type %d, %d bit%s)\n", name, pn.supported ? "rows to the GPU" : "not handled", pn.color_type,
                        pn.bit_depth, pn.interlace ? ", interlaced" : "");
        else printf("%-34.34s not handled (comp %d, photometric %d, %d x %d bit)\n", name, in.compression, in.photometric, in.spp, in.bps);
        g_unsup++;
        free(data);
        return;
    }
    if (png == 2) { w = av.width; h = av.height; k1 = av.ntiles; k2 = av.alpha; k3 = av.nclx ? av.matrix : -1; }
    else if (png) { w = pn.width; h = pn.height; k1 = pn.color_type; k2 = pn.channels; k3 = pn.bit_depth; }
    else { w = in.width; h = in.height; k1 = in.compression; k2 = in.spp; k3 = in.bps; }
    uint8_t *ours = malloc((size_t)w * h * 4), *raw = png == 1 ? malloc(pn.raw_size) : NULL;
    double tn = 1e9, t0;
    int rc = 0;
    for (int k = 0; k < 3 && !rc; k++) {   // best of 3 (the first one also faults the output's pages in)
        t0 = nitro_now_ms();
        if (png == 2) rc = na_decode(data, len, &av, ours, (size_t)w * 4, g_threads);
        else if (png) rc = np_decode(data, len, &pn, raw, g_threads, NULL) || np_to_bgrx(data, len, &pn, raw, ours, (size_t)w * 4, g_threads);
        else rc = nt_decode(data, len, &in, ours, (size_t)w * 4, g_threads);
        if (nitro_now_ms() - t0 < tn) tn = nitro_now_ms() - t0;
    }
    free(raw);
    // ImageIO, decoded at once
    t0 = nitro_now_ms();
    CFDataRef cfd = CFDataCreateWithBytesNoCopy(NULL, data, (CFIndex)len, kCFAllocatorNull);
    CGImageSourceRef src = CGImageSourceCreateWithData(cfd, NULL);
    NSDictionary *opt = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)opt) : NULL;
    double ti = nitro_now_ms() - t0;
    const char *how = "";
    if (rc) {
        printf("%-34.34s %5dx%-5d FAILED (%s)\n", name, w, h, png == 2 ? "nitroavif" : png ? "nitropng" : "nitrotiff");
        g_bad++;
    } else if (!img || (int)CGImageGetWidth(img) != w || (int)CGImageGetHeight(img) != h) {
        printf("%-34.34s %5dx%-5d ours %6.1f ms, no ImageIO reference\n", name, w, h, tn);
    } else {
        CGColorSpaceRef cs = CGImageGetColorSpace(img);
        CGColorSpaceModel m = CGColorSpaceGetModel(cs);
        if (m == kCGColorSpaceModelIndexed) cs = CGColorSpaceGetBaseColorSpace(cs), m = CGColorSpaceGetModel(cs);
        int gray = m == kCGColorSpaceModelMonochrome, cmyk = m != kCGColorSpaceModelRGB && !gray;
        CGColorSpaceRef ctxcs = cmyk ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : CGColorSpaceRetain(cs);
        int nc = gray ? 1 : 4;
        uint8_t *ref = calloc((size_t)w * h, (size_t)nc);
        CGContextRef ctx = CGBitmapContextCreate(ref, (size_t)w, (size_t)h, 8, (size_t)w * nc, ctxcs,
                                                 gray ? (CGBitmapInfo)kCGImageAlphaNone : (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextSetBlendMode(ctx, kCGBlendModeCopy);   // (no premultiplied alpha left in the result: over black)
        CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
        if (getenv("DUMP")) {   // (debugging: ours and ImageIO's as PPM)
            char fn[512];
            for (int k = 0; k < 2; k++) {
                snprintf(fn, sizeof fn, "%s.%s.ppm", getenv("DUMP"), k ? "ref" : "ours");
                FILE *pf = fopen(fn, "wb");
                fprintf(pf, "P6 %d %d 255\n", w, h);
                for (size_t p = 0; p < (size_t)w * h; p++) {
                    uint8_t px[3];
                    for (int c = 0; c < 3; c++) px[c] = k ? ref[p * nc + (gray ? 0 : c)] : ours[p * 4 + 2 - c];
                    fwrite(px, 1, 3, pf);
                }
                fclose(pf);
            }
        }
        int maxd = 0;
        long n1 = 0, cnt = 0;
        double sum = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *o = ours + ((size_t)y * w + x) * 4, *r = ref + ((size_t)y * w + x) * nc;
                for (int c = 0; c < (gray ? 1 : 3); c++) {
                    int e = abs((int)o[2 - c] - (int)r[c]);
                    if (e > maxd) maxd = e;
                    n1 += e <= 1;
                    sum += e;
                    cnt++;
                }
            }
        // (AVIF 4:2:0 / 4:2:2: ImageIO upsamples chroma by repeating samples - nearest, as tried - ours is
        // linear, so colour edges differ; luma and 4:4:4 match within 1)
        int ok = png == 2 ? sum / cnt <= 2.5 : maxd <= 1 || cmyk;
        how = cmyk ? "  (CMYK: ImageIO with the profile)" : png == 2 ? "  (AVIF: ImageIO's chroma nearest, ours linear)" : "";
        printf("%-34.34s %5dx%-5d %s%-5d %dx%-2d %s %6.1f ms  ImageIO %6.1f ms  max %3d mean %.3f <=1 %7.3f%% %s%s\n", name, w, h,
               png == 2 ? "n" : png ? "t" : "c", k1, k2, k3, png == 2 ? "nitroavif" : png ? "nitropng " : "nitrotiff", tn, ti, maxd, sum / cnt, 100.0 * n1 / cnt, ok ? "OK" : "DIFFERENT", how);
        if (ok) g_ok++;
        else g_bad++;
        g_tn += tn;
        g_ti += ti;
        CGContextRelease(ctx);
        CGColorSpaceRelease(ctxcs);
        free(ref);
    }
    if (img) CGImageRelease(img);
    if (src) CFRelease(src);
    CFRelease(cfd);
    free(ours);
    free(data);
}

static void walk(const char *p) {
    struct stat st;
    if (stat(p, &st)) return;
    if (!S_ISDIR(st.st_mode)) { one(p); return; }
    struct dirent **ents;
    int n = scandir(p, &ents, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        const char *e = ents[i]->d_name, *dot = strrchr(e, '.');
        char q[4096];
        snprintf(q, sizeof q, "%s/%s", p, e);
        if (e[0] != '.' && ents[i]->d_type == DT_DIR) walk(q);   // (subfolders too)
        else if (e[0] != '.' && dot && (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff") || !strcasecmp(dot, ".png") ||
                                         !strcasecmp(dot, ".avif"))) one(q);
        free(ents[i]);
    }
    free(ents);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-j") && i + 1 < argc) g_threads = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-a")) g_all = 1;
            else walk(argv[i]);
        }
        printf("\n%d OK, %d bad, %d not handled; ours %.1f ms, ImageIO %.1f ms (the compared files)\n", g_ok, g_bad, g_unsup,
               g_tn, g_ti);
    }
    return g_bad != 0;
}
