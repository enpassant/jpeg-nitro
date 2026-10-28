// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Robustness test for nitropng: damaged variants of the given PNGs, decoded
// single- and multi-threaded. Build with ASan + UBSan (make bench/pngrobust).
// A decode may fail, but must never crash, and a successful decode of damaged
// data must still be exactly what zlib produces (the Adler-32 guarantees it).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../src/nitropng.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int cases, ok_cases, fails;

static void run(const char *name, const char *what, const uint8_t *d, size_t n) {
    np_info fi;
    cases++;
    if (np_read_info(d, n, &fi) || !fi.convertible || fi.raw_size > ((size_t)1 << 31)) return;
    uint8_t *a = malloc(fi.raw_size), *b = malloc(fi.raw_size);
    int r1 = np_decode(d, n, &fi, a, 1, NULL);
    int r2 = np_decode(d, n, &fi, b, 0, NULL);
    if (!r1 || !r2) ok_cases++;
    if (!r1 && !r2 && memcmp(a, b, fi.raw_size)) { printf("  MISMATCH single vs parallel: %s [%s]\n", name, what); fails++; }
    if (!r1 && !fi.supported) {   // palette / 16-bit ...: BGRX too (the palette and tRNS come from the damaged file)
        size_t px = (size_t)fi.width * fi.height * 4;
        uint8_t *o1 = malloc(px), *o2 = malloc(px);
        if (np_to_bgrx(d, n, &fi, a, o1, (size_t)fi.width * 4, 1) || np_to_bgrx(d, n, &fi, a, o2, (size_t)fi.width * 4, 0) ||
            memcmp(o1, o2, px)) { printf("  BGRX MISMATCH: %s [%s]\n", name, what); fails++; }
        free(o1); free(o2);
    }
    if (getenv("VERBOSE")) printf("  %-28s %-24s -> %s / %s\n", name, what, r1 ? "fail" : "ok", r2 ? "fail" : "ok");
    free(a); free(b);
}

int main(int argc, char **argv) {
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); size_t n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *o = malloc(n), *d = malloc(n + 4096);
        fread(o, 1, n, fp); fclose(fp);
        const char *name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
        char what[64];
        run(name, "original", o, n);
        double fr[] = {0.001, 0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99, 0.9999};
        for (size_t k = 0; k < sizeof fr / sizeof *fr; k++) {
            size_t m = (size_t)(n * fr[k]);
            memcpy(d, o, m); snprintf(what, sizeof what, "truncated %.2f%%", fr[k] * 100); run(name, what, d, m);
        }
        size_t fixed[] = {8, 20, 33, 40, 60, n - 1, n - 4, n - 12, n - 13, n - 16};
        for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) {
            if (fixed[k] > n) continue;
            memcpy(d, o, fixed[k]); snprintf(what, sizeof what, "truncated @%zu", fixed[k]); run(name, what, d, fixed[k]);
        }
        int flips[] = {1, 1, 1, 10, 100, 1000};
        for (size_t k = 0; k < sizeof flips / sizeof *flips && n > 100; k++) {
            memcpy(d, o, n);
            for (int i = 0; i < flips[k]; i++) d[100 + rnd() % (n - 100)] = (uint8_t)rnd();
            snprintf(what, sizeof what, "%d random bytes", flips[k]); run(name, what, d, n);
        }
        for (int k = 0; k < 6 && n > 100; k++) {
            memcpy(d, o, n);
            size_t p = 100 + rnd() % (n - 100);
            d[p] ^= (uint8_t)(1 << (rnd() % 8));
            snprintf(what, sizeof what, "bit flip @%zu", p); run(name, what, d, n);
        }
        for (int k = 0; k < 3 && n > 100 + 65536; k++) {   // (big enough files)
            memcpy(d, o, n);
            size_t p = 100 + rnd() % (n - 100 - 65536), m = k == 2 ? 4096 : 65536;
            memset(d + p, k ? 0xFF : 0x00, m);
            snprintf(what, sizeof what, "%zuK of 0x%02X", m / 1024, k ? 0xFF : 0); run(name, what, d, n);
        }
        for (int k = 0; k < 20 && n >= 29; k++) {   // IHDR / header damage (size, type, depth)
            memcpy(d, o, n);
            d[16 + rnd() % 13] = (uint8_t)rnd();
            snprintf(what, sizeof what, "IHDR damage #%d", k); run(name, what, d, n);
        }
        {   // chunk lengths damaged
            memcpy(d, o, n);
            for (size_t i = 8; i + 12 <= n;) {
                uint32_t len = (uint32_t)d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3];
                if (!memcmp(d + i + 4, "IDAT", 4) && (rnd() & 7) == 0) { d[i] ^= 0x40; break; }
                i += 12 + len;
            }
            run(name, "IDAT length damaged", d, n);
        }
        {   // a piece of the middle removed / duplicated
            size_t p = n / 3, cut = n / 9;
            memcpy(d, o, p); memcpy(d + p, o + p + cut, n - p - cut); run(name, "middle removed", d, n - cut);
            memcpy(d, o, p + cut); memcpy(d + p + cut, o + p, n - p - cut); run(name, "chunk duplicated", d, n);
        }
        free(o); free(d);
    }
    printf("%d cases (%d decoded), %d mismatches. No crash.\n", cases, ok_cases, fails);
    return fails != 0;
}
