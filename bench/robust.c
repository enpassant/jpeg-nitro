// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Robustness test: feeds damaged variants of the sample files through the
// same decode path as the viewer (nitrojpeg, then TurboJPEG fallback).
// Build with -fsanitize=address,undefined (make robust). Must not crash.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include "../src/nitro_os.h"   // nitro_now_ms
#include "../src/nitrojpeg.h"

static double now_ms(void) { return nitro_now_ms(); }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// Viewer decode path. Returns 0 nitrojpeg ok, 1 TurboJPEG fallback, 2 undecodable.
static int decode_like_viewer(const uint8_t *d, size_t n) {
    nj_info fi;
    if (!nj_read_info(d, n, &fi) && fi.supported && fi.width <= 16384 && fi.height <= 16384) {
        size_t pitch[3] = {0}, tot = 0, off[3] = {0};
        for (int c = 0; c < fi.ncomp; c++) { pitch[c] = (fi.plane_w[c] + 255) & ~255; off[c] = tot; tot += pitch[c] * fi.plane_h[c]; }
        uint8_t *buf = malloc(tot);
        uint8_t *pl[3] = {buf + off[0], buf + off[1], buf + off[2]};
        int rc = nj_decode_planes(d, n, &fi, pl, pitch, 0, NULL);
        free(buf);
        if (!rc) return 0;
    }
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(h, d, n) < 0) { tj3Destroy(h); return 2; }
    int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
    tjscalingfactor sf = {1, 1};
    while ((w + sf.denom - 1) / sf.denom > 16384 || (ht + sf.denom - 1) / sf.denom > 16384) sf.denom *= 2;
    tj3SetScalingFactor(h, sf);
    w = TJSCALED(w, sf); ht = TJSCALED(ht, sf);
    if (w <= 0 || ht <= 0) { tj3Destroy(h); return 2; }
    uint8_t *out = malloc((size_t)w * ht * 4);
    tj3Set(h, TJPARAM_STOPONWARNING, 0);
    int rc = tj3Decompress8(h, d, n, out, w * 4, TJPF_BGRX);
    int fatal = rc < 0 && tj3GetErrorCode(h) == TJERR_FATAL;
    tj3Destroy(h);
    free(out);
    return fatal ? 2 : 1;
}

static size_t scan_start(const uint8_t *d, size_t n) {   // byte after the SOS header
    for (size_t i = 2; i + 4 < n; ) {
        if (d[i] != 0xFF) return n / 4;
        if (d[i + 1] == 0xDA) return i + 2 + ((d[i + 2] << 8) | d[i + 3]);
        i += 2 + ((d[i + 2] << 8) | d[i + 3]);
    }
    return n / 4;
}

static int stats[3], cases;
static double worst_ms;
static char worst_name[256];

static void run(const char *file, const char *what, const uint8_t *d, size_t n) {
    double t = now_ms();
    int r = decode_like_viewer(d, n);
    t = now_ms() - t;
    stats[r]++;
    cases++;
    if (t > worst_ms) { worst_ms = t; snprintf(worst_name, sizeof worst_name, "%s [%s]", file, what); }
    if (getenv("VERBOSE")) printf("  %-28s %-26s -> %s  %.1f ms\n", file, what, r == 0 ? "nitrojpeg" : r == 1 ? "fallback" : "undecodable", t);
}

int main(int argc, char **argv) {
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); size_t n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *orig = malloc(n);
        fread(orig, 1, n, fp); fclose(fp);
        const char *name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
        uint8_t *d = malloc(n + 4096);
        size_t ss = scan_start(orig, n);
        char what[64];

        run(name, "original", orig, n);
        // 1. truncation (interrupted download)
        double fr[] = {0.02, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 0.9999};
        size_t fixed[] = {0, 1, 2, 3, 10, 100, 1000, ss - 1, ss, ss + 1, ss + 7, n - 1, n - 2, n - 3};
        for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) {
            if (fixed[k] > n) continue;
            memcpy(d, orig, fixed[k]);
            snprintf(what, sizeof what, "truncated @%zu", fixed[k]);
            run(name, what, d, fixed[k]);
        }
        for (size_t k = 0; k < sizeof fr / sizeof *fr; k++) {
            size_t m = (size_t)(n * fr[k]);
            memcpy(d, orig, m);
            snprintf(what, sizeof what, "truncated %.2f%%", fr[k] * 100);
            run(name, what, d, m);
        }
        // 2. random byte errors in the compressed data
        int flips[] = {1, 1, 1, 10, 100, 1000, 10000};
        for (size_t k = 0; k < sizeof flips / sizeof *flips; k++) {
            memcpy(d, orig, n);
            for (int i = 0; i < flips[k]; i++) d[ss + rnd() % (n - ss)] = (uint8_t)rnd();
            snprintf(what, sizeof what, "%d random bytes", flips[k]);
            run(name, what, d, n);
        }
        // 3. single bit flips (hardest case: stream stays "plausible")
        for (int k = 0; k < 5; k++) {
            memcpy(d, orig, n);
            size_t p = ss + rnd() % (n - ss);
            d[p] ^= (uint8_t)(1 << (rnd() % 8));
            snprintf(what, sizeof what, "bit flip @%zu", p);
            run(name, what, d, n);
        }
        // 4. zeroed / 0xFF-filled block (bad sector, partial write); small files: half their data
        size_t blk = n - ss > 2 * 65536 ? 65536 : (n - ss) / 2;
        for (int k = 0; k < 2 && blk >= 16; k++) {
            memcpy(d, orig, n);
            size_t p = ss + rnd() % (n - ss - blk);
            memset(d + p, k ? 0xFF : 0x00, blk);
            run(name, k ? "block of 0xFF" : "block of zeros", d, n);
        }
        // 5. spurious markers inside the data (RSTn, EOI, SOS, random)
        uint8_t mk[] = {0xD0, 0xD3, 0xD9, 0xDA, 0xC4, 0xE1, 0x01, 0xFF};
        for (size_t k = 0; k < sizeof mk; k++) {
            memcpy(d, orig, n);
            size_t p = ss + rnd() % (n - ss - 2);
            d[p] = 0xFF; d[p + 1] = mk[k];
            snprintf(what, sizeof what, "marker FF%02X inside", mk[k]);
            run(name, what, d, n);
        }
        // 6. header damage
        for (int k = 0; k < 40; k++) {
            memcpy(d, orig, n);
            int cnt = 1 + (int)(rnd() % 4);
            for (int i = 0; i < cnt; i++) d[rnd() % ss] = (uint8_t)rnd();
            snprintf(what, sizeof what, "header damage #%d", k);
            run(name, what, d, n);
        }
        // 7. chunk removed from the middle / duplicated
        {
            size_t p = ss + (n - ss) / 3, cut = (n - ss) / 7;
            memcpy(d, orig, p);
            memcpy(d + p, orig + p + cut, n - p - cut);
            run(name, "middle chunk removed", d, n - cut);
            memcpy(d, orig, p + cut);
            memcpy(d + p + cut, orig + p, n - p - cut);
            run(name, "chunk duplicated", d, n);
        }
        free(d);
        free(orig);
    }
    // 8. not JPEG at all
    const char *txt = "hello, this is not a jpeg file\n";
    run("text", "text file", (const uint8_t *)txt, strlen(txt));
    uint8_t soi[] = {0xFF, 0xD8, 0xFF};
    run("soi", "only SOI", soi, sizeof soi);
    uint8_t *g = malloc(1 << 20);
    for (int k = 0; k < 20; k++) {
        size_t m = 1 + rnd() % (1 << 20);
        for (size_t i = 0; i < m; i++) g[i] = (uint8_t)rnd();
        if (k & 1) { g[0] = 0xFF; g[1] = 0xD8; }
        run("garbage", k & 1 ? "garbage with SOI" : "garbage", g, m);
    }
    free(g);
    printf("%d cases: nitrojpeg %d, TurboJPEG fallback %d, undecodable %d. No crash.\n", cases, stats[0], stats[1], stats[2]);
    printf("slowest: %.1f ms  %s\n", worst_ms, worst_name);
    return 0;
}
